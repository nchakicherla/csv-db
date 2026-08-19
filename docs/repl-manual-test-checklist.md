# REPL manual test checklist

The REPL (`src/repl/repl.c`) is inherently interactive -- real terminal
raw-mode key handling, Ctrl-C, and history recall aren't practically
unit-testable the same way the rest of the codebase is (its
non-interactive logic, like statement-boundary splitting, *is* covered
by automated tests -- see `tests/integration/test_hardening.c` and the
piped-stdin checks below for what can be automated). Run through this
checklist by hand after any change to `src/repl/` or `src/cli/format.c`.

Start from a scratch directory so table state doesn't carry over between
runs:

```bash
rm -rf /tmp/repl-check && mkdir /tmp/repl-check
./build/csvdb -d /tmp/repl-check
```

## Prompts and multi-line entry

- [ ] The prompt is `csvdb> ` at a fresh line.
- [ ] Typing `SELECT` and pressing Enter without a `;` switches the
      prompt to `...> ` (continuation).
- [ ] Finishing the statement with a line containing `;` executes it and
      returns to `csvdb> `.
- [ ] Pasting/typing two full statements on one line, e.g.
      `CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1);`, runs both
      in order (check with `SELECT * FROM t` afterward).

## Meta-commands

- [ ] `.help` prints the meta-command list.
- [ ] `.tables` lists tables (and prints `(no tables)` on an empty db).
- [ ] `.schema t` prints `t`'s column definitions; `.schema nonexistent`
      prints a "no such table" error to stderr, not a crash.
- [ ] `.schema` with no argument prints a usage message, not a crash.
- [ ] An unrecognized command like `.bogus` prints "unknown command" and
      the REPL keeps running.
- [ ] `.exit` and `.quit` both cleanly end the session (exit code 0).

## Ctrl-C and Ctrl-D

- [ ] Mid-statement (after the prompt has switched to `...> `), pressing
      Ctrl-C discards the partial input and returns to a fresh
      `csvdb> ` prompt -- it does not exit the REPL.
- [ ] At a fresh `csvdb> ` prompt, pressing Ctrl-C also just reprompts
      (nothing to discard, but the process must not exit or crash).
- [ ] At a fresh `csvdb> ` prompt, pressing Ctrl-D exits the REPL
      cleanly (exit code 0).
- [ ] Mid-line (cursor not at the start), Ctrl-D deletes the character
      under the cursor instead of exiting -- standard line-editing
      behavior, not REPL-specific, but worth confirming it isn't
      accidentally intercepted.

## History

- [ ] Run a few statements, `.exit`, then start a new REPL session
      against the same terminal: pressing the Up arrow recalls the
      previous session's statements, most recent first.
- [ ] A multi-line statement is recalled as a single flattened line
      (embedded newlines become spaces), not reproduced with its
      original line breaks.
- [ ] `~/.csvdb_history` exists after a session and contains one line
      per submitted statement/meta-command, growing (not truncating)
      across sessions.
- [ ] If `$HOME` is unset, the REPL still works for the duration of the
      session (history just isn't persisted) -- it must not crash.

## Output formatting (shared with the CLI, but worth re-checking live)

- [ ] `SELECT` results render as an aligned table by default, with `NULL`
      cells showing the literal text `NULL`.
- [ ] A table with zero rows still prints a header and separator line,
      not nothing.
- [ ] Errors (bad SQL, a missing table, a constraint violation) print to
      stderr with a clear message and the REPL keeps running -- one bad
      statement must not end the session.

## Signal/terminal hygiene

- [ ] After exiting the REPL by any path above (`.exit`, Ctrl-D, killing
      the terminal), the shell's own line editing (arrow keys, etc.)
      still works normally afterward -- confirms linenoise restored the
      terminal out of raw mode.
