// Created by Roel Van de Paar, MariaDB
// omnium - one binary for the MariaDB QA pipeline. Shared declarations for every module.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

// MemorySanitizer cannot see a library it did not instrument write into a buffer, so every value
// omnium takes out of pcre2, the resolver or another system library is marked written here.
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
#    include <sanitizer/msan_interface.h>
#    define OMNIUM_LIB_WROTE(p, n) __msan_unpoison((p), (n))
#  endif
#endif
#ifndef OMNIUM_LIB_WROTE
#  define OMNIUM_LIB_WROTE(p, n) ((void)(p), (void)(n))
#endif

namespace fs = std::filesystem;
using std::string;
using std::vector;

extern const char* OMNIUM_VERSION;

// ---------------------------------------------------------------------------------------------
// util.cpp - strings, files, time, processes
// ---------------------------------------------------------------------------------------------
string trim(std::string_view v);
string upper(std::string_view v);
string lower(std::string_view v);
vector<string> split(std::string_view v, char sep);
vector<string> split_ws(std::string_view v);                  // on runs of blanks, no empties
vector<string> split_lines(std::string_view v);               // keeps empty lines, drops a final ""
string join(const vector<string>& v, std::string_view sep);
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
bool icontains(std::string_view hay, std::string_view needle);
bool iequals(std::string_view a, std::string_view b);
string replace_all(string s, std::string_view from, std::string_view to);
bool is_digits(std::string_view s);
long to_long(std::string_view s, long dflt = 0);
double to_double(std::string_view s, double dflt = 0);
string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
string read_file(const string& path);                         // "" when unreadable
string read_file_from(const string& path, int64_t from);      // what follows the first `from` bytes, "" when unreadable: a log read from where a start began
bool write_file(const string& path, std::string_view content); // atomic: tmp + rename
bool append_file(const string& path, std::string_view content);
bool file_exists(const string& path);
bool dir_exists(const string& path);
bool is_executable(const string& path);
int64_t file_size(const string& path);                        // -1 when absent
int64_t file_mtime(const string& path);                       // seconds, 0 when absent
bool mkdirs(const string& path);
bool remove_tree(const string& path);                         // rm -rf, false on error
int sweep_stale_tmp(const string& prefix);                    // /tmp/<prefix><pid> left by a killed run
bool copy_tree(const string& from, const string& to, string* why = nullptr);
string basename_of(string p);
string dirname_of(const string& p);
string abs_path(const string& p);
string mount_point_of(const string& p);                         // the mount the path is on: the nearest parent on another device is left out
string home_dir();
// The box: omnium built under MSYS2 runs on Windows (docs/windows.md), anything else is Linux
#if defined(__MSYS__) || defined(__CYGWIN__)
inline constexpr bool kHostMsys2 = true;
#define OMNIUM_FORK_PLAN 1                                      // the forks make an ExecPlan first (see ExecPlan below); elsewhere they are as they were
#else
inline constexpr bool kHostMsys2 = false;
#endif
// Fixes the Windows port found that are not about Windows (a page size that never reached the datadir, a root
// turned away that was filed as a bug, ...). Linux keeps its behaviour until the user decides; true here takes
// them there as well, and kTakeFixes is what each of them tests.
inline constexpr bool kFixesOnLinux = false;
inline constexpr bool kTakeFixes = kHostMsys2 || kFixesOnLinux;
// the user's Windows profile as an MSYS2 path, /c/Users/Roel; "" off MSYS2
string windows_home();
// A copy of an executable that nothing replaces, for MSYS2 ("" elsewhere, or when it cannot be made): its fork() starts the
// child by launching the parent's exe again by its path and copying the parent's memory into it, so a process whose exe was
// replaced on disk (a rebuild of omnium.exe) can never fork again ("child_copy: data read copy failed ... error 299",
// errno 11): a run stalled that way. The verbs that run for long start from this copy instead, in /tmp/omnium_exe.
string private_exe_copy(const string& exe);
// an absolute POSIX path as a native Windows program reads it, C:/msys64/dev/shm/x; "" when it is not one, and off MSYS2
string native_path(const string& posix);
// A server option for a native Windows server with its path made native. The MSYS2 runtime converts a plain
// --opt=/path of an argument itself when it starts a native program, but not --opt=FILE:/path, which then
// reaches the server as /dev/shm/... and is looked for on the wrong drive. A value without a path is kept.
string native_option(const string& opt);
// Whether two paths name one folder, the way a Windows server reports its datadir (C:\msys64\dev\shm\x\) and an MSYS2 path
// names it (/dev/shm/x): slashes and case do not count, nor a slash at the end. Off MSYS2 only the spelling is compared.
bool same_path(const string& a, const string& b);
// the person's own home: $HOME, and on Windows the profile, as the MSYS2 home is a folder of the
// shared MSYS2 install (a HOME that was set to something else is kept)
string user_home();
// Which HOME omnium runs with, MSYS2: started from a Windows shell that hands HOME over as the profile (/c/Users/Roel), omnium would
// write a settings file and shell files there beside the real ones of the MSYS2 home, and fall over on the defaults. A HOME that is
// the profile, has no settings file, while the MSYS2 home has one is a mistake of the environment: "" keeps HOME, else the MSYS2 home.
string msys2_home_fix(const string& home, const string& profile, const string& msys_home, bool home_has_conf, bool msys_has_conf);
void fix_msys2_home();                                        // sets HOME to the MSYS2 home in that case and says so; does nothing elsewhere
// the Jira token file when PAT_FILE names none: <home>/.omnium_jira_pat, or the file ~/jira reads
// when only that one exists
string default_pat_file(const string& home);
string self_exe();                                            // /proc/self/exe resolved
extern string g_exe_override;                                 // a run's own copy of the binary; "" = /proc/self/exe
string now_hms();                                             // HH:MM:SS local
string now_stamp();                                           // YYYY-MM-DD HH:MM:SS local
string date_ddmmyy();                                         // build-name date
int64_t now_s();
double now_ms();
string human_bytes(uint64_t b);
string human_secs(long s);                                    // 1d 02:03:04
uint64_t fnv1a(std::string_view s);
string hex64(uint64_t v);
string sh_quote(const string& s);
string tail_lines(const string& text, size_t n);
string cap_text(const string& text, size_t max_bytes);        // first + last half when longer
uint64_t dir_free_bytes(const string& path);
uint64_t dir_total_bytes(const string& path);
int dir_used_pct(const string& path);
bool core_on_data(int shm_used_pct);                           // true = this trial's datadir, and its core, go under DATA_DIR
string backup_issue_uid(const string& step, const string& message);   // BACKUP_ISSUE|<step>|<message, numbers as N>
string backup_failure_line(const string& out);                 // the line of a failed backup step's output that says what failed
string strip_exe_names(const string& text);                    // s/mariadbd.exe/mariadbd/ and the like (detect.cpp)
int ram_used_pct();
bool meminfo_parse(const string& text, uint64_t& total_kb, uint64_t& avail_kb);   // /proc/meminfo; MemFree stands in where MemAvailable is missing (Cygwin)
uint64_t ram_total_bytes();
uint64_t ram_available_bytes();
int cpu_threads();
double load_average();

struct CmdResult { int rc = -1; string out; };
// fork+exec, stdout and stderr captured; timeout_s 0 = none; rc -1 = could not run, 124 = timeout
CmdResult run_capture(const vector<string>& argv, int timeout_s = 0, const string& cwd = "",
                      const vector<string>& env_add = {});
CmdResult run_shell(const string& script, int timeout_s = 0, const string& cwd = "");
// the same with text on the child's stdin (a gdb command list)
CmdResult run_capture_in(const vector<string>& argv, const string& stdin_text, int timeout_s = 0, const string& cwd = "",
                         const vector<string>& env_add = {});
// Everything a child needs, made before the fork. Cygwin copies only the forking thread's stack into
// the child: what the child reads that lives on another thread's stack (a reference to the caller's
// std::string, say) is unmapped there, and the first read kills it with SIGSEGV. That ended every
// trial under MSYS2: the generator is spawned from a thread, and its directory was a string on the
// main thread's stack. So argv, environment, program, directory and log are copied into a plan,
// which lives on the forking thread's own stack, and the child touches nothing but the plan, its
// own locals, the heap and globals. (A child of a process with threads may call only
// async-signal-safe functions anyway, so it allocates nothing.) Every fork in omnium makes a plan
// first, and the child calls exec_plan_run(plan) once its descriptors are set.
struct ExecPlan {
  vector<char*> av, envp;     // null-terminated; they point into the strings the plan was made from
  string path;                // what is exec'd: argv[0], or where PATH finds it
  string cwd;                 // the child changes to it first; "" = stays where it is
  string log;                 // the child's stdout and stderr are appended to it; "" = left as they are
  bool own_env = false;       // envp is the environment to hand over; false = the process's own
};
ExecPlan exec_plan(const vector<string>& argv, const vector<string>& env_add = {},   // env_add: KEY=VALUE, set over the environment
                   const string& cwd = "", const string& log = "");
[[noreturn]] void exec_plan_run(const ExecPlan& p);           // execve; _exit(127) when that fails
string wait_status_text(int st);                              // a raw wait status in words: "exit 3", "killed by signal 11 (SIGSEGV)"

// ---------------------------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------------------------
void log_open(const string& path);                            // run log; stderr until then
void log_set_quiet(bool q);                                   // no stderr copy (TUI up)
void logline(const char* f, ...) __attribute__((format(printf, 1, 2)));
void logwarn(const char* f, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void die(const char* f, ...) __attribute__((format(printf, 1, 2)));
vector<string> log_recent(size_t n);                          // last n lines for the TUI

// ---------------------------------------------------------------------------------------------
// rng.cpp - xoshiro256++, the one source of randomness
// ---------------------------------------------------------------------------------------------
struct Xoshiro256pp {
  uint64_t s[4]{};
  void seed(uint64_t z);
  void seed_full();                                           // getrandom + clock + pid
  void jump();                                                // 2^128 draws ahead: a new stream
  uint64_t next();
  uint64_t below(uint64_t n);                                 // [0, n)
  long range(long lo, long hi);                               // [lo, hi]
  double unit();                                              // [0, 1)
  bool chance_pct(int pct);
  template <class T> void shuffle(vector<T>& v) {
    for (size_t i = v.size(); i > 1; i--) std::swap(v[i - 1], v[below(i)]);
  }
  string digits(int n);                                       // n decimal digits, first non-zero
};
Xoshiro256pp rng();                                           // a stream of its own, seeded from this process's stream; safe from any thread
void rng_seed_process(uint64_t seed);                         // a fixed seed for a repeatable run
Xoshiro256pp rng_stream(unsigned k);                          // stream k: k jumps from the seed
uint64_t rng_seed_used();

// ---------------------------------------------------------------------------------------------
// config.cpp - ~/.omnium.conf plus CLI KEY=VALUE overrides
// ---------------------------------------------------------------------------------------------
struct Config {
  string email;                       // one mail per new inbox item; empty = no mail
  string jira_url = "https://jira.mariadb.org";                 // the Jira the filing talks to
  string pat_file;                    // Jira PAT: ~/.omnium_jira_pat, else the one ~/jira reads
  int smtp_port = 25;                 // the port the mail goes to on the recipient's mail server
  int ram_cap_pct = 85;               // no new server above this RAM use
  int shm_cap_pct = 90;               // hard cap on /dev/shm use
  int shm_pause_pct = 97;             // pause the biggest trials first at this /dev/shm use
  int shm_stepdown_pct = 75;          // above this /dev/shm use a new trial runs on DATA_DIR, so no core is written to the tmpfs
  double data_floor_gb = 2;           // /data under this: stop saving trials
  double root_floor_gb = 1;           // / under this: no new builds
  int run_days = 7;                   // a run's length
  int trial_seconds = 20;             // the trial window (wall time)
  int multi_thread_pct = 10;          // trials with several client threads
  int backup_pct = 5;                 // trials with a mariabackup round trip
  int crash_recovery_pct = 5;         // trials that kill and recover the server
  bool auto_pipeline = true;          // a new UID goes to reduce and then report inside the run
  string core_dir = "auto";           // auto: tmpfs while it is below SHM_STEPDOWN_PCT, DATA_DIR above it; shm: always the tmpfs, the core moved out as soon as the server is gone; data: always DATA_DIR (about 6x slower on insert-heavy SQL)
  int core_max_gb = 40;               // a core larger than this is cut off, so one runaway trial cannot fill the disk; 0 = no limit
  double sql_size_factor = 1.5;       // SQL per trial = factor x recently executed statements
  int keep_per_uid = 3;               // trials kept per new UID
  int discovery_min_pct = 25;         // slots discovery never drops below
  int workers = 0;                    // 0 = sized from RAM and the flavour mix
  int gdb_parallel = 4;               // gdb runs at once
  int cores_parallel = 2;             // cores being written at once
  bool follow = true;                 // daily remote check of every branch under test
  string qa_dir;                      // the mariadb-qa checkout; empty = ~/mariadb-qa when present, else <repo>/mariadb-qa
  string test_dir = kHostMsys2 ? "/c/test" : "/test";   // MSYS2 names C:\test /c/test; its own /test is C:\msys64\test, where no build is
  string data_dir = "/data";
  string shm_dir = "/dev/shm";
  string editor;                      // kb edit, eb
  string infile;                      // INFILE; empty = the pquery tarball
  string perl;                        // PERL: the native Windows perl MTR runs with; empty = look for one (omnium mtr, Windows only)
  bool all_disk_sql = true;           // the all-disk source
};
extern Config g_cfg;
string config_path();
string repo_dir();                                            // the omnium checkout; OMNIUM_REPO when a copy of the binary runs
void config_load(bool write_defaults_when_missing);
bool config_set(const string& key, const string& value);      // false = unknown key, or a value the key does not take
bool config_get(const string& key, string& value);            // false = unknown key
string config_refusal(const string& key, const string& value); // what config_set's false means, in words
string config_dump();
vector<string> config_missing_keys();                         // keys the settings file does not carry yet
void config_write_keys(const vector<string>& names);          // these keys into the settings file, as in force now; its other lines stay
// a helper script: the copy omnium ships, else the mariadb-qa one
string script_path(const string& name);
// the dirs omnium writes into, made when they are missing (the queues, /data/NEWBUGS)
void ensure_dirs();
void ensure_om_alias();                                       // alias om in ~/.bashrc, written once
vector<std::pair<string, string>> config_keys_help();

// ---------------------------------------------------------------------------------------------
// paths - the files omnium shares with the framework (all under g_cfg.qa_dir or the fixed spots)
// ---------------------------------------------------------------------------------------------
struct Paths {
  string repo;              // the omnium checkout (dirname of the binary)
  string qa;                // the mariadb-qa checkout in use
  string human_queue;       // /test/omnium/HUMAN-queue
  string ai_queue;          // /test/omnium/AI-queue
  string builds_file;       // /test/omnium.builds
  string seen_file;         // /data/omnium.seen
  string known_bugs;        // known_bugs.strings
  string known_bugs_san;    // known_bugs.strings.SAN
  string bugs_dir;          // BUGS/
  string regex_scan, regex_filter, regex_lastline;
  string asan_filter, ubsan_filter, tsan_filter;
  string san_opt;           // omnium.san.opt
  string sql_filter;        // QA_DIR/filter.sql: the framework list, no second copy
  string adv_filter;        // filters/adv.filter
  string assignees;         // skills/_shared/default_assignees.md
  string aliases_file;      // ~/.omnium_aliases
  string history_file;      // ~/.omnium_history
};
extern Paths g_paths;
void paths_init();

// ---------------------------------------------------------------------------------------------
// proc.cpp - the process model: driver, roles, servers
// ---------------------------------------------------------------------------------------------
// A child of the driver: fd 3 in the child is the event pipe up, fd 4 the command pipe down.
struct Child {
  pid_t pid = -1;
  int ev_fd = -1;             // driver reads events here
  int cmd_fd = -1;            // driver writes commands here
  string role, tag, logfile;
  long id = 0;                // trial number, bug number, build index ...
  bool exited = false;
  int status = 0;             // waitpid status once exited
  string buf;                 // partial event line
  int64_t started = 0;
};
// spawn /proc/self/exe --role <role> <args>; own process group; PR_SET_PDEATHSIG
bool spawn_role(Child& c, const string& role, const vector<string>& args, const string& logfile,
                const string& cwd = "");
// spawn any program (server, builder, gdb); own process group when own_group
pid_t spawn_program(const vector<string>& argv, const string& logfile, const string& cwd,
                    bool own_group, const vector<string>& env_add = {}, bool die_with_us = true);
bool child_send(Child& c, const string& line);
// one event line without the newline; false on timeout or EOF (c.exited set on EOF+reaped)
bool child_read(Child& c, string& line, int timeout_ms);
int child_reap(Child& c, int timeout_ms);                     // exit code, -1 still running
void child_close(Child& c);
void kill_group(pid_t pid, int sig);
bool pid_alive(pid_t pid);
int wait_pid(pid_t pid, int timeout_ms);                      // exit status or -1 (still running)
string proc_cmdline(pid_t pid);
uint64_t proc_rss_bytes(pid_t pid);
long proc_cpu_ms(pid_t pid);                                   // user plus system CPU time the process has used, in ms; -1 when it cannot be read
vector<pid_t> proc_children(pid_t pid);
// role side
void role_init();                                             // takes fd 3/4, no-op outside a role
void role_emit(const string& event);                          // one line up to the driver
bool role_cmd(string& line, int timeout_ms);                  // one command line down, or false
bool role_stop_requested();                                   // a "stop" command arrived
void raise_fd_limit();
bool set_oom_score(pid_t pid, int adj);                      // raising needs no privilege, lowering below 0 needs root
void install_stop_signals(std::atomic<int>& counter);          // SIGINT/SIGTERM count into it

// ---------------------------------------------------------------------------------------------
// store.cpp - workdir /data/O<6 digits>, ledger, resume, status, omnium.seen
// ---------------------------------------------------------------------------------------------
struct Workdir {
  string id;                // O123456
  string dir;               // /data/O123456
  string rundir;            // /dev/shm/O123456
  string logfile;           // /data/O123456/O123456.log
  string ledger;            // /data/O123456/omnium.ledger
  string status_file;       // /data/O123456/status.txt
  string sock;              // /data/O123456/omnium.sock
  string lock;              // /data/O123456/omnium.pid
  string pids;              // /data/O123456/omnium.pids (servers and children, for resume)
  string build_dir;         // /data/O123456/build (build logs)
};
extern Workdir g_wd;
void release_lock();                                          // the run is over: the pid file goes
bool pid_is_live_omnium(pid_t p);                             // the pid of a lock file still holds it: alive, and an omnium
bool workdir_create();                                        // new id, dirs, lock
bool workdir_open(const string& id_or_path, bool take_lock);  // --resume, status, cli
bool workdir_is_omnium(const string& path);
string workdir_trial_dir(long trial);
string workdir_id_from_cwd();                                 // "" when the cwd is not inside one
void ledger_append(const string& kind, const string& text);   // "<epoch> <kind> <text>"
vector<string> ledger_read();
void status_write(const vector<std::pair<string, string>>& kv);
vector<std::pair<string, string>> status_read(const string& dir);
vector<std::pair<string, string>> status_read_file(const string& path);   // any KEY=VALUE file (run.conf)
void pids_add(pid_t pid, const string& kind, const string& note);
void pids_remove(pid_t pid);
vector<std::tuple<pid_t, string, string>> pids_read(const string& dir);
struct SeenEntry { string uid; int64_t first = 0, last = 0; string run; long count = 0; string outcome; };
std::optional<SeenEntry> seen_lookup(const string& uid);
void seen_record(const string& uid, const string& run, const string& outcome);

// ---------------------------------------------------------------------------------------------
// server.cpp - basedirs, the version banner, datadir templates, server instances, ports
// ---------------------------------------------------------------------------------------------
enum class Vendor { MariaDB, MySQL, Percona, Unknown };
enum class Flavour { Plain, UBASAN, MSAN, TSAN, VAL, GAL };
struct Basedir {
  string path;              // /test/MD180826-mariadb-13.1.0-linux-x86_64-opt
  string name;              // the last path component
  string bin;               // <path>/bin/mariadbd or bin/mysqld
  string client;            // bin/mariadb or bin/mysql
  string admin;             // bin/mariadb-admin or bin/mysqladmin
  string dump;              // bin/mariadb-dump or bin/mysqldump
  string backup;            // bin/mariadb-backup when present
  string init_tool;         // scripts/mariadb-install-db, scripts/mysql_install_db, or the server binary
  bool init_via_bin = false;// MySQL 5.7+: server --initialize-insecure
  Vendor vendor = Vendor::Unknown;
  bool es = false;          // MariaDB Enterprise
  Flavour flavour = Flavour::Plain;
  bool dbg = false;
  bool windows = false;     // a Windows build: MD120926-mariadb-13.1.1-windows-x86_64-opt
  string version;           // 13.1.0, 12.3.2-1, 8.0.36
  string series;            // 13.1, 12.3, 8.0
  string date;              // ddmmyy from the name, "" when the name has none
  string tag;               // MDEV-1234, MENT-1234, bb-13.1-xyz: a feature build; "" for a release build
  string git_rev;           // git_revision.txt or the binary
  string cmake_cmd;         // BUILD_CMD_CMAKE
  bool in_tree = false;     // a build dir with sql/mariadbd, no bin/
  string short_name() const;   // 13.1-dbg, 13.1-uba-opt, 13.1-msan-dbg, es-12.3-opt, MDEV-1234-dbg
  string vendor_str() const;   // CS, ES, MS, PS
  string flavour_str() const;  // "", UBASAN, MSAN, TSAN, VAL, GAL
  bool is_san() const { return flavour == Flavour::UBASAN || flavour == Flavour::MSAN || flavour == Flavour::TSAN; }
};
bool basedir_probe(const string& path, Basedir& b);            // false when no server binary
bool basedir_parse_name(const string& name, Basedir& b);       // the name grammar alone
vector<Basedir> basedirs_scan(const string& test_dir);          // every basedir under /test, by name
string basedir_banner_title(const Basedir& b);
string basedir_test_dir(const Basedir& b);                     // <path>/mariadb-test, else <path>/mysql-test, else ""
// the server binary and its ldd libraries copied into a directory, as ldd_files.sh does
bool ldd_copy(const string& binary, const string& dir, const string& core, vector<string>* copied, string* err);                 // CS 13.1.0 <rev> (Optimized, Clang 22.1.8) Build 18/08/26
string basedir_banner(const Basedir& b);                       // {noformat:title=...}\n\n{noformat}
string basedir_source_rev(const Basedir& b, bool files_only = false);  // files_only: no strings(1) pass
vector<string> san_env_for(const Basedir& b);                  // ASAN_OPTIONS=... lines from omnium.san.opt
string vendor_options(const Basedir& b);                         // what a MySQL or Percona server needs on top
bool log_aborted(const string& log);                             // the server gave up at startup
bool server_log_ready(const string& log);                        // "ready for connections": the server listens (a bind that failed never prints it)
// the server could not bind its port because something holds it: "Address already in use" on Linux, error 10048 "Only one
// usage of each socket address" on Windows. The trial is dropped, as nothing ran on a server that never had its port.
bool port_clash_in_log(const string& log);
string mysafe_options(const Basedir& b);                       // the MYSAFE block, flavour adjusted
int version_cmp(const string& a, const string& b);             // 10.11.19 vs 11.4.13
bool version_at_least(const string& v, const string& floor);

// one datadir template per basedir + MYINIT, made once, under <workdir>/templates
string template_for(const Basedir& b, const string& myinit, const string& templates_root);
string myinit_from(const string& options);                     // the options that shape a datadir when it is made (the page size), out of a server option string
// the install-db command line: the Linux script's options, or the Windows tool's own set
vector<string> install_db_argv(const Basedir& b, const string& datadir, const string& tmpdir, const string& myinit);

int port_pick();                                               // a free TCP port in [13001, 65000]; MSYS2: [13001, 49151], below the ports Windows gives its clients
// MSYS2: whether the server's own bind on the port would work. mysqld binds the wildcard address and sets no SO_REUSEADDR, which is the bind
// that a listening server and an open connection's source port both refuse; a bind on 127.0.0.1 is told "free" by both.
bool port_free(int port);

// how a client reaches a server: the unix socket, or TCP on 127.0.0.1 where there is none (a
// Windows server, or OMNIUM_TCP=1 to run the same path on Linux)
struct Endpoint { string sock; int port = 0; bool tcp = false; };
vector<string> endpoint_args(const Endpoint& e);               // -S<sock>, or -h127.0.0.1 -P<port> --protocol=tcp --skip-ssl
bool endpoint_tcp(const Basedir& b);                           // a Windows build, or OMNIUM_TCP=1

struct Instance {
  const Basedir* bd = nullptr;
  string root;              // the trial dir on tmpfs
  string datadir, tmpdir, sock, errlog, pidfile, logdir;
  int port = 0;
  bool tcp = false;         // clients use 127.0.0.1:port, not the socket: set from the basedir, or OMNIUM_TCP=1
  pid_t pid = -1;
  bool start_failed = false;
  bool detached = false;    // the server outlives omnium (omnium fresh); a trial server never does
  string start_note;
  int64_t log_from = 0;     // the size of the error log when this start began: a restart on the same log has the first start's lines before it
  vector<string> extra;     // MYEXTRA and friends, one option per entry
  string myinit;
  bool stopping = false;    // omnium asked it to stop (shutdown, kill_hard): how it ends is no finding
  int exit_status = -1;     // the raw wait status, once alive() has seen it end on its own
  uintptr_t win_handle = 0;          // MSYS2: a handle to the native server process, opened once it is up (winproc.h)
  uint32_t win_status = 0;           // its exit status, the NTSTATUS of an exception, once alive() has seen it end on its own
  bool win_status_known = false;
  void set_paths(const string& trial_root, const string& datadir_override = "");
  Endpoint endpoint() const { return {sock, port, tcp}; }
  vector<string> argv() const;
  bool alive();
  bool start_fresh(const string& tpl, int timeout_s);
  bool start_only(int timeout_s);
  bool wait_ready(int timeout_s);                              // connect probe on the socket
  bool shutdown(int timeout_s, string* note = nullptr);        // clean; false = did not stop in time, note says what the admin client said
  void kill_hard();
  bool has_core() const;
  bool has_dump() const;      // Windows: the minidump the server leaves in its datadir when it crashes
  string core_path() const;
  // Windows: the server ended on its own with a status that is no clean exit, and left no minidump. A failed /GS stack
  // cookie check or a __fastfail kills a release server with no banner in the log and no dump; Cygwin reports that
  // death, an NTSTATUS it has no signal for (0xC0000409), as exit status 127.
  bool silent_death() const;
  // CRASH_NO_LOG|exit code 0xC00000FD with the NTSTATUS of the native process, or CRASH_NO_LOG|exit status N when it could not be read
  string silent_death_uid() const;
  string silent_death_note() const;                             // in words: a stack overflow, a failed stack cookie check, ...
};
// the start/stop/cl helpers of a saved trial, and the gdb one when it has a core
void write_helpers(const string& tdir, const Basedir& b, const Instance& inst, const string& myextra, bool core);
bool root_turned_away(const string& text);                    // a start or backup step that failed because the trial's SQL locked root out: a configured state, no finding
string silent_death_statement(const string& tdir);            // the last statement of a trial's first thread as its first two words: the per-UID cap keeps each cause of a frameless UID
bool backup_is_encrypted(const vector<string>& myextra);      // the server ran with the key plugin: its backup is prepared with its own backup-my.cnf

// ---------------------------------------------------------------------------------------------
// registry.cpp - /test/omnium.builds, the known-bugs lists, BUGS/ files, the regex lists
// ---------------------------------------------------------------------------------------------
struct BuildEntry {
  string name;              // basedir name under /test (a path when it holds a /)
  string vendor;            // CS ES MS PS
  string version, series;   // 13.1.0, 13.1
  string flavour;           // plain UBASAN MSAN TSAN VAL GAL
  string type;              // opt dbg
  string date;              // ddmmyy
  bool test = false;        // omnium with no version named runs trials on it
  bool report = false;      // a bug is checked on it for the report matrix
  string origin;            // scan omnium hand
  string commit;            // git revision or -
  string path() const;
  string key() const;       // vendor series flavour type: a newer build of the same key replaces the older
};
struct Registry { vector<BuildEntry> entries; vector<string> notices; };
bool entry_from_basedir(const Basedir& b, BuildEntry& e);      // false when the name does not parse
bool entry_newer(const BuildEntry& a, const BuildEntry& b);
bool registry_load(Registry& r, const string& path = "");      // false when the file is missing
bool registry_save(const Registry& r, const string& path = "");
string registry_format(const Registry& r);
void registry_sync(Registry& r, const vector<Basedir>& scanned, bool first_import, bool check_disk = true);
Registry registry_current();                                   // load, scan /test, sync, save when changed
// "" when TEST_DIR holds a build; else that it holds none, and on MSYS2 the /c/test that does (a settings
// file from before /c/test became the default still says /test, which there is C:\msys64\test)
string test_dir_empty_note();
vector<string> registry_names(const Registry& r, bool test_set);
// The registered Windows builds that have no mariadb-test, by name: omnium mtr cannot run a testcase on
// them. And what to do about it. The builds of C:\test come from build.ps1, which passes
// -DBUILD_CONFIG=mysql_release, and that leaves INSTALL_MYSQLTESTDIR empty on Windows.
vector<string> windows_builds_without_mtr(const Registry& r);
string mtr_suite_fix();
const BuildEntry* registry_find(const Registry& r, const string& name_or_path);

enum class KbVerdict { NotFound, Partial, Known, FixedOnly, KnownAndFixed };
struct KbMatch { bool san = false; vector<string> exact, partial; string frame; int frame_pos = 1; };
bool kb_uid_is_san(const string& uid);                         // "SAN" anywhere, as tt decides
string kb_file_for(const string& uid);
KbMatch kb_search(const string& uid);                          // grep -Fi on the whole UID and on the frame
bool kb_line_has(const string& line, const string& uid);       // grep -Fi of a UID in a known-bugs line, __null and 0 read as one
KbVerdict kb_verdict(const KbMatch& m);
vector<string> kb_keys(const vector<string>& lines);           // the MDEV-n / MENT-n keys in kb lines
string kb_verdict_text(const string& uid, const KbMatch& m);   // tt's String Scan block, same wording
vector<string> kb_jira_urls(const string& uid);                // tt's search URLs
string kb_format_line(const string& uid, const string& key);   // UID padded, then ## MDEV-n
string uri_escape(const string& s);
bool kb_add_to(const string& path, const string& uid, const string& key, string* err);
bool kb_add(const string& uid, const string& key, bool force_san, string* err);
int kb_fixed_in(const string& path, const string& key);        // lines moved; -1 on a write error
int kb_fixed(const string& key, vector<string>* also_mentioned);
string bug_key_normalize(const string& in);                    // 12345, mdev-12345 -> MDEV-12345
string bugs_file_for(const string& key);                       // ~/mariadb-qa/BUGS/MDEV-n.sql
bool bugs_write(const string& key, const string& options, const string& sql, string* err);
vector<string> regex_list_read(const string& path);            // the | alternatives of a one-line list
bool regex_list_add(const string& path, const string& alt);    // false when present already
int open_in_editor(const string& path);

// ---------------------------------------------------------------------------------------------
// build.cpp - omnium build: clone, the cmake flag table, ninja, install into /test/<name>, list it
// ---------------------------------------------------------------------------------------------
struct SourceInfo {
  string dir;               // /test/13.1
  Vendor vendor = Vendor::Unknown;
  bool es = false;
  int major = 0, minor = 0, patch = 0;
  string extra;             // -1 for ES, -28 for Percona, "" otherwise
  string version;           // 13.1.0, 12.3.2-1, 8.0.36
  string series;            // 13.1
  string product;           // mariadb, mysql, percona-server
  string git_rev, branch;
  bool has_rocksdb = false;
  bool maturity = false;    // VERSION has SERVER_MATURITY: a MariaDB tree, gets the sigaction patch
};
struct BuildJob { string source, flavour, tag; int jobs = 0; bool tar = false, rebuild = false; };
struct BuildResult { bool ok = false, listed = false; string name, basedir, log, tar, note, version_line; double seconds = 0; };
bool source_info(const string& dir, SourceInfo& si, string* err);
string tag_from_branch(const string& branch);                 // "" for a version branch or main
bool flavour_valid(const string& fl);                          // opt dbg ubasan-opt ubasan-dbg msan-opt msan-dbg
string build_name(const SourceInfo& si, const string& flavour, const string& tag, const string& date);
vector<string> cmake_command(const SourceInfo& si, const string& flavour, const string& prefix, string* err);
int build_one(const BuildJob& job, BuildResult& res);          // 0 = built; the rc says which step failed
bool clone_source(const string& what, Vendor vendor, bool es, string& dir, string* err);
bool patch1_tree(const string& feature_dir, string& tree, string& note, string* err);  // tree "" = base build serves

// ---------------------------------------------------------------------------------------------
// sources.cpp - the SQL of a trial: four sources, filters, transforms, the area table, seeds
// ---------------------------------------------------------------------------------------------
struct RxSet {                       // PCRE2 patterns joined as one case-insensitive alternation
  RxSet() = default;
  ~RxSet();
  RxSet(const RxSet&) = delete;
  RxSet& operator=(const RxSet&) = delete;
  bool compile(const vector<string>& patterns, string* err);
  bool hit(std::string_view line);
  size_t count = 0;
  void* re = nullptr;
  void* md = nullptr;
  std::mutex mtx;
};
string bre_to_pcre(const string& bre);                         // grep basic regex to PCRE
vector<string> read_pattern_file(const string& path);          // one pattern per line, # and blanks skipped

struct Area {
  string name;
  vector<string> myextra_choices;   // one option set picked per trial
  string myinit;
  string interleave;                // SQL block, newline separated, inserted every interleave_lines lines
  int interleave_lines = 0;
  string engine_swap;               // engine names in the SQL become this one
  int engine_swap_pct = 50;
  bool encryption = false;          // file-key-management plugin with per-trial keys
  string encryption_items;          // the --*encrypt* options
  bool table_swap_all = false, table_swap_create = false;
  string preload_file;              // SQL run first, under ~/mariadb-qa (spiderpreload.sql)
  string needs;                     // rocksdb | spider | federated | ""
  bool mode = false;                // a trial mode, not an option set
  int weight = 0;                   // share of trials; 0 = never picked as an area
  string note;
};
const vector<Area>& areas_all();
bool area_available(const Area& a, const Basedir& b, string* why);
vector<const Area*> areas_available(const Basedir& b, const vector<string>& only);
const Area* area_pick(const vector<const Area*>& v, Xoshiro256pp& r);
const Area* area_by_name(const string& name);

string sql_line_cleanup(string l);                             // file-name prefixes and ;#NOERROR markers off
void sql_engine_swap(vector<string>& lines, const string& engine, int pct);
void sql_interleave(vector<string>& lines, const string& block, int every);
void sql_swap_create_table_names(vector<string>& lines);
void sql_swap_all_table_names(vector<string>& lines);

struct TrialSql {
  string area, sql_path, preload_path, myinit;
  vector<string> myextra, random_options;
  size_t lines = 0, gen_lines = 0, rev_lines = 0, infile_lines = 0, disk_lines = 0, filtered = 0;
  uint64_t seed_gen = 0, seed_rev = 0, seed_shuffle = 0, seed_opt = 0;
  bool encryption = false;
  string seed_text() const;                                    // the SEED file
};
struct SqlSources {                  // one per process: a trial's own, and the driver's for the size
  string work;                      // tmpfs dir for the run's SQL files
  string infile;                    // the extracted INFILE
  vector<uint64_t> infile_offsets;  // line starts, so a random line is one pread
  int infile_fd = -1;
  size_t infile_lines = 0;
  vector<string> disk_p, disk_a;    // the all-disk index: home/SQL/TESTCASES set and whole-disk set
  vector<string> disk_pool;         // the current all-disk pool
  int disk_pool_age = 0;
  int64_t disk_pool_mtime = 0;      // the shared pool file's mtime when it was last read
  RxSet sql_filter, adv_filter;
  std::deque<size_t> executed;      // statements the last trials executed, for the auto-size
  size_t target = 0;                // set: the SQL lines to assemble, whatever executed says
  std::mutex mtx;
  bool use_gen = true, use_rev = true, use_infile = true, use_disk = true;
  string revgen_yacc;
  string federated_conn = "SOCKET '../socket.sock'";   // the OPTIONS the FederatedX SERVER reaches this trial's server with; HOST/PORT on a TCP server
  vector<string> notes;
  ~SqlSources() { if (infile_fd >= 0) close(infile_fd); }
};
bool sources_init(SqlSources& s, const Basedir& b, const string& work, string* err);
void sources_note_executed(SqlSources& s, size_t n);
size_t sources_target_lines(SqlSources& s);                    // target when set, else sql_size_factor x the recent executed average
bool sources_assemble(SqlSources& s, const Basedir& b, const Area& a, Xoshiro256pp& r, const string& trial_dir, bool multi_thread, TrialSql& t, string* err);

// ---------------------------------------------------------------------------------------------
// detect.cpp - the UniqueID chain (new_text_string.sh and friends, byte for byte)
// ---------------------------------------------------------------------------------------------
struct UidOptions { string binary; string core; bool wait_core = true; bool frames_only = false; };   // core: when it is not under the dir
struct UidResult {
  string uid, err;              // err = the script's Assert text where it would exit 1
  string loc, binary, core;     // what the chain worked on
  string gdb_first_bt, gdb_second_bt;
  vector<string> logs;
  bool san = false;
};
bool uid_for_dir(const string& loc, UidResult& r, const UidOptions& o);   // new_text_string.sh on a trial or basedir dir
string uid_raw_gdb(const string& file);                        // RAW_GDB_UID|... from a pasted gdb trace
string uid_san(const vector<string>& logs, string* err);       // san_text_string.sh
string uid_fallback(const string& log, string* err);           // fallback_text_string.sh: FALLBACK|...
string uid_other_strings(const vector<string>& logs);          // the no-core error-log tiers
string assert_from_logs(const vector<string>& logs);           // the assertion text as the chain reads it
bool els_run(const string& mode, const vector<string>& logs, bool exclude_assert, string& out, string* err);  // error_log_scan.sh
vector<string> capped_logs(const vector<string>& logs);        // capped_error_log.sh: over 10 MB = first + last 5 MB
bool gdb_backtraces(const string& binary, const string& core, string& out1, string& out2, string* err);
string frames_from_backtraces(const string& out1, const string& out2, string* sig, bool* no_frames_at_all);
string frames_from_windows_log(const vector<string>& logs, string* sig);   // a Windows server's own backtrace in the log: frame1|..|frame4
string windows_signal(const string& code);                     // 0xc0000005 -> SIGSEGV, 0x80000003 -> SIGABRT, ...
string binary_for_dir(const string& loc);                      // the server binary the chain would use
int san_drop_known(const string& trial_dir, const string& log_rel);   // drop_one_or_more_san_from_log.sh; blocks removed
string stack_text(const string& dir, const string& title, string* err);   // stack.sh: the noformat block for a report
bool text_has_san_marker(const string& text);                  // a sanitizer report in the text

// ---------------------------------------------------------------------------------------------
// matrix.cpp - the Bug Detection Matrix
// ---------------------------------------------------------------------------------------------
struct MatrixRow {
  Basedir b;
  string uid;               // the UID observed, or "No bug found" / "No result (...)"
  string stack;             // stack.sh block when the build showed the bug
  string errlog;            // the error log of that replay, for the SAN and assert blocks
  string note;              // start failure, hang, shutdown timeout
  bool crashed = false, san = false;
  unsigned long long performed = 0, failed = 0;
};
struct MatrixResult { vector<MatrixRow> rows; string options; };

// ---------------------------------------------------------------------------------------------
// mtr.cpp - the MTR form of a testcase and its verification
// ---------------------------------------------------------------------------------------------
struct MtrTest {
  string test, opt;                 // the .test text; the -master.opt content ("" when none)
  string gate_kind;                 // what makes the test fail while the bug is there
  vector<string> notes;
  bool replayed = false, crash_at_statement = false, shutdown_crash = false, debug_only = false;
};
struct MtrVerdict {
  string build, verdict, reason, output;
  bool pass = false, fail = false;
  bool server_died = false, query_error = false, log_error = false;
  bool gate = false;                // failed the way the bug shows
};
// mail.cpp - one mail per new inbox item when EMAIL is set (direct to the recipient's MX)
bool mail_mx(const string& domain, vector<string>& hosts, string* err);
bool mx_parse(const unsigned char* answer, int len, vector<string>& hosts);   // the MX hosts of a DNS answer, best first
bool mail_send(const string& to, const string& subject, const string& body, string* err, bool dry_run = false);
bool mtr_make(const string& sql_text, const string& options, const Basedir* b, const string& uid, MtrTest& t, string* err);
bool mtr_verify(const Basedir& b, MtrTest& t, const string& tag, MtrVerdict& v, string* err);
// The perl MTR runs with on Windows: the PERL setting, else the usual homes of Strawberry Perl, else the PATH, and each one is
// asked what $^O is. MSYS2's perl and Git's say msys, MTR then takes the box for Cygwin and demands --cygwin-subshell-fix=do,
// which puts a wrapper over /bin/sh, the shell omnium itself runs on. "" and the reason when there is no native perl.
string native_perl_find(string* why);
// the runner's output, read into a verdict; suppress collects one mtr.add_suppression line per
// flagged log line the testcase itself makes
void mtr_parse_verdict(const string& out, int rc, const string& test_name, bool crash_at_statement, MtrVerdict& v, vector<string>* suppress);
// report.cpp: the testcase of a saved trial: the deepest reduced file, else the raw trace
struct TrialTestcase {
  string file, sql, options, uid, basedir;   // options: the replay header minus the MYSAFE block
  bool reduced = true;
};
bool trial_testcase(const string& workdir, long trial, TrialTestcase& tc, string* err);
bool basedir_from_arg(const string& arg, Basedir& b, string* err);   // a name under /test, a path, or the cwd
bool matrix_builds(const vector<string>& names, vector<Basedir>& out, string* err);   // empty names = the report set
bool matrix_run(const string& sql_file, const vector<Basedir>& builds, const string& options, int slots, MatrixResult& out, string* err);
string matrix_format(const MatrixResult& m);
bool row_less(const MatrixRow& a, const MatrixRow& b);           // the row order of the matrix: vendor, version, flavour, dbg first
bool matrix_row_shows_bug(const MatrixRow& r);                   // a UID, not "No bug found" or "No result (...)"
// The testcase a report carries. testcase_prettify.sh (mariadb-qa) rewrites the SQL as plain text, and some of what it does changes what
// the SQL means: a keyword that holds other keywords is half lower-cased (MINUTE_MICROSECOND), a space goes between a function and its "("
// (NEXTVAL (x), which is no call without IGNORE_SPACE) and into string literals. A testcase it changed can lose the crash, and then the report
// says no build shows the bug. `m` is the matrix of the prettified testcase; when the build that crashed in the trial (own_path, "" for any
// build) shows no bug there, run_reduced replays the reduced testcase, and when that shows it `m` becomes that matrix and true comes back.
bool report_prefer_reduced(MatrixResult& m, const string& own_path, const std::function<bool(MatrixResult&)>& run_reduced);

// ---------------------------------------------------------------------------------------------
// jira.cpp - Jira REST (libcurl, the PAT ~/jira uses) and the small JSON it needs
// ---------------------------------------------------------------------------------------------
struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  string str;                                     // String and Number
  vector<JsonValue> arr;
  vector<std::pair<string, JsonValue>> obj;
  const JsonValue* get(const string& key) const;
  string str_at(const string& dotted_path) const; // "fields.status.name"
};
string json_escape(const string& s);              // quoted
bool json_parse(const string& text, JsonValue& v);
struct JiraHit { string key, status, resolution, summary; };
struct JiraFields {
  string project = "MDEV", issuetype = "Bug", summary, description, priority, assignee, security_id;
  vector<string> affects, fix, components, labels, es_versions;
};
string jira_base();                                          // the Jira base URL, JIRA_URL in the config
string jira_pat();
bool jira_get(const string& url, string& out, long* http, string* err);
bool jira_post(const string& url, const string& json, string& out, long* http, string* err);
bool jira_put(const string& url, const string& json, string& out, long* http, string* err);
bool jira_search_url(const string& tt_url, vector<JiraHit>& hits, string* err);
bool jira_whoami(string& name, string* err);
string jira_create_payload(const JiraFields& f);
bool jira_create(const JiraFields& f, string& key, string* err);
bool jira_comment(const string& key, const string& text, string* err);
bool jira_link(const string& key, const string& other, const string& type, string* err);
bool jira_project_names(const string& project, const string& what, vector<string>& names, string* err);   // versions | components

// ---------------------------------------------------------------------------------------------
// report.cpp - the report file: a KEY: value header, a ----- line, the Jira body
// ---------------------------------------------------------------------------------------------
vector<std::pair<string, string>> report_header(const string& text, string* body);
string report_cap_frames(const string& stack, int cap);   // the stack blocks of a report, cut to cap frames
string report_san_label(const string& errlog);            // the sanitizer class of a SUMMARY line, "" when there is none
vector<string> guess_components(const string& uid, const vector<string>& sql);   // Jira components from the frames; the first names the owner
string report_field(const vector<std::pair<string, string>>& kv, const string& key);
bool report_to_fields(const string& text, JiraFields& f, string* err);
// Fix Version from the affected branches, oldest first: with several affected the newest is left
// out, it gets the fix by up-merge; a single affected branch is the Fix Version itself
vector<string> fix_versions(const vector<string>& affects);

// ---------------------------------------------------------------------------------------------
// client.cpp - the in-process client with pquery semantics
// ---------------------------------------------------------------------------------------------
struct ClientParams {
  Endpoint ep;
  string user = "root", db = "test";
  string logdir, name = "default.node.tld";   // <logdir>/<name>_thread-N.sql, <name>_general.log
  int threads = 1;
  unsigned long queries_per_thread = 5000000;
  bool shuffle = true;
  bool log_all = true, log_failed = true, log_duration = true, log_stats = false, log_numbers = false, log_client_output = false;
  uint64_t seed = 0;
  int crash_last_lines = 30;
  int max_con_failures = 250;
};
struct ClientResult {
  unsigned long long performed = 0, failed = 0;
  int lost_connection = 0;      // threads that hit 2013
  int consecutive_stop = 0;     // threads that stopped on the 250 rule
  int connect_failed = 0;       // threads that never connected
  long gone_away = 0;           // errno 2006 answers, over all threads
  vector<uint64_t> thread_seeds;
};
// runs the SQL file until every thread is done or stop is set; false only when nothing could run
bool client_run(const ClientParams& p, const string& sql_path, std::atomic<bool>& stop, ClientResult& res, string* err);
void client_kill_connections(const ClientParams& p);           // KILL CONNECTION on every client thread still connected
string node_summary_line(unsigned long long failed, unsigned long long performed);   // the NODE SUMMARY line, one wording for both logs

// ---------------------------------------------------------------------------------------------
// sources.cpp - the shared per-run files the workers read (the driver prepares and refreshes them)
// ---------------------------------------------------------------------------------------------
bool sources_shared_prepare(const string& work, string* err);   // INFILE extracted, the disk index and its first pool
void sources_shared_refresh_pool(const string& work, Xoshiro256pp& r);   // a new disk pool, every 45 trials

// util additions
string stamp_of(int64_t epoch);                                // YYYY-MM-DD HH:MM:SS local
bool move_tree(const string& from, const string& to, string* why = nullptr);   // rename, or copy + remove across filesystems
bool copy_file(const string& from, const string& to);
