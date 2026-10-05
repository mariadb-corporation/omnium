#!/bin/bash
# Created by Roel Van de Paar, MariaDB
# Line coverage of omnium's own sources (src/*.cpp), from the selftest and, when a build is named,
# from a short live run as well. The embedded generator, revgen and reducer are not omnium code and
# stay out of the numbers.
#
#   ./coverage.sh                 build if needed, run the selftest, print the report
#   ./coverage.sh --deep          the selftest plus the live checks (needs a basedir under /test)
#   ./coverage.sh --html          also write coverage/html/index.html
set -euo pipefail
cd "$(dirname "$0")"
DEEP=0; HTML=0
for a in "$@"; do
  case "$a" in
    --deep) DEEP=1 ;;
    --html) HTML=1 ;;
    *) echo "usage: ./coverage.sh [--deep] [--html]" >&2; exit 2 ;;
  esac
done
PROFDATA="$(command -v /usr/local/bin/llvm-profdata || command -v llvm-profdata || true)"
COV="$(command -v /usr/local/bin/llvm-cov || command -v llvm-cov || true)"
[ -n "$PROFDATA" ] && [ -n "$COV" ] || { echo "llvm-profdata and llvm-cov are needed (apt install llvm)" >&2; exit 2; }
SKIP_SELFTEST=1 ./build.sh coverage          # ninja is incremental; a stale binary would give stale numbers
rm -rf coverage/raw; mkdir -p coverage/raw
export LLVM_PROFILE_FILE="$PWD/coverage/raw/%p.profraw"
./omnium_cov --selftest $( [ "$DEEP" = 1 ] && echo --deep ) || true
"$PROFDATA" merge -sparse coverage/raw/*.profraw -o coverage/omnium.profdata
SRCS=(src/*.cpp)
"$COV" report ./omnium_cov -instr-profile=coverage/omnium.profdata "${SRCS[@]}" | tail -n 40
if [ "$HTML" = 1 ]; then
  "$COV" show ./omnium_cov -instr-profile=coverage/omnium.profdata "${SRCS[@]}" -format=html -output-dir=coverage/html >/dev/null
  echo "coverage/html/index.html written"
fi
