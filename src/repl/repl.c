#include "repl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linenoise.h"

#define HISTORY_MAX_LEN 1000

size_t repl_find_statement_end(const char *sql) {
    size_t i = 0;
    size_t n = strlen(sql);
    while (i < n) {
        char c = sql[i];
        if (c == '\'') {
            i++;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        if (c == ';') {
            return i + 1;
        }
        i++;
    }
    return n;
}

static bool statement_has_content(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
            return true;
        }
    }
    return false;
}

static char *history_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return NULL;
    }
    static const char suffix[] = "/.csvdb_history";
    size_t len = strlen(home) + sizeof(suffix);
    char *path = malloc(len);
    if (path != NULL) {
        snprintf(path, len, "%s%s", home, suffix);
    }
    return path;
}

static char *flatten_for_history(const char *text) {
    size_t n = strlen(text);
    char *copy = malloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        copy[j++] = (text[i] == '\n') ? ' ' : text[i];
    }
    copy[j] = '\0';
    return copy;
}

static void print_help(void) {
    printf(
        "Meta-commands:\n"
        "  .tables          list tables in this database\n"
        "  .schema <table>  show a table's column definitions\n"
        "  .help            show this help\n"
        "  .exit / .quit    exit the REPL\n"
        "\n"
        "Anything else is treated as SQL. A statement may span multiple\n"
        "lines; entry continues until a top-level ';' is seen.\n");
}

static void handle_tables(csvdb *db) {
    size_t count = csvdb_table_count(db);
    if (count == 0) {
        printf("(no tables)\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        printf("%s\n", csvdb_table_name_at(db, i));
    }
}

static void handle_schema(csvdb *db, const char *table_name) {
    if (table_name == NULL || table_name[0] == '\0') {
        fprintf(stderr, "usage: .schema <table>\n");
        return;
    }
    char *desc = csvdb_table_schema_string(db, table_name);
    if (desc == NULL) {
        fprintf(stderr, "%s\n", csvdb_errmsg(db));
        return;
    }
    fputs(desc, stdout);
    free(desc);
}

/* Returns false if the REPL should stop after this command. */
static bool handle_meta_command(csvdb *db, const char *line) {
    if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
        return false;
    }
    if (strcmp(line, ".help") == 0) {
        print_help();
        return true;
    }
    if (strcmp(line, ".tables") == 0) {
        handle_tables(db);
        return true;
    }
    if (strncmp(line, ".schema", 7) == 0) {
        const char *rest = line + 7;
        while (*rest == ' ' || *rest == '\t') {
            rest++;
        }
        handle_schema(db, rest);
        return true;
    }
    fprintf(stderr, "unknown command \"%s\" (try .help)\n", line);
    return true;
}

static void run_statement(csvdb *db, const char *sql, OutputFormat format) {
    csvdb_result *result = NULL;
    csvdb_code code = csvdb_exec(db, sql, &result, NULL);
    if (code != CSVDB_OK) {
        fprintf(stderr, "%s\n", csvdb_errmsg(db));
        return;
    }
    if (result != NULL) {
        format_write_result(stdout, result, format);
        csvdb_result_free(result);
    }
}

/* Executes every complete statement currently at the front of `buffer`,
 * left to right, shrinking `buffer`/`buffer_len` to whatever incomplete
 * trailing text (if any) is left over. An error in one statement is
 * reported but doesn't stop the REPL from processing the rest -- unlike
 * a script, an interactive session should keep going. */
static void drain_complete_statements(csvdb *db, OutputFormat format, char **buffer, size_t *buffer_len) {
    for (;;) {
        if (*buffer_len == 0) {
            return;
        }
        size_t end = repl_find_statement_end(*buffer);
        if (end == 0 || end > *buffer_len || (*buffer)[end - 1] != ';') {
            return; /* no complete statement yet */
        }

        if (statement_has_content(*buffer, end)) {
            char *stmt_text = malloc(end + 1);
            memcpy(stmt_text, *buffer, end);
            stmt_text[end] = '\0';

            char *hist = flatten_for_history(stmt_text);
            linenoiseHistoryAdd(hist);
            free(hist);

            run_statement(db, stmt_text, format);
            free(stmt_text);
        }

        /* Also consume any whitespace right after the ';' -- otherwise a
         * stray trailing newline (every submitted line gets one appended
         * below) would leave the buffer non-empty, showing a spurious
         * "...> " continuation prompt and, worse, keeping the very next
         * meta-command from being recognized (that check requires an
         * empty buffer). */
        while (end < *buffer_len &&
               ((*buffer)[end] == ' ' || (*buffer)[end] == '\t' ||
                (*buffer)[end] == '\n' || (*buffer)[end] == '\r')) {
            end++;
        }

        size_t remaining = *buffer_len - end;
        memmove(*buffer, *buffer + end, remaining);
        *buffer_len = remaining;
        (*buffer)[*buffer_len] = '\0';
    }
}

bool repl_run(csvdb *db, OutputFormat format) {
    linenoiseHistorySetMaxLen(HISTORY_MAX_LEN);
    char *hpath = history_path();
    if (hpath != NULL) {
        linenoiseHistoryLoad(hpath); /* fine if it doesn't exist yet */
    }

    char *buffer = NULL;
    size_t buffer_len = 0;

    bool running = true;
    while (running) {
        const char *prompt = (buffer_len == 0) ? "csvdb> " : "...> ";
        errno = 0;
        char *line = linenoise(prompt);
        if (line == NULL) {
            if (errno == EAGAIN) {
                /* Ctrl-C: cancel any in-progress multi-line statement */
                free(buffer);
                buffer = NULL;
                buffer_len = 0;
                continue;
            }
            break; /* Ctrl-D / EOF */
        }

        if (buffer_len == 0 && line[0] == '.') {
            running = handle_meta_command(db, line);
            linenoiseHistoryAdd(line);
            linenoiseFree(line);
            continue;
        }

        if (buffer_len == 0 && line[0] == '\0') {
            linenoiseFree(line);
            continue; /* ignore a blank line at a fresh prompt */
        }

        size_t line_len = strlen(line);
        char *grown = realloc(buffer, buffer_len + line_len + 2); /* +1 newline, +1 NUL */
        buffer = grown;
        memcpy(buffer + buffer_len, line, line_len);
        buffer_len += line_len;
        buffer[buffer_len++] = '\n';
        buffer[buffer_len] = '\0';
        linenoiseFree(line);

        drain_complete_statements(db, format, &buffer, &buffer_len);
    }

    free(buffer);

    if (hpath != NULL) {
        linenoiseHistorySave(hpath); /* best effort */
        free(hpath);
    }

    return true;
}
