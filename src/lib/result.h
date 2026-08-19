#ifndef CSVDB_RESULT_H
#define CSVDB_RESULT_H

#include <stddef.h>

#include "value.h"

typedef struct {
    char *name;
    ValueType type;
} ResultColumn;

typedef struct {
    ResultColumn *columns;
    size_t column_count;
    Value **rows; /* row_count arrays, each column_count Values long */
    size_t row_count;
} Result;

size_t result_row_count(const Result *result);
size_t result_col_count(const Result *result);
const char *result_col_name(const Result *result, size_t col);
ValueType result_col_type(const Result *result, size_t col);
const Value *result_get(const Result *result, size_t row, size_t col);

void result_free(Result *result);

#endif /* CSVDB_RESULT_H */
