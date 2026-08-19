#include "test_util.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include "catalog.h"
#include "executor.h"
#include "parser.h"
#include "result.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static char *make_temp_dir(void) {
    char template_buf[] = "/tmp/csvdb_integration_XXXXXX";
    char *result = mkdtemp(template_buf);
    if (result == NULL) {
        return NULL;
    }
    return dup_str(result);
}

static void remove_dir_best_effort(const char *dir) {
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        return;
    }
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(dp);
    rmdir(dir);
}

static bool exec_sql(Catalog *catalog, const char *sql, Result **out_result, size_t *out_affected,
                      char *errbuf, size_t errlen) {
    Statement *stmt = parser_parse(sql, errbuf, errlen);
    if (stmt == NULL) {
        return false;
    }
    bool ok = executor_exec(catalog, stmt, out_result, out_affected, errbuf, errlen);
    statement_free(stmt);
    return ok;
}

static bool exec_ok(Catalog *catalog, const char *sql) {
    char err[256];
    Result *result = NULL;
    bool ok = exec_sql(catalog, sql, &result, NULL, err, sizeof(err));
    if (!ok) {
        fprintf(stderr, "  unexpected failure for [%s]: %s\n", sql, err);
    }
    result_free(result);
    return ok;
}

static bool exec_fails(Catalog *catalog, const char *sql, const char *error_substring) {
    char err[256] = {0};
    Result *result = NULL;
    bool ok = exec_sql(catalog, sql, &result, NULL, err, sizeof(err));
    result_free(result);
    if (ok) {
        fprintf(stderr, "  [%s] expected failure but succeeded\n", sql);
        return false;
    }
    if (error_substring != NULL && strstr(err, error_substring) == NULL) {
        fprintf(stderr, "  [%s] error \"%s\" doesn't contain \"%s\"\n", sql, err, error_substring);
        return false;
    }
    return true;
}

static bool cell_str_eq(const Result *r, size_t row, size_t col, const char *expected) {
    const Value *v = result_get(r, row, col);
    if (v == NULL) {
        return expected == NULL;
    }
    char *s = value_to_string(v);
    bool eq = strcmp(s, expected) == 0;
    free(s);
    return eq;
}

int main(void) {
    char *dir = make_temp_dir();
    TEST_CHECK(dir != NULL);

    char err[256];
    Catalog catalog;
    TEST_CHECK(catalog_open(dir, &catalog, err, sizeof(err)));

    /* ---- schema setup ---- */
    TEST_CHECK(exec_ok(&catalog, "CREATE TABLE departments (id INTEGER PRIMARY KEY, label TEXT)"));
    TEST_CHECK(exec_ok(&catalog,
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, "
        "dept_id INTEGER REFERENCES departments(id))"));

    /* ---- seed data ---- */
    TEST_CHECK(exec_ok(&catalog, "INSERT INTO departments VALUES (1, 'Engineering'), (2, 'Sales')"));
    TEST_CHECK(exec_ok(&catalog,
        "INSERT INTO users VALUES (1, 'Alice', 30, 1), (2, 'Bob', 25, 2), (3, 'Carol', NULL, NULL)"));

    /* ---- validation failures leave the table unchanged ---- */
    TEST_CHECK(exec_fails(&catalog, "INSERT INTO users VALUES (4, NULL, 40, 1)", "NOT NULL"));
    TEST_CHECK(exec_fails(&catalog, "INSERT INTO users VALUES (1, 'Dup', 20, 1)", "primary key"));
    TEST_CHECK(exec_fails(&catalog, "INSERT INTO users VALUES (5, 'Eve', 22, 99)", "does not exist"));

    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT * FROM users", &r, NULL, e, sizeof(e)));
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 3); /* failed inserts above didn't leak any rows */
            result_free(r);
        }
    }

    /* ---- SELECT: projection, WHERE, ORDER BY ---- */
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT * FROM users ORDER BY id", &r, NULL, e, sizeof(e)));
        TEST_CHECK(r != NULL);
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 3);
            TEST_CHECK(result_col_count(r) == 4);
            TEST_CHECK(cell_str_eq(r, 0, 1, "Alice"));
            TEST_CHECK(cell_str_eq(r, 1, 1, "Bob"));
            TEST_CHECK(cell_str_eq(r, 2, 1, "Carol"));
            TEST_CHECK(cell_str_eq(r, 2, 2, "")); /* Carol's NULL age */
            result_free(r);
        }
    }

    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT name, age FROM users WHERE age > 26", &r, NULL, e, sizeof(e)));
        TEST_CHECK(r != NULL);
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 1);
            TEST_CHECK(result_col_count(r) == 2);
            TEST_CHECK(cell_str_eq(r, 0, 0, "Alice"));
            result_free(r);
        }
    }

    /* ---- JOINs ---- */
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog,
            "SELECT u.name, d.label FROM users AS u INNER JOIN departments AS d "
            "ON u.dept_id = d.id ORDER BY u.id",
            &r, NULL, e, sizeof(e)));
        TEST_CHECK(r != NULL);
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 2); /* Carol has no dept_id, excluded by INNER JOIN */
            TEST_CHECK(cell_str_eq(r, 0, 0, "Alice"));
            TEST_CHECK(cell_str_eq(r, 0, 1, "Engineering"));
            TEST_CHECK(cell_str_eq(r, 1, 0, "Bob"));
            TEST_CHECK(cell_str_eq(r, 1, 1, "Sales"));
            result_free(r);
        }
    }

    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog,
            "SELECT u.name, d.label FROM users AS u LEFT JOIN departments AS d "
            "ON u.dept_id = d.id ORDER BY u.id",
            &r, NULL, e, sizeof(e)));
        TEST_CHECK(r != NULL);
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 3); /* Carol kept, with a NULL department */
            TEST_CHECK(cell_str_eq(r, 2, 0, "Carol"));
            TEST_CHECK(cell_str_eq(r, 2, 1, ""));
            result_free(r);
        }
    }

    /* ---- ORDER BY DESC + LIMIT (NULLs sort last in DESC) ---- */
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT name FROM users ORDER BY age DESC LIMIT 2",
                             &r, NULL, e, sizeof(e)));
        TEST_CHECK(r != NULL);
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 2);
            TEST_CHECK(cell_str_eq(r, 0, 0, "Alice"));
            TEST_CHECK(cell_str_eq(r, 1, 0, "Bob"));
            result_free(r);
        }
    }

    /* ---- UPDATE ---- */
    TEST_CHECK(exec_ok(&catalog, "UPDATE users SET age = 31 WHERE name = 'Alice'"));
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT age FROM users WHERE name = 'Alice'", &r, NULL, e, sizeof(e)));
        if (r != NULL) {
            TEST_CHECK(cell_str_eq(r, 0, 0, "31"));
            result_free(r);
        }
    }
    TEST_CHECK(exec_fails(&catalog, "UPDATE users SET dept_id = 99 WHERE name = 'Bob'", "does not exist"));

    /* ---- DELETE ---- */
    TEST_CHECK(exec_ok(&catalog, "DELETE FROM users WHERE name = 'Carol'"));
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT * FROM users", &r, NULL, e, sizeof(e)));
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 2);
            result_free(r);
        }
        r = NULL;
        TEST_CHECK(exec_sql(&catalog, "SELECT * FROM departments", &r, NULL, e, sizeof(e)));
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 2); /* untouched by the DELETE on users */
            result_free(r);
        }
    }

    /* ---- DROP TABLE ---- */
    TEST_CHECK(exec_ok(&catalog, "DROP TABLE users"));
    TEST_CHECK(!catalog_has_table(&catalog, "users"));
    TEST_CHECK(exec_fails(&catalog, "SELECT * FROM users", "no such table"));

    catalog_close(&catalog);

    /* ---- persistence across a reopen ---- */
    Catalog reopened;
    TEST_CHECK(catalog_open(dir, &reopened, err, sizeof(err)));
    TEST_CHECK(!catalog_has_table(&reopened, "users"));
    {
        char e[256];
        Result *r = NULL;
        TEST_CHECK(exec_sql(&reopened, "SELECT * FROM departments ORDER BY id", &r, NULL, e, sizeof(e)));
        if (r != NULL) {
            TEST_CHECK(result_row_count(r) == 2);
            TEST_CHECK(cell_str_eq(r, 0, 1, "Engineering"));
            result_free(r);
        }
    }
    catalog_close(&reopened);

    remove_dir_best_effort(dir);
    free(dir);

    if (test_failures == 0) {
        printf("all executor integration tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
