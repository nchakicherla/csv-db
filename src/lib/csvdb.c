#include "csvdb/csvdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "catalog.h"
#include "executor.h"
#include "parser.h"
#include "result.h"
#include "schema.h"
#include "value.h"

struct csvdb {
    Catalog catalog;
    char errmsg[256];
};

struct csvdb_result {
    Result *result;
};

csvdb *csvdb_open(const char *dir, char *errbuf, size_t errlen) {
    if (dir == NULL) {
        snprintf(errbuf, errlen, "database directory must not be NULL");
        return NULL;
    }

    csvdb *db = malloc(sizeof(csvdb));
    if (db == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }

    if (!catalog_open(dir, &db->catalog, errbuf, errlen)) {
        free(db);
        return NULL;
    }

    db->errmsg[0] = '\0';
    return db;
}

void csvdb_close(csvdb *db) {
    if (db == NULL) {
        return;
    }
    catalog_close(&db->catalog);
    free(db);
}

csvdb_code csvdb_exec(csvdb *db, const char *sql, csvdb_result **out_result,
                       size_t *out_affected_rows) {
    if (out_result != NULL) {
        *out_result = NULL;
    }
    if (out_affected_rows != NULL) {
        *out_affected_rows = 0;
    }
    if (db == NULL || sql == NULL) {
        return CSVDB_MISUSE;
    }

    Statement *stmt = parser_parse(sql, db->errmsg, sizeof(db->errmsg));
    if (stmt == NULL) {
        return CSVDB_ERROR;
    }

    Result *result = NULL;
    bool ok = executor_exec(&db->catalog, stmt, &result, out_affected_rows, db->errmsg, sizeof(db->errmsg));
    statement_free(stmt);

    if (!ok) {
        result_free(result);
        return CSVDB_ERROR;
    }

    if (result != NULL) {
        if (out_result == NULL) {
            result_free(result);
        } else {
            csvdb_result *wrapper = malloc(sizeof(csvdb_result));
            if (wrapper == NULL) {
                result_free(result);
                snprintf(db->errmsg, sizeof(db->errmsg), "out of memory");
                return CSVDB_ERROR;
            }
            wrapper->result = result;
            *out_result = wrapper;
        }
    }

    return CSVDB_OK;
}

const char *csvdb_errmsg(csvdb *db) {
    if (db == NULL) {
        return "";
    }
    return db->errmsg;
}

void csvdb_result_free(csvdb_result *result) {
    if (result == NULL) {
        return;
    }
    result_free(result->result);
    free(result);
}

size_t csvdb_result_row_count(const csvdb_result *result) {
    return result_row_count(result->result);
}

size_t csvdb_result_col_count(const csvdb_result *result) {
    return result_col_count(result->result);
}

const char *csvdb_result_col_name(const csvdb_result *result, size_t col) {
    return result_col_name(result->result, col);
}

static csvdb_type value_type_to_csvdb_type(ValueType t) {
    switch (t) {
    case VALUE_NULL: return CSVDB_NULL;
    case VALUE_INTEGER: return CSVDB_INTEGER;
    case VALUE_REAL: return CSVDB_REAL;
    case VALUE_TEXT: return CSVDB_TEXT;
    case VALUE_BOOLEAN: return CSVDB_BOOLEAN;
    }
    return CSVDB_NULL;
}

csvdb_type csvdb_result_col_type(const csvdb_result *result, size_t col) {
    return value_type_to_csvdb_type(result_col_type(result->result, col));
}

csvdb_value csvdb_result_get(const csvdb_result *result, size_t row, size_t col) {
    csvdb_value out;
    const Value *v = result_get(result->result, row, col);
    if (v == NULL) {
        out.type = CSVDB_NULL;
        return out;
    }
    out.type = value_type_to_csvdb_type(v->type);
    switch (v->type) {
    case VALUE_NULL: break;
    case VALUE_INTEGER: out.as.integer = v->as.integer; break;
    case VALUE_REAL: out.as.real = v->as.real; break;
    case VALUE_TEXT: out.as.text = v->as.text; break;
    case VALUE_BOOLEAN: out.as.boolean = v->as.boolean; break;
    }
    return out;
}

size_t csvdb_table_count(csvdb *db) {
    return catalog_table_count(&db->catalog);
}

const char *csvdb_table_name_at(csvdb *db, size_t index) {
    return catalog_table_name_at(&db->catalog, index);
}

static void append_str(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 256 : *cap * 2;
        while (new_cap < *len + n + 1) {
            new_cap *= 2;
        }
        *buf = realloc(*buf, new_cap);
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, n + 1); /* copies the NUL too */
    *len += n;
}

char *csvdb_table_schema_string(csvdb *db, const char *table_name) {
    if (db == NULL || table_name == NULL) {
        return NULL;
    }
    const Schema *schema = catalog_get_schema(&db->catalog, table_name, db->errmsg, sizeof(db->errmsg));
    if (schema == NULL) {
        return NULL;
    }

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    char line[256];

    snprintf(line, sizeof(line), "%s (\n", schema->name);
    append_str(&buf, &len, &cap, line);

    for (size_t i = 0; i < schema->column_count; i++) {
        const Column *col = &schema->columns[i];
        int n = snprintf(line, sizeof(line), "  %s %s", col->name, schema_type_to_name(col->type));
        if (!col->nullable) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " NOT NULL");
        }
        if (col->primary_key) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " PRIMARY KEY");
        }
        if (col->has_foreign_key) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " REFERENCES %s(%s)",
                           col->foreign_key.table, col->foreign_key.column);
        }
        snprintf(line + n, sizeof(line) - (size_t)n, "%s\n", i + 1 < schema->column_count ? "," : "");
        append_str(&buf, &len, &cap, line);
    }

    append_str(&buf, &len, &cap, ")\n");
    return buf;
}

const char *csvdb_version(void) {
    return "0.1.0-dev";
}
