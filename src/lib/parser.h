#ifndef CSVDB_PARSER_H
#define CSVDB_PARSER_H

#include <stddef.h>

#include "ast.h"

/* Parses exactly one SQL statement from `sql`. An optional trailing ';'
 * (and surrounding whitespace) is allowed; anything else left over after
 * the statement is a parse error. Table/column identifiers that don't
 * match [A-Za-z_][A-Za-z0-9_]* are rejected by construction (the lexer
 * only ever produces TOK_IDENTIFIER for that shape). Returns NULL and
 * fills errbuf (including line/column) on any lexical or syntax error.
 * Caller owns the result (statement_free it). */
Statement *parser_parse(const char *sql, char *errbuf, size_t errlen);

#endif /* CSVDB_PARSER_H */
