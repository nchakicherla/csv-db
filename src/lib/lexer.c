#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *word;
    TokenType type;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"CREATE", TOK_CREATE}, {"TABLE", TOK_TABLE}, {"DROP", TOK_DROP},
    {"INSERT", TOK_INSERT}, {"INTO", TOK_INTO}, {"VALUES", TOK_VALUES},
    {"SELECT", TOK_SELECT}, {"FROM", TOK_FROM}, {"AS", TOK_AS},
    {"INNER", TOK_INNER}, {"LEFT", TOK_LEFT}, {"JOIN", TOK_JOIN}, {"ON", TOK_ON},
    {"WHERE", TOK_WHERE}, {"ORDER", TOK_ORDER}, {"BY", TOK_BY},
    {"ASC", TOK_ASC}, {"DESC", TOK_DESC}, {"LIMIT", TOK_LIMIT},
    {"UPDATE", TOK_UPDATE}, {"SET", TOK_SET}, {"DELETE", TOK_DELETE},
    {"AND", TOK_AND}, {"OR", TOK_OR}, {"NOT", TOK_NOT}, {"LIKE", TOK_LIKE},
    {"NULL", TOK_NULL}, {"TRUE", TOK_TRUE}, {"FALSE", TOK_FALSE},
    {"PRIMARY", TOK_PRIMARY}, {"KEY", TOK_KEY}, {"REFERENCES", TOK_REFERENCES},
    {"INTEGER", TOK_INTEGER}, {"REAL", TOK_REAL}, {"TEXT", TOK_TEXT}, {"BOOLEAN", TOK_BOOLEAN},
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

void lexer_init(Lexer *lex, const char *src) {
    lex->src = src;
    lex->pos = 0;
    lex->len = strlen(src);
    lex->line = 1;
    lex->col = 1;
}

static void advance_char(Lexer *lex) {
    if (lex->pos >= lex->len) {
        return;
    }
    if (lex->src[lex->pos] == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    lex->pos++;
}

static void skip_whitespace(Lexer *lex) {
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_char(lex);
        } else {
            break;
        }
    }
}

static char *dup_range(const char *src, size_t start, size_t end) {
    size_t n = end - start;
    char *out = malloc(n + 1);
    memcpy(out, src + start, n);
    out[n] = '\0';
    return out;
}

static char *dup_range_upper(const char *src, size_t start, size_t end) {
    size_t n = end - start;
    char *out = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        out[i] = (char)toupper((unsigned char)src[start + i]);
    }
    out[n] = '\0';
    return out;
}

static bool lex_identifier_or_keyword(Lexer *lex, Token *out) {
    size_t start = lex->pos;
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (isalnum((unsigned char)c) || c == '_') {
            advance_char(lex);
        } else {
            break;
        }
    }
    size_t end = lex->pos;

    char *upper = dup_range_upper(lex->src, start, end);
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (strcmp(upper, KEYWORDS[i].word) == 0) {
            out->type = KEYWORDS[i].type;
            out->text = NULL;
            free(upper);
            return true;
        }
    }
    free(upper);

    out->type = TOK_IDENTIFIER;
    out->text = dup_range(lex->src, start, end);
    return true;
}

static bool lex_number(Lexer *lex, Token *out, bool negative, char *errbuf, size_t errlen) {
    size_t start = lex->pos;
    while (lex->pos < lex->len && isdigit((unsigned char)lex->src[lex->pos])) {
        advance_char(lex);
    }

    bool is_real = false;
    if (lex->pos < lex->len && lex->src[lex->pos] == '.' &&
        lex->pos + 1 < lex->len && isdigit((unsigned char)lex->src[lex->pos + 1])) {
        is_real = true;
        advance_char(lex); /* consume '.' */
        while (lex->pos < lex->len && isdigit((unsigned char)lex->src[lex->pos])) {
            advance_char(lex);
        }
    }

    size_t end = lex->pos;
    char *text = dup_range(lex->src, start, end);

    errno = 0;
    if (is_real) {
        double v = strtod(text, NULL);
        if (errno == ERANGE) {
            snprintf(errbuf, errlen, "real literal '%s%s' is out of range", negative ? "-" : "", text);
            free(text);
            return false;
        }
        out->type = TOK_REAL_LITERAL;
        out->real_value = negative ? -v : v;
    } else {
        long long v = strtoll(text, NULL, 10);
        if (errno == ERANGE) {
            snprintf(errbuf, errlen, "integer literal '%s%s' is out of range", negative ? "-" : "", text);
            free(text);
            return false;
        }
        out->type = TOK_INT_LITERAL;
        out->int_value = negative ? -v : v;
    }
    free(text);
    out->text = NULL;
    return true;
}

static bool lex_string(Lexer *lex, Token *out, char *errbuf, size_t errlen) {
    int start_line = lex->line;
    int start_col = lex->col;
    advance_char(lex); /* consume opening quote */

    size_t cap = 16;
    size_t n = 0;
    char *buf = malloc(cap);

    for (;;) {
        if (lex->pos >= lex->len) {
            free(buf);
            snprintf(errbuf, errlen, "unterminated string literal starting at line %d, column %d",
                     start_line, start_col);
            return false;
        }
        char c = lex->src[lex->pos];
        if (c == '\'') {
            if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '\'') {
                if (n + 1 >= cap) {
                    cap *= 2;
                    buf = realloc(buf, cap);
                }
                buf[n++] = '\'';
                advance_char(lex);
                advance_char(lex);
                continue;
            }
            advance_char(lex); /* consume closing quote */
            break;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[n++] = c;
        advance_char(lex);
    }

    buf[n] = '\0';
    out->type = TOK_STRING_LITERAL;
    out->text = buf;
    return true;
}

bool lexer_next(Lexer *lex, Token *out, char *errbuf, size_t errlen) {
    skip_whitespace(lex);

    out->text = NULL;
    out->line = lex->line;
    out->col = lex->col;

    if (lex->pos >= lex->len) {
        out->type = TOK_EOF;
        return true;
    }

    char c = lex->src[lex->pos];

    if (isalpha((unsigned char)c) || c == '_') {
        return lex_identifier_or_keyword(lex, out);
    }
    if (isdigit((unsigned char)c)) {
        return lex_number(lex, out, false, errbuf, errlen);
    }
    /* No arithmetic operators exist in this grammar, so a '-' immediately
     * before a digit is unambiguously the sign of a numeric literal. */
    if (c == '-' && lex->pos + 1 < lex->len && isdigit((unsigned char)lex->src[lex->pos + 1])) {
        advance_char(lex);
        return lex_number(lex, out, true, errbuf, errlen);
    }
    if (c == '\'') {
        return lex_string(lex, out, errbuf, errlen);
    }

    switch (c) {
    case '(': advance_char(lex); out->type = TOK_LPAREN; return true;
    case ')': advance_char(lex); out->type = TOK_RPAREN; return true;
    case ',': advance_char(lex); out->type = TOK_COMMA; return true;
    case '.': advance_char(lex); out->type = TOK_DOT; return true;
    case ';': advance_char(lex); out->type = TOK_SEMICOLON; return true;
    case '*': advance_char(lex); out->type = TOK_STAR; return true;
    case '=': advance_char(lex); out->type = TOK_EQ; return true;
    case '<':
        advance_char(lex);
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            advance_char(lex);
            out->type = TOK_LE;
        } else {
            out->type = TOK_LT;
        }
        return true;
    case '>':
        advance_char(lex);
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            advance_char(lex);
            out->type = TOK_GE;
        } else {
            out->type = TOK_GT;
        }
        return true;
    case '!':
        if (lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '=') {
            advance_char(lex);
            advance_char(lex);
            out->type = TOK_NE;
            return true;
        }
        snprintf(errbuf, errlen, "unexpected character '!' at line %d, column %d", lex->line, lex->col);
        return false;
    default:
        snprintf(errbuf, errlen, "unexpected character '%c' at line %d, column %d", c, lex->line, lex->col);
        return false;
    }
}

void token_free(Token *tok) {
    if (tok == NULL) {
        return;
    }
    free(tok->text);
    tok->text = NULL;
}

const char *token_type_name(TokenType type) {
    switch (type) {
    case TOK_EOF: return "end of input";
    case TOK_INT_LITERAL: return "integer literal";
    case TOK_REAL_LITERAL: return "real literal";
    case TOK_STRING_LITERAL: return "string literal";
    case TOK_IDENTIFIER: return "identifier";
    case TOK_CREATE: return "CREATE";
    case TOK_TABLE: return "TABLE";
    case TOK_DROP: return "DROP";
    case TOK_INSERT: return "INSERT";
    case TOK_INTO: return "INTO";
    case TOK_VALUES: return "VALUES";
    case TOK_SELECT: return "SELECT";
    case TOK_FROM: return "FROM";
    case TOK_AS: return "AS";
    case TOK_INNER: return "INNER";
    case TOK_LEFT: return "LEFT";
    case TOK_JOIN: return "JOIN";
    case TOK_ON: return "ON";
    case TOK_WHERE: return "WHERE";
    case TOK_ORDER: return "ORDER";
    case TOK_BY: return "BY";
    case TOK_ASC: return "ASC";
    case TOK_DESC: return "DESC";
    case TOK_LIMIT: return "LIMIT";
    case TOK_UPDATE: return "UPDATE";
    case TOK_SET: return "SET";
    case TOK_DELETE: return "DELETE";
    case TOK_AND: return "AND";
    case TOK_OR: return "OR";
    case TOK_NOT: return "NOT";
    case TOK_LIKE: return "LIKE";
    case TOK_NULL: return "NULL";
    case TOK_TRUE: return "TRUE";
    case TOK_FALSE: return "FALSE";
    case TOK_PRIMARY: return "PRIMARY";
    case TOK_KEY: return "KEY";
    case TOK_REFERENCES: return "REFERENCES";
    case TOK_INTEGER: return "INTEGER";
    case TOK_REAL: return "REAL";
    case TOK_TEXT: return "TEXT";
    case TOK_BOOLEAN: return "BOOLEAN";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_COMMA: return "','";
    case TOK_DOT: return "'.'";
    case TOK_SEMICOLON: return "';'";
    case TOK_STAR: return "'*'";
    case TOK_EQ: return "'='";
    case TOK_NE: return "'!='";
    case TOK_LT: return "'<'";
    case TOK_LE: return "'<='";
    case TOK_GT: return "'>'";
    case TOK_GE: return "'>='";
    }
    return "unknown token";
}
