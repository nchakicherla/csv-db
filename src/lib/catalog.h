#ifndef CSVDB_CATALOG_H
#define CSVDB_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#include "lock.h"
#include "schema.h"
#include "storage.h"

typedef struct {
    char *name;
    Schema *schema; /* NULL until first accessed; owned once loaded */
} CatalogEntry;

typedef struct {
    char *dir; /* database directory path, owned */
    CatalogEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
} Catalog;

/* Opens an existing database directory, discovering tables by scanning
 * for "*.schema.json" files. Schema contents are loaded lazily (on first
 * catalog_get_schema/catalog_read_table/catalog_write_table call) and
 * cached from then on. Returns false and fills errbuf if `dir` doesn't
 * exist or isn't a directory, or contains a schema.json file whose
 * derived table name fails the identifier check (a corrupted or
 * tampered-with database directory). Does not create `dir`. */
bool catalog_open(const char *dir, Catalog *out, char *errbuf, size_t errlen);

void catalog_close(Catalog *catalog);

size_t catalog_table_count(const Catalog *catalog);
const char *catalog_table_name_at(const Catalog *catalog, size_t index);
bool catalog_has_table(const Catalog *catalog, const char *table_name);

/* Loads and caches (or returns the already-cached) Schema for
 * `table_name`. The returned pointer is owned by the catalog and stays
 * valid until catalog_drop_table or catalog_close. */
const Schema *catalog_get_schema(Catalog *catalog, const char *table_name,
                                  char *errbuf, size_t errlen);

/* Creates a new table: writes `<dir>/<name>.schema.json` and a
 * header-only `<dir>/<name>.csv` (both atomically), then registers it in
 * the catalog. `schema` is checked with schema_validate, and its name is
 * independently re-checked against path-unsafe characters (defense in
 * depth alongside the parser-level identifier check -- see PLAN.md's
 * Identifiers section) before it's used to build any path. Fails if a
 * table with that name already exists. */
bool catalog_create_table(Catalog *catalog, const Schema *schema,
                           char *errbuf, size_t errlen);

/* Removes `<dir>/<name>.csv`, `<dir>/<name>.schema.json`, and (if
 * present) `<dir>/<name>.csv.lock`, and drops the catalog entry. */
bool catalog_drop_table(Catalog *catalog, const char *table_name,
                         char *errbuf, size_t errlen);

/* Reads/writes a table's rows, wrapped in a shared/exclusive flock() on
 * `<name>.csv.lock`. Convenient for simple single-table, single-lock
 * access. A statement that needs to hold locks across more than one
 * table (a JOIN, an FK-checking INSERT/UPDATE) must NOT mix these with
 * catalog_lock_tables below: acquiring a second, independent flock() on
 * the same file from the same process (a fresh open() gets its own lock
 * identity) can self-deadlock. Such statements should call
 * catalog_lock_tables once for everything they need, then read/write via
 * storage_load/storage_write directly using catalog_table_csv_path --
 * this is what executor.c does. */
bool catalog_read_table(Catalog *catalog, const char *table_name, RowSet *out,
                         char *errbuf, size_t errlen);
bool catalog_write_table(Catalog *catalog, const char *table_name, const RowSet *rows,
                          char *errbuf, size_t errlen);

/* Locks several tables at once for a multi-table statement (JOINs,
 * FK-checking INSERTs), always in a fixed order -- sorted by name, with
 * duplicate names (e.g. a self-join) collapsed to a single lock -- so two
 * statements locking the same tables can never deadlock by acquiring
 * them in opposite order. `out_locks` must have room for `count`
 * entries; on success *out_locked_count is how many were actually
 * acquired (<= count, once duplicates collapse). On any failure,
 * already-acquired locks are released before returning false. */
bool catalog_lock_tables(Catalog *catalog, const char **table_names, size_t count,
                          LockMode mode, TableLock *out_locks, size_t *out_locked_count,
                          char *errbuf, size_t errlen);
void catalog_unlock_tables(TableLock *locks, size_t count);

/* Builds "<dir>/<table_name>.csv" (caller frees). For callers -- namely
 * executor.c -- that manage their own locking via catalog_lock_tables
 * and talk to storage_load/storage_write directly. */
char *catalog_table_csv_path(const Catalog *catalog, const char *table_name);

#endif /* CSVDB_CATALOG_H */
