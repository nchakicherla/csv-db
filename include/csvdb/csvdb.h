/**
 * csvdb.h -- public API for libcsvdb, a CSV-backed database engine with a
 * SQL-like query language.
 *
 * A "database" is a directory of `<table>.csv` + `<table>.schema.json`
 * pairs (see PLAN.md's On-disk format section). Typical usage:
 *
 *   char err[256];
 *   csvdb *db = csvdb_open("mydb", err, sizeof(err));
 *   if (db == NULL) { fprintf(stderr, "%s\n", err); exit(1); }
 *
 *   csvdb_result *result = NULL;
 *   if (csvdb_exec(db, "SELECT * FROM users", &result, NULL) != CSVDB_OK) {
 *       fprintf(stderr, "%s\n", csvdb_errmsg(db));
 *   } else if (result != NULL) {
 *       for (size_t r = 0; r < csvdb_result_row_count(result); r++) {
 *           for (size_t c = 0; c < csvdb_result_col_count(result); c++) {
 *               csvdb_value v = csvdb_result_get(result, r, c);
 *               // ... use v.type / v.as.* ...
 *           }
 *       }
 *       csvdb_result_free(result);
 *   }
 *
 *   csvdb_close(db);
 *
 * All strings passed in are expected to be valid, NUL-terminated UTF-8 (or
 * plain ASCII); the library does no locale-aware processing.
 */

#ifndef CSVDB_H
#define CSVDB_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to an open database. Create with csvdb_open, release
 * with csvdb_close. Not thread-safe: a given handle must not be used
 * concurrently from more than one thread (separate handles opened on the
 * same directory are fine -- that's what the file-locking layer is for). */
typedef struct csvdb csvdb;

/** Opaque handle to the result of a SELECT, returned by csvdb_exec.
 * Release with csvdb_result_free once you're done reading from it. */
typedef struct csvdb_result csvdb_result;

/** Outcome of a csvdb_exec call. On anything other than CSVDB_OK, call
 * csvdb_errmsg(db) for a human-readable description. */
typedef enum {
    CSVDB_OK = 0,    /**< The statement ran successfully. */
    CSVDB_ERROR = 1, /**< A parse, validation, or storage error occurred. */
    CSVDB_MISUSE = 2 /**< Invalid arguments to the API itself (e.g. a NULL handle). */
} csvdb_code;

/** The type of one column/cell. Mirrors the four schema column types
 * (INTEGER, REAL, TEXT, BOOLEAN) plus NULL for an absent value. */
typedef enum {
    CSVDB_NULL,
    CSVDB_INTEGER,
    CSVDB_REAL,
    CSVDB_TEXT,
    CSVDB_BOOLEAN
} csvdb_type;

/** A single cell's value, returned by csvdb_result_get. `as.text` (only
 * meaningful when type == CSVDB_TEXT) is borrowed: it's valid until the
 * owning csvdb_result is freed, and must not be modified or freed by the
 * caller. */
typedef struct {
    csvdb_type type;
    union {
        long long integer;
        double real;
        const char *text;
        bool boolean;
    } as;
} csvdb_value;

/**
 * Opens an existing database directory. Does not create `dir` -- it must
 * already exist. Returns NULL and fills errbuf on failure (a missing or
 * non-directory path, or a corrupted schema file in it); returns a valid
 * handle (to be released with csvdb_close) on success.
 */
csvdb *csvdb_open(const char *dir, char *errbuf, size_t errlen);

/** Closes `db` and releases all resources associated with it. Safe to
 * call with NULL (a no-op). Any csvdb_result values obtained from this
 * handle should be freed first, though they don't depend on the handle
 * staying open (a Result is a self-contained snapshot). */
void csvdb_close(csvdb *db);

/**
 * Executes exactly one SQL statement (an optional trailing ';' is
 * allowed; anything else after the statement is a parse error -- this
 * function does not run multi-statement scripts, see PLAN.md's CLI/REPL
 * phases for that layer).
 *
 * For a SELECT that succeeds, *out_result (if out_result is non-NULL) is
 * set to a newly allocated csvdb_result the caller must csvdb_result_free.
 * For every other statement kind, or if out_result is NULL, no result is
 * returned. If out_affected_rows is non-NULL, it's set to the number of
 * rows inserted/updated/deleted (0 for CREATE/DROP/SELECT).
 *
 * Returns CSVDB_OK on success. On failure, returns CSVDB_ERROR (or
 * CSVDB_MISUSE for a NULL db/sql) and leaves a description retrievable
 * via csvdb_errmsg(db); *out_result and *out_affected_rows are left
 * empty/zero in that case.
 */
csvdb_code csvdb_exec(csvdb *db, const char *sql, csvdb_result **out_result,
                       size_t *out_affected_rows);

/** The message for the most recent non-OK csvdb_exec call on `db` (or
 * an empty string if none has failed yet). Never NULL. The returned
 * pointer is owned by `db` and is only valid until the next csvdb_exec
 * call or csvdb_close. */
const char *csvdb_errmsg(csvdb *db);

/** Releases a csvdb_result obtained from csvdb_exec. Safe to call with
 * NULL (a no-op). */
void csvdb_result_free(csvdb_result *result);

size_t csvdb_result_row_count(const csvdb_result *result);
size_t csvdb_result_col_count(const csvdb_result *result);

/** The display name of column `col` (its bare column name, never
 * table-qualified). Returns NULL if `col` is out of range. The returned
 * pointer is owned by `result`. */
const char *csvdb_result_col_name(const csvdb_result *result, size_t col);

/** The declared type of column `col`. Returns CSVDB_NULL if `col` is out
 * of range (indistinguishable from a column whose values happen to all
 * be NULL -- check row_count/col_count first if that matters). */
csvdb_type csvdb_result_col_type(const csvdb_result *result, size_t col);

/** The value at (`row`, `col`). Returns a CSVDB_NULL value if either
 * index is out of range. */
csvdb_value csvdb_result_get(const csvdb_result *result, size_t row, size_t col);

/** The library's version string, e.g. "0.1.0-dev". */
const char *csvdb_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CSVDB_H */
