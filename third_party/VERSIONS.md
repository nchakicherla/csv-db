# Vendored dependency versions

Tracks the exact upstream commit each vendored library was pulled from, so updates are
traceable. Update this file whenever a vendored source is re-pulled.

## libcsv

- Upstream: https://github.com/rgamble/libcsv
- Commit: `b1d5212831842ee5869d99bc208a21837e4037d5` (2021-08-20)
- License: LGPL-2.1 (see `libcsv/COPYING`)
- Files: `libcsv.c` renamed to `csv.c` on import to match `csv.h`; no other changes.
- No upstream CMake support (autotools only) — vendored and built as a standalone
  static-lib CMake target, kept separate from `libcsvdb`'s own sources per the
  LGPL relinking note in PLAN.md.

## linenoise

- Upstream: https://github.com/antirez/linenoise
- Commit: `a473823d74b93eab2ba83480df16ed37617493f2` (2026-05-01)
- License: BSD (see `linenoise/LICENSE`)
- Files: `linenoise.c`, `linenoise.h` imported unmodified.
- No upstream build system — vendored and built as a standalone static-lib CMake
  target.
