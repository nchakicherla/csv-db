#include "expr_eval.h"

#include <stdio.h>
#include <string.h>

typedef enum { TRI_FALSE, TRI_TRUE, TRI_UNKNOWN } Tri;

static bool value_to_tri(const Value *v, Tri *out, char *errbuf, size_t errlen) {
    if (v->type == VALUE_NULL) {
        *out = TRI_UNKNOWN;
        return true;
    }
    if (v->type != VALUE_BOOLEAN) {
        snprintf(errbuf, errlen, "expected a boolean expression, got %s", value_type_name(v->type));
        return false;
    }
    *out = v->as.boolean ? TRI_TRUE : TRI_FALSE;
    return true;
}

static Value tri_to_value(Tri t) {
    if (t == TRI_UNKNOWN) {
        return value_make_null();
    }
    return value_make_boolean(t == TRI_TRUE);
}

static ValueCmpOp binop_to_cmp_op(BinOpKind op) {
    switch (op) {
    case BINOP_EQ: return VALUE_OP_EQ;
    case BINOP_NE: return VALUE_OP_NE;
    case BINOP_LT: return VALUE_OP_LT;
    case BINOP_LE: return VALUE_OP_LE;
    case BINOP_GT: return VALUE_OP_GT;
    case BINOP_GE: return VALUE_OP_GE;
    default: return VALUE_OP_EQ; /* unreachable: AND/OR/LIKE are handled separately */
    }
}

/* Classic recursive wildcard match: '%' consumes zero or more
 * characters, '_' consumes exactly one. Case-sensitive, matching this
 * project's case-sensitive TEXT comparisons elsewhere. */
static bool like_match(const char *text, const char *pattern) {
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '%') {
        if (like_match(text, pattern + 1)) {
            return true;
        }
        if (*text != '\0') {
            return like_match(text + 1, pattern);
        }
        return false;
    }
    if (*text == '\0') {
        return false;
    }
    if (*pattern == '_' || *pattern == *text) {
        return like_match(text + 1, pattern + 1);
    }
    return false;
}

static bool resolve_column(const RowContext *ctx, const char *table, const char *column,
                            Value *out, char *errbuf, size_t errlen) {
    const RowBinding *match = NULL;
    for (size_t i = 0; i < ctx->count; i++) {
        if (table != NULL && strcmp(ctx->bindings[i].alias, table) != 0) {
            continue;
        }
        if (schema_find_column(ctx->bindings[i].schema, column) < 0) {
            continue;
        }
        if (match != NULL) {
            snprintf(errbuf, errlen, "column reference \"%s\" is ambiguous", column);
            return false;
        }
        match = &ctx->bindings[i];
    }
    if (match == NULL) {
        snprintf(errbuf, errlen, "unknown column \"%s%s%s\"",
                 table != NULL ? table : "", table != NULL ? "." : "", column);
        return false;
    }
    if (match->row == NULL) {
        *out = value_make_null();
        return true;
    }
    int idx = schema_find_column(match->schema, column);
    *out = value_copy(&match->row->values[idx]);
    return true;
}

static bool eval_logical(const Expr *expr, const RowContext *ctx, BinOpKind op, Value *out,
                          char *errbuf, size_t errlen) {
    Value lv;
    if (!expr_eval(expr->as.binary.left, ctx, &lv, errbuf, errlen)) {
        return false;
    }
    Tri lt;
    bool ok = value_to_tri(&lv, &lt, errbuf, errlen);
    value_free(&lv);
    if (!ok) {
        return false;
    }

    /* short-circuit: TRUE OR x = TRUE; FALSE AND x = FALSE */
    if (op == BINOP_OR && lt == TRI_TRUE) {
        *out = value_make_boolean(true);
        return true;
    }
    if (op == BINOP_AND && lt == TRI_FALSE) {
        *out = value_make_boolean(false);
        return true;
    }

    Value rv;
    if (!expr_eval(expr->as.binary.right, ctx, &rv, errbuf, errlen)) {
        return false;
    }
    Tri rt;
    ok = value_to_tri(&rv, &rt, errbuf, errlen);
    value_free(&rv);
    if (!ok) {
        return false;
    }

    Tri result;
    if (op == BINOP_AND) {
        if (lt == TRI_FALSE || rt == TRI_FALSE) result = TRI_FALSE;
        else if (lt == TRI_TRUE && rt == TRI_TRUE) result = TRI_TRUE;
        else result = TRI_UNKNOWN;
    } else {
        if (lt == TRI_TRUE || rt == TRI_TRUE) result = TRI_TRUE;
        else if (lt == TRI_FALSE && rt == TRI_FALSE) result = TRI_FALSE;
        else result = TRI_UNKNOWN;
    }
    *out = tri_to_value(result);
    return true;
}

static bool eval_like(const Expr *expr, const RowContext *ctx, Value *out, char *errbuf, size_t errlen) {
    Value left, right;
    if (!expr_eval(expr->as.binary.left, ctx, &left, errbuf, errlen)) {
        return false;
    }
    if (!expr_eval(expr->as.binary.right, ctx, &right, errbuf, errlen)) {
        value_free(&left);
        return false;
    }

    bool result_is_null = (left.type == VALUE_NULL || right.type == VALUE_NULL);
    bool ok = true;
    bool matched = false;
    if (!result_is_null) {
        if (left.type != VALUE_TEXT || right.type != VALUE_TEXT) {
            snprintf(errbuf, errlen, "LIKE requires TEXT operands, got %s and %s",
                     value_type_name(left.type), value_type_name(right.type));
            ok = false;
        } else {
            matched = like_match(left.as.text, right.as.text);
        }
    }
    value_free(&left);
    value_free(&right);
    if (!ok) {
        return false;
    }
    *out = result_is_null ? value_make_null() : value_make_boolean(matched);
    return true;
}

bool expr_eval(const Expr *expr, const RowContext *ctx, Value *out, char *errbuf, size_t errlen) {
    switch (expr->kind) {
    case EXPR_LITERAL:
        *out = value_copy(&expr->as.literal);
        return true;

    case EXPR_COLUMN_REF:
        return resolve_column(ctx, expr->as.column_ref.table, expr->as.column_ref.column,
                               out, errbuf, errlen);

    case EXPR_NOT: {
        Value operand;
        if (!expr_eval(expr->as.not_expr.operand, ctx, &operand, errbuf, errlen)) {
            return false;
        }
        Tri t;
        bool ok = value_to_tri(&operand, &t, errbuf, errlen);
        value_free(&operand);
        if (!ok) {
            return false;
        }
        *out = tri_to_value(t == TRI_UNKNOWN ? TRI_UNKNOWN : (t == TRI_TRUE ? TRI_FALSE : TRI_TRUE));
        return true;
    }

    case EXPR_BINARY: {
        BinOpKind op = expr->as.binary.op;
        if (op == BINOP_AND || op == BINOP_OR) {
            return eval_logical(expr, ctx, op, out, errbuf, errlen);
        }
        if (op == BINOP_LIKE) {
            return eval_like(expr, ctx, out, errbuf, errlen);
        }
        Value left, right;
        if (!expr_eval(expr->as.binary.left, ctx, &left, errbuf, errlen)) {
            return false;
        }
        if (!expr_eval(expr->as.binary.right, ctx, &right, errbuf, errlen)) {
            value_free(&left);
            return false;
        }
        ValueBool result;
        bool ok = value_compare(&left, binop_to_cmp_op(op), &right, &result, errbuf, errlen);
        value_free(&left);
        value_free(&right);
        if (!ok) {
            return false;
        }
        *out = (result == VALUE_UNKNOWN) ? value_make_null() : value_make_boolean(result == VALUE_TRUE);
        return true;
    }
    }

    snprintf(errbuf, errlen, "unknown expression kind");
    return false;
}

bool expr_eval_bool(const Expr *expr, const RowContext *ctx, bool *out, char *errbuf, size_t errlen) {
    Value v;
    if (!expr_eval(expr, ctx, &v, errbuf, errlen)) {
        return false;
    }
    if (v.type != VALUE_BOOLEAN && v.type != VALUE_NULL) {
        value_free(&v);
        snprintf(errbuf, errlen, "WHERE/ON expression must be boolean, got %s", value_type_name(v.type));
        return false;
    }
    *out = (v.type == VALUE_BOOLEAN) && v.as.boolean;
    value_free(&v);
    return true;
}
