#include "test_util.h"

#include <stdio.h>

#include "schema.h"

static const char *fixture_path(const char *name) {
    static char path[1024];
    snprintf(path, sizeof(path), "%s/schema/%s", FIXTURES_DIR, name);
    return path;
}

static Schema *parse_fixture(const char *name, char *errbuf, size_t errlen) {
    char *text = read_file_to_string(fixture_path(name));
    if (text == NULL) {
        snprintf(errbuf, errlen, "could not read fixture %s", name);
        return NULL;
    }
    Schema *schema = schema_parse_json(text, errbuf, errlen);
    free(text);
    return schema;
}

static bool schemas_equal(const Schema *a, const Schema *b) {
    if (strcmp(a->name, b->name) != 0) {
        return false;
    }
    if (a->column_count != b->column_count) {
        return false;
    }
    for (size_t i = 0; i < a->column_count; i++) {
        const Column *ca = &a->columns[i];
        const Column *cb = &b->columns[i];
        if (strcmp(ca->name, cb->name) != 0) return false;
        if (ca->type != cb->type) return false;
        if (ca->nullable != cb->nullable) return false;
        if (ca->primary_key != cb->primary_key) return false;
        if (ca->has_foreign_key != cb->has_foreign_key) return false;
        if (ca->has_foreign_key) {
            if (strcmp(ca->foreign_key.table, cb->foreign_key.table) != 0) return false;
            if (strcmp(ca->foreign_key.column, cb->foreign_key.column) != 0) return false;
        }
    }
    return true;
}

static void test_parse_valid_users(void) {
    char err[256] = {0};
    Schema *schema = parse_fixture("users.schema.json", err, sizeof(err));
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }

    TEST_CHECK_STR_EQ(schema->name, "users");
    TEST_CHECK(schema->column_count == 4);

    int id_idx = schema_find_column(schema, "id");
    TEST_CHECK(id_idx == 0);
    TEST_CHECK(schema->columns[id_idx].type == VALUE_INTEGER);
    TEST_CHECK(schema->columns[id_idx].nullable == false);
    TEST_CHECK(schema->columns[id_idx].primary_key == true);
    TEST_CHECK(schema->columns[id_idx].has_foreign_key == false);

    int dept_idx = schema_find_column(schema, "dept_id");
    TEST_CHECK(dept_idx == 3);
    TEST_CHECK(schema->columns[dept_idx].has_foreign_key == true);
    TEST_CHECK_STR_EQ(schema->columns[dept_idx].foreign_key.table, "departments");
    TEST_CHECK_STR_EQ(schema->columns[dept_idx].foreign_key.column, "id");

    TEST_CHECK(schema_find_column(schema, "nonexistent") == -1);

    schema_free(schema);
}

static void test_parse_defaults(void) {
    char err[256] = {0};
    Schema *schema = parse_fixture("minimal_defaults.schema.json", err, sizeof(err));
    TEST_CHECK(schema != NULL);
    if (schema == NULL) {
        fprintf(stderr, "  error: %s\n", err);
        return;
    }
    TEST_CHECK(schema->column_count == 1);
    TEST_CHECK(schema->columns[0].nullable == true);
    TEST_CHECK(schema->columns[0].primary_key == false);
    TEST_CHECK(schema->columns[0].has_foreign_key == false);
    schema_free(schema);
}

static void test_round_trip(void) {
    char err[256] = {0};
    Schema *original = parse_fixture("users.schema.json", err, sizeof(err));
    TEST_CHECK(original != NULL);
    if (original == NULL) {
        return;
    }

    char *json = schema_to_json_string(original);
    TEST_CHECK(json != NULL);

    Schema *reparsed = schema_parse_json(json, err, sizeof(err));
    TEST_CHECK(reparsed != NULL);
    if (reparsed != NULL) {
        TEST_CHECK(schemas_equal(original, reparsed));
        schema_free(reparsed);
    } else {
        fprintf(stderr, "  round-trip reparse error: %s\n", err);
    }

    free(json);
    schema_free(original);
}

static void test_invalid_fixture(const char *name) {
    char err[256] = {0};
    Schema *schema = parse_fixture(name, err, sizeof(err));
    TEST_CHECK(schema == NULL);
    TEST_CHECK(err[0] != '\0');
    schema_free(schema);
}

static void test_invalid_fixtures(void) {
    test_invalid_fixture("invalid_duplicate_column.schema.json");
    test_invalid_fixture("invalid_unknown_type.schema.json");
    test_invalid_fixture("invalid_multiple_primary_keys.schema.json");
    test_invalid_fixture("invalid_bad_identifier.schema.json");
    test_invalid_fixture("invalid_malformed_json.schema.json");
    test_invalid_fixture("invalid_missing_name.schema.json");
    test_invalid_fixture("invalid_bad_foreign_key.schema.json");
}

static void test_type_name_round_trip(void) {
    ValueType types[] = {VALUE_INTEGER, VALUE_REAL, VALUE_TEXT, VALUE_BOOLEAN};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        const char *name = schema_type_to_name(types[i]);
        ValueType parsed;
        TEST_CHECK(schema_type_from_name(name, &parsed));
        TEST_CHECK(parsed == types[i]);
    }

    ValueType out;
    TEST_CHECK(!schema_type_from_name("VARCHAR", &out));
}

int main(void) {
    test_parse_valid_users();
    test_parse_defaults();
    test_round_trip();
    test_invalid_fixtures();
    test_type_name_round_trip();

    if (test_failures == 0) {
        printf("all schema tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
