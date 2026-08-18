#include "catalog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *catalog_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* Independent, minimal re-check that a table name is safe to turn into a
 * filesystem path -- defense in depth alongside schema_validate()'s
 * identifier check (see PLAN.md's Identifiers section), deliberately not
 * sharing code with it so a bug in one doesn't silently defeat both. */
static bool is_safe_table_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (name[0] >= '0' && name[0] <= '9') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static char *build_path(const char *dir, const char *name, const char *suffix) {
    size_t len = strlen(dir) + 1 + strlen(name) + strlen(suffix) + 1;
    char *path = malloc(len);
    if (path != NULL) {
        snprintf(path, len, "%s/%s%s", dir, name, suffix);
    }
    return path;
}

static char *read_whole_file(const char *path, char *errbuf, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(errbuf, errlen, "cannot open \"%s\": %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(errbuf, errlen, "cannot seek \"%s\": %s", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        snprintf(errbuf, errlen, "cannot determine size of \"%s\"", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        snprintf(errbuf, errlen, "out of memory reading \"%s\"", path);
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        snprintf(errbuf, errlen, "short read on \"%s\"", path);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static bool write_file_atomic(const char *path, const char *content,
                               char *errbuf, size_t errlen) {
    size_t path_len = strlen(path);
    char *tmp_path = malloc(path_len + 5); /* ".tmp" + NUL */
    if (tmp_path == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }
    memcpy(tmp_path, path, path_len);
    memcpy(tmp_path + path_len, ".tmp", 5);

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        snprintf(errbuf, errlen, "cannot open \"%s\": %s", tmp_path, strerror(errno));
        free(tmp_path);
        return false;
    }

    size_t len = strlen(content);
    bool ok = fwrite(content, 1, len, f) == len;
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
        snprintf(errbuf, errlen, "failed writing \"%s\": %s", tmp_path, strerror(errno));
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        snprintf(errbuf, errlen, "failed to replace \"%s\": %s", path, strerror(errno));
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    free(tmp_path);
    return true;
}

static bool catalog_append_entry(Catalog *catalog, const char *name, Schema *schema) {
    if (catalog->entry_count == catalog->entry_capacity) {
        size_t new_cap = catalog->entry_capacity == 0 ? 8 : catalog->entry_capacity * 2;
        CatalogEntry *grown = realloc(catalog->entries, new_cap * sizeof(CatalogEntry));
        if (grown == NULL) {
            return false;
        }
        catalog->entries = grown;
        catalog->entry_capacity = new_cap;
    }
    char *copy = catalog_strdup(name);
    if (copy == NULL) {
        return false;
    }
    catalog->entries[catalog->entry_count].name = copy;
    catalog->entries[catalog->entry_count].schema = schema;
    catalog->entry_count++;
    return true;
}

static int catalog_find_entry(const Catalog *catalog, const char *table_name) {
    for (size_t i = 0; i < catalog->entry_count; i++) {
        if (strcmp(catalog->entries[i].name, table_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool catalog_open(const char *dir, Catalog *out, char *errbuf, size_t errlen) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        snprintf(errbuf, errlen, "cannot open database directory \"%s\": %s", dir, strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        snprintf(errbuf, errlen, "\"%s\" is not a directory", dir);
        return false;
    }

    out->dir = catalog_strdup(dir);
    out->entries = NULL;
    out->entry_count = 0;
    out->entry_capacity = 0;
    if (out->dir == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }

    DIR *dp = opendir(dir);
    if (dp == NULL) {
        snprintf(errbuf, errlen, "cannot list \"%s\": %s", dir, strerror(errno));
        catalog_close(out);
        return false;
    }

    static const char suffix[] = ".schema.json";
    size_t suffix_len = sizeof(suffix) - 1;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        size_t name_len = strlen(ent->d_name);
        if (name_len <= suffix_len) {
            continue;
        }
        if (strcmp(ent->d_name + (name_len - suffix_len), suffix) != 0) {
            continue;
        }

        size_t table_len = name_len - suffix_len;
        char *table_name = malloc(table_len + 1);
        if (table_name == NULL) {
            closedir(dp);
            catalog_close(out);
            snprintf(errbuf, errlen, "out of memory");
            return false;
        }
        memcpy(table_name, ent->d_name, table_len);
        table_name[table_len] = '\0';

        if (!is_safe_table_name(table_name)) {
            snprintf(errbuf, errlen, "\"%s\" in \"%s\" is not a valid table name",
                     table_name, dir);
            free(table_name);
            closedir(dp);
            catalog_close(out);
            return false;
        }

        bool appended = catalog_append_entry(out, table_name, NULL);
        free(table_name);
        if (!appended) {
            closedir(dp);
            catalog_close(out);
            snprintf(errbuf, errlen, "out of memory");
            return false;
        }
    }
    closedir(dp);
    return true;
}

void catalog_close(Catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    for (size_t i = 0; i < catalog->entry_count; i++) {
        free(catalog->entries[i].name);
        schema_free(catalog->entries[i].schema);
    }
    free(catalog->entries);
    free(catalog->dir);
    catalog->dir = NULL;
    catalog->entries = NULL;
    catalog->entry_count = 0;
    catalog->entry_capacity = 0;
}

size_t catalog_table_count(const Catalog *catalog) {
    return catalog->entry_count;
}

const char *catalog_table_name_at(const Catalog *catalog, size_t index) {
    if (index >= catalog->entry_count) {
        return NULL;
    }
    return catalog->entries[index].name;
}

bool catalog_has_table(const Catalog *catalog, const char *table_name) {
    return catalog_find_entry(catalog, table_name) >= 0;
}

const Schema *catalog_get_schema(Catalog *catalog, const char *table_name,
                                  char *errbuf, size_t errlen) {
    int idx = catalog_find_entry(catalog, table_name);
    if (idx < 0) {
        snprintf(errbuf, errlen, "no such table \"%s\"", table_name);
        return NULL;
    }
    if (catalog->entries[idx].schema != NULL) {
        return catalog->entries[idx].schema;
    }

    char *path = build_path(catalog->dir, table_name, ".schema.json");
    if (path == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return NULL;
    }
    char *text = read_whole_file(path, errbuf, errlen);
    free(path);
    if (text == NULL) {
        return NULL;
    }

    Schema *schema = schema_parse_json(text, errbuf, errlen);
    free(text);
    if (schema == NULL) {
        return NULL;
    }

    catalog->entries[idx].schema = schema;
    return schema;
}

bool catalog_create_table(Catalog *catalog, const Schema *schema, char *errbuf, size_t errlen) {
    if (!is_safe_table_name(schema->name)) {
        snprintf(errbuf, errlen, "\"%s\" is not a valid table name", schema->name);
        return false;
    }
    if (!schema_validate(schema, errbuf, errlen)) {
        return false;
    }
    if (catalog_has_table(catalog, schema->name)) {
        snprintf(errbuf, errlen, "table \"%s\" already exists", schema->name);
        return false;
    }

    char *schema_path = build_path(catalog->dir, schema->name, ".schema.json");
    char *csv_path = build_path(catalog->dir, schema->name, ".csv");
    char *json = schema_to_json_string(schema);
    if (schema_path == NULL || csv_path == NULL || json == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        free(schema_path);
        free(csv_path);
        free(json);
        return false;
    }

    bool ok = write_file_atomic(schema_path, json, errbuf, errlen);

    RowSet empty = {.schema = schema, .rows = NULL, .row_count = 0, .row_capacity = 0};
    if (ok) {
        ok = storage_write(&empty, csv_path, errbuf, errlen);
        if (!ok) {
            remove(schema_path);
        }
    }

    Schema *cached = NULL;
    if (ok) {
        cached = schema_parse_json(json, errbuf, errlen);
        ok = cached != NULL; /* shouldn't fail: we just validated + serialized this ourselves */
    }

    free(json);
    free(schema_path);
    free(csv_path);

    if (!ok) {
        return false;
    }

    if (!catalog_append_entry(catalog, schema->name, cached)) {
        schema_free(cached);
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }

    return true;
}

bool catalog_drop_table(Catalog *catalog, const char *table_name, char *errbuf, size_t errlen) {
    if (!is_safe_table_name(table_name)) {
        snprintf(errbuf, errlen, "\"%s\" is not a valid table name", table_name);
        return false;
    }
    int idx = catalog_find_entry(catalog, table_name);
    if (idx < 0) {
        snprintf(errbuf, errlen, "no such table \"%s\"", table_name);
        return false;
    }

    char *csv_path = build_path(catalog->dir, table_name, ".csv");
    char *schema_path = build_path(catalog->dir, table_name, ".schema.json");
    char *lock_path = build_path(catalog->dir, table_name, ".csv.lock");

    bool ok = true;
    if (remove(csv_path) != 0 && errno != ENOENT) {
        snprintf(errbuf, errlen, "failed to remove \"%s\": %s", csv_path, strerror(errno));
        ok = false;
    }
    if (ok && remove(schema_path) != 0 && errno != ENOENT) {
        snprintf(errbuf, errlen, "failed to remove \"%s\": %s", schema_path, strerror(errno));
        ok = false;
    }
    remove(lock_path); /* best-effort: may never have been created */

    free(csv_path);
    free(schema_path);
    free(lock_path);

    if (!ok) {
        return false;
    }

    free(catalog->entries[idx].name);
    schema_free(catalog->entries[idx].schema);
    catalog->entries[idx] = catalog->entries[catalog->entry_count - 1];
    catalog->entry_count--;

    return true;
}

bool catalog_read_table(Catalog *catalog, const char *table_name, RowSet *out,
                         char *errbuf, size_t errlen) {
    const Schema *schema = catalog_get_schema(catalog, table_name, errbuf, errlen);
    if (schema == NULL) {
        return false;
    }

    char *csv_path = build_path(catalog->dir, table_name, ".csv");
    char *lock_path = build_path(catalog->dir, table_name, ".csv.lock");
    if (csv_path == NULL || lock_path == NULL) {
        free(csv_path);
        free(lock_path);
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }

    TableLock lock;
    bool ok = lock_acquire(lock_path, LOCK_SHARED, &lock, errbuf, errlen);
    if (ok) {
        ok = storage_load(schema, csv_path, out, errbuf, errlen);
        lock_release(&lock);
    }

    free(csv_path);
    free(lock_path);
    return ok;
}

bool catalog_write_table(Catalog *catalog, const char *table_name, const RowSet *rows,
                          char *errbuf, size_t errlen) {
    if (!catalog_has_table(catalog, table_name)) {
        snprintf(errbuf, errlen, "no such table \"%s\"", table_name);
        return false;
    }

    char *csv_path = build_path(catalog->dir, table_name, ".csv");
    char *lock_path = build_path(catalog->dir, table_name, ".csv.lock");
    if (csv_path == NULL || lock_path == NULL) {
        free(csv_path);
        free(lock_path);
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }

    TableLock lock;
    bool ok = lock_acquire(lock_path, LOCK_EXCLUSIVE, &lock, errbuf, errlen);
    if (ok) {
        ok = storage_write(rows, csv_path, errbuf, errlen);
        lock_release(&lock);
    }

    free(csv_path);
    free(lock_path);
    return ok;
}

static int cmp_str_ptr(const void *a, const void *b) {
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

bool catalog_lock_tables(Catalog *catalog, const char **table_names, size_t count,
                          LockMode mode, TableLock *out_locks, size_t *out_locked_count,
                          char *errbuf, size_t errlen) {
    *out_locked_count = 0;
    if (count == 0) {
        return true;
    }

    const char **sorted = malloc(count * sizeof(char *));
    if (sorted == NULL) {
        snprintf(errbuf, errlen, "out of memory");
        return false;
    }
    memcpy(sorted, table_names, count * sizeof(char *));
    qsort(sorted, count, sizeof(char *), cmp_str_ptr);

    size_t locked = 0;
    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && strcmp(sorted[i], sorted[i - 1]) == 0) {
            continue; /* duplicate table (e.g. a self-join): lock it once */
        }
        if (!catalog_has_table(catalog, sorted[i])) {
            snprintf(errbuf, errlen, "no such table \"%s\"", sorted[i]);
            ok = false;
            break;
        }
        char *lock_path = build_path(catalog->dir, sorted[i], ".csv.lock");
        if (lock_path == NULL) {
            snprintf(errbuf, errlen, "out of memory");
            ok = false;
            break;
        }
        ok = lock_acquire(lock_path, mode, &out_locks[locked], errbuf, errlen);
        free(lock_path);
        if (!ok) {
            break;
        }
        locked++;
    }

    free(sorted);

    if (!ok) {
        catalog_unlock_tables(out_locks, locked);
        return false;
    }

    *out_locked_count = locked;
    return true;
}

void catalog_unlock_tables(TableLock *locks, size_t count) {
    for (size_t i = 0; i < count; i++) {
        lock_release(&locks[i]);
    }
}
