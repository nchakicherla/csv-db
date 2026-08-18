#ifndef CSVDB_STORAGE_H
#define CSVDB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "schema.h"
#include "value.h"

typedef struct {
    Value *values; /* length == schema->column_count, owned */
} Row;

typedef struct {
    const Schema *schema; /* borrowed: must outlive the RowSet */
    Row *rows;
    size_t row_count;
    size_t row_capacity;
} RowSet;

/* Loads `csv_path` into `out` using `schema` for type coercion. The CSV's
 * header row must match `schema`'s column names in order exactly. Each
 * field is parsed per its column type via value_parse (an empty field is
 * always NULL, per the on-disk NULL convention). Returns false and fills
 * errbuf on any I/O, structural (header/field-count mismatch), or
 * type-coercion error; `out` is left safely empty in that case. */
bool storage_load(const Schema *schema, const char *csv_path, RowSet *out,
                   char *errbuf, size_t errlen);

/* Writes `rows` to `csv_path` atomically: fully written to
 * `<csv_path>.tmp`, fsync'd, then rename()'d over the original (no
 * partial-write corruption on crash). Every non-NULL field is quoted via
 * libcsv's writer; NULL is written as a bare, unquoted empty field. */
bool storage_write(const RowSet *rows, const char *csv_path,
                    char *errbuf, size_t errlen);

void rowset_free(RowSet *rows);

#endif /* CSVDB_STORAGE_H */
