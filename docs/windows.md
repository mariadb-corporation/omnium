# omnium on Windows

omnium is one tree. The Windows side runs the same binary, built under MSYS2, against the Windows
server builds under `C:\test`. This file covers what differs from Linux.

## Build

`build_windows.bat [release|debug|clean]` finds MSYS2, checks the packages the build and the runs
need (`openssl`, `xz`, `findutils` and `perl` are the ones the trials call; `cmake`, `openssl-devel`
and `zlib-devel` are for the client library below) and runs `./build.sh` in an MSYS2 shell, started
in the folder of the script. The result is `omnium.exe` beside the sources. Run it from an MSYS2
shell, or with `C:\msys64\usr\bin` on `PATH`, since that is where its runtime DLLs live.

omnium is written against POSIX: it forks and execs child processes, keeps a control pipe on fd 3
and 4 and reads `/proc`, and its selftest opens a pty to check the live view. MSYS2's runtime supplies all of that, so the
build uses the MSYS2 packages, not the mingw ones. Under MSYS2 `build.sh` differs in these points:

- The compiler is `clang++` when the box has it, else `g++`, with the default C++ library and
  linker. There is no rpath and no build-id on a Windows binary.
- The client library is MariaDB Connector/C, built from source for the MSYS2 runtime. MSYS2 has no
  package for it under that runtime (its mingw packages are built against another one), and the
  basedirs carry an MSVC `mariadbclient.lib` and `libmariadb.dll`, which is not a mix to link into
  an MSYS2 binary. When `/usr/local/include/mysql/mysql.h` is missing, `build.sh` runs
  `connector_windows.sh`: it clones the pinned release (`CONNECTOR_TAG`) into `build/connector-src`,
  builds it with cmake and installs it into `/usr/local`. `MARIADB_BASEDIR=` names another prefix.
  omnium links the static `libmariadbclient.a` as on Linux, with `-lssl -lcrypto -lz`.
- `-ldw -lelf -lresolv` are Linux libraries and are left out; the runtime has `dlopen` itself.
- `-D_GNU_SOURCE` is added, which the compilers define on their own on Linux: without it
  `-std=c++20` hides `usleep`, `kill`, `setenv`, `pipe2`, `u_char`, the pty calls and more in the
  MSYS2 headers.
- The generator, revgen and reducer of the mariadb-qa checkout are linked as compiled. On Linux
  `objcopy` makes every symbol of theirs local. On a COFF object that also turns the COMDAT leaders
  (the std templates they instantiate, and the `.refptr.*` stubs) into static symbols, the linker
  stops folding them with the other objects', and the first static initializer runs on an
  unrelocated `.refptr.__dso_handle` and crashes. What they define with external linkage besides
  the entry point is only COMDATs, with one exception: their global `Xoshiro256pp` has the name of
  omnium's own (`common.h`), so it is renamed with a `-D` on their compiles. `src/msys2/sys/auxv.h`
  stands in for the Linux header they include to seed their random generator.
- The entry points are weak references, and a PE link has no undefined weak symbols, so the one a
  `NO_GENERATOR=1` or `NO_REDUCER=1` build leaves out is defined as 0, which reads as absent.
- The C++ library is libstdc++, not libc++, and its `file_time_type` has an epoch of its own (2174),
  so `fs::last_write_time(...).time_since_epoch()` is not Unix time. The code takes file times from
  `file_mtime()`, or subtracts two clocks as `inbox.cpp` does.
- The sanitizer modes (`ubasan`, `msan`, `tsan`) and `coverage` are not available.

## Where things live

`TEST_DIR` defaults to `/c/test` here (`/test` on Linux), which is how MSYS2 names `C:\test`. MSYS2's
own `/test` is `C:\msys64\test`, where no build is. A settings file written before that default
still says `/test`: `omnium builds` and `omnium init` then say that `TEST_DIR` holds no build, and
that `/c/test` does, and `omnium config TEST_DIR=/c/test` fixes it. A Windows basedir is named
`MD<ddmmyy>-mariadb-<version>-windows-x86_64-<opt|dbg>`, and the name parser marks it as a Windows
build.

The checkout does not go in `C:\test\omnium`: that is omnium's own queue directory,
`<TEST_DIR>/omnium`, beside the registry `<TEST_DIR>/omnium.builds`. The repo goes in the home
folder or in `C:\omnium`, as on Linux it sits in the home folder beside `/test/omnium`.

The settings file `~/.omnium.conf` and the shell files (`~/.omnium_aliases`, `~/.bashrc`) are in the
MSYS2 home, `C:\msys64\home\<you>`, because the shell reads them there. The Jira token is not: the
MSYS2 home is a folder of the shared MSYS2 install, so the token defaults to `~/.omnium_jira_pat` in
the Windows profile, `C:\Users\<you>`, which MSYS2 names `/c/Users/<you>`. Only MSYS2's own home is
replaced that way: a `HOME` set to anything else is the home, which is how the selftest keeps the
omniums it starts away from the real token. `PAT_FILE` names another file.

A Windows file is protected by its ACL, which `umask` does not set, and MSYS2 mounts ignore ACLs,
so `ls -l` shows 644 whatever the ACL says. When it finds no token, `omnium init` prints the line
that keeps the file to one account:
`icacls "$(cygpath -w FILE)" /inheritance:r /grant:r "$USERNAME:F"`.

## How a client reaches the server

A Windows server has no unix socket, so every client omnium runs goes over TCP to `127.0.0.1` on
the trial's port: the in-process client, the readiness probe, the shutdown through `mariadb-admin`,
`mariadb-backup` (`--host` and `--port` instead of `--socket`), the MTR replay and the `omnium cl`
and `omnium replay` clients. The server gets `--port=` and no `--socket=`, which on Windows would
name a pipe nobody opens. A saved trial's `start`, `stop` and `cl` scripts carry the same
`-h127.0.0.1 -P<port> --protocol=tcp --skip-ssl` arguments (no TLS on the loopback, so the client
tool does not warn about a passwordless login), and the FederatedX area's `CREATE SERVER` names
`HOST '127.0.0.1', PORT <port>` where the Linux SQL names the socket file. The in-process client turns
TLS off for that connection too (`MYSQL_OPT_SSL_ENFORCE` and `MYSQL_OPT_SSL_VERIFY_SERVER_CERT` off):
Connector/C 3.4 switches TLS on for a connection that verifies the server certificate, which is its
default, and then refuses a server that has none, as a 10.11 build has not
(`SSL is required, but the server does not support it`). A matrix row for a build that did not start
reads "server did not start", and takes no UID from the noise of aborted connections in its log.

The switch is the basedir: a `-windows-` name puts the trial on TCP. `OMNIUM_TCP=1` in the
environment does the same for a Linux build, which is how the TCP path is exercised on Linux:
`OMNIUM_TCP=1 ./omnium selftest --deep` runs every live check over TCP. A Linux server keeps its
`--socket=` even then, since the default socket path would clash between trials.

Plugins are named without an extension in the SQL (`INSTALL SONAME 'ha_federatedx'`), so the server
loads `ha_federatedx.dll` on Windows and `.so` on Linux, and an area that needs a plugin looks for
the `.dll` in a Windows basedir.

A server option with a path has to be native for a native server. The MSYS2 runtime converts a plain
`--opt=/path` itself when it starts a Windows program, but not `--opt=FILE:/path`, which then reaches
the server as `/dev/shm/...` and is looked for on the wrong drive (the `innodb-encryption` area's
`--file-key-management-filekey=FILE:...` never started). `Instance::argv()` and the `start` script of a
saved trial therefore turn the path of such an option into `C:/msys64/dev/shm/...`
(`native_option` in `util.cpp`).

## The datadir template

`mariadb-install-db.exe` has its own option set. omnium calls it with `--datadir=` alone, plus
`--innodb-page-size=` when MYINIT sets a page size (`omnium matrix`, `omnium fresh` and the MTR
replays put the page size of a testcase's `mysqld options required for replay` header into MYINIT, as
the server refuses a datadir made with another one); the Linux options (`--no-defaults`,
`--basedir=`, `--force`, `--auth-root-authentication-method=normal`, the buffer pool cap and any
other MYINIT option) do not exist there and are left out. The tool writes a `my.ini` into the
datadir; the server runs with `--no-defaults` and never reads it.

## Memory

MSYS2's `/proc/meminfo` has no `MemAvailable` line. The RAM governor and the slot sizing take
`MemFree` instead, which reads lower than the Linux figure, so the same `RAM_CAP_PCT` is a little
more cautious on Windows.

MSYS2 has no tmpfs: `SHM_DIR` (`/dev/shm`) is a plain folder of the MSYS2 install, `C:\msys64\dev\shm`,
so the trials run on disk and `SHM_CAP_PCT`, `SHM_PAUSE_PCT` and `SHM_STEPDOWN_PCT` measure that
disk's use, not RAM's. `omnium init` says so rather than printing the disk's size as RAM. `SHM_DIR=`
can name a folder on a RAM disk to get the Linux behaviour.

## Child processes

Cygwin copies only the forking thread's stack into a forked child. A child of a thread other than
the main one therefore cannot read anything that lives on another thread's stack, such as a
`std::string` the caller passed by reference: the first read ends it with SIGSEGV. That was why
every trial failed with `generator rc 11`, since the generator is started from a thread with a
directory that sat on the main thread's stack. So every fork (`run_capture`, `run_capture_in`,
`spawn_program`, `spawn_role`) first copies argv, the environment, the program's path (a bare
command is looked up on the `PATH` the child is given), the directory and the log into an
`ExecPlan` on its own stack, and the child touches nothing but that, its locals, the heap and
globals. The failure reads `generator killed by signal 11 (SIGSEGV)`, with the last lines of the
log, instead of the raw wait status.

**A rebuild of `omnium.exe` must not reach a process that runs from it.** `fork()` under MSYS2 starts
the child by launching the parent's executable again, by its path, and copying the parent's memory
into it. When a different file stands at that path (a rebuilt `omnium.exe`), the child image does not
match and the copy fails for every fork from then on: `child_copy: data read copy failed ... Win32
error 299`, then `CreateThread failed for sig ... error 193`, then `errno 11`. A run stalled that way,
logging `cannot spawn a trial child` every two seconds, and the README's "a rebuild cannot change a
live run" is only true because of what follows. So the verbs that run for long (`run`, `tui`,
`matrix`, `report`, `reduce`, `mtr`, `adopt`, `parity`, `build`, `selftest`) first start from a copy of the
binary, `/tmp/omnium_exe/omnium-<size>-<mtime>.exe` (`private_exe_copy`; `OMNIUM_COPY` marks the copy and
`OMNIUM_REPO` tells it where the checkout is), and `omnium.exe` is free to be rebuilt. A run keeps its roles
on a copy of its own, `omnium.bin` in its workdir, as before. Copies that nothing runs go once they are ten
minutes old; opening a running exe for writing fails with "Device or resource busy", which is how one in use
is told. A process started before this existed, or from `omnium.exe` by any other verb, still depends on
the file: **`build.sh` therefore never replaces an `omnium.exe` that is in use.** It waits ten seconds for
it, and then leaves the build as `omnium.exe.new` and says so.

The all-disk SQL set (`ALL_DISK_SQL=1`) is not `find /` here: `/` is the MSYS2 install, and its
`/proc/registry` is the Windows registry as folders, which `find` walks for minutes. The scan
covers the Windows profile, the MSYS2 home, `TEST_DIR` and `DATA_DIR`, without `AppData` and the
`.git` folders.

## MTR (`omnium mtr`, and the MTR part of `omnium report`)

A testcase is verified by running it under the build's own `mariadb-test-run.pl`. On Windows that
needs two things the builds and the box do not have by default.

- **The suite in the build.** The Windows builds of `C:\test` come from `build.ps1`, which passes
  `-DBUILD_CONFIG=mysql_release`, and `cmake/build_configurations/mysql_release.cmake` leaves
  `INSTALL_MYSQLTESTDIR` empty on Windows, so a build has no `mariadb-test` folder. For a new build
  put `'-DINSTALL_MYSQLTESTDIR=mariadb-test'` into the cmake arguments of `build.ps1`. For one that
  exists, configure its source again with that flag and install the Test component into it
  (`cmake --install <builddir> --component Test --prefix <basedir>`); that adds `mariadb-test`
  (some 18000 files, 260 MB) and the test plugins in `lib\plugin`, and leaves the server alone.
  `omnium builds` and `omnium init` name every registered Windows build without it, and
  `omnium mtr` says the same instead of "no test runner". omnium does not build on Windows
  (`omnium build` is not ported), so no cmake line of its own carries the flag.
- **A native Windows perl.** MTR on MSYS2's perl (`$^O` is `cygwin` or `msys`) takes the box for
  Cygwin and stops with `Cygwin /bin/sh subshell requires fix with --cygwin-subshell-fix=do`. That
  flag puts a wrapper over `/bin/sh`, which omnium itself runs on: never use it. MTR wants Strawberry
  Perl (`$^O` is `MSWin32`); the portable zip needs no admin rights. omnium finds the perl in this
  order: the `PERL` setting, `C:\Strawberry\perl\bin`, `C:\Perl64\bin`, `C:\Perl\bin`,
  `%USERPROFILE%\tools\strawberry-perl\perl\bin`, then every `perl.exe` on the `PATH`, and takes the
  first one that answers `MSWin32` to `print $^O`. Its folder goes first on the `PATH` of the
  runner. The `--vardir` and `--tmpdir` arguments are MSYS2 paths, which the runtime converts for a
  native program, so they need no handling. `omnium init` checks for the perl as well.

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

A frame is named as gdb names it where MSVC differs: X::`scalar deleting destructor' is the second
X::~X frame, std::_Atomic_storage<T,N> is std::__atomic_base<T>, `anonymous namespace' is (anonymous
namespace). A failed /RTC1 run-time check (failwithmessage, _RTC_* frames) is no frame, as abort() is
none, and the check that failed (_RTC_StackFailure, _RTC_UninitUse) stands where an assertion's text
does. Spelling is not renamed but read as one when a UID is looked up in the known-bugs lists
(Windows only): __null is 0, 64u is 64, true is 1, 'A, B' is 'A,B', '> >' is '>>', 'Item *' is
'Item*', __int64 is long; a real 0 stays 0 and a UID stays as printed. An error-log line of a Windows
server says mariadbd.exe: where Linux says mariadbd:. s/mariadbd.exe/mariadbd/ (and mysqld.exe,
mariadb.exe, mysql.exe) is done once, as the error-log lines are read, so every rule sees a Windows
line as its Linux twin. `omnium parity` gives the scripts a copy of a Windows log with mariadbd.exe
and mysqld.exe stripped, as the port reads it, so the two chains compare the same text.

A crash leaves `mariadbd.dmp` in the datadir, and that dump is what a core is to a trial
(`Instance::has_dump`): the trial takes the crash UID from the frames in the log even when the
error-log scan flagged another line as well, as a core wins on Linux. Before that a trial whose log had
both was saved with the flagged line's UID (`SLAVE_ERROR|...`) in `MYBUG`, and the crash was filed as
noise. A flagged line with no crash gets the UID `omnium t` gives it (the same function), so `MYBUG`
and `omnium t` agree; the scan's own pick stays when the chain has none.

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

## The selftest

`omnium selftest` runs under MSYS2 and, as on any box, names the checks it cannot run under the
summary as skipped:

- `cmake_command`: `omnium build` is not ported.
- The out-of-memory score of a child: `/proc/<pid>/oom_score_adj` is the Linux kernel's.
- A BASEDIR file pointing the chain at a build: the chain takes ELF server binaries, and a Windows
  server is a PE file.
- A holder started through a link: MSYS2's `/proc/<pid>/exe` names the path a process was started
  by, not the binary behind a link, so a second omnium cannot tell such a run from another program
  by its binary. A run started as `omnium.exe` shows omnium in its command line and is told.
- `mount_point_of` on `/proc/self`: `/proc/<pid>` is on a device of its own.
- The shell scan on a log line with a byte the locale cannot read: the grep 3.0 of MSYS2 prints its
  note on stdout, so the script answers `UNTYPED` where the check describes grep 3.5 and later.
- A stopped run picked up again, on a box with no build under `TEST_DIR`.

The Jira checks talk to a stand-in Jira that takes any token, so they run with a stand-in one on
every box and the real token never leaves its file. A box without a token gets a "Jira PAT missing"
note under the summary: only a real search or filing needs one.
