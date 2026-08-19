# Library API usage

`libcsvdb` (built as the `csvdb_core` CMake target) exposes exactly one
public header, `include/csvdb/csvdb.h`. Everything in `src/lib/` besides
`csvdb.c` is a private implementation detail -- the CLI and REPL
(`src/cli/`, `src/repl/`) only ever talk to the library through this
header too, so it's a real boundary, not just a convention.

See [`csvdb.h`](../include/csvdb/csvdb.h) itself for the authoritative,
fully-documented API surface. This page is a worked example plus the
handful of conventions worth calling out explicitly. For the SQL
language it accepts, see [`sql-grammar.md`](sql-grammar.md); for the
on-disk schema format, see [`schema-format.md`](schema-format.md).

## Linking it

From within this repository, link the `csvdb_core` CMake target:

```cmake
target_link_libraries(your_target PRIVATE csvdb_core)
```

That target already exposes `include/` publicly, so `#include
"csvdb/csvdb.h"` resolves without any extra include-path setup.

## A worked example

```c
#include <stdio.h>
#include "csvdb/csvdb.h"

int main(void) {
    char err[256];
    csvdb *db = csvdb_open("mydb", err, sizeof(err));
    if (db == NULL) {
        fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }

    /* DDL/DML: no result to read, just check the status code */
    if (csvdb_exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, "
                        "name TEXT NOT NULL)", NULL, NULL) != CSVDB_OK) {
        fprintf(stderr, "%s\n", csvdb_errmsg(db));
    }

    if (csvdb_exec(db, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')",
                   NULL, NULL) != CSVDB_OK) {
        fprintf(stderr, "%s\n", csvdb_errmsg(db));
    }

    /* SELECT: read the result, then free it */
    csvdb_result *result = NULL;
    if (csvdb_exec(db, "SELECT id, name FROM users ORDER BY id",
                   &result, NULL) != CSVDB_OK) {
        fprintf(stderr, "%s\n", csvdb_errmsg(db));
    } else {
        for (size_t r = 0; r < csvdb_result_row_count(result); r++) {
            for (size_t c = 0; c < csvdb_result_col_count(result); c++) {
                csvdb_value v = csvdb_result_get(result, r, c);
                switch (v.type) {
                case CSVDB_NULL:    printf("NULL"); break;
                case CSVDB_INTEGER: printf("%lld", v.as.integer); break;
                case CSVDB_REAL:    printf("%g", v.as.real); break;
                case CSVDB_TEXT:    printf("%s", v.as.text); break;
                case CSVDB_BOOLEAN: printf(v.as.boolean ? "true" : "false"); break;
                }
                printf(c + 1 < csvdb_result_col_count(result) ? "\t" : "\n");
            }
        }
        csvdb_result_free(result);
    }

    csvdb_close(db);
    return 0;
}
```

## Conventions worth knowing

**One statement per `csvdb_exec` call.** It matches the parser's own
contract: an optional trailing `;`, and anything else after the
statement is a parse error. There's no built-in way to run a
semicolon-separated script in one call -- see how `src/cli/main.c`'s
script-file runner and `src/repl/repl.c` split multi-statement input
themselves (on a `;` that isn't inside a string literal) if you need
that.

**Two different error-reporting conventions, deliberately.**
`csvdb_open` takes an explicit `errbuf`/`errlen` pair, because there's no
valid handle yet to attach an error message to if it fails. Every other
function that can fail reports through `csvdb_errmsg(db)` instead, once
a handle exists -- closer to how `sqlite3_errmsg()` works, and more
convenient than threading a buffer through every call site.

**`csvdb_result` owns its data.** A `csvdb_value`'s `as.text` pointer
(when `type == CSVDB_TEXT`) is borrowed from the `csvdb_result` and stays
valid only until you `csvdb_result_free()` it -- copy the string out
first if you need it to outlive the result.

**`out_result` and `out_affected_rows` are both optional.** Pass `NULL`
for either if you don't need it; `csvdb_exec` never requires a caller to
collect a result it doesn't want. For SELECT, `*out_affected_rows` (if
requested) is always 0 -- use `csvdb_result_row_count()` instead. For
INSERT/UPDATE/DELETE, `*out_result` (if requested) is always left NULL.

**Not thread-safe per handle.** A single `csvdb *` must not be used
concurrently from more than one thread. Separate handles opened on the
*same* database directory, from separate threads or separate processes,
are fine -- that's exactly what the `flock()`-based locking layer exists
for (see PLAN.md's locking section for what it does and doesn't
guarantee).

**Introspection beyond `csvdb_exec`.** `csvdb_table_count`/
`csvdb_table_name_at` list the tables in a database, and
`csvdb_table_schema_string` renders one table's column definitions as a
human-readable string -- these back the REPL's `.tables` and `.schema`
meta-commands, but are ordinary public API you can call directly too.
