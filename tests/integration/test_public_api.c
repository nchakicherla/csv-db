#include "test_util.h"

#include <dirent.h>
#include <stdio.h>
#include <unistd.h>

#include "csvdb/csvdb.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static char *make_temp_dir(void) {
    char template_buf[] = "/tmp/csvdb_public_api_XXXXXX";
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

static void test_open_nonexistent_dir_fails(void) {
    char err[256] = {0};
    csvdb *db = csvdb_open("/tmp/csvdb_definitely_does_not_exist_dir", err, sizeof(err));
    TEST_CHECK(db == NULL);
    TEST_CHECK(err[0] != '\0');
}

static void test_crud_through_public_api(void) {
    char *dir = make_temp_dir();
    TEST_CHECK(dir != NULL);

    char err[256];
    csvdb *db = csvdb_open(dir, err, sizeof(err));
    TEST_CHECK(db != NULL);
    if (db == NULL) {
        fprintf(stderr, "  open error: %s\n", err);
        free(dir);
        return;
    }

    TEST_CHECK(csvdb_exec(db, "CREATE TABLE widgets (id INTEGER PRIMARY KEY, label TEXT NOT NULL)",
                           NULL, NULL) == CSVDB_OK);
    TEST_CHECK(csvdb_exec(db, "INSERT INTO widgets VALUES (1, 'a'), (2, 'b')", NULL, NULL) == CSVDB_OK);

    size_t affected = 0;
    TEST_CHECK(csvdb_exec(db, "UPDATE widgets SET label = 'z' WHERE id = 1", NULL, &affected) == CSVDB_OK);
    TEST_CHECK(affected == 1);

    csvdb_result *result = NULL;
    TEST_CHECK(csvdb_exec(db, "SELECT id, label FROM widgets ORDER BY id", &result, NULL) == CSVDB_OK);
    TEST_CHECK(result != NULL);
    if (result != NULL) {
        TEST_CHECK(csvdb_result_row_count(result) == 2);
        TEST_CHECK(csvdb_result_col_count(result) == 2);
        TEST_CHECK_STR_EQ(csvdb_result_col_name(result, 0), "id");
        TEST_CHECK_STR_EQ(csvdb_result_col_name(result, 1), "label");
        TEST_CHECK(csvdb_result_col_type(result, 0) == CSVDB_INTEGER);
        TEST_CHECK(csvdb_result_col_type(result, 1) == CSVDB_TEXT);
        TEST_CHECK(csvdb_result_col_name(result, 99) == NULL); /* out of range */

        csvdb_value v0 = csvdb_result_get(result, 0, 0);
        TEST_CHECK(v0.type == CSVDB_INTEGER && v0.as.integer == 1);
        csvdb_value v1 = csvdb_result_get(result, 0, 1);
        TEST_CHECK(v1.type == CSVDB_TEXT && strcmp(v1.as.text, "z") == 0);

        csvdb_value oob = csvdb_result_get(result, 99, 0);
        TEST_CHECK(oob.type == CSVDB_NULL); /* out-of-range row is reported as NULL, not a crash */

        csvdb_result_free(result);
    }

    /* a real error surfaces a non-empty message */
    err[0] = '\0';
    TEST_CHECK(csvdb_exec(db, "SELECT * FROM nonexistent", NULL, NULL) == CSVDB_ERROR);
    TEST_CHECK(strlen(csvdb_errmsg(db)) > 0);

    /* misuse is reported distinctly from a real execution error */
    TEST_CHECK(csvdb_exec(NULL, "SELECT 1", NULL, NULL) == CSVDB_MISUSE);
    TEST_CHECK(csvdb_exec(db, NULL, NULL, NULL) == CSVDB_MISUSE);

    TEST_CHECK(csvdb_exec(db, "DELETE FROM widgets WHERE id = 2", NULL, &affected) == CSVDB_OK);
    TEST_CHECK(affected == 1);

    TEST_CHECK(csvdb_exec(db, "DROP TABLE widgets", NULL, NULL) == CSVDB_OK);

    TEST_CHECK(strlen(csvdb_version()) > 0);

    csvdb_close(db);
    csvdb_close(NULL); /* must be a safe no-op */

    remove_dir_best_effort(dir);
    free(dir);
}

int main(void) {
    test_open_nonexistent_dir_fails();
    test_crud_through_public_api();

    if (test_failures == 0) {
        printf("all public API tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
