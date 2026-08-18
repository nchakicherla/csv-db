#include "schema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static char *schema_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static bool is_valid_identifier(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return false;
    }
    for (const char *p = s + 1; *p != '\0'; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) {
            return false;
        }
    }
    return true;
}

bool schema_type_from_name(const char *name, ValueType *out) {
    if (strcmp(name, "INTEGER") == 0) { *out = VALUE_INTEGER; return true; }
    if (strcmp(name, "REAL") == 0)    { *out = VALUE_REAL;    return true; }
    if (strcmp(name, "TEXT") == 0)    { *out = VALUE_TEXT;    return true; }
    if (strcmp(name, "BOOLEAN") == 0) { *out = VALUE_BOOLEAN; return true; }
    return false;
}

const char *schema_type_to_name(ValueType type) {
    switch (type) {
    case VALUE_INTEGER: return "INTEGER";
    case VALUE_REAL:    return "REAL";
    case VALUE_TEXT:    return "TEXT";
    case VALUE_BOOLEAN: return "BOOLEAN";
    case VALUE_NULL:    break;
    }
    return "UNKNOWN";
}

bool schema_validate(const Schema *schema, char *errbuf, size_t errlen) {
    if (!is_valid_identifier(schema->name)) {
        snprintf(errbuf, errlen, "\"%s\" is not a valid table name", schema->name);
        return false;
    }
    if (schema->column_count == 0) {
        snprintf(errbuf, errlen, "schema \"%s\" must have at least one column", schema->name);
        return false;
    }

    int primary_key_count = 0;
    for (size_t i = 0; i < schema->column_count; i++) {
        const Column *col = &schema->columns[i];

        if (!is_valid_identifier(col->name)) {
            snprintf(errbuf, errlen, "\"%s\" is not a valid column name", col->name);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(schema->columns[j].name, col->name) == 0) {
                snprintf(errbuf, errlen, "duplicate column name \"%s\"", col->name);
                return false;
            }
        }
        if (col->primary_key && ++primary_key_count > 1) {
            snprintf(errbuf, errlen,
                     "schema \"%s\" has more than one primary_key column", schema->name);
            return false;
        }
        if (col->has_foreign_key &&
            (!is_valid_identifier(col->foreign_key.table) ||
             !is_valid_identifier(col->foreign_key.column))) {
            snprintf(errbuf, errlen, "column \"%s\" has a malformed foreign_key", col->name);
            return false;
        }
    }

    return true;
}

Schema *schema_parse_json(const char *json_text, char *errbuf, size_t errlen) {
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        const char *err_at = cJSON_GetErrorPtr();
        if (err_at != NULL) {
            snprintf(errbuf, errlen, "malformed JSON near: %.40s", err_at);
        } else {
            snprintf(errbuf, errlen, "malformed JSON");
        }
        return NULL;
    }

    Schema *schema = NULL;

    if (!cJSON_IsObject(root)) {
        snprintf(errbuf, errlen, "schema must be a JSON object");
        goto fail;
    }

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_item) || name_item->valuestring[0] == '\0') {
        snprintf(errbuf, errlen, "schema is missing a string \"name\"");
        goto fail;
    }

    cJSON *columns_item = cJSON_GetObjectItemCaseSensitive(root, "columns");
    if (!cJSON_IsArray(columns_item)) {
        snprintf(errbuf, errlen, "schema is missing a \"columns\" array");
        goto fail;
    }

    int n = cJSON_GetArraySize(columns_item);

    schema = calloc(1, sizeof(Schema));
    schema->name = schema_strdup(name_item->valuestring);
    schema->column_count = (size_t)n;
    schema->columns = (n > 0) ? calloc((size_t)n, sizeof(Column)) : NULL;

    for (int i = 0; i < n; i++) {
        cJSON *col_item = cJSON_GetArrayItem(columns_item, i);
        Column *col = &schema->columns[i];

        if (!cJSON_IsObject(col_item)) {
            snprintf(errbuf, errlen, "column %d is not a JSON object", i);
            goto fail;
        }

        cJSON *col_name = cJSON_GetObjectItemCaseSensitive(col_item, "name");
        if (!cJSON_IsString(col_name) || col_name->valuestring[0] == '\0') {
            snprintf(errbuf, errlen, "column %d is missing a string \"name\"", i);
            goto fail;
        }
        col->name = schema_strdup(col_name->valuestring);

        cJSON *col_type = cJSON_GetObjectItemCaseSensitive(col_item, "type");
        if (!cJSON_IsString(col_type) || !schema_type_from_name(col_type->valuestring, &col->type)) {
            snprintf(errbuf, errlen, "column \"%s\" has an unknown type", col->name);
            goto fail;
        }

        cJSON *col_nullable = cJSON_GetObjectItemCaseSensitive(col_item, "nullable");
        if (col_nullable == NULL) {
            col->nullable = true;
        } else if (cJSON_IsBool(col_nullable)) {
            col->nullable = cJSON_IsTrue(col_nullable);
        } else {
            snprintf(errbuf, errlen, "column \"%s\" has a non-boolean \"nullable\"", col->name);
            goto fail;
        }

        cJSON *col_pk = cJSON_GetObjectItemCaseSensitive(col_item, "primary_key");
        if (col_pk == NULL) {
            col->primary_key = false;
        } else if (cJSON_IsBool(col_pk)) {
            col->primary_key = cJSON_IsTrue(col_pk);
        } else {
            snprintf(errbuf, errlen, "column \"%s\" has a non-boolean \"primary_key\"", col->name);
            goto fail;
        }

        cJSON *fk_item = cJSON_GetObjectItemCaseSensitive(col_item, "foreign_key");
        if (fk_item != NULL) {
            cJSON *fk_table = cJSON_IsObject(fk_item)
                                   ? cJSON_GetObjectItemCaseSensitive(fk_item, "table")
                                   : NULL;
            cJSON *fk_column = cJSON_IsObject(fk_item)
                                    ? cJSON_GetObjectItemCaseSensitive(fk_item, "column")
                                    : NULL;
            if (!cJSON_IsString(fk_table) || !cJSON_IsString(fk_column)) {
                snprintf(errbuf, errlen, "column \"%s\" has a malformed foreign_key", col->name);
                goto fail;
            }
            col->has_foreign_key = true;
            col->foreign_key.table = schema_strdup(fk_table->valuestring);
            col->foreign_key.column = schema_strdup(fk_column->valuestring);
        }
    }

    cJSON_Delete(root);

    if (!schema_validate(schema, errbuf, errlen)) {
        schema_free(schema);
        return NULL;
    }

    return schema;

fail:
    cJSON_Delete(root);
    schema_free(schema);
    return NULL;
}

char *schema_to_json_string(const Schema *schema) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", schema->name);
    cJSON *columns = cJSON_AddArrayToObject(root, "columns");

    for (size_t i = 0; i < schema->column_count; i++) {
        const Column *col = &schema->columns[i];
        cJSON *col_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(col_obj, "name", col->name);
        cJSON_AddStringToObject(col_obj, "type", schema_type_to_name(col->type));
        cJSON_AddBoolToObject(col_obj, "nullable", col->nullable);
        cJSON_AddBoolToObject(col_obj, "primary_key", col->primary_key);
        if (col->has_foreign_key) {
            cJSON *fk_obj = cJSON_AddObjectToObject(col_obj, "foreign_key");
            cJSON_AddStringToObject(fk_obj, "table", col->foreign_key.table);
            cJSON_AddStringToObject(fk_obj, "column", col->foreign_key.column);
        }
        cJSON_AddItemToArray(columns, col_obj);
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}

int schema_find_column(const Schema *schema, const char *name) {
    for (size_t i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool schema_append_column(Schema *schema, char *name, ValueType type, bool nullable,
                           bool primary_key, bool has_foreign_key,
                           char *fk_table, char *fk_column) {
    Column *grown = realloc(schema->columns, (schema->column_count + 1) * sizeof(Column));
    if (grown == NULL) {
        return false;
    }
    schema->columns = grown;

    Column *col = &schema->columns[schema->column_count];
    col->name = name;
    col->type = type;
    col->nullable = nullable;
    col->primary_key = primary_key;
    col->has_foreign_key = has_foreign_key;
    col->foreign_key.table = fk_table;
    col->foreign_key.column = fk_column;
    schema->column_count++;
    return true;
}

void schema_free(Schema *schema) {
    if (schema == NULL) {
        return;
    }
    free(schema->name);
    for (size_t i = 0; i < schema->column_count; i++) {
        free(schema->columns[i].name);
        free(schema->columns[i].foreign_key.table);
        free(schema->columns[i].foreign_key.column);
    }
    free(schema->columns);
    free(schema);
}
