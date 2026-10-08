// Created by Roel Van de Paar, MariaDB
// report.cpp - omnium report: the bug report for one reduced trial in the b layout, ready for the
// inbox. The reduced testcase (prettified), the Bug Detection Matrix from a replay on the report
// builds, the assert and stack blocks of the build that shows the bug, the sanitizer block and
// Setup block when a sanitizer build shows it, the closing UID line. The report file opens with
// the Jira fields (title, project, versions, components, labels, assignee) derived from the matrix
// and the UID; they are edited by hand before the item is approved.
#include "verbs.h"
#include <regex>

namespace fs = std::filesystem;

namespace {
// the deepest _out of a reducer chain; the trace itself when nothing was reduced
string deepest_out(const string& tdir, const string& base) {
  string best;
  int depth = -1;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(tdir, ec)) {
    string n = e.path().filename().string();
    if (!starts_with(n, base + "_out") || ends_with(n, ".prev")) continue;
    string rest = n.substr(base.size());
    int d = 0;
    for (size_t p = 0; (p = rest.find("_out", p)) != string::npos; p += 4) d++;
    if (rest.size() != (size_t)d * 4) continue;               // _out_out only, no other suffix
    if (d > depth) { depth = d; best = e.path().string(); }
  }
  return best;
}
string first_keyword_op(const string& stmt) {
  vector<string> w = split_ws(upper(trim(stmt)));
  if (w.empty()) return "";
  static const std::set<string> two = {"CREATE", "ALTER", "DROP", "RENAME", "LOCK", "UNLOCK", "SHOW", "SET", "FLUSH", "OPTIMIZE", "ANALYZE", "CHECK", "REPAIR", "TRUNCATE", "INSTALL", "UNINSTALL", "LOAD", "HANDLER", "PREPARE", "EXECUTE"};
  string op = w[0];
  if (two.count(op) && w.size() > 1) {
    // walk past the words that sit between the verb and the thing it acts on, one at a time
    static const std::set<string> filler = {"OR", "REPLACE", "TEMPORARY", "UNIQUE", "FULLTEXT", "SPATIAL", "GLOBAL", "SESSION",
                                            "LOCAL", "DEFINER", "DEFINER=", "ALGORITHM", "IF", "SQL", "NOT", "EXISTS", "ONLINE"};
    size_t k = 1;
    while (k + 1 < w.size() && filler.count(w[k])) k++;
    string second = w[k];
    while (!second.empty() && (second.back() == '(' || second.back() == ';' || second.back() == ',')) second.pop_back();
    if (second == "TABLE" || second == "VIEW" || second == "INDEX" || second == "DATABASE" || second == "TRIGGER" || second == "PROCEDURE" ||
        second == "FUNCTION" || second == "EVENT" || second == "SEQUENCE" || second == "USER" || second == "ROLE" || second == "SERVER" ||
        second == "TABLESPACE" || second == "TABLES" || second == "DATA" || second == "PLUGIN" || second == "SONAME" || second == "STATEMENT" || second == "PACKAGE")
      op += " " + second;
  }
  return op;
}
// the statement that was running when the server died: the stack carries it (mysql_parse rawbuf,
// dispatch_command packet), else the error log names it, else the last statement of the testcase
string crashing_op(const string& stack, const string& errlog, const vector<string>& sql) {
  string last;
  {
    static const std::regex re("(rawbuf|packet)=0x[0-9a-fA-F]+ \"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(stack, m, re)) last = m[2];
  }
  if (last.empty()) {
    static const std::regex re("^Query \\([x0-9a-fA-F]*\\): (.*)$");
    for (auto& l : split_lines(errlog)) { std::smatch m; if (std::regex_search(l, m, re)) last = m[1]; }
  }
  if (last.empty() && !sql.empty()) {
    for (auto it = sql.rbegin(); it != sql.rend(); ++it) { string t = trim(*it); if (!t.empty() && t[0] != '#') { last = t; break; } }
  }
  string op = first_keyword_op(last);
  while (!op.empty() && (op.back() == ';' || op.back() == '(')) op.pop_back();
  return op;
}
// the framework's own safety options never belong in a testcase header
string strip_mysafe(const string& header_opts, const Basedir* b) {
  static const string common = "--no-defaults --loose-innodb-buffer-pool-in-core-dump=0 --max_allowed_packet=33554432 "
      "--maximum-bulk_insert_buffer_size=1M --maximum-join_buffer_size=1M --maximum-max_heap_table_size=1M --maximum-max_join_size=1M "
      "--maximum-myisam_max_sort_file_size=1M --maximum-myisam_mmap_size=1M --maximum-myisam_sort_buffer_size=1M "
      "--maximum-optimizer_trace_max_mem_size=1M --maximum-preload_buffer_size=1M --maximum-query_alloc_block_size=1M "
      "--maximum-query_prealloc_size=1M --maximum-range_alloc_block_size=1M --maximum-read_buffer_size=1M --maximum-read_rnd_buffer_size=1M "
      "--maximum-sort_buffer_size=1M --maximum-tmp_table_size=1M --maximum-transaction_alloc_block_size=1M --maximum-transaction_prealloc_size=1M "
      "--log-output=none --skip-stack-trace --loose-innodb-buffer-pool-size-max=2G";
  std::set<string> safe;
  for (auto& o : split_ws(common)) safe.insert(o);
  if (b) for (auto& o : split_ws(mysafe_options(*b))) safe.insert(o);
  vector<string> keep;
  for (auto& o : split_ws(header_opts)) if (!safe.count(o)) keep.push_back(o);
  return join(keep, " ");
}
struct UidParts { string assert_text, signal, frame, san_tool, san_class; bool san = false; };
UidParts uid_parts(const string& uid) {
  UidParts p;
  vector<string> f = split(uid, '|');
  if (f.empty()) return p;
  if (f[0] == "ASAN" || f[0] == "UBSAN" || f[0] == "MSAN" || f[0] == "TSAN" || f[0] == "LSAN") {
    p.san = true; p.san_tool = f[0];
    if (f.size() > 1) p.san_class = f[1];
    if (f.size() > 2) p.frame = f[2];
    return p;
  }
  size_t i = 0;
  if (starts_with(f[0], "SIG")) { p.signal = f[0]; i = 1; }
  else if (f.size() > 1 && starts_with(f[1], "SIG")) { p.assert_text = f[0]; p.signal = f[1]; i = 2; }
  else { i = 1; }
  if (i < f.size()) p.frame = f[i];
  return p;
}
string make_title(const string& uid, const string& op) {
  UidParts p = uid_parts(uid);
  string t;
  if (p.san) {
    if (p.san_tool == "UBSAN") t = "UBSAN: " + p.san_class;
    else t = p.san_tool + " " + p.san_class;
  } else if (!p.assert_text.empty()) {
    t = "Assertion `" + p.assert_text + "' failed";
  } else if (!p.signal.empty()) {
    t = p.signal;
  } else {
    t = uid.substr(0, uid.find('|'));
  }
  if (!p.frame.empty()) t += " in " + p.frame;
  if (!op.empty()) t += " on " + op;
  return t;
}
}  // namespace
// components from the frames and the SQL; the first one names the owning subsystem
vector<string> guess_components(const string& uid, const vector<string>& sql) {
  string frames = uid;
  string all = lower(join(sql, "\n"));
  vector<string> out;
  auto has = [&](std::initializer_list<const char*> keys) { for (auto k : keys) if (frames.find(k) != string::npos) return true; return false; };
  // a key that has to open a frame: mi_ inside semi_join or Repl_semi_sync is not MyISAM
  vector<string> fields = split(uid, '|');
  auto opens = [&](std::initializer_list<const char*> keys) { for (auto& f : fields) for (auto k : keys) if (starts_with(trim(f), k)) return true; return false; };
  auto add = [&](const string& c) { if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); };
  if (has({"row0", "btr0", "buf0", "fil0", "fsp0", "trx0", "lock0", "dict0", "ibuf", "innobase", "page0", "rem0", "srv0", "log0", "mtr0", "fts0", "innodb", "ha_innobase", "purge_", "ib_"})) add("Storage Engine - InnoDB");
  if (has({"ha_maria", "maria_", "_ma_"})) add("Storage Engine - Aria");
  if (has({"ha_myisam"}) || opens({"mi_", "_mi_"})) add("Storage Engine - MyISAM");
  if (has({"rocksdb", "ha_rocksdb", "myrocks"})) add("Storage Engine - RocksDB");
  if (has({"spider"})) add("Storage Engine - Spider");
  if (has({"ha_partition", "partition_info", "get_partition", "part_"})) add("Partitioning");
  if (has({"MYSQL_BIN_LOG", "Log_event", "binlog", "rpl_", "Relay_log", "Master_info", "handle_slave", "Gtid", "Rows_log_event", "slave_"})) add("Replication");
  if (has({"Item_func_json", "json_"})) add("JSON");
  if (has({"Gis_", "Item_func_spatial", "spatial", "Geometry", "gcalc"})) add("GIS");
  if (has({"Item_window", "Window_", "window_func"})) add("Optimizer - Window functions");
  if (has({"sp_head", "sp_instr", "sp_rcontext", "sp_lex", "Sp_handler", "sp_pcontext"})) add("Stored routines");
  if (has({"Table_triggers", "Item_trigger", "trigger"})) add("Triggers");
  if (has({"Event_scheduler", "Event_queue", "Event_db", "events_"})) add("Events");
  if (has({"MYSQLparse", "yyparse", "sql_yacc", "Lex_input_stream", "LEX::"})) add("Parser");
  if (has({"wsrep", "Wsrep"})) add("Galera");
  if (has({"acl_", "check_access", "Grant", "check_grant", "Security_context", "ACL_"})) add("Authentication and Privilege System");
  if (has({"my_charset", "Charset::", "strconvert", "mbminlen", "my_ci_", "my_uca", "String::copy", "copy_and_convert"})) add("Character Sets");
  if (has({"vers_", "Vers_", "system_versioning", "TR_table"})) add("Temporal Types");
  if (has({"Item_func_sequence", "sequence", "SEQUENCE::", "ha_sequence"})) add("Sequences");
  if (has({"Type_handler", "Field_", "my_decimal", "decimal_", "Item_num_op", "Item_func_additive", "Item_func_div", "Item_func_mod", "Datetime", "Temporal", "Item_temporal"})) add("Data types");
  if (has({"JOIN::", "opt_", "best_access_path", "make_join", "Item_subselect", "Item_in_subselect", "st_select_lex", "Item_sum", "SQL_SELECT", "QUICK_", "range", "Item_cond", "Item_func_", "setup_", "Item::", "Item_"})) add("Optimizer");
  if (has({"mysql_alter_table", "mysql_create_table", "Alter_", "mysql_rm_table", "mysql_prepare_create_table", "create_table_impl"})) add("Data Definition - Alter Table");
  if (has({"mysql_insert", "mysql_update", "mysql_delete", "write_record", "mysql_load", "select_insert", "multi_update", "multi_delete"})) add("Data Manipulation - Update");
  if (has({"Prepared_statement", "mysql_stmt_", "Item_param"})) add("Prepared Statements");
  if (out.empty()) add("Server");
  if (out.size() > 3) out.resize(3);
  return out;
}
namespace {
// the default assignee table of the jira-ticket skill: subsystem -> display name, plus the usernames line
string default_assignee(const string& component) {
  string md = read_file(g_paths.assignees);
  if (md.empty()) return "";
  static const vector<std::pair<const char*, const char*>> subsystem = {
    {"Storage Engine - InnoDB", "InnoDB storage engine"}, {"Optimizer", "Optimizer (generic)"}, {"Optimizer - Window functions", "Optimizer (generic)"},
    {"Replication", "Replication"}, {"Parser", "Parser"}, {"Partitioning", "Partitioning"}, {"JSON", "JSON functions"}, {"GIS", "GIS"},
    {"Data types", "Data types"}, {"Stored routines", "Stored functions/procedures"}, {"Triggers", "Triggers"}, {"Events", "Events"},
    {"Galera", "Galera"}, {"Character Sets", "Character sets"}, {"Storage Engine - Spider", "Spider storage engine"},
    {"Storage Engine - RocksDB", "RocksDB storage engine"}, {"Storage Engine - Aria", "Aria storage engine"},
    {"Storage Engine - MyISAM", "Storage engines (generic"}, {"Temporal Types", "System-versioned tables"},
    {"Prepared Statements", "Prepared statements"}, {"Sequences", "Sequences"}, {"Authentication and Privilege System", "Authentication & Privilege"},
    {"Data Definition - Alter Table", "DDL (generic)"}, {"Data Manipulation - Update", "DML (generic)"}};
  string want;
  for (auto& s : subsystem) if (component == s.first) want = s.second;
  if (want.empty()) return "";
  string display;
  for (auto& l : split_lines(md)) {
    if (!starts_with(l, "| ")) continue;
    vector<string> cells = split(l, '|');
    if (cells.size() < 3) continue;
    string sub = trim(cells[1]), who = trim(cells[2]);
    if (starts_with(sub, want)) { display = who; break; }
  }
  if (display.empty() || display == "(none)") return "";
  size_t slash = display.find(" / ");
  if (slash != string::npos) display = display.substr(0, slash);
  // "Name Surname=`user`" on the usernames line; the surname decides
  vector<string> dw = split_ws(display);
  if (dw.empty()) return "";
  string surname = dw.back();
  for (auto& l : split_lines(md)) {
    if (!starts_with(l, "Jira usernames:")) continue;
    for (auto& entry : split(l.substr(15), ',')) {
      size_t eq = entry.find("=`");
      if (eq == string::npos) continue;
      string names = trim(entry.substr(0, eq)), user = entry.substr(eq + 2);
      user = user.substr(0, user.find('`'));
      if (names.find(surname) != string::npos) return user;
    }
  }
  return "";
}
string setup_block(const Basedir& san_build, const string& errlog) {
  string o = "Setup:\n\n{noformat}\n";
  bool clang = san_build.cmake_cmd.find("clang") != string::npos || read_file(san_build.path + "/BUILD_CMD_CMAKE").find("clang") != string::npos;
  if (clang) {
    o += "Compiled with a recent version of Clang and LLVM. Ubuntu instructions for Clang/LLVM 18:\n";
    o += "  # Note: It is strongly recommended to uninstall all old Clang & LLVM packages (ref  dpkg --list | grep -iE 'clang|llvm'  and use  apt purge  and  dpkg --purge  to remove the packages), before installing Clang/LLVM 18\n";
    o += "     sudo apt install clang llvm-18 llvm-18-linker-tools llvm-18-runtime llvm-18-tools llvm-18-dev libstdc++-14-dev llvm-dev lld-18\n";
    string cmake = san_build.cmake_cmd.empty() ? read_file(san_build.path + "/BUILD_CMD_CMAKE") : san_build.cmake_cmd;
    std::smatch m;
    string olevel = std::regex_search(cmake, m, std::regex("-O[0-9g] ")) ? m[0].str() : (san_build.dbg ? "-O1 " : "-O2 ");
    o += "Compiled with: \"-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_C{,XX}_FLAGS='" + olevel + "-march=native -mtune=native'\" and:\n";
  } else {
    o += "Compiled with a recent version of GCC and:\n";
  }
  switch (san_build.flavour) {
    case Flavour::TSAN:
      o += "    -DWITH_TSAN=ON -DWSREP_LIB_WITH_TSAN=ON -DMUTEXTYPE=sys\nSet before execution:\n";
      o += "    export TSAN_OPTIONS=suppressions=TSAN.filter:suppress_equal_stacks=1:history_size=7:second_deadlock_stack=1:verbosity=1:exitcode=0   # TSAN.filter suppresses known server start/stop data races so real races stand out\n";
      break;
    case Flavour::MSAN:
      o += "    -DWITH_MSAN=ON  # Note: WITH_MSAN=ON is auto-ignored when not using clang (MDEV-20377)\nSet before execution:\n    export MSAN_OPTIONS=abort_on_error=1:poison_in_dtor=0\n";
      break;
    case Flavour::VAL:
      o += "    -DWITH_VALGRIND=ON\nRun the server under Valgrind (from within the base directory):\n";
      o += "    valgrind --suppressions=./mariadb-test/valgrind.supp --num-callers=40 --show-reachable=yes ./bin/mariadbd <options>   # The suppressions file is ./mysql-test/valgrind.supp in older versions\n";
      break;
    default: {
      o += "    -DWITH_ASAN=ON -DWITH_ASAN_SCOPE=ON -DWITH_UBSAN=ON -DWSREP_LIB_WITH_ASAN=ON\n";
      bool ub = icontains(errlog, "runtime error:");
      bool asan = errlog.find("=ERROR:") != string::npos || errlog.find("LeakSanitizer:") != string::npos || errlog.find("AddressSanitizer:") != string::npos;
      if (ub) {
        o += "Set before execution:\n    export UBSAN_OPTIONS=print_stacktrace=1:report_error_type=1   # And you may also want to supress UBSAN startup issues using 'suppressions=UBSAN.filter' in UBSAN_OPTIONS. For an example of UBSAN.filter, which includes some suppressions for MariaDB startup issues, see https://github.com/mariadb-corporation/mariadb-qa/blob/master/UBSAN.filter\n";
      }
      if (asan) {
        if (!ub) o += "Set before execution:\n";
        o += "    export ASAN_OPTIONS=quarantine_size_mb=512:atexit=0:detect_invalid_pointer_pairs=3:dump_instruction_bytes=1:abort_on_error=1:allocator_may_return_null=1\n";
      }
    }
  }
  return o + "{noformat}\n";
}
}  // namespace

// the b stack blocks with the frame cap of the plan
string report_cap_frames(const string& stack, int cap) {
  vector<string> out;
  int dropped = 0;
  for (auto& l : split_lines(stack)) {
    if (l.size() > 1 && l[0] == '#' && isdigit((unsigned char)l[1])) {
      long n = to_long(l.substr(1, l.find(' ') - 1), 0);
      if (n >= cap) { dropped++; continue; }
    }
    out.push_back(l);
  }
  string s = join(out, "\n") + "\n";
  if (dropped) {
    size_t last = s.rfind("{noformat}");
    if (last != string::npos) s.insert(last, fmt("[%d more frames not shown]\n", dropped));
  }
  return s;
}
string report_san_label(const string& errlog) {
  static const std::regex re("SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer|MemorySanitizer|ThreadSanitizer|LeakSanitizer): ([a-zA-Z0-9_-]+)");
  std::smatch m;
  if (std::regex_search(errlog, m, re)) return m[2];
  return "";
}

// the header of a report file: KEY: value lines up to the ----- line
vector<std::pair<string, string>> report_header(const string& text, string* body) {
  vector<std::pair<string, string>> kv;
  bool in_body = false;
  string b;
  for (auto& l : split_lines(text)) {
    if (in_body) { b += l + "\n"; continue; }
    if (starts_with(l, "-----")) { in_body = true; continue; }
    size_t c = l.find(':');
    if (c == string::npos) continue;
    kv.push_back({trim(l.substr(0, c)), trim(l.substr(c + 1))});
  }
  if (body) *body = b;
  return kv;
}
string report_field(const vector<std::pair<string, string>>& kv, const string& key) {
  for (auto& p : kv) if (iequals(p.first, key)) return p.second;
  return "";
}
static vector<string> csv(const string& s) {
  vector<string> out;
  for (auto& x : split(s, ',')) { string t = trim(x); if (!t.empty()) out.push_back(t); }
  return out;
}
vector<string> fix_versions(const vector<string>& affects) {
  vector<string> fix = affects;
  if (fix.size() > 1) fix.pop_back();
  return fix;
}

bool report_to_fields(const string& text, JiraFields& f, string* err) {
  string body;
  auto kv = report_header(text, &body);
  f.summary = report_field(kv, "Title");
  f.project = report_field(kv, "Project");
  if (f.project.empty()) f.project = "MDEV";
  f.issuetype = "Bug";
  f.priority = report_field(kv, "Priority");
  f.affects = csv(report_field(kv, "Affects"));
  f.es_versions = csv(report_field(kv, "Affects ES"));
  f.fix = csv(report_field(kv, "Fix Version"));
  f.components = csv(report_field(kv, "Components"));
  f.labels = csv(report_field(kv, "Labels"));
  f.assignee = report_field(kv, "Assignee");
  f.description = body;
  while (!f.description.empty() && f.description.back() == '\n') f.description.pop_back();
  if (f.summary.empty()) { if (err) *err = "the report has no Title: line"; return false; }
  if (f.description.empty()) { if (err) *err = "the report has no body under the ----- line"; return false; }
  return true;
}

// whether the build that crashed in the trial (own_path; any build when it is empty) shows a bug in the matrix
static bool own_build_shows_bug(const MatrixResult& m, const string& own_path) {
  for (auto& r : m.rows)
    if (matrix_row_shows_bug(r) && (own_path.empty() || r.b.path == own_path)) return true;
  return false;
}
bool report_prefer_reduced(MatrixResult& m, const string& own_path, const std::function<bool(MatrixResult&)>& run_reduced) {
  if (own_build_shows_bug(m, own_path)) return false;
  MatrixResult reduced;
  if (!run_reduced(reduced) || !own_build_shows_bug(reduced, own_path)) return false;
  m = reduced;
  return true;
}

// the testcase of a saved trial (omnium or pquery-run layout): the deepest reduced file, else the raw trace;
// the header keeps only the options the bug needs
bool trial_testcase(const string& workdir, long trial, TrialTestcase& tc, string* err) {
  tc = TrialTestcase();
  string tdir = workdir + "/" + std::to_string(trial);
  if (!dir_exists(tdir)) { if (err) *err = "no trial dir " + tdir; return false; }
  string uid = trim(read_file(tdir + "/MYBUG"));
  tc.uid = uid.substr(0, uid.find('\n'));
  string basedir = trim(read_file(tdir + "/BASEDIR"));
  if (basedir.empty())
    for (auto& l : split_lines(read_file(workdir + "/pquery-run.log"))) {
      size_t k = l.find("Basedir:");
      if (k != string::npos) { string v = trim(l.substr(k + 8)); basedir = trim(v.substr(0, v.find('|'))); break; }
    }
  if (basedir.empty()) basedir = trim(read_file(workdir + "/BASEDIR.template"));
  tc.basedir = basedir;
  Basedir tb;
  bool have_tb = !basedir.empty() && basedir_probe(basedir, tb);
  for (const string& base : {string("default.node.tld_thread-0.sql"), std::to_string(trial) + ".sql", "quick_" + std::to_string(trial) + ".sql"}) {
    string d = deepest_out(tdir, base);
    if (!d.empty()) { tc.file = d; break; }
  }
  if (tc.file.empty()) {
    tc.reduced = false;
    for (const string& base : {string("default.node.tld_thread-0.sql"), std::to_string(trial) + ".sql"})
      if (file_exists(tdir + "/" + base)) { tc.file = tdir + "/" + base; break; }
  }
  if (tc.file.empty()) { if (err) *err = "no testcase in " + tdir; return false; }
  vector<string> lines = split_lines(read_file(tc.file));
  static const string tag = "mysqld options required for replay:";
  if (!lines.empty() && !lines[0].empty() && lines[0][0] == '#' && lower(lines[0]).find(tag) != string::npos) {
    tc.options = strip_mysafe(trim(lines[0].substr(lower(lines[0]).find(tag) + tag.size())), have_tb ? &tb : nullptr);
    lines.erase(lines.begin());
  }
  tc.sql = join(lines, "\n") + "\n";
  return true;
}

// omnium report [<workdir>] <trial> [build ...] [--slots N] [--no-matrix] [--no-mtr] [--sql FILE] [--out DIR]
int cmd_report(const Args& a) {
  vector<string> pos;
  int slots = 0;
  bool no_matrix = false, no_mtr = false;
  string sql_override, outdir;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--slots") slots = (int)to_long(val(s), 0);
    else if (s == "--no-matrix") no_matrix = true;
    else if (s == "--no-mtr") no_mtr = true;
    else if (s == "--sql") sql_override = val(s);
    else if (s == "--out") outdir = val(s);
    else if (starts_with(s, "--")) { fprintf(stderr, "omnium report: unknown flag %s\n", s.c_str()); return 2; }
    else pos.push_back(s);
  }
  // report <trial> [build ...] in a workdir, or report <workdir> <trial> [build ...]
  string wd = ".";
  long trial = 0;
  vector<string> builds;
  if (pos.empty()) { fprintf(stderr, "usage: omnium report [<workdir>] <trial> [build ...] [--slots N] [--no-matrix] [--no-mtr] [--sql FILE] [--out DIR]\n"); return 2; }
  if (is_digits(pos[0]) && (pos.size() == 1 || !is_digits(pos[1]))) { trial = to_long(pos[0], 0); builds.assign(pos.begin() + 1, pos.end()); }
  else if (pos.size() >= 2 && is_digits(pos[1])) { wd = pos[0]; trial = to_long(pos[1], 0); builds.assign(pos.begin() + 2, pos.end()); }
  else { fprintf(stderr, "omnium report: the trial is a number; got %s\n", join(pos, " ").c_str()); return 2; }
  if (wd.find('/') == string::npos && wd != ".") {
    if (dir_exists(g_cfg.data_dir + "/" + wd)) wd = g_cfg.data_dir + "/" + wd;
    else if (is_digits(wd) && dir_exists(g_cfg.data_dir + "/O" + wd)) wd = g_cfg.data_dir + "/O" + wd;
  }
  string workdir = abs_path(wd);
  string tdir = workdir + "/" + std::to_string(trial);
  string err;
  // the trial: UID, server, testcase
  TrialTestcase tc;
  if (!trial_testcase(workdir, trial, tc, &err)) { fprintf(stderr, "omnium report: %s\n", err.c_str()); return 1; }
  string uid = tc.uid;
  if (uid.empty()) { fprintf(stderr, "omnium report: %s/MYBUG is empty; omnium t in the trial dir first\n", tdir.c_str()); return 1; }
  Basedir tb;
  bool have_tb = !tc.basedir.empty() && basedir_probe(tc.basedir, tb);
  bool reduced = tc.reduced;
  string raw = tc.options.empty() ? tc.sql : "# mysqld options required for replay: " + tc.options + "\n" + tc.sql;
  if (!sql_override.empty()) { raw = read_file(sql_override); reduced = true; }
  string pretty;
  {
    sweep_stale_tmp("/tmp/omnium_tcp_");                          // what a killed report left behind
    string tmp = fmt("/tmp/omnium_tcp_%d.sql", (int)getpid());
    write_file(tmp, raw);
    CmdResult r = run_capture({script_path("testcase_prettify.sh"), tmp}, 120);
    fs::remove(tmp);
    if (r.rc == 0 && !trim(r.out).empty() && r.out.find("Assert:") != 0) pretty = r.out;
    else pretty = raw;
  }
  vector<string> sql;
  for (auto& l : split_lines(pretty)) if (!trim(l).empty()) sql.push_back(l);
  string wdname = basename_of(workdir);
  string bug_sql = workdir + fmt("/bug%ld.sql", trial);
  write_file(bug_sql, join(sql, "\n") + "\n");
  // the matrix
  MatrixResult m;
  string prettify_note;                                         // set when the report carries the reduced testcase over the prettified one
  if (!no_matrix) {
    vector<Basedir> set;
    if (!matrix_builds(builds, set, &err)) { fprintf(stderr, "omnium report: %s\n", err.c_str()); return 1; }
    if (have_tb) { bool in = false; for (auto& b : set) if (b.path == tb.path) in = true; if (!in) set.push_back(tb); }
    if (slots <= 0) slots = std::max(1, std::min((int)set.size(), (int)(ram_available_bytes() / (3ull << 30))));
    fprintf(stderr, "matrix: %zu builds, %d at a time\n", set.size(), slots);
    if (!matrix_run(bug_sql, set, "", slots, m, &err)) { fprintf(stderr, "omnium report: matrix: %s\n", err.c_str()); return 1; }
    // testcase_prettify.sh can change what the SQL means (see report_prefer_reduced), and the reducer saw the reduced testcase crash: when the
    // trial's own build shows no bug on the prettified one, the reduced one is replayed, and carried when it does
    if (kTakeFixes && pretty != raw) {
      vector<string> reduced_lines;
      for (auto& l : split_lines(raw)) if (!trim(l).empty()) reduced_lines.push_back(l);
      string reduced_sql = workdir + fmt("/bug%ld.reduced.sql", trial);
      bool took = report_prefer_reduced(m, have_tb ? tb.path : "", [&](MatrixResult& out) {
        fprintf(stderr, "matrix: the prettified testcase shows no bug on %s; replaying the reduced one\n", have_tb ? tb.name.c_str() : "any build");
        write_file(reduced_sql, join(reduced_lines, "\n") + "\n");
        string e2;
        return matrix_run(reduced_sql, set, "", slots, out, &e2);
      });
      unlink(reduced_sql.c_str());
      if (took) {
        string kept = workdir + fmt("/bug%ld.prettified.sql", trial);
        write_file(kept, join(sql, "\n") + "\n");
        sql = reduced_lines;
        write_file(bug_sql, join(sql, "\n") + "\n");
        prettify_note = "testcase_prettify.sh changed the testcase so that it showed no bug on " + (have_tb ? tb.name : string("any build")) +
                        "; the report carries the reduced testcase as the reducer left it (the prettified one is " + kept + ")";
        fprintf(stderr, "matrix: the reduced testcase shows the bug, and goes into the report\n");
      }
    }
  }
  auto shows_bug = [](const MatrixRow& r) { return matrix_row_shows_bug(r); };
  // the build whose blocks go in: the trial's own when it shows the bug, else a dbg build, else any
  const MatrixRow* lead = nullptr;
  for (auto& r : m.rows) if (have_tb && r.b.path == tb.path && shows_bug(r) && r.b.flavour == Flavour::Plain) lead = &r;
  if (!lead) for (auto& r : m.rows) if (shows_bug(r) && r.b.flavour == Flavour::Plain && r.b.dbg) { lead = &r; break; }
  if (!lead) for (auto& r : m.rows) if (shows_bug(r) && r.b.flavour == Flavour::Plain) { lead = &r; break; }
  vector<const MatrixRow*> san_rows;
  for (auto& r : m.rows) if (shows_bug(r) && r.b.flavour != Flavour::Plain) san_rows.push_back(&r);
  // the MTR form: made and replayed on the affected build, verified there (fail) and on a clean build (pass)
  MtrTest mt;
  MtrVerdict mv_fail, mv_pass;
  bool have_mtr = false, mtr_gate = false;
  string mtr_line, bug_test = workdir + fmt("/bug%ld.test", trial);
  if (!no_mtr) {
    const Basedir* affected = lead ? &lead->b : (have_tb ? &tb : nullptr);
    if (!affected && !san_rows.empty()) affected = &san_rows[0]->b;
    string mtr_err;
    string sql_only;
    for (auto& l : sql) if (!l.empty() && l[0] != '#') sql_only += l + "\n";
    string header_opts;
    if (!sql.empty() && !sql[0].empty() && sql[0][0] == '#') { static const string tg = "mysqld options required for replay:"; size_t k = lower(sql[0]).find(tg); if (k != string::npos) header_opts = trim(sql[0].substr(k + tg.size())); }
    if (mtr_make(sql_only, header_opts, affected, uid, mt, &mtr_err)) {
      have_mtr = true;
      if (!mt.opt.empty()) write_file(workdir + fmt("/bug%ld.opt", trial), mt.opt); else fs::remove(workdir + fmt("/bug%ld.opt", trial));
      if (affected) {
        fprintf(stderr, "mtr: verifying on %s\n", affected->name.c_str());
        string tag = wdname + fmt("_%ld", trial);
        if (mtr_verify(*affected, mt, tag, mv_fail, &mtr_err)) {
          mtr_gate = mv_fail.gate;
          mtr_line = mv_fail.build + ": " + mv_fail.verdict;
          // a clean build of the same vendor from the matrix: the test must pass there
          const MatrixRow* clean = nullptr;
          for (auto& r : m.rows) if (!shows_bug(r) && r.uid == "No bug found" && r.b.vendor_str() == affected->vendor_str() && r.b.flavour == Flavour::Plain) { clean = &r; break; }
          if (clean && mtr_gate) {
            fprintf(stderr, "mtr: verifying on %s (expect pass)\n", clean->b.name.c_str());
            if (mtr_verify(clean->b, mt, tag, mv_pass, &mtr_err)) {
              mtr_line += "; " + mv_pass.build + ": " + mv_pass.verdict;
              if (!mv_pass.pass) { mtr_gate = false; mt.notes.push_back("the test does not pass on a build without the bug; the gate is not clean"); }
            }
          }
        } else mtr_line = "verification did not run: " + mtr_err;
      } else mtr_line = "not verified: no build to run it on";
      write_file(bug_test, mt.test);              // with the suppression lines the verification added
    } else mtr_line = "not made: " + mtr_err;
  }
  // fields from the matrix
  vector<string> affects, affects_es;
  for (auto& r : m.rows) {
    if (!shows_bug(r)) continue;
    string v = r.b.vendor_str();
    vector<string>* dst = v == "CS" ? &affects : v == "ES" ? &affects_es : nullptr;
    if (!dst) continue;
    string ver = v == "CS" ? r.b.series : r.b.version;
    if (std::find(dst->begin(), dst->end(), ver) == dst->end()) dst->push_back(ver);
  }
  if (m.rows.empty() && have_tb) { (tb.es ? affects_es : affects).push_back(tb.es ? tb.version : tb.series); }
  vector<string> fix = fix_versions(affects);
  bool es_only = affects.empty() && !affects_es.empty();
  string project = es_only ? "MENT" : "MDEV";
  // an ES build that ran and did not show the bug: N/A, which says checked and not affected, where an
  // empty field reads as not checked. It stays empty when no ES build ran.
  if (affects_es.empty()) for (auto& r : m.rows) if (r.b.vendor_str() == "ES" && r.uid == "No bug found") { affects_es = {"N/A"}; break; }
  string op = crashing_op(lead ? lead->stack : "", lead ? lead->errlog : read_file(tdir + "/log/master.err"), sql);
  vector<string> components = guess_components(uid, sql);
  string assignee = default_assignee(components[0]);
  vector<string> labels;
  for (auto r : san_rows) { string l = report_san_label(r->errlog); if (!l.empty() && std::find(labels.begin(), labels.end(), l) == labels.end()) labels.push_back(l); }
  // dedup: the known lists, the seen ledger, Jira
  KbMatch km = kb_search(uid);
  string kb_text = trim(kb_verdict_text(uid, km));
  string kb_line = kb_text.substr(kb_text.find("-----\n") == string::npos ? 0 : kb_text.find("-----\n") + 6);
  kb_line = trim(kb_line.substr(0, kb_line.find('\n')));
  vector<JiraHit> hits;
  string jira_note;
  for (auto& u : kb_jira_urls(uid)) {
    vector<JiraHit> h;
    string e;
    if (!jira_search_url(u, h, &e)) { jira_note = "not run (" + e + ")"; break; }
    for (auto& x : h) { bool dup = false; for (auto& y : hits) if (y.key == x.key) dup = true; if (!dup) hits.push_back(x); }
  }
  bool jira_fixed = false;
  if (jira_note.empty()) {
    if (hits.empty()) jira_note = "no hits";
    else {
      vector<string> parts;
      for (auto& h : hits) { parts.push_back(h.key + " (" + h.status + (h.resolution == "-" ? "" : ", " + h.resolution) + ")"); if (iequals(h.resolution, "Fixed")) jira_fixed = true; }
      jira_note = join(parts, "; ") + " - possible duplicate, see the .possible_dup file";
    }
  }
  string seen_note;
  if (auto s = seen_lookup(uid)) seen_note = fmt("seen %ld times since %s, last in %s (%s)", s->count, stamp_of(s->first).c_str(), s->run.c_str(), s->outcome.c_str());
  // the body
  string body;
  body += "{code:sql}\n" + join(sql, "\n") + "\n{code}\n\n";
  if (have_mtr && mtr_gate) {
    body += "MTR testcase (fails while the bug is present, passes once fixed):\n{code}\n" + mt.test + "{code}\n";
    if (!mt.opt.empty()) body += "-master.opt:\n{code}\n" + mt.opt + "{code}\n";
    body += "\n";
  }
  body += "Leads to:\n\n";
  string stack;
  string banner_title = have_tb ? basedir_banner_title(tb) : "";
  if (lead) stack = lead->stack;
  else {
    string e;
    stack = stack_text(tdir, banner_title.empty() ? "stack" : banner_title, &e);
  }
  if (!stack.empty()) body += report_cap_frames(stack, 40) + "\n";
  if (!m.rows.empty()) body += matrix_format(m) + "\n";
  else body += "{noformat:title=Bug Detection Matrix}\nNot run for this report (omnium report --no-matrix). The versions above come from the trial alone.\n{noformat}\n\n";
  for (auto r : san_rows) {
    if (r->stack.empty()) continue;
    body += r->stack + "\n";
  }
  if (!san_rows.empty()) body += setup_block(san_rows[0]->b, san_rows[0]->errlog) + "\n";
  body += "UID: " + uid + "\n";
  // the notes the reader needs before the .ok
  vector<string> notes;
  if (!reduced) notes.push_back("the testcase is the raw trace, not a reduced one");
  if (!prettify_note.empty()) notes.push_back(prettify_note);
  if (!m.rows.empty() && !lead && san_rows.empty()) notes.push_back("no build in the matrix showed the bug; the stack is the trial's own");
  if (have_tb && !m.rows.empty()) { bool own = false; for (auto& r : m.rows) if (r.b.path == tb.path && shows_bug(r)) own = true; if (!own) notes.push_back("the trial's own build did not show the bug on this replay"); }
  if (no_mtr) notes.push_back("MTR testcase: skipped (--no-mtr); the report carries the SQL testcase alone");
  else if (!have_mtr) notes.push_back("MTR testcase: " + mtr_line + "; the report carries the SQL testcase alone");
  else if (!mtr_gate) notes.push_back("MTR testcase " + bug_test + " has no verified gate (" + mtr_line + "); it is not in the report");
  for (auto& n : mt.notes) notes.push_back("MTR: " + n);
  if (es_only) notes.push_back("ES-only: project MENT; check the version names Jira expects before the .ok");
  if (jira_fixed) notes.push_back("a Jira hit is Closed/Fixed while the bug reproduces here: the fix may be missing in this build (upmerge pending) or this is a regression; check before the .ok");
  for (auto& r : m.rows) if (!r.note.empty()) notes.push_back(r.b.name + ": " + r.note);
  string title = make_title(uid, op);
  string report;
  report += "Title: " + title + "\n";
  report += "Project: " + project + "\n";
  report += "Type: Bug\n";
  report += "Priority: Major\n";
  report += "Affects: " + join(affects, ", ") + "\n";
  report += "Affects ES: " + join(affects_es, ", ") + "\n";
  report += "Fix Version: " + join(fix, ", ") + "\n";
  report += "Components: " + join(components, ", ") + "\n";
  report += "Labels: " + join(labels, ", ") + "\n";
  report += "Assignee: " + assignee + "\n";
  report += "UID: " + uid + "\n";
  report += "Source: " + tdir + (have_tb ? " on " + tb.name : "") + "\n";
  report += "Testcase: " + bug_sql + "\n";
  if (have_mtr) report += "MTR: " + bug_test + (mtr_gate ? " (gate verified: " : " (gate not verified: ") + mtr_line + ")\n";
  report += "Known bugs: " + kb_line + "\n";
  report += "Jira search: " + jira_note + "\n";
  if (!seen_note.empty()) report += "Seen: " + seen_note + "\n";
  for (auto& n : notes) report += "Note: " + n + "\n";
  report += "----- description (Jira markup; everything below this line is the ticket body) -----\n";
  report += body;
  // where it goes: the inbox, and a copy in the workdir
  string inbox = outdir.empty() ? g_paths.human_queue : outdir;
  mkdirs(inbox);
  string item = wdname + fmt("_bug%ld", trial);
  string report_path = inbox + "/" + item + ".report";
  if (file_exists(report_path + ".filed") || file_exists(inbox + "/" + item + ".filed")) {
    printf("%s is already filed (%s); the report was not rewritten\n", item.c_str(), trim(read_file(inbox + "/" + item + ".filed")).c_str());
    return 0;
  }
  write_file(report_path, report);
  write_file(workdir + fmt("/bug%ld.report", trial), report);
  JiraFields f;
  if (report_to_fields(report, f, &err)) write_file(inbox + "/" + item + ".preview", jira_create_payload(f) + "\n");
  string dup_path = inbox + "/" + item + ".possible_dup";
  fs::remove(dup_path);
  if (!hits.empty()) {
    string d;
    for (auto& h : hits) d += h.key + "\t" + h.status + "\t" + h.resolution + "\t" + h.summary + "\n";
    write_file(dup_path, d);
  }
  if (!kb_text.empty()) write_file(inbox + "/" + item + ".kb", kb_text + "\n");
  printf("report     %s\n", report_path.c_str());
  printf("title      %s\n", title.c_str());
  printf("testcase   %s (%zu lines%s)\n", bug_sql.c_str(), sql.size(), reduced ? "" : ", not reduced");
  if (!m.rows.empty()) {
    int n = 0; for (auto& r : m.rows) if (shows_bug(r)) n++;
    printf("matrix     %d of %zu builds show the bug\n", n, m.rows.size());
  }
  if (have_mtr) printf("mtr        %s (%s)\n", bug_test.c_str(), mtr_gate ? "gate verified" : "no verified gate");
  printf("known bugs %s\n", kb_line.c_str());
  printf("jira       %s\n", jira_note.c_str());
  for (auto& n : notes) printf("note       %s\n", n.c_str());
  // what still needs a person or a Claude session: the AI queue holds one dir per item
  vector<string> needs;
  if (!reduced) needs.push_back("reduce the testcase: it is the raw trace");
  if (!no_mtr && (!have_mtr || !mtr_gate)) needs.push_back("write or repair the MTR test so it fails on the bug and passes once fixed (" + (have_mtr ? mtr_line : string("not made")) + ")");
  if (jira_fixed) needs.push_back("check the Jira hits: one is Closed/Fixed while the bug reproduces here");
  if (kb_verdict(km) == KbVerdict::Partial) needs.push_back("check the partial known-bug match before filing");
  if (m.rows.empty()) needs.push_back("no matrix was run: the affected versions come from the trial alone");
  if (!needs.empty()) {
    string aid = g_paths.ai_queue + "/" + item;
    mkdirs(aid);
    string info;
    info += "title      " + title + "\n";
    info += "uid        " + uid + "\n";
    info += "report     " + report_path + "\n";
    info += "testcase   " + bug_sql + "\n";
    if (have_mtr) info += "mtr        " + bug_test + "\n";
    info += "trial      " + tdir + "\n";
    info += "build      " + (have_tb ? tb.name : string("unknown")) + "\n";
    info += "\nwhat it needs:\n";
    for (auto& n : needs) info += "  - " + n + "\n";
    info += "\nwhen it is right: touch " + inbox + "/" + item + ".ok, then omnium inbox --process\n";
    write_file(aid + "/info.txt", info);
    printf("ai queue   %s/info.txt (%zu item%s)\n", aid.c_str(), needs.size(), needs.size() == 1 ? "" : "s");
  }
  // one mail per new item, when EMAIL is set
  if (!g_cfg.email.empty()) {
    string body;
    body += title + "\n\n";
    body += "report    " + report_path + "\n";
    body += "testcase  " + bug_sql + "\n";
    if (have_mtr) body += "mtr       " + bug_test + (mtr_gate ? " (gate verified)" : " (no verified gate)") + "\n";
    body += "uid       " + uid + "\n";
    body += "known     " + kb_line + "\n";
    body += "jira      " + jira_note + "\n";
    for (auto& n : notes) body += "note      " + n + "\n";
    body += "\napprove: touch " + inbox + "/" + item + ".ok\nfile:    omnium inbox --process\n";
    string e;
    if (!mail_send(g_cfg.email, "omnium: " + title, body, &e)) printf("mail       not sent: %s\n", e.c_str());
    else printf("mail       sent to %s\n", g_cfg.email.c_str());
  }
  printf("approve    touch %s/%s.ok\n", inbox.c_str(), item.c_str());
  return 0;
}
