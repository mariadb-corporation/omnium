# Where a core lands

A trial runs on the tmpfs for speed, so the kernel writes its core there first and a large core can
fill the tmpfs. This is how omnium keeps the speed and keeps the tmpfs.

## What the box does

- `kernel.core_pattern` is the single word `core`, so the kernel writes the core into the working
  directory of the process that died.
- `mariadbd` changes its working directory to its data directory at startup, so the core lands in
  the data directory.
- A symlink at that path does not help: with a symlink where the core would go, the kernel replaces
  the symlink with a real file. A core cannot be pointed somewhere else with a link.
- The data directory has to be on the tmpfs for speed. 20,000 single-row inserts, same build, same
  SQL: 1.98 s with the data directory on `/dev/shm`, 11.91 s with it on `/data`. About six times
  slower.
- A core can be very large. One SIGSEGV core from a 13.1 build with the DuckDB engine loaded was
  23 GB. `/proc/self/coredump_filter` is `0x11` for every server omnium starts, which leaves the
  file-backed pages out, and `--loose-innodb-buffer-pool-in-core-dump=0` keeps the InnoDB buffer
  pool out, but an engine that brings its own allocator still puts its arenas in the core.

## What omnium does

`CORE_DIR=auto` is the default and does two things:

- While `/dev/shm` is below `SHM_STEPDOWN_PCT` (75), a trial runs on the tmpfs at full speed. The
  moment the server process is gone, omnium waits for the core to stop growing, moves it to
  `<workdir>/<trial>/` under `/data`, and leaves a symlink behind so the reducer and gdb still find
  it in the trial directory.
- At or above that mark, a new trial's data directory is created under `DATA_DIR` instead, so its
  core is written straight to `/data` and never touches the tmpfs. The run gets slower rather than
  filling the tmpfs, and the trial says so in its own log, `<workdir>/log/trial_<n>.log`:
  `/dev/shm is at 78%, so this trial runs on /data`.

`CORE_MAX_GB` (40) caps how large one core may be. The trial process sets `RLIMIT_CORE` and the
server it starts inherits it, so a single runaway trial cannot fill the disk. `0` means no limit.

`CORE_DIR=shm` and `CORE_DIR=data` force one or the other for a whole run.

## The one option not taken

`kernel.core_pattern` can be a program (`|/path/to/handler`) that reads the core on a pipe and
writes it straight to `/data`, so nothing is written to the tmpfs at any point. That is the only way
to keep the data directory on the tmpfs and never write a core there. It needs root and it is
box-wide: every tool on the box, the mariadb-qa framework included, would go through it, and the
framework expects a plain `core` file in the trial directory.

`CORE_DIR=data` for every run is the other option, and it is six times slower on the SQL that finds
most bugs.
