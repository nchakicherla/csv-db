# csv-db: CSV-backed database with SQL, REPL, and library

**Status:** Plan approved, implementation not yet started (repo still has only the
placeholder `CMakeLists.txt` / `src/main.c` hello-world). Start at Phase 0.

## Context

`csv-db` is currently a bare CMake "hello, world" C project. The goal is to grow it into
a real embedded-style database: a `libcsvdb` core library, a CLI, and an interactive REPL,
all built on top of three specific dependencies:

- **libcsv** — CSV parsing/writing (streaming, callback-based)
- **cJSON** — parsing/writing the JSON schema files that define each table's columns/types
- **linenoise** — line editing, history, and multi-line input for the REPL

Per the user's decisions:
- Query interface: a **SQL-like language** (not ad-hoc verbs), including **JOIN** support.
- Tables can reference each other and be **joined at query time** (full joins, not just FKs).
- On-disk layout: a **directory of CSV+schema pairs**, one pair per table.
- Concurrency: **file locking** (flock-based, advisory) for write safety — no full
  transactions/rollback in v1.

This plan lays out a phased build order so each layer is independently testable before the
next is built on top of it, ending with a working library + CLI + REPL that share one engine.

## Architecture

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

One binary (`csvdb`) links `libcsvdb`. Given no args / no `-c`/script argument, it starts
the linenoise REPL; given `-c "<SQL>"` or a script file, it runs one-shot and exits. This
mirrors `sqlite3`/`psql`/`redis-cli` and avoids maintaining two near-duplicate entrypoints,
while still giving three real components: the library, the CLI parsing/formatting layer,
and the REPL loop on top of it.

## On-disk format

A "database" is a directory. Each table `T` is a pair of files:

```
mydb/
  users.csv
  users.schema.json
  departments.csv
  departments.schema.json
  users.csv.lock          (created on demand by the locking layer)
```

`users.schema.json` (parsed/written via cJSON):
```json
{
  "name": "users",
  "columns": [
    {"name": "id",      "type": "INTEGER", "nullable": false, "primary_key": true},
    {"name": "name",    "type": "TEXT",    "nullable": false},
    {"name": "age",     "type": "INTEGER", "nullable": true},
    {"name": "dept_id", "type": "INTEGER", "nullable": true,
     "foreign_key": {"table": "departments", "column": "id"}}
  ]
}
```
v1 types: `INTEGER`, `REAL`, `TEXT`, `BOOLEAN` (extensible later, e.g. `DATE`).

`users.csv` has a header row matching the schema's column order; libcsv does the actual
field-level parsing/escaping, the schema layer does type coercion and validation.

**Write safety:** writes (INSERT/UPDATE/DELETE/CREATE/DROP) go to `<table>.csv.tmp`, then
`fsync` + `rename()` over the original — atomic on POSIX, avoids partial-write corruption.
Reads take a shared `flock()` on `<table>.csv.lock`; writes take an exclusive lock. This is
advisory locking for safety between processes, not MVCC/transactions.

## SQL subset (v1)

```
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
Expressions: literals, `col` / `table.col` refs, `= != < <= > >=`, `AND OR NOT`, `LIKE`,
parentheses. No arithmetic/aggregates in v1 (noted as a clean future extension point).

Execution model: **whole-table-in-memory** per query (read full CSV into a `RowSet`, no
streaming/indexing). This is the right complexity trade-off for CSV-scale data and keeps
the executor a straightforward nested-loop join + filter + project + sort pipeline. Indexing
and streaming execution are natural v2 work if tables get large.

## Module layout

```
csv-db/
  CMakeLists.txt
  third_party/
    libcsv/          csv.c, csv.h (vendored — no upstream CMake support, LGPL-2.1)
    linenoise/       linenoise.c, linenoise.h (vendored — no build system upstream, BSD)
                     (cJSON pulled via CMake FetchContent — it ships its own CMakeLists.txt)
  include/csvdb/
    csvdb.h          public library API
  src/
    lib/
      value.c/.h       tagged-union Value type (NULL/INT/REAL/TEXT/BOOL) + comparisons
      schema.c/.h      cJSON <-> Schema struct, validation
      storage.c/.h     libcsv-backed table read/write, RowSet, atomic rewrite
      lock.c/.h        flock() wrapper (shared/exclusive acquire+release)
      catalog.c/.h     database dir: table discovery, create/drop, schema cache
      lexer.c/.h       SQL tokenizer
      ast.h            AST node types
      parser.c/.h      recursive-descent parser -> AST
      expr_eval.c/.h   expression evaluation over a row context
      executor.c/.h    AST -> execution: scan/join/filter/project/sort/limit, DML/DDL
      result.c/.h      result-set struct + iteration API
      error.c/.h       error codes/messages
      csvdb.c          public API glue (open/close/exec)
    cli/
      main.c           arg parsing, one-shot/script execution, format selection
      format.c/.h      table/csv/json output formatting
    repl/
      main.c           linenoise loop entry
      repl.c/.h        multi-line buffering, history file, meta-commands (.tables etc.)
  tests/
    unit/              lexer, parser, schema, storage, value round-trip tests
    integration/        fixture DBs + SQL -> expected-output tests, run via libcsvdb directly
    fixtures/          sample .csv/.schema.json files
  examples/
    sample_db/
  docs/
    schema-format.md
    sql-grammar.md
    library-api.md
```

## Dependency integration

- **cJSON**: `FetchContent` from GitHub — it has a working upstream `CMakeLists.txt`, so
  this is the clean case. Bump `cmake_minimum_required` to 3.15 (FetchContent needs 3.11+;
  3.15 gives headroom for other modern CMake features).
- **linenoise**: vendor `linenoise.c`/`linenoise.h` directly into `third_party/linenoise/`
  — it's ~2 files with no upstream build system, so vendoring is simpler than fighting a
  FetchContent+custom-build setup. Add as a small local static-lib CMake target.
- **libcsv**: vendor `csv.c`/`csv.h` into `third_party/libcsv/` — upstream ships
  autotools (`configure`/`Makefile`), not CMake; vendoring the two source files and
  compiling them directly under our own CMake target avoids mixing build systems.
- Record the exact upstream commit/tag pulled for each vendored library in a
  `third_party/VERSIONS.md` so updates are traceable.
- **License note**: libcsv is LGPL-2.1. Static-linking LGPL code carries relinking
  obligations. Default to building `libcsv`'s vendored sources as a separate object/static
  archive (not `#include`d directly into `libcsvdb`'s sources) so it stays a distinct,
  swappable unit — this is a documentation/packaging concern for later, not a blocker now.

## Phased build order

Each phase should compile, pass its own tests, and be committed before moving on.

**Phase 0 — Scaffolding & dependencies**
Restructure into the module layout above; vendor libcsv + linenoise; wire cJSON via
FetchContent; define CMake targets (`csvdb` static lib, `csvdb` executable); smoke test
that everything links and the binary runs.

**Phase 1 — Value system & schema (cJSON)**
`value.{c,h}`: typed `Value` (NULL/INT/REAL/TEXT/BOOL), comparisons, stringification.
`schema.{c,h}`: parse `<table>.schema.json` into a `Schema` struct via cJSON, validate
(duplicate columns, known types), serialize a `Schema` back to JSON for `CREATE TABLE`.
Unit tests: valid/invalid schema fixtures, parse/serialize round-trip.

**Phase 2 — Storage engine (libcsv)**
`storage.{c,h}`: load a table (schema + CSV) into an in-memory `RowSet` using libcsv's
streaming callbacks, with schema-driven type coercion (reject bad values with a clear
error); write a `RowSet` back out via libcsv's writer with correct quoting; atomic
`.tmp` + `rename()` write path. No locking yet — correctness first.
Unit tests: write known rows, read back, assert equality; quoting edge cases (commas,
quotes, embedded newlines).

**Phase 3 — Catalog & locking**
`catalog.{c,h}`: open a database directory, discover tables (scan `*.schema.json`),
lazy-load/cache schemas, create/drop table (writes schema.json + header-only CSV).
`lock.{c,h}`: flock()-based shared/exclusive acquire+release, wired into storage's
read/write entry points from here on.
Test: a forked-child contention test proving a writer excludes readers/other writers.

**Phase 4 — SQL lexer & parser**
`lexer.{c,h}`, `ast.h`, `parser.{c,h}`: tokenizer + recursive-descent parser for all
statement types in the grammar above, with line/column error reporting.
Unit tests: table-driven — one SQL string in, expected AST shape (or expected parse
error) out, covering every statement type and common malformed input.

**Phase 5 — Query executor**
`expr_eval.{c,h}`: evaluate WHERE/ON expressions against a row context (handles
qualified `table.col`). `executor.{c,h}`: CREATE/DROP -> catalog; INSERT -> schema
validation (NOT NULL, PK uniqueness, FK existence) + storage append; UPDATE/DELETE ->
load, mutate/filter in memory, atomic rewrite; SELECT -> scan, nested-loop
INNER/LEFT JOIN, WHERE filter, projection, ORDER BY, LIMIT. `result.{c,h}`: result-set
struct returned to callers.
Integration tests: 2-3 fixture tables with an FK relationship, a suite of SQL
statements with expected output, executed directly through the library.

**Phase 6 — Public library API**
Finalize `include/csvdb/csvdb.h`: `csvdb_open/close`, `csvdb_exec`, result iteration
(`row_count`/`col_count`/`col_name`/`get`), error codes, version info. This header gets
real doc comments since it's the library's contract; implementation files stay
comment-free per usual style.

**Phase 7 — CLI**
`cli/main.c` + `format.{c,h}`: `-d/--db <dir>`, `-c/--command "<SQL>"`, script-file
positional arg, `--format table|csv|json` (csv output reuses libcsv's writer, json
reuses cJSON), meaningful exit codes, errors to stderr.

**Phase 8 — REPL (linenoise)**
`repl/main.c` + `repl.{c,h}`: prompt `csvdb> ` / continuation `...> ` until `;`,
persistent history file, meta-commands (`.tables`, `.schema <t>`, `.help`, `.exit`),
SIGINT handling, reuses `format.c` for output. Documented manual test checklist
(REPL is inherently interactive, not unit-testable the same way).

**Phase 9 — Hardening, docs, packaging**
Remaining edge cases (NULL handling, type-coercion errors, FK violations, lock
contention under load, malformed CSV recovery). `docs/`: schema format reference, SQL
grammar (EBNF), library API usage example. Optional: CMake install rules, GitHub
Actions CI running `ctest` on push.

## Testing strategy

- Unit tests per module (lexer, parser, schema, storage, value) as small executables
  registered with **CTest** — no external test framework needed, keeps the dependency
  set exactly the three requested libraries plus stdlib.
- Integration tests: fixture database directories + SQL scripts with expected output,
  run through `libcsvdb` directly (fast, no process spawn) and, once Phase 7 lands,
  also spot-checked through the actual CLI binary.
- REPL gets a manual checklist (Phase 8) since interactive line-editing isn't
  practically unit-testable.

## Verification (per phase)

- `cmake -S . -B build && cmake --build build` succeeds with no warnings from our own
  code (`-Wall -Wextra -Werror` on `src/`, not on `third_party/`).
- `ctest --test-dir build` passes all registered unit/integration tests for that phase.
- From Phase 7 on: run the built binary against `examples/sample_db` with a few
  representative `-c "SELECT ..."` calls and confirm output matches expectations.
- From Phase 8 on: manually run the REPL, exercise multi-line input, meta-commands,
  and history (`~/.csvdb_history`) per the documented checklist.
