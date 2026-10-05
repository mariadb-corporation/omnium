// Created by Roel Van de Paar, MariaDB
// trial.cpp - one trial, as the child role `--role trial`: assemble the SQL, start a fresh server
// from the datadir template, run the client for the trial window, shut down, detect, decide, save or
// drop. The trial dir keeps the framework's layout (data/, log/master.err, MYBUG, MYEXTRA, MYINIT,
// MYSAFE, SEED, the traces, start/stop/cl), so ~/t, ~/tt, ~/stack and the reducer work on it as is.
#include "verbs.h"
#include <sys/resource.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "connect.h"

namespace {
const int SHUTDOWN_SECONDS = 25;                 // clean shutdown, then kill (plan Q225; pquery-run.sh used 35)
const int CRASH_KILL_BEFORE_END_SEC = 10;        // crash-recovery trials: kill -9 this long before the end
const int BACKUP_START_BEFORE_END_SEC = 10;      // backup trials: the full backup starts this long before the end, against the busy server
const int BACKUP_STEP_TIMEOUT_S = 300;           // one mariadb-backup step; longer is a hang, and kept as a finding
const long MULTI_THREAD_LINES = 30000;           // multi-thread trials run this many lines (MULTI_THREADED_TESTC_LINES)
const int64_t LARGE_LOG_BYTES = 5242880;         // an error log over this is kept on its own
const long GONE_AWAY_SAVE = 200;                 // "MySQL server has gone away" answers before the trial is kept

string opt(const Args& a, const char* key, const string& dflt = "") {
  for (size_t i = 0; i + 1 < a.size(); i++) if (a[i] == key) return a[i + 1];
  return dflt;
}
bool flag(const Args& a, const char* key) {
  for (auto& x : a) if (x == key) return true;
  return false;
}
string sh(const string& s) { return sh_quote(s); }
string quote_id(const string& s) {
  string q = "`";
  for (char c : s) { if (c == '`') q += '`'; q += c; }
  return q + "`";
}
// CHECKSUM TABLE of every user table a backup carries over: not MEMORY, which any restart empties, and
// not an engine whose data lives outside the datadir. A table that gives no checksum says why instead.
bool table_checksums(const Endpoint& ep, std::map<string, string>& out, std::map<string, string>& engine, string* err) {
  MYSQL* m = mysql_init(nullptr);
  unsigned t = 30;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
  if (!endpoint_connect(m, ep, "root", nullptr, 0)) { *err = mysql_error(m); mysql_close(m); return false; }
  mysql_query(m, "SET GLOBAL event_scheduler=OFF");            // nothing may change the data between the two sides
  mysql_query(m, "SET SESSION wait_timeout=2147483, max_statement_time=0");   // as mariadb-backup sets for itself
  const char* q = "SELECT table_schema, table_name, engine FROM information_schema.tables WHERE table_type='BASE TABLE'"
                  " AND table_schema NOT IN ('mysql','information_schema','performance_schema','sys')"
                  " AND engine NOT IN ('MEMORY','BLACKHOLE','CONNECT','SPIDER','FEDERATED','MRG_MyISAM','PERFORMANCE_SCHEMA')";
  if (mysql_query(m, q) != 0) { *err = mysql_error(m); mysql_close(m); return false; }
  vector<string> names;
  if (MYSQL_RES* res = mysql_store_result(m)) {
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
      if (!row[0] || !row[1]) continue;
      string n = quote_id(row[0]) + "." + quote_id(row[1]);
      names.push_back(n);
      engine[n] = row[2] ? row[2] : "";
    }
    mysql_free_result(res);
  }
  for (auto& n : names) {
    string sum = "no checksum (NULL)";
    if (mysql_query(m, ("CHECKSUM TABLE " + n).c_str()) != 0) sum = string("no checksum (") + mysql_error(m) + ")";
    else if (MYSQL_RES* res = mysql_store_result(m)) {
      MYSQL_ROW row = mysql_fetch_row(res);
      if (row && row[1]) sum = row[1];
      mysql_free_result(res);
    }
    out[n] = sum;
  }
  mysql_close(m);
  return true;
}

bool log_has_san_marker(const string& log) { return text_has_san_marker(read_file(log)); }
}  // namespace
// the SQL of a trial can lock root out: a changed or expired password, a revoked privilege, a broken
// init_connect, every connection taken. That is a configured state, not a backup finding, so the round
// trip of such a trial is skipped.
bool root_turned_away(const string& text) {
  return icontains(text, "Access denied") || icontains(text, "Failed to connect") || icontains(text, "SET PASSWORD") ||
         icontains(text, "init_connect") || icontains(text, "Too many connections") ||
         (kTakeFixes && icontains(text, "is not allowed to connect"));                 // error 1130: the user's host was taken away
}
// An encrypted backup (the server ran with the file key management plugin) is prepared with the backup's own
// backup-my.cnf, which holds the key plugin and its files: --no-defaults leaves them out, and srv_start() then
// ends 11. The backup tool does not take the plugin options on its command line ("unknown variable").
bool backup_is_encrypted(const vector<string>& myextra) {
  for (auto& o : myextra) {
    string k = lower(o.substr(0, o.find('=')));
    for (char& c : k) if (c == '_') c = '-';
    if (starts_with(k, "--file-key-management") || (k == "--plugin-load-add" && icontains(o, "file_key_management"))) return true;
  }
  return false;
}
namespace {
bool uid_known(const string& uid) {
  if (uid.empty()) return false;
  KbVerdict v = kb_verdict(kb_search(uid));
  return v == KbVerdict::Known || v == KbVerdict::KnownAndFixed;
}
long saved_with_uid(const string& workdir, const string& uid, const string& except_dir) {
  long n = 0;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(workdir, ec)) {
    if (!e.is_directory(ec) || e.path().string() == except_dir) continue;
    string mb = e.path().string() + "/MYBUG";
    if (!file_exists(mb)) continue;
    string u = read_file(mb);
    size_t nl = u.find('\n');
    if (nl != string::npos) u = u.substr(0, nl);
    if (u == uid) n++;
  }
  return n;
}
// how mariadb-backup reaches the server: the socket, or the host and port of a TCP server
vector<string> backup_conn(const Instance& inst) {
  if (inst.tcp) return {"--host=127.0.0.1", "--port=" + std::to_string(inst.port)};
  return {"--socket=" + inst.sock};
}
vector<string> with_conn(vector<string> head, const Instance& inst, const vector<string>& tail) {
  for (auto& x : backup_conn(inst)) head.push_back(x);
  for (auto& x : tail) head.push_back(x);
  return head;
}
}  // namespace
// the handy start/stop/cl helpers of a saved trial, plus the gdb note when a core is there
void write_helpers(const string& tdir, const Basedir& b, const Instance& inst, const string& myextra_in, bool core) {
  string mysafe = mysafe_options(b), myextra = myextra_in;
  if (b.windows) {                                              // as Instance::argv() does: a FILE:/path reaches a native server only when it is native
    vector<string> w;
    for (auto& o : split_ws(myextra_in)) w.push_back(native_option(o));
    myextra = join(w, " ");
  }
  string conn = inst.tcp ? " " + join(endpoint_args(inst.endpoint()), " ") : " -S$PWD/socket.sock";
  string start = "#!/bin/bash\n# Starts this trial's server on its saved datadir\ncd \"$(dirname \"$(readlink -f \"$0\")\")\"\nrm -f socket.sock pid.pid\n" +
                 sh(b.bin) + " " + mysafe + " " + myextra + " --basedir=" + sh(b.path) +
                 " --datadir=$PWD/data --tmpdir=$PWD/tmp --core-file --port=" + std::to_string(inst.port) +
                 " --pid_file=$PWD/pid.pid" + (b.windows ? "" : " --socket=$PWD/socket.sock") + " --log-output=none --log-error=$PWD/log/master.err" + (vendor_options(b).empty() ? "" : " " + vendor_options(b)) + " >> log/master.err 2>&1 &\n"
                 "for i in $(seq 1 60); do sleep 1; " + sh(b.admin) + " -uroot" + conn + " ping >/dev/null 2>&1 && break; done\n";
  string stop = "#!/bin/bash\ncd \"$(dirname \"$(readlink -f \"$0\")\")\"\n" + sh(b.admin) + " -uroot" + conn + " shutdown\n";
  string cl = "#!/bin/bash\ncd \"$(dirname \"$(readlink -f \"$0\")\")\"\n" + sh(b.client) + " -uroot" + conn + " test\n";
  write_file(tdir + "/start", start);
  write_file(tdir + "/stop", stop);
  write_file(tdir + "/cl", cl);
  chmod((tdir + "/start").c_str(), 0755);
  chmod((tdir + "/stop").c_str(), 0755);
  chmod((tdir + "/cl").c_str(), 0755);
  if (core) {
    // the copy of the server binary in the workdir comes first: a trial read on another box has it
    string bin_name = basename_of(b.bin), shortn = b.short_name();
    string gdb = "#!/bin/bash\ncd \"$(dirname \"$(readlink -f \"$0\")\")\"\n"
                 "if [ -r ../mysqld/" + shortn + "/" + bin_name + " ]; then BIN=../mysqld/" + shortn + "/" + bin_name + "\n"
                 "elif [ -r ../mysqld/" + bin_name + " ]; then BIN=../mysqld/" + bin_name + "\n"
                 "elif [ -r ../../mysqld/" + bin_name + " ]; then BIN=../../mysqld/" + bin_name + "\n"
                 "else BIN=" + sh(b.bin) + "; fi\n"
                 "echo 'Handy copy and paste script:'\necho '  set pagination off'\necho '  set print pretty on'\n"
                 "echo '  set print frame-arguments all'\necho '  thread apply all backtrace full'\necho 'OR simple one-thread backtrace instead of all threads (i.e. instead of last line):'\n"
                 "echo '  bt'\nsleep 5\ngdb -iex 'set debuginfod enabled off' \"$BIN\" ./data*/*core*\n";
    write_file(tdir + "/gdb", gdb);
    chmod((tdir + "/gdb").c_str(), 0755);
    string stack = script_path("stack.sh");
    if (file_exists(stack)) symlink(stack.c_str(), (tdir + "/stack").c_str());
  }
}
namespace {
void chmod_tree(const string& dir) {
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
    struct stat st;
    if (stat(e.path().c_str(), &st) != 0) continue;
    mode_t m = st.st_mode | S_IRUSR | S_IRGRP | S_IROTH;
    if (S_ISDIR(st.st_mode) || (st.st_mode & S_IXUSR)) m |= S_IXUSR | S_IXGRP | S_IXOTH;
    chmod(e.path().c_str(), m);
  }
  chmod(dir.c_str(), 0755);
}
}  // namespace

bool text_has_san_marker(const string& t) {
  return t.find("=ERROR:") != string::npos || icontains(t, "runtime error:") || icontains(t, "AddressSanitizer:") || icontains(t, "ThreadSanitizer:") ||
         icontains(t, "LeakSanitizer:") || icontains(t, "MemorySanitizer:");
}

// a backup issue as a UID: the step, then its message with every number made N and the
// [NN] YYYY-MM-DD HH:MM:SS prefix of a mariadb-backup line taken off, so one cause is one UID
string backup_issue_uid(const string& step, const string& message) {
  string m;
  for (char c : message) { if (isdigit((unsigned char)c)) { if (m.empty() || m.back() != 'N') m += 'N'; } else m += c; }
  size_t at = !m.empty() && m[0] == '[' ? m.find("] ") : string::npos;
  if (at != string::npos) { size_t s = m.find_first_not_of("N-: ", at + 2); m = s == string::npos ? "" : m.substr(s); }
  return "BACKUP_ISSUE|" + step + "|" + m.substr(0, 200);
}

// Where a trial's datadir goes, and with it the core the kernel writes there. CORE_DIR=auto keeps
// the tmpfs speed while there is room and puts a new trial on DATA_DIR once the tmpfs is over
// SHM_STEPDOWN_PCT, so a large core cannot fill it.
bool core_on_data(int shm_used_pct) {
  if (g_cfg.core_dir == "data") return true;
  if (g_cfg.core_dir == "auto") return shm_used_pct >= g_cfg.shm_stepdown_pct;
  return false;
}

int role_trial(const Args& a) {
  string basedir = opt(a, "--basedir"), workdir = opt(a, "--workdir"), rundir = opt(a, "--rundir");
  string area_name = opt(a, "--area"), mode = opt(a, "--mode", "normal");
  long trial = to_long(opt(a, "--trial", "1"));
  int threads = (int)to_long(opt(a, "--threads", "1"));
  int seconds = (int)to_long(opt(a, "--seconds", std::to_string(g_cfg.trial_seconds)));
  uint64_t seed = strtoull(opt(a, "--seed", "0").c_str(), nullptr, 10);
  bool no_disk = flag(a, "--no-disk");
  if (flag(a, "--infile")) g_cfg.infile = opt(a, "--infile");   // the run's INFILE, not this process's settings file
  string run_id = basename_of(workdir);
  auto fail = [&](const string& text) { role_emit("result outcome=error trial=" + std::to_string(trial) + " text=" + text); return 2; };
  if (basedir.empty() || workdir.empty() || rundir.empty()) return fail("--basedir, --workdir and --rundir are needed");
  if (seed == 0) seed = rng().next();
  Basedir b;
  if (!basedir_probe(basedir, b)) return fail("no server binary under " + basedir);
  if (mode == "backup" && b.backup.empty()) return fail("no mariadb-backup in " + b.path);
  string tdir = rundir + "/" + std::to_string(trial);
  string final_dir = workdir + "/" + std::to_string(trial);
  int shm_pct = dir_used_pct(g_cfg.shm_dir);
  bool data_core = core_on_data(shm_pct);
  if (data_core && g_cfg.core_dir == "auto")
    logline("trial %ld: %s is at %d%%, so this trial runs on %s", trial, g_cfg.shm_dir.c_str(), shm_pct, g_cfg.data_dir.c_str());
  // one runaway core cannot fill the disk: the kernel stops writing at CORE_MAX_GB. mariadbd
  // inherits this limit from here.
#ifdef __linux__
  if (g_cfg.core_max_gb > 0) {
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)g_cfg.core_max_gb * 1024 * 1024 * 1024;
    rl.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &rl) != 0) logwarn("trial %ld: the core size limit could not be set", trial);
  }
#endif
  remove_tree(tdir);
  if (data_core) remove_tree(final_dir);
  // saving: with the datadir already in the final dir only the tmpfs part moves across
  auto save_trial = [&](string* why) {
    // a plain rename when nothing is in the final dir yet; else the parts move across one by one
    // (the datadir in data mode, or a core that already went there while the trial ran)
    if (!data_core && !dir_exists(final_dir)) return move_tree(tdir, final_dir, why);
    mkdirs(final_dir);
    std::error_code ec;
    bool ok = true;
    for (auto& e : fs::directory_iterator(tdir, ec)) {
      string name = e.path().filename().string();
      string dst = final_dir + "/" + name;
      remove_tree(dst);
      if (!move_tree(e.path().string(), dst, why)) ok = false;
    }
    remove_tree(tdir);
    return ok;
  };
  auto discard_trial = [&]() {
    remove_tree(tdir);
    remove_tree(final_dir);                                      // the datadir, or a core moved there while the trial ran
  };
  mkdirs(tdir + "/log");
  string err;
  // the SQL
  SqlSources s;
  s.use_disk = !no_disk && g_cfg.all_disk_sql;
  s.target = (size_t)std::max(0L, to_long(opt(a, "--sql-lines", "0")));   // the driver sizes it from what the last trials ran
  if (!sources_init(s, b, rundir + "/sql", &err)) return fail("sources: " + err);
  Xoshiro256pp r;
  r.seed(seed);
  const Area* area = nullptr;
  if (area_name.empty()) {
    auto avail = areas_available(b, {});
    area = area_pick(avail, r);
  } else {
    area = area_by_name(area_name);
    string why;
    if (area && !area_available(*area, b, &why)) return fail("area " + area_name + " cannot run on " + b.name + ": " + why);
  }
  if (!area) return fail("no area for " + b.name);
  bool multi = mode == "multi";
  if (multi && threads < 2) threads = (int)r.range(2, 10);
  if (!multi) threads = 1;
  // the server's port is picked here: a TCP server's SQL (the FederatedX SERVER) has to name it
  Instance inst;
  inst.bd = &b;
  inst.port = port_pick();
  if (inst.port == 0) return fail("no free port");
  if (endpoint_tcp(b)) s.federated_conn = fmt("HOST '127.0.0.1', PORT %d", inst.port);
  TrialSql t;
  if (!sources_assemble(s, b, *area, r, tdir, multi, t, &err)) return fail("sql: " + err);
  write_file(tdir + "/BASEDIR", b.path + "\n");
  write_file(tdir + "/MYSAFE", mysafe_options(b) + "\n");
  append_file(tdir + "/SEED", fmt("trial=%llu\nmode=%s\nthreads=%d\nsql_lines=%zu\n", (unsigned long long)seed, mode.c_str(), threads, sources_target_lines(s)));
  string infile = t.sql_path;
  if (multi) {
    vector<string> lines = split_lines(read_file(t.sql_path));
    r.shuffle(lines);
    if ((long)lines.size() > MULTI_THREAD_LINES) lines.resize(MULTI_THREAD_LINES);
    infile = tdir + "/" + std::to_string(trial) + ".sql";
    write_file(infile, join(lines, "\n") + "\n");
  }
  // the server
  string tpl = template_for(b, t.myinit, workdir + "/templates");
  if (tpl.empty()) return fail("no datadir template for " + b.name);
  // where the datadir goes decides where the kernel writes a core: the server changes to its datadir
  // at startup and the core file is written there, so CORE_DIR=data keeps a big core off the tmpfs.
  string datadir_override;
  if (data_core) { datadir_override = final_dir + "/data"; mkdirs(final_dir); }
  inst.set_paths(tdir, datadir_override);
  inst.extra = t.myextra;
  inst.myinit = t.myinit;
  int start_timeout = b.is_san() ? 240 : 60;
  bool started = inst.start_fresh(tpl, start_timeout);
  string base_ev = fmt("trial=%ld area=%s mode=%s threads=%d", trial, area->name.c_str(), mode.c_str(), threads);
  if (!started) {
    string log = read_file(inst.errlog);
    bool core = inst.has_core() || inst.has_dump();
    if (!core) {
      inst.kill_hard();
      remove_tree(inst.datadir);                                  // the copy of the template says nothing
      string why = inst.start_note;
      if (log_aborted(log)) {
        if (icontains(log, "Address already in use")) { discard_trial(); role_emit("result outcome=port-clash " + base_ev); return 0; }
        if (icontains(log, "Can't initialize timers")) { discard_trial(); role_emit("result outcome=dropped-timers " + base_ev); return 0; }
        // a start that failed on a server option is a configured value, not a bug: the trial goes
        static const char* opt_refusals[] = {"unknown variable", "error while setting value", "unknown option", "requires innodb_buffer_pool_size",
                                             "registration as a STORAGE ENGINE failed", "unknown/unsupported storage engine", "is not a valid value for"};
        string low = lower(log);
        bool option_fault = !t.random_options.empty();
        string which;
        for (auto* k : opt_refusals) if (low.find(k) != string::npos) { option_fault = true; if (which.empty()) which = k; }
        if (option_fault) {
          discard_trial();
          string note = t.random_options.empty() ? which : join(t.random_options, " ");
          role_emit("result outcome=options " + base_ev + " text=" + note);
          return 0;
        }
        why = "[ERROR] Aborting: an option was refused";
      }
      write_file(tdir + "/SERVER_START_FAILED", why + "\n");
      string mvwhy;
      if (!save_trial(&mvwhy)) return fail("cannot save " + tdir + ": " + mvwhy);
      role_emit("result outcome=start-failed " + base_ev + " dir=" + final_dir + " text=" + why);
      return 0;
    }
  }
  role_emit(fmt("started %s pid=%d port=%d", base_ev.c_str(), (int)inst.pid, inst.port));
  // spider and friends: the preload runs first, its trace is prepended when the trial is kept
  if (started && !t.preload_path.empty()) {
    ClientParams pp;
    pp.ep = inst.endpoint(); pp.logdir = tdir + "/preload"; pp.threads = 1; pp.queries_per_thread = 99999999; pp.shuffle = false; pp.seed = seed;
    std::atomic<bool> st{false};
    ClientResult pr;
    client_run(pp, t.preload_path, st, pr, &err);
  }
  // the trial window
  ClientParams cp;
  cp.ep = inst.endpoint(); cp.logdir = tdir; cp.threads = threads; cp.shuffle = true; cp.seed = seed;
  std::atomic<bool> stop{false};
  std::atomic<bool> client_done{false};
  ClientResult cr;
  string client_err;
  bool client_ok = false;
  std::thread ct;
  if (started) ct = std::thread([&] { client_ok = client_run(cp, infile, stop, cr, &client_err); client_done = true; });
  double deadline = now_ms() + seconds * 1000.0;
  bool crash_done = false, recovery_failed = false, server_died = false;
  // backup trials: the full backup runs against the busy server; the rest of the round trip follows the
  // window. The backup and the restored server sit beside the datadir, so on DATA_DIR when the trial is.
  string bk_home = data_core ? final_dir : tdir, bk_log = tdir + "/log/backup.log";
  // mariadb-backup runs in a directory of its own, so a core it leaves stays with the trial, one level
  // below the */*core* the UID chain reads the server's core from
  string bk_cwd = bk_home + "/backup/cwd";
  pid_t bk_pid = -1;
  while (started && now_ms() < deadline) {
    if (client_done) break;
    if (!inst.alive()) { server_died = true; break; }
    if (mode == "crash" && !crash_done && now_ms() >= deadline - CRASH_KILL_BEFORE_END_SEC * 1000.0) {
      crash_done = true;
      kill_group(inst.pid, SIGKILL);
      wait_pid(inst.pid, 10000);
      inst.pid = -1;
      sync();
      sleep(2);
      append_file(inst.errlog, "omnium: crash recovery test: the server was killed with SIGKILL and is started again on the same datadir\n");
      unlink(inst.sock.c_str());
      if (!inst.start_only(start_timeout)) {
        // the server recovered and the trial's own SQL had turned root away: a configured state, not a recovery failure
        if (kTakeFixes && root_turned_away(inst.start_note)) { logline("trial %ld: crash recovery: the server turns root away, %s", trial, inst.start_note.c_str()); break; }
        recovery_failed = true;
        write_file(tdir + "/CRASH_RECOVERY_ISSUE", inst.start_note + "\n");
        break;
      }
    }
    if (mode == "backup" && bk_pid == -1 && now_ms() >= deadline - BACKUP_START_BEFORE_END_SEC * 1000.0) {
      vector<string> argv = {b.backup, "--no-defaults", "--backup", "--user=root"};
      for (auto& x : backup_conn(inst)) argv.push_back(x);
      argv.push_back("--target-dir=" + bk_home + "/backup/full");
      mkdirs(bk_cwd);
      append_file(bk_log, "$ " + join(argv, " ") + "\n");
      bk_pid = spawn_program(argv, bk_log, bk_cwd, true, {}, true);
    }
    usleep(200000);
  }
  bool timeout_reached = started && !client_done && !server_died && now_ms() >= deadline;
  stop = true;
  if (started) {
    for (int i = 0; i < 20 && !client_done; i++) usleep(100000);
    if (!client_done) client_kill_connections(cp);
    for (int i = 0; i < 50 && !client_done; i++) usleep(100000);
  }
  sleep(b.is_san() ? 5 : 3);
  // backup trials: the round trip. With the data at rest its checksums are taken, an incremental backup
  // brings the full backup of the busy server up to them, both are prepared, a server starts on the
  // result and its checksums are compared. A step that fails, or a difference, is kept as BACKUP_ISSUE.
  // A server that turns root away is not a backup finding, so that round trip is skipped.
  bool backup_issue = false;
  string restore_dir = bk_home + "/restore", backup_uid;
  Instance ri;
  if (mode == "backup" && started && !server_died && inst.alive()) {
    string full = bk_home + "/backup/full", inc = bk_home + "/backup/inc";
    string why, skip;
    auto judge = [&](const string& what, int rc, const string& out) {
      if (rc == 0) return true;
      string last = trim(tail_lines(out, 1));
      if (root_turned_away(out)) skip = what + ": " + last;
      else if (rc == -1 || rc == 124) { why = what + fmt(" did not finish within %d s", BACKUP_STEP_TIMEOUT_S); backup_uid = backup_issue_uid(what, "hang"); }
      else { why = what + fmt(" ended %d: ", rc) + last; backup_uid = backup_issue_uid(what, last); }
      return false;
    };
    auto step = [&](const string& what, const vector<string>& args) {
      vector<string> argv = {b.backup};
      if (args.empty() || !starts_with(args[0], "--defaults-file=")) argv.push_back("--no-defaults");   // a step may name the file it reads
      argv.insert(argv.end(), args.begin(), args.end());
      mkdirs(bk_cwd);
      CmdResult r = run_capture(argv, BACKUP_STEP_TIMEOUT_S, bk_cwd);
      append_file(bk_log, "$ " + join(argv, " ") + "\n" + r.out + fmt("[exit %d]\n", r.rc));
      return judge(what, r.rc, r.out);
    };
    bool ok;
    if (bk_pid > 0) {
      int st = wait_pid(bk_pid, BACKUP_STEP_TIMEOUT_S * 1000);
      if (st == -1) { kill_group(bk_pid, SIGKILL); wait_pid(bk_pid, 5000); }
      int rc = st == -1 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : st;
      ok = judge("the full backup of the busy server", rc, read_file(bk_log));
      append_file(bk_log, fmt("[exit %d]\n", rc));
    } else ok = step("the full backup", with_conn({"--backup", "--user=root"}, inst, {"--target-dir=" + full}));
    std::map<string, string> orig, rest, engine;
    if (ok && !table_checksums(inst.endpoint(), orig, engine, &why)) { skip = "checksums on the server: " + why; ok = false; }
    if (ok) ok = step("the incremental backup", with_conn({"--backup", "--user=root"}, inst, {"--target-dir=" + inc, "--incremental-basedir=" + full}));
    vector<string> prep_full = {"--prepare", "--target-dir=" + full}, prep_inc = {"--prepare", "--target-dir=" + full, "--incremental-dir=" + inc};
    if (kTakeFixes && backup_is_encrypted(t.myextra)) {          // the backup's own backup-my.cnf has the key plugin and its files
      string cnf = "--defaults-file=" + full + "/backup-my.cnf";
      prep_full.insert(prep_full.begin(), cnf);
      prep_inc.insert(prep_inc.begin(), cnf);
    }
    if (ok) ok = step("prepare of the full backup", prep_full);
    if (ok) ok = step("prepare of the incremental backup", prep_inc);
    if (ok) {
      // the prepared backup is a datadir: a server starts on it with the trial's own options
      ri.bd = &b;
      ri.set_paths(restore_dir);
      ri.extra = t.myextra;
      ri.myinit = t.myinit;
      mkdirs(ri.logdir);
      mkdirs(ri.tmpdir);
      write_file(restore_dir + "/BASEDIR", b.path + "\n");
      string mvwhy;
      if (!move_tree(full, ri.datadir, &mvwhy)) { why = "the prepared backup could not be moved into place: " + mvwhy; ok = false; }
      else {
        ri.port = port_pick();
        if (!ri.start_only(start_timeout)) {
          ok = false;
          if (root_turned_away(ri.start_note)) skip = "the server on the restored data: " + ri.start_note;
          else { why = "the server did not start on the restored data: " + ri.start_note; backup_uid = backup_issue_uid("the server did not start on the restored data", ri.start_note); }
        }
      }
    }
    if (ok && !table_checksums(ri.endpoint(), rest, engine, &why)) { why = "the server on the restored data gave no checksums: " + why; backup_uid = backup_issue_uid("the server on the restored data gave no checksums", why); ok = false; }
    if (ok) {
      vector<string> diffs, engines;
      for (auto& [name, sum] : orig) {
        if (starts_with(sum, "no checksum")) continue;
        auto it = rest.find(name);
        string got = it == rest.end() ? "missing" : it->second;
        if (got == sum) continue;
        diffs.push_back(name + " (" + engine[name] + "): " + sum + " on the server, " + got + " after the round trip");
        if (std::find(engines.begin(), engines.end(), engine[name]) == engines.end()) engines.push_back(engine[name]);
      }
      if (!diffs.empty()) {
        std::sort(engines.begin(), engines.end());
        why = fmt("%zu of %zu tables differ after the round trip\n", diffs.size(), orig.size()) + join(diffs, "\n");
        backup_uid = backup_issue_uid("tables differ after the round trip", join(engines, ","));
        ok = false;
      }
    }
    if (!ri.shutdown(SHUTDOWN_SECONDS) && ok) { why = fmt("the server on the restored data did not stop within %d s", SHUTDOWN_SECONDS); backup_uid = backup_issue_uid("the server on the restored data did not stop", ""); ok = false; }
    ri.kill_hard();
    if ((ri.has_core() || ri.has_dump()) && ok) { why = "the server on the restored data crashed; its core and log are under restore/"; ok = false; }
    if (ok) logline("trial %ld: backup round trip: %zu tables match after the full and incremental backups, prepare and a start on the result", trial, orig.size());
    else if (!skip.empty()) logline("trial %ld: backup round trip skipped, %s", trial, skip.c_str());
    else { backup_issue = true; write_file(tdir + "/BACKUP_ISSUE", why + "\n"); logline("trial %ld: backup round trip issue, %s", trial, why.substr(0, why.find('\n')).c_str()); }
  }
  // the error log scan, before the shutdown (pquery-run.sh order)
  bool save = false;
  string save_reason, errlog_uid;
  {
    string top;
    if (els_run("check", {inst.errlog}, false, top, nullptr) && els_run("top", {inst.errlog}, false, top, nullptr) && !top.empty()) {
      errlog_uid = split_lines(top).empty() ? top : split_lines(top)[0];
      if (!uid_known(errlog_uid)) { write_file(tdir + "/ERROR_LOG_SCAN_ISSUE", errlog_uid + "\n"); save = true; save_reason = "errlog"; }
    }
  }
  // clean shutdown, so shutdown asserts and hangs show
  bool shutdown_hang = false;
  if (inst.alive()) {
    string snote;
    bool gone = inst.shutdown(SHUTDOWN_SECONDS, &snote);
    if (!gone) {
      for (int i = 0; i < 5 && !inst.has_core() && !inst.has_dump(); i++) sleep(1);
      if (!inst.has_core() && !inst.has_dump()) {
        shutdown_hang = true;
        write_file(tdir + "/SHUTDOWN_TIMEOUT_ISSUE",
                   fmt("the server did not stop within %d seconds; %s\n", SHUTDOWN_SECONDS, snote.c_str()));
        if (!save) { save = true; save_reason = "shutdown-timeout"; }
      }
    }
  }
  inst.kill_hard();
  if (ct.joinable()) ct.join();
  // shm mode: the core sits on the tmpfs, so it moves to /data now, not when the trial is saved
  if (!data_core) {
    string core = inst.core_path();
    if (!core.empty()) {
      int64_t a = file_size(core), bsz = 0;
      for (int i = 0; i < 60; i++) { usleep(300000); bsz = file_size(core); if (bsz == a && bsz > 0) break; a = bsz; }
      mkdirs(final_dir);
      string dst = final_dir + "/" + basename_of(core);
      string why;
      if (move_tree(core, dst, &why)) {
        if (symlink(dst.c_str(), core.c_str()) != 0) { /* the UID chain finds it in the trial dir either way */ }
      } else {
        logwarn("trial %ld: the core stays on the tmpfs (%s)", trial, why.c_str());
      }
    }
  }
  if (started) role_emit(fmt("client %s performed=%llu failed=%llu lost=%d gone_away=%ld connect_failed=%d", base_ev.c_str(), cr.performed, cr.failed, cr.lost_connection, cr.gone_away, cr.connect_failed));
  // pquery.log: the run record of the client, in the shape pquery writes it. omnium runs the client
  // in its own process, so the worker line names this trial's process.
  {
    string pq;
    pq += "> Infile: " + infile + "\n";
    pq += "> Database: " + cp.db + "\n";
    pq += fmt("> Threads: %d\n", threads);
    pq += fmt("> Queries per thread: %lu\n", cp.queries_per_thread);
    pq += "> Logdir: " + tdir + "\n";
    pq += string("> Log all queries: ") + (cp.log_all ? "ON" : "OFF") + "\n";
    pq += string("> Log failed queries: ") + (cp.log_failed ? "ON" : "OFF") + "\n";
    pq += "> Username: " + cp.user + "\n";
    pq += inst.tcp ? fmt("> Port: %d\n", inst.port) : "> Socket: " + inst.sock + "\n";
    pq += fmt("* Waiting for created worker %d\n", (int)getpid());
    pq += fmt("- Base RNG seed: %llu (from the trial seed)\n", (unsigned long long)seed);
    pq += "- Connecting to " + cp.name + " [localhost]...\n";
    if (!started) pq += "! The server did not start, so no query ran\n";
    else {
      for (size_t i = 0; i < cr.thread_seeds.size(); i++) pq += fmt("- Thread #%zu seed: %llu\n", i, (unsigned long long)cr.thread_seeds[i]);
      pq += node_summary_line(cr.failed, cr.performed);
      if (cr.lost_connection) pq += fmt("! %d thread(s) lost the connection to the server\n", cr.lost_connection);
      if (cr.connect_failed) pq += fmt("! %d thread(s) never connected\n", cr.connect_failed);
      if (!client_err.empty()) pq += "! " + client_err + "\n";
      pq += fmt("! Exit status of the client: %d\n", client_ok ? 0 : 1);
    }
    write_file(tdir + "/pquery.log", pq);
  }
  if (recovery_failed && !save) { save = true; save_reason = "crash-recovery"; }
  // known *SAN reports leave the top of the log
  bool san = log_has_san_marker(inst.errlog);
  if (san) { san_drop_known(tdir, "log/master.err"); san = log_has_san_marker(inst.errlog); }
  bool large = file_size(inst.errlog) > LARGE_LOG_BYTES;
  if (large) write_file(tdir + "/LARGE_ERROR_LOG_ISSUE", "");
  // the UniqueID and the decision
  bool have_core = inst.has_core();
  bool crashed = have_core || inst.has_dump();                  // a Windows server leaves a minidump and the frames in its log: that crash is the bug
  string fallback = uid_fallback(inst.errlog, nullptr);
  string uid, outcome;
  if (crashed || !fallback.empty() || san) {
    UidResult ur;
    UidOptions uo;
    uo.wait_core = true;
    if (data_core) uo.core = inst.core_path();                 // the datadir is under the final dir, not the trial dir
    uid_for_dir(tdir, ur, uo);
    uid = ur.uid.empty() ? ur.err : ur.uid;
    write_file(tdir + "/MYBUG", uid + "\n");
    if (san) { save = true; outcome = "saved-san"; }
    else if (!save) {
      if (uid_known(uid) && !file_exists(tdir + "/ERROR_LOG_SCAN_ISSUE")) outcome = "known";
      else { save = true; outcome = "saved-new"; }
    }
  } else if (inst.silent_death()) {
    // a Windows release server that died with no banner and no minidump: its exit status is the UID, as the frames are a crash's
    uid = inst.silent_death_uid();
    write_file(tdir + "/MYBUG", uid + "\n");
    if (uid_known(uid) && !file_exists(tdir + "/ERROR_LOG_SCAN_ISSUE")) outcome = "known";
    else { save = true; outcome = "saved-new"; }
  } else if (kTakeFixes && file_exists(tdir + "/ERROR_LOG_SCAN_ISSUE")) {
    // a flagged log line and no crash: the UID is what omnium t says, so that MYBUG and t agree (the scan's own pick of
    // a line can differ from the chain's); the scan's UID stays when the chain has none
    UidResult ur;
    UidOptions uo;
    uo.wait_core = false;
    uid_for_dir(tdir, ur, uo);
    if (!ur.uid.empty() && !starts_with(ur.uid, "Assert:")) { uid = ur.uid; write_file(tdir + "/MYBUG", uid + "\n"); }
  } else if (read_file(inst.errlog).find("SIGKILL myself") != string::npos) {
    save = true; outcome = "saved-sigkill";
  } else if (cr.gone_away >= GONE_AWAY_SAVE && !timeout_reached && server_died) {
    save = true; outcome = "saved-gone-away";   // the server went away on its own, not on omnium's own stop
  } else if (large && !save) {
    save = true; outcome = "saved-large-log";
  }
  // backup trials: a crash of the server on the restored data has its core and log in the trial's layout
  // under restore/; any other issue carries its own UID, so the known list and the per-UID cap apply to it too
  if (backup_issue && uid.empty() && !save) {
    if (ri.has_core() || ri.has_dump()) {
      UidResult ur;
      UidOptions uo;
      uo.wait_core = true;
      uid_for_dir(restore_dir, ur, uo);
      if (!ur.uid.empty()) backup_uid = ur.uid;
    }
    uid = backup_uid;
    write_file(tdir + "/MYBUG", uid + "\n");
    if (uid_known(uid)) outcome = "known";
    else { save = true; save_reason = "backup"; }
  }
  if (outcome.empty()) outcome = save ? "saved-" + save_reason : "clean";
  if (uid.empty() && save && !errlog_uid.empty()) { uid = errlog_uid; write_file(tdir + "/MYBUG", uid + "\n"); }
  // enough copies of a new bug are kept already: this one is a duplicate
  if (save && (outcome == "saved-new" || outcome == "saved-san" || outcome == "saved-backup") && !uid.empty() && !shutdown_hang && !file_exists(tdir + "/ERROR_LOG_SCAN_ISSUE")) {
    long have = saved_with_uid(workdir, uid, final_dir);
    if (have >= std::max(1, g_cfg.keep_per_uid)) { save = false; outcome = "dup"; }
  }
  if (!uid.empty()) seen_record(uid, run_id, outcome);
  string dir_note;
  if (save) {
    // the preload trace goes in front of the main trace, as pquery-run.sh does
    string main_trace = tdir + "/default.node.tld_thread-0.sql", pre_trace = tdir + "/preload/default.node.tld_thread-0.sql";
    if (!t.preload_path.empty() && file_exists(pre_trace) && file_exists(main_trace)) {
      string merged = read_file(pre_trace) + read_file(main_trace);
      copy_file(main_trace, tdir + "/sql_without_preload.sql");
      write_file(main_trace, merged);
    }
    write_helpers(tdir, b, inst, join(t.myextra, " "), have_core);
    unlink(inst.sock.c_str());
    unlink(inst.pidfile.c_str());
    string why;
    if (save_trial(&why)) {
      chmod_tree(final_dir);
      dir_note = " dir=" + final_dir;
    } else {
      outcome = "save-failed";
      dir_note = " dir=" + tdir + " text=" + why;
    }
  } else {
    discard_trial();
  }
  role_emit("result outcome=" + outcome + " " + base_ev + fmt(" performed=%llu", cr.performed) + dir_note + (uid.empty() ? "" : " uid=" + uid));
  return 0;
}
