#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

typedef struct {
    Lexer lexer;
    Token current;
    bool has_error;
    char errbuf[256];
} Parser;

/* ---- AST teardown (declared in ast.h; this is the only .c that builds
 * AST nodes, so it owns freeing them too -- see PLAN.md's module layout,
 * which lists no separate ast.c) ---- */

void expr_free(Expr *e) {
    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case EXPR_LITERAL:
        value_free(&e->as.literal);
        break;
    case EXPR_COLUMN_REF:
        free(e->as.column_ref.table);
        free(e->as.column_ref.column);
        break;
    case EXPR_BINARY:
        expr_free(e->as.binary.left);
        expr_free(e->as.binary.right);
        break;
    case EXPR_NOT:
        expr_free(e->as.not_expr.operand);
        break;
    }
    free(e);
}

static void table_ref_free_fields(TableRef *ref) {
    free(ref->table_name);
    free(ref->alias);
}

static void select_stmt_free(SelectStmt *s) {
    if (s->list_kind == SELECT_COLUMNS) {
        for (size_t i = 0; i < s->column_count; i++) {
            free(s->columns[i].table);
            free(s->columns[i].column);
        }
    }
    free(s->columns);

    table_ref_free_fields(&s->from);
    for (size_t i = 0; i < s->join_count; i++) {
        table_ref_free_fields(&s->joins[i].table);
        expr_free(s->joins[i].on);
    }
    free(s->joins);

    expr_free(s->where);

    if (s->has_order_by) {
        free(s->order_by_table);
        free(s->order_by_column);
    }
}

void statement_free(Statement *stmt) {
    if (stmt == NULL) {
        return;
    }
    switch (stmt->kind) {
    case STMT_CREATE_TABLE:
        schema_free(stmt->as.create_table.schema);
        break;
    case STMT_DROP_TABLE:
        free(stmt->as.drop_table.table_name);
        break;
    case STMT_INSERT:
        free(stmt->as.insert.table_name);
        for (size_t i = 0; i < stmt->as.insert.column_count; i++) {
            free(stmt->as.insert.columns[i]);
        }
        free(stmt->as.insert.columns);
        for (size_t i = 0; i < stmt->as.insert.row_count; i++) {
            for (size_t j = 0; j < stmt->as.insert.values_per_row; j++) {
                value_free(&stmt->as.insert.rows[i][j]);
            }
            free(stmt->as.insert.rows[i]);
        }
        free(stmt->as.insert.rows);
        break;
    case STMT_SELECT:
        select_stmt_free(&stmt->as.select);
        break;
    case STMT_UPDATE:
        free(stmt->as.update.table_name);
        for (size_t i = 0; i < stmt->as.update.assignment_count; i++) {
            free(stmt->as.update.assignments[i].column);
            value_free(&stmt->as.update.assignments[i].value);
        }
        free(stmt->as.update.assignments);
        expr_free(stmt->as.update.where);
        break;
    case STMT_DELETE:
        free(stmt->as.delete_stmt.table_name);
        expr_free(stmt->as.delete_stmt.where);
        break;
    }
    free(stmt);
}

/* ---- parser primitives ---- */

static void parser_errorf(Parser *p, const char *fmt, ...) {
    if (p->has_error) {
        return; /* keep the first error, don't overwrite with a cascade */
    }
    char msg[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    snprintf(p->errbuf, sizeof(p->errbuf), "%s at line %d, column %d", msg,
             p->current.line, p->current.col);
    p->has_error = true;
}

static bool advance(Parser *p) {
    if (p->has_error) {
        return false;
    }
    token_free(&p->current);
    if (!lexer_next(&p->lexer, &p->current, p->errbuf, sizeof(p->errbuf))) {
        p->has_error = true;
        return false;
    }
    return true;
}

static bool check(Parser *p, TokenType type) {
    return !p->has_error && p->current.type == type;
}

static bool expect(Parser *p, TokenType type, const char *what) {
    if (p->has_error) {
        return false;
    }
    if (p->current.type != type) {
        parser_errorf(p, "expected %s but got %s", what, token_type_name(p->current.type));
        return false;
    }
    return advance(p);
}

/* Consumes a TOK_IDENTIFIER and returns its text (ownership transferred
 * to the caller). Returns NULL (with a parser error set) for anything
 * else -- including any of our reserved keywords, which by construction
 * of the lexer can never be TOK_IDENTIFIER. */
static char *parse_identifier(Parser *p, const char *what) {
    if (p->has_error) {
        return NULL;
    }
    if (p->current.type != TOK_IDENTIFIER) {
        parser_errorf(p, "expected %s but got %s", what, token_type_name(p->current.type));
        return NULL;
    }
    char *name = p->current.text;
    p->current.text = NULL; /* steal it before advance()'s token_free runs */
    if (!advance(p)) {
        free(name);
        return NULL;
    }
    return name;
}

static bool parse_literal_value(Parser *p, Value *out) {
    if (p->has_error) {
        return false;
    }
    if (check(p, TOK_INT_LITERAL)) {
        *out = value_make_integer(p->current.int_value);
    } else if (check(p, TOK_REAL_LITERAL)) {
        *out = value_make_real(p->current.real_value);
    } else if (check(p, TOK_STRING_LITERAL)) {
        *out = value_make_text(p->current.text);
    } else if (check(p, TOK_TRUE)) {
        *out = value_make_boolean(true);
    } else if (check(p, TOK_FALSE)) {
        *out = value_make_boolean(false);
    } else if (check(p, TOK_NULL)) {
        *out = value_make_null();
    } else {
        parser_errorf(p, "expected a literal value but got %s", token_type_name(p->current.type));
        return false;
    }
    return advance(p);
}

/* Parses "( ident (, ident)* )", already positioned at '('. */
static bool parse_identifier_list(Parser *p, char ***out_names, size_t *out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!expect(p, TOK_LPAREN, "'('")) {
        return false;
    }

    char **names = NULL;
    size_t count = 0;
    for (;;) {
        char *name = parse_identifier(p, "an identifier");
        if (name == NULL) {
            for (size_t i = 0; i < count; i++) {
                free(names[i]);
            }
            free(names);
            return false;
        }
        char **grown = realloc(names, (count + 1) * sizeof(char *));
        names = grown;
        names[count++] = name;

        if (check(p, TOK_COMMA)) {
            if (!advance(p)) {
                for (size_t i = 0; i < count; i++) {
                    free(names[i]);
                }
                free(names);
                return false;
            }
            continue;
        }
        break;
    }

    if (!expect(p, TOK_RPAREN, "')'")) {
        for (size_t i = 0; i < count; i++) {
            free(names[i]);
        }
        free(names);
        return false;
    }

    *out_names = names;
    *out_count = count;
    return true;
}

/* Parses "( literal (, literal)* )", already positioned at '('. */
static bool parse_value_tuple(Parser *p, Value **out_values, size_t *out_count) {
    *out_values = NULL;
    *out_count = 0;
    if (!expect(p, TOK_LPAREN, "'('")) {
        return false;
    }

    Value *values = NULL;
    size_t count = 0;
    for (;;) {
        Value *grown = realloc(values, (count + 1) * sizeof(Value));
        values = grown;
        if (!parse_literal_value(p, &values[count])) {
            for (size_t i = 0; i < count; i++) {
                value_free(&values[i]);
            }
            free(values);
            return false;
        }
        count++;

        if (check(p, TOK_COMMA)) {
            if (!advance(p)) {
                for (size_t i = 0; i < count; i++) {
                    value_free(&values[i]);
                }
                free(values);
                return false;
            }
            continue;
        }
        break;
    }

    if (!expect(p, TOK_RPAREN, "')'")) {
        for (size_t i = 0; i < count; i++) {
            value_free(&values[i]);
        }
        free(values);
        return false;
    }

    *out_values = values;
    *out_count = count;
    return true;
}

static bool parse_table_ref(Parser *p, TableRef *out) {
    out->table_name = NULL;
    out->alias = NULL;

    char *name = parse_identifier(p, "table name");
    if (name == NULL) {
        return false;
    }
    out->table_name = name;

    if (check(p, TOK_AS)) {
        if (!advance(p)) {
            free(out->table_name);
            out->table_name = NULL;
            return false;
        }
        char *alias = parse_identifier(p, "alias");
        if (alias == NULL) {
            free(out->table_name);
            out->table_name = NULL;
            return false;
        }
        out->alias = alias;
    }
    return true;
}

/* Parses `col` or `table.col`. */
static bool parse_qualified_column(Parser *p, char **out_table, char **out_column) {
    *out_table = NULL;
    *out_column = NULL;

    char *first = parse_identifier(p, "column name");
    if (first == NULL) {
        return false;
    }
    if (check(p, TOK_DOT)) {
        if (!advance(p)) {
            free(first);
            return false;
        }
        char *col = parse_identifier(p, "column name after '.'");
        if (col == NULL) {
            free(first);
            return false;
        }
        *out_table = first;
        *out_column = col;
    } else {
        *out_column = first;
    }
    return true;
}

/* ---- expressions: OR > AND > NOT > comparison > primary ---- */

static Expr *parse_or_expr(Parser *p);

static Expr *make_binary(BinOpKind op, Expr *left, Expr *right) {
    Expr *e = malloc(sizeof(Expr));
    e->kind = EXPR_BINARY;
    e->as.binary.op = op;
    e->as.binary.left = left;
    e->as.binary.right = right;
    return e;
}

static Expr *make_literal(Value v) {
    Expr *e = malloc(sizeof(Expr));
    e->kind = EXPR_LITERAL;
    e->as.literal = v;
    return e;
}

static Expr *parse_primary(Parser *p) {
    if (p->has_error) {
        return NULL;
    }

    if (check(p, TOK_LPAREN)) {
        if (!advance(p)) {
            return NULL;
        }
        Expr *inner = parse_or_expr(p);
        if (inner == NULL) {
            return NULL;
        }
        if (!expect(p, TOK_RPAREN, "')'")) {
            expr_free(inner);
            return NULL;
        }
        return inner;
    }

    if (check(p, TOK_INT_LITERAL)) {
        Expr *e = make_literal(value_make_integer(p->current.int_value));
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_REAL_LITERAL)) {
        Expr *e = make_literal(value_make_real(p->current.real_value));
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_STRING_LITERAL)) {
        Expr *e = make_literal(value_make_text(p->current.text));
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_TRUE)) {
        Expr *e = make_literal(value_make_boolean(true));
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_FALSE)) {
        Expr *e = make_literal(value_make_boolean(false));
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_NULL)) {
        Expr *e = make_literal(value_make_null());
        if (!advance(p)) {
            expr_free(e);
            return NULL;
        }
        return e;
    }
    if (check(p, TOK_IDENTIFIER)) {
        char *table = NULL;
        char *column = NULL;
        if (!parse_qualified_column(p, &table, &column)) {
            return NULL;
        }
        Expr *e = malloc(sizeof(Expr));
        e->kind = EXPR_COLUMN_REF;
        e->as.column_ref.table = table;
        e->as.column_ref.column = column;
        return e;
    }

    parser_errorf(p, "expected an expression but got %s", token_type_name(p->current.type));
    return NULL;
}

/* Comparisons don't chain (no `a < b < c`): at most one operator. */
static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_primary(p);
    if (left == NULL) {
        return NULL;
    }

    BinOpKind op;
    if (check(p, TOK_EQ)) op = BINOP_EQ;
    else if (check(p, TOK_NE)) op = BINOP_NE;
    else if (check(p, TOK_LT)) op = BINOP_LT;
    else if (check(p, TOK_LE)) op = BINOP_LE;
    else if (check(p, TOK_GT)) op = BINOP_GT;
    else if (check(p, TOK_GE)) op = BINOP_GE;
    else if (check(p, TOK_LIKE)) op = BINOP_LIKE;
    else return left;

    if (!advance(p)) {
        expr_free(left);
        return NULL;
    }
    Expr *right = parse_primary(p);
    if (right == NULL) {
        expr_free(left);
        return NULL;
    }
    return make_binary(op, left, right);
}

static Expr *parse_not_expr(Parser *p) {
    if (check(p, TOK_NOT)) {
        if (!advance(p)) {
            return NULL;
        }
        Expr *operand = parse_not_expr(p);
        if (operand == NULL) {
            return NULL;
        }
        Expr *e = malloc(sizeof(Expr));
        e->kind = EXPR_NOT;
        e->as.not_expr.operand = operand;
        return e;
    }
    return parse_comparison(p);
}

static Expr *parse_and_expr(Parser *p) {
    Expr *left = parse_not_expr(p);
    if (left == NULL) {
        return NULL;
    }
    while (check(p, TOK_AND)) {
        if (!advance(p)) {
            expr_free(left);
            return NULL;
        }
        Expr *right = parse_not_expr(p);
        if (right == NULL) {
            expr_free(left);
            return NULL;
        }
        left = make_binary(BINOP_AND, left, right);
    }
    return left;
}

static Expr *parse_or_expr(Parser *p) {
    Expr *left = parse_and_expr(p);
    if (left == NULL) {
        return NULL;
    }
    while (check(p, TOK_OR)) {
        if (!advance(p)) {
            expr_free(left);
            return NULL;
        }
        Expr *right = parse_and_expr(p);
        if (right == NULL) {
            expr_free(left);
            return NULL;
        }
        left = make_binary(BINOP_OR, left, right);
    }
    return left;
}

static Expr *parse_expr(Parser *p) {
    return parse_or_expr(p);
}

/* ---- statements ---- */

static Statement *parse_create_table(Parser *p) {
    if (!advance(p)) { /* consume CREATE */
        return NULL;
    }
    if (!expect(p, TOK_TABLE, "TABLE")) {
        return NULL;
    }

    char *table_name = parse_identifier(p, "table name");
    if (table_name == NULL) {
        return NULL;
    }

    Schema *schema = calloc(1, sizeof(Schema));
    schema->name = table_name;

    if (!expect(p, TOK_LPAREN, "'('")) {
        schema_free(schema);
        return NULL;
    }

    for (;;) {
        char *col_name = parse_identifier(p, "column name");
        if (col_name == NULL) {
            schema_free(schema);
            return NULL;
        }

        ValueType col_type;
        if (check(p, TOK_INTEGER)) col_type = VALUE_INTEGER;
        else if (check(p, TOK_REAL)) col_type = VALUE_REAL;
        else if (check(p, TOK_TEXT)) col_type = VALUE_TEXT;
        else if (check(p, TOK_BOOLEAN)) col_type = VALUE_BOOLEAN;
        else {
            parser_errorf(p, "expected a column type (INTEGER, REAL, TEXT, or BOOLEAN) but got %s",
                          token_type_name(p->current.type));
            free(col_name);
            schema_free(schema);
            return NULL;
        }
        if (!advance(p)) {
            free(col_name);
            schema_free(schema);
            return NULL;
        }

        bool not_null = false;
        bool primary_key = false;
        bool has_fk = false;
        char *fk_table = NULL;
        char *fk_column = NULL;

        for (;;) {
            if (check(p, TOK_NOT)) {
                if (!advance(p) || !expect(p, TOK_NULL, "NULL after NOT")) {
                    free(col_name);
                    free(fk_table);
                    free(fk_column);
                    schema_free(schema);
                    return NULL;
                }
                not_null = true;
            } else if (check(p, TOK_PRIMARY)) {
                if (!advance(p) || !expect(p, TOK_KEY, "KEY after PRIMARY")) {
                    free(col_name);
                    free(fk_table);
                    free(fk_column);
                    schema_free(schema);
                    return NULL;
                }
                primary_key = true;
            } else if (check(p, TOK_REFERENCES)) {
                if (!advance(p)) {
                    free(col_name);
                    schema_free(schema);
                    return NULL;
                }
                fk_table = parse_identifier(p, "referenced table name");
                if (fk_table == NULL) {
                    free(col_name);
                    schema_free(schema);
                    return NULL;
                }
                if (!expect(p, TOK_LPAREN, "'('")) {
                    free(col_name);
                    free(fk_table);
                    schema_free(schema);
                    return NULL;
                }
                fk_column = parse_identifier(p, "referenced column name");
                if (fk_column == NULL) {
                    free(col_name);
                    free(fk_table);
                    schema_free(schema);
                    return NULL;
                }
                if (!expect(p, TOK_RPAREN, "')'")) {
                    free(col_name);
                    free(fk_table);
                    free(fk_column);
                    schema_free(schema);
                    return NULL;
                }
                has_fk = true;
            } else {
                break;
            }
        }

        if (!schema_append_column(schema, col_name, col_type, !not_null, primary_key,
                                   has_fk, fk_table, fk_column)) {
            free(col_name);
            free(fk_table);
            free(fk_column);
            schema_free(schema);
            parser_errorf(p, "out of memory");
            return NULL;
        }

        if (check(p, TOK_COMMA)) {
            if (!advance(p)) {
                schema_free(schema);
                return NULL;
            }
            continue;
        }
        break;
    }

    if (!expect(p, TOK_RPAREN, "')'")) {
        schema_free(schema);
        return NULL;
    }

    char validate_err[200];
    if (!schema_validate(schema, validate_err, sizeof(validate_err))) {
        parser_errorf(p, "%s", validate_err);
        schema_free(schema);
        return NULL;
    }

    Statement *stmt = malloc(sizeof(Statement));
    stmt->kind = STMT_CREATE_TABLE;
    stmt->as.create_table.schema = schema;
    return stmt;
}

static Statement *parse_drop_table(Parser *p) {
    if (!advance(p)) { /* consume DROP */
        return NULL;
    }
    if (!expect(p, TOK_TABLE, "TABLE")) {
        return NULL;
    }
    char *name = parse_identifier(p, "table name");
    if (name == NULL) {
        return NULL;
    }

    Statement *stmt = malloc(sizeof(Statement));
    stmt->kind = STMT_DROP_TABLE;
    stmt->as.drop_table.table_name = name;
    return stmt;
}

static Statement *parse_insert(Parser *p) {
    if (!advance(p)) { /* consume INSERT */
        return NULL;
    }
    if (!expect(p, TOK_INTO, "INTO")) {
        return NULL;
    }

    char *table_name = parse_identifier(p, "table name");
    if (table_name == NULL) {
        return NULL;
    }

    char **columns = NULL;
    size_t column_count = 0;
    if (check(p, TOK_LPAREN)) {
        if (!parse_identifier_list(p, &columns, &column_count)) {
            free(table_name);
            return NULL;
        }
    }

    if (!expect(p, TOK_VALUES, "VALUES")) {
        free(table_name);
        for (size_t i = 0; i < column_count; i++) {
            free(columns[i]);
        }
        free(columns);
        return NULL;
    }

    Value **rows = NULL;
    size_t row_count = 0;
    size_t values_per_row = 0;

    for (;;) {
        Value *row_values;
        size_t row_len;
        if (!parse_value_tuple(p, &row_values, &row_len)) {
            goto fail;
        }

        if (row_count == 0) {
            values_per_row = row_len;
        } else if (row_len != values_per_row) {
            parser_errorf(p, "row %zu has %zu value(s), expected %zu",
                          row_count + 1, row_len, values_per_row);
            for (size_t i = 0; i < row_len; i++) {
                value_free(&row_values[i]);
            }
            free(row_values);
            goto fail;
        }

        Value **grown_rows = realloc(rows, (row_count + 1) * sizeof(Value *));
        rows = grown_rows;
        rows[row_count++] = row_values;

        if (check(p, TOK_COMMA)) {
            if (!advance(p)) {
                goto fail;
            }
            continue;
        }
        break;
    }

    {
        Statement *stmt = malloc(sizeof(Statement));
        stmt->kind = STMT_INSERT;
        stmt->as.insert.table_name = table_name;
        stmt->as.insert.columns = columns;
        stmt->as.insert.column_count = column_count;
        stmt->as.insert.rows = rows;
        stmt->as.insert.row_count = row_count;
        stmt->as.insert.values_per_row = values_per_row;
        return stmt;
    }

fail:
    free(table_name);
    for (size_t i = 0; i < column_count; i++) {
        free(columns[i]);
    }
    free(columns);
    for (size_t i = 0; i < row_count; i++) {
        for (size_t j = 0; j < values_per_row; j++) {
            value_free(&rows[i][j]);
        }
        free(rows[i]);
    }
    free(rows);
    return NULL;
}

static Statement *parse_select(Parser *p) {
    if (!advance(p)) { /* consume SELECT */
        return NULL;
    }

    SelectStmt sel;
    memset(&sel, 0, sizeof(sel));

    if (check(p, TOK_STAR)) {
        sel.list_kind = SELECT_ALL;
        if (!advance(p)) {
            goto fail_select;
        }
    } else {
        sel.list_kind = SELECT_COLUMNS;
        for (;;) {
            char *tbl, *col;
            if (!parse_qualified_column(p, &tbl, &col)) {
                goto fail_select;
            }
            SelectColumn *grown = realloc(sel.columns, (sel.column_count + 1) * sizeof(SelectColumn));
            sel.columns = grown;
            sel.columns[sel.column_count].table = tbl;
            sel.columns[sel.column_count].column = col;
            sel.column_count++;

            if (check(p, TOK_COMMA)) {
                if (!advance(p)) {
                    goto fail_select;
                }
                continue;
            }
            break;
        }
    }

    if (!expect(p, TOK_FROM, "FROM")) {
        goto fail_select;
    }
    if (!parse_table_ref(p, &sel.from)) {
        goto fail_select;
    }

    while (check(p, TOK_INNER) || check(p, TOK_LEFT)) {
        JoinType jt = check(p, TOK_INNER) ? JOIN_INNER : JOIN_LEFT;
        if (!advance(p)) {
            goto fail_select;
        }
        if (!expect(p, TOK_JOIN, "JOIN")) {
            goto fail_select;
        }

        TableRef ref;
        if (!parse_table_ref(p, &ref)) {
            goto fail_select;
        }
        if (!expect(p, TOK_ON, "ON")) {
            table_ref_free_fields(&ref);
            goto fail_select;
        }
        Expr *on = parse_expr(p);
        if (on == NULL) {
            table_ref_free_fields(&ref);
            goto fail_select;
        }

        JoinClause *grown = realloc(sel.joins, (sel.join_count + 1) * sizeof(JoinClause));
        sel.joins = grown;
        sel.joins[sel.join_count].type = jt;
        sel.joins[sel.join_count].table = ref;
        sel.joins[sel.join_count].on = on;
        sel.join_count++;
    }

    if (check(p, TOK_WHERE)) {
        if (!advance(p)) {
            goto fail_select;
        }
        sel.where = parse_expr(p);
        if (sel.where == NULL) {
            goto fail_select;
        }
    }

    if (check(p, TOK_ORDER)) {
        if (!advance(p)) {
            goto fail_select;
        }
        if (!expect(p, TOK_BY, "BY")) {
            goto fail_select;
        }
        char *tbl, *col;
        if (!parse_qualified_column(p, &tbl, &col)) {
            goto fail_select;
        }
        sel.has_order_by = true;
        sel.order_by_table = tbl;
        sel.order_by_column = col;
        sel.order_by_direction = ORDER_ASC;
        if (check(p, TOK_ASC)) {
            if (!advance(p)) {
                goto fail_select;
            }
        } else if (check(p, TOK_DESC)) {
            sel.order_by_direction = ORDER_DESC;
            if (!advance(p)) {
                goto fail_select;
            }
        }
    }

    if (check(p, TOK_LIMIT)) {
        if (!advance(p)) {
            goto fail_select;
        }
        if (!check(p, TOK_INT_LITERAL)) {
            parser_errorf(p, "expected an integer after LIMIT but got %s",
                          token_type_name(p->current.type));
            goto fail_select;
        }
        if (p->current.int_value < 0) {
            parser_errorf(p, "LIMIT must not be negative");
            goto fail_select;
        }
        sel.has_limit = true;
        sel.limit = p->current.int_value;
        if (!advance(p)) {
            goto fail_select;
        }
    }

    {
        Statement *stmt = malloc(sizeof(Statement));
        stmt->kind = STMT_SELECT;
        stmt->as.select = sel;
        return stmt;
    }

fail_select:
    select_stmt_free(&sel);
    return NULL;
}

static Statement *parse_update(Parser *p) {
    if (!advance(p)) { /* consume UPDATE */
        return NULL;
    }

    char *table_name = parse_identifier(p, "table name");
    if (table_name == NULL) {
        return NULL;
    }
    if (!expect(p, TOK_SET, "SET")) {
        free(table_name);
        return NULL;
    }

    Assignment *assignments = NULL;
    size_t count = 0;
    Expr *where = NULL;

    for (;;) {
        char *col = parse_identifier(p, "column name");
        if (col == NULL) {
            goto fail_update;
        }
        if (!expect(p, TOK_EQ, "'='")) {
            free(col);
            goto fail_update;
        }
        Value val;
        if (!parse_literal_value(p, &val)) {
            free(col);
            goto fail_update;
        }

        Assignment *grown = realloc(assignments, (count + 1) * sizeof(Assignment));
        assignments = grown;
        assignments[count].column = col;
        assignments[count].value = val;
        count++;

        if (check(p, TOK_COMMA)) {
            if (!advance(p)) {
                goto fail_update;
            }
            continue;
        }
        break;
    }

    if (check(p, TOK_WHERE)) {
        if (!advance(p)) {
            goto fail_update;
        }
        where = parse_expr(p);
        if (where == NULL) {
            goto fail_update;
        }
    }

    {
        Statement *stmt = malloc(sizeof(Statement));
        stmt->kind = STMT_UPDATE;
        stmt->as.update.table_name = table_name;
        stmt->as.update.assignments = assignments;
        stmt->as.update.assignment_count = count;
        stmt->as.update.where = where;
        return stmt;
    }

fail_update:
    free(table_name);
    for (size_t i = 0; i < count; i++) {
        free(assignments[i].column);
        value_free(&assignments[i].value);
    }
    free(assignments);
    expr_free(where);
    return NULL;
}

static Statement *parse_delete(Parser *p) {
    if (!advance(p)) { /* consume DELETE */
        return NULL;
    }
    if (!expect(p, TOK_FROM, "FROM")) {
        return NULL;
    }

    char *table_name = parse_identifier(p, "table name");
    if (table_name == NULL) {
        return NULL;
    }

    Expr *where = NULL;
    if (check(p, TOK_WHERE)) {
        if (!advance(p)) {
            free(table_name);
            return NULL;
        }
        where = parse_expr(p);
        if (where == NULL) {
            free(table_name);
            return NULL;
        }
    }

    Statement *stmt = malloc(sizeof(Statement));
    stmt->kind = STMT_DELETE;
    stmt->as.delete_stmt.table_name = table_name;
    stmt->as.delete_stmt.where = where;
    return stmt;
}

Statement *parser_parse(const char *sql, char *errbuf, size_t errlen) {
    Parser p;
    lexer_init(&p.lexer, sql);
    p.has_error = false;
    p.errbuf[0] = '\0';
    p.current.text = NULL;

    if (!advance(&p)) {
        snprintf(errbuf, errlen, "%s", p.errbuf);
        return NULL;
    }

    Statement *stmt = NULL;
    switch (p.current.type) {
    case TOK_CREATE: stmt = parse_create_table(&p); break;
    case TOK_DROP: stmt = parse_drop_table(&p); break;
    case TOK_INSERT: stmt = parse_insert(&p); break;
    case TOK_SELECT: stmt = parse_select(&p); break;
    case TOK_UPDATE: stmt = parse_update(&p); break;
    case TOK_DELETE: stmt = parse_delete(&p); break;
    default:
        parser_errorf(&p, "expected a statement (CREATE, DROP, INSERT, SELECT, UPDATE, or DELETE) but got %s",
                      token_type_name(p.current.type));
        break;
    }

    if (!p.has_error && check(&p, TOK_SEMICOLON)) {
        advance(&p);
    }
    if (!p.has_error && !check(&p, TOK_EOF)) {
        parser_errorf(&p, "unexpected %s after statement", token_type_name(p.current.type));
    }

    token_free(&p.current);

    if (p.has_error) {
        if (stmt != NULL) {
            statement_free(stmt);
        }
        snprintf(errbuf, errlen, "%s", p.errbuf);
        return NULL;
    }

    return stmt;
}
