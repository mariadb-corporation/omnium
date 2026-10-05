# omnium on Windows

omnium is one tree. The Windows side runs the same binary, built under MSYS2, against the Windows
server builds under `C:\test`. This file covers what differs from Linux.

## Build

`build_windows.bat [release|debug|clean]` finds MSYS2, checks the packages the build and the runs
need (`openssl`, `xz`, `findutils` and `perl` are the ones the trials call) and runs
`./build.sh` in an MSYS2 shell. The result is `omnium.exe` beside the sources. Run it from an MSYS2
shell, or with `C:\msys64\usr\bin` on `PATH`, since that is where its runtime DLLs live.

omnium is written against POSIX: it forks and execs child processes, keeps a control pipe on fd 3
and 4 and reads `/proc`, and its selftest opens a pty to check the live view. MSYS2's runtime supplies all of that, so the
build uses the MSYS2 packages, not the mingw ones. Under MSYS2 `build.sh` differs in these points:

- The compiler is `clang++` when the box has it, else `g++`, with the default C++ library and
  linker. There is no rpath and no build-id on a Windows binary.
- The client library is the MSYS2 `libmariadbclient-devel` package, `-lmariadb` with the headers
  under `/usr/include/mysql`. It is built with the same runtime as omnium. The basedirs carry an
  MSVC `mariadbclient.lib` and `libmariadb.dll`, which is not a mix to link into an MSYS2 binary.
- `-ldw -lelf -ldl -lresolv` are Linux libraries and are left out.
- The sanitizer modes (`ubasan`, `msan`, `tsan`) and `coverage` are not available.

## Where things live

`TEST_DIR` is `/c/test`, which is how MSYS2 names `C:\test`. A Windows basedir is named
`MD<ddmmyy>-mariadb-<version>-windows-x86_64-<opt|dbg>`, and the name parser marks it as a Windows
build.

The checkout does not go in `C:\test\omnium`: that is omnium's own queue directory,
`<TEST_DIR>/omnium`, beside the registry `<TEST_DIR>/omnium.builds`. The repo goes in the home
folder or in `C:\omnium`, as on Linux it sits in the home folder beside `/test/omnium`.

## How a client reaches the server

A Windows server has no unix socket, so every client omnium runs goes over TCP to `127.0.0.1` on
the trial's port: the in-process client, the readiness probe, the shutdown through `mariadb-admin`,
`mariadb-backup` (`--host` and `--port` instead of `--socket`), the MTR replay and the `omnium cl`
and `omnium replay` clients. The server gets `--port=` and no `--socket=`, which on Windows would
name a pipe nobody opens. A saved trial's `start`, `stop` and `cl` scripts carry the same
`-h127.0.0.1 -P<port> --protocol=tcp --skip-ssl` arguments (no TLS on the loopback, so the client
tool does not warn about a passwordless login), and the FederatedX area's `CREATE SERVER` names
`HOST '127.0.0.1', PORT <port>` where the Linux SQL names the socket file.

The switch is the basedir: a `-windows-` name puts the trial on TCP. `OMNIUM_TCP=1` in the
environment does the same for a Linux build, which is how the TCP path is exercised on Linux:
`OMNIUM_TCP=1 ./omnium selftest --deep` runs every live check over TCP. A Linux server keeps its
`--socket=` even then, since the default socket path would clash between trials.

Plugins are named without an extension in the SQL (`INSTALL SONAME 'ha_federatedx'`), so the server
loads `ha_federatedx.dll` on Windows and `.so` on Linux, and an area that needs a plugin looks for
the `.dll` in a Windows basedir.

## The datadir template

`mariadb-install-db.exe` has its own option set. omnium calls it with `--datadir=` alone, plus
`--innodb-page-size=` when MYINIT sets a page size; the Linux options (`--no-defaults`,
`--basedir=`, `--force`, `--auth-root-authentication-method=normal`, the buffer pool cap and any
other MYINIT option) do not exist there and are left out. The tool writes a `my.ini` into the
datadir; the server runs with `--no-defaults` and never reads it.

## Memory

MSYS2's `/proc/meminfo` has no `MemAvailable` line. The RAM governor and the slot sizing take
`MemFree` instead, which reads lower than the Linux figure, so the same `RAM_CAP_PCT` is a little
more cautious on Windows.

## The UniqueID of a Windows crash

A Windows server writes no core file, and there is no gdb for it: the binaries carry PDB symbols,
not DWARF. Instead the server resolves its own frames at crash time and writes them into the error
log, one per line as `module!symbol()[file:line]`, followed by a minidump in the data directory.
The server code lives in `server.dll`, so the symbols that matter are `server.pdb`; both ship in
`bin`.

omnium reads those frames the way it reads gdb's. The abort route (`my_sigabrt_handler`, `raise`,
`abort`, the CRT's assert) and the CRT and OS modules are left out, as `__GI_raise`, `__GI_abort` and
`__assert_fail` are on Linux; `???` is a frame without a symbol, gdb's `?? ()`; the `do_command`
rule holds; the first four frames remain. The first crash in the log is the one read.

The exception code becomes the signal name a Linux build gives the same fault, so the UID matches
the known-bug lists and reads the same in a report:

| Exception code | Signal | Fault |
|---|---|---|
| `0xc0000005`, `0xc0000006`, `0xc00000fd` | `SIGSEGV` | access violation, in-page error, stack overflow |
| `0x80000003` | `SIGABRT` | `abort()` and a failed assert: the handler reaches the server through `__debugbreak` |
| `0xc000001d`, `0xc0000096` | `SIGILL` | illegal or privileged instruction |
| `0xc0000094`, `0xc0000095`, `0xc000008e`, `0xc0000090`, `0xc0000091` | `SIGFPE` | integer and float faults |
| `0x80000002` | `SIGBUS` | datatype misalignment |
| any other | `exception 0x...` | kept as the server printed it |

The assert expression comes from the CRT's line, `Assertion failed: expr, file X, line N` from the
release CRT or `X(N) : Assertion failed: expr` from the debug CRT. InnoDB's `Failing assertion:`
reads as on Linux. The UID shapes are the Linux ones: `SIGSEGV|f1|f2|f3|f4` for a crash and
`expr|SIGABRT|f1|f2|f3|f4` for a failed assert.

The version directory under the builds' root is scrubbed to `/test/X/` on both sides. `TEST_DIR`
names the root, and for `/c/test` the native `C:\test\` and `C:/test/` count too, with the rest of
that path turned to slashes, so the same line gives the same UID on both boxes.

## What does not carry over

- `omnium build` is not ported: it drives the cmake lines of the Linux build scripts and names the
  result `-linux-x86_64-`. The Windows builds come from `build.ps1` in `C:\test`.
- `omnium reduce` is not ported: the embedded reducer drives the framework's Linux `pquery`
  binary and its bash stages. A saved Windows trial is reduced on a Linux box, or by hand.
- `omnium stack`, `omnium ldd` and the copy of the server binary beside a saved trial read ELF
  files with gdb and `ldd`. A Windows trial keeps its error log, the frames in it and the
  minidump; the copy step logs a note and moves on.
- `PR_SET_PDEATHSIG`, a child dying with its parent, has no Windows equivalent. A child is still
  killed with its process group on a stop, so the effect is the same on a clean stop and differs
  only when omnium itself is killed with SIGKILL.
- `/proc/self/coredump_filter` and `RLIMIT_CORE` shape a Linux core file. Windows writes a
  minidump instead, so `CORE_MAX_GB` and the buffer-pool trimming do not apply, and `omnium init`
  skips its gdb, screen and `kernel.core_pattern` checks there.
- `screen` is not on Windows. `omnium reduce --screen` needs it; `omnium reduce` without it works.
- The compiler line in the report banner comes from `readelf`; a Windows build's banner has the
  version and the revision without it.
