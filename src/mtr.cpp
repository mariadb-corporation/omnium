// Created by Roel Van de Paar, MariaDB
// mtr.cpp - the MTR form of a reduced testcase, and its verification. The .test is a reverse gate:
// it fails while the bug is there and passes once fixed. The conversion reads the SQL and the
// server options the bug needs; a replay on the affected build gives the error of each statement
// (the --error lines), tells whether the crash comes at a statement or at shutdown, and whether
// the storage engine is load-bearing. Verification runs the test from the build's own mariadb-test
// tree with a vardir on tmpfs, as main/omnium_<tag>.test, removed afterwards.
#include "verbs.h"
#include "connect.h"
#include <regex>
#include <signal.h>

namespace fs = std::filesystem;

// the replay's client: a connect timeout, and a cap on one statement's answer, so a statement that
// hangs ends the replay instead of holding omnium mtr and the report job with it
unsigned replay_client_options(MYSQL* m, bool san) {
  unsigned int connect_s = 30, answer_s = san ? 600 : 300;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &connect_s);
  mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &answer_s);
  mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &answer_s);
  return answer_s;
}

namespace {
// ---- the error names: <basedir>/include/mysql/mysqld_error.h ----------------------------------
std::map<int, string> er_names(const Basedir& b) {
  std::map<int, string> out;
  for (const char* rel : {"/include/mysql/mysqld_error.h", "/include/mysql/server/mysqld_error.h", "/include/mysqld_error.h"}) {
    string text = read_file(b.path + rel);
    if (text.empty()) continue;
    static const std::regex def(R"(^#define\s+(ER_[A-Z0-9_]+)\s+(\d+))");
    for (auto& l : split_lines(text)) {
      std::smatch m;
      if (std::regex_search(l, m, def)) out[(int)to_long(m[2].str())] = m[1].str();
    }
    if (!out.empty()) break;
  }
  return out;
}
string er_name(const std::map<int, string>& names, int code) {
  auto it = names.find(code);
  return it != names.end() ? it->second : std::to_string(code);
}

// ---- SQL text helpers ----------------------------------------------------------------------------
// the statement with quoted strings blanked, so keyword and identifier scans do not read literals
string blank_quotes(const string& s) {
  string out = s;
  char q = 0;
  for (size_t i = 0; i < out.size(); i++) {
    char c = out[i];
    if (q) {
      if (c == '\\' && i + 1 < out.size()) { out[i] = out[i + 1] = ' '; i++; continue; }
      if (c == q) { q = 0; continue; }
      out[i] = ' ';
    } else if (c == '\'' || c == '"') q = c;
  }
  return out;
}
size_t count_semicolons(const string& s) {
  size_t n = 0;
  for (char c : blank_quotes(s)) if (c == ';') n++;
  return n;
}
string strip_trailing_semicolon(string s) {
  s = trim(s);
  while (!s.empty() && s.back() == ';') { s.pop_back(); s = trim(s); }
  return s;
}
// backticks around a plain identifier are noise in a testcase (and the TUI drops them)
string drop_plain_backticks(const string& s) {
  static const std::regex rx(R"(`([A-Za-z_][A-Za-z0-9_]*)`)");
  return std::regex_replace(s, rx, "$1");
}
// a stored program body carries its own semicolons: CREATE PROCEDURE ... BEGIN ...; END
bool needs_delimiter(const string& stmt) {
  static const std::regex rx(R"(^\s*(CREATE\s+(OR\s+REPLACE\s+)?(DEFINER\s*=\s*\S+\s+)?(PROCEDURE|FUNCTION|TRIGGER|EVENT|PACKAGE|AGGREGATE\s+FUNCTION)|BEGIN\s+NOT\s+ATOMIC))", std::regex::icase);
  return std::regex_search(stmt, rx) && count_semicolons(stmt) > 1;
}
// the object a statement creates: kind and name, for the cleanup at the end
struct Created { string kind, name; };
bool created_object(const string& stmt, Created& c) {
  static const std::regex rx(R"(^\s*CREATE\s+(OR\s+REPLACE\s+)?(TEMPORARY\s+)?(DEFINER\s*=\s*\S+\s+)?(ALGORITHM\s*=\s*\w+\s+)?(SQL\s+SECURITY\s+\w+\s+)?(TABLE|SEQUENCE|VIEW|DATABASE|SCHEMA|USER|ROLE|PROCEDURE|FUNCTION|TRIGGER|EVENT|SERVER|TABLESPACE)\s+(IF\s+NOT\s+EXISTS\s+)?([`"']?[A-Za-z0-9_$%]+[`"']?(@[`"']?[A-Za-z0-9_.%-]+[`"']?)?(\.[`"]?[A-Za-z0-9_$]+[`"]?)?))", std::regex::icase);
  std::smatch m;
  if (!std::regex_search(stmt, m, rx)) return false;
  if (m[2].length()) return false;                          // a temporary table goes with the session
  string kind = upper(m[6].str());
  if (kind == "SCHEMA") kind = "DATABASE";
  c.kind = kind;
  string name = m[8].str();
  if (kind != "USER" && kind != "ROLE") {
    name.erase(std::remove(name.begin(), name.end(), '`'), name.end());
    name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
  }
  c.name = name;
  return true;
}
// a SET GLOBAL in the SQL: the variable, for the restore at the end
string set_global_var(const string& stmt) {
  static const std::regex rx(R"(^\s*SET\s+(GLOBAL|@@GLOBAL\.|@@global\.)\s*([A-Za-z_][A-Za-z0-9_]*)\s*=)", std::regex::icase);
  std::smatch m;
  if (std::regex_search(stmt, m, rx)) return lower(m[2].str());
  return "";
}
bool has_word(const string& low, const char* w) {
  std::regex rx(string("\\b") + w + "\\b");
  return std::regex_search(low, rx);
}
bool create_table_without_engine(const string& stmt) {
  string low = lower(blank_quotes(stmt));
  if (!std::regex_search(low, std::regex(R"(^\s*create\s+(or\s+replace\s+)?(temporary\s+)?table\b)"))) return false;
  if (low.find(" like ") != string::npos) return false;
  return low.find("engine") == string::npos;
}
string add_engine(const string& stmt, const string& engine) {
  // ENGINE= goes before a PARTITION BY clause, else at the end
  string low = lower(blank_quotes(stmt));
  size_t part = low.find(" partition by");
  string body = strip_trailing_semicolon(stmt);
  if (part != string::npos && part < body.size()) return trim(body.substr(0, part)) + " ENGINE=" + engine + " " + trim(body.substr(part)) + ";";
  return body + " ENGINE=" + engine + ";";
}

// ---- the option lines a testcase needs ------------------------------------------------------------
struct Opt { string raw, name, value; bool has_value = false; };
Opt parse_opt(const string& raw) {
  Opt o;
  o.raw = raw;
  string s = raw;
  while (starts_with(s, "-")) s = s.substr(1);
  if (starts_with(lower(s), "loose-") || starts_with(lower(s), "loose_")) s = s.substr(6);
  size_t eq = s.find('=');
  if (eq != string::npos) { o.has_value = true; o.value = s.substr(eq + 1); s = s.substr(0, eq); }
  for (auto& c : s) if (c == '-') c = '_';
  o.name = lower(s);
  return o;
}

// ---- the replay: one connection, statement by statement -------------------------------------------
struct Replay {
  bool ran = false;                      // the server started and the client connected
  vector<int> errnos;                    // per statement, -1 = not reached
  int crash_at = -1;                     // the statement that lost the connection
  bool shutdown_crash = false;           // every statement ran, the core came at shutdown
  string uid;                            // the UID chain on the replay's error log and core
  string note;
};
bool sysvar_dynamic(MYSQL* m, const string& name, bool& dynamic) {
  string q = "SELECT READ_ONLY FROM information_schema.SYSTEM_VARIABLES WHERE VARIABLE_NAME='" + upper(name) + "'";
  if (mysql_real_query(m, q.c_str(), q.size())) return false;
  MYSQL_RES* res = mysql_store_result(m);
  if (!res) return false;
  MYSQL_ROW row = mysql_fetch_row(res);
  bool found = row && row[0];
  if (found) dynamic = string(row[0]) == "NO";
  mysql_free_result(res);
  return found;
}
void replay(const Basedir& b, const vector<string>& stmts, const vector<string>& server_opts, const string& prelude,
            const string& root, const string& templates, Replay& r, std::map<string, bool>* dynamic_of = nullptr) {
  r = Replay();
  r.errnos.assign(stmts.size(), -1);
  Instance inst;
  inst.bd = &b;
  inst.set_paths(root);
  mkdirs(root);
  write_file(root + "/BASEDIR", b.path + "\n");
  for (auto& o : server_opts) inst.extra.push_back(o);
  string tpl = template_for(b, myinit_from(join(server_opts, " ")), templates);
  if (tpl.empty()) { r.note = "no datadir template"; return; }
  if (!inst.start_fresh(tpl, b.is_san() ? 240 : 60)) { r.note = "server did not start: " + inst.start_note; inst.kill_hard(); return; }
  MYSQL* m = mysql_init(nullptr);
  unsigned answer_s = replay_client_options(m, b.is_san());
  if (!endpoint_connect(m, inst.endpoint(), "root", "test", CLIENT_MULTI_STATEMENTS)) {
    r.note = string("connect failed: ") + mysql_error(m);
    mysql_close(m);
    inst.kill_hard();
    return;
  }
  r.ran = true;
  if (dynamic_of) for (auto& kv : *dynamic_of) { bool d = false; if (sysvar_dynamic(m, kv.first, d)) kv.second = d; }
  if (!prelude.empty()) { mysql_real_query(m, prelude.c_str(), prelude.size()); while (mysql_next_result(m) == 0) {} }
  for (size_t i = 0; i < stmts.size(); i++) {
    string s = strip_trailing_semicolon(stmts[i]);
    double q0 = now_ms();
    int rc = mysql_real_query(m, s.c_str(), s.size());
    unsigned int e = rc ? mysql_errno(m) : 0;
    if (!rc) { MYSQL_RES* res = mysql_store_result(m); if (res) mysql_free_result(res); while (mysql_next_result(m) == 0) { res = mysql_store_result(m); if (res) mysql_free_result(res); } }
    r.errnos[i] = (int)e;
    // a lost connection after the whole answer cap is the cap, not a crash, when the server is still up;
    // a crash loses it at once, and the server can look alive for a while as it writes its core
    if (e == 2013 && now_ms() - q0 >= answer_s * 1000.0 - 1000 && inst.alive())
      r.note = fmt("statement %zu gave no answer within the read timeout; the server is still up", i + 1);
    if (e == 2013 || e == 2006 || e == 2002) { r.crash_at = (int)i; break; }
  }
  mysql_close(m);
  sleep(b.is_san() ? 3 : 1);
  if (r.crash_at < 0 && inst.alive()) {
    if (!inst.shutdown(25)) for (int i = 0; i < 5 && !inst.has_core() && !inst.has_dump(); i++) sleep(1);
  }
  inst.kill_hard();
  UidResult ur; UidOptions uo; uo.wait_core = false;
  uid_for_dir(root, ur, uo);
  r.uid = trim(ur.uid);
  if (starts_with(r.uid, "Assert:")) r.uid.clear();
  if (r.crash_at < 0 && (inst.has_core() || inst.has_dump() || (!r.uid.empty() && ur.san))) r.shutdown_crash = inst.has_core() || inst.has_dump();
}
// a statement's own error, the kind a --error line names: the server's codes, not the client's 2000-2999
bool server_error(int e) { return e > 0 && (e < 2000 || e >= 3000); }
bool reproduces(const Replay& r, const string& uid_want) {
  if (!r.ran) return false;
  if (!uid_want.empty() && !r.uid.empty()) return r.uid == uid_want;
  return r.crash_at >= 0 || r.shutdown_crash;
}
}  // namespace

// the conversion; b = the affected build the replays run on (nullptr: text rules only)
bool mtr_make(const string& sql_text, const string& options, const Basedir* b, const string& uid, MtrTest& t, string* err) {
  t = MtrTest();
  vector<string> stmts;
  for (auto& l : split_lines(sql_text)) {
    string s = trim(l);
    if (s.empty()) continue;
    if (s[0] == '#') continue;                              // the options header, or a comment
    stmts.push_back(drop_plain_backticks(s));
  }
  if (stmts.empty()) { if (err) *err = "no SQL statements"; return false; }
  // the options: sql_mode inline, log-bin and binlog-format as includes, the rest by kind
  vector<string> includes, prelude_sets, opt_lines, restores;
  std::map<string, bool> dynamic_of;
  vector<Opt> pending;
  for (auto& raw : split_ws(options)) {
    Opt o = parse_opt(raw);
    if (o.name == "sql_mode") { prelude_sets.push_back("SET sql_mode='" + o.value + "';"); continue; }
    if (o.name == "log_bin") { includes.push_back("--source include/have_log_bin.inc"); continue; }
    if (o.name == "binlog_format" && o.has_value) { includes.push_back("--source include/have_binlog_format_" + lower(o.value) + ".inc"); continue; }
    if (o.name == "innodb" || starts_with(o.name, "plugin_load") || starts_with(o.name, "skip_") || !o.has_value) { opt_lines.push_back(o.raw); continue; }
    pending.push_back(o);
    dynamic_of[o.name] = false;
  }
  // guards from the SQL text
  string all = lower(blank_quotes(join(stmts, "\n")));
  bool any_engine_less = false;
  for (auto& s : stmts) if (create_table_without_engine(s)) any_engine_less = true;
  if (all.find("engine=innodb") != string::npos || all.find("engine = innodb") != string::npos) includes.push_back("--source include/have_innodb.inc");
  // a table's PARTITION BY, not a window's: that one opens its OVER ( or WINDOW w AS (
  bool partitioned = false;
  for (size_t k = all.find("partition by"); k != string::npos && !partitioned; k = all.find("partition by", k + 1)) {
    size_t j = k;
    while (j > 0 && isspace((unsigned char)all[j - 1])) j--;
    partitioned = j == 0 || all[j - 1] != '(';
  }
  if (partitioned) includes.push_back("--source include/have_partition.inc");
  if (all.find("rocksdb") != string::npos) includes.push_back("--source include/have_rocksdb.inc");
  if (all.find("engine=spider") != string::npos || all.find("engine = spider") != string::npos) includes.push_back("--source include/have_spider.inc");
  if (has_word(all, "sequence") || all.find("nextval") != string::npos || all.find("next value") != string::npos) includes.push_back("--source include/have_sequence.inc");
  if (all.find("debug_dbug") != string::npos) { includes.push_back("--source include/have_debug.inc"); t.debug_only = true; t.notes.push_back("debug build only (SET debug_dbug)"); }
  if (all.find("debug_sync") != string::npos) includes.push_back("--source include/have_debug_sync.inc");
  if (all.find("engine=connect") != string::npos || all.find("engine=federated") != string::npos) t.notes.push_back("CONNECT or FederatedX: no standard have_*.inc; the plugin must be loaded in the test's own way");
  // the replays on the affected build: statement errors, crash point, engine dependence, dynamic options
  Replay r1, r2, re;
  bool have_replay = false;
  string root, templates;
  if (b) {
    root = g_cfg.shm_dir + fmt("/Omtr%d", (int)getpid());
    remove_tree(root);
    templates = root + "/templates";
    mkdirs(templates);
    Child hold;
    spawn_role(hold, "hold", {root}, "/dev/null");
    vector<string> server_opts;
    for (auto& o : split_ws(options)) { Opt p = parse_opt(o); if (p.name != "sql_mode") server_opts.push_back(o); }
    string prelude = join(prelude_sets, " ");
    replay(*b, stmts, server_opts, prelude, root + "/r1", templates, r1, &dynamic_of);
    have_replay = r1.ran;
    if (have_replay) {
      replay(*b, stmts, server_opts, prelude, root + "/r2", templates, r2);
      if (any_engine_less && reproduces(r1, uid)) {
        // the other engine: does the bug need the default (InnoDB) engine?
        replay(*b, stmts, server_opts, trim(prelude + " SET default_storage_engine=MyISAM;"), root + "/re", templates, re);
        if (!reproduces(re, uid)) {
          for (auto& s : stmts) if (create_table_without_engine(s)) s = add_engine(s, "InnoDB");
          includes.push_back("--source include/have_innodb.inc");
          t.notes.push_back("the bug needs InnoDB: ENGINE=InnoDB added (the MyISAM replay did not show it)");
        }
      } else if (any_engine_less && !reproduces(r1, uid)) {
        t.notes.push_back("engine dependence not checked: the replay did not show the bug");
      }
    } else {
      t.notes.push_back("replay on " + b->name + " did not run: " + r1.note);
    }
    if (have_replay && !r1.note.empty()) t.notes.push_back("replay on " + b->name + ": " + r1.note);
    kill_group(hold.pid, SIGKILL);
    child_reap(hold, 2000);
    remove_tree(root);
  } else {
    t.notes.push_back("no replay: --error lines, the crash point and the engine dependence were not checked");
  }
  // dynamic options become SET GLOBAL, the rest the .opt file
  for (auto& o : pending) {
    if (have_replay && dynamic_of[o.name]) {
      prelude_sets.push_back("SET GLOBAL " + o.name + "=" + o.value + ";");
      restores.push_back("SET GLOBAL " + o.name + "=DEFAULT;");
    } else {
      opt_lines.push_back(o.raw);
    }
  }
  // dedupe the includes, keep the order
  { vector<string> u; for (auto& i : includes) if (std::find(u.begin(), u.end(), i) == u.end()) u.push_back(i); includes = u; }
  // the cleanup: what the SQL creates, in reverse; every SET GLOBAL restored
  vector<Created> created;
  for (auto& s : stmts) {
    Created c;
    if (created_object(s, c)) { bool dup = false; for (auto& x : created) if (x.kind == c.kind && iequals(x.name, c.name)) dup = true; if (!dup) created.push_back(c); }
    string v = set_global_var(s);
    if (!v.empty()) { string line = "SET GLOBAL " + v + "=DEFAULT;"; if (std::find(restores.begin(), restores.end(), line) == restores.end()) restores.push_back(line); }
  }
  // the text
  std::map<int, string> names;
  if (b) names = er_names(*b);
  string out;
  for (auto& i : includes) out += i + "\n";
  if (!includes.empty()) out += "\n";
  for (auto& s : prelude_sets) out += s + "\n";
  int crash_at = have_replay ? r1.crash_at : -1;
  for (size_t i = 0; i < stmts.size(); i++) {
    const string& s = stmts[i];
    if (have_replay && (int)i != crash_at) {
      int e1 = r1.errnos[i], e2 = r2.ran && i < r2.errnos.size() ? r2.errnos[i] : e1;
      if (server_error(e1)) {
        if (e2 == e1 || e2 < 0) out += "--error " + er_name(names, e1) + "\n";
        else out += "--error 0," + er_name(names, e1) + (server_error(e2) ? "," + er_name(names, e2) : "") + "\n";
      } else if (e1 == 0 && server_error(e2)) {
        out += "--error 0," + er_name(names, e2) + "\n";
      }
    }
    if (needs_delimiter(s)) out += "DELIMITER |;\n" + strip_trailing_semicolon(s) + "|\nDELIMITER ;|\n";
    else out += s + "\n";
  }
  if (have_replay && r1.shutdown_crash) {
    out += "\n# the server dies on shutdown: restart, then the assert in the log fails the test\n";
    out += "--source include/restart_mysqld.inc\n";
    out += "--let SEARCH_FILE= $MYSQLTEST_VARDIR/log/mysqld.1.err\n";
    out += "--let SEARCH_PATTERN= Assertion|signal [0-9]+|AddressSanitizer|MemorySanitizer|runtime error\n";
    out += "--let SEARCH_ABORT= FOUND\n";
    out += "--source include/search_pattern_in_file.inc\n";
    t.shutdown_crash = true;
  }
  if (!created.empty() || !restores.empty()) {
    out += "\n";
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
      string kind = it->kind == "SERVER" ? "SERVER" : it->kind;
      out += "DROP " + kind + " IF EXISTS " + it->name + ";\n";
    }
    for (auto& r : restores) out += r + "\n";
  }
  t.test = out;
  t.opt = opt_lines.empty() ? "" : join(opt_lines, " ") + "\n";
  t.replayed = have_replay;
  t.crash_at_statement = crash_at >= 0;
  if (have_replay) {
    if (crash_at >= 0) t.gate_kind = "the server dies at statement " + std::to_string(crash_at + 1);
    else if (r1.shutdown_crash) t.gate_kind = "the server dies on shutdown";
    else if (!r1.uid.empty()) { t.gate_kind = "no crash on the replay; the UID chain found " + r1.uid + " (the error-log check fails the test)"; }
    else { t.gate_kind = "no crash on the replay and nothing in the error log: no gate"; t.notes.push_back("the replay did not show the bug; the test has no gate"); }
    if (!uid.empty() && !r1.uid.empty() && r1.uid != uid) t.notes.push_back("the replay UID differs from the trial's: " + r1.uid);
  }
  return true;
}

// a flagged log line that carries the bug, not noise the testcase itself makes
static bool log_line_is_bug(const string& l) {
  static const std::regex bug(R"(Assertion|got signal [0-9]+|Fatal signal [0-9]+|Attempting backtrace|\[ERROR\] Aborting)");
  return text_has_san_marker(l) || std::regex_search(l, bug);
}

// a flagged log line as an mtr.add_suppression line: the timestamp, the thread id and the level
// are dropped, and what is left becomes a REGEXP inside a double-quoted SQL string
static string suppression_for(const string& l) {
  static const std::regex head(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2} +[0-9]+:[0-9]+:[0-9]+(\.[0-9]+)? +[0-9]+ +\[[A-Za-z]+\] +)");
  std::smatch m;
  if (!std::regex_search(l, m, head)) return "";
  string msg = trim(l.substr(m[0].length())), pat;
  for (char c : msg) {
    string p(1, c);
    if (c == '"') p = "\\\"";
    else if (strchr("\\.^$|()[]{}*+?", c)) p = "\\\\" + p;
    if (pat.size() + p.size() > 240) break;               // mtr.test_suppressions.pattern is VARCHAR(255)
    pat += p;
  }
  return pat.empty() ? "" : "call mtr.add_suppression(\"" + pat + "\");";
}

// the runner's own output, read into a verdict
void mtr_parse_verdict(const string& out, int rc, const string& test_name, bool crash_at_statement, MtrVerdict& v, vector<string>* suppress) {
  v.output = out;
  v.pass = v.fail = v.server_died = v.query_error = v.log_error = v.gate = false;
  v.verdict.clear();
  v.reason.clear();
  if (suppress) suppress->clear();
  bool pass = false, fail = false;
  for (auto& l : split_lines(out)) {
    if (l.find("main." + test_name) != string::npos) {
      if (l.find("[ pass ]") != string::npos) pass = true;
      if (l.find("[ fail ]") != string::npos) fail = true;
    }
  }
  // the log lines the runner's own check flagged: it prints them under the header, one per line.
  // Only the per-test check fails the test, so only its lines are worth a suppression.
  int block = 0;                            // 1 = the per-test log check, 2 = the scan after shutdown
  bool log_check = false;                   // the per-test log check is what failed the run
  for (auto& raw : split_lines(out)) {
    string s = trim(raw);
    if (s.find("Found warnings/errors in server log") != string::npos) { block = 1; log_check = true; continue; }
    if (starts_with(s, "***Warnings generated in error logs")) { block = 2; continue; }
    if (!block) continue;
    if (starts_with(s, "^ Found warnings in") || starts_with(s, "---") || starts_with(s, "- saving") || s == "ok") { block = 0; continue; }
    if (s.empty() || s == "line" || starts_with(s, "Test ended at")) continue;
    if (log_line_is_bug(s)) { v.log_error = true; continue; }
    if (block != 1 || !suppress) continue;
    string sp = suppression_for(s);
    if (!sp.empty() && std::find(suppress->begin(), suppress->end(), sp) == suppress->end()) suppress->push_back(sp);
  }
  // the reason, in the runner's own words
  vector<string> why;
  for (auto& l : split_lines(out)) {
    string s = trim(l);
    if (starts_with(s, "mysqltest:") || s.find("failed during test run") != string::npos || s.find("Lost connection") != string::npos ||
        starts_with(s, "Result content mismatch") || starts_with(s, "Result length mismatch") || s.find("Found warnings/errors in server log") != string::npos ||
        s.find("Assertion") != string::npos || s.find("Sanitizer") != string::npos || s.find("runtime error") != string::npos)
      if (!starts_with(s, "#") && why.size() < 3 && std::find(why.begin(), why.end(), s) == why.end()) why.push_back(s);
  }
  for (auto& s : why) {
    if (s.find("Lost connection") != string::npos || s.find("failed during test run") != string::npos || s.find("Assertion") != string::npos ||
        s.find("Sanitizer") != string::npos || s.find("runtime error") != string::npos || s.find("(2013)") != string::npos || s.find("(2006)") != string::npos) v.server_died = true;
    // a statement's own error is not the bug: the --error line is missing, or the SQL differs under MTR
    if (starts_with(s, "mysqltest:") && s.find("failed: ER_") != string::npos && s.find("(2013)") == string::npos && s.find("(2006)") == string::npos) v.query_error = true;
  }
  v.fail = fail || v.log_error;
  v.pass = pass && !v.fail;
  // the gate is real when the run fails the way the bug shows: the server died, or a flagged log
  // line carries the bug. A statement's own error, or a log line the testcase itself makes, is not it.
  v.gate = v.fail && !v.query_error && (v.server_died || v.log_error || (!crash_at_statement && !log_check));
  if (!pass && !v.fail) v.verdict = fmt("the runner gave no verdict (rc %d): %s", rc, tail_lines(out, 3).c_str());
  else v.verdict = string(v.pass ? "pass" : v.gate ? "fail" : "fail, not the bug's way") + (why.empty() ? "" : ": " + join(why, " | "));
  v.reason = join(why, "\n");
}

static bool perl_is_native(const string& perl) {
  if (!is_executable(perl)) return false;
  CmdResult r = run_capture({perl, "-e", "print $^O"}, 20);
  return r.rc == 0 && trim(r.out) == "MSWin32";
}
string native_perl_find(string* why) {
  string tried;
  auto ok = [&](const string& p) {
    if (perl_is_native(p)) return true;
    if (is_executable(p)) tried += (tried.empty() ? "" : ", ") + p + " (not native)";
    return false;
  };
  if (!g_cfg.perl.empty()) {
    if (ok(g_cfg.perl)) return g_cfg.perl;
    if (why) *why = "PERL=" + g_cfg.perl + " is " + (is_executable(g_cfg.perl) ? "not a native Windows perl (it says $^O is not MSWin32)" : "not there") + ": MTR needs a native Windows perl, such as Strawberry Perl";
    return "";
  }
  vector<string> cands = {"/c/Strawberry/perl/bin/perl.exe", "/c/Perl64/bin/perl.exe", "/c/Perl/bin/perl.exe"};
  string wh = windows_home();
  if (!wh.empty()) cands.push_back(wh + "/tools/strawberry-perl/perl/bin/perl.exe");
  if (const char* path = getenv("PATH")) for (auto& d : split(path, ':')) if (!d.empty()) cands.push_back(d + "/perl.exe");
  for (auto& c : cands) if (ok(c)) return c;
  if (why)
    *why = "MTR on Windows needs a native Windows perl, and there is none" + (tried.empty() ? string() : " (" + tried + ")") +
           ": MSYS2's perl reports $^O=msys, which MTR takes for Cygwin and then asks for --cygwin-subshell-fix=do, a wrapper over /bin/sh "
           "that omnium runs on, so never use that flag. Install Strawberry Perl (the portable zip needs no admin rights) into C:\\Strawberry or "
           "%USERPROFILE%\\tools\\strawberry-perl, or set PERL=<its perl.exe> (docs/windows.md)";
  return "";
}

// runs the test from the build's mariadb-test tree; true = the run happened (verdict tells the outcome)
bool mtr_verify(const Basedir& b, MtrTest& t, const string& tag, MtrVerdict& v, string* err) {
  v = MtrVerdict();
  v.build = b.name;
  string mt = basedir_test_dir(b);
  if (mt.empty()) { if (err) *err = b.windows ? "no mariadb-test in " + b.path + ": " + mtr_suite_fix() : "no mariadb-test or mysql-test dir in " + b.path; return false; }
  string runner = file_exists(mt + "/mariadb-test-run.pl") ? "./mariadb-test-run.pl" : "./mysql-test-run.pl";
  if (!file_exists(mt + "/" + runner.substr(2))) { if (err) *err = "no test runner in " + mt; return false; }
  string perl = "perl";
  vector<string> perl_env;
  if (b.windows) {                                              // a native perl, first on the PATH the runner and its helpers see
    perl = native_perl_find(err);
    if (perl.empty()) return false;
    perl_env.push_back("PATH=" + dirname_of(perl) + ":" + (getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin"));
  }
  // a run killed between the install below and the removal at the end leaves its files in the
  // build's test dir; the longest verify is two runs of 1800 s, so an older file is such a leftover
  {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(mt + "/main", ec)) {
      string n = e.path().filename().string();
      if (starts_with(n, "omnium_") && e.is_regular_file(ec) && now_s() - file_mtime(e.path().string()) > 7200) fs::remove(e.path(), ec);
    }
  }
  string name = "omnium_" + tag;
  string test_path = mt + "/main/" + name + ".test";
  string opt_path = mt + "/main/" + name + "-master.opt";
  string vardir = g_cfg.shm_dir + fmt("/Omtr%d_%s", (int)getpid(), b.short_name().c_str());
  remove_tree(vardir);
  mkdirs(vardir);
  Child hold;
  spawn_role(hold, "hold", {vardir}, "/dev/null");
  write_file(test_path, t.test);
  if (!t.opt.empty()) write_file(opt_path, t.opt); else fs::remove(opt_path);
  vector<string> argv = {perl, runner, "--vardir=" + vardir + "/var", "--tmpdir=" + vardir + "/tmp", "--parallel=1", "--retry=0",
                         "--force", "--testcase-timeout=20", "main." + name};
  if (t.debug_only && !b.dbg) { v.verdict = "skipped: the test needs a debug build"; fs::remove(test_path); fs::remove(opt_path); kill_group(hold.pid, SIGKILL); child_reap(hold, 2000); remove_tree(vardir); return true; }
  vector<string> env = san_env_for(b);
  for (auto& e : perl_env) env.push_back(e);
  env.push_back("MTR_MAX_SAVE_CORE=0");
  env.push_back("MTR_MAX_SAVE_DATADIR=0");
  CmdResult r = run_capture(argv, 1800, mt, env);
  vector<string> suppress;
  mtr_parse_verdict(r.out, r.rc, name, t.crash_at_statement, v, &suppress);
  // the log lines the testcase itself makes are not the bug: suppress them the way an MTR test
  // does, run once more, and keep the lines in the test so it passes for a developer once fixed
  if (v.fail && !v.log_error && !suppress.empty()) {
    t.test = join(suppress, "\n") + "\n\n" + t.test;
    write_file(test_path, t.test);
    r = run_capture(argv, 1800, mt, env);
    mtr_parse_verdict(r.out, r.rc, name, t.crash_at_statement, v, nullptr);
    v.verdict += fmt(" (after suppressing %zu log line%s the testcase itself makes)", suppress.size(), suppress.size() == 1 ? "" : "s");
  }
  fs::remove(test_path);
  fs::remove(opt_path);
  fs::remove(mt + "/main/" + name + ".result");
  fs::remove(mt + "/main/" + name + ".reject");
  kill_group(hold.pid, SIGKILL);
  child_reap(hold, 2000);
  remove_tree(vardir);
  return true;
}

// omnium mtr [<workdir>] <trial> [--basedir DIR] [--no-verify] [--out FILE]
// omnium mtr <sql-file> --basedir DIR [--options "..."] [--clean DIR] [--no-verify] [--out FILE]
int cmd_mtr(const Args& a) {
  vector<string> pos;
  string basedir, clean, options, outfile;
  bool verify = true;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--basedir") basedir = val(s);
    else if (s == "--clean") clean = val(s);
    else if (s == "--options") options = val(s);
    else if (s == "--out") outfile = val(s);
    else if (s == "--no-verify") verify = false;
    else if (starts_with(s, "--")) { fprintf(stderr, "omnium mtr: unknown flag %s\n", s.c_str()); return 2; }
    else pos.push_back(s);
  }
  const char* usage = "usage: omnium mtr [<workdir>] <trial> [--basedir DIR] [--clean DIR] [--no-verify] [--out FILE]\n"
                      "       omnium mtr <sql-file> --basedir DIR [--options \"...\"] [--clean DIR] [--no-verify] [--out FILE]\n";
  if (pos.empty()) { fputs(usage, stderr); return 2; }
  string sql_text, uid, tag;
  if (pos.size() == 1 && !is_digits(pos[0]) && file_exists(pos[0])) {
    sql_text = read_file(pos[0]);
    tag = basename_of(pos[0]);
    tag = tag.substr(0, tag.find('.'));
    if (options.empty()) {
      string first = trim(sql_text.substr(0, sql_text.find('\n')));
      static const string tg = "mysqld options required for replay:";
      size_t k = lower(first).find(tg);
      if (!first.empty() && first[0] == '#' && k != string::npos) options = trim(first.substr(k + tg.size()));
    }
  } else {
    string wd = ".";
    long trial = 0;
    if (is_digits(pos[0]) && pos.size() == 1) trial = to_long(pos[0], 0);
    else if (pos.size() == 2 && is_digits(pos[1])) { wd = pos[0]; trial = to_long(pos[1], 0); }
    else { fputs(usage, stderr); return 2; }
    if (wd.find('/') == string::npos && wd != ".") {
      if (dir_exists(g_cfg.data_dir + "/" + wd)) wd = g_cfg.data_dir + "/" + wd;
      else if (is_digits(wd) && dir_exists(g_cfg.data_dir + "/O" + wd)) wd = g_cfg.data_dir + "/O" + wd;
    }
    string workdir = abs_path(wd), tdir = workdir + "/" + std::to_string(trial);
    if (!dir_exists(tdir)) { fprintf(stderr, "omnium mtr: no trial dir %s\n", tdir.c_str()); return 1; }
    string err;
    TrialTestcase tc;
    if (!trial_testcase(workdir, trial, tc, &err)) { fprintf(stderr, "omnium mtr: %s\n", err.c_str()); return 1; }
    sql_text = tc.sql;
    if (options.empty()) options = tc.options;
    uid = tc.uid;
    if (basedir.empty()) basedir = tc.basedir;
    tag = fmt("%s_%ld", basename_of(workdir).c_str(), trial);
    if (outfile.empty()) outfile = workdir + fmt("/bug%ld.test", trial);
  }
  Basedir b;
  bool have_b = !basedir.empty();
  string err;
  if (have_b && !basedir_from_arg(basedir, b, &err)) { fprintf(stderr, "omnium mtr: %s\n", err.c_str()); return 1; }
  MtrTest t;
  if (!mtr_make(sql_text, options, have_b ? &b : nullptr, uid, t, &err)) { fprintf(stderr, "omnium mtr: %s\n", err.c_str()); return 1; }
  // the verification runs first: it can add suppression lines to the test, and those go out with it
  int rc = 0;
  vector<string> verdicts;
  if (verify && have_b) {
    MtrVerdict v;
    fprintf(stderr, "mtr: verifying on %s\n", b.name.c_str());
    if (!mtr_verify(b, t, tag, v, &err)) { verdicts.push_back("# verify: " + err); rc = 1; }
    else {
      verdicts.push_back("# verify on " + b.name + ": " + v.verdict);
      if (!v.gate) rc = 1;
    }
    if (!clean.empty()) {
      Basedir cb;
      MtrVerdict cv;
      if (basedir_from_arg(clean, cb, &err) && mtr_verify(cb, t, tag, cv, &err)) verdicts.push_back("# verify on " + cb.name + " (expect pass): " + cv.verdict);
      else verdicts.push_back("# verify on the clean build: " + err);
    }
  }
  if (!outfile.empty()) {
    write_file(outfile, t.test);
    string optfile = outfile.substr(0, outfile.rfind('.')) + ".opt";
    if (!t.opt.empty()) write_file(optfile, t.opt); else fs::remove(optfile);
  }
  fputs(t.test.c_str(), stdout);
  if (!t.opt.empty()) printf("# -master.opt: %s", t.opt.c_str());
  if (!t.gate_kind.empty()) printf("# gate: %s\n", t.gate_kind.c_str());
  for (auto& n : t.notes) printf("# note: %s\n", n.c_str());
  if (!outfile.empty()) printf("# written: %s\n", outfile.c_str());
  for (auto& l : verdicts) printf("%s\n", l.c_str());
  return rc;
}
