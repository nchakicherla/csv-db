#include "csvdb/csvdb.h"

#include <stdio.h>
#include <stdlib.h>

#include "ast.h"
#include "catalog.h"
#include "executor.h"
#include "parser.h"
#include "result.h"
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

const char *csvdb_version(void) {
    return "0.1.0-dev";
}
