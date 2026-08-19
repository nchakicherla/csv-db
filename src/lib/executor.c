#include "executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr_eval.h"
#include "lock.h"

static char *executor_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* Validates `value` against `col`'s type/nullability, promoting
 * INTEGER -> REAL in place when needed. Caller keeps ownership either way. */
static bool coerce_and_validate(const Column *col, Value *value, char *errbuf, size_t errlen) {
    if (value->type == VALUE_NULL) {
        if (!col->nullable) {
            snprintf(errbuf, errlen, "column \"%s\" is NOT NULL", col->name);
            return false;
        }
        return true;
    }
    if (value->type == VALUE_INTEGER && col->type == VALUE_REAL) {
        double v = (double)value->as.integer;
        *value = value_make_real(v);
        return true;
    }
    if (value->type != col->type) {
        snprintf(errbuf, errlen, "column \"%s\" expects %s, got %s",
                 col->name, value_type_name(col->type), value_type_name(value->type));
        return false;
    }
    return true;
}

/* Scans `rows` for a value equal to `value` at column `col_idx`. Used for
 * both PK-uniqueness checks (found == conflict) and FK-existence checks
 * (found == a valid reference). */
static bool value_exists_in_column(const RowSet *rows, int col_idx, const Value *value,
                                    bool *out_found, char *errbuf, size_t errlen) {
    *out_found = false;
    for (size_t i = 0; i < rows->row_count; i++) {
        ValueBool eq;
        if (!value_compare(&rows->rows[i].values[col_idx], VALUE_OP_EQ, value, &eq, errbuf, errlen)) {
            return false;
        }
        if (eq == VALUE_TRUE) {
            *out_found = true;
            return true;
        }
    }
    return true;
}

/* ---- CREATE TABLE / DROP TABLE ---- */

static bool exec_create_table(Catalog *catalog, const CreateTableStmt *stmt, char *errbuf, size_t errlen) {
    return catalog_create_table(catalog, stmt->schema, errbuf, errlen);
}

static bool exec_drop_table(Catalog *catalog, const DropTableStmt *stmt, char *errbuf, size_t errlen) {
    return catalog_drop_table(catalog, stmt->table_name, errbuf, errlen);
}

/* ---- INSERT ---- */

static bool exec_insert(Catalog *catalog, const InsertStmt *stmt, size_t *out_affected,
                         char *errbuf, size_t errlen) {
    const Schema *schema = catalog_get_schema(catalog, stmt->table_name, errbuf, errlen);
    if (schema == NULL) {
        return false;
    }

    size_t values_per_row = stmt->values_per_row;
    size_t *target_index = malloc((values_per_row > 0 ? values_per_row : 1) * sizeof(size_t));

    if (stmt->columns != NULL) {
        if (stmt->column_count != values_per_row) {
            snprintf(errbuf, errlen, "INSERT has %zu column(s) but %zu value(s) per row",
                     stmt->column_count, values_per_row);
            free(target_index);
            return false;
        }
        for (size_t i = 0; i < stmt->column_count; i++) {
            int idx = schema_find_column(schema, stmt->columns[i]);
            if (idx < 0) {
                snprintf(errbuf, errlen, "unknown column \"%s\"", stmt->columns[i]);
                free(target_index);
                return false;
            }
            for (size_t j = 0; j < i; j++) {
                if (target_index[j] == (size_t)idx) {
                    snprintf(errbuf, errlen, "column \"%s\" specified more than once", stmt->columns[i]);
                    free(target_index);
                    return false;
                }
            }
            target_index[i] = (size_t)idx;
        }
    } else {
        if (values_per_row != schema->column_count) {
            snprintf(errbuf, errlen, "table \"%s\" has %zu column(s) but INSERT provides %zu value(s) per row",
                     schema->name, schema->column_count, values_per_row);
            free(target_index);
            return false;
        }
        for (size_t i = 0; i < values_per_row; i++) {
            target_index[i] = i;
        }
    }

    /* Lock the target plus every FK-referenced table, conservatively --
     * not just the ones this particular statement happens to set. */
    const char **lock_names = malloc((schema->column_count + 1) * sizeof(char *));
    size_t lock_name_count = 0;
    lock_names[lock_name_count++] = stmt->table_name;
    for (size_t i = 0; i < schema->column_count; i++) {
        if (schema->columns[i].has_foreign_key) {
            lock_names[lock_name_count++] = schema->columns[i].foreign_key.table;
        }
    }
    TableLock *locks = malloc(lock_name_count * sizeof(TableLock));
    size_t locked_count = 0;
    bool ok = catalog_lock_tables(catalog, lock_names, lock_name_count, LOCK_EXCLUSIVE, locks,
                                   &locked_count, errbuf, errlen);
    free(lock_names);
    if (!ok) {
        free(locks);
        free(target_index);
        return false;
    }

    char *csv_path = catalog_table_csv_path(catalog, stmt->table_name);
    RowSet existing;
    ok = storage_load(schema, csv_path, &existing, errbuf, errlen);
    free(csv_path);

    /* Preload each distinct FK-referenced table once, tracking which
     * ref_rowsets slot (if any) each schema column maps to. */
    size_t fk_table_count = 0;
    for (size_t i = 0; i < schema->column_count; i++) {
        if (schema->columns[i].has_foreign_key) {
            fk_table_count++;
        }
    }
    RowSet *ref_rowsets = NULL;
    int *ref_col_idx = NULL;
    int *fk_slot = malloc(schema->column_count * sizeof(int));
    for (size_t i = 0; i < schema->column_count; i++) {
        fk_slot[i] = -1;
    }

    if (ok && fk_table_count > 0) {
        ref_rowsets = calloc(fk_table_count, sizeof(RowSet));
        ref_col_idx = malloc(fk_table_count * sizeof(int));
        size_t k = 0;
        for (size_t i = 0; i < schema->column_count && ok; i++) {
            if (!schema->columns[i].has_foreign_key) {
                continue;
            }
            const Schema *ref_schema =
                catalog_get_schema(catalog, schema->columns[i].foreign_key.table, errbuf, errlen);
            if (ref_schema == NULL) {
                ok = false;
                break;
            }
            int rci = schema_find_column(ref_schema, schema->columns[i].foreign_key.column);
            if (rci < 0) {
                snprintf(errbuf, errlen, "referenced column \"%s\" not found in table \"%s\"",
                         schema->columns[i].foreign_key.column, schema->columns[i].foreign_key.table);
                ok = false;
                break;
            }
            char *ref_path = catalog_table_csv_path(catalog, schema->columns[i].foreign_key.table);
            ok = storage_load(ref_schema, ref_path, &ref_rowsets[k], errbuf, errlen);
            free(ref_path);
            if (!ok) {
                break;
            }
            ref_col_idx[k] = rci;
            fk_slot[i] = (int)k;
            k++;
        }
        fk_table_count = k;
    }

    size_t inserted = 0;
    Value **new_rows = NULL;
    size_t new_row_count = 0;

    if (ok) {
        new_rows = malloc((stmt->row_count > 0 ? stmt->row_count : 1) * sizeof(Value *));

        for (size_t r = 0; r < stmt->row_count && ok; r++) {
            Value *row = malloc(schema->column_count * sizeof(Value));
            for (size_t i = 0; i < schema->column_count; i++) {
                row[i] = value_make_null(); /* columns omitted from an explicit list default to NULL */
            }
            for (size_t i = 0; i < values_per_row; i++) {
                row[target_index[i]] = value_copy(&stmt->rows[r][i]);
            }

            for (size_t c = 0; c < schema->column_count && ok; c++) {
                const Column *col = &schema->columns[c];
                if (!coerce_and_validate(col, &row[c], errbuf, errlen)) {
                    ok = false;
                    break;
                }
                if (row[c].type == VALUE_NULL) {
                    continue;
                }
                if (col->primary_key) {
                    bool conflict = false;
                    if (!value_exists_in_column(&existing, (int)c, &row[c], &conflict, errbuf, errlen)) {
                        ok = false;
                        break;
                    }
                    for (size_t nr = 0; nr < new_row_count && !conflict; nr++) {
                        ValueBool eq;
                        if (!value_compare(&new_rows[nr][c], VALUE_OP_EQ, &row[c], &eq, errbuf, errlen)) {
                            ok = false;
                            break;
                        }
                        if (eq == VALUE_TRUE) {
                            conflict = true;
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (conflict) {
                        snprintf(errbuf, errlen, "duplicate value for primary key column \"%s\"", col->name);
                        ok = false;
                        break;
                    }
                }
                if (col->has_foreign_key) {
                    int slot = fk_slot[c];
                    bool found = false;
                    if (!value_exists_in_column(&ref_rowsets[slot], ref_col_idx[slot], &row[c], &found,
                                                 errbuf, errlen)) {
                        ok = false;
                        break;
                    }
                    if (!found) {
                        snprintf(errbuf, errlen, "value for column \"%s\" does not exist in %s.%s",
                                 col->name, col->foreign_key.table, col->foreign_key.column);
                        ok = false;
                        break;
                    }
                }
            }

            if (!ok) {
                for (size_t i = 0; i < schema->column_count; i++) {
                    value_free(&row[i]);
                }
                free(row);
                break;
            }
            new_rows[new_row_count++] = row;
        }

        if (ok) {
            Row *grown = realloc(existing.rows, (existing.row_count + new_row_count) * sizeof(Row));
            existing.rows = grown;
            for (size_t i = 0; i < new_row_count; i++) {
                existing.rows[existing.row_count + i].values = new_rows[i];
            }
            existing.row_count += new_row_count;
            existing.row_capacity = existing.row_count;

            csv_path = catalog_table_csv_path(catalog, stmt->table_name);
            ok = storage_write(&existing, csv_path, errbuf, errlen);
            free(csv_path);
            if (ok) {
                inserted = new_row_count;
            }
        } else {
            for (size_t i = 0; i < new_row_count; i++) {
                for (size_t c = 0; c < schema->column_count; c++) {
                    value_free(&new_rows[i][c]);
                }
                free(new_rows[i]);
            }
        }
        free(new_rows);
    }

    rowset_free(&existing);
    for (size_t k = 0; k < fk_table_count; k++) {
        rowset_free(&ref_rowsets[k]);
    }
    free(ref_rowsets);
    free(ref_col_idx);
    free(fk_slot);
    free(target_index);
    catalog_unlock_tables(locks, locked_count);
    free(locks);

    if (ok && out_affected != NULL) {
        *out_affected = inserted;
    }
    return ok;
}

/* ---- UPDATE ---- */

static bool exec_update(Catalog *catalog, const UpdateStmt *stmt, size_t *out_affected,
                         char *errbuf, size_t errlen) {
    const Schema *schema = catalog_get_schema(catalog, stmt->table_name, errbuf, errlen);
    if (schema == NULL) {
        return false;
    }

    int *assign_col = malloc((stmt->assignment_count > 0 ? stmt->assignment_count : 1) * sizeof(int));
    for (size_t i = 0; i < stmt->assignment_count; i++) {
        int idx = schema_find_column(schema, stmt->assignments[i].column);
        if (idx < 0) {
            snprintf(errbuf, errlen, "unknown column \"%s\"", stmt->assignments[i].column);
            free(assign_col);
            return false;
        }
        assign_col[i] = idx;
    }

    const char **lock_names = malloc((stmt->assignment_count + 1) * sizeof(char *));
    size_t lock_name_count = 0;
    lock_names[lock_name_count++] = stmt->table_name;
    for (size_t i = 0; i < stmt->assignment_count; i++) {
        const Column *col = &schema->columns[assign_col[i]];
        if (col->has_foreign_key) {
            lock_names[lock_name_count++] = col->foreign_key.table;
        }
    }
    TableLock *locks = malloc(lock_name_count * sizeof(TableLock));
    size_t locked_count = 0;
    bool ok = catalog_lock_tables(catalog, lock_names, lock_name_count, LOCK_EXCLUSIVE, locks,
                                   &locked_count, errbuf, errlen);
    free(lock_names);
    if (!ok) {
        free(locks);
        free(assign_col);
        return false;
    }

    char *csv_path = catalog_table_csv_path(catalog, stmt->table_name);
    RowSet existing;
    ok = storage_load(schema, csv_path, &existing, errbuf, errlen);
    free(csv_path);

    RowSet *ref_rowsets = NULL;
    int *ref_col_idx = NULL;
    if (ok && stmt->assignment_count > 0) {
        ref_rowsets = calloc(stmt->assignment_count, sizeof(RowSet));
        ref_col_idx = malloc(stmt->assignment_count * sizeof(int));
        for (size_t i = 0; i < stmt->assignment_count; i++) {
            ref_col_idx[i] = -1;
        }
        for (size_t i = 0; i < stmt->assignment_count && ok; i++) {
            const Column *col = &schema->columns[assign_col[i]];
            if (!col->has_foreign_key) {
                continue;
            }
            const Schema *ref_schema = catalog_get_schema(catalog, col->foreign_key.table, errbuf, errlen);
            if (ref_schema == NULL) {
                ok = false;
                break;
            }
            int rci = schema_find_column(ref_schema, col->foreign_key.column);
            if (rci < 0) {
                snprintf(errbuf, errlen, "referenced column \"%s\" not found in table \"%s\"",
                         col->foreign_key.column, col->foreign_key.table);
                ok = false;
                break;
            }
            char *ref_path = catalog_table_csv_path(catalog, col->foreign_key.table);
            ok = storage_load(ref_schema, ref_path, &ref_rowsets[i], errbuf, errlen);
            free(ref_path);
            ref_col_idx[i] = rci;
        }
    }

    size_t updated = 0;
    if (ok) {
        for (size_t r = 0; r < existing.row_count && ok; r++) {
            bool matches = true;
            if (stmt->where != NULL) {
                RowBinding binding = {.alias = stmt->table_name, .schema = schema, .row = &existing.rows[r]};
                RowContext ctx = {.bindings = &binding, .count = 1};
                ok = expr_eval_bool(stmt->where, &ctx, &matches, errbuf, errlen);
                if (!ok) {
                    break;
                }
            }
            if (!matches) {
                continue;
            }

            Value *new_values = malloc((stmt->assignment_count > 0 ? stmt->assignment_count : 1) * sizeof(Value));
            for (size_t i = 0; i < stmt->assignment_count; i++) {
                new_values[i] = value_copy(&stmt->assignments[i].value);
            }

            for (size_t i = 0; i < stmt->assignment_count && ok; i++) {
                const Column *col = &schema->columns[assign_col[i]];
                if (!coerce_and_validate(col, &new_values[i], errbuf, errlen)) {
                    ok = false;
                    break;
                }
                if (new_values[i].type == VALUE_NULL) {
                    continue;
                }
                if (col->primary_key) {
                    bool conflict = false;
                    for (size_t o = 0; o < existing.row_count && !conflict; o++) {
                        if (o == r) {
                            continue;
                        }
                        ValueBool eq;
                        if (!value_compare(&existing.rows[o].values[assign_col[i]], VALUE_OP_EQ,
                                            &new_values[i], &eq, errbuf, errlen)) {
                            ok = false;
                            break;
                        }
                        if (eq == VALUE_TRUE) {
                            conflict = true;
                        }
                    }
                    if (!ok) {
                        break;
                    }
                    if (conflict) {
                        snprintf(errbuf, errlen, "duplicate value for primary key column \"%s\"", col->name);
                        ok = false;
                        break;
                    }
                }
                if (col->has_foreign_key) {
                    bool found = false;
                    if (!value_exists_in_column(&ref_rowsets[i], ref_col_idx[i], &new_values[i], &found,
                                                 errbuf, errlen)) {
                        ok = false;
                        break;
                    }
                    if (!found) {
                        snprintf(errbuf, errlen, "value for column \"%s\" does not exist in %s.%s",
                                 col->name, col->foreign_key.table, col->foreign_key.column);
                        ok = false;
                        break;
                    }
                }
            }

            if (ok) {
                for (size_t i = 0; i < stmt->assignment_count; i++) {
                    value_free(&existing.rows[r].values[assign_col[i]]);
                    existing.rows[r].values[assign_col[i]] = new_values[i];
                }
                updated++;
            } else {
                for (size_t i = 0; i < stmt->assignment_count; i++) {
                    value_free(&new_values[i]);
                }
            }
            free(new_values);
        }
    }

    if (ok && updated > 0) {
        csv_path = catalog_table_csv_path(catalog, stmt->table_name);
        ok = storage_write(&existing, csv_path, errbuf, errlen);
        free(csv_path);
    }

    rowset_free(&existing);
    if (ref_rowsets != NULL) {
        for (size_t i = 0; i < stmt->assignment_count; i++) {
            rowset_free(&ref_rowsets[i]);
        }
    }
    free(ref_rowsets);
    free(ref_col_idx);
    free(assign_col);
    catalog_unlock_tables(locks, locked_count);
    free(locks);

    if (ok && out_affected != NULL) {
        *out_affected = updated;
    }
    return ok;
}

/* ---- DELETE ---- */

static bool exec_delete(Catalog *catalog, const DeleteStmt *stmt, size_t *out_affected,
                         char *errbuf, size_t errlen) {
    const Schema *schema = catalog_get_schema(catalog, stmt->table_name, errbuf, errlen);
    if (schema == NULL) {
        return false;
    }

    const char *lock_names[1] = {stmt->table_name};
    TableLock locks[1];
    size_t locked_count = 0;
    if (!catalog_lock_tables(catalog, lock_names, 1, LOCK_EXCLUSIVE, locks, &locked_count, errbuf, errlen)) {
        return false;
    }

    char *csv_path = catalog_table_csv_path(catalog, stmt->table_name);
    RowSet existing;
    bool ok = storage_load(schema, csv_path, &existing, errbuf, errlen);
    free(csv_path);

    size_t deleted = 0;
    if (ok) {
        size_t write_idx = 0;
        for (size_t r = 0; r < existing.row_count; r++) {
            bool matches = true;
            if (stmt->where != NULL) {
                RowBinding binding = {.alias = stmt->table_name, .schema = schema, .row = &existing.rows[r]};
                RowContext ctx = {.bindings = &binding, .count = 1};
                if (!expr_eval_bool(stmt->where, &ctx, &matches, errbuf, errlen)) {
                    ok = false;
                }
            }
            if (!ok) {
                for (size_t rest = r; rest < existing.row_count; rest++) {
                    for (size_t i = 0; i < schema->column_count; i++) {
                        value_free(&existing.rows[rest].values[i]);
                    }
                    free(existing.rows[rest].values);
                }
                existing.row_count = write_idx;
                break;
            }
            if (matches) {
                for (size_t i = 0; i < schema->column_count; i++) {
                    value_free(&existing.rows[r].values[i]);
                }
                free(existing.rows[r].values);
                deleted++;
            } else {
                existing.rows[write_idx++] = existing.rows[r];
            }
        }
        if (ok) {
            existing.row_count = write_idx;
        }
    }

    if (ok && deleted > 0) {
        csv_path = catalog_table_csv_path(catalog, stmt->table_name);
        ok = storage_write(&existing, csv_path, errbuf, errlen);
        free(csv_path);
    }

    rowset_free(&existing);
    catalog_unlock_tables(locks, locked_count);

    if (ok && out_affected != NULL) {
        *out_affected = deleted;
    }
    return ok;
}

/* ---- SELECT ---- */

typedef struct {
    const char *table_name; /* borrowed from the AST */
    const char *alias;      /* borrowed from the AST (falls back to table_name) */
    const Schema *schema;
} JoinTableInfo;

static bool resolve_column_slot(const JoinTableInfo *tables, size_t table_count,
                                 const char *qualifier, const char *column,
                                 size_t *out_table_idx, int *out_col_idx,
                                 char *errbuf, size_t errlen) {
    int match_table = -1;
    int match_col = -1;
    for (size_t i = 0; i < table_count; i++) {
        if (qualifier != NULL && strcmp(tables[i].alias, qualifier) != 0) {
            continue;
        }
        int idx = schema_find_column(tables[i].schema, column);
        if (idx < 0) {
            continue;
        }
        if (match_table >= 0) {
            snprintf(errbuf, errlen, "column reference \"%s\" is ambiguous", column);
            return false;
        }
        match_table = (int)i;
        match_col = idx;
    }
    if (match_table < 0) {
        snprintf(errbuf, errlen, "unknown column \"%s%s%s\"",
                 qualifier != NULL ? qualifier : "", qualifier != NULL ? "." : "", column);
        return false;
    }
    *out_table_idx = (size_t)match_table;
    *out_col_idx = match_col;
    return true;
}

/* Total order for ORDER BY (NULLs sort first), distinct from
 * value_compare's three-valued WHERE/ON semantics. */
static bool value_order_cmp(const Value *a, const Value *b, int *out_cmp, char *errbuf, size_t errlen) {
    if (a->type == VALUE_NULL && b->type == VALUE_NULL) {
        *out_cmp = 0;
        return true;
    }
    if (a->type == VALUE_NULL) {
        *out_cmp = -1;
        return true;
    }
    if (b->type == VALUE_NULL) {
        *out_cmp = 1;
        return true;
    }
    ValueBool eq;
    if (!value_compare(a, VALUE_OP_EQ, b, &eq, errbuf, errlen)) {
        return false;
    }
    if (eq == VALUE_TRUE) {
        *out_cmp = 0;
        return true;
    }
    ValueBool lt;
    if (!value_compare(a, VALUE_OP_LT, b, &lt, errbuf, errlen)) {
        return false;
    }
    *out_cmp = (lt == VALUE_TRUE) ? -1 : 1;
    return true;
}

static void free_tuples(RowBinding **tuples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(tuples[i]);
    }
    free(tuples);
}

static Result *build_result(const JoinTableInfo *tables, size_t table_count, const SelectStmt *sel,
                             RowBinding **tuples, size_t tuple_count, char *errbuf, size_t errlen) {
    size_t proj_count;
    size_t *proj_table;
    int *proj_col;

    if (sel->list_kind == SELECT_ALL) {
        proj_count = 0;
        for (size_t i = 0; i < table_count; i++) {
            proj_count += tables[i].schema->column_count;
        }
        proj_table = malloc((proj_count > 0 ? proj_count : 1) * sizeof(size_t));
        proj_col = malloc((proj_count > 0 ? proj_count : 1) * sizeof(int));
        size_t k = 0;
        for (size_t i = 0; i < table_count; i++) {
            for (size_t c = 0; c < tables[i].schema->column_count; c++) {
                proj_table[k] = i;
                proj_col[k] = (int)c;
                k++;
            }
        }
    } else {
        proj_count = sel->column_count;
        proj_table = malloc((proj_count > 0 ? proj_count : 1) * sizeof(size_t));
        proj_col = malloc((proj_count > 0 ? proj_count : 1) * sizeof(int));
        for (size_t i = 0; i < proj_count; i++) {
            if (!resolve_column_slot(tables, table_count, sel->columns[i].table, sel->columns[i].column,
                                      &proj_table[i], &proj_col[i], errbuf, errlen)) {
                free(proj_table);
                free(proj_col);
                return NULL;
            }
        }
    }

    Result *result = calloc(1, sizeof(Result));
    result->column_count = proj_count;
    result->columns = malloc((proj_count > 0 ? proj_count : 1) * sizeof(ResultColumn));
    for (size_t i = 0; i < proj_count; i++) {
        const Column *col = &tables[proj_table[i]].schema->columns[proj_col[i]];
        result->columns[i].name = executor_strdup(col->name);
        result->columns[i].type = col->type;
    }

    result->row_count = tuple_count;
    result->rows = tuple_count > 0 ? malloc(tuple_count * sizeof(Value *)) : NULL;
    for (size_t r = 0; r < tuple_count; r++) {
        Value *row = malloc((proj_count > 0 ? proj_count : 1) * sizeof(Value));
        for (size_t c = 0; c < proj_count; c++) {
            const Row *src_row = tuples[r][proj_table[c]].row;
            row[c] = (src_row == NULL) ? value_make_null() : value_copy(&src_row->values[proj_col[c]]);
        }
        result->rows[r] = row;
    }

    free(proj_table);
    free(proj_col);
    return result;
}

static bool exec_select(Catalog *catalog, const SelectStmt *sel, Result **out_result,
                         char *errbuf, size_t errlen) {
    size_t table_count = 1 + sel->join_count;
    JoinTableInfo *tables = malloc(table_count * sizeof(JoinTableInfo));
    tables[0].table_name = sel->from.table_name;
    tables[0].alias = sel->from.alias != NULL ? sel->from.alias : sel->from.table_name;
    tables[0].schema = NULL;
    for (size_t i = 0; i < sel->join_count; i++) {
        tables[i + 1].table_name = sel->joins[i].table.table_name;
        tables[i + 1].alias =
            sel->joins[i].table.alias != NULL ? sel->joins[i].table.alias : sel->joins[i].table.table_name;
        tables[i + 1].schema = NULL;
    }

    const char **lock_names = malloc(table_count * sizeof(char *));
    for (size_t i = 0; i < table_count; i++) {
        lock_names[i] = tables[i].table_name;
    }
    TableLock *locks = malloc(table_count * sizeof(TableLock));
    size_t locked_count = 0;
    bool ok = catalog_lock_tables(catalog, lock_names, table_count, LOCK_SHARED, locks, &locked_count,
                                   errbuf, errlen);
    free(lock_names);
    if (!ok) {
        free(locks);
        free(tables);
        return false;
    }

    RowSet *rowsets = calloc(table_count, sizeof(RowSet));
    for (size_t i = 0; i < table_count && ok; i++) {
        const Schema *schema = catalog_get_schema(catalog, tables[i].table_name, errbuf, errlen);
        if (schema == NULL) {
            ok = false;
            break;
        }
        tables[i].schema = schema;
        char *csv_path = catalog_table_csv_path(catalog, tables[i].table_name);
        ok = storage_load(schema, csv_path, &rowsets[i], errbuf, errlen);
        free(csv_path);
    }

    RowBinding **tuples = NULL;
    size_t tuple_count = 0;

    if (ok) {
        tuples = rowsets[0].row_count > 0 ? malloc(rowsets[0].row_count * sizeof(RowBinding *)) : NULL;
        for (size_t r = 0; r < rowsets[0].row_count; r++) {
            RowBinding *tuple = malloc(table_count * sizeof(RowBinding));
            tuple[0].alias = tables[0].alias;
            tuple[0].schema = tables[0].schema;
            tuple[0].row = &rowsets[0].rows[r];
            tuples[tuple_count++] = tuple;
        }
    }

    for (size_t j = 1; j < table_count && ok; j++) {
        RowBinding **new_tuples = NULL;
        size_t new_count = 0;
        size_t new_cap = 0;
        bool join_failed = false;

        for (size_t t = 0; t < tuple_count; t++) {
            bool any_match = false;

            for (size_t r = 0; r < rowsets[j].row_count && !join_failed; r++) {
                tuples[t][j].alias = tables[j].alias;
                tuples[t][j].schema = tables[j].schema;
                tuples[t][j].row = &rowsets[j].rows[r];

                RowContext ctx = {.bindings = tuples[t], .count = j + 1};
                bool matched;
                if (!expr_eval_bool(sel->joins[j - 1].on, &ctx, &matched, errbuf, errlen)) {
                    join_failed = true;
                    break;
                }
                if (matched) {
                    any_match = true;
                    RowBinding *copy = malloc(table_count * sizeof(RowBinding));
                    memcpy(copy, tuples[t], (j + 1) * sizeof(RowBinding));
                    if (new_count == new_cap) {
                        new_cap = new_cap == 0 ? 8 : new_cap * 2;
                        RowBinding **grown = realloc(new_tuples, new_cap * sizeof(RowBinding *));
                        new_tuples = grown;
                    }
                    new_tuples[new_count++] = copy;
                }
            }

            if (!join_failed && !any_match && sel->joins[j - 1].type == JOIN_LEFT) {
                RowBinding *copy = malloc(table_count * sizeof(RowBinding));
                memcpy(copy, tuples[t], j * sizeof(RowBinding));
                copy[j].alias = tables[j].alias;
                copy[j].schema = tables[j].schema;
                copy[j].row = NULL;
                if (new_count == new_cap) {
                    new_cap = new_cap == 0 ? 8 : new_cap * 2;
                    RowBinding **grown = realloc(new_tuples, new_cap * sizeof(RowBinding *));
                    new_tuples = grown;
                }
                new_tuples[new_count++] = copy;
            }

            free(tuples[t]);
        }

        free(tuples);
        tuples = new_tuples;
        tuple_count = new_count;

        if (join_failed) {
            ok = false;
        }
    }

    if (ok && sel->where != NULL) {
        size_t write_idx = 0;
        for (size_t t = 0; t < tuple_count; t++) {
            RowContext ctx = {.bindings = tuples[t], .count = table_count};
            bool keep;
            if (!expr_eval_bool(sel->where, &ctx, &keep, errbuf, errlen)) {
                for (size_t r = t; r < tuple_count; r++) {
                    free(tuples[r]);
                }
                tuple_count = write_idx;
                ok = false;
                break;
            }
            if (keep) {
                tuples[write_idx++] = tuples[t];
            } else {
                free(tuples[t]);
            }
        }
        if (ok) {
            tuple_count = write_idx;
        }
    }

    if (ok && sel->has_order_by) {
        size_t order_table_idx;
        int order_col_idx;
        if (!resolve_column_slot(tables, table_count, sel->order_by_table, sel->order_by_column,
                                  &order_table_idx, &order_col_idx, errbuf, errlen)) {
            ok = false;
        } else {
            for (size_t i = 1; i < tuple_count && ok; i++) {
                RowBinding *key = tuples[i];
                size_t j = i;
                while (j > 0) {
                    const Row *row_j1 = tuples[j - 1][order_table_idx].row;
                    const Row *row_key = key[order_table_idx].row;
                    Value tmp_a, tmp_b;
                    const Value *va;
                    const Value *vb;
                    if (row_j1 != NULL) {
                        va = &row_j1->values[order_col_idx];
                    } else {
                        tmp_a = value_make_null();
                        va = &tmp_a;
                    }
                    if (row_key != NULL) {
                        vb = &row_key->values[order_col_idx];
                    } else {
                        tmp_b = value_make_null();
                        vb = &tmp_b;
                    }
                    int cmp;
                    if (!value_order_cmp(va, vb, &cmp, errbuf, errlen)) {
                        ok = false;
                        break;
                    }
                    bool should_swap = sel->order_by_direction == ORDER_ASC ? (cmp > 0) : (cmp < 0);
                    if (!should_swap) {
                        break;
                    }
                    tuples[j] = tuples[j - 1];
                    j--;
                }
                tuples[j] = key;
            }
        }
    }

    if (ok && sel->has_limit && (size_t)sel->limit < tuple_count) {
        for (size_t i = (size_t)sel->limit; i < tuple_count; i++) {
            free(tuples[i]);
        }
        tuple_count = (size_t)sel->limit;
    }

    Result *result = NULL;
    if (ok) {
        result = build_result(tables, table_count, sel, tuples, tuple_count, errbuf, errlen);
        ok = result != NULL;
    }

    free_tuples(tuples, tuple_count);
    for (size_t i = 0; i < table_count; i++) {
        rowset_free(&rowsets[i]);
    }
    free(rowsets);
    free(tables);
    catalog_unlock_tables(locks, locked_count);
    free(locks);

    if (!ok) {
        result_free(result);
        return false;
    }
    *out_result = result;
    return true;
}

/* ---- dispatch ---- */

bool executor_exec(Catalog *catalog, const Statement *stmt, Result **out_result,
                    size_t *out_affected_rows, char *errbuf, size_t errlen) {
    if (out_result != NULL) {
        *out_result = NULL;
    }
    if (out_affected_rows != NULL) {
        *out_affected_rows = 0;
    }

    switch (stmt->kind) {
    case STMT_CREATE_TABLE:
        return exec_create_table(catalog, &stmt->as.create_table, errbuf, errlen);
    case STMT_DROP_TABLE:
        return exec_drop_table(catalog, &stmt->as.drop_table, errbuf, errlen);
    case STMT_INSERT:
        return exec_insert(catalog, &stmt->as.insert, out_affected_rows, errbuf, errlen);
    case STMT_UPDATE:
        return exec_update(catalog, &stmt->as.update, out_affected_rows, errbuf, errlen);
    case STMT_DELETE:
        return exec_delete(catalog, &stmt->as.delete_stmt, out_affected_rows, errbuf, errlen);
    case STMT_SELECT:
        return exec_select(catalog, &stmt->as.select, out_result, errbuf, errlen);
    }

    snprintf(errbuf, errlen, "unknown statement kind");
    return false;
}
