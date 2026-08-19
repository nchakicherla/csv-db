# Schema file format

Every table `T` in a csvdb database directory is a pair of files:

```
mydb/
  T.csv           the data, one header row + zero or more data rows
  T.schema.json   the schema described in this document
  T.csv.lock      created on demand by the locking layer; not user-managed
```

## Structure

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

| Field                  | Type   | Required | Default | Meaning                                                   |
|-------------------------|--------|----------|---------|-------------------------------------------------------------|
| `name`                  | string | yes      | --      | The table's name; must match the identifier rule below.     |
| `columns`                | array  | yes      | --      | At least one column.                                          |
| `columns[].name`          | string | yes      | --      | Column name; must match the identifier rule below.           |
| `columns[].type`          | string | yes      | --      | One of `INTEGER`, `REAL`, `TEXT`, `BOOLEAN` (exact case).      |
| `columns[].nullable`       | bool   | no       | `true`  | Whether the column accepts NULL.                              |
| `columns[].primary_key`     | bool   | no       | `false` | At most one column per table may set this.                   |
| `columns[].foreign_key`      | object | no       | (absent) | `{"table": "...", "column": "..."}` -- see below.            |

## Identifiers

Both the table's `name` and every `columns[].name` must match
`[A-Za-z_][A-Za-z0-9_]*` -- a letter or underscore, then any number of
letters, digits, or underscores. This isn't just a style rule: a table's
name is used directly to build its `.csv`/`.schema.json`/`.csv.lock` file
paths, so it's re-validated independently by both the SQL parser (which
can never produce a differently-shaped identifier token in the first
place) and the catalog layer (which re-checks it before touching any
path, as defense in depth against a hand-edited or otherwise corrupted
schema file).

## Types

v1 supports exactly four column types:

- `INTEGER` -- a 64-bit signed integer
- `REAL` -- a double-precision floating point number
- `TEXT` -- a UTF-8/ASCII string
- `BOOLEAN` -- `true` or `false`

There's no `DATE` or numeric-precision type in v1; storing dates as
`TEXT` in a fixed format (e.g. ISO 8601) is the current workaround.

## NULL representation

A NULL value is written as a completely empty CSV field -- no quotes, no
literal `NULL` token. On read, an empty field is *always* interpreted as
NULL, regardless of the column's declared type. One consequence: an empty
`TEXT` string (`""`) is indistinguishable from NULL. This is a known, v1
limitation rather than something the format tries to work around.

Every other (non-NULL) field is always written wrapped in double quotes,
with embedded quotes doubled (standard CSV escaping) -- regardless of
whether quoting was strictly necessary for that particular value. This is
a deliberately simple, always-quote convention rather than a
minimal-quoting one.

## Primary keys

At most one column may have `"primary_key": true`. There's no support
for composite (multi-column) primary keys in v1. A primary-key column's
uniqueness is enforced at INSERT/UPDATE time by scanning the table's
existing rows -- there's no index, which is an appropriate trade-off at
CSV scale but means uniqueness checks are O(n) per write.

## Foreign keys

`foreign_key` is a structural reference only:

```json
{"table": "departments", "column": "id"}
```

It does **not** get cross-validated against the referenced table's actual
columns at schema-parse time (schema files are parsed and validated in
isolation, without access to the rest of the database directory) -- that
check happens later, when a query actually needs to resolve the
reference (e.g. at INSERT/UPDATE time, when the executor loads the
referenced table to check the value exists). A `foreign_key` whose
`table` or `column` doesn't actually exist will surface as a runtime
error the first time it's exercised, not at schema-load time.

There's no `ON DELETE`/`ON UPDATE` cascade behavior in v1: deleting a row
that other rows reference via foreign key does not cascade, restrict, or
error -- it's simply not checked (see PLAN.md's Phase 5 scope note).

## Defaults on parse

If `nullable` is omitted, it defaults to `true` (matching plain SQL,
where a column is nullable unless `NOT NULL` is given). If `primary_key`
is omitted, it defaults to `false`. If `foreign_key` is omitted, the
column has none.

## Validation performed on every parse

- `name` and every column `name` match the identifier rule above.
- No two columns share a name.
- Every column's `type` is one of the four canonical names (exact case).
- At most one column has `primary_key: true`.
- A present `foreign_key`'s `table` and `column` are themselves
  well-formed identifiers (their *existence* is not checked here, per
  above).

A schema file failing any of these is rejected with a descriptive error
naming the specific problem.
