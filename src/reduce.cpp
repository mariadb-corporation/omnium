// Created by Roel Van de Paar, MariaDB
// reduce.cpp - omnium reduce: prepares one saved trial the way pquery-prep-red.sh does (the mode and
// the text from MYBUG, the failing and trigger queries, the trace clean-up, the multi-thread
// variants) and runs the reducer on it. The reducer is ~/mariadb-qa/reducercpp/reducer.cpp compiled
// into this binary (--role reducer); it keeps its own env-var contract, so the plan below is handed
// over as environment variables. The reduced testcase lands beside the input as <input>_out.
#include "verbs.h"
#include <regex>
#include <sys/stat.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace {
struct Plan {
  string workdir, tdir;
  long trial = 0;
  string basedir, bin, errlog, core;
  string inputfile, text, myextra, myinit;
  int mode = 3, use_nts = 1, scan_new = 1;
  bool multi = false, san = false, startup = false, legacy = false;
  int threads = 1;
  string variant;                       // "" (as the trial ran), quick, onethd, onethd-rnd
  string mode11_type, mode11_blf;
  bool skipv = false, sporadic = false;
  int repeats = 1, stage1_lines = 0;
  vector<std::pair<string, string>> env;
};

vector<string> dir_files(const string& dir, const std::function<bool(const string&)>& keep) {
  vector<string> v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    string n = e.path().filename().string();
    if (keep(n)) v.push_back(e.path().string());
  }
  std::sort(v.begin(), v.end());
  return v;
}
// the trace files: default.node.tld_thread-N.sql, or whatever *thread-N.sql an old run left
vector<string> trace_files(const string& tdir) {
  static const std::regex re("thread-[0-9]+\\.sql$");
  return dir_files(tdir, [&](const string& n) { return std::regex_search(n, re); });
}
string thread0_trace(const string& tdir) {
  string p = tdir + "/default.node.tld_thread-0.sql";
  if (file_exists(p)) return p;
  for (auto& f : trace_files(tdir)) if (ends_with(f, "thread-0.sql")) return f;
  return "";
}
string find_core(const string& tdir) {
  for (const string& d : {tdir + "/data", tdir}) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(d, ec)) {
      if (!e.is_regular_file(ec)) continue;
      string n = e.path().filename().string();
      if (n.find("core") != string::npos && !ends_with(n, ".txt") && !ends_with(n, ".gdb") && e.file_size(ec) > 0) return e.path().string();
    }
  }
  return "";
}
// the first known-bugs line that carries the UID (grep -Fi, as kb_line_has reads it); a line starting with # is a fixed bug
bool uid_in_known_lists(const string& uid) {
  if (uid.empty()) return false;
  for (const string& f : {g_paths.known_bugs, g_paths.known_bugs_san}) {
    for (auto& l : split_lines(read_file(f))) {
      if (!kb_line_has(l, uid)) continue;
      return !starts_with(trim(l), "#");
    }
  }
  return false;
}

// ---- the failing and trigger queries (pquery-failing-sql.sh) --------------------------------------
// <trial>.sql.failing: the queries the crashed threads were running (gdb on the core), the
// "Query (0x..):" lines of the error log, the trace lines that lost their connection, SELECT 1 and
// SELECT SLEEP(2); all of that three times, then SHUTDOWN.
vector<string> failing_from_core(const Plan& p) {
  vector<string> out;
  if (p.core.empty() || p.bin.empty()) return out;
  string parse = p.tdir + "/gdb_PARSE.txt";
  fs::remove(parse);
  string script = read_file(script_path("extract_query.gdb"));
  if (script.empty()) {
    script = "set debuginfod enabled off\nset auto-load safe-path /\nset libthread-db-search-path /usr/lib/\nset trace-commands on\n"
             "set pagination off\nset print pretty on\nset print elements 65536\nset print repeats 999999999\n"
             "set logging file /tmp/gdb_PARSE.txt\nset logging enabled on\n";
    for (int t = 1; t <= 102; t++)
      script += fmt("t %d\nprint do_command::thd->query_string.string.str\nprint do_command::thd->m_query_string.str\nprint dispatch_command::packet\n", t);
    script += "set logging off\nquit\n";
  }
  script = replace_all(script, "file /tmp/gdb_PARSE.txt", "file " + parse);
  run_capture_in({"gdb", "-iex", "set debuginfod enabled off", p.bin, p.core}, script, 900, "", {"DEBUGINFOD_TIMEOUT=13", "DEBUGINFOD_PROGRESS=0"});
  // $1 = 0x7f... "SELECT 1"  ->  SELECT 1; ;   and a $ line without a string is dropped
  static const std::regex lead("^[$0-9a-fx =]*\"");
  for (auto& l : split_lines(read_file(parse))) {
    if (l.empty() || l[0] != '$') continue;
    string s = std::regex_replace(l, lead, "", std::regex_constants::format_first_only);
    if (!s.empty() && s.back() == '"') s.pop_back();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    s = replace_all(s, "\\\"", "\"");
    s += "; ;";
    if (!s.empty() && s[0] == '$') continue;
    out.push_back(s);
  }
  fs::remove(parse);
  return out;
}
vector<string> failing_from_errlog(const Plan& p) {
  vector<string> out;
  static const std::regex re("^Query \\([x0-9a-fA-F]*\\): ");
  for (auto& l : split_lines(read_file(p.errlog))) {
    if (!std::regex_search(l, re)) continue;
    out.push_back(std::regex_replace(l, re, "") + "; ;");
  }
  // a lone "Connection ID (thread ID): N" carries no query
  string joined = join(out, " ");
  joined = std::regex_replace(joined, std::regex(": [0-9]+"), ": 0");
  if (joined == "Connection ID (thread ID): 0; ;") out.clear();
  return out;
}
vector<string> lost_connection_lines(const vector<string>& traces) {
  vector<string> out;
  for (auto& f : traces)
    for (auto& l : split_lines(read_file(f)))
      if (icontains(l, "lost connection to server during query") && !starts_with(l, "/data/") && !starts_with(l, "/test/")) out.push_back(l);
  return out;
}
vector<string> build_failing(const Plan& p) {
  vector<string> v = failing_from_core(p);
  for (auto& l : failing_from_errlog(p)) v.push_back(l);
  for (auto& l : lost_connection_lines(trace_files(p.tdir))) v.push_back(l);
  v.push_back("SELECT 1;");
  v.push_back("SELECT SLEEP(2);");
  size_t n = v.size();
  for (int k = 0; k < 2; k++) for (size_t i = 0; i < n; i++) v.push_back(v[i]);
  v.push_back("SHUTDOWN;");
  write_file(p.tdir + "/" + std::to_string(p.trial) + ".sql.failing", join(v, "\n") + "\n");
  return v;
}

// ---- the reducer input ----------------------------------------------------------------------------
// the diagnostic lines the client wrote into the trace are not SQL
void remove_non_sql(vector<string>& lines) {
  static const std::regex re("Last [0-9]+ consecutive queries all failed");
  vector<string> out;
  for (auto& l : lines) if (!std::regex_search(l, re)) out.push_back(l);
  lines.swap(out);
}
// sed "0~N r failing": the failing set after every Nth line, N from the sizes of both
void interleave_failing(vector<string>& lines, const vector<string>& failing) {
  if (failing.empty()) return;
  size_t n = lines.size();
  int every;
  if (failing.size() < 10) every = n <= 100 ? 3 : n <= 500 ? 15 : n <= 1000 ? 35 : 50;
  else every = n <= 100 ? 5 : n <= 500 ? 25 : n <= 1000 ? 50 : 75;
  vector<string> out;
  out.reserve(n + (n / every + 1) * failing.size());
  for (size_t i = 0; i < n; i++) {
    out.push_back(lines[i]);
    if ((i + 1) % every == 0) for (auto& f : failing) out.push_back(f);
  }
  lines.swap(out);
}
// the input file is rewritten from its .backup each time, so a re-run does not grow it
bool prepare_input(Plan& p, const vector<string>& failing, string* err) {
  string backup = p.inputfile + ".backup";
  if (!file_exists(backup)) { if (!copy_file(p.inputfile, backup)) { *err = "cannot write " + backup; return false; } }
  vector<string> lines = split_lines(read_file(backup));
  remove_non_sql(lines);
  for (auto& f : failing) lines.push_back(f);
  if (p.multi) interleave_failing(lines, failing);
  if (!write_file(p.inputfile, join(lines, "\n") + "\n")) { *err = "cannot write " + p.inputfile; return false; }
  return true;
}
// quick_<trial>.sql: every thread's executed SQL, the failing set four times, the lost-connection
// lines of thread 0 three times, then the interleave; reduced as one random-order replay
bool prepare_quick(Plan& p, const vector<string>& failing, string* err) {
  vector<string> lines;
  for (auto& f : trace_files(p.tdir)) for (auto& l : split_lines(read_file(f))) lines.push_back(l);
  for (int k = 0; k < 3; k++) for (auto& f : failing) lines.push_back(f);
  string t0 = thread0_trace(p.tdir);
  if (!t0.empty()) { auto lost = lost_connection_lines({t0}); for (int k = 0; k < 3; k++) for (auto& l : lost) lines.push_back(l); }
  for (auto& f : failing) lines.push_back(f);
  remove_non_sql(lines);
  interleave_failing(lines, failing);
  p.inputfile = p.tdir + "/quick_" + std::to_string(p.trial) + ".sql";
  if (!write_file(p.inputfile, join(lines, "\n") + "\n")) { *err = "cannot write " + p.inputfile; return false; }
  return true;
}

// ---- the mode and the text (pquery-prep-red.sh generate_reducer_script) ----------------------------
bool san_marker_in_logs(const vector<string>& logs) {
  for (auto& l : capped_logs(logs)) {
    string s = read_file(l);
    if (s.find("=ERROR:") != string::npos || icontains(s, "ThreadSanitizer:") || icontains(s, "runtime error:") ||
        s.find("LeakSanitizer:") != string::npos || s.find("MemorySanitizer:") != string::npos) return true;
  }
  return false;
}
void derive_mode(Plan& p) {
  const string& t = p.text;
  bool too_general = t.empty() || t == "my_print_stacktrace" || t == "0" || t == "NULL" ||
                     (t == "Assert: no core file found in */*core*" && !p.san);
  if (too_general) { p.mode = 4; p.use_nts = 0; p.scan_new = 0; return; }
  if (p.san) { p.mode = 3; p.use_nts = 1; p.scan_new = 1; return; }
  if (starts_with(t, "BINLOG_RECOVERY_ERROR") || starts_with(t, "BINLOG_CHECKSUM_DIFF")) {
    p.mode = 11; p.mode11_type = "binlog";
    string m = " " + p.myextra + " ";
    p.mode11_blf = m.find("--binlog_format=ROW") != string::npos || m.find("--binlog-format=ROW") != string::npos ? "ROW"
                 : m.find("--binlog_format=STATEMENT") != string::npos || m.find("--binlog-format=STATEMENT") != string::npos ? "STATEMENT" : "MIXED";
    p.use_nts = 0; p.scan_new = 0;
    return;
  }
  // MEMORY_NOT_FREED, GOT_ERROR, MARKED_AS_CRASHED, MARIADB_ERROR_CODE and every plain UID: the
  // reducer compares each replay's UID with the text
  p.mode = 3; p.use_nts = 1; p.scan_new = 1;
}
// a known nts UID, or "no core found", with an ERROR_LOG_SCAN_ISSUE flag: the error-log UID is the
// bug this trial was kept for, so it becomes the text (and MYBUG, the original kept as MYBUG.orig)
void errlog_override(Plan& p) {
  bool findbug = uid_in_known_lists(p.text);
  bool has_flag = file_exists(p.tdir + "/ERROR_LOG_SCAN_ISSUE");
  if (!findbug && has_flag) {
    static const std::regex nofound("No .* found", std::regex::icase);
    if (std::regex_search(p.text, nofound) || !file_exists(p.tdir + "/MYBUG")) findbug = true;
  }
  if (!findbug) return;
  string out, err;
  vector<string> logs = {p.errlog};
  els_run("top", logs, false, out, &err);
  string uid = trim(out);
  static const std::regex sig("(^|\\|)SIG[A-Z]+\\|");
  if (!uid.empty() && starts_with(uid, "ASSERT|") && std::regex_search(p.text, sig)) {
    out.clear();
    els_run("top", logs, true, out, &err);
    uid = trim(out);
    if (uid.empty()) printf("* override skipped: the UID has frames and the error log only adds an ASSERT| shadow\n");
  }
  if (uid.empty()) return;
  if (file_exists(p.tdir + "/MYBUG")) copy_file(p.tdir + "/MYBUG", p.tdir + "/MYBUG.orig");
  write_file(p.tdir + "/MYBUG", uid + "\n");
  p.text = uid;
  p.mode = 3; p.use_nts = 1;
  printf("* text set to the error-log UID: %s\n", uid.c_str());
}

// ---- the plan -------------------------------------------------------------------------------------
bool plan_trial(Plan& p, string* err) {
  p.tdir = p.workdir + "/" + std::to_string(p.trial);
  if (!dir_exists(p.tdir)) { *err = "no trial dir " + p.tdir; return false; }
  p.legacy = !workdir_is_omnium(p.workdir);
  if (file_exists(p.tdir + "/VALGRIND")) { *err = "a Valgrind trial; not supported yet"; return false; }
  if (file_exists(p.tdir + "/diff.result")) { *err = "a query-correctness trial; not supported yet"; return false; }
  p.errlog = p.tdir + "/log/master.err";
  // the server that ran it
  if (p.basedir.empty()) p.basedir = trim(read_file(p.tdir + "/BASEDIR"));
  if (p.basedir.empty()) {
    for (auto& l : split_lines(read_file(p.workdir + "/pquery-run.log"))) {
      size_t k = l.find("Basedir:");
      if (k == string::npos) continue;
      string v = trim(l.substr(k + 8));
      size_t bar = v.find('|');
      if (bar != string::npos) v = trim(v.substr(0, bar));
      p.basedir = v;
      break;
    }
  }
  if (p.basedir.empty()) { *err = "no BASEDIR file in the trial and no Basedir: line in pquery-run.log; give --basedir"; return false; }
  if (!dir_exists(p.basedir)) { *err = "basedir " + p.basedir + " is not there (moved to /data/VARIOUS_BUILDS?)"; return false; }
  for (const string& c : {p.workdir + "/mysqld/mariadbd", p.workdir + "/mysqld/mysqld", p.basedir + "/bin/mariadbd", p.basedir + "/bin/mysqld"})
    if (file_exists(c)) { p.bin = c; break; }
  if (p.bin.empty()) p.bin = binary_for_dir(p.tdir);
  p.core = find_core(p.tdir);
  // threads
  string seed = read_file(p.tdir + "/SEED");
  for (auto& l : split_lines(seed)) if (starts_with(l, "threads=")) p.threads = (int)to_long(l.substr(8), 1);
  int traces = (int)trace_files(p.tdir).size();
  if (traces > 1) p.threads = std::max(p.threads, traces);
  p.multi = p.threads > 1;
  if (p.variant == "onethd" || p.variant == "onethd-rnd") p.multi = false;
  p.startup = file_exists(p.tdir + "/SERVER_START_FAILED") ||
              !dir_files(p.tdir, [](const string& n) { return n.find("startup_failure_thread-") != string::npos; }).empty();
  // the options the server ran with: MYSAFE and MYEXTRA together, MYINIT apart
  string mysafe = trim(read_file(p.tdir + "/MYSAFE")), myextra = trim(read_file(p.tdir + "/MYEXTRA"));
  {
    vector<string> opts;
    for (auto& o : split_ws(mysafe + " " + myextra)) if (std::find(opts.begin(), opts.end(), o) == opts.end()) opts.push_back(o);
    p.myextra = join(opts, " ");
  }
  p.myinit = trim(read_file(p.tdir + "/MYINIT"));
  // the UID; regenerated when missing or when the stored one is an assert of the chain itself
  string mybug = p.tdir + "/MYBUG";
  string uid = trim(read_file(mybug));
  if (p.text.empty()) {
    if (uid.empty() || starts_with(uid, "Assert:")) {
      UidResult r;
      UidOptions o;
      o.wait_core = false;
      if (uid_for_dir(p.tdir, r, o) && !r.uid.empty()) { uid = r.uid; write_file(mybug, uid + "\n"); }
    }
    auto lines = split_lines(uid);
    p.text = lines.empty() ? "" : trim(lines[0]);
    // a SAN report with no core: the sanitizer UID is the text
    if (p.text.find("Assert: no core file found in") != string::npos && san_marker_in_logs({p.errlog})) {
      string e;
      string s = trim(uid_san({p.errlog}, &e));
      if (!s.empty()) { p.text = s; p.san = true; }
    }
    if (!p.san) p.san = kb_uid_is_san(p.text) && p.text.find('|') != string::npos && (starts_with(p.text, "ASAN|") || starts_with(p.text, "UBSAN|") || starts_with(p.text, "TSAN|") || starts_with(p.text, "MSAN|") || starts_with(p.text, "LSAN|"));
  }
  derive_mode(p);
  if (p.mode != 4 && p.variant.empty()) errlog_override(p);
  // the input
  if (p.multi || p.variant == "onethd" || p.variant == "onethd-rnd" || p.variant == "quick") {
    string f = p.tdir + "/" + std::to_string(p.trial) + ".sql";
    p.inputfile = file_exists(f) ? f : thread0_trace(p.tdir);
  } else {
    p.inputfile = thread0_trace(p.tdir);
  }
  if (p.inputfile.empty()) { *err = "no SQL trace (*thread-0.sql) in " + p.tdir; return false; }
  vector<string> failing = build_failing(p);
  bool ok = (p.variant.empty() || p.variant == "multi") ? prepare_input(p, failing, err) : prepare_quick(p, failing, err);
  if (!ok) return false;
  // the reducer's variables
  auto set = [&](const string& k, const string& v) { p.env.push_back({k, v}); };
  set("INPUTFILE", p.inputfile);
  set("MODE", std::to_string(p.mode));
  set("TEXT", p.text);
  set("USE_NEW_TEXT_STRING", std::to_string(p.use_nts));
  set("SCAN_FOR_NEW_BUGS", std::to_string(p.scan_new));
  set("MYEXTRA", p.myextra);
  set("MYINIT", p.myinit);
  set("BASEDIR", p.basedir);
  set("DISABLE_TOKUDB_AUTOLOAD", file_exists(p.basedir + "/lib/mysql/plugin/ha_tokudb.so") ? "0" : "1");
  // The reducer refuses a tmpfs workdir when the server runs with O_DIRECT (it fails on tmpfs), so
  // such a trial is reduced on the disk the trial is on. The reducer checks that disk's free space
  // by grepping df for the directory, so it has to be the mount point; the work goes in <mount>/<epoch>.
  if (lower(p.myextra).find("o_direct") != string::npos) { set("WORKDIR_LOCATION", "3"); set("WORKDIR_M3_DIRECTORY", mount_point_of(p.tdir)); }
  else set("WORKDIR_LOCATION", "1");
  set("SCRIPT_PWD", g_paths.qa);
  set("TEXT_STRING_LOC", script_path("new_text_string.sh"));
  set("KNOWN_BUGS_LOC", g_paths.known_bugs);
  set("PQUERY_LOC", g_paths.qa + "/pquery/pquery2-md");
  set("REDUCER_STAGES_TBL", script_path("reducercpp/stages.tbl"));
  set("NR_OF_TRIAL_REPEATS", std::to_string(p.repeats));
  if (p.stage1_lines > 0) set("STAGE1_LINES", std::to_string(p.stage1_lines));
  if (p.startup) set("REDUCE_STARTUP_ISSUES", "1");
  if (p.mode == 11) { set("MODE11_TYPE", p.mode11_type); set("MODE11_BINLOG_FORMAT", p.mode11_blf); }
  bool multi_replay = p.multi && p.variant != "onethd" && p.variant != "onethd-rnd";
  if (multi_replay) { set("PQUERY_MULTI", "1"); p.skipv = true; p.sporadic = true; }
  if (p.variant == "quick" || p.variant == "onethd" || p.variant == "onethd-rnd") { set("MULTI_THREADS", "10"); set("PQUERY_MULTI_CLIENT_THREADS", "20"); p.skipv = true; p.sporadic = true; }
  if (p.variant == "onethd-rnd") set("PQUERY_REVERSE_NOSHUFFLE_OPT", "1");
  if (p.skipv) set("FORCE_SKIPV", "1");
  if (p.sporadic) set("FORCE_SPORADIC", "1");
  if (file_exists(p.workdir + "/pquery-run.log") && icontains(read_file(p.workdir + "/pquery-run.log"), "MDG Encryption run: YES")) set("ENCRYPTION_RUN", "1");
  return true;
}

string exe_path() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "omnium";
  buf[n] = 0;
  return buf;
}
// <trial>/reduce: the re-run stub; the reducer also wants a readable file as its own name
string write_stub(const Plan& p, const vector<string>& flags) {
  string stub = p.tdir + "/reduce";
  string line = sh_quote(exe_path()) + " reduce " + sh_quote(p.workdir) + " " + std::to_string(p.trial);
  for (auto& f : flags) line += " " + sh_quote(f);
  write_file(stub, "#!/bin/bash\n# reduces this trial again with omnium; extra flags are passed on\nexec " + line + " \"$@\"\n");
  chmod(stub.c_str(), 0755);
  return stub;
}
void write_conf(const Plan& p) {
  string s;
  for (auto& kv : p.env) s += kv.first + "=" + kv.second + "\n";
  write_file(p.tdir + "/reduce.conf", s);
}
}  // namespace

// omnium reduce [<workdir>] <trial> [--mode M] [--text T] [--basedir DIR] [--variant quick|onethd|onethd-rnd]
//               [--skipv] [--sporadic] [--repeats N] [--stage1-lines N] [--screen] [--plan]
int cmd_reduce(const Args& a) {
  Plan p;
  vector<string> pos, flags;
  bool screen = false, plan_only = false;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--mode") { p.mode = (int)to_long(val(s), 3); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--text") { p.text = val(s); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--basedir") { p.basedir = val(s); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--variant") { p.variant = val(s); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--repeats") { p.repeats = (int)to_long(val(s), 1); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--stage1-lines") { p.stage1_lines = (int)to_long(val(s), 0); flags.push_back(s); flags.push_back(a[i]); }
    else if (s == "--skipv") { p.skipv = true; flags.push_back(s); }
    else if (s == "--sporadic") { p.sporadic = true; flags.push_back(s); }
    else if (s == "--screen") screen = true;
    else if (s == "--plan") plan_only = true;
    else if (starts_with(s, "--")) { fprintf(stderr, "omnium reduce: unknown flag %s\n", s.c_str()); return 2; }
    else pos.push_back(s);
  }
  if (pos.empty() || pos.size() > 2) { fprintf(stderr, "usage: omnium reduce [<workdir>] <trial> [flags]; omnium help reduce\n"); return 2; }
  if (!(p.variant.empty() || p.variant == "quick" || p.variant == "onethd" || p.variant == "onethd-rnd")) { fprintf(stderr, "--variant takes quick, onethd or onethd-rnd\n"); return 2; }
  string wd = pos.size() == 2 ? pos[0] : ".";
  if (wd.find('/') == string::npos && wd != ".") {
    if (dir_exists(g_cfg.data_dir + "/" + wd)) wd = g_cfg.data_dir + "/" + wd;
    else if (is_digits(wd) && dir_exists(g_cfg.data_dir + "/O" + wd)) wd = g_cfg.data_dir + "/O" + wd;
  }
  p.workdir = abs_path(wd);
  if (!is_digits(pos.back())) { fprintf(stderr, "the trial is a number, got %s\n", pos.back().c_str()); return 2; }
  p.trial = to_long(pos.back(), 0);
  int mode_flag = 0;
  for (size_t i = 0; i < flags.size(); i++) if (flags[i] == "--mode") mode_flag = (int)to_long(flags[i + 1], 0);
  string err;
  bool text_given = !p.text.empty();
  if (!plan_trial(p, &err)) { fprintf(stderr, "omnium reduce: %s\n", err.c_str()); return 1; }
  if (mode_flag) {
    for (auto& kv : p.env) if (kv.first == "MODE") kv.second = std::to_string(mode_flag);
    if (mode_flag != 3) for (auto& kv : p.env) if (kv.first == "USE_NEW_TEXT_STRING") kv.second = "0";
    p.mode = mode_flag;
  }
  if (text_given) for (auto& kv : p.env) if (kv.first == "TEXT") kv.second = p.text;
  write_conf(p);
  string stub = write_stub(p, flags);
  printf("trial      %s%s\n", p.tdir.c_str(), p.legacy ? " (pquery-run layout)" : "");
  printf("server     %s\n", p.basedir.c_str());
  printf("input      %s (%zu lines)\n", p.inputfile.c_str(), split_lines(read_file(p.inputfile)).size());
  printf("mode       %d%s%s\n", p.mode, p.use_nts ? ", UID compare" : "", p.multi ? fmt(", multi-thread replay (%d threads ran)", p.threads).c_str() : "");
  printf("text       %s\n", p.text.c_str());
  printf("options    %s\n", p.myextra.c_str());
  if (!p.myinit.empty()) printf("init       %s\n", p.myinit.c_str());
  if (!p.core.empty()) printf("core       %s\n", p.core.c_str());
  printf("result     %s_out\n", p.inputfile.c_str());
  printf("settings   %s/reduce.conf; re-run: %s\n", p.tdir.c_str(), stub.c_str());
  if (plan_only) return 0;
  if (screen) {
    string name = "s" + std::to_string(p.trial);
    vector<string> argv = {"screen", "-dmS", name, exe_path(), "reduce", p.workdir, std::to_string(p.trial)};
    for (auto& f : flags) argv.push_back(f);
    pid_t pid = spawn_program(argv, "", "", false, {}, false);
    if (pid <= 0) { fprintf(stderr, "omnium reduce: cannot start screen\n"); return 1; }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("started screen %s (s %s attaches)\n", name.c_str(), name.c_str());
    return 0;
  }
  for (auto& kv : p.env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
  fflush(stdout);
  return role_reducer({stub, p.inputfile});
}

// --role reducer <argv0> <inputfile>: the reducer as compiled in; argv0 is the file the reducer
// calls itself (it wants a readable one). Without the embedded reducer the standalone binary runs.
int role_reducer(const Args& a) {
  vector<string> argv;
  for (auto& x : a) argv.push_back(x);
  if (argv.empty()) argv.push_back(exe_path());
  vector<char*> av;
  for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
  av.push_back(nullptr);
  if (omnium_reducer_main) return omnium_reducer_main((int)av.size() - 1, av.data());
  string bin = g_paths.qa + "/reducercpp/reducer";
  if (is_executable(bin)) { execv(bin.c_str(), av.data()); }
  fprintf(stderr, "omnium: the reducer is not compiled in and %s is not there\n", bin.c_str());
  return 127;
}
