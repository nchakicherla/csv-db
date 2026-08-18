#ifndef CSVDB_VALUE_H
#define CSVDB_VALUE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VALUE_NULL,
    VALUE_INTEGER,
    VALUE_REAL,
    VALUE_TEXT,
    VALUE_BOOLEAN
} ValueType;

typedef struct {
    ValueType type;
    union {
        long long integer;
        double real;
        char *text; /* owned, heap-allocated */
        bool boolean;
    } as;
} Value;

typedef enum {
    VALUE_OP_EQ,
    VALUE_OP_NE,
    VALUE_OP_LT,
    VALUE_OP_LE,
    VALUE_OP_GT,
    VALUE_OP_GE
} ValueCmpOp;

typedef enum {
    VALUE_FALSE = 0,
    VALUE_TRUE = 1,
    VALUE_UNKNOWN = 2 /* NULL was involved on either side */
} ValueBool;

const char *value_type_name(ValueType type);

Value value_make_null(void);
Value value_make_integer(long long v);
Value value_make_real(double v);
Value value_make_text(const char *v); /* copies v */
Value value_make_boolean(bool v);

Value value_copy(const Value *v);
void value_free(Value *v);

/* Parses `text` as `type`. An empty string always yields NULL regardless of
 * `type` — the on-disk NULL convention (see PLAN.md's NULL representation
 * section). Returns false and fills errbuf if `text` doesn't fit `type`. */
bool value_parse(ValueType type, const char *text, Value *out,
                  char *errbuf, size_t errlen);

/* Renders `v` to its canonical text form, the inverse of value_parse:
 * NULL -> "", BOOLEAN -> "true"/"false", INTEGER -> decimal, REAL -> a
 * round-trippable decimal, TEXT -> as-is. Caller frees the result. */
char *value_to_string(const Value *v);

/* Compares `a op b`. NULL on either side never errors: `*out` becomes
 * VALUE_UNKNOWN (three-valued SQL logic). INTEGER and REAL compare with
 * numeric promotion; TEXT and BOOLEAN only compare against their own type.
 * Any other type pairing is a runtime error (false + errbuf), not a
 * silent cast. */
bool value_compare(const Value *a, ValueCmpOp op, const Value *b,
                    ValueBool *out, char *errbuf, size_t errlen);

#endif /* CSVDB_VALUE_H */
