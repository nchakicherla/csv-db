#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *value_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

const char *value_type_name(ValueType type) {
    switch (type) {
    case VALUE_NULL: return "NULL";
    case VALUE_INTEGER: return "INTEGER";
    case VALUE_REAL: return "REAL";
    case VALUE_TEXT: return "TEXT";
    case VALUE_BOOLEAN: return "BOOLEAN";
    }
    return "UNKNOWN";
}

Value value_make_null(void) {
    Value v;
    v.type = VALUE_NULL;
    return v;
}

Value value_make_integer(long long n) {
    Value v;
    v.type = VALUE_INTEGER;
    v.as.integer = n;
    return v;
}

Value value_make_real(double n) {
    Value v;
    v.type = VALUE_REAL;
    v.as.real = n;
    return v;
}

Value value_make_text(const char *s) {
    Value v;
    v.type = VALUE_TEXT;
    v.as.text = value_strdup(s);
    return v;
}

Value value_make_boolean(bool b) {
    Value v;
    v.type = VALUE_BOOLEAN;
    v.as.boolean = b;
    return v;
}

Value value_copy(const Value *v) {
    if (v->type == VALUE_TEXT) {
        return value_make_text(v->as.text);
    }
    return *v;
}

void value_free(Value *v) {
    if (v->type == VALUE_TEXT) {
        free(v->as.text);
    }
    v->type = VALUE_NULL;
}

bool value_parse(ValueType type, const char *text, Value *out,
                  char *errbuf, size_t errlen) {
    if (text == NULL || text[0] == '\0') {
        *out = value_make_null();
        return true;
    }

    switch (type) {
    case VALUE_NULL:
        snprintf(errbuf, errlen, "cannot parse a value into type NULL");
        return false;

    case VALUE_INTEGER: {
        char *end = NULL;
        errno = 0;
        long long n = strtoll(text, &end, 10);
        if (end == text || *end != '\0') {
            snprintf(errbuf, errlen, "'%s' is not a valid INTEGER", text);
            return false;
        }
        if (errno == ERANGE) {
            snprintf(errbuf, errlen, "'%s' is out of range for INTEGER", text);
            return false;
        }
        *out = value_make_integer(n);
        return true;
    }

    case VALUE_REAL: {
        char *end = NULL;
        errno = 0;
        double n = strtod(text, &end);
        if (end == text || *end != '\0') {
            snprintf(errbuf, errlen, "'%s' is not a valid REAL", text);
            return false;
        }
        if (errno == ERANGE) {
            snprintf(errbuf, errlen, "'%s' is out of range for REAL", text);
            return false;
        }
        *out = value_make_real(n);
        return true;
    }

    case VALUE_BOOLEAN:
        if (strcmp(text, "true") == 0) {
            *out = value_make_boolean(true);
            return true;
        }
        if (strcmp(text, "false") == 0) {
            *out = value_make_boolean(false);
            return true;
        }
        snprintf(errbuf, errlen,
                 "'%s' is not a valid BOOLEAN (expected \"true\" or \"false\")", text);
        return false;

    case VALUE_TEXT:
        *out = value_make_text(text);
        return true;
    }

    snprintf(errbuf, errlen, "unknown value type");
    return false;
}

char *value_to_string(const Value *v) {
    char buf[64];
    switch (v->type) {
    case VALUE_NULL:
        return value_strdup("");
    case VALUE_INTEGER:
        snprintf(buf, sizeof(buf), "%lld", v->as.integer);
        return value_strdup(buf);
    case VALUE_REAL:
        snprintf(buf, sizeof(buf), "%.17g", v->as.real);
        return value_strdup(buf);
    case VALUE_BOOLEAN:
        return value_strdup(v->as.boolean ? "true" : "false");
    case VALUE_TEXT:
        return value_strdup(v->as.text);
    }
    return value_strdup("");
}

bool value_compare(const Value *a, ValueCmpOp op, const Value *b,
                    ValueBool *out, char *errbuf, size_t errlen) {
    if (a->type == VALUE_NULL || b->type == VALUE_NULL) {
        *out = VALUE_UNKNOWN;
        return true;
    }

    int cmp;
    bool a_numeric = a->type == VALUE_INTEGER || a->type == VALUE_REAL;
    bool b_numeric = b->type == VALUE_INTEGER || b->type == VALUE_REAL;

    if (a_numeric && b_numeric) {
        double da = a->type == VALUE_INTEGER ? (double)a->as.integer : a->as.real;
        double db = b->type == VALUE_INTEGER ? (double)b->as.integer : b->as.real;
        cmp = (da < db) ? -1 : (da > db) ? 1 : 0;
    } else if (a->type == VALUE_TEXT && b->type == VALUE_TEXT) {
        int c = strcmp(a->as.text, b->as.text);
        cmp = (c < 0) ? -1 : (c > 0) ? 1 : 0;
    } else if (a->type == VALUE_BOOLEAN && b->type == VALUE_BOOLEAN) {
        cmp = (a->as.boolean == b->as.boolean) ? 0 : (a->as.boolean ? 1 : -1);
    } else {
        snprintf(errbuf, errlen, "cannot compare %s to %s",
                 value_type_name(a->type), value_type_name(b->type));
        return false;
    }

    bool result;
    switch (op) {
    case VALUE_OP_EQ: result = cmp == 0; break;
    case VALUE_OP_NE: result = cmp != 0; break;
    case VALUE_OP_LT: result = cmp < 0; break;
    case VALUE_OP_LE: result = cmp <= 0; break;
    case VALUE_OP_GT: result = cmp > 0; break;
    case VALUE_OP_GE: result = cmp >= 0; break;
    default:
        snprintf(errbuf, errlen, "unknown comparison operator");
        return false;
    }

    *out = result ? VALUE_TRUE : VALUE_FALSE;
    return true;
}
