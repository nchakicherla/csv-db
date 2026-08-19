#ifndef CSVDB_CLI_FORMAT_H
#define CSVDB_CLI_FORMAT_H

#include <stdbool.h>
#include <stdio.h>

#include "csvdb/csvdb.h"

typedef enum {
    FORMAT_TABLE,
    FORMAT_CSV,
    FORMAT_JSON
} OutputFormat;

/* Parses a --format argument value ("table"/"csv"/"json", case-insensitive).
 * Returns false if `name` doesn't match one of the three. */
bool format_from_name(const char *name, OutputFormat *out);

/* Writes `result` to `out` in the given format:
 *   table -- aligned, human-readable columns, NULL shown as "NULL"
 *   csv   -- reuses libcsv's writer, matching this project's on-disk CSV
 *            convention (every non-NULL field quoted, NULL as a bare
 *            empty field), so the output round-trips through storage.c
 *   json  -- a JSON array of objects, one per row, via cJSON
 * Returns false on a write error. */
bool format_write_result(FILE *out, const csvdb_result *result, OutputFormat format);

#endif /* CSVDB_CLI_FORMAT_H */
