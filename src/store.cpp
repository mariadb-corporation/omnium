// Created by Roel Van de Paar, MariaDB
// The workdir /data/O<6 digits>: dirs, lock, ledger, status file, pid registry, and the box-wide
// /data/omnium.seen memory.
#include "common.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

Workdir g_wd;

static void fill_paths(const string& id) {
  g_wd.id = id;
  g_wd.dir = g_cfg.data_dir + "/" + id;
  g_wd.rundir = g_cfg.shm_dir + "/" + id;
  g_wd.logfile = g_wd.dir + "/" + id + ".log";
  g_wd.ledger = g_wd.dir + "/omnium.ledger";
  g_wd.status_file = g_wd.dir + "/status.txt";
  g_wd.sock = g_wd.dir + "/omnium.sock";
  g_wd.lock = g_wd.dir + "/omnium.pid";
  g_wd.pids = g_wd.dir + "/omnium.pids";
  g_wd.build_dir = g_wd.dir + "/build";
}

// the run's pid file goes when the run ends, so the workdir no longer looks live
void release_lock() {
  if (!g_wd.lock.empty() && trim(read_file(g_wd.lock)) == std::to_string((long)getpid())) unlink(g_wd.lock.c_str());
}
// the pid in the lock file holds the workdir while it is alive and is an omnium: after a reboot the
// number can belong to anything, and a lock of a dead run must not stop a resume. The binary tells
// as well as the command line does: a run started through the o link has o as its command.
bool pid_is_live_omnium(pid_t p) {
  if (p <= 0 || !pid_alive(p)) return false;
  string cmd = proc_cmdline(p);
  std::error_code ec;
  string exe = fs::read_symlink(fmt("/proc/%d/exe", (int)p), ec).string();
  return cmd.empty() || cmd.find("omnium") != string::npos || exe.find("omnium") != string::npos;
}
static bool take_lock() {
  string held = trim(read_file(g_wd.lock));
  if (!held.empty()) {
    pid_t p = (pid_t)to_long(held);
    if (p != getpid() && pid_is_live_omnium(p)) {
      logwarn("%s is held by a live omnium, pid %d", g_wd.dir.c_str(), (int)p);
      return false;
    }
  }
  return write_file(g_wd.lock, std::to_string(getpid()) + "\n");
}

bool workdir_create() {
  if (!dir_exists(g_cfg.data_dir)) { logwarn("%s does not exist", g_cfg.data_dir.c_str()); return false; }
  for (int i = 0; i < 100; i++) {
    string id = "O" + rng().digits(6);
    fill_paths(id);
    if (dir_exists(g_wd.dir) || dir_exists(g_wd.rundir)) continue;
    if (!mkdirs(g_wd.dir) || !mkdirs(g_wd.build_dir)) return false;
    mkdirs(g_wd.rundir);
    if (!take_lock()) return false;
    log_open(g_wd.logfile);
    ledger_append("run", "created " + now_stamp() + " seed " + hex64(rng_seed_used()));
    return true;
  }
  return false;
}

bool workdir_is_omnium(const string& path) {
  return file_exists(path + "/omnium.ledger") || file_exists(path + "/omnium.pid");
}

bool workdir_open(const string& id_or_path, bool take) {
  string id = basename_of(id_or_path);
  if (is_digits(id) && id.size() == 6) id = "O" + id;
  if (id_or_path.find('/') != string::npos && !workdir_is_omnium(abs_path(id_or_path))) {
    logwarn("%s is not an omnium workdir", id_or_path.c_str());
    return false;
  }
  fill_paths(id);
  if (!workdir_is_omnium(g_wd.dir)) { logwarn("%s is not an omnium workdir", g_wd.dir.c_str()); return false; }
  if (take) {
    if (!take_lock()) return false;
    mkdirs(g_wd.rundir);
    log_open(g_wd.logfile);
  }
  return true;
}

string workdir_trial_dir(long trial) { return g_wd.dir + "/" + std::to_string(trial); }

string workdir_id_from_cwd() {
  string cwd = abs_path(".");
  for (string p = cwd; p != "/" && !p.empty(); p = dirname_of(p)) {
    string b = basename_of(p);
    if (b.size() == 7 && b[0] == 'O' && is_digits(b.substr(1)) && workdir_is_omnium(p)) return b;
  }
  return {};
}

static std::mutex g_ledger_mtx;
void ledger_append(const string& kind, const string& text) {
  if (g_wd.ledger.empty()) return;
  std::lock_guard<std::mutex> lk(g_ledger_mtx);
  append_file(g_wd.ledger, fmt("%lld %s %s\n", (long long)now_s(), kind.c_str(), text.c_str()));
}
vector<string> ledger_read() { return split_lines(read_file(g_wd.ledger)); }

void status_write(const vector<std::pair<string, string>>& kv) {
  if (g_wd.status_file.empty()) return;
  string out;
  for (auto& [k, v] : kv) out += k + "=" + v + "\n";
  write_file(g_wd.status_file, out);
}
vector<std::pair<string, string>> status_read_file(const string& path) {
  vector<std::pair<string, string>> out;
  for (auto& l : split_lines(read_file(path))) {
    size_t eq = l.find('=');
    if (eq != string::npos) out.push_back({l.substr(0, eq), l.substr(eq + 1)});
  }
  return out;
}
vector<std::pair<string, string>> status_read(const string& dir) { return status_read_file(dir + "/status.txt"); }

static std::mutex g_pids_mtx;
void pids_add(pid_t pid, const string& kind, const string& note) {
  if (g_wd.pids.empty()) return;
  std::lock_guard<std::mutex> lk(g_pids_mtx);
  append_file(g_wd.pids, fmt("%d %s %s\n", (int)pid, kind.c_str(), note.c_str()));
}
void pids_remove(pid_t pid) {
  if (g_wd.pids.empty()) return;
  std::lock_guard<std::mutex> lk(g_pids_mtx);
  string out;
  string want = std::to_string((int)pid) + " ";
  for (auto& l : split_lines(read_file(g_wd.pids))) if (!starts_with(l, want)) out += l + "\n";
  write_file(g_wd.pids, out);
}
vector<std::tuple<pid_t, string, string>> pids_read(const string& dir) {
  vector<std::tuple<pid_t, string, string>> out;
  for (auto& l : split_lines(read_file(dir + "/omnium.pids"))) {
    auto f = split_ws(l);
    if (f.size() < 2) continue;
    string note;
    if (f.size() > 2) {
      size_t p = l.find(f[1], l.find(f[0]) + f[0].size()) + f[1].size();
      note = trim(l.substr(p));
    }
    out.push_back({(pid_t)to_long(f[0]), f[1], note});
  }
  return out;
}

// /data/omnium.seen: one line per UID, tab separated: uid, first, last, run, count, outcome.
// Several omnium processes may write it, so the update runs under flock.
static string seen_line(const SeenEntry& e) {
  return fmt("%s\t%lld\t%lld\t%s\t%ld\t%s\n", e.uid.c_str(), (long long)e.first, (long long)e.last,
             e.run.c_str(), e.count, e.outcome.c_str());
}
static bool seen_parse(const string& l, SeenEntry& e) {
  auto f = split(l, '\t');
  if (f.size() < 6) return false;
  e.uid = f[0]; e.first = to_long(f[1]); e.last = to_long(f[2]); e.run = f[3]; e.count = to_long(f[4]); e.outcome = f[5];
  return true;
}
std::optional<SeenEntry> seen_lookup(const string& uid) {
  for (auto& l : split_lines(read_file(g_paths.seen_file))) {
    SeenEntry e;
    if (seen_parse(l, e) && e.uid == uid) return e;
  }
  return std::nullopt;
}
void seen_record(const string& uid, const string& run, const string& outcome) {
  if (uid.empty()) return;
  int lfd = open((g_paths.seen_file + ".lock").c_str(), O_WRONLY | O_CREAT, 0644);
  if (lfd >= 0) flock(lfd, LOCK_EX);
  string out;
  bool found = false;
  for (auto& l : split_lines(read_file(g_paths.seen_file))) {
    SeenEntry e;
    if (!seen_parse(l, e)) continue;
    if (e.uid == uid) {
      found = true;
      e.last = now_s(); e.run = run; e.count++;
      if (!outcome.empty()) e.outcome = outcome;
    }
    out += seen_line(e);
  }
  if (!found) {
    SeenEntry e;
    e.uid = uid; e.first = e.last = now_s(); e.run = run; e.count = 1; e.outcome = outcome.empty() ? "new" : outcome;
    out += seen_line(e);
  }
  write_file(g_paths.seen_file, out);
  if (lfd >= 0) { flock(lfd, LOCK_UN); close(lfd); }
}
