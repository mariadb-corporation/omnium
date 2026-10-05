# What omnium reads from the mariadb-qa checkout

`QA_DIR` names the checkout; the section before last says how it is chosen. This repo keeps no copy of
anything below: one checkout is one source of truth, so a bug list or a filter is never split in two.

## At build time

`build.sh` compiles three of the framework's own translation units into the binary, so a build needs
the checkout.

| Path | Size | What it is |
|---|---|---|
| `generatorcpp/generator.cpp` | 22 MB | the SQL generator, as `omnium --role generator` |
| `generatorcpp/pools.h` | 353 KB | the word pools it includes |
| `revgen/revgen.cpp` | 130 KB | the grammar walker, as `omnium --role revgen` |
| `reducercpp/reducer.cpp` | 340 KB | the reducer, as `omnium --role reducer` |

## At run time, shared and edited

These change while omnium runs and are shared with the framework. A copy would go stale, or worse,
split the list in two.

| Path | Size | Read by |
|---|---|---|
| `known_bugs.strings` | 717 KB | is this UniqueID already reported |
| `known_bugs.strings.SAN` | 258 KB | the same for a sanitizer UniqueID |
| `BUGS/` | 4.3 MB | the SQL of a filed bug, and where `omnium report` writes one |
| `REGEX_ERRORS_SCAN` | 1.4 KB | which error-log line counts as a problem |
| `REGEX_ERRORS_FILTER` | 16 KB | which to ignore |
| `REGEX_ERRORS_LASTLINE` | 90 B | which to ignore on the last line only |
| `ASAN.filter` | 667 B | sanitizer suppressions, also handed to every server omnium starts |
| `UBSAN.filter` | 6.7 KB | the same |
| `TSAN.filter` | 2.4 KB | the same |
| `MSAN.ignorelist` | 115 B | the same, and passed to a MSAN build |
| `filter.sql` | 1.1 KB | the SQL a trial leaves out |
| `filter.sql.info` | 1.5 KB | what that list is for, shown by `omnium builds report` |
| `REGEX_ERRORS_FILTER.info` | 33 KB | the same for the error filter |

## At run time, static

These do not change between framework releases. omnium reads them where they are; nothing here is
tuned per run.

| Path | Size | Read by |
|---|---|---|
| `new_text_string.sh` | 46 KB | the bash UniqueID chain, for `omnium parity` and for the reducer |
| `san_text_string.sh` | 30 KB | the same, sanitizer form |
| `fallback_text_string.sh` | 11 KB | the same, fallback form |
| `error_log_scan.sh` | 25 KB | the same, error-log form |
| `stack.sh` | 7.8 KB | the stack or sanitizer block of a trial |
| `testcase_prettify.sh` | 21 KB | tidying a testcase for a report |
| `extract_query.gdb` | 14 KB | the query out of a core |
| `reducercpp/stages.tbl` | 67 KB | the reducer's sed stages |
| `skills/_shared/default_assignees.md` | 5.5 KB | who a bug report is assigned to |
| `spiderpreload.sql` | 1.3 KB | the SQL the Spider area runs first |

Those four UniqueID scripts reach for three more files beside them, and for each other, so none of
them works alone:

| Path | Size | Reached by |
|---|---|---|
| `capped_error_log.sh` | 3.4 KB | a log over the size cap is read as its first and last chunk |
| `random` | 14 KB | a compiled binary, not a script: the per-run random suffix |
| `version_chk_helper.source` | 13 KB | which server version an error log came from |

## At run time, the SQL and the client

| Path | Size | Read by |
|---|---|---|
| `pquery/pquery2-md` | 1.6 MB | the client each trial runs its SQL through |
| `pquery/main-ms-ps-md.sql.tar.xz` | 35 MB | the default `INFILE`, every distribution's MTR suite as SQL |
| `pquery/mysqld_options_mariadb_<version>.txt` | 493 KB, 4 files | the random server options, newest at or below the build's series |
| `yacc/<version>_sql_yacc.yy` | 4.6 MB for the directory | the grammar revgen walks, per version |
| `yacc/<version>_lex.h` | in the same | the keyword table beside it |
| `yacc/<version>_coldefs.txt` | in the same | the column definitions revgen builds its tables from |

## The fallbacks

When a build left one of the three embedded units out, omnium runs the framework's own binary
instead: `generatorcpp/generator`, `revgen/revgen`, `reducercpp/reducer`. Those are not in omnium's
own sparse clone, so the fallbacks need a full checkout.

## What omnium owns

| Path | What it is |
|---|---|
| `filters/adv.filter` | the line filter for the generated SQL. The framework holds this as the `ADV_FILTER_LIST` value inside `pquery-run.conf`, not as a file, so there is nothing to point at |
| `omnium.san.opt` | the sanitizer options omnium passes to every server it starts. `QA_DIR` in it expands to the checkout, so the suppression files are still read where they live |

## Where the checkout comes from

`QA_DIR` set in the settings wins. Otherwise omnium uses `~/mariadb-qa` when the box has one, so a
box that runs the framework has one checkout for both. Otherwise `build.sh` and `omnium init` make
omnium's own at `<repo>/mariadb-qa` through `qa_checkout.sh`: a blobless clone, so the history comes
without file contents, with a sparse working tree that holds the files above and nothing else. That
is about 120 MB in all, against 8 GB for a full checkout with its history. It is a real checkout of
the same repository: `omnium init` pulls it, and a line added to a bug list there is committed and
pushed like any other. It holds what is committed on the default branch, so it can lag a checkout
that carries unpushed work. The omnium repo ignores the directory. `qa_checkout.sh` carries the file list,
one pattern per line in git's non-cone mode so that a single file can be named and `pquery/` is not
pulled whole; the selftest checks the list against the paths the binary reads and against what the
listed shell helpers source.

## When the checkout is not there

`build.sh` stops when no clone could be made: revgen is always compiled in, and the generator and
the reducer are too unless `NO_GENERATOR=1` or `NO_REDUCER=1` leaves them out. At run time the
filters and the bug lists read empty, which means nothing is filtered and every failure looks new, so
`omnium init` checks for the checkout and says so first.
