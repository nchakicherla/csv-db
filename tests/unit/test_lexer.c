#include "test_util.h"

#include <stdio.h>

#include "lexer.h"

#define MAX_TOKENS 64

static bool tokenize_all(const char *src, TokenType *out_types, size_t max_tokens,
                          size_t *out_count, char *errbuf, size_t errlen) {
    Lexer lex;
    lexer_init(&lex, src);
    size_t count = 0;
    for (;;) {
        Token tok;
        if (!lexer_next(&lex, &tok, errbuf, errlen)) {
            return false;
        }
        if (count < max_tokens) {
            out_types[count] = tok.type;
        }
        count++;
        bool is_eof = tok.type == TOK_EOF;
        token_free(&tok);
        if (is_eof) {
            break;
        }
    }
    *out_count = count;
    return true;
}

typedef struct {
    const char *name;
    const char *src;
    TokenType expected[MAX_TOKENS];
    size_t expected_count;
} LexCase;

static void run_lex_cases(const LexCase *cases, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char err[256] = {0};
        TokenType actual[MAX_TOKENS];
        size_t actual_count = 0;
        bool ok = tokenize_all(cases[i].src, actual, MAX_TOKENS, &actual_count, err, sizeof(err));
        TEST_CHECK(ok);
        if (!ok) {
            fprintf(stderr, "  [%s] unexpected lex error: %s\n", cases[i].name, err);
            continue;
        }
        TEST_CHECK(actual_count == cases[i].expected_count);
        if (actual_count != cases[i].expected_count) {
            fprintf(stderr, "  [%s] got %zu tokens, expected %zu\n", cases[i].name,
                    actual_count, cases[i].expected_count);
            continue;
        }
        for (size_t j = 0; j < cases[i].expected_count; j++) {
            TEST_CHECK(actual[j] == cases[i].expected[j]);
            if (actual[j] != cases[i].expected[j]) {
                fprintf(stderr, "  [%s] token %zu: got %s, expected %s\n", cases[i].name, j,
                        token_type_name(actual[j]), token_type_name(cases[i].expected[j]));
            }
        }
    }
}

static void test_keywords_and_punctuation(void) {
    LexCase cases[] = {
        {"select-star-from", "SELECT * FROM users",
         {TOK_SELECT, TOK_STAR, TOK_FROM, TOK_IDENTIFIER, TOK_EOF}, 5},
        {"case-insensitive-keywords", "select * from users",
         {TOK_SELECT, TOK_STAR, TOK_FROM, TOK_IDENTIFIER, TOK_EOF}, 5},
        {"mixed-case-keywords", "SeLeCt * FrOm users",
         {TOK_SELECT, TOK_STAR, TOK_FROM, TOK_IDENTIFIER, TOK_EOF}, 5},
        {"qualified-column", "a.b",
         {TOK_IDENTIFIER, TOK_DOT, TOK_IDENTIFIER, TOK_EOF}, 4},
        {"comparison-operators", "= != < <= > >=",
         {TOK_EQ, TOK_NE, TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_EOF}, 7},
        {"ddl-keywords",
         "CREATE TABLE DROP INSERT INTO VALUES AS INNER LEFT JOIN ON WHERE ORDER BY "
         "ASC DESC LIMIT UPDATE SET DELETE AND OR NOT LIKE NULL TRUE FALSE PRIMARY KEY "
         "REFERENCES INTEGER REAL TEXT BOOLEAN",
         {TOK_CREATE, TOK_TABLE, TOK_DROP, TOK_INSERT, TOK_INTO, TOK_VALUES, TOK_AS,
          TOK_INNER, TOK_LEFT, TOK_JOIN, TOK_ON, TOK_WHERE, TOK_ORDER, TOK_BY, TOK_ASC,
          TOK_DESC, TOK_LIMIT, TOK_UPDATE, TOK_SET, TOK_DELETE, TOK_AND, TOK_OR, TOK_NOT,
          TOK_LIKE, TOK_NULL, TOK_TRUE, TOK_FALSE, TOK_PRIMARY, TOK_KEY, TOK_REFERENCES,
          TOK_INTEGER, TOK_REAL, TOK_TEXT, TOK_BOOLEAN, TOK_EOF},
         35},
        {"numbers", "1 2.5 -3 -4.5",
         {TOK_INT_LITERAL, TOK_REAL_LITERAL, TOK_INT_LITERAL, TOK_REAL_LITERAL, TOK_EOF}, 5},
        {"dotted-chain", "..",
         {TOK_DOT, TOK_DOT, TOK_EOF}, 3},
    };
    run_lex_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static void test_number_values(void) {
    char err[256];
    Lexer lex;
    Token tok;

    lexer_init(&lex, "42");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_INT_LITERAL && tok.int_value == 42);
    token_free(&tok);

    lexer_init(&lex, "-17");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_INT_LITERAL && tok.int_value == -17);
    token_free(&tok);

    lexer_init(&lex, "3.14");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_REAL_LITERAL && tok.real_value > 3.13 && tok.real_value < 3.15);
    token_free(&tok);

    lexer_init(&lex, "-2.5");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_REAL_LITERAL && tok.real_value > -2.51 && tok.real_value < -2.49);
    token_free(&tok);
}

static void test_string_literal_escaping(void) {
    char err[256];
    Lexer lex;
    Token tok;

    lexer_init(&lex, "'hello world'");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_STRING_LITERAL);
    TEST_CHECK_STR_EQ(tok.text, "hello world");
    token_free(&tok);

    lexer_init(&lex, "'it''s a test'");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_STRING_LITERAL);
    TEST_CHECK_STR_EQ(tok.text, "it's a test");
    token_free(&tok);

    lexer_init(&lex, "''");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_STRING_LITERAL);
    TEST_CHECK_STR_EQ(tok.text, "");
    token_free(&tok);
}

static void test_identifier_preserves_case(void) {
    char err[256];
    Lexer lex;
    Token tok;

    lexer_init(&lex, "MyTable");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(tok.type == TOK_IDENTIFIER);
    TEST_CHECK_STR_EQ(tok.text, "MyTable");
    token_free(&tok);
}

static void test_lex_errors(void) {
    char err[256];
    Lexer lex;
    Token tok;

    lexer_init(&lex, "'unterminated");
    TEST_CHECK(!lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(strstr(err, "unterminated") != NULL);

    lexer_init(&lex, "@");
    err[0] = '\0';
    TEST_CHECK(!lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    lexer_init(&lex, "!x");
    err[0] = '\0';
    TEST_CHECK(!lexer_next(&lex, &tok, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    /* '/' isn't part of any token in this grammar (no arithmetic, no
     * quoted-identifier escaping), so a path-traversal-shaped table name
     * like "../evil" can't even lex, let alone parse -- extra defense in
     * depth beyond the identifier-shape check the parser itself does. */
    lexer_init(&lex, "../evil");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err))); /* first '.' */
    TEST_CHECK(tok.type == TOK_DOT);
    token_free(&tok);
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err))); /* second '.' */
    TEST_CHECK(tok.type == TOK_DOT);
    token_free(&tok);
    err[0] = '\0';
    TEST_CHECK(!lexer_next(&lex, &tok, err, sizeof(err))); /* '/' rejected */
    TEST_CHECK(strstr(err, "/") != NULL);
}

static void test_line_column_tracking(void) {
    char err[256];
    Lexer lex;
    Token tok;

    lexer_init(&lex, "SELECT\n  *\n  FROM t");
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err))); /* SELECT */
    TEST_CHECK(tok.line == 1 && tok.col == 1);
    token_free(&tok);
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err))); /* * */
    TEST_CHECK(tok.line == 2 && tok.col == 3);
    token_free(&tok);
    TEST_CHECK(lexer_next(&lex, &tok, err, sizeof(err))); /* FROM */
    TEST_CHECK(tok.line == 3 && tok.col == 3);
    token_free(&tok);
}

int main(void) {
    test_keywords_and_punctuation();
    test_number_values();
    test_string_literal_escaping();
    test_identifier_preserves_case();
    test_lex_errors();
    test_line_column_tracking();

    if (test_failures == 0) {
        printf("all lexer tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
