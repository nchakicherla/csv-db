#ifndef CSVDB_LEXER_H
#define CSVDB_LEXER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    TOK_EOF,

    TOK_INT_LITERAL,
    TOK_REAL_LITERAL,
    TOK_STRING_LITERAL,
    TOK_IDENTIFIER,

    TOK_CREATE, TOK_TABLE, TOK_DROP, TOK_INSERT, TOK_INTO, TOK_VALUES,
    TOK_SELECT, TOK_FROM, TOK_AS, TOK_INNER, TOK_LEFT, TOK_JOIN, TOK_ON,
    TOK_WHERE, TOK_ORDER, TOK_BY, TOK_ASC, TOK_DESC, TOK_LIMIT,
    TOK_UPDATE, TOK_SET, TOK_DELETE,
    TOK_AND, TOK_OR, TOK_NOT, TOK_LIKE,
    TOK_NULL, TOK_TRUE, TOK_FALSE,
    TOK_PRIMARY, TOK_KEY, TOK_REFERENCES,
    TOK_INTEGER, TOK_REAL, TOK_TEXT, TOK_BOOLEAN,

    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_DOT, TOK_SEMICOLON, TOK_STAR,
    TOK_EQ, TOK_NE, TOK_LT, TOK_LE, TOK_GT, TOK_GE
} TokenType;

typedef struct {
    TokenType type;
    int line;
    int col;
    char *text;           /* owned; set for TOK_IDENTIFIER/TOK_STRING_LITERAL, else NULL */
    long long int_value;  /* set for TOK_INT_LITERAL */
    double real_value;    /* set for TOK_REAL_LITERAL */
} Token;

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    int col;
} Lexer;

void lexer_init(Lexer *lex, const char *src);

/* Scans and returns the next token, advancing past it. On a lexical
 * error (unterminated string, unrecognized character, out-of-range
 * numeric literal), returns false and fills errbuf with a message that
 * includes the offending line/column; `out` is left token_free-safe. */
bool lexer_next(Lexer *lex, Token *out, char *errbuf, size_t errlen);

void token_free(Token *tok);

const char *token_type_name(TokenType type);

#endif /* CSVDB_LEXER_H */
