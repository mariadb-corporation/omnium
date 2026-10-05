# omnium

One binary for the MariaDB QA pipeline: build a server from a branch name, run trials against it,
find and identify the failures, reduce each new one to a small testcase, replay that on every build
under test, write the bug report, and file it in Jira once a human has said ok.

omnium runs beside the mariadb-qa framework and reads its files where they live, in the mariadb-qa
checkout (`~/mariadb-qa`, or omnium's own sparse clone beside the binary, see
`docs/framework_files.md`): `known_bugs.strings` and `known_bugs.strings.SAN` stay the source of
truth for what is known, `BUGS/` keeps the testcases of filed bugs, the `REGEX_ERRORS_*` lists, the
`*.filter` files and `filter.sql` drive detection and the SQL each trial leaves out. This repo keeps
no copy of any of them, so there is one list to read and one list to edit. The UniqueID chain is a
port of the framework's shell scripts and gives the same UID for the same trial, so a UID from
omnium can be searched in the same lists and tickets.

## Build

```
./build.sh                # release build, ./omnium, then the selftest
./build.sh debug          # ./omnium_dbg
NO_GENERATOR=1 ./build.sh # without the embedded generator (faster link, fewer SQL sources)
NO_REDUCER=1 ./build.sh   # without the embedded reducer; omnium reduce then runs reducercpp/reducer
SKIP_SELFTEST=1 ./build.sh
./build.sh coverage      # ./omnium_cov, built for coverage numbers
./build.sh ubasan        # ./omnium_ubasan, UBSAN+ASAN
./build.sh tsan          # ./omnium_tsan
./build.sh msan          # ./omnium_msan, needs the instrumented libc++ in /MSAN_libs
./build.sh clean
```

Each sanitizer binary takes the same verbs, so `./omnium_tsan selftest` is the whole check list under
ThreadSanitizer. `docs/sanitizer_builds.md` says what each one can prove here.

On Windows, `build_windows.bat` builds the same binary under MSYS2; `docs/windows.md` covers that
side: the build, the Windows basedirs and how a Windows crash gets its UniqueID.

`./coverage.sh` runs the selftest against `./omnium_cov` and prints the line coverage of omnium's own
sources. `./coverage.sh --deep` adds the checks that need a real server, `--html` writes a report
under `coverage/`.

Needs clang, ninja, lld, libc++, PCRE2, libcurl, libdw and a built MariaDB basedir under `/test` for
the client library (`build.sh` picks the newest optimised build). `revgen/revgen.cpp`,
`generatorcpp/generator.cpp` and `reducercpp/reducer.cpp` are compiled into the binary from the
mariadb-qa checkout, and the run-time files are read from it. That checkout is `~/mariadb-qa` when
the box has one. Otherwise `build.sh` makes a blobless clone at `<repo>/mariadb-qa` whose sparse
working tree holds just the files omnium needs, about 120 MB in all (`qa_checkout.sh` carries the
list), and `omnium init` pulls it. `QA_DIR` names another checkout.

## First steps

```
omnium init                     # what the box needs, the queues under /test/omnium, ~/.omnium_aliases
omnium builds                   # scan /test, list the registry /test/omnium.builds
omnium builds test <name> yes   # a build that trials run against
omnium builds report <name> yes # a build the Bug Detection Matrix replays on
omnium run                      # trials on every test build, one workdir /data/O<id>
omnium status                   # every run, live or finished
```

A stopped run can be picked up again: `omnium run --resume` takes the newest run under `/data`, or
name it with `omnium run --resume O123456`. It reads the run's own `run.conf`, so the builds, the
slots and the trial window are the same, and the trial numbers carry on. `omnium run --for <seconds>`
bounds the whole run; `--seconds` is one trial.

Ctrl+C once lets the running trials and reductions finish, twice stops at once; what is still
queued waits for `--resume`. `omnium tui` shows the run while it goes, and its keys ask the same
through `<workdir>/omnium.ctl`. A trial that shows a new UID is kept under `/data/O<id>/<trial>/`
with its data directory, error log, SQL trace, core and `MYBUG`.
With `AUTO_PIPELINE=1` (the default) the run reduces the first trial of each new UID, then writes the
report, while the discovery slots keep going.

## The verbs

| Verb | What it does |
|---|---|
| `build <version or branch> [flavours] [--tar]` | clone and build into `/test/<name>`; `build follow` rebuilds on a new head |
| `builds` | the registry: list, edit, `test`/`report` flags, `set` |
| `run [basedir ...] [--trials N] [--slots N] [--seconds S] [--area NAME]` | the trials |
| `areas [basedir]`, `sql <basedir>` | the area table; assemble one trial's SQL into a file |
| `t`, `tt`, `els`, `sts`, `fts`, `stack`, `myver` | the framework's identification tools, in process |
| `parity [N or all]` | the shell UID chain against omnium's, on saved trials |
| `reduce [<workdir>] <trial> [--screen] [--plan]` | reduce one saved trial; `--plan` only shows the settings |
| `matrix <sql> [build ...]` | the Bug Detection Matrix of a testcase |
| `mtr [<workdir>] <trial>` or `mtr <sql> --basedir DIR` | the MTR form of a testcase, verified as a reverse gate |
| `report [<workdir>] <trial> [build ...]` | the bug report, into the inbox; includes the matrix and the MTR test |
| `inbox [--process] [--dry-run] [--file <item>]` | the human queue; `touch <item>.ok` approves an item |
| `jira whoami`, `jira search <uid>`, `jira versions`, `jira components` | Jira over REST, read only |
| `mail <to> [--mx DOMAIN] [--dry-run]` | the mail path a new inbox item uses, for a check |
| `tui [<run>]` | the live view of a run; keys q p P s S l L r |
| `cli` | a shell with the omnium shortcuts; `h` shows them in a box, `h <name>` explains one |
| `fresh [basedir] [--cl]`, `cl`, `replay <file>`, `trial <n>` | a server by hand, its client, a replay, one trial's detail |
| `kb`, `kba`, `kbs`, `kbsa`, `eb` | the known-bug lists and `BUGS/` |
| `adopt <workdir> [--reduce] [--report]` | take a pquery-run workdir's saved trials through reduce and report |
| `ldd [basedir or dir]` | a server binary with the libraries it needs, gathered for a core |
| `status [run]`, `init`, `config`, `selftest`, `help`, `version` | the box and the settings |

`omnium selftest` runs the built-in checks; `omnium selftest --deep` adds the ones that start a real
server. The checks send no mail, reach no Jira but a stand-in, and keep the items they file in their
own directory under `/tmp`; the runs of the deep checks are real runs under `/data`. A check this box
cannot run is named under the summary as skipped. `build.sh` runs the plain set on every build.

`omnium help <verb>` shows the flags of one verb.

## Where things live

```
/test/<basedir>/                a built server. omnium reads it; the MTR check puts its test in mariadb-test/main/ while it runs, and rp writes mysql.out where it is run
/test/omnium.builds             the registry: one line per basedir, test and report flags, commit
/test/omnium/HUMAN-queue/       the inbox: <workdir>_bug<N>.report .preview .possible_dup .kb, .ok, .filed
/test/omnium/AI-queue/<item>/   info.txt: what an item still needs (a reduction, an MTR gate, a duplicate check)
/data/O<id>/                    one run: status.txt, ledger, log/, templates/, <trial>/ for each saved trial
/data/O<id>/run.conf            the settings of that run, so omnium run --resume can pick it up
/data/O<id>/omnium.bin          the binary the run started with; a rebuild cannot change a live run
/data/O<id>/mysqld/<build>/     the server binary and its libraries, copied per build, for gdb later
/data/O<id>/bug<N>.sql          the reduced testcase of trial N, tidied for the report
/data/O<id>/bug<N>.test         its MTR form (and bug<N>.opt when the server needs start options)
/data/omnium.seen               every UID any run has seen, with the first workdir and trial
/dev/shm/O<id>/                 the live trial directories of a run (tmpfs)
~/.omnium.conf                  the settings; omnium config lists them, KEY=VALUE on the command line overrides one
~/.omnium_aliases               short names, written by omnium init: orun ost ot ott osr orep omx oin oad obuilds ostack otui ocli omtr ofr ocl orp oi
~/.bashrc                       omnium adds alias om=<repo>/omnium the first time it runs, when no om alias is there
<repo>/filters/adv.filter       omnium's own line filter for the generated SQL
$QA_DIR/                        the framework checkout (~/mariadb-qa, else <repo>/mariadb-qa): the bug lists, the filters, the shell helpers
```

A saved trial holds the data directory, `log/master.err`, the SQL of each client thread,
`pquery.log` with what each thread did, `MYEXTRA` / `MYINIT` / `MYSAFE`, `MYBUG` with the UniqueID,
and the core when there is one.

The framework has a cleaner, `~/mariadb-qa/tmpfs_clean.sh`, that deletes a directory under
`/dev/shm` when no running program mentions it. A run keeps its trial directories there, so omnium
starts one small child that does nothing but sleep with that directory in its command line. The
cleaner then sees the directory in use and leaves it alone. The child ends with the run.

## Settings

`omnium config` prints every key with its meaning. The ones a first run cares about:

| Key | Meaning |
|---|---|
| `RAM_CAP_PCT`, `SHM_CAP_PCT`, `SHM_PAUSE_PCT` | the governors: no new server above these, the biggest trials pause first |
| `TRIAL_SECONDS`, `RUN_DAYS` | the trial window and the length of a run |
| `MULTI_THREAD_PCT`, `CRASH_RECOVERY_PCT`, `BACKUP_PCT` | the share of trials that run several threads, kill and recover, or do a backup round trip: a full `mariadb-backup` of the busy server, an incremental one once the SQL has stopped, prepare, a server started on the result, and `CHECKSUM TABLE` compared on both. A failed step or a difference is kept as `BACKUP_ISSUE`. Builds without `mariadb-backup` (the sanitizer builds) run that share as normal trials |
| `INFILE`, `ALL_DISK_SQL` | the fixed SQL file every trial samples; every `.sql` on the disk as a fourth source |
| `AUTO_PIPELINE` | 1 reduces and reports a new UID inside the run; 0 leaves that to `omnium reduce` and `omnium report` |
| `KEEP_PER_UID` | trials kept per new UID |
| `CORE_DIR` | `auto` (the default): the trial runs on the tmpfs while it is below `SHM_STEPDOWN_PCT` and on `DATA_DIR` above it, so a core never fills the tmpfs. `shm`: always the tmpfs, with the core moved to `DATA_DIR` as soon as the server is gone. `data`: always `DATA_DIR`, at about six times the write cost |
| `SHM_STEPDOWN_PCT` | the `/dev/shm` use at which `CORE_DIR=auto` puts a new trial on `DATA_DIR` instead (75) |
| `CORE_MAX_GB` | a core larger than this is cut off, so one runaway trial cannot fill the disk (40); `0` = no limit |
| `EMAIL`, `PAT_FILE`, `JIRA_URL` | the address that hears about a new inbox item; the Jira token file; the Jira the filing talks to |

## Filing a bug

`omnium report` writes `<workdir>_bug<N>.report` into the inbox with the Jira fields as header lines
and the ticket body in Jira markup under the `-----` line. The `.possible_dup` file lists the Jira
hits for the UID, the `.kb` file the known-bugs verdict. Edit the report as needed, then:

```
omnium inbox                                  # what waits, with age and title
omnium inbox --dry-run --file <item>          # the exact payload Jira would get, in <item>.preview
touch /test/omnium/HUMAN-queue/<item>.ok      # approve
omnium inbox --process                        # file every approved item
```

After a filing the UID goes in `known_bugs.strings` (or `.SAN`) with the key, the testcase in
`BUGS/<key>.sql`, and `<item>.filed` holds the key and the URL. omnium prints the `git add` line for
the two files and runs no git itself. A failure leaves `<item>.error` and the item stays.
Memory-safety sanitizer classes, and an MSAN uninitialised read, are never filed by omnium: they may
be security issues and go by hand. At most ten tickets are filed a day.

## Documentation

`README.md` is what omnium is and how to drive it. `docs/` holds one file per subject that needs
more than a paragraph: what the tool does and why, never a record of how it got there.
`docs/framework_files.md` lists what omnium reads from the mariadb-qa checkout and why this repo
keeps no copy. `docs/core_placement.md` covers where a trial's core file lands.
`docs/sanitizer_builds.md` covers the sanitizer builds and what each one can prove here.
`docs/windows.md` covers the Windows side.
