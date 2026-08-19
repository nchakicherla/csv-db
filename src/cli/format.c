#include "format.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "csv.h"

static bool ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool format_from_name(const char *name, OutputFormat *out) {
    if (ieq(name, "table")) { *out = FORMAT_TABLE; return true; }
    if (ieq(name, "csv"))   { *out = FORMAT_CSV;   return true; }
    if (ieq(name, "json"))  { *out = FORMAT_JSON;  return true; }
    return false;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* Renders a cell for human/CSV display. NULL becomes the literal string
 * "null_text" so callers can pick what NULL looks like in their format. */
static char *stringify_value(csvdb_value v, const char *null_text) {
    char buf[64];
    switch (v.type) {
    case CSVDB_NULL:
        return dup_str(null_text);
    case CSVDB_INTEGER:
        snprintf(buf, sizeof(buf), "%lld", v.as.integer);
        return dup_str(buf);
    case CSVDB_REAL:
        snprintf(buf, sizeof(buf), "%.17g", v.as.real);
        return dup_str(buf);
    case CSVDB_BOOLEAN:
        return dup_str(v.as.boolean ? "true" : "false");
    case CSVDB_TEXT:
        return dup_str(v.as.text);
    }
    return dup_str(null_text);
}

static bool write_table(FILE *out, const csvdb_result *result) {
    size_t cols = csvdb_result_col_count(result);
    size_t rows = csvdb_result_row_count(result);
    if (cols == 0) {
        return true;
    }

    size_t *widths = malloc(cols * sizeof(size_t));
    for (size_t c = 0; c < cols; c++) {
        widths[c] = strlen(csvdb_result_col_name(result, c));
    }

    char ***cells = rows > 0 ? malloc(rows * sizeof(char **)) : NULL;
    for (size_t r = 0; r < rows; r++) {
        cells[r] = malloc(cols * sizeof(char *));
        for (size_t c = 0; c < cols; c++) {
            char *s = stringify_value(csvdb_result_get(result, r, c), "NULL");
            cells[r][c] = s;
            size_t len = strlen(s);
            if (len > widths[c]) {
                widths[c] = len;
            }
        }
    }

    for (size_t c = 0; c < cols; c++) {
        fprintf(out, "%-*s%s", (int)widths[c], csvdb_result_col_name(result, c),
                c + 1 < cols ? "  " : "\n");
    }
    for (size_t c = 0; c < cols; c++) {
        for (size_t i = 0; i < widths[c]; i++) {
            fputc('-', out);
        }
        fputs(c + 1 < cols ? "  " : "\n", out);
    }
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            fprintf(out, "%-*s%s", (int)widths[c], cells[r][c], c + 1 < cols ? "  " : "\n");
        }
    }

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            free(cells[r][c]);
        }
        free(cells[r]);
    }
    free(cells);
    free(widths);
    return true;
}

static bool write_csv(FILE *out, const csvdb_result *result) {
    size_t cols = csvdb_result_col_count(result);
    size_t rows = csvdb_result_row_count(result);

    for (size_t c = 0; c < cols; c++) {
        if (c > 0 && fputc(',', out) == EOF) {
            return false;
        }
        const char *name = csvdb_result_col_name(result, c);
        if (csv_fwrite2(out, name, strlen(name), CSV_QUOTE) != 0) {
            return false;
        }
    }
    if (fputc('\n', out) == EOF) {
        return false;
    }

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            if (c > 0 && fputc(',', out) == EOF) {
                return false;
            }
            csvdb_value v = csvdb_result_get(result, r, c);
            if (v.type == CSVDB_NULL) {
                continue; /* bare empty field, matching storage.c's on-disk convention */
            }
            char *s = stringify_value(v, "");
            int rc = csv_fwrite2(out, s, strlen(s), CSV_QUOTE);
            free(s);
            if (rc != 0) {
                return false;
            }
        }
        if (fputc('\n', out) == EOF) {
            return false;
        }
    }
    return true;
}

static bool write_json(FILE *out, const csvdb_result *result) {
    cJSON *arr = cJSON_CreateArray();
    size_t cols = csvdb_result_col_count(result);
    size_t rows = csvdb_result_row_count(result);

    for (size_t r = 0; r < rows; r++) {
        cJSON *obj = cJSON_CreateObject();
        for (size_t c = 0; c < cols; c++) {
            const char *name = csvdb_result_col_name(result, c);
            csvdb_value v = csvdb_result_get(result, r, c);
            switch (v.type) {
            case CSVDB_NULL: cJSON_AddNullToObject(obj, name); break;
            case CSVDB_INTEGER: cJSON_AddNumberToObject(obj, name, (double)v.as.integer); break;
            case CSVDB_REAL: cJSON_AddNumberToObject(obj, name, v.as.real); break;
            case CSVDB_BOOLEAN: cJSON_AddBoolToObject(obj, name, v.as.boolean); break;
            case CSVDB_TEXT: cJSON_AddStringToObject(obj, name, v.as.text); break;
            }
        }
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_Print(arr);
    bool ok = json != NULL;
    if (ok) {
        ok = fputs(json, out) >= 0 && fputc('\n', out) != EOF;
    }
    free(json);
    cJSON_Delete(arr);
    return ok;
}

bool format_write_result(FILE *out, const csvdb_result *result, OutputFormat format) {
    switch (format) {
    case FORMAT_TABLE: return write_table(out, result);
    case FORMAT_CSV: return write_csv(out, result);
    case FORMAT_JSON: return write_json(out, result);
    }
    return false;
}
