#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csvdb/csvdb.h"
#include "format.h"
#include "repl.h"

typedef struct {
    const char *db_dir;
    const char *command;
    const char *script_path;
    OutputFormat format;
    bool show_help;
} CliArgs;

static void print_usage(FILE *out, const char *prog) {
    fprintf(out,
            "usage: %s [-d DIR] [-c SQL | SCRIPT] [--format table|csv|json]\n"
            "       %s -h | --help\n"
            "\n"
            "  -d, --db DIR        database directory (default: current directory)\n"
            "  -c, --command SQL   run one SQL statement and exit\n"
            "  --format FORMAT     output format for SELECT results: table (default), csv, json\n"
            "  SCRIPT              a file of ';'-separated SQL statements to run and exit\n"
            "  -h, --help          show this help and exit\n"
            "\n"
            "With neither -c nor SCRIPT, starts an interactive REPL.\n",
            prog, prog);
}

static bool parse_args(int argc, char **argv, CliArgs *out, char *errbuf, size_t errlen) {
    out->db_dir = ".";
    out->command = NULL;
    out->script_path = NULL;
    out->format = FORMAT_TABLE;
    out->show_help = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            out->show_help = true;
            return true;
        }
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--db") == 0) {
            if (i + 1 >= argc) {
                snprintf(errbuf, errlen, "%s requires an argument", arg);
                return false;
            }
            out->db_dir = argv[++i];
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--command") == 0) {
            if (i + 1 >= argc) {
                snprintf(errbuf, errlen, "%s requires an argument", arg);
                return false;
            }
            out->command = argv[++i];
            continue;
        }
        if (strcmp(arg, "--format") == 0) {
            if (i + 1 >= argc) {
                snprintf(errbuf, errlen, "%s requires an argument", arg);
                return false;
            }
            const char *fmt = argv[++i];
            if (!format_from_name(fmt, &out->format)) {
                snprintf(errbuf, errlen, "unknown format \"%s\" (expected table, csv, or json)", fmt);
                return false;
            }
            continue;
        }
        if (arg[0] == '-') {
            snprintf(errbuf, errlen, "unknown option \"%s\"", arg);
            return false;
        }
        if (out->script_path != NULL) {
            snprintf(errbuf, errlen, "unexpected extra argument \"%s\"", arg);
            return false;
        }
        out->script_path = arg;
    }

    if (out->command != NULL && out->script_path != NULL) {
        snprintf(errbuf, errlen, "cannot use -c/--command together with a script file");
        return false;
    }
    return true;
}

static bool run_one_statement(csvdb *db, const char *sql, OutputFormat format) {
    csvdb_result *result = NULL;
    csvdb_code code = csvdb_exec(db, sql, &result, NULL);
    if (code != CSVDB_OK) {
        fprintf(stderr, "csvdb: %s\n", csvdb_errmsg(db));
        return false;
    }
    if (result != NULL) {
        format_write_result(stdout, result, format);
        csvdb_result_free(result);
    }
    return true;
}

static bool run_script(csvdb *db, const char *path, OutputFormat format) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "csvdb: cannot open \"%s\": %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "csvdb: cannot read \"%s\"\n", path);
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "csvdb: cannot read \"%s\"\n", path);
        fclose(f);
        return false;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';

    bool ok = true;
    int stmt_num = 0;
    size_t pos = 0;
    while (pos < n) {
        while (pos < n && (buf[pos] == ' ' || buf[pos] == '\t' || buf[pos] == '\n' || buf[pos] == '\r')) {
            pos++;
        }
        if (pos >= n) {
            break;
        }

        size_t end = pos + repl_find_statement_end(buf + pos);
        size_t stmt_len = end - pos;
        bool has_content = false;
        for (size_t k = pos; k < end; k++) {
            char c = buf[k];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
                has_content = true;
                break;
            }
        }

        if (has_content) {
            stmt_num++;
            char *stmt_text = malloc(stmt_len + 1);
            memcpy(stmt_text, buf + pos, stmt_len);
            stmt_text[stmt_len] = '\0';

            csvdb_result *result = NULL;
            csvdb_code code = csvdb_exec(db, stmt_text, &result, NULL);
            free(stmt_text);

            if (code != CSVDB_OK) {
                fprintf(stderr, "csvdb: statement %d: %s\n", stmt_num, csvdb_errmsg(db));
                ok = false;
                break;
            }
            if (result != NULL) {
                format_write_result(stdout, result, format);
                csvdb_result_free(result);
            }
        }

        pos = end;
    }

    free(buf);
    return ok;
}

int main(int argc, char **argv) {
    CliArgs args;
    char err[256];
    if (!parse_args(argc, argv, &args, err, sizeof(err))) {
        fprintf(stderr, "csvdb: %s\n", err);
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (args.show_help) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    csvdb *db = csvdb_open(args.db_dir, err, sizeof(err));
    if (db == NULL) {
        fprintf(stderr, "csvdb: %s\n", err);
        return 1;
    }

    bool ok;
    if (args.command != NULL) {
        ok = run_one_statement(db, args.command, args.format);
    } else if (args.script_path != NULL) {
        ok = run_script(db, args.script_path, args.format);
    } else {
        ok = repl_run(db, args.format);
    }

    csvdb_close(db);
    return ok ? 0 : 1;
}
