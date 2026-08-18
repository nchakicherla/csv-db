#include "test_util.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "schema.h"
#include "storage.h"
#include "value.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *make_temp_dir(void) {
    char template_buf[] = "/tmp/csvdb_test_dir_XXXXXX";
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

static Schema *make_widgets_schema(void) {
    char err[256];
    Schema *s = schema_parse_json(
        "{\"name\":\"widgets\",\"columns\":["
        "{\"name\":\"id\",\"type\":\"INTEGER\",\"nullable\":false,\"primary_key\":true},"
        "{\"name\":\"label\",\"type\":\"TEXT\",\"nullable\":true}]}",
        err, sizeof(err));
    if (s == NULL) {
        fprintf(stderr, "  fixture schema build failed: %s\n", err);
    }
    return s;
}

static void test_create_and_lookup(void) {
    char err[256];
    char *dir = make_temp_dir();
    TEST_CHECK(dir != NULL);

    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));
    TEST_CHECK(catalog_table_count(&cat) == 0);

    Schema *schema = make_widgets_schema();
    TEST_CHECK(schema != NULL);
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));

    TEST_CHECK(catalog_has_table(&cat, "widgets"));
    TEST_CHECK(catalog_table_count(&cat) == 1);
    TEST_CHECK_STR_EQ(catalog_table_name_at(&cat, 0), "widgets");

    char path[1024];
    snprintf(path, sizeof(path), "%s/widgets.schema.json", dir);
    TEST_CHECK(path_exists(path));
    snprintf(path, sizeof(path), "%s/widgets.csv", dir);
    TEST_CHECK(path_exists(path));

    /* header-only CSV: just the column names, no data rows */
    char *csv_text = read_file_to_string(path);
    TEST_CHECK(csv_text != NULL);
    if (csv_text != NULL) {
        TEST_CHECK(strstr(csv_text, "id") != NULL);
        TEST_CHECK(strstr(csv_text, "label") != NULL);
        free(csv_text);
    }

    schema_free(schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_get_schema_caches(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *schema = make_widgets_schema();
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));

    const Schema *first = catalog_get_schema(&cat, "widgets", err, sizeof(err));
    TEST_CHECK(first != NULL);
    const Schema *second = catalog_get_schema(&cat, "widgets", err, sizeof(err));
    TEST_CHECK(first == second); /* cached, not re-parsed */

    TEST_CHECK(catalog_get_schema(&cat, "nonexistent", err, sizeof(err)) == NULL);

    schema_free(schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_read_write_round_trip(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *schema = make_widgets_schema();
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));

    const Schema *cached = catalog_get_schema(&cat, "widgets", err, sizeof(err));
    TEST_CHECK(cached != NULL);

    Value *row_values = malloc(2 * sizeof(Value));
    row_values[0] = value_make_integer(1);
    row_values[1] = value_make_text("gadget");
    Row row = {.values = row_values};
    RowSet input = {.schema = cached, .rows = &row, .row_count = 1, .row_capacity = 1};

    TEST_CHECK(catalog_write_table(&cat, "widgets", &input, err, sizeof(err)));
    free(row_values); /* RowSet here wraps a stack Row; free its Values manually */

    RowSet loaded;
    bool ok = catalog_read_table(&cat, "widgets", &loaded, err, sizeof(err));
    TEST_CHECK(ok);
    if (ok) {
        TEST_CHECK(loaded.row_count == 1);
        TEST_CHECK(loaded.rows[0].values[0].as.integer == 1);
        TEST_CHECK_STR_EQ(loaded.rows[0].values[1].as.text, "gadget");
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    schema_free(schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_drop_table(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *schema = make_widgets_schema();
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));
    TEST_CHECK(catalog_drop_table(&cat, "widgets", err, sizeof(err)));

    TEST_CHECK(!catalog_has_table(&cat, "widgets"));
    TEST_CHECK(catalog_table_count(&cat) == 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/widgets.schema.json", dir);
    TEST_CHECK(!path_exists(path));
    snprintf(path, sizeof(path), "%s/widgets.csv", dir);
    TEST_CHECK(!path_exists(path));

    TEST_CHECK(!catalog_drop_table(&cat, "widgets", err, sizeof(err)));

    schema_free(schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_duplicate_create_fails(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *schema = make_widgets_schema();
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));
    TEST_CHECK(!catalog_create_table(&cat, schema, err, sizeof(err)));

    schema_free(schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_reopen_discovers_existing_tables(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *schema = make_widgets_schema();
    TEST_CHECK(catalog_create_table(&cat, schema, err, sizeof(err)));
    catalog_close(&cat);

    Catalog reopened;
    TEST_CHECK(catalog_open(dir, &reopened, err, sizeof(err)));
    TEST_CHECK(catalog_table_count(&reopened) == 1);
    TEST_CHECK(catalog_has_table(&reopened, "widgets"));

    const Schema *loaded_schema = catalog_get_schema(&reopened, "widgets", err, sizeof(err));
    TEST_CHECK(loaded_schema != NULL);
    if (loaded_schema != NULL) {
        TEST_CHECK(loaded_schema->column_count == 2);
        TEST_CHECK(schema_find_column(loaded_schema, "label") == 1);
    }

    schema_free(schema);
    catalog_close(&reopened);
    remove_dir_best_effort(dir);
    free(dir);
}

/* A hand-built Schema, bypassing schema_parse_json's own identifier check,
 * so this test exercises catalog.c's independent is_safe_table_name()
 * defense-in-depth check specifically, not schema.c's. */
static Schema *make_raw_schema(const char *name) {
    Schema *s = malloc(sizeof(Schema));
    s->name = dup_str(name);
    s->column_count = 1;
    s->columns = malloc(sizeof(Column));
    s->columns[0].name = dup_str("id");
    s->columns[0].type = VALUE_INTEGER;
    s->columns[0].nullable = false;
    s->columns[0].primary_key = true;
    s->columns[0].has_foreign_key = false;
    s->columns[0].foreign_key.table = NULL;
    s->columns[0].foreign_key.column = NULL;
    return s;
}

static void test_path_traversal_rejected(void) {
    char err[256] = {0};
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *malicious = make_raw_schema("../evil");
    TEST_CHECK(!catalog_create_table(&cat, malicious, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');
    TEST_CHECK(catalog_table_count(&cat) == 0);

    /* "<dir>/../evil.schema.json" would land as a sibling of `dir` if the
     * traversal weren't blocked -- confirm it never got created. */
    char escaped_path[1024];
    snprintf(escaped_path, sizeof(escaped_path), "%s/../evil.schema.json", dir);
    TEST_CHECK(!path_exists(escaped_path));

    schema_free(malicious);

    Schema *slashy = make_raw_schema("a/b");
    err[0] = '\0';
    TEST_CHECK(!catalog_create_table(&cat, slashy, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');
    schema_free(slashy);

    /* drop is exposed to the same class of caller-controlled name */
    err[0] = '\0';
    TEST_CHECK(!catalog_drop_table(&cat, "../evil", err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

static void test_lock_tables_sorted_and_dedup(void) {
    char err[256];
    char *dir = make_temp_dir();
    Catalog cat;
    TEST_CHECK(catalog_open(dir, &cat, err, sizeof(err)));

    Schema *a_schema = schema_parse_json(
        "{\"name\":\"a\",\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\"}]}", err, sizeof(err));
    Schema *b_schema = schema_parse_json(
        "{\"name\":\"b\",\"columns\":[{\"name\":\"id\",\"type\":\"INTEGER\"}]}", err, sizeof(err));
    TEST_CHECK(a_schema != NULL && b_schema != NULL);
    TEST_CHECK(catalog_create_table(&cat, a_schema, err, sizeof(err)));
    TEST_CHECK(catalog_create_table(&cat, b_schema, err, sizeof(err)));

    const char *names[3] = {"b", "a", "a"}; /* unsorted, with a duplicate */
    TableLock locks[3];
    size_t locked_count = 0;
    bool ok = catalog_lock_tables(&cat, names, 3, LOCK_EXCLUSIVE, locks, &locked_count, err, sizeof(err));
    TEST_CHECK(ok);
    TEST_CHECK(locked_count == 2); /* "a" collapsed to a single lock */
    catalog_unlock_tables(locks, locked_count);

    const char *missing[1] = {"nonexistent"};
    size_t missing_locked = 0;
    err[0] = '\0';
    TEST_CHECK(!catalog_lock_tables(&cat, missing, 1, LOCK_SHARED, locks, &missing_locked, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');
    TEST_CHECK(missing_locked == 0);

    schema_free(a_schema);
    schema_free(b_schema);
    catalog_close(&cat);
    remove_dir_best_effort(dir);
    free(dir);
}

int main(void) {
    test_create_and_lookup();
    test_get_schema_caches();
    test_read_write_round_trip();
    test_drop_table();
    test_duplicate_create_fails();
    test_reopen_discovers_existing_tables();
    test_path_traversal_rejected();
    test_lock_tables_sorted_and_dedup();

    if (test_failures == 0) {
        printf("all catalog tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
