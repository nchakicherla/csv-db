#include "test_util.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "catalog.h"
#include "csvdb/csvdb.h"
#include "executor.h"
#include "parser.h"
#include "result.h"
#include "schema.h"
#include "storage.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static char *make_temp_dir(void) {
    char template_buf[] = "/tmp/csvdb_hardening_XXXXXX";
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

static bool exec_sql(Catalog *catalog, const char *sql, Result **out_result, char *errbuf, size_t errlen) {
    Statement *stmt = parser_parse(sql, errbuf, errlen);
    if (stmt == NULL) {
        return false;
    }
    bool ok = executor_exec(catalog, stmt, out_result, NULL, errbuf, errlen);
    statement_free(stmt);
    return ok;
}

/* ---- malformed CSV recovery, through the full SQL -> executor -> catalog
 * -> storage stack (not just storage.c directly, which Phase 2 already
 * unit-tests) ---- */

static void test_malformed_csv_recovery_through_full_stack(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog catalog;
    TEST_CHECK(catalog_open(dir, &catalog, err, sizeof(err)));

    TEST_CHECK(exec_sql(&catalog, "CREATE TABLE t (a TEXT, b INTEGER)", NULL, err, sizeof(err)));
    catalog_close(&catalog);

    /* Hand-corrupt the CSV as if it were edited outside csvdb or partially
     * written by an unrelated process: header now claims 3 columns, but
     * the schema still says 2. */
    char csv_path[1024];
    snprintf(csv_path, sizeof(csv_path), "%s/t.csv", dir);
    FILE *f = fopen(csv_path, "wb");
    TEST_CHECK(f != NULL);
    if (f != NULL) {
        fputs("\"a\",\"b\",\"c\"\n\"x\",\"1\",\"y\"\n", f);
        fclose(f);
    }

    TEST_CHECK(catalog_open(dir, &catalog, err, sizeof(err)));
    err[0] = '\0';
    Result *result = NULL;
    bool ok = exec_sql(&catalog, "SELECT * FROM t", &result, err, sizeof(err));
    TEST_CHECK(!ok); /* a clean, reported error -- not a crash or hang */
    TEST_CHECK(err[0] != '\0');
    result_free(result);

    catalog_close(&catalog);
    remove_dir_best_effort(dir);
    free(dir);
}

/* ---- INTEGER -> REAL promotion on INSERT ---- */

static void test_integer_to_real_promotion(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog catalog;
    TEST_CHECK(catalog_open(dir, &catalog, err, sizeof(err)));

    TEST_CHECK(exec_sql(&catalog, "CREATE TABLE t (price REAL)", NULL, err, sizeof(err)));
    TEST_CHECK(exec_sql(&catalog, "INSERT INTO t VALUES (5)", NULL, err, sizeof(err)));

    Result *result = NULL;
    TEST_CHECK(exec_sql(&catalog, "SELECT price FROM t", &result, err, sizeof(err)));
    if (result != NULL) {
        TEST_CHECK(result_col_type(result, 0) == VALUE_REAL);
        const Value *v = result_get(result, 0, 0);
        TEST_CHECK(v != NULL && v->type == VALUE_REAL && v->as.real == 5.0);
        result_free(result);
    }

    catalog_close(&catalog);
    remove_dir_best_effort(dir);
    free(dir);
}

/* ---- executor-level column-count validation on INSERT (distinct from
 * the parser's own same-statement row-length check tested in Phase 4)
 * ---- */

static void test_insert_column_count_mismatch_rejected(void) {
    char err[256] = {0};
    char *dir = make_temp_dir();
    Catalog catalog;
    TEST_CHECK(catalog_open(dir, &catalog, err, sizeof(err)));

    TEST_CHECK(exec_sql(&catalog, "CREATE TABLE t (a INTEGER, b INTEGER, c INTEGER)", NULL, err, sizeof(err)));

    err[0] = '\0';
    bool ok = exec_sql(&catalog, "INSERT INTO t VALUES (1, 2)", NULL, err, sizeof(err));
    TEST_CHECK(!ok);
    TEST_CHECK(err[0] != '\0');

    Result *result = NULL;
    TEST_CHECK(exec_sql(&catalog, "SELECT * FROM t", &result, err, sizeof(err)));
    if (result != NULL) {
        TEST_CHECK(result_row_count(result) == 0); /* the rejected insert left no partial row */
        result_free(result);
    }

    catalog_close(&catalog);
    remove_dir_best_effort(dir);
    free(dir);
}

/* ---- concurrent multi-process INSERT stress test: proves the
 * flock()-based locking model actually serializes writers under real
 * contention, with no lost or duplicated rows ---- */

#define WORKER_COUNT 4
#define INSERTS_PER_WORKER 15

static void run_worker(const char *dir, int worker_id) {
    char err[256];
    csvdb *db = csvdb_open(dir, err, sizeof(err));
    if (db == NULL) {
        _exit(1);
    }
    for (int i = 0; i < INSERTS_PER_WORKER; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO counter VALUES (%d, %d, %d)",
                 worker_id * 1000 + i, worker_id, i);
        if (csvdb_exec(db, sql, NULL, NULL) != CSVDB_OK) {
            csvdb_close(db);
            _exit(1);
        }
    }
    csvdb_close(db);
    _exit(0);
}

static void test_concurrent_multiprocess_insert(void) {
    char err[256];
    char *dir = make_temp_dir();
    TEST_CHECK(dir != NULL);

    csvdb *setup_db = csvdb_open(dir, err, sizeof(err));
    TEST_CHECK(setup_db != NULL);
    TEST_CHECK(csvdb_exec(setup_db,
        "CREATE TABLE counter (id INTEGER PRIMARY KEY, worker INTEGER, seq INTEGER)",
        NULL, NULL) == CSVDB_OK);
    csvdb_close(setup_db);

    pid_t pids[WORKER_COUNT];
    for (int w = 0; w < WORKER_COUNT; w++) {
        pid_t pid = fork();
        TEST_CHECK(pid >= 0);
        if (pid == 0) {
            run_worker(dir, w);
            /* run_worker always _exit()s; never returns */
        }
        pids[w] = pid;
    }

    bool all_children_ok = true;
    for (int w = 0; w < WORKER_COUNT; w++) {
        int status = 0;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            all_children_ok = false;
        }
    }
    TEST_CHECK(all_children_ok);

    csvdb *verify_db = csvdb_open(dir, err, sizeof(err));
    TEST_CHECK(verify_db != NULL);
    csvdb_result *result = NULL;
    TEST_CHECK(csvdb_exec(verify_db, "SELECT worker, seq FROM counter", &result, NULL) == CSVDB_OK);
    TEST_CHECK(result != NULL);
    if (result != NULL) {
        size_t expected_rows = WORKER_COUNT * INSERTS_PER_WORKER;
        TEST_CHECK(csvdb_result_row_count(result) == expected_rows);

        bool seen[WORKER_COUNT][INSERTS_PER_WORKER];
        memset(seen, 0, sizeof(seen));
        bool any_duplicate = false;
        for (size_t r = 0; r < csvdb_result_row_count(result); r++) {
            csvdb_value w = csvdb_result_get(result, r, 0);
            csvdb_value s = csvdb_result_get(result, r, 1);
            TEST_CHECK(w.type == CSVDB_INTEGER && s.type == CSVDB_INTEGER);
            if (w.type == CSVDB_INTEGER && s.type == CSVDB_INTEGER &&
                w.as.integer >= 0 && w.as.integer < WORKER_COUNT &&
                s.as.integer >= 0 && s.as.integer < INSERTS_PER_WORKER) {
                if (seen[w.as.integer][s.as.integer]) {
                    any_duplicate = true;
                }
                seen[w.as.integer][s.as.integer] = true;
            }
        }
        TEST_CHECK(!any_duplicate);

        bool all_present = true;
        for (int w = 0; w < WORKER_COUNT; w++) {
            for (int i = 0; i < INSERTS_PER_WORKER; i++) {
                if (!seen[w][i]) {
                    all_present = false;
                }
            }
        }
        TEST_CHECK(all_present);

        csvdb_result_free(result);
    }
    csvdb_close(verify_db);

    remove_dir_best_effort(dir);
    free(dir);
}

int main(void) {
    test_malformed_csv_recovery_through_full_stack();
    test_integer_to_real_promotion();
    test_insert_column_count_mismatch_rejected();
    test_concurrent_multiprocess_insert();

    if (test_failures == 0) {
        printf("all hardening tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
