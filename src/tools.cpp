// Created by Roel Van de Paar, MariaDB
// tools.cpp - omnium status (every run on the box, live or finished), omnium adopt (a pquery-run
// workdir's saved trials taken through reduce and report), omnium init (what the box needs, the
// queues, the alias file).
#include "verbs.h"

namespace fs = std::filesystem;

// ---- status ------------------------------------------------------------------------------------
static string kv_get(const vector<std::pair<string, string>>& kv, const string& k) {
  for (auto& p : kv) if (p.first == k) return p.second;
  return "";
}
int cmd_status(const Args& a) {
  vector<string> dirs;
  if (!a.empty()) {
    for (auto& x : a) {
      string d = x;
      if (d.find('/') == string::npos) d = g_cfg.data_dir + "/" + (starts_with(d, "O") ? d : "O" + d);
      dirs.push_back(d);
    }
  } else {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) {
      string n = e.path().filename().string();
      if (n.size() == 7 && n[0] == 'O' && is_digits(n.substr(1)) && e.is_directory(ec)) dirs.push_back(e.path().string());
    }
    std::sort(dirs.begin(), dirs.end());
  }
  if (dirs.empty()) { printf("no omnium run under %s\n", g_cfg.data_dir.c_str()); return 0; }
  int live = 0;
  for (auto& d : dirs) {
    if (!workdir_is_omnium(d)) { printf("%s: not an omnium workdir\n", d.c_str()); continue; }
    auto kv = status_read(d);
    pid_t pid = (pid_t)to_long(trim(read_file(d + "/omnium.pid")), 0);
    bool alive = pid_is_live_omnium(pid);                          // a killed run leaves its pid file; the number can be reused
    if (alive) live++;
    string state = alive ? kv_get(kv, "state") : "finished";
    if (state.empty()) state = alive ? "running" : "finished";
    long saved = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(d, ec)) if (e.is_directory(ec) && is_digits(e.path().filename().string()) && file_exists(e.path().string() + "/MYBUG")) saved++;
    printf("%s  %-9s trials %s/%s  statements %s  saved %ld  ram %s%%  shm %s%%%s\n", basename_of(d).c_str(), state.c_str(),
           kv_get(kv, "finished").c_str(), kv_get(kv, "launched").c_str(), kv_get(kv, "performed").c_str(), saved,
           kv_get(kv, "ram_pct").c_str(), kv_get(kv, "shm_pct").c_str(), alive ? fmt("  pid %d", (int)pid).c_str() : "");
    for (auto& p : kv) {
      if (starts_with(p.first, "outcome_")) printf("    %-24s %s\n", p.first.substr(8).c_str(), p.second.c_str());
    }
    for (auto& p : kv) {
      if (starts_with(p.first, "build_")) printf("    %s: %s\n", p.first.substr(6).c_str(), p.second.c_str());
    }
  }
  printf("%d run%s live\n", live, live == 1 ? "" : "s");
  return 0;
}

// ---- adopt -------------------------------------------------------------------------------------
// omnium adopt <workdir> [--reduce] [--report] [--screen]: the saved trials of a pquery-run workdir
// (or an omnium one), one line each with the UID and the known-bugs verdict; --reduce starts a
// reduction per new UID (the smallest trace of each), --report writes a report for each trial that
// has a reduced testcase
int cmd_adopt(const Args& a) {
  string wd;
  bool do_reduce = false, do_report = false, screen = false;
  for (auto& s : a) {
    if (s == "--reduce") do_reduce = true;
    else if (s == "--report") do_report = true;
    else if (s == "--screen") screen = true;
    else if (starts_with(s, "--")) { fprintf(stderr, "omnium adopt <workdir> [--reduce] [--report] [--screen]\n"); return 2; }
    else wd = s;
  }
  if (wd.empty()) { fprintf(stderr, "omnium adopt <workdir> [--reduce] [--report] [--screen]\n"); return 2; }
  if (wd.find('/') == string::npos) {
    if (dir_exists(g_cfg.data_dir + "/" + wd)) wd = g_cfg.data_dir + "/" + wd;
    else if (is_digits(wd) && dir_exists(g_cfg.data_dir + "/O" + wd)) wd = g_cfg.data_dir + "/O" + wd;
  }
  wd = abs_path(wd);
  if (!dir_exists(wd)) { fprintf(stderr, "omnium adopt: no such dir %s\n", wd.c_str()); return 1; }
  struct T { long n; string uid; size_t trace; bool reduced; };
  vector<T> trials;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(wd, ec)) {
    string n = e.path().filename().string();
    if (!e.is_directory(ec) || !is_digits(n)) continue;
    string uid = trim(read_file(e.path().string() + "/MYBUG"));
    uid = uid.substr(0, uid.find('\n'));
    if (uid.empty()) continue;
    T t;
    t.n = to_long(n, 0);
    t.uid = uid;
    t.trace = (size_t)fs::file_size(e.path() / "default.node.tld_thread-0.sql", ec);
    t.reduced = false;
    for (auto& f : fs::directory_iterator(e.path(), ec)) { string fn = f.path().filename().string(); if (fn.find("_out") != string::npos && !ends_with(fn, ".prev")) t.reduced = true; }
    trials.push_back(t);
  }
  std::sort(trials.begin(), trials.end(), [](const T& x, const T& y) { return x.n < y.n; });
  if (trials.empty()) { printf("%s: no saved trial with a MYBUG\n", wd.c_str()); return 0; }
  std::map<string, long> smallest;                             // uid -> the trial with the smallest trace
  std::map<string, int> count;
  for (auto& t : trials) {
    count[t.uid]++;
    auto it = smallest.find(t.uid);
    if (it == smallest.end()) smallest[t.uid] = t.n;
    else { for (auto& o : trials) if (o.n == it->second && t.trace < o.trace) it->second = t.n; }
  }
  printf("%s: %zu saved trial%s, %zu UID%s%s\n", wd.c_str(), trials.size(), trials.size() == 1 ? "" : "s", smallest.size(),
         smallest.size() == 1 ? "" : "s", workdir_is_omnium(wd) ? "" : " (pquery-run layout)");
  vector<long> reduce_list, report_list;
  for (auto& [uid, n] : smallest) {
    KbVerdict v = kb_verdict(kb_search(uid));
    const char* verdict = v == KbVerdict::Known || v == KbVerdict::KnownAndFixed ? "known" : v == KbVerdict::FixedOnly ? "fixed-only (new)" : v == KbVerdict::Partial ? "partial match" : "new";
    bool reduced = false;
    for (auto& t : trials) if (t.n == n) reduced = t.reduced;
    printf("  trial %-6ld x%-3d %-16s %s%s\n", n, count[uid], verdict, uid.c_str(), reduced ? "  [reduced]" : "");
    if (v != KbVerdict::Known && v != KbVerdict::KnownAndFixed) {
      if (!reduced) reduce_list.push_back(n); else report_list.push_back(n);
    }
  }
  string exe;
  { char buf[4096]; ssize_t k = readlink("/proc/self/exe", buf, sizeof(buf) - 1); exe = k > 0 ? string(buf, k) : "omnium"; }
  int rc = 0;
  if (do_reduce) {
    for (long n : reduce_list) {
      vector<string> argv = {exe, "reduce", wd, std::to_string(n)};
      if (screen) argv.push_back("--screen");
      printf("reduce: %s\n", join(argv, " ").c_str());
      CmdResult r = run_capture(argv, screen ? 120 : 0);
      if (r.rc != 0) { printf("  failed: %s\n", tail_lines(r.out, 3).c_str()); rc = 1; }
      else if (!screen) printf("  %s\n", tail_lines(r.out, 1).c_str());
    }
  }
  if (do_report) {
    for (long n : report_list) {
      vector<string> argv = {exe, "report", wd, std::to_string(n)};
      printf("report: %s\n", join(argv, " ").c_str());
      CmdResult r = run_capture(argv, 0);
      fputs(r.out.c_str(), stdout);
      if (r.rc != 0) rc = 1;
    }
  }
  if (!do_reduce && !do_report) printf("next: omnium adopt %s --reduce [--screen]; then omnium adopt %s --report\n", wd.c_str(), wd.c_str());
  return rc;
}

// ---- init --------------------------------------------------------------------------------------
// what a box needs for omnium, checked and said in plain words; the queues made; the alias file written
// The binary by its full path, so the file works with omnium off the PATH. o is not one of them: it
// is the framework's newest optimised build, and the cli's o is the same
static string aliases_text() {
  string bin = g_paths.repo + "/omnium";
  string s = "# omnium shortcuts, written by omnium init; source from ~/.bashrc: [ -r ~/.omnium_aliases ] && . ~/.omnium_aliases\n";
  for (auto [name, verb] : std::initializer_list<std::pair<const char*, const char*>>{
         {"orun", "run"}, {"ost", "status"}, {"ot", "t"}, {"ott", "tt"}, {"osr", "reduce"}, {"orep", "report"}, {"omx", "matrix"},
         {"oin", "inbox"}, {"oad", "adopt"}, {"obuilds", "builds"}, {"ostack", "stack"}, {"otui", "tui"}, {"ocli", "cli"},
         {"omtr", "mtr"}, {"ofr", "fresh"}, {"ocl", "cl"}, {"orp", "replay"}, {"oi", "trial"}})
    s += fmt("alias %s='%s %s'\n", name, bin.c_str(), verb);
  return s;
}
int cmd_init(const Args& a) {
  bool fix = false;
  for (auto& s : a) { if (s == "--fix") fix = true; }
  int missing = 0;
  auto check = [&](bool ok, const string& what, const string& how) {
    printf("%s %s\n", ok ? "ok  " : "MISS", what.c_str());
    if (!ok) { missing++; if (!how.empty()) printf("       %s\n", how.c_str()); }
  };
  check(is_executable("/usr/bin/clang++") || !run_capture({"which", "clang++"}, 10).out.empty(), "clang++", "sudo apt install clang lld llvm");
  check(!run_capture({"which", "ninja"}, 10).out.empty(), "ninja", "sudo apt install ninja-build");
#ifdef __linux__
  check(!run_capture({"which", "gdb"}, 10).out.empty(), "gdb", "sudo apt install gdb");
  check(!run_capture({"which", "screen"}, 10).out.empty(), "screen", "sudo apt install screen");
#endif
  if (g_paths.qa == g_paths.repo + "/mariadb-qa") {
    // omnium's own sparse clone of the framework: made here the first time, pulled after that
    CmdResult r = run_capture({g_paths.repo + "/qa_checkout.sh", g_paths.qa}, 3600);
    string last = replace_all(trim(tail_lines(r.out, 1)), "[qa_checkout.sh] ", "");
    bool there = r.rc == 0 || r.rc == 2;                       // 2 = there but not refreshed, still usable
    check(there, "the mariadb-qa clone " + (there ? last : "at " + g_paths.qa),
          there ? "" : last.empty() ? "could not run " + g_paths.repo + "/qa_checkout.sh" : last);
  } else {
    check(dir_exists(g_paths.qa), "the mariadb-qa checkout at " + g_paths.qa, "clone it there, or set QA_DIR= (empty) and omnium init makes a sparse clone beside the binary");
  }
  check(file_exists(g_paths.known_bugs), "known_bugs.strings", "part of the mariadb-qa checkout");
  check(file_exists(script_path("new_text_string.sh")), "the UID scripts (new_text_string.sh and friends) in " + g_paths.qa, "clone mariadb-qa there, or point QA_DIR at the checkout");
  check(file_exists(script_path("reducercpp/stages.tbl")), "stages.tbl (the reducer's sed stages)", "it comes with the mariadb-qa checkout at QA_DIR");
  check(file_exists(g_paths.sql_filter), "filter.sql (the SQL the trials leave out)", "it comes with the mariadb-qa checkout at QA_DIR");
  check(dir_exists(g_cfg.test_dir), "the build dir " + g_cfg.test_dir, "sudo mkdir " + g_cfg.test_dir + " && sudo chown $USER " + g_cfg.test_dir);
  check(dir_exists(g_cfg.data_dir), "the results dir " + g_cfg.data_dir, "sudo mkdir " + g_cfg.data_dir + " && sudo chown $USER " + g_cfg.data_dir);
  check(dir_exists(g_cfg.shm_dir) && dir_total_bytes(g_cfg.shm_dir) >= ram_total_bytes() / 4,
        "/dev/shm is " + human_bytes(dir_total_bytes(g_cfg.shm_dir)) + " of " + human_bytes(ram_total_bytes()) + " RAM (trials run there; the framework sizes it to about 80%)",
        "mount -o remount,size=80% /dev/shm (see ~/mariadb-qa/setup_server.sh)");
  {
    string pat = read_file(g_cfg.pat_file);
#ifdef __linux__
    string core = trim(read_file("/proc/sys/kernel/core_pattern"));
    check(!core.empty() && core[0] != '|' && core.find("core") != string::npos, "kernel.core_pattern writes a core file next to the server (" + core + ")",
          "sudo sysctl -w kernel.core_pattern=core (see ~/mariadb-qa/setup_server.sh)");
#endif
    check(!trim(pat).empty(), "a Jira PAT in " + g_cfg.pat_file, "jira.mariadb.org: avatar, Profile, Personal Access Tokens; then (umask 077; printf '%s\\n' '<token>' > " + g_cfg.pat_file + ")");
  }
  {
    Registry r;
    check(registry_load(r) && !r.entries.empty(), "the build registry " + g_paths.builds_file, "omnium builds (after at least one build under " + g_cfg.test_dir + ", or omnium build 13.1)");
  }
  mkdirs(g_paths.human_queue);
  mkdirs(g_paths.ai_queue);
  printf("ok   queues %s and %s\n", g_paths.human_queue.c_str(), g_paths.ai_queue.c_str());
  if (!file_exists(config_path())) { write_file(config_path(), config_dump()); printf("ok   settings written to %s\n", config_path().c_str()); }
  else if (!config_missing_keys().empty()) {                  // a file from before a key was added: the new keys join it
    vector<string> missing = config_missing_keys();
    config_write_keys(missing);
    printf("ok   settings in %s, new keys added: %s\n", config_path().c_str(), join(missing, ", ").c_str());
  }
  else printf("ok   settings in %s\n", config_path().c_str());
  if (read_file(g_paths.aliases_file) != aliases_text() || fix) {
    write_file(g_paths.aliases_file, aliases_text());
    printf("ok   aliases written to %s (source it from ~/.bashrc)\n", g_paths.aliases_file.c_str());
  } else {
    printf("ok   aliases in %s (omnium init --fix rewrites them)\n", g_paths.aliases_file.c_str());
  }
  if (missing) printf("%d item%s missing; the lines above say what to do\n", missing, missing == 1 ? "" : "s");
  else printf("everything omnium needs is in place\n");
  return missing ? 1 : 0;
}
