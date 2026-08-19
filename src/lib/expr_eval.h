#ifndef CSVDB_EXPR_EVAL_H
#define CSVDB_EXPR_EVAL_H

#include <stdbool.h>
#include <stddef.h>

#include "ast.h"
#include "schema.h"
#include "storage.h"
#include "value.h"

/* One table's binding within a row context: `row` NULL means "no
 * matching row" (a LEFT JOIN miss), and every column reads as NULL. */
typedef struct {
    const char *alias; /* table alias, or the table name if none was given */
    const Schema *schema;
    const Row *row;
} RowBinding;

typedef struct {
    const RowBinding *bindings;
    size_t count;
} RowContext;

/* Evaluates `expr` against `ctx`, yielding a Value (caller value_free's
 * it). Comparisons go through value_compare (numeric promotion,
 * cross-type errors, NULL -> VALUE_UNKNOWN collapsed to a NULL Value
 * here). AND/OR/NOT operate on this boolean-or-NULL domain per standard
 * three-valued SQL logic. Column refs are resolved against `ctx`;
 * unknown or ambiguous refs are an error. */
bool expr_eval(const Expr *expr, const RowContext *ctx, Value *out,
                char *errbuf, size_t errlen);

/* Convenience for WHERE/ON: evaluates `expr` and collapses the result to
 * a plain true/false (NULL/UNKNOWN counts as false, matching how
 * three-valued logic filters rows). */
bool expr_eval_bool(const Expr *expr, const RowContext *ctx, bool *out,
                     char *errbuf, size_t errlen);

#endif /* CSVDB_EXPR_EVAL_H */
