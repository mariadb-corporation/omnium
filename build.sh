#!/bin/bash
# Created by Roel Van de Paar, MariaDB
# Builds the one omnium binary with ninja, clang++, lld and ccache.
#
#   ./build.sh            release build -> ./omnium, then --selftest
#   ./build.sh debug      -O0 -g3       -> ./omnium_dbg
#   ./build.sh ubasan     UBSAN+ASAN    -> ./omnium_ubasan
#   ./build.sh msan       MSAN          -> ./omnium_msan   (needs /MSAN_libs)
#   ./build.sh tsan       TSAN          -> ./omnium_tsan
#   ./build.sh coverage   coverage build -> ./omnium_cov, plus ./coverage.sh for the report
#   ./build.sh clean      removes the build tree
#   under MSYS2           the Windows build, ./omnium.exe or ./omnium_dbg.exe; release, debug and clean only (docs/windows.md)
#   SKIP_SELFTEST=1       keeps a binary that was not self-tested
#   NO_REDUCER=1          leaves reducer.cpp out (omnium reduce then runs reducercpp/reducer from a full
#                         mariadb-qa checkout; the sparse clone has no binaries)
#   NO_GENERATOR=1        leaves generator.cpp out (a fast build for work on other modules; the
#                         generator source is then run through the external binary if present)
#   MARIADB_BASEDIR=...   the basedir whose client library and headers are linked (default: the
#                         newest MariaDB -opt basedir under /test)
#   QA_DIR=...            the mariadb-qa checkout (default: ~/mariadb-qa, else a sparse clone beside this script)
#
# generator.cpp (820k lines) and revgen.cpp from the mariadb-qa checkout are compiled into the binary. Their
# main() becomes omnium_generator_main / omnium_revgen_main and every other symbol of theirs is
# made local with objcopy, so nothing clashes. ccache keeps the generator compile (about 5 min) to
# a one-time cost until the file changes.

set -euo pipefail
cd "$(dirname "$0")"
MODE="${1:-release}"
# under MSYS2 this is the Windows build (docs/windows.md): the MSYS2 toolchain, its client library,
# and release, debug or clean only
WIN=0
case "$(uname -s)" in MSYS*|MINGW*|CYGWIN*) WIN=1 ;; esac
if [ "$WIN" = 1 ]; then CXX="${CXX:-$(command -v clang++ >/dev/null 2>&1 && echo clang++ || echo g++)}"
else CXX="${CXX:-clang++}"; fi
# the mariadb-qa checkout: QA_DIR when set, else ~/mariadb-qa when the box has one, else omnium's own
# sparse clone beside this script, made and refreshed by qa_checkout.sh (omnium init follows the same rule)
if [ -z "${QA_DIR:-}" ]; then
  if [ -d "$HOME/mariadb-qa" ]; then QA_DIR="$HOME/mariadb-qa"
  else
    QA_DIR="$PWD/mariadb-qa"
    rc=0; ./qa_checkout.sh "$QA_DIR" || rc=$?
    [ "$rc" -ne 1 ] || exit 1                               # 2 = there but not refreshed: build with what is there
  fi
fi
GEN_SRC="$QA_DIR/generatorcpp/generator.cpp"
REVGEN_SRC="$QA_DIR/revgen/revgen.cpp"
REDUCER_SRC="$QA_DIR/reducercpp/reducer.cpp"
CCACHE=""
command -v ccache >/dev/null && CCACHE="ccache"
if [ "$WIN" = 1 ]; then OBJCOPY="$(command -v objcopy || command -v llvm-objcopy)"   # binutils objcopy for the COFF objects
else OBJCOPY="$(command -v llvm-objcopy || command -v objcopy)"; fi
# the embedded generator, revgen and reducer keep their own symbols to themselves through this, and
# there is no build without at least revgen, so a missing objcopy stops here rather than in ninja
[ -n "$OBJCOPY" ] || { echo "[build.sh] no llvm-objcopy or objcopy on the box; install the llvm or binutils package" >&2; exit 2; }

if [ "$MODE" = clean ]; then rm -rf build; echo "[build.sh] build/ removed"; exit 0; fi
case "$WIN$MODE" in 1release|1rel|1debug|1dbg) ;; 1*) echo "[build.sh] $MODE is not available on Windows: release, debug or clean" >&2; exit 2 ;; esac

pick_mariadb_basedir() {
  if [ -n "${MARIADB_BASEDIR:-}" ]; then printf '%s\n' "$MARIADB_BASEDIR"; return; fi
  local best
  best=$(ls -d /test/MD[0-9]*-mariadb-*-opt 2>/dev/null \
    | awk -F'-mariadb-' '{ print $2, $0 }' | sort -V | tail -1 | awk '{print $2}')
  [ -n "$best" ] || { echo "[build.sh] no MariaDB -opt basedir under /test; set MARIADB_BASEDIR=" >&2; exit 2; }
  printf '%s\n' "$best"
}
if [ "$WIN" = 1 ]; then
  # MariaDB Connector/C, built for the MSYS2 runtime, which has no package for it
  BD="${MARIADB_BASEDIR:-/usr/local}"
  if [ ! -f "$BD/include/mysql/mysql.h" ] || [ ! -f "$BD/lib/libmariadbclient.a" ]; then
    ./connector_windows.sh "$BD" || exit 2
  fi
else
  BD="$(pick_mariadb_basedir)"
  [ -f "$BD/lib/libmariadbclient.a" ] || { echo "[build.sh] $BD: lib/libmariadbclient.a missing" >&2; exit 2; }
  [ -f "$BD/include/mysql/mysql.h" ] || { echo "[build.sh] $BD: include/mysql/mysql.h missing" >&2; exit 2; }
fi

COMMON="-std=c++20 -stdlib=libc++ -pthread -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -fPIC -I$BD/include -Isrc -DOMNIUM_CLIENT_BASEDIR=\"$BD\""
LINK_COMMON="-fuse-ld=lld -stdlib=libc++ -lc++abi -pthread -Wl,--build-id=sha1 -Wl,-rpath,$BD/lib"
LIBS="-L$BD/lib -lmariadbclient -lgnutls -lssl -lcrypto -lz -lzstd -lresolv -lm -ldl -lpcre2-8 -lcurl -ldw -lelf"
if [ "$WIN" = 1 ]; then
  # the default C++ library and linker; a Windows binary has no rpath and no build-id; the client
  # library is the static one, as on Linux, with the TLS and zlib libraries it needs; libdw, libelf
  # and libresolv are Linux (the runtime has dlopen itself, libdl is only a stub)
  COMMON="${COMMON/-stdlib=libc++ /}"
  COMMON="${COMMON/-fPIC /}"
  # on Linux the compilers define _GNU_SOURCE themselves; without it -std=c++20 hides usleep, kill,
  # setenv, pipe2, u_char, the pty calls and more in the MSYS2 headers
  COMMON="$COMMON -D_GNU_SOURCE"
  LINK_COMMON="-pthread"
  LIBS="-L$BD/lib -lmariadbclient -lssl -lcrypto -lz -lm -ldl -lpcre2-8 -lcurl"
  # the entry points are weak references (verbs.h), and a PE link has no undefined weak symbols, so
  # the one a NO_GENERATOR or NO_REDUCER build leaves out is defined as 0, which reads as absent
  absent() { LINK_COMMON="$LINK_COMMON -Wl,--defsym=$1=0"; }
  [ "${NO_GENERATOR:-0}" != 1 ] && [ -f "$GEN_SRC" ] || absent _Z21omnium_generator_mainiPPc
  [ -f "$REVGEN_SRC" ] || absent _Z18omnium_revgen_mainiPPc
  [ "${NO_REDUCER:-0}" != 1 ] && [ -f "$REDUCER_SRC" ] || absent _Z19omnium_reducer_mainiPPc
fi
case "$MODE" in
  release|rel) OUT=omnium;        CXXFLAGS="-O3 -march=native -mtune=native -DNDEBUG -g1 -pipe"; LDEXTRA="" ;;
  debug|dbg)   OUT=omnium_dbg;    CXXFLAGS="-O0 -g3 -fno-omit-frame-pointer -pipe"; LDEXTRA="" ;;
  coverage|cov) OUT=omnium_cov;   CXXFLAGS="-O1 -g -fprofile-instr-generate -fcoverage-mapping -pipe"; LDEXTRA="-fprofile-instr-generate" ;;
  ubasan)      OUT=omnium_ubasan; CXXFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=undefined,address -fno-sanitize-recover=undefined"; LDEXTRA="-fsanitize=undefined,address" ;;
  tsan)        OUT=omnium_tsan;   CXXFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=thread"; LDEXTRA="-fsanitize=thread" ;;
  msan)        OUT=omnium_msan;   CXXFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-recover=memory"; LDEXTRA="-fsanitize=memory"
               MSAN_LIBS="${MSAN_LIBS:-/MSAN_libs}"
               if [ -f "$MSAN_LIBS/libc++.a" ]; then
                 COMMON="${COMMON/-stdlib=libc++/-nostdinc++ -isystem $MSAN_LIBS/include/c++/v1}"
                 LINK_COMMON="-fuse-ld=lld -nostdlib++ -fsanitize=memory $MSAN_LIBS/libc++.a $MSAN_LIBS/libc++abi.a -pthread -Wl,--build-id=sha1 -Wl,-rpath,$BD/lib"
               else echo "[build.sh] msan: no instrumented libc++ in $MSAN_LIBS; expect reports in the library" >&2; fi ;;
  *) echo "usage: $0 [release|debug|coverage|ubasan|msan|tsan|clean]"; exit 2 ;;
esac
if [ "$WIN" = 1 ]; then OUT="$OUT.exe"; fi
# The generator is one 22 MB translation unit: -O2 keeps its compile in minutes, and no -march so
# one ccache entry serves every mode that shares the flags below. A sanitizer mode adds its own
# flags here as well: without them these objects are neither instrumented nor built against the
# same libc++ as the rest, which under MSAN does not even link. That costs one ccache entry per
# sanitizer mode.
GEN_CXXFLAGS="-O2 -DNDEBUG -pipe -Wno-everything"
# g++ has no -Wno-everything. The embedded sources include <sys/auxv.h>, which MSYS2 lacks, so
# src/msys2 supplies one. Their global Xoshiro256pp is a class of its own with the name of omnium's
# (common.h). On Linux the localize step below keeps the two apart; on Windows, where the objects
# are linked as compiled, theirs is renamed, or its seed_full() would be defined twice.
if [ "$WIN" = 1 ]; then
  GEN_CXXFLAGS="${GEN_CXXFLAGS/-Wno-everything/-w} -Isrc/msys2 -DXoshiro256pp=QaXoshiro256pp"
fi
case "$MODE" in
  ubasan) GEN_CXXFLAGS="$GEN_CXXFLAGS -fno-omit-frame-pointer -fsanitize=undefined,address" ;;
  tsan)   GEN_CXXFLAGS="$GEN_CXXFLAGS -fno-omit-frame-pointer -fsanitize=thread" ;;
  msan)   GEN_CXXFLAGS="$GEN_CXXFLAGS -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-recover=memory" ;;
esac
# Once these objects are instrumented, the sanitizer's module constructor in each of them refers to
# the runtime's own globals. --keep-global-symbol alone makes those local, and lld will not link a
# local common symbol ("relocation R_X86_64_PC32 cannot refer to absolute symbol"), so the runtime's
# symbols are kept global beside the one entry point. Matching them by pattern needs llvm-objcopy.
LOCALIZE_KEEP=""
case "$MODE" in
  ubasan|tsan|msan)
    case "$OBJCOPY" in
      *llvm-objcopy) LOCALIZE_KEEP="--regex --keep-global-symbol='_+(asan|ubsan|tsan|msan|lsan|sanitizer)_.*'" ;;
      *) echo "[build.sh] $MODE needs llvm-objcopy for the embedded objects, found ${OBJCOPY:-none}" >&2; exit 2 ;;
    esac ;;
esac
B="build/$MODE"
mkdir -p "$B"

# ---- build.ninja ---------------------------------------------------------------------------
NINJA="$B/build.ninja"
{
  echo "cxx = $CCACHE $CXX"
  echo "cxxflags = $COMMON $CXXFLAGS"
  echo "rule cc"
  echo "  command = \$cxx \$cxxflags -MMD -MF \$out.d -c \$in -o \$out"
  echo "  depfile = \$out.d"
  echo "  deps = gcc"
  echo "rule cc_ext"
  echo "  command = \$cxx $COMMON $GEN_CXXFLAGS \$defs -MMD -MF \$out.d -c \$in -o \$out"
  echo "  depfile = \$out.d"
  echo "  deps = gcc"
  # the renamed main is an ordinary C++ function, so $keep is its mangled name:
  # _Z21omnium_generator_mainiPPc is int omnium_generator_main(int, char**)
  echo "rule localize"
  echo "  command = $OBJCOPY $LOCALIZE_KEEP --keep-global-symbol=\$keep \$in \$out"
  echo "rule link"
  echo "  command = $CXX \$in -o \$out $LINK_COMMON $LDEXTRA $LIBS"
  # On Windows the embedded objects are linked as compiled. Localizing a COFF object also turns its
  # COMDAT leaders (the std templates it instantiates and the .refptr stubs) into static symbols,
  # the linker then no longer folds them with the other objects', and a stub such as
  # .refptr.__dso_handle is left unrelocated, which crashes the first static initializer. Besides
  # the entry point these objects define nothing with external linkage that is not a COMDAT (but
  # Xoshiro256pp, see GEN_CXXFLAGS), so there is nothing to hide.
  RAW=_raw; [ "$WIN" != 1 ] || RAW=""
  localize_obj() {
    [ "$WIN" = 1 ] && return
    echo "build $B/$1.o: localize $B/$1_raw.o"
    echo "  keep = $2"
  }
  OBJS=""
  for src in src/*.cpp; do
    o="$B/$(basename "${src%.cpp}").o"
    echo "build $o: cc $src"
    OBJS="$OBJS $o"
  done
  if [ "${NO_GENERATOR:-0}" != 1 ] && [ -f "$GEN_SRC" ]; then
    echo "build $B/generator$RAW.o: cc_ext $GEN_SRC"
    echo "  defs = -Dmain=omnium_generator_main -DOMNIUM_EMBEDDED"
    localize_obj generator _Z21omnium_generator_mainiPPc
    OBJS="$OBJS $B/generator.o"
  fi
  if [ -f "$REVGEN_SRC" ]; then
    echo "build $B/revgen$RAW.o: cc_ext $REVGEN_SRC"
    echo "  defs = -Dmain=omnium_revgen_main -DOMNIUM_EMBEDDED"
    localize_obj revgen _Z18omnium_revgen_mainiPPc
    OBJS="$OBJS $B/revgen.o"
  fi
  if [ "${NO_REDUCER:-0}" != 1 ] && [ -f "$REDUCER_SRC" ]; then
    echo "build $B/reducer$RAW.o: cc_ext $REDUCER_SRC"
    echo "  defs = -Dmain=omnium_reducer_main -DOMNIUM_EMBEDDED"
    localize_obj reducer _Z19omnium_reducer_mainiPPc
    OBJS="$OBJS $B/reducer.o"
  fi
  echo "build $B/$OUT: link $OBJS"
  echo "default $B/$OUT"
} > "$NINJA"

echo "[build.sh] mode=$MODE client basedir=$BD"
# inside the if, so a failed ninja reaches the message rather than ending the script under set -e
if ! ninja -f "$NINJA" ${NINJA_ARGS:-} 2>&1 | grep -v '^ninja: Entering directory'; then
  echo "[build.sh] ninja failed (rc=${PIPESTATUS[0]})"; exit 1
fi
[ -f "$B/$OUT" ] || { echo "[build.sh] build failed" >&2; exit 1; }
cp -f "$B/$OUT" "./$OUT.tmp"
BIN="./$OUT"
if ! mv -f "./$OUT.tmp" "./$OUT" 2>/dev/null; then
  # Windows will not overwrite an .exe that runs, but it would let one be renamed and another put in its place.
  # That must not be done: under MSYS2 fork() starts the child from the parent's exe path, so a process whose
  # exe was replaced can never fork again (a live run stalled with "cannot spawn a trial child"). So a busy
  # exe waits a while for whatever runs it, and then stays: the new build is ./$OUT.new.
  if [ "$WIN" = 1 ]; then
    for i in 1 2 3 4 5 6 7 8 9 10; do sleep 1; mv -f "./$OUT.tmp" "./$OUT" 2>/dev/null && break; done
    if [ -f "./$OUT.tmp" ]; then
      mv -f "./$OUT.tmp" "./$OUT.new"
      BIN="./$OUT.new"
      echo "[build.sh] ./$OUT is in use and is not replaced (a process whose exe is replaced cannot fork any more under MSYS2): the build is $BIN" >&2
    fi
  else
    mv -f "./$OUT.tmp" "./$OUT"                                 # the failure again, so that set -e reports it
  fi
fi
[ "$BIN" = "./$OUT" ] && rm -f "./$OUT.new"                      # a new build took the name: the one that waited is of no more use
echo "[build.sh] built: $BIN ($(stat -c %s "$BIN") bytes)"
echo "[build.sh] sanity: $($BIN --version 2>&1 | head -1)"
if [ "${SKIP_SELFTEST:-0}" != 1 ]; then
  if ! "$BIN" --selftest; then
    mv -f "$BIN" "./$OUT.failed"
    echo "[build.sh] selftest FAILED: kept as ./$OUT.failed" >&2
    exit 1
  fi
  # the build is good, so the binary kept from an earlier failure is of no more use
  rm -f "./$OUT.failed"
fi
