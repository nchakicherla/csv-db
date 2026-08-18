#include "test_util.h"

#include <stdio.h>

#include "parser.h"

typedef struct {
    const char *name;
    const char *sql;
    bool should_succeed;
    const char *error_substring; /* only checked when should_succeed is false and non-NULL */
} ParseCase;

static void run_cases(const ParseCase *cases, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char err[256] = {0};
        Statement *stmt = parser_parse(cases[i].sql, err, sizeof(err));

        if (cases[i].should_succeed) {
            TEST_CHECK(stmt != NULL);
            if (stmt == NULL) {
                fprintf(stderr, "  [%s] unexpected parse error: %s\n  sql: %s\n",
                        cases[i].name, err, cases[i].sql);
            } else {
                statement_free(stmt);
            }
        } else {
            TEST_CHECK(stmt == NULL);
            if (stmt != NULL) {
                fprintf(stderr, "  [%s] expected a parse error but succeeded\n  sql: %s\n",
                        cases[i].name, cases[i].sql);
                statement_free(stmt);
            } else if (cases[i].error_substring != NULL) {
                bool contains = strstr(err, cases[i].error_substring) != NULL;
                TEST_CHECK(contains);
                if (!contains) {
                    fprintf(stderr, "  [%s] error \"%s\" doesn't contain \"%s\"\n",
                            cases[i].name, err, cases[i].error_substring);
                }
            }
        }
    }
}

static void test_create_table_cases(void) {
    ParseCase cases[] = {
        {"basic", "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                   "age INTEGER, dept_id INTEGER REFERENCES departments(id))",
         true, NULL},
        {"lowercase-keywords", "create table t (id integer)", true, NULL},
        {"missing-table-keyword", "CREATE users (id INTEGER)", false, "TABLE"},
        {"path-like-name", "CREATE TABLE ../evil (id INTEGER)", false, "table name"},
        {"digit-leading-name", "CREATE TABLE 3bad (id INTEGER)", false, "table name"},
        {"duplicate-columns", "CREATE TABLE t (id INTEGER, id TEXT)", false, "duplicate"},
        {"multiple-primary-keys",
         "CREATE TABLE t (a INTEGER PRIMARY KEY, b INTEGER PRIMARY KEY)", false, "primary_key"},
        {"unknown-type", "CREATE TABLE t (a VARCHAR)", false, "column type"},
        {"missing-close-paren", "CREATE TABLE t (a INTEGER", false, NULL},
        {"empty-columns", "CREATE TABLE t ()", false, NULL},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_drop_table_cases(void) {
    ParseCase cases[] = {
        {"basic", "DROP TABLE users", true, NULL},
        {"missing-table-keyword", "DROP users", false, "TABLE"},
        {"missing-name", "DROP TABLE", false, NULL},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_insert_cases(void) {
    ParseCase cases[] = {
        {"basic", "INSERT INTO users VALUES (1, 'Alice', 30, NULL)", true, NULL},
        {"explicit-columns",
         "INSERT INTO users (id, name) VALUES (1, 'Alice'), (2, 'Bob')", true, NULL},
        {"mismatched-row-lengths",
         "INSERT INTO users VALUES (1, 'a'), (2)", false, "value"},
        {"missing-values-keyword", "INSERT INTO users (1, 'a')", false, NULL},
        {"qualified-table-name", "INSERT INTO a.b VALUES (1)", false, "VALUES"},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_select_cases(void) {
    ParseCase cases[] = {
        {"star", "SELECT * FROM users", true, NULL},
        {"columns-and-where", "SELECT id, name FROM users WHERE age > 18", true, NULL},
        {"full-featured",
         "SELECT u.id, d.label FROM users AS u INNER JOIN departments AS d "
         "ON u.dept_id = d.id WHERE u.age >= 21 AND NOT u.name LIKE 'A%' "
         "ORDER BY u.id DESC LIMIT 10",
         true, NULL},
        {"left-join", "SELECT * FROM t LEFT JOIN u ON t.id = u.id", true, NULL},
        {"parenthesized-expr", "SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3", true, NULL},
        {"missing-from", "SELECT * users", false, "FROM"},
        {"unknown-join-type", "SELECT * FROM t OUTER JOIN u ON t.id = u.id", false, NULL},
        {"dangling-where", "SELECT * FROM t WHERE", false, NULL},
        {"negative-limit", "SELECT * FROM t LIMIT -1", false, "negative"},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_update_cases(void) {
    ParseCase cases[] = {
        {"with-where", "UPDATE users SET age = 31 WHERE id = 1", true, NULL},
        {"no-where-multi-assign", "UPDATE users SET age = NULL, name = 'X'", true, NULL},
        {"missing-set", "UPDATE users age = 1", false, "SET"},
        {"missing-eq", "UPDATE users SET age 31", false, NULL},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_delete_cases(void) {
    ParseCase cases[] = {
        {"with-where", "DELETE FROM users WHERE id = 1", true, NULL},
        {"no-where", "DELETE FROM users", true, NULL},
        {"missing-from", "DELETE users", false, "FROM"},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_trailing_garbage_rejected(void) {
    ParseCase cases[] = {
        {"garbage-after-semicolon", "DELETE FROM users;;", false, NULL},
        {"garbage-after-statement", "DELETE FROM users extra", false, NULL},
        {"optional-semicolon", "DELETE FROM users;", true, NULL},
    };
    run_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

/* ---- deep structural checks ---- */

static void test_create_table_schema_shape(void) {
    char err[256];
    Statement *stmt = parser_parse(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "age INTEGER, dept_id INTEGER REFERENCES departments(id))",
        err, sizeof(err));
    TEST_CHECK(stmt != NULL);
    if (stmt == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    TEST_CHECK(stmt->kind == STMT_CREATE_TABLE);
    Schema *schema = stmt->as.create_table.schema;
    TEST_CHECK_STR_EQ(schema->name, "users");
    TEST_CHECK(schema->column_count == 4);

    TEST_CHECK(schema->columns[0].type == VALUE_INTEGER);
    TEST_CHECK(schema->columns[0].primary_key == true);
    TEST_CHECK(schema->columns[0].nullable == true); /* no explicit NOT NULL on this column */

    TEST_CHECK(schema->columns[1].type == VALUE_TEXT);
    TEST_CHECK(schema->columns[1].nullable == false);

    TEST_CHECK(schema->columns[3].has_foreign_key == true);
    TEST_CHECK_STR_EQ(schema->columns[3].foreign_key.table, "departments");
    TEST_CHECK_STR_EQ(schema->columns[3].foreign_key.column, "id");

    statement_free(stmt);
}

static void test_insert_row_shape(void) {
    char err[256];
    Statement *stmt = parser_parse(
        "INSERT INTO users (id, name) VALUES (1, 'Alice'), (2, 'Bob')", err, sizeof(err));
    TEST_CHECK(stmt != NULL);
    if (stmt == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    TEST_CHECK(stmt->kind == STMT_INSERT);
    InsertStmt *ins = &stmt->as.insert;
    TEST_CHECK_STR_EQ(ins->table_name, "users");
    TEST_CHECK(ins->column_count == 2);
    TEST_CHECK_STR_EQ(ins->columns[0], "id");
    TEST_CHECK_STR_EQ(ins->columns[1], "name");
    TEST_CHECK(ins->row_count == 2);
    TEST_CHECK(ins->values_per_row == 2);
    TEST_CHECK(ins->rows[0][0].type == VALUE_INTEGER && ins->rows[0][0].as.integer == 1);
    TEST_CHECK_STR_EQ(ins->rows[0][1].as.text, "Alice");
    TEST_CHECK(ins->rows[1][0].as.integer == 2);
    TEST_CHECK_STR_EQ(ins->rows[1][1].as.text, "Bob");

    statement_free(stmt);
}

static void test_select_full_shape(void) {
    char err[256];
    Statement *stmt = parser_parse(
        "SELECT u.id, d.label FROM users AS u INNER JOIN departments AS d "
        "ON u.dept_id = d.id WHERE u.age >= 21 ORDER BY u.id DESC LIMIT 10",
        err, sizeof(err));
    TEST_CHECK(stmt != NULL);
    if (stmt == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    TEST_CHECK(stmt->kind == STMT_SELECT);
    SelectStmt *sel = &stmt->as.select;

    TEST_CHECK(sel->list_kind == SELECT_COLUMNS);
    TEST_CHECK(sel->column_count == 2);
    TEST_CHECK_STR_EQ(sel->columns[0].table, "u");
    TEST_CHECK_STR_EQ(sel->columns[0].column, "id");

    TEST_CHECK_STR_EQ(sel->from.table_name, "users");
    TEST_CHECK_STR_EQ(sel->from.alias, "u");

    TEST_CHECK(sel->join_count == 1);
    TEST_CHECK(sel->joins[0].type == JOIN_INNER);
    TEST_CHECK_STR_EQ(sel->joins[0].table.table_name, "departments");
    TEST_CHECK_STR_EQ(sel->joins[0].table.alias, "d");
    TEST_CHECK(sel->joins[0].on->kind == EXPR_BINARY);
    TEST_CHECK(sel->joins[0].on->as.binary.op == BINOP_EQ);

    TEST_CHECK(sel->where != NULL);
    TEST_CHECK(sel->where->kind == EXPR_BINARY);
    TEST_CHECK(sel->where->as.binary.op == BINOP_GE);

    TEST_CHECK(sel->has_order_by);
    TEST_CHECK_STR_EQ(sel->order_by_table, "u");
    TEST_CHECK_STR_EQ(sel->order_by_column, "id");
    TEST_CHECK(sel->order_by_direction == ORDER_DESC);

    TEST_CHECK(sel->has_limit);
    TEST_CHECK(sel->limit == 10);

    statement_free(stmt);
}

static void test_operator_precedence_and_over_or(void) {
    char err[256];
    Statement *stmt = parser_parse(
        "SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3", err, sizeof(err));
    TEST_CHECK(stmt != NULL);
    if (stmt == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    Expr *where = stmt->as.select.where;
    /* AND binds tighter than OR: a=1 OR (b=2 AND c=3) */
    TEST_CHECK(where->kind == EXPR_BINARY);
    TEST_CHECK(where->as.binary.op == BINOP_OR);
    TEST_CHECK(where->as.binary.left->kind == EXPR_BINARY);
    TEST_CHECK(where->as.binary.left->as.binary.op == BINOP_EQ);
    Expr *right = where->as.binary.right;
    TEST_CHECK(right->kind == EXPR_BINARY);
    TEST_CHECK(right->as.binary.op == BINOP_AND);

    statement_free(stmt);
}

static void test_not_binds_tighter_than_and(void) {
    char err[256];
    Statement *stmt = parser_parse(
        "SELECT * FROM t WHERE NOT a = 1 AND b = 2", err, sizeof(err));
    TEST_CHECK(stmt != NULL);
    if (stmt == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    Expr *where = stmt->as.select.where;
    /* (NOT a=1) AND b=2 */
    TEST_CHECK(where->kind == EXPR_BINARY);
    TEST_CHECK(where->as.binary.op == BINOP_AND);
    Expr *left = where->as.binary.left;
    TEST_CHECK(left->kind == EXPR_NOT);
    TEST_CHECK(left->as.not_expr.operand->kind == EXPR_BINARY);
    TEST_CHECK(left->as.not_expr.operand->as.binary.op == BINOP_EQ);

    statement_free(stmt);
}

int main(void) {
    test_create_table_cases();
    test_drop_table_cases();
    test_insert_cases();
    test_select_cases();
    test_update_cases();
    test_delete_cases();
    test_trailing_garbage_rejected();

    test_create_table_schema_shape();
    test_insert_row_shape();
    test_select_full_shape();
    test_operator_precedence_and_over_or();
    test_not_binds_tighter_than_and();

    if (test_failures == 0) {
        printf("all parser tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
