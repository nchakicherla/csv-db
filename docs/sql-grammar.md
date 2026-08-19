# SQL grammar (v1)

This is the exact grammar implemented by `lexer.c`/`parser.c`, in EBNF.
Notation: `::=` defines a rule, `|` is alternation, `[x]` means `x` is
optional, `{x}` means zero or more repetitions of `x`, and literal
tokens are single-quoted (case-insensitive keywords) or named (lexical
rules at the bottom).

## Statements

```
statement      ::= ( create_table | drop_table | insert | select | update | delete ) [ ';' ]
```

`parser_parse()` accepts exactly one statement per call; anything left
over after it (other than one optional trailing `;`) is a parse error.
There is no support for running a semicolon-separated batch through a
single parse call -- the CLI's script-file runner and the REPL both
split multi-statement input into separate `parser_parse()` calls
themselves, splitting only on a `;` that isn't inside a string literal.

```
create_table   ::= 'CREATE' 'TABLE' identifier '(' column_def { ',' column_def } ')'
column_def     ::= identifier column_type { column_constraint }
column_type    ::= 'INTEGER' | 'REAL' | 'TEXT' | 'BOOLEAN'
column_constraint
               ::= 'NOT' 'NULL'
                 | 'PRIMARY' 'KEY'
                 | 'REFERENCES' identifier '(' identifier ')'

drop_table     ::= 'DROP' 'TABLE' identifier

insert         ::= 'INSERT' 'INTO' identifier [ '(' identifier { ',' identifier } ')' ]
                    'VALUES' value_tuple { ',' value_tuple }
value_tuple    ::= '(' literal { ',' literal } ')'

select         ::= 'SELECT' select_list
                    'FROM' table_ref
                    { join_clause }
                    [ 'WHERE' expr ]
                    [ 'ORDER' 'BY' qualified_column [ 'ASC' | 'DESC' ] ]
                    [ 'LIMIT' integer_literal ]
select_list    ::= '*' | qualified_column { ',' qualified_column }
table_ref      ::= identifier [ 'AS' identifier ]
join_clause    ::= ( 'INNER' | 'LEFT' ) 'JOIN' table_ref 'ON' expr
qualified_column
               ::= identifier [ '.' identifier ]

update         ::= 'UPDATE' identifier 'SET' assignment { ',' assignment } [ 'WHERE' expr ]
assignment     ::= identifier '=' literal

delete         ::= 'DELETE' 'FROM' identifier [ 'WHERE' expr ]
```

Column constraints in `column_def` may appear in any order and any
combination (the grammar above allows repeats too; repeating `NOT NULL`
or `PRIMARY KEY` is harmless, just redundant). `INSERT`'s and `UPDATE`'s
right-hand sides are always `literal`s, not general expressions -- there
is no way to reference another column or compute a value there.

## Expressions

Used in `WHERE` and `ON`, by precedence from lowest to highest:

```
expr           ::= or_expr
or_expr        ::= and_expr { 'OR' and_expr }
and_expr       ::= not_expr { 'AND' not_expr }
not_expr       ::= 'NOT' not_expr | comparison
comparison     ::= primary [ comp_op primary ]
comp_op        ::= '=' | '!=' | '<' | '<=' | '>' | '>=' | 'LIKE'
primary        ::= literal | qualified_column | '(' or_expr ')'
```

Comparisons don't chain (`a < b < c` is not a single three-way
comparison; it's not even valid, since `a < b` produces a boolean and
`< c` after that isn't a legal continuation). `NOT` binds tighter than
`AND`, which binds tighter than `OR`: `NOT a = 1 AND b = 2` parses as
`(NOT (a = 1)) AND (b = 2)`, and `a = 1 OR b = 2 AND c = 3` parses as
`(a = 1) OR ((b = 2) AND (c = 3))`.

There is no arithmetic (`+ - * /`) and no aggregate functions (`COUNT`,
`SUM`, ...) in v1 -- a clean extension point for a later version, not an
oversight.

### Comparison semantics

- `INTEGER` and `REAL` compare with implicit numeric promotion: an
  `INTEGER` operand is promoted to `REAL` when compared against a `REAL`.
- `TEXT` and `BOOLEAN` only compare against their own type.
- Comparing incompatible types (e.g. `TEXT = INTEGER`) is a runtime
  error, not a silent cast.
- Any comparison involving `NULL` evaluates to `NULL` (SQL's
  three-valued logic), which a `WHERE`/`ON` filter treats as
  not-matching -- never `TRUE`.
- `AND`/`OR` implement full three-valued logic (e.g. `FALSE AND NULL` is
  `FALSE`, not `NULL`, because `FALSE` alone already determines the
  result), not a simplified two-valued approximation.

### LIKE

`LIKE` is case-sensitive and supports the standard two wildcards: `%`
matches any sequence of zero or more characters, `_` matches exactly one
character. Both operands must be `TEXT` (or `NULL`, which always yields a
`NULL`/not-matching result). There is no `ESCAPE` clause.

## Identifiers and keywords

```
identifier     ::= ( letter | '_' ) { letter | digit | '_' }
```

Identifiers are case-sensitive (matching the filesystem, since table
names become file names) and must not be a reserved keyword -- the
lexer always tokenizes a reserved word as its keyword token, never as
`identifier`, regardless of the surrounding grammar position, so there's
no way to name a table or column e.g. `select`. Keywords themselves are
matched case-insensitively (`SELECT`, `select`, and `Select` are the same
token). The reserved words are: `CREATE TABLE DROP INSERT INTO VALUES
SELECT FROM AS INNER LEFT JOIN ON WHERE ORDER BY ASC DESC LIMIT UPDATE
SET DELETE AND OR NOT LIKE NULL TRUE FALSE PRIMARY KEY REFERENCES
INTEGER REAL TEXT BOOLEAN`.

There's no quoted-identifier escape hatch (no `"table"` or `` `table` ``
syntax) to work around this in v1.

## Literals

```
literal        ::= integer_literal | real_literal | string_literal
                  | 'TRUE' | 'FALSE' | 'NULL'

integer_literal ::= [ '-' ] digit { digit }
real_literal    ::= [ '-' ] digit { digit } '.' digit { digit }
string_literal  ::= "'" { any character except an unescaped "'" | "''" } "'"
```

A leading `-` is unambiguously part of a numeric literal's sign in this
grammar, since there's no subtraction operator it could otherwise mean.
There's no exponent notation (`1e10`) and no bare `.5`/`5.` form -- a real
literal needs at least one digit on both sides of the `.`.

String literals use `''` to represent a single embedded quote
(`'it''s'` is the four-character string `it's`); there is no backslash
escaping.

## Statement termination

`;` is not part of any individual grammar rule above -- it's a
statement-boundary marker only. The lexer never treats a `;` specially
except as the `TOK_SEMICOLON` token; splitting on it is done by
`parser_parse()` (accepting one optional trailing `;`) and, for
multi-statement input, by the CLI/REPL layer as described above.
