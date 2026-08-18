#ifndef CSVDB_AST_H
#define CSVDB_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "schema.h"
#include "value.h"

/* ---- expressions (WHERE / ON conditions) ---- */

typedef enum {
    EXPR_LITERAL,
    EXPR_COLUMN_REF,
    EXPR_BINARY,
    EXPR_NOT
} ExprKind;

typedef enum {
    BINOP_EQ, BINOP_NE, BINOP_LT, BINOP_LE, BINOP_GT, BINOP_GE,
    BINOP_LIKE,
    BINOP_AND, BINOP_OR
} BinOpKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    union {
        Value literal;
        struct {
            char *table;  /* NULL if unqualified */
            char *column;
        } column_ref;
        struct {
            BinOpKind op;
            Expr *left;
            Expr *right;
        } binary;
        struct {
            Expr *operand;
        } not_expr;
    } as;
};

void expr_free(Expr *expr);

/* ---- shared fragments ---- */

typedef struct {
    char *table_name;
    char *alias; /* NULL if none */
} TableRef;

/* ---- CREATE TABLE / DROP TABLE ---- */

typedef struct {
    Schema *schema; /* owned; built directly from the parsed column defs */
} CreateTableStmt;

typedef struct {
    char *table_name;
} DropTableStmt;

/* ---- INSERT ---- */

typedef struct {
    char *table_name;
    char **columns;      /* explicit column list; NULL if omitted (all columns, schema order) */
    size_t column_count; /* 0 if columns == NULL */
    Value **rows;         /* row_count arrays, each values_per_row Values long */
    size_t values_per_row;
    size_t row_count;
} InsertStmt;

/* ---- SELECT ---- */

typedef enum { JOIN_INNER, JOIN_LEFT } JoinType;

typedef struct {
    JoinType type;
    TableRef table;
    Expr *on;
} JoinClause;

typedef enum { SELECT_ALL, SELECT_COLUMNS } SelectListKind;

typedef struct {
    char *table;  /* qualifier; NULL if unqualified */
    char *column;
} SelectColumn;

typedef enum { ORDER_ASC, ORDER_DESC } OrderDirection;

typedef struct {
    SelectListKind list_kind;
    SelectColumn *columns; /* used when list_kind == SELECT_COLUMNS */
    size_t column_count;

    TableRef from;
    JoinClause *joins;
    size_t join_count;

    Expr *where; /* NULL if absent */

    bool has_order_by;
    char *order_by_table; /* NULL if unqualified; only meaningful if has_order_by */
    char *order_by_column;
    OrderDirection order_by_direction;

    bool has_limit;
    long long limit;
} SelectStmt;

/* ---- UPDATE ---- */

typedef struct {
    char *column;
    Value value;
} Assignment;

typedef struct {
    char *table_name;
    Assignment *assignments;
    size_t assignment_count;
    Expr *where; /* NULL if absent */
} UpdateStmt;

/* ---- DELETE ---- */

typedef struct {
    char *table_name;
    Expr *where; /* NULL if absent */
} DeleteStmt;

/* ---- top-level statement ---- */

typedef enum {
    STMT_CREATE_TABLE,
    STMT_DROP_TABLE,
    STMT_INSERT,
    STMT_SELECT,
    STMT_UPDATE,
    STMT_DELETE
} StatementKind;

typedef struct {
    StatementKind kind;
    union {
        CreateTableStmt create_table;
        DropTableStmt drop_table;
        InsertStmt insert;
        SelectStmt select;
        UpdateStmt update;
        DeleteStmt delete_stmt;
    } as;
} Statement;

void statement_free(Statement *stmt);

#endif /* CSVDB_AST_H */
