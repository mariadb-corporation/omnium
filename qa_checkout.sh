#!/bin/bash
# Created by Roel Van de Paar, MariaDB
# Makes or refreshes omnium's own checkout of the mariadb-qa framework: a blobless clone whose sparse
# working tree holds just the files omnium reads and the three it compiles in. It is used when the
# box has no ~/mariadb-qa and QA_DIR names nothing else; build.sh and omnium init call it with the
# directory beside the binary. A ~/mariadb-qa or a QA_DIR checkout is never touched.
#
#   ./qa_checkout.sh <dir>    make the clone when <dir> is missing, else apply the list and pull
#
# exit 0 = the files are there and current; 1 = no checkout could be made; 2 = the checkout is there
# but was not refreshed (offline, or a local edit conflicts), so the files stay usable as they are
#
# The list below is the one place that says which framework files omnium needs. Each line is a git
# sparse-checkout pattern in non-cone mode, so a single file can be named and pquery/ (1.8 GB) is not
# pulled whole. The shell helpers read siblings from their own directory, and those siblings are
# listed with them. The selftest checks the list against the paths the binary reads and against
# what the listed helpers source.

set -uo pipefail
URL="https://github.com/mariadb-corporation/mariadb-qa.git"
DIR="${1:?usage: $0 <dir>}"

FILES='
# compiled into the binary by build.sh
/generatorcpp/generator.cpp
/generatorcpp/pools.h
/revgen/revgen.cpp
/reducercpp/reducer.cpp
# shared with the framework and edited while omnium runs
/known_bugs.strings
/known_bugs.strings.SAN
/BUGS/
/REGEX_ERRORS_SCAN
/REGEX_ERRORS_FILTER
/REGEX_ERRORS_FILTER.info
/REGEX_ERRORS_LASTLINE
/ASAN.filter
/UBSAN.filter
/TSAN.filter
/MSAN.ignorelist
/filter.sql
/filter.sql.info
# the shell UniqueID chain and what it sources
/new_text_string.sh
/san_text_string.sh
/fallback_text_string.sh
/error_log_scan.sh
/capped_error_log.sh
/random
# the stack trace, the tidied testcase, the query out of a core
/stack.sh
/version_chk_helper.source
/source_code_rev.sh
/testcase_prettify.sh
/extract_query.gdb
# the reducer stages, the assignee list, the Spider preload
/reducercpp/stages.tbl
/skills/_shared/default_assignees.md
/spiderpreload.sql
# the client, the default INFILE and the server option lists
/pquery/pquery2-md
/pquery/main-ms-ps-md.sql.tar.xz
/pquery/mysqld_options_mariadb_*.txt
# the grammars, keyword tables and column definitions revgen walks
/yacc/*_sql_yacc.yy
/yacc/*_lex.h
/yacc/*_coldefs.txt
'
patterns() { printf '%s\n' "$FILES" | grep -vE '^[[:space:]]*(#|$)'; }
summary() { echo "[qa_checkout.sh] $DIR at $(git -C "$DIR" log -1 --format='%h %cs'), $(git -C "$DIR" ls-files -t | grep -c '^H ') files, $(du -sh --exclude=.git "$DIR" | cut -f1)"; }

if [ ! -d "$DIR/.git" ]; then
  if [ -e "$DIR" ]; then echo "[qa_checkout.sh] $DIR exists and is not a git checkout" >&2; exit 1; fi
  echo "[qa_checkout.sh] cloning $URL into $DIR (blobless, sparse)"
  if ! git clone --filter=blob:none --no-checkout --quiet "$URL" "$DIR"; then rm -rf "$DIR"; echo "[qa_checkout.sh] git clone failed" >&2; exit 1; fi
  if ! patterns | git -C "$DIR" sparse-checkout set --no-cone --stdin \
     || ! git -C "$DIR" checkout --quiet "$(git -C "$DIR" symbolic-ref --short HEAD)"; then
    rm -rf "$DIR"; echo "[qa_checkout.sh] git checkout failed" >&2; exit 1
  fi
  summary
  exit 0
fi
# the list may have grown since the clone was made, so it is applied again before the pull
patterns | git -C "$DIR" sparse-checkout set --no-cone --stdin || exit 2
if ! OUT="$(git -C "$DIR" pull --ff-only --quiet 2>&1)"; then
  echo "[qa_checkout.sh] $DIR not refreshed: $(printf '%s\n' "$OUT" | grep -v '^$' | tail -1)" >&2; exit 2
fi
summary
