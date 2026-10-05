// Created by Roel Van de Paar, MariaDB
// run.cpp - `omnium run`: the driver. One workdir /data/O<id>, the shared SQL sources, a datadir
// template per build, N slots each running a `--role trial` child, the governors, the ledger and
// the status file. Ctrl+C once finishes the running trials, twice stops at once.
#include "verbs.h"
#include <poll.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace {
const int DISK_POOL_REFRESH_TRIALS = 45;
const int TRIAL_HARD_CAP_S = 900;      // a trial child past window + this is killed
const int CONSECUTIVE_FAIL_STOP = 20;  // start failures in a row that stop a build's trials (plan Q224)

struct Slot {
  Child c;
  pid_t server = 0;                  // the trial's server, from its started event, for omnium.pids
  bool busy = false;
  long trial = 0;
  string build, area;
  int64_t started = 0;
  string phase = "start";           // start, run, uid: what the trial is doing, for the TUI
  unsigned long long performed = 0;
};
// a reduce or report child of this run: a plain omnium verb in its own process, one per new UID
struct Job {
  pid_t pid = -1;
  string kind;              // reduce, report
  long trial = 0;
  int64_t started = 0;
  string logfile;
};
struct Stats {
  long launched = 0, finished = 0;
  std::map<string, long> outcomes;
  std::map<string, long> per_build_saved, per_build_trials;
  std::map<string, int> consecutive_fail;
  std::set<string> stopped_builds;
  unsigned long long performed = 0;
};
std::map<string, string> parse_event(const string& line, string& kind) {
  std::map<string, string> kv;
  vector<string> w = split_ws(line);
  if (w.empty()) return kv;
  kind = w[0];
  // key=value tokens; the first key of {text, uid} takes the rest of the line
  size_t pos = line.find(' ');
  string rest = pos == string::npos ? "" : line.substr(pos + 1);
  while (!rest.empty()) {
    size_t eq = rest.find('=');
    if (eq == string::npos) break;
    string key = rest.substr(0, eq);
    if (key == "text" || key == "uid") { kv[key] = rest.substr(eq + 1); break; }
    size_t sp = rest.find(' ', eq + 1);
    kv[key] = rest.substr(eq + 1, sp == string::npos ? string::npos : sp - eq - 1);
    if (sp == string::npos) break;
    rest = rest.substr(sp + 1);
  }
  return kv;
}
int size_slots(const vector<Basedir>& builds) {
  if (g_cfg.workers > 0) return g_cfg.workers;
  double per = 0;
  for (auto& b : builds) per += b.flavour == Flavour::MSAN ? 4.0 : b.flavour == Flavour::UBASAN || b.flavour == Flavour::TSAN ? 3.0 : b.dbg ? 1.5 : 1.2;
  per /= (double)std::max<size_t>(1, builds.size());
  double ram_gb = (double)ram_available_bytes() / (1024.0 * 1024 * 1024);
  int by_ram = (int)(ram_gb * (g_cfg.ram_cap_pct / 100.0) / per);
  int by_cpu = std::max(1, cpu_threads() / 2);
  return std::max(1, std::min(by_ram, by_cpu));
}
}  // namespace

int cmd_run(const Args& a) {
  vector<string> names;
  long trials_target = 0;
  int slots_opt = 0, threads = 1, seconds = g_cfg.trial_seconds;
  string area, mode_fixed;
  long run_for = 0;                                            // whole-run wall time in seconds; 0 means RUN_DAYS
  bool no_disk = false;
  bool resume = false;
  string resume_dir;
  for (size_t i = 0; i < a.size(); i++) {
    const string& x = a[i];
    auto val = [&](const char* what) -> string { if (i + 1 >= a.size()) die("%s needs a value", what); return a[++i]; };
    if (x == "--trials") trials_target = to_long(val("--trials"));
    else if (x == "--slots") slots_opt = (int)to_long(val("--slots"));
    else if (x == "--threads") threads = (int)to_long(val("--threads"));
    else if (x == "--seconds") seconds = (int)to_long(val("--seconds"));
    else if (x == "--for") run_for = to_long(val("--for"));
    else if (x == "--area") area = val("--area");
    else if (x == "--mode") mode_fixed = val("--mode");
    else if (x == "--no-disk") no_disk = true;
    else if (x == "--resume") {
      resume = true;
      if (i + 1 < a.size() && !starts_with(a[i + 1], "--")) resume_dir = a[++i];
    }
    else if (x == "--help" || x == "-h") {
      printf("usage: omnium run [basedir|name ...] [--trials N] [--slots N] [--area NAME] [--mode normal|multi|crash|backup] [--threads N] [--seconds S] [--for S] [--no-disk]\n"
             "  --seconds is one trial, --for is the whole run (default: RUN_DAYS days)\n"
             "       omnium run --resume [<workdir>]      picks up a stopped run: same builds and settings, the trial numbers carry on\n"
             "  no basedir: every build marked test=yes in %s\n", g_paths.builds_file.c_str());
      return 0;
    } else names.push_back(x);
  }
  // --resume: the stopped run's own settings, unless the command line says otherwise
  long resume_from = 0;
  if (resume) {
    string dir = resume_dir;
    if (dir.empty()) {                                          // the newest stopped omnium run
      std::error_code ec;
      int64_t best = 0;
      for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) {
        string n = e.path().filename().string();
        if (n.size() != 7 || n[0] != 'O' || !is_digits(n.substr(1))) continue;
        if (!file_exists(e.path().string() + "/run.conf")) continue;
        if (pid_is_live_omnium((pid_t)to_long(trim(read_file(e.path().string() + "/omnium.pid")), 0))) continue;
        // file_mtime, not fs::last_write_time: libstdc++'s file_time_type has an epoch of its own
        int64_t secs = file_mtime(e.path().string() + "/omnium.ledger");
        if (secs > best) { best = secs; dir = e.path().string(); }
      }
      if (dir.empty()) { printf("no stopped omnium run with a run.conf under %s\n", g_cfg.data_dir.c_str()); return 1; }
    }
    if (dir.find('/') == string::npos) dir = g_cfg.data_dir + "/" + (starts_with(dir, "O") ? dir : "O" + dir);
    if (!dir_exists(dir)) { printf("no such workdir %s\n", dir.c_str()); return 1; }
    if (pid_is_live_omnium((pid_t)to_long(trim(read_file(dir + "/omnium.pid")), 0))) { printf("%s is still running (pid %s)\n", dir.c_str(), trim(read_file(dir + "/omnium.pid")).c_str()); return 1; }
    auto conf = status_read_file(dir + "/run.conf");
    auto cv = [&](const string& k) { for (auto& p : conf) if (p.first == k) return p.second; return string(); };
    if (names.empty()) for (auto& n : split(cv("builds"), ',')) if (!trim(n).empty()) names.push_back(trim(n));
    if (trials_target == 0) trials_target = to_long(cv("trials"), 0);
    if (run_for == 0) run_for = to_long(cv("run_for"), 0);
    if (slots_opt == 0) slots_opt = (int)to_long(cv("slots"), 0);
    if (seconds == g_cfg.trial_seconds && !cv("seconds").empty()) seconds = (int)to_long(cv("seconds"));
    if (threads == 1 && !cv("threads").empty()) threads = (int)to_long(cv("threads"));
    if (area.empty()) area = cv("area");
    if (mode_fixed.empty()) mode_fixed = cv("mode");
    if (!no_disk) no_disk = cv("no_disk") == "1";
    {
      // INFILE is a setting, so a KEY=VALUE on this command line wins; the file's value does not
      const char* set = getenv("OMNIUM_SET");
      bool cli_infile = ("\n" + string(set ? set : "")).find("\nINFILE=") != string::npos;
      bool recorded = false;
      for (auto& p : conf) if (p.first == "infile") recorded = true;
      if (recorded && !cli_infile) g_cfg.infile = cv("infile");
    }
    resume_dir = dir;
  }
  vector<Basedir> builds;
  if (names.empty()) {
    Registry reg = registry_current();
    for (auto& n : registry_names(reg, true)) names.push_back(n);
    if (names.empty()) { printf("no build is marked test=yes in %s; name a basedir or run omnium builds\n", g_paths.builds_file.c_str()); return 1; }
  }
  for (auto& n : names) {
    string p = n.find('/') == string::npos ? g_cfg.test_dir + "/" + n : n;
    Basedir b;
    if (!basedir_probe(p, b)) { printf("%s: no server binary, skipped\n", p.c_str()); continue; }
    builds.push_back(b);
  }
  if (builds.empty()) { printf("no usable build\n"); return 1; }
  if (resume) {
    if (!workdir_open(resume_dir, true)) { printf("cannot take over %s\n", resume_dir.c_str()); return 1; }
    // what the stopped run left running: a pid of its list that is still up and still is what the
    // list says, a server in this run's rundir or a trial or job of this workdir, goes; a pid that
    // now belongs to anything else stays, a tail on one of the run's logs as well
    auto names = [](const string& cmd, const string& path) {       // the path itself, not a longer name
      for (size_t k = cmd.find(path); k != string::npos; k = cmd.find(path, k + 1)) {
        size_t e = k + path.size();
        if (e == cmd.size() || cmd[e] == '/' || cmd[e] == ' ') return true;
      }
      return false;
    };
    auto ours = [&](const string& cmd, const string& kind) {
      string exe = basename_of(cmd.substr(0, cmd.find(' ')));
      if (kind == "server") return (exe == "mariadbd" || exe == "mysqld") && names(cmd, g_wd.rundir);
      if (kind == "trial") return cmd.find(" --role trial ") != string::npos && names(cmd, g_wd.dir);
      return names(cmd, " " + kind + " " + g_wd.dir);              // a reduce or report job: <binary> <kind> <workdir> <trial>
    };
    for (auto& [pid, kind, note] : pids_read(g_wd.dir)) {
      if (pid <= 0 || !pid_alive(pid)) continue;
      if (!ours(proc_cmdline(pid), kind)) continue;
      kill_group(pid, SIGKILL);
      logwarn("pid %d (%s, %s) of the stopped run was still up: killed", (int)pid, kind.c_str(), note.c_str());
    }
    write_file(g_wd.pids, "");
    mkdirs(g_wd.rundir);
    mkdirs(g_wd.build_dir);
    // the trial numbers carry on from what the workdir already holds
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_wd.dir, ec)) {
      string n = e.path().filename().string();
      if (e.is_directory(ec) && is_digits(n)) resume_from = std::max(resume_from, to_long(n, 0));
    }
    for (auto& l : split_lines(read_file(g_wd.ledger))) {
      vector<string> f = split_ws(l);
      if (f.size() >= 3 && f[1] == "trial" && is_digits(f[2])) resume_from = std::max(resume_from, to_long(f[2], 0));
    }
    ledger_append("run", fmt("resumed %s from trial %ld", now_stamp().c_str(), resume_from + 1));
  } else if (!workdir_create()) { printf("cannot create a workdir under %s\n", g_cfg.data_dir.c_str()); return 1; }
  raise_fd_limit();
  set_oom_score(getpid(), -1000);                               // Q201; takes effect only as root, omnium calls no sudo
  // the run works from its own copy of the binary: rebuilding omnium while a run goes on then
  // cannot leave a trial child without an executable
  {
    string own = g_wd.dir + "/omnium.bin";
    if (copy_file(self_exe(), own)) { chmod(own.c_str(), 0755); g_exe_override = own; }
  }
  std::atomic<int> stopc{0};
  install_stop_signals(stopc);
  int slots = slots_opt > 0 ? slots_opt : size_slots(builds);
  logline("run %s: %zu build(s), %d slot(s), %d s trials%s", g_wd.id.c_str(), builds.size(), slots, seconds, trials_target ? fmt(", %ld trials", trials_target).c_str() : "");
  for (auto& b : builds) logline("  %s", b.name.c_str());
  ledger_append("run", fmt("builds %zu slots %d", builds.size(), slots));
  {
    vector<string> bn;
    for (auto& b : builds) bn.push_back(b.name);
    string conf;
    conf += "builds=" + join(bn, ",") + "\n";
    conf += fmt("slots=%d\ntrials=%ld\nseconds=%d\nthreads=%d\nrun_for=%ld\n", slots, trials_target, seconds, threads, run_for);
    conf += "area=" + area + "\nmode=" + mode_fixed + "\n";
    conf += string("no_disk=") + (no_disk ? "1" : "0") + "\n";
    conf += "infile=" + g_cfg.infile + "\n";
    write_file(g_wd.dir + "/run.conf", conf);
  }
  write_file(g_wd.dir + "/BASEDIR.template", builds[0].path + "\n");
  // The framework has a cleaner, ~/mariadb-qa/tmpfs_clean.sh, that deletes a directory under
  // /dev/shm when no running program mentions it. A run keeps its trial directories there, so
  // omnium starts one small child that does nothing but sleep with that directory in its command
  // line. While the run is on, the cleaner sees the directory in use and leaves it alone. The
  // child ends with the run.
  Child holder;
  spawn_role(holder, "hold", {g_wd.rundir}, "/dev/null");
  // the shared SQL sources
  string sqlwork = g_wd.rundir + "/sql";
  string err;
  logline("preparing the SQL sources under %s", sqlwork.c_str());
  if (!sources_shared_prepare(sqlwork, &err)) { logwarn("sources: %s", err.c_str()); return 1; }
  // one datadir template per build
  {
    vector<std::thread> th;
    vector<string> tpls(builds.size());
    for (size_t i = 0; i < builds.size(); i++) th.emplace_back([&, i] { tpls[i] = template_for(builds[i], "", g_wd.dir + "/templates"); });
    for (auto& t : th) t.join();
    vector<Basedir> ok;
    for (size_t i = 0; i < builds.size(); i++) { if (tpls[i].empty()) logwarn("%s: no datadir template, build left out", builds[i].name.c_str()); else ok.push_back(builds[i]); }
    builds = ok;
    if (builds.empty()) { logwarn("no build has a datadir template"); return 1; }
  }
  Xoshiro256pp pool_rng = rng_stream(7);
  std::thread pool_thread;                                    // the disk pool refresh, one at a time
  std::atomic<bool> pool_busy{false};
  vector<Slot> S(slots);
  Stats st;
  SqlSources pace;                                            // only its list of what the last trials ran: the SQL size of the next
  // the in-run pipeline children (reduce, then report), declared here so the status can show them
  std::set<string> reduced_uids;
  std::deque<long> reduce_queue;
  vector<Job> jobs;
  long next_trial = resume_from + 1;
  if (resume) st.launched = resume_from;                        // --trials counts the whole run, not this process
  size_t rr = 0;
  int64_t run_end = now_s() + (run_for > 0 ? run_for : (int64_t)g_cfg.run_days * 86400);
  int64_t last_status = 0;
  long pool_age = 0;
  bool paused = false;
  Xoshiro256pp seeds = rng_stream(3);
  // the build's short name, from the registry entry the run holds
  auto short_of = [&](const string& name) {
    for (auto& b : builds) if (b.name == name) return b.short_name();
    return name;
  };
  auto status = [&] {
    vector<std::pair<string, string>> kv = {{"run", g_wd.id}, {"state", stopc.load() ? "stopping" : paused ? "paused" : "running"}, {"slots", std::to_string(slots)},
                                            {"launched", std::to_string(st.launched)}, {"finished", std::to_string(st.finished)},
                                            {"performed", std::to_string(st.performed)}, {"sql_lines", std::to_string(sources_target_lines(pace))},
                                            {"ram_pct", std::to_string(ram_used_pct())},
                                            {"shm_pct", std::to_string(dir_used_pct(g_cfg.shm_dir))}};
    for (auto& [k, v] : st.outcomes) kv.push_back({"outcome_" + k, std::to_string(v)});
    for (size_t i = 0; i < S.size(); i++) {
      const Slot& s = S[i];
      if (!s.busy) continue;
      kv.push_back({fmt("slot_%02zu", i), fmt("%s trial %ld %s %s %llus%s", short_of(s.build).c_str(), s.trial, s.area.c_str(), s.phase.c_str(),
                                              (unsigned long long)(now_s() - s.started), s.performed ? fmt(" %lluq", s.performed).c_str() : "")});
    }
    {
      vector<string> js;
      for (auto& j : jobs) js.push_back(fmt("%s %ld %llus", j.kind.c_str(), j.trial, (unsigned long long)(now_s() - j.started)));
      if (!js.empty()) kv.push_back({"jobs", join(js, ", ")});
      if (!reduce_queue.empty()) kv.push_back({"queue", std::to_string(reduce_queue.size())});
    }
    for (auto& b : builds) kv.push_back({"build_" + b.name, fmt("%ld trials, %ld saved%s", st.per_build_trials[b.name], st.per_build_saved[b.name], st.stopped_builds.count(b.name) ? ", stopped" : "")});
    status_write(kv);
    last_status = now_s();
  };
  auto governors_ok = [&](string* why) {
    int ram = ram_used_pct();
    if (ram >= g_cfg.ram_cap_pct) { *why = fmt("RAM at %d%%", ram); return false; }
    int shm = dir_used_pct(g_cfg.shm_dir);
    if (shm >= g_cfg.shm_cap_pct) { *why = fmt("%s at %d%%", g_cfg.shm_dir.c_str(), shm); return false; }
    double data_gb = (double)dir_free_bytes(g_cfg.data_dir) / (1024.0 * 1024 * 1024);
    if (data_gb < g_cfg.data_floor_gb) { *why = fmt("%s has %.1f GB free", g_cfg.data_dir.c_str(), data_gb); return false; }
    return true;
  };
  string last_gov_why;
  int64_t last_gov_log = 0;
  // a reducer runs many servers, so few at a time
  const size_t max_jobs = std::max<size_t>(1, (size_t)slots / 8);
  string exe;
  exe = g_exe_override.empty() ? self_exe() : g_exe_override;
  // the server binary and its libraries, once per build, when that build first saves a trial: the
  // trial can then be read on a box that has no /test (ldd_files.sh, in the binary)
  std::set<string> mysqld_copied;
  vector<std::thread> copy_threads;
  auto ensure_mysqld_copy = [&](const string& build_name) {
    if (!mysqld_copied.insert(build_name).second) return;
    const Basedir* b = nullptr;
    for (auto& x : builds) if (x.name == build_name) b = &x;
    if (!b || b->bin.empty()) return;
    Basedir copy = *b;
    size_t nbuilds = builds.size();
    copy_threads.emplace_back([copy, nbuilds] {
      string dir = g_wd.dir + "/mysqld/" + copy.short_name();
      vector<string> files;
      string e;
      if (!ldd_copy(copy.bin, dir, "", &files, &e)) { logwarn("%s: no binary copy (%s)", copy.name.c_str(), e.c_str()); return; }
      if (nbuilds == 1) {                                       // the framework layout, for its tools
        string link = g_wd.dir + "/mysqld/" + basename_of(copy.bin);
        if (!file_exists(link)) { string rel = copy.short_name() + "/" + basename_of(copy.bin); if (symlink(rel.c_str(), link.c_str()) != 0) { /* a copy is there already */ } }
      }
      logline("%s: server binary and %zu libraries copied to %s", copy.short_name().c_str(), files.size(), dir.c_str());
    });
  };
  auto start_job = [&](const string& kind, long trial) {
    Job j;
    j.kind = kind; j.trial = trial; j.started = now_s();
    mkdirs(g_wd.dir + "/log");
    j.logfile = g_wd.dir + fmt("/log/%s_%ld.log", kind.c_str(), trial);
    j.pid = spawn_program({exe, kind, g_wd.dir, std::to_string(trial)}, j.logfile, g_wd.dir, true, {}, true);
    if (j.pid <= 0) { logwarn("cannot start %s for trial %ld", kind.c_str(), trial); return; }
    pids_add(j.pid, kind, fmt("trial %ld", trial));
    logline("%s of trial %ld started (pid %d), log %s", kind.c_str(), trial, (int)j.pid, j.logfile.c_str());
    jobs.push_back(j);
  };
  auto reap_jobs = [&]() {
    for (size_t i = 0; i < jobs.size();) {
      Job& j = jobs[i];
      int st_code = 0;
      pid_t r = waitpid(j.pid, &st_code, WNOHANG);
      if (r != j.pid) { i++; continue; }
      pids_remove(j.pid);
      int rc = WIFEXITED(st_code) ? WEXITSTATUS(st_code) : 128 + WTERMSIG(st_code);
      long trial = j.trial;
      string kind = j.kind, logfile = j.logfile;
      jobs.erase(jobs.begin() + i);
      if (kind == "reduce") {
        bool has_out = false;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(g_wd.dir + "/" + std::to_string(trial), ec)) {
          string n = e.path().filename().string();
          if (n.find("_out") != string::npos && !ends_with(n, ".prev")) has_out = true;
        }
        st.outcomes[has_out ? "reduced" : "reduce-failed"]++;
        ledger_append("reduce", fmt("%ld %s rc=%d", trial, has_out ? "done" : "no result", rc));
        // after a stop no new job starts: --resume picks up the report, and the queue below
        bool stopping = stopc.load() > 0;
        logline("reduce of trial %ld: %s (rc %d)", trial, !has_out ? "no reduced testcase" : stopping ? "done, the report is left for --resume" : "done, report next", rc);
        if (has_out && !stopping) start_job("report", trial);
      } else {
        st.outcomes[rc == 0 ? "reported" : "report-failed"]++;
        string first;
        for (auto& l : split_lines(read_file(logfile))) if (starts_with(l, "report ")) first = trim(l.substr(7));
        ledger_append("report", fmt("%ld rc=%d %s", trial, rc, first.c_str()));
        logline("report of trial %ld: %s", trial, rc == 0 ? ("in the inbox: " + first).c_str() : fmt("failed (rc %d), see %s", rc, logfile.c_str()).c_str());
      }
    }
    while (stopc.load() == 0 && !reduce_queue.empty() && jobs.size() < max_jobs) { long t = reduce_queue.front(); reduce_queue.pop_front(); start_job("reduce", t); }
  };
  auto handle_event = [&](Slot& s, const string& line) {
    string kind;
    auto kv = parse_event(line, kind);
    if (kind == "result") {
      string outcome = kv["outcome"];
      st.outcomes[outcome]++;
      st.per_build_trials[s.build]++;
      unsigned long long performed = (unsigned long long)to_long(kv["performed"]);
      st.performed += performed;
      if (performed > 0) sources_note_executed(pace, (size_t)performed);
      if (starts_with(outcome, "saved")) st.per_build_saved[s.build]++;
      if (outcome == "start-failed" || outcome == "error") {
        if (++st.consecutive_fail[s.build] >= CONSECUTIVE_FAIL_STOP && !st.stopped_builds.count(s.build)) {
          st.stopped_builds.insert(s.build);
          logwarn("%s: %d trials in a row could not start, no more trials for this build", s.build.c_str(), CONSECUTIVE_FAIL_STOP);
        }
      } else if (outcome != "options" && outcome != "port-clash") {
        st.consecutive_fail[s.build] = 0;
      }
      string uid = kv.count("uid") ? kv["uid"] : (kv.count("text") ? kv["text"] : "");
      if (kv.count("savefail")) uid += " [not saved: " + replace_all(kv["savefail"], "_", " ") + "]";   // the ledger says why the trial stayed where it was
      string dir = kv.count("dir") ? kv["dir"] : "";
      logline("[T%zu] trial %ld %s %s: %s%s%s", (size_t)(&s - &S[0]), s.trial, s.build.c_str(), kv["area"].c_str(), outcome.c_str(), dir.empty() ? "" : (" " + dir).c_str(), uid.empty() ? "" : (" " + uid).c_str());
      ledger_append("trial", fmt("%ld %s %s %s %s", s.trial, s.build.c_str(), kv["area"].c_str(), outcome.c_str(), uid.c_str()));
      // a new UID goes to the reducer once, the first trial that shows it
      if (starts_with(outcome, "saved")) ensure_mysqld_copy(s.build);
      if (g_cfg.auto_pipeline && starts_with(outcome, "saved") && !uid.empty() && !starts_with(uid, "BACKUP_ISSUE|") && !reduced_uids.count(uid)) {
        KbVerdict v = kb_verdict(kb_search(uid));
        if (v != KbVerdict::Known && v != KbVerdict::KnownAndFixed) { reduced_uids.insert(uid); reduce_queue.push_back(s.trial); }
      }
    } else if (kind == "started") {
      s.server = (pid_t)to_long(kv["pid"]);
      pids_add(s.server, "server", fmt("trial %ld %s", s.trial, s.build.c_str()));
    }
  };
  if (resume) {
    // what the stopped run left unfinished: a saved trial with no reduced testcase, or one with a
    // testcase but no report
    std::error_code ec;
    vector<long> saved;
    for (auto& e : fs::directory_iterator(g_wd.dir, ec)) {
      string n = e.path().filename().string();
      if (e.is_directory(ec) && is_digits(n) && file_exists(e.path().string() + "/MYBUG")) saved.push_back(to_long(n, 0));
    }
    std::sort(saved.begin(), saved.end());
    for (long n : saved) {
      string td = g_wd.dir + "/" + std::to_string(n);
      string uid = trim(read_file(td + "/MYBUG"));
      uid = uid.substr(0, uid.find('\n'));
      if (uid.empty() || starts_with(uid, "Assert:")) continue;
      KbVerdict v = kb_verdict(kb_search(uid));
      if (v == KbVerdict::Known || v == KbVerdict::KnownAndFixed) continue;
      bool has_out = false;
      for (auto& f : fs::directory_iterator(td, ec)) {
        string fn = f.path().filename().string();
        if (fn.find("_out") != string::npos && !ends_with(fn, ".prev")) has_out = true;
      }
      bool reported = file_exists(g_wd.dir + fmt("/bug%ld.report", n));
      if (!reduced_uids.insert(uid).second) continue;
      if (!g_cfg.auto_pipeline) continue;                     // AUTO_PIPELINE=0: reduce and report are asked for by hand
      if (!has_out) reduce_queue.push_back(n);
      else if (!reported) start_job("report", n);
    }
    logline("resumed %s: %zu saved trial(s), %zu queued for reduction, trials carry on at %ld",
            g_wd.id.c_str(), saved.size(), reduce_queue.size(), next_trial);
  }
  logline("starting");
  status();
  for (;;) {
    int sc = stopc.load();
    if (sc >= 2) {
      logwarn("second stop request: killing the running trials%s", jobs.empty() ? "" : " and the reduce/report children");
      for (auto& s : S) if (s.busy) { kill_group(s.c.pid, SIGKILL); child_reap(s.c, 5000); child_close(s.c); pids_remove(s.c.pid); if (s.server > 0) pids_remove(s.server); s.server = 0; s.busy = false; }
      for (auto& j : jobs) { kill_group(j.pid, SIGKILL); waitpid(j.pid, nullptr, 0); pids_remove(j.pid); }
      jobs.clear();
      break;
    }
    reap_jobs();
    // the TUI (and anyone else) asks through the control file: one word, read once
    {
      string ctl = trim(read_file(g_wd.dir + "/omnium.ctl"));
      if (!ctl.empty()) {
        fs::remove(g_wd.dir + "/omnium.ctl");
        if (ctl == "pause") { paused = true; logline("paused: no new trial is launched"); }
        else if (ctl == "resume") { paused = false; logline("resumed"); }
        else if (ctl == "stop") { stopc = 1; logline("stop asked: the running trials%s finish first", jobs.empty() ? "" : " and reduce/report jobs"); }
        else if (ctl == "stop-now") { stopc = 2; logline("stop now asked"); }
        else logwarn("omnium.ctl: %s is not pause, resume, stop or stop-now", ctl.c_str());
      }
    }
    // stopc, not the sc read above: the control file is read after that, so a stop asked on this
    // same turn has to count, or one more trial goes out after the stop
    bool launching = !paused && stopc.load() == 0 && (trials_target == 0 || st.launched < trials_target) && now_s() < run_end;
    if (launching) {
      // a running reducer or matrix is worth a few slots; discovery keeps at least a quarter
      size_t held = std::min(S.size() - std::max<size_t>(1, S.size() / 4), jobs.size() * 4);
      size_t busy_now = 0;
      for (auto& s : S) if (s.busy) busy_now++;
      for (size_t i = 0; i < S.size(); i++) {
        Slot& s = S[i];
        if (s.busy) continue;
        if (busy_now + held >= S.size()) break;
        busy_now++;
        if (trials_target && st.launched >= trials_target) break;
        string why;
        if (!governors_ok(&why)) {
          if (why != last_gov_why || now_s() - last_gov_log > 60) { logwarn("no new trial: %s", why.c_str()); last_gov_why = why; last_gov_log = now_s(); }
          break;
        }
        // the next build not stopped, round robin
        const Basedir* b = nullptr;
        for (size_t k = 0; k < builds.size(); k++) {
          const Basedir& cand = builds[(rr + k) % builds.size()];
          if (!st.stopped_builds.count(cand.name)) { b = &cand; rr = (rr + k + 1) % builds.size(); break; }
        }
        if (!b) { launching = false; break; }
        string mode = mode_fixed;
        if (mode.empty()) {
          int roll = (int)seeds.range(1, 100);
          int backup_pct = b->backup.empty() ? 0 : g_cfg.backup_pct;   // no mariadb-backup in the build: its share runs as normal trials
          if (roll <= g_cfg.multi_thread_pct) mode = "multi";
          else if (roll <= g_cfg.multi_thread_pct + g_cfg.crash_recovery_pct) mode = "crash";
          else if (roll <= g_cfg.multi_thread_pct + g_cfg.crash_recovery_pct + backup_pct) mode = "backup";
          else mode = "normal";
        }
        long trial = next_trial++;
        vector<string> args = {"--basedir", b->path, "--workdir", g_wd.dir, "--rundir", g_wd.rundir, "--trial", std::to_string(trial),
                               "--mode", mode, "--threads", std::to_string(threads), "--seconds", std::to_string(seconds),
                               "--seed", std::to_string(seeds.next()), "--sql-lines", std::to_string(sources_target_lines(pace))};
        if (!area.empty()) { args.push_back("--area"); args.push_back(area); }
        if (no_disk) args.push_back("--no-disk");
        // a trial process loads its own settings: the INFILE of this run goes with it, empty included
        args.push_back("--infile"); args.push_back(g_cfg.infile);
        mkdirs(g_wd.dir + "/log");
        string logfile = g_wd.dir + fmt("/log/trial_%ld.log", trial);
        if (!spawn_role(s.c, "trial", args, logfile)) { logwarn("cannot spawn a trial child"); break; }
        s.busy = true; s.trial = trial; s.build = b->name; s.started = now_s(); s.area = area;
        st.launched++;
        pids_add(s.c.pid, "trial", fmt("trial %ld %s", trial, b->name.c_str()));
      }
    }
    // events and exits
    vector<struct pollfd> pf;
    vector<size_t> idx;
    for (size_t i = 0; i < S.size(); i++) if (S[i].busy && S[i].c.ev_fd >= 0) { pf.push_back({S[i].c.ev_fd, POLLIN, 0}); idx.push_back(i); }
    bool any_busy = false;
    for (auto& s : S) any_busy |= s.busy;
    if (!any_busy && !launching && jobs.empty() && (reduce_queue.empty() || stopc.load() > 0)) break;
    if (!any_busy && !launching) { if (now_s() - last_status >= 5) status(); sleep(1); continue; }
    // nothing is running and the governors hold every new trial back: wait, do not spin
    if (!any_busy && pf.empty()) { if (now_s() - last_status >= 5) status(); sleep(2); continue; }
    if (!pf.empty()) {
      int pr = poll(pf.data(), pf.size(), 500);
      if (pr > 0) {
        for (size_t k = 0; k < pf.size(); k++) {
          if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
          Slot& s = S[idx[k]];
          string line;
          while (s.c.ev_fd >= 0 && child_read(s.c, line, 0)) handle_event(s, line);
        }
      }
    } else {
      usleep(500000);
    }
    for (size_t i = 0; i < S.size(); i++) {
      Slot& s = S[i];
      if (!s.busy) continue;
      int rc = child_reap(s.c, 0);
      if (rc == -1) {
        if (now_s() - s.started > seconds + TRIAL_HARD_CAP_S) {
          logwarn("[T%zu] trial %ld %s: no end after %ld s, killed", i, s.trial, s.build.c_str(), (long)(now_s() - s.started));
          kill_group(s.c.pid, SIGKILL);
          child_reap(s.c, 5000);
          st.outcomes["killed"]++;
          ledger_append("trial", fmt("%ld %s killed", s.trial, s.build.c_str()));
        } else {
          continue;
        }
      }
      string line;
      while (s.c.ev_fd >= 0 && child_read(s.c, line, 0)) handle_event(s, line);
      if (rc > 0 && rc != -1) { st.outcomes["child-rc-" + std::to_string(rc)]++; logwarn("[T%zu] trial %ld %s: child exit %d, see %s", i, s.trial, s.build.c_str(), rc, s.c.logfile.c_str()); }
      pids_remove(s.c.pid);
      if (s.server > 0) pids_remove(s.server);                  // the server ended with its trial
      s.server = 0;
      child_close(s.c);
      s.busy = false;
      st.finished++;
      if (++pool_age >= DISK_POOL_REFRESH_TRIALS && !pool_busy.load()) {
        pool_age = 0;
        if (pool_thread.joinable()) pool_thread.join();         // the last one is done: this returns at once
        pool_busy = true;
        pool_thread = std::thread([&] { sources_shared_refresh_pool(sqlwork, pool_rng); pool_busy = false; });
      }
    }
    if (now_s() - last_status >= 5) status();
  }
  if (!reduce_queue.empty()) logline("%zu queued reduction(s) left for omnium run --resume", reduce_queue.size());
  for (auto& t : copy_threads) if (t.joinable()) t.join();      // the binary copies finish before we go
  if (pool_thread.joinable()) pool_thread.join();
  string sum;
  for (auto& [k, v] : st.outcomes) sum += fmt(" %s=%ld", k.c_str(), v);
  if (stopc.load() >= 2) logwarn("the rundir %s stays for a look; remove it by hand", g_wd.rundir.c_str());
  else remove_tree(g_wd.rundir);
  if (holder.pid > 0) {                                       // ask it to end, then make sure it did
    child_send(holder, "stop");
    if (child_reap(holder, 2000) < 0) { kill(holder.pid, SIGKILL); child_reap(holder, 2000); }
    child_close(holder);
  }
  release_lock();
  logline("run %s done: %ld trials%s", g_wd.id.c_str(), st.finished, sum.c_str());
  ledger_append("run", fmt("ended trials %ld%s", st.finished, sum.c_str()));
  status();
  printf("%s: %ld trials%s\nworkdir %s\n", g_wd.id.c_str(), st.finished, sum.c_str(), g_wd.dir.c_str());
  return 0;
}
