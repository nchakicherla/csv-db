# csv-db

[![CI](https://github.com/nchakicherla/csv-db/actions/workflows/ci.yml/badge.svg)](https://github.com/nchakicherla/csv-db/actions/workflows/ci.yml)

A small embedded database where every table is a plain CSV file. `csv-db`
gives you a SQL-like query language (with JOINs) over a directory of
`<table>.csv` + `<table>.schema.json` pairs, exposed three ways from one
engine:

- **`libcsvdb`** -- a small public C library (`include/csvdb/csvdb.h`)
- **`csvdb`** -- a one-shot/scriptable CLI
- **`csvdb`** (no args) -- an interactive REPL with history and meta-commands

```
            ┌─────────────┐   ┌─────────────┐
            │   csv-db CLI │   │  REPL mode  │   (one binary, two entry paths)
            └──────┬──────┘   └──────┬──────┘
                   └─────────┬───────┘
                       libcsvdb (public C API)
                              │
        ┌──────────┬─────────┼─────────┬───────────┐
        │           │         │          │           │
     lexer/      executor   catalog    storage      value
     parser      (+expr                (+lock)     (typed
     (AST)        eval)                             cells)
        │                        │          │
        └── SQL text            cJSON    libcsv
                                (schema)  (CSV I/O)
```

## Why

CSV is already the universal, human-readable, tool-friendly tabular
format -- every language and spreadsheet reads and writes it natively.
`csv-db` adds just enough structure on top (typed columns, a real query
language, JOINs, file locking) to make a directory of CSVs behave like a
small relational database, without needing a server, a binary file
format, or giving up the ability to `cat`, `grep`, or open a table
directly in a spreadsheet.

It's built from three vendored/fetched dependencies and the C standard
library: **libcsv** for CSV parsing/writing, **cJSON** for schema files,
and **linenoise** for the REPL's line editing.

## Quick start

```bash
git clone https://github.com/nchakicherla/csv-db.git
cd csv-db
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # optional: run the test suite
```

This produces `build/csvdb`, a single binary that's the CLI and the REPL.

```bash
mkdir mydb
build/csvdb -d mydb -c "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER)"
build/csvdb -d mydb -c "INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', NULL)"
build/csvdb -d mydb -c "SELECT * FROM users"
```

```
id  name   age
--  -----  ----
1   Alice  30
2   Bob    NULL
```

`mydb/` is now a plain directory with `users.csv` and `users.schema.json`
in it -- open `users.csv` in anything that reads CSV.

Start an interactive session by running with no `-c`/script argument:

```bash
build/csvdb -d mydb
csvdb> SELECT name FROM users WHERE age > 25;
csvdb> .tables
csvdb> .schema users
csvdb> .exit
```

## SQL subset

```sql
CREATE TABLE <name> ( <col> <type> [NOT NULL] [PRIMARY KEY] [REFERENCES <table>(<col>)], ... )
DROP TABLE <name>
INSERT INTO <table> [(<cols>)] VALUES (<vals>), (<vals>), ...
SELECT <cols|*> FROM <table> [AS <alias>]
    [ (INNER|LEFT) JOIN <table> [AS <alias>] ON <cond> ]*
    [ WHERE <cond> ]
    [ ORDER BY <col> [ASC|DESC] ]
    [ LIMIT <n> ]
UPDATE <table> SET <col> = <val>, ... [ WHERE <cond> ]
DELETE FROM <table> [ WHERE <cond> ]
```

Column types are `INTEGER`, `REAL`, `TEXT`, `BOOLEAN`. Expressions support
`= != < <= > >=`, `AND OR NOT`, `LIKE` (`%`/`_` wildcards), and
parentheses -- no arithmetic or aggregate functions in this version. Full
grammar, precedence, and comparison semantics (numeric promotion,
three-valued NULL logic) are in
[`docs/sql-grammar.md`](docs/sql-grammar.md).

## CLI

```
usage: csvdb [-d DIR] [-c SQL | SCRIPT] [--format table|csv|json]
       csvdb -h | --help

  -d, --db DIR        database directory (default: current directory)
  -c, --command SQL   run one SQL statement and exit
  --format FORMAT     output format for SELECT results: table (default), csv, json
  SCRIPT               a file of ';'-separated SQL statements to run and exit
  -h, --help           show this help and exit

With neither -c nor SCRIPT, starts an interactive REPL.
```

Exit codes: `0` success, `1` a runtime error (bad SQL, a constraint
violation, an I/O error), `2` a usage error (bad arguments).

`--format csv` reuses libcsv's writer, so its output matches the on-disk
convention exactly (every non-NULL field quoted, NULL as a bare empty
field) -- it round-trips cleanly back through storage. `--format json`
emits a JSON array of row objects via cJSON.

## REPL

```
Meta-commands:
  .tables          list tables in this database
  .schema <table>  show a table's column definitions
  .help            show this help
  .exit / .quit    exit the REPL

Anything else is treated as SQL. A statement may span multiple
lines; entry continues until a top-level ';' is seen.
```

History persists to `~/.csvdb_history`. Ctrl-C cancels whatever
statement you're mid-way through typing rather than exiting the REPL;
Ctrl-D at a fresh prompt exits cleanly.

## Library

```c
#include "csvdb/csvdb.h"

char err[256];
csvdb *db = csvdb_open("mydb", err, sizeof(err));

csvdb_result *result = NULL;
if (csvdb_exec(db, "SELECT * FROM users", &result, NULL) != CSVDB_OK) {
    fprintf(stderr, "%s\n", csvdb_errmsg(db));
} else {
    for (size_t r = 0; r < csvdb_result_row_count(result); r++) {
        for (size_t c = 0; c < csvdb_result_col_count(result); c++) {
            csvdb_value v = csvdb_result_get(result, r, c);
            /* ... use v.type / v.as.* ... */
        }
    }
    csvdb_result_free(result);
}

csvdb_close(db);
```

Link the `csvdb_core` CMake target (it exposes `include/` publicly) to
use it from another target in this repo. See
[`include/csvdb/csvdb.h`](include/csvdb/csvdb.h) for the full,
documented API, and [`docs/library-api.md`](docs/library-api.md) for a
complete worked example and the API's conventions (error handling,
result ownership, thread-safety).

## On-disk format

A "database" is just a directory. Each table is a CSV file plus a JSON
schema file describing its columns:

```
mydb/
  users.csv
  users.schema.json
  users.csv.lock          (created on demand by the locking layer)
```

```json
{
  "name": "users",
  "columns": [
    {"name": "id",   "type": "INTEGER", "nullable": false, "primary_key": true},
    {"name": "name", "type": "TEXT",    "nullable": false},
    {"name": "age",  "type": "INTEGER", "nullable": true}
  ]
}
```

Writes are atomic (`.tmp` + `fsync` + `rename()`), and reads/writes take
a shared/exclusive `flock()` on the table's `.lock` file for safety
between concurrent processes -- advisory locking, not full
transactions/MVCC. Full details, including the NULL representation and
identifier rules, are in [`docs/schema-format.md`](docs/schema-format.md).

## Documentation

- [`docs/schema-format.md`](docs/schema-format.md) -- the `.schema.json` format in full
- [`docs/sql-grammar.md`](docs/sql-grammar.md) -- the SQL grammar in EBNF, precedence, comparison semantics
- [`docs/library-api.md`](docs/library-api.md) -- library usage, conventions, a worked example
- [`docs/repl-manual-test-checklist.md`](docs/repl-manual-test-checklist.md) -- what to check by hand when touching the REPL
- [`PLAN.md`](PLAN.md) -- the phased build plan and architecture decisions this project was built from

## Project layout

```
csv-db/
  include/csvdb/      public library header
  src/
    lib/               libcsvdb: value/schema/storage/lock/catalog/lexer/parser/
                        expr_eval/executor/result/csvdb (the public API glue)
    cli/               argument parsing, one-shot/script execution, output formatting
    repl/               the interactive REPL
  third_party/         vendored libcsv and linenoise (see VERSIONS.md for pinned commits)
  tests/
    unit/               one executable per module, registered with CTest
    integration/         full-stack tests through the public API and internals
    fixtures/            sample schema/CSV files used by the tests
  docs/
```

## Building blocks and their licenses

`csv-db`'s own code has no license file yet. The three dependencies it
builds on carry their own licenses:

- **libcsv** (vendored, `third_party/libcsv/`) -- LGPL-2.1
- **linenoise** (vendored, `third_party/linenoise/`) -- BSD
- **cJSON** (fetched at configure time) -- MIT

See [`third_party/VERSIONS.md`](third_party/VERSIONS.md) for the exact
pinned commit of each vendored dependency.
