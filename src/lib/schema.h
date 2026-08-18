#ifndef CSVDB_SCHEMA_H
#define CSVDB_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"

typedef struct {
    char *table;
    char *column;
} ForeignKeyRef;

typedef struct {
    char *name;
    ValueType type;
    bool nullable;
    bool primary_key;
    bool has_foreign_key;
    ForeignKeyRef foreign_key;
} Column;

typedef struct {
    char *name;
    Column *columns;
    size_t column_count;
} Schema;

/* Maps a schema.json "type" string ("INTEGER"/"REAL"/"TEXT"/"BOOLEAN") to a
 * ValueType. Returns false if `name` isn't one of the four canonical names. */
bool schema_type_from_name(const char *name, ValueType *out);

/* Inverse of schema_type_from_name. */
const char *schema_type_to_name(ValueType type);

/* Parses a schema.json document's text into a new heap-allocated Schema,
 * validating structure along the way: duplicate column names, unknown
 * types, malformed identifiers, more than one primary_key column, and
 * malformed foreign_key objects. Returns NULL and fills errbuf on any
 * parse or validation failure. Caller owns the result (schema_free it). */
Schema *schema_parse_json(const char *json_text, char *errbuf, size_t errlen);

/* Re-runs the structural checks schema_parse_json applies. Exposed
 * separately for schemas built programmatically (e.g. from a CREATE TABLE
 * statement in a later phase) rather than parsed from disk. */
bool schema_validate(const Schema *schema, char *errbuf, size_t errlen);

/* Serializes `schema` back to a schema.json document. Caller frees the
 * returned string. */
char *schema_to_json_string(const Schema *schema);

/* Index of the column named `name` in `schema`, or -1 if not found. */
int schema_find_column(const Schema *schema, const char *name);

/* Appends one column to `schema`, growing its columns array by one. On
 * success, `name`, `fk_table`, and `fk_column` (the latter two only used
 * when has_foreign_key is true) are owned by `schema` from then on. On
 * failure (out of memory), ownership is not transferred -- the caller
 * still owns and must free them. Intended for building a Schema
 * incrementally (e.g. the CREATE TABLE parser), one column at a time,
 * where schema_free stays correct no matter how far construction got. */
bool schema_append_column(Schema *schema, char *name, ValueType type, bool nullable,
                           bool primary_key, bool has_foreign_key,
                           char *fk_table, char *fk_column);

void schema_free(Schema *schema);

#endif /* CSVDB_SCHEMA_H */
