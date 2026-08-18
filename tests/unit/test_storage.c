#include "test_util.h"

#include <stdio.h>
#include <unistd.h>

#include "schema.h"
#include "storage.h"
#include "value.h"

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static char *make_temp_csv_path(void) {
    char template_buf[] = "/tmp/csvdb_test_XXXXXX";
    int fd = mkstemp(template_buf);
    if (fd < 0) {
        return NULL;
    }
    close(fd);
    remove(template_buf); /* storage_write creates its own .tmp then renames over this path */
    return dup_str(template_buf);
}

static bool write_string_to_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return written == len;
}

static RowSet build_rowset(const Schema *schema, Row *rows, size_t count) {
    RowSet rs;
    rs.schema = schema;
    rs.row_count = count;
    rs.row_capacity = count;
    rs.rows = NULL;
    if (count > 0) {
        rs.rows = malloc(count * sizeof(Row));
        memcpy(rs.rows, rows, count * sizeof(Row));
    }
    return rs;
}

static bool values_equal(const Value *a, const Value *b) {
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case VALUE_NULL: return true;
    case VALUE_INTEGER: return a->as.integer == b->as.integer;
    case VALUE_REAL: return a->as.real == b->as.real;
    case VALUE_BOOLEAN: return a->as.boolean == b->as.boolean;
    case VALUE_TEXT: return strcmp(a->as.text, b->as.text) == 0;
    }
    return false;
}

static bool rowsets_equal(const RowSet *a, const RowSet *b) {
    if (a->row_count != b->row_count) {
        return false;
    }
    for (size_t i = 0; i < a->row_count; i++) {
        for (size_t j = 0; j < a->schema->column_count; j++) {
            if (!values_equal(&a->rows[i].values[j], &b->rows[i].values[j])) {
                return false;
            }
        }
    }
    return true;
}

static void test_round_trip_basic(void) {
    char err[256];
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    TEST_CHECK(schema_text != NULL);
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    Value *row1 = malloc(4 * sizeof(Value));
    row1[0] = value_make_integer(1);
    row1[1] = value_make_text("Alice");
    row1[2] = value_make_integer(30);
    row1[3] = value_make_null();

    Value *row2 = malloc(4 * sizeof(Value));
    row2[0] = value_make_integer(2);
    row2[1] = value_make_text("Bob");
    row2[2] = value_make_null();
    row2[3] = value_make_integer(5);

    Row local_rows[2] = {{.values = row1}, {.values = row2}};
    RowSet input = build_rowset(schema, local_rows, 2);

    char *path = make_temp_csv_path();
    TEST_CHECK(path != NULL);

    TEST_CHECK(storage_write(&input, path, err, sizeof(err)));

    RowSet loaded;
    bool loaded_ok = storage_load(schema, path, &loaded, err, sizeof(err));
    TEST_CHECK(loaded_ok);
    if (loaded_ok) {
        TEST_CHECK(loaded.row_count == 2);
        TEST_CHECK(rowsets_equal(&input, &loaded));
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    remove(path);
    free(path);
    rowset_free(&input);
    schema_free(schema);
}

static void test_quoting_edge_cases(void) {
    char err[256];
    Schema *schema = schema_parse_json(
        "{\"name\":\"t\",\"columns\":[{\"name\":\"c\",\"type\":\"TEXT\",\"nullable\":true}]}",
        err, sizeof(err));
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    const char *tricky = "a,b\"c\nd";
    Value *row = malloc(sizeof(Value));
    row[0] = value_make_text(tricky);
    Row local_rows[1] = {{.values = row}};
    RowSet input = build_rowset(schema, local_rows, 1);

    char *path = make_temp_csv_path();
    TEST_CHECK(storage_write(&input, path, err, sizeof(err)));

    RowSet loaded;
    bool loaded_ok = storage_load(schema, path, &loaded, err, sizeof(err));
    TEST_CHECK(loaded_ok);
    if (loaded_ok) {
        TEST_CHECK(loaded.row_count == 1);
        TEST_CHECK(loaded.rows[0].values[0].type == VALUE_TEXT);
        TEST_CHECK_STR_EQ(loaded.rows[0].values[0].as.text, tricky);
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    remove(path);
    free(path);
    rowset_free(&input);
    schema_free(schema);
}

static void test_null_round_trip(void) {
    char err[256];
    Schema *schema = schema_parse_json(
        "{\"name\":\"t\",\"columns\":["
        "{\"name\":\"a\",\"type\":\"INTEGER\",\"nullable\":true},"
        "{\"name\":\"b\",\"type\":\"TEXT\",\"nullable\":true},"
        "{\"name\":\"c\",\"type\":\"REAL\",\"nullable\":true},"
        "{\"name\":\"d\",\"type\":\"BOOLEAN\",\"nullable\":true}]}",
        err, sizeof(err));
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    Value *all_null = malloc(4 * sizeof(Value));
    for (int i = 0; i < 4; i++) {
        all_null[i] = value_make_null();
    }
    Value *none_null = malloc(4 * sizeof(Value));
    none_null[0] = value_make_integer(7);
    none_null[1] = value_make_text("x");
    none_null[2] = value_make_real(1.5);
    none_null[3] = value_make_boolean(true);

    Row local_rows[2] = {{.values = all_null}, {.values = none_null}};
    RowSet input = build_rowset(schema, local_rows, 2);

    char *path = make_temp_csv_path();
    TEST_CHECK(storage_write(&input, path, err, sizeof(err)));

    RowSet loaded;
    bool loaded_ok = storage_load(schema, path, &loaded, err, sizeof(err));
    TEST_CHECK(loaded_ok);
    if (loaded_ok) {
        TEST_CHECK(rowsets_equal(&input, &loaded));
        for (size_t j = 0; j < 4; j++) {
            TEST_CHECK(loaded.rows[0].values[j].type == VALUE_NULL);
        }
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    remove(path);
    free(path);
    rowset_free(&input);
    schema_free(schema);
}

static void test_empty_table_with_trailing_newline(void) {
    char err[256];
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, "id,name,age,dept_id\n"));

    RowSet loaded;
    bool ok = storage_load(schema, path, &loaded, err, sizeof(err));
    TEST_CHECK(ok);
    if (ok) {
        TEST_CHECK(loaded.row_count == 0);
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    remove(path);
    free(path);
    schema_free(schema);
}

static void test_empty_table_without_trailing_newline(void) {
    char err[256];
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, "id,name,age,dept_id"));

    RowSet loaded;
    bool ok = storage_load(schema, path, &loaded, err, sizeof(err));
    TEST_CHECK(ok);
    if (ok) {
        TEST_CHECK(loaded.row_count == 0);
        rowset_free(&loaded);
    } else {
        fprintf(stderr, "  error: %s\n", err);
    }

    remove(path);
    free(path);
    schema_free(schema);
}

static void test_empty_file_is_an_error(void) {
    char err[256] = {0};
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, ""));

    RowSet loaded;
    err[0] = '\0';
    TEST_CHECK(!storage_load(schema, path, &loaded, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    remove(path);
    free(path);
    schema_free(schema);
}

static void test_header_mismatch_is_an_error(void) {
    char err[256] = {0};
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, "id,name\n1,x\n"));

    RowSet loaded;
    err[0] = '\0';
    TEST_CHECK(!storage_load(schema, path, &loaded, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    remove(path);
    free(path);
    schema_free(schema);
}

static void test_bad_type_value_is_an_error(void) {
    char err[256] = {0};
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, "id,name,age,dept_id\nnot_a_number,Alice,30,\n"));

    RowSet loaded;
    err[0] = '\0';
    TEST_CHECK(!storage_load(schema, path, &loaded, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    remove(path);
    free(path);
    schema_free(schema);
}

static void test_field_count_mismatch_is_an_error(void) {
    char err[256] = {0};
    char *schema_text = read_file_to_string(fixture_path("schema", "users.schema.json"));
    Schema *schema = schema_parse_json(schema_text, err, sizeof(err));
    free(schema_text);
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        return;
    }

    char *path = make_temp_csv_path();
    TEST_CHECK(write_string_to_file(path, "id,name,age,dept_id\n1,Alice,30\n"));

    RowSet loaded;
    err[0] = '\0';
    TEST_CHECK(!storage_load(schema, path, &loaded, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    remove(path);
    free(path);
    schema_free(schema);
}

int main(void) {
    test_round_trip_basic();
    test_quoting_edge_cases();
    test_null_round_trip();
    test_empty_table_with_trailing_newline();
    test_empty_table_without_trailing_newline();
    test_empty_file_is_an_error();
    test_header_mismatch_is_an_error();
    test_bad_type_value_is_an_error();
    test_field_count_mismatch_is_an_error();

    if (test_failures == 0) {
        printf("all storage tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
