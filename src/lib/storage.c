#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csv.h"

typedef struct {
    const Schema *schema;
    RowSet *out;

    bool in_header;
    size_t field_index;

    Value *pending_values; /* scratch buffer, schema->column_count entries */

    bool failed;
    char errbuf[256];
} ParseCtx;

static void field_cb(void *field_ptr, size_t len, void *data) {
    (void)len; /* CSV_APPEND_NULL guarantees a NUL-terminated field buffer */
    ParseCtx *ctx = data;
    if (ctx->failed) {
        return;
    }

    const char *text = field_ptr != NULL ? (const char *)field_ptr : "";

    if (ctx->in_header) {
        if (ctx->field_index >= ctx->schema->column_count ||
            strcmp(text, ctx->schema->columns[ctx->field_index].name) != 0) {
            ctx->failed = true;
            snprintf(ctx->errbuf, sizeof(ctx->errbuf),
                     "header column %zu is \"%s\", expected \"%s\"", ctx->field_index, text,
                     ctx->field_index < ctx->schema->column_count
                         ? ctx->schema->columns[ctx->field_index].name
                         : "(none)");
            return;
        }
        ctx->field_index++;
        return;
    }

    if (ctx->field_index >= ctx->schema->column_count) {
        ctx->failed = true;
        snprintf(ctx->errbuf, sizeof(ctx->errbuf),
                 "row has more than %zu field(s)", ctx->schema->column_count);
        return;
    }

    Value v;
    char parse_err[128];
    ValueType type = ctx->schema->columns[ctx->field_index].type;
    if (!value_parse(type, text, &v, parse_err, sizeof(parse_err))) {
        ctx->failed = true;
        snprintf(ctx->errbuf, sizeof(ctx->errbuf), "column \"%s\": %s",
                 ctx->schema->columns[ctx->field_index].name, parse_err);
        return;
    }

    ctx->pending_values[ctx->field_index] = v;
    ctx->field_index++;
}

static void row_cb(int c, void *data) {
    (void)c;
    ParseCtx *ctx = data;
    if (ctx->failed) {
        return;
    }

    if (ctx->in_header) {
        if (ctx->field_index != ctx->schema->column_count) {
            ctx->failed = true;
            snprintf(ctx->errbuf, sizeof(ctx->errbuf),
                     "header has %zu field(s), schema \"%s\" has %zu column(s)",
                     ctx->field_index, ctx->schema->name, ctx->schema->column_count);
            return;
        }
        ctx->in_header = false;
        ctx->field_index = 0;
        return;
    }

    if (ctx->field_index != ctx->schema->column_count) {
        ctx->failed = true;
        snprintf(ctx->errbuf, sizeof(ctx->errbuf),
                 "row has %zu field(s), schema \"%s\" has %zu column(s)",
                 ctx->field_index, ctx->schema->name, ctx->schema->column_count);
        return;
    }

    RowSet *rs = ctx->out;
    if (rs->row_count == rs->row_capacity) {
        size_t new_cap = rs->row_capacity == 0 ? 8 : rs->row_capacity * 2;
        Row *grown = realloc(rs->rows, new_cap * sizeof(Row));
        if (grown == NULL) {
            ctx->failed = true;
            snprintf(ctx->errbuf, sizeof(ctx->errbuf), "out of memory growing row set");
            return;
        }
        rs->rows = grown;
        rs->row_capacity = new_cap;
    }

    size_t n = ctx->schema->column_count;
    Value *copy = malloc(n * sizeof(Value));
    if (copy == NULL) {
        ctx->failed = true;
        snprintf(ctx->errbuf, sizeof(ctx->errbuf), "out of memory allocating row");
        return;
    }
    memcpy(copy, ctx->pending_values, n * sizeof(Value));
    rs->rows[rs->row_count].values = copy;
    rs->row_count++;

    ctx->field_index = 0;
}

bool storage_load(const Schema *schema, const char *csv_path, RowSet *out,
                   char *errbuf, size_t errlen) {
    out->schema = schema;
    out->rows = NULL;
    out->row_count = 0;
    out->row_capacity = 0;

    FILE *f = fopen(csv_path, "rb");
    if (f == NULL) {
        snprintf(errbuf, errlen, "cannot open \"%s\": %s", csv_path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(errbuf, errlen, "cannot seek \"%s\": %s", csv_path, strerror(errno));
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        snprintf(errbuf, errlen, "cannot determine size of \"%s\"", csv_path);
        fclose(f);
        return false;
    }
    rewind(f);

    unsigned char *buf = malloc((size_t)size > 0 ? (size_t)size : 1);
    if (buf == NULL) {
        snprintf(errbuf, errlen, "out of memory reading \"%s\"", csv_path);
        fclose(f);
        return false;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        snprintf(errbuf, errlen, "short read on \"%s\"", csv_path);
        return false;
    }

    struct csv_parser parser;
    if (csv_init(&parser, CSV_APPEND_NULL) != 0) {
        free(buf);
        snprintf(errbuf, errlen, "failed to initialize CSV parser");
        return false;
    }

    ParseCtx ctx;
    ctx.schema = schema;
    ctx.out = out;
    ctx.in_header = true;
    ctx.field_index = 0;
    ctx.failed = false;
    ctx.errbuf[0] = '\0';
    ctx.pending_values = malloc(schema->column_count * sizeof(Value));
    if (ctx.pending_values == NULL) {
        csv_free(&parser);
        free(buf);
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }

    size_t consumed = csv_parse(&parser, buf, nread, field_cb, row_cb, &ctx);
    if (consumed != nread && !ctx.failed) {
        ctx.failed = true;
        snprintf(ctx.errbuf, sizeof(ctx.errbuf), "malformed CSV in \"%s\": %s", csv_path,
                 csv_strerror(csv_error(&parser)));
    }
    if (!ctx.failed) {
        csv_fini(&parser, field_cb, row_cb, &ctx);
    }
    csv_free(&parser);
    free(buf);

    bool ok = !ctx.failed;
    if (ok && ctx.in_header) {
        ok = false;
        snprintf(ctx.errbuf, sizeof(ctx.errbuf), "\"%s\" has no header row", csv_path);
    }

    if (!ok) {
        if (!ctx.in_header) {
            /* Free the partially-parsed row that never got committed. */
            for (size_t i = 0; i < ctx.field_index; i++) {
                value_free(&ctx.pending_values[i]);
            }
        }
        rowset_free(out);
        free(ctx.pending_values);
        snprintf(errbuf, errlen, "%s", ctx.errbuf);
        return false;
    }

    free(ctx.pending_values);
    return true;
}

static bool write_csv_field(FILE *f, const char *text, char *errbuf, size_t errlen) {
    if (csv_fwrite2(f, text, strlen(text), CSV_QUOTE) != 0) {
        snprintf(errbuf, errlen, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

bool storage_write(const RowSet *rows, const char *csv_path, char *errbuf, size_t errlen) {
    const Schema *schema = rows->schema;

    size_t path_len = strlen(csv_path);
    char *tmp_path = malloc(path_len + 5); /* ".tmp" + NUL */
    if (tmp_path == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }
    memcpy(tmp_path, csv_path, path_len);
    memcpy(tmp_path + path_len, ".tmp", 5);

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        snprintf(errbuf, errlen, "cannot open \"%s\": %s", tmp_path, strerror(errno));
        free(tmp_path);
        return false;
    }

    bool ok = true;
    char write_err[256] = {0};

    for (size_t i = 0; ok && i < schema->column_count; i++) {
        if (i > 0 && fputc(',', f) == EOF) {
            ok = false;
            break;
        }
        ok = write_csv_field(f, schema->columns[i].name, write_err, sizeof(write_err));
    }
    if (ok && fputc('\n', f) == EOF) {
        ok = false;
    }

    for (size_t r = 0; ok && r < rows->row_count; r++) {
        for (size_t i = 0; ok && i < schema->column_count; i++) {
            if (i > 0 && fputc(',', f) == EOF) {
                ok = false;
                break;
            }
            const Value *v = &rows->rows[r].values[i];
            if (v->type == VALUE_NULL) {
                continue; /* bare empty field, unquoted */
            }
            char *text = value_to_string(v);
            if (text == NULL) {
                ok = false;
                break;
            }
            ok = write_csv_field(f, text, write_err, sizeof(write_err));
            free(text);
        }
        if (ok && fputc('\n', f) == EOF) {
            ok = false;
        }
    }

    if (ok && fflush(f) != 0) {
        ok = false;
    }
    if (ok) {
        int fd = fileno(f);
        if (fd < 0 || fsync(fd) != 0) {
            ok = false;
        }
    }
    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        if (write_err[0] != '\0') {
            snprintf(errbuf, errlen, "%s", write_err);
        } else {
            snprintf(errbuf, errlen, "failed writing \"%s\": %s", tmp_path, strerror(errno));
        }
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    if (rename(tmp_path, csv_path) != 0) {
        snprintf(errbuf, errlen, "failed to replace \"%s\": %s", csv_path, strerror(errno));
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    free(tmp_path);
    return true;
}

void rowset_free(RowSet *rs) {
    if (rs == NULL || rs->rows == NULL) {
        return;
    }
    for (size_t i = 0; i < rs->row_count; i++) {
        for (size_t j = 0; j < rs->schema->column_count; j++) {
            value_free(&rs->rows[i].values[j]);
        }
        free(rs->rows[i].values);
    }
    free(rs->rows);
    rs->rows = NULL;
    rs->row_count = 0;
    rs->row_capacity = 0;
}
