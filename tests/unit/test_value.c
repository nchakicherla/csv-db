#include "test_util.h"

#include "value.h"

static void test_make_and_to_string(void) {
    Value n = value_make_null();
    char *s = value_to_string(&n);
    TEST_CHECK_STR_EQ(s, "");
    free(s);
    value_free(&n);

    Value i = value_make_integer(-42);
    s = value_to_string(&i);
    TEST_CHECK_STR_EQ(s, "-42");
    free(s);
    value_free(&i);

    Value r = value_make_real(3.5);
    s = value_to_string(&r);
    TEST_CHECK_STR_EQ(s, "3.5");
    free(s);
    value_free(&r);

    Value t = value_make_text("hello world");
    s = value_to_string(&t);
    TEST_CHECK_STR_EQ(s, "hello world");
    free(s);
    value_free(&t);

    Value b1 = value_make_boolean(true);
    s = value_to_string(&b1);
    TEST_CHECK_STR_EQ(s, "true");
    free(s);
    value_free(&b1);

    Value b0 = value_make_boolean(false);
    s = value_to_string(&b0);
    TEST_CHECK_STR_EQ(s, "false");
    free(s);
    value_free(&b0);
}

static void test_parse_valid(void) {
    char err[128];
    Value v;

    TEST_CHECK(value_parse(VALUE_INTEGER, "123", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_INTEGER && v.as.integer == 123);
    value_free(&v);

    TEST_CHECK(value_parse(VALUE_INTEGER, "-7", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_INTEGER && v.as.integer == -7);
    value_free(&v);

    TEST_CHECK(value_parse(VALUE_REAL, "3.14", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_REAL && v.as.real > 3.13 && v.as.real < 3.15);
    value_free(&v);

    TEST_CHECK(value_parse(VALUE_TEXT, "anything goes", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_TEXT && strcmp(v.as.text, "anything goes") == 0);
    value_free(&v);

    TEST_CHECK(value_parse(VALUE_BOOLEAN, "true", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_BOOLEAN && v.as.boolean == true);
    value_free(&v);

    TEST_CHECK(value_parse(VALUE_BOOLEAN, "false", &v, err, sizeof(err)));
    TEST_CHECK(v.type == VALUE_BOOLEAN && v.as.boolean == false);
    value_free(&v);
}

static void test_parse_empty_is_always_null(void) {
    char err[128];
    Value v;
    ValueType types[] = {VALUE_INTEGER, VALUE_REAL, VALUE_TEXT, VALUE_BOOLEAN};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        TEST_CHECK(value_parse(types[i], "", &v, err, sizeof(err)));
        TEST_CHECK(v.type == VALUE_NULL);
        value_free(&v);
    }
}

static void test_parse_invalid(void) {
    char err[128];
    Value v;

    TEST_CHECK(!value_parse(VALUE_INTEGER, "abc", &v, err, sizeof(err)));
    TEST_CHECK(!value_parse(VALUE_INTEGER, "12abc", &v, err, sizeof(err)));
    TEST_CHECK(!value_parse(VALUE_REAL, "abc", &v, err, sizeof(err)));
    TEST_CHECK(!value_parse(VALUE_BOOLEAN, "yes", &v, err, sizeof(err)));
    TEST_CHECK(!value_parse(VALUE_BOOLEAN, "TRUE", &v, err, sizeof(err)));
}

static void test_compare_numeric_promotion(void) {
    char err[128];
    ValueBool r;

    Value i = value_make_integer(5);
    Value f = value_make_real(5.0);
    TEST_CHECK(value_compare(&i, VALUE_OP_EQ, &f, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);

    Value f2 = value_make_real(5.5);
    TEST_CHECK(value_compare(&i, VALUE_OP_LT, &f2, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);

    value_free(&i);
    value_free(&f);
    value_free(&f2);
}

static void test_compare_text_and_boolean(void) {
    char err[128];
    ValueBool r;

    Value a = value_make_text("apple");
    Value b = value_make_text("banana");
    TEST_CHECK(value_compare(&a, VALUE_OP_LT, &b, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);
    TEST_CHECK(value_compare(&a, VALUE_OP_EQ, &a, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);
    value_free(&a);
    value_free(&b);

    Value t1 = value_make_boolean(true);
    Value t2 = value_make_boolean(false);
    TEST_CHECK(value_compare(&t1, VALUE_OP_NE, &t2, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);
    TEST_CHECK(value_compare(&t1, VALUE_OP_GT, &t2, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_TRUE);
}

static void test_compare_null_is_always_unknown(void) {
    char err[128];
    ValueBool r;

    Value n1 = value_make_null();
    Value n2 = value_make_null();
    Value i = value_make_integer(1);

    TEST_CHECK(value_compare(&n1, VALUE_OP_EQ, &n2, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_UNKNOWN);
    TEST_CHECK(value_compare(&n1, VALUE_OP_EQ, &i, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_UNKNOWN);
    TEST_CHECK(value_compare(&i, VALUE_OP_NE, &n1, &r, err, sizeof(err)));
    TEST_CHECK(r == VALUE_UNKNOWN);

    value_free(&i);
}

static void test_compare_cross_type_error(void) {
    char err[128];
    ValueBool r;

    Value t = value_make_text("5");
    Value i = value_make_integer(5);
    TEST_CHECK(!value_compare(&t, VALUE_OP_EQ, &i, &r, err, sizeof(err)));
    TEST_CHECK(err[0] != '\0');

    Value b = value_make_boolean(true);
    TEST_CHECK(!value_compare(&t, VALUE_OP_EQ, &b, &r, err, sizeof(err)));

    value_free(&t);
    value_free(&i);
    value_free(&b);
}

static void test_copy_is_independent(void) {
    Value original = value_make_text("original");
    Value copy = value_copy(&original);
    value_free(&original);
    /* copy must still be valid after original is freed */
    TEST_CHECK(strcmp(copy.as.text, "original") == 0);
    value_free(&copy);
}

int main(void) {
    test_make_and_to_string();
    test_parse_valid();
    test_parse_empty_is_always_null();
    test_parse_invalid();
    test_compare_numeric_promotion();
    test_compare_text_and_boolean();
    test_compare_null_is_always_unknown();
    test_compare_cross_type_error();
    test_copy_is_independent();

    if (test_failures == 0) {
        printf("all value tests passed\n");
    }
    return TEST_MAIN_RETURN();
}
