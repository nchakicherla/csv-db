#include "result.h"

#include <stdlib.h>

size_t result_row_count(const Result *result) {
    return result->row_count;
}

size_t result_col_count(const Result *result) {
    return result->column_count;
}

const char *result_col_name(const Result *result, size_t col) {
    if (col >= result->column_count) {
        return NULL;
    }
    return result->columns[col].name;
}

ValueType result_col_type(const Result *result, size_t col) {
    if (col >= result->column_count) {
        return VALUE_NULL;
    }
    return result->columns[col].type;
}

const Value *result_get(const Result *result, size_t row, size_t col) {
    if (row >= result->row_count || col >= result->column_count) {
        return NULL;
    }
    return &result->rows[row][col];
}

void result_free(Result *result) {
    if (result == NULL) {
        return;
    }
    for (size_t i = 0; i < result->column_count; i++) {
        free(result->columns[i].name);
    }
    free(result->columns);
    for (size_t r = 0; r < result->row_count; r++) {
        for (size_t c = 0; c < result->column_count; c++) {
            value_free(&result->rows[r][c]);
        }
        free(result->rows[r]);
    }
    free(result->rows);
    free(result);
}
