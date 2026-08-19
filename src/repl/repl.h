#ifndef CSVDB_REPL_H
#define CSVDB_REPL_H

#include <stdbool.h>
#include <stddef.h>

#include "csvdb/csvdb.h"
#include "format.h"

/* Runs the interactive REPL against `db` until the user exits (.exit,
 * .quit, or EOF/Ctrl-D). Ctrl-C cancels any in-progress multi-line
 * statement rather than exiting (linenoise reports it as a normal NULL
 * return with errno == EAGAIN, since it intercepts the keystroke itself
 * while the terminal is in raw mode -- no OS-level signal handler is
 * needed here). Returns true once the REPL has exited normally. */
bool repl_run(csvdb *db, OutputFormat format);

/* Finds the end of the next SQL statement in `sql`: the index just past
 * its terminating top-level ';', or strlen(sql) if there's no such ';'.
 * Skips over '...' string literals (with '' escaping) so a semicolon
 * inside one doesn't split a statement early. Shared with cli/main.c's
 * script-file runner, since both need the same "don't split inside a
 * string literal" statement-boundary logic. */
size_t repl_find_statement_end(const char *sql);

#endif /* CSVDB_REPL_H */
