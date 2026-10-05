#!/bin/bash
# Builds MariaDB Connector/C from source for the MSYS2 runtime and installs it, for the Windows
# build. MSYS2 has no package for the client library under its own runtime (its mingw packages are
# built against another one, which is not a mix to link into omnium, see docs/windows.md), so
# build.sh calls this when <prefix>/include/mysql/mysql.h is missing.
#
#   ./connector_windows.sh [<prefix>]   default /usr/local: headers in <prefix>/include/mysql,
#                                       the static libmariadbclient.a and the client plugins in
#                                       <prefix>/lib
#   CONNECTOR_TAG=v3.4.11               the release tag to build
#
# Needs git, cmake, ninja, gcc and the openssl and zlib headers: build_windows.bat checks for them.
set -euo pipefail
cd "$(dirname "$0")"
say() { echo "[connector_windows.sh] $*"; }
die() { say "$@" >&2; exit "${RC:-2}"; }
case "$(uname -s)" in MSYS*|MINGW*|CYGWIN*) ;; *)
  die "this is for MSYS2; the Linux build links the client library of a basedir" ;;
esac
PREFIX="${1:-/usr/local}"
TAG="${CONNECTOR_TAG:-v3.4.11}"
URL=https://github.com/mariadb-corporation/mariadb-connector-c.git
SRC=build/connector-src
B=build/connector

for tool in git cmake ninja gcc; do
  command -v "$tool" >/dev/null || die "$tool missing: pacman -S $tool"
done
[ -f /usr/include/openssl/ssl.h ] || die "openssl headers missing: pacman -S openssl-devel"
[ -f /usr/include/zlib.h ] || die "zlib headers missing: pacman -S zlib-devel"

mkdir -p build
if [ ! -d "$SRC/.git" ]; then
  rm -rf "$SRC"
  say "cloning MariaDB Connector/C $TAG"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" "$URL" "$SRC" \
    || { rm -rf "$SRC"; RC=1 die "git clone failed"; }
fi

# the connector builds with -Werror, and Cygwin's ctype macros (isspace and the rest, which index
# with a plain char) trip -Wchar-subscripts in it, so warnings are left as warnings
say "building $TAG into $PREFIX"
mkdir -p "$B"
if ! { cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX="$PREFIX" -DINSTALL_INCLUDEDIR=include/mysql \
         -DINSTALL_LIBDIR=lib -DINSTALL_PLUGINDIR=lib/plugin -DWITH_SSL=OPENSSL \
         -DWITH_EXTERNAL_ZLIB=ON -DWITH_UNIT_TESTS=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
       && cmake --build "$B" && cmake --install "$B"; } >"$B/build.log" 2>&1; then
  tail -30 "$B/build.log" >&2
  RC=1 die "failed; the whole log is $B/build.log"
fi
if [ ! -f "$PREFIX/include/mysql/mysql.h" ] || [ ! -f "$PREFIX/lib/libmariadbclient.a" ]; then
  RC=1 die "installed, but $PREFIX has no include/mysql/mysql.h and lib/libmariadbclient.a"
fi
say "installed in $PREFIX"
