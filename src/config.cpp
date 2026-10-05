// Created by Roel Van de Paar, MariaDB
// ~/.omnium.conf: about 15 KEY=VALUE lines, written with defaults on first run. CLI KEY=VALUE
// overrides. Plus the shared paths.
#include "common.h"

Config g_cfg;
Paths g_paths;

string config_path() { return home_dir() + "/.omnium.conf"; }

struct Key { const char* name; const char* help; std::function<string()> get; std::function<bool(const string&)> set; };
static vector<Key>& keys() {
  static vector<Key> k = {
    {"EMAIL", "address that gets one mail per new inbox item; empty = none",
     [] { return g_cfg.email; }, [](const string& v) { g_cfg.email = v; return true; }},
    {"PAT_FILE", "Jira personal access token file (~/.omnium_jira_pat, else the file ~/jira reads)",
     [] { return g_cfg.pat_file; }, [](const string& v) { g_cfg.pat_file = v; return true; }},
    {"JIRA_URL", "the Jira the filing talks to",
     [] { return g_cfg.jira_url; }, [](const string& v) { g_cfg.jira_url = v; return true; }},
    {"SMTP_PORT", "the port the mail goes to on the recipient's mail server",
     [] { return std::to_string(g_cfg.smtp_port); }, [](const string& v) { long n = to_long(v, 25); if (n < 1 || n > 65535) return false; g_cfg.smtp_port = (int)n; return true; }},
    {"RAM_CAP_PCT", "no new server above this RAM use",
     [] { return std::to_string(g_cfg.ram_cap_pct); }, [](const string& v) { g_cfg.ram_cap_pct = (int)to_long(v, 85); return true; }},
    {"SHM_CAP_PCT", "hard cap on /dev/shm use",
     [] { return std::to_string(g_cfg.shm_cap_pct); }, [](const string& v) { g_cfg.shm_cap_pct = (int)to_long(v, 90); return true; }},
    {"SHM_PAUSE_PCT", "pause the biggest trials first at this /dev/shm use",
     [] { return std::to_string(g_cfg.shm_pause_pct); }, [](const string& v) { g_cfg.shm_pause_pct = (int)to_long(v, 97); return true; }},
    {"SHM_STEPDOWN_PCT", "above this /dev/shm use a new trial runs on DATA_DIR, so no core is written to the tmpfs",
     [] { return std::to_string(g_cfg.shm_stepdown_pct); }, [](const string& v) { g_cfg.shm_stepdown_pct = (int)to_long(v, 75); return true; }},
    {"DATA_FLOOR_GB", "/data free space under which no trial is saved",
     [] { return fmt("%g", g_cfg.data_floor_gb); }, [](const string& v) { g_cfg.data_floor_gb = to_double(v, 2); return true; }},
    {"ROOT_FLOOR_GB", "/ free space under which no build starts",
     [] { return fmt("%g", g_cfg.root_floor_gb); }, [](const string& v) { g_cfg.root_floor_gb = to_double(v, 1); return true; }},
    {"RUN_DAYS", "length of a run",
     [] { return std::to_string(g_cfg.run_days); }, [](const string& v) { g_cfg.run_days = (int)to_long(v, 7); return true; }},
    {"TRIAL_SECONDS", "the trial window, wall time",
     [] { return std::to_string(g_cfg.trial_seconds); }, [](const string& v) { g_cfg.trial_seconds = (int)to_long(v, 20); return true; }},
    {"MULTI_THREAD_PCT", "share of trials that run several client threads",
     [] { return std::to_string(g_cfg.multi_thread_pct); }, [](const string& v) { g_cfg.multi_thread_pct = (int)to_long(v, 10); return true; }},
    {"BACKUP_PCT", "share of trials with a mariabackup round trip",
     [] { return std::to_string(g_cfg.backup_pct); }, [](const string& v) { g_cfg.backup_pct = (int)to_long(v, 5); return true; }},
    {"CRASH_RECOVERY_PCT", "share of trials that kill and recover the server",
     [] { return std::to_string(g_cfg.crash_recovery_pct); }, [](const string& v) { g_cfg.crash_recovery_pct = (int)to_long(v, 5); return true; }},
    {"AUTO_PIPELINE", "1: a new UID is reduced and reported inside the run; 0: omnium reduce / report by hand",
     [] { return g_cfg.auto_pipeline ? "1" : "0"; }, [](const string& v) { g_cfg.auto_pipeline = v != "0" && !iequals(v, "no") && !iequals(v, "off"); return true; }},
    {"CORE_DIR", "auto: tmpfs below SHM_STEPDOWN_PCT and DATA_DIR above it; shm: always the tmpfs, core moved out the moment the server is gone; data: always DATA_DIR (about 6x slower on insert-heavy SQL)",
     [] { return g_cfg.core_dir; }, [](const string& v) { string x = lower(trim(v)); if (x != "data" && x != "shm" && x != "auto") return false; g_cfg.core_dir = x; return true; }},
    {"CORE_MAX_GB", "a core larger than this is cut off, so one runaway trial cannot fill the disk; 0 = no limit",
     [] { return std::to_string(g_cfg.core_max_gb); }, [](const string& v) { g_cfg.core_max_gb = (int)to_long(v, 40); return true; }},
    {"SQL_SIZE_FACTOR", "SQL per trial = this x the statements recent trials executed",
     [] { return fmt("%g", g_cfg.sql_size_factor); }, [](const string& v) { g_cfg.sql_size_factor = to_double(v, 1.5); return true; }},
    {"KEEP_PER_UID", "trials kept per new UID",
     [] { return std::to_string(g_cfg.keep_per_uid); }, [](const string& v) { g_cfg.keep_per_uid = (int)to_long(v, 3); return true; }},
    {"DISCOVERY_MIN_PCT", "share of the slots discovery never drops below",
     [] { return std::to_string(g_cfg.discovery_min_pct); }, [](const string& v) { g_cfg.discovery_min_pct = (int)to_long(v, 25); return true; }},
    {"WORKERS", "slot cap; 0 = sized from RAM",
     [] { return std::to_string(g_cfg.workers); }, [](const string& v) { g_cfg.workers = (int)to_long(v, 0); return true; }},
    {"GDB_PARALLEL", "gdb runs at once",
     [] { return std::to_string(g_cfg.gdb_parallel); }, [](const string& v) { g_cfg.gdb_parallel = (int)to_long(v, 4); return true; }},
    {"CORES_PARALLEL", "cores being written at once",
     [] { return std::to_string(g_cfg.cores_parallel); }, [](const string& v) { g_cfg.cores_parallel = (int)to_long(v, 2); return true; }},
    {"FOLLOW", "1 = check every branch under test once a day and rebuild on a new head",
     [] { return g_cfg.follow ? "1" : "0"; }, [](const string& v) { g_cfg.follow = to_long(v, 1) != 0; return true; }},
    {"QA_DIR", "the mariadb-qa checkout; empty = ~/mariadb-qa when the box has one, else omnium's sparse clone beside the binary",
     [] { return g_cfg.qa_dir; }, [](const string& v) { g_cfg.qa_dir = v; return true; }},
    {"TEST_DIR", "where the builds live",
     [] { return g_cfg.test_dir; }, [](const string& v) { g_cfg.test_dir = v; return true; }},
    {"DATA_DIR", "where the workdirs live",
     [] { return g_cfg.data_dir; }, [](const string& v) { g_cfg.data_dir = v; return true; }},
    {"SHM_DIR", "the tmpfs for the trial datadirs",
     [] { return g_cfg.shm_dir; }, [](const string& v) { g_cfg.shm_dir = v; return true; }},
    {"EDITOR", "editor for kb edit and eb; empty = $EDITOR, then vi",
     [] { return g_cfg.editor; }, [](const string& v) { g_cfg.editor = v; return true; }},
    {"INFILE", "the fixed SQL file (or .tar.xz) every trial samples; empty = pquery/main-ms-ps-md.sql.tar.xz",
     [] { return g_cfg.infile; }, [](const string& v) { g_cfg.infile = v; return true; }},
    {"ALL_DISK_SQL", "1 = every .sql file on the disk is a source as well",
     [] { return string(g_cfg.all_disk_sql ? "1" : "0"); }, [](const string& v) { g_cfg.all_disk_sql = v == "1" || iequals(v, "yes") || iequals(v, "true"); return true; }},
  };
  // a setting only the Windows build has: Linux lists exactly the keys it did
  static const bool windows_keys = [] {
    if (kHostMsys2)
      k.push_back({"PERL", "the native perl MTR runs with (Strawberry Perl); empty = C:\\Strawberry, C:\\Perl64, ~\\tools\\strawberry-perl, then the PATH",
                   [] { return g_cfg.perl; }, [](const string& v) { g_cfg.perl = v; return true; }});
    return true;
  }();
  (void)windows_keys;
  return k;
}

// the settings file holds one line per setting and starts the comment at "   #", so a value with
// either in it would not come back the way it went in
static bool value_round_trips(const string& v) { return v.find('\n') == string::npos && v.find("   #") == string::npos; }
bool config_set(const string& key, const string& value) {
  string k = upper(trim(key));
  for (auto& e : keys()) if (k == e.name) return value_round_trips(trim(value)) && e.set(trim(value));
  return false;
}
bool config_get(const string& key, string& value) {
  string k = upper(trim(key));
  for (auto& e : keys()) if (k == e.name) { value = e.get(); return true; }
  return false;
}
// why config_set said no: a key omnium does not have, or a value that key does not take
string config_refusal(const string& key, const string& value) {
  string now;
  if (!config_get(key, now)) return "unknown key " + upper(trim(key));
  if (!value_round_trips(trim(value)))
    return "bad value for " + upper(trim(key)) + ": a setting is one line and cannot hold three spaces before a # (it stays " + now + ")";
  return "bad value for " + upper(trim(key)) + ": " + trim(value) + " (it stays " + now + ")";
}
string config_dump() {
  string out = "# omnium settings. KEY=VALUE, one per line. A CLI KEY=VALUE overrides a line here.\n";
  const size_t comment_col = 55;                              // every # lines up here
  for (auto& e : keys()) {
    string line = string(e.name) + "=" + e.get();
    if (line.size() + 3 <= comment_col) line.append(comment_col - line.size(), ' ');
    else line += "   ";                                        // never fewer than the three spaces the reader splits on
    out += line + "# " + e.help + "\n";
  }
  return out;
}

// a framework shell helper omnium calls, in the mariadb-qa checkout at QA_DIR. There is no second
// copy: each of those scripts reads its filter lists and its random helper from its own directory,
// and the bug lists live there too, so one checkout is one source of truth.
string script_path(const string& name) {
  return g_paths.qa + "/" + name;
}

// the dirs and links omnium needs; made when they are missing, so no run stops for a missing dir
// The one shortcut worth having in the shell without being asked for: om. It goes in ~/.bashrc the
// first time omnium runs, once, and a note says what was added and how to pick it up. A line that
// is already there is left alone, and nothing else in the file is touched.
void ensure_om_alias() {
  string rc = home_dir() + "/.bashrc";
  string have = read_file(rc);
  if (have.find("alias om=") != string::npos) return;
  string line = "alias om='" + g_paths.repo + "/omnium'";
  string add = string(have.empty() || have.back() == '\n' ? "" : "\n") + "\n# omnium\n" + line + "\n";
  if (!append_file(rc, add)) return;                             // a read-only home is not an error worth stopping for
  printf("note: added \"%s\" to %s; a new shell picks it up, or run: source %s\n", line.c_str(), rc.c_str(), rc.c_str());
}
void ensure_dirs() {
  mkdirs(g_paths.human_queue);
  mkdirs(g_paths.ai_queue);
  mkdirs(g_cfg.data_dir + "/NEWBUGS");                        // the reducer's SCAN_FOR_NEW_BUGS deposit
  mkdirs(g_cfg.shm_dir);
}
vector<string> config_missing_keys() {
  std::set<string> have;
  for (auto& l : split_lines(read_file(config_path()))) {
    string s = trim(l);
    if (s.empty() || s[0] == '#') continue;
    size_t eq = s.find('=');
    if (eq != string::npos) have.insert(upper(trim(s.substr(0, eq))));
  }
  vector<string> out;
  for (auto& e : keys()) if (!have.count(e.name)) out.push_back(e.name);
  return out;
}
// A line that sets one of the keys is replaced, a key the file lacks goes at the end, and every
// other line stays as it is. A rewrite from config_dump would save a KEY=VALUE given for one call
// only, and drop the file's own notes.
void config_write_keys(const vector<string>& names) {
  vector<std::pair<string, string>> want;                     // KEY and its line in the dump, in the dump's order
  for (auto& l : split_lines(config_dump())) {
    size_t eq = l.find('=');
    if (l.empty() || l[0] == '#' || eq == string::npos) continue;
    for (auto& n : names) if (upper(trim(n)) == l.substr(0, eq)) { want.push_back({l.substr(0, eq), l}); break; }
  }
  std::set<string> done;
  string out;
  for (auto& l : split_lines(read_file(config_path()))) {
    string s = trim(l);
    size_t eq = s.find('=');
    if (!s.empty() && s[0] != '#' && eq != string::npos) {
      string k = upper(trim(s.substr(0, eq)));
      auto it = std::find_if(want.begin(), want.end(), [&](auto& w) { return w.first == k; });
      if (it != want.end()) {
        if (done.insert(k).second) out += it->second + "\n";    // a second line for the key would win on load, so it goes
        continue;
      }
    }
    out += l + "\n";
  }
  for (auto& w : want) if (!done.count(w.first)) out += w.second + "\n";
  write_file(config_path(), out);
}
vector<std::pair<string, string>> config_keys_help() {
  vector<std::pair<string, string>> v;
  for (auto& e : keys()) v.push_back({e.name, e.help});
  return v;
}
// the omnium checkout: the directory of the binary, or two levels up when it runs from build/<mode>/. A binary that
// started from a copy of itself (private_exe_copy, MSYS2) is told where the checkout is by OMNIUM_REPO, which its
// children inherit.
string repo_dir() {
  if (const char* r = getenv("OMNIUM_REPO")) if (*r) return r;
  string exe = self_exe();
  string repo = exe.empty() ? "." : dirname_of(exe);
  if (!file_exists(repo + "/omnium.san.opt") && file_exists(repo + "/../../omnium.san.opt")) repo = abs_path(repo + "/../..");
  return repo;
}
// QA_DIR when the setting is empty: ~/mariadb-qa when the box has that checkout, else the sparse
// clone omnium keeps beside itself, made and refreshed by build.sh and omnium init through qa_checkout.sh
static string default_qa_dir() {
  string home_qa = home_dir() + "/mariadb-qa";
  return dir_exists(home_qa) ? home_qa : repo_dir() + "/mariadb-qa";
}
void config_load(bool write_defaults_when_missing) {
  if (g_cfg.qa_dir.empty()) g_cfg.qa_dir = default_qa_dir();
  if (g_cfg.pat_file.empty()) g_cfg.pat_file = default_pat_file(user_home());
  string p = config_path();
  string text = read_file(p);
  if (text.empty()) {
    if (write_defaults_when_missing) {
      write_file(p, config_dump());
      logline("wrote default settings to %s", p.c_str());
    }
  } else {
    for (auto& l : split_lines(text)) {
      string s = trim(l);
      if (s.empty() || s[0] == '#') continue;
      size_t eq = s.find('=');
      if (eq == string::npos) continue;
      string val = s.substr(eq + 1);
      size_t hash = val.find("   #");
      if (hash != string::npos) val = val.substr(0, hash);
      if (!config_set(s.substr(0, eq), val)) logwarn("%s: %s", p.c_str(), config_refusal(s.substr(0, eq), val).c_str());
    }
  }
  // the KEY=VALUE settings of the command line this process, or the omnium that started it, was given
  if (const char* set = getenv("OMNIUM_SET")) {
    for (auto& l : split_lines(set)) {
      size_t eq = l.find('=');
      if (eq != string::npos) config_set(l.substr(0, eq), l.substr(eq + 1));
    }
  }
  paths_init();
}

void paths_init() {
  g_paths.repo = repo_dir();
  g_paths.qa = g_cfg.qa_dir.empty() ? default_qa_dir() : g_cfg.qa_dir;   // QA_DIR= in the settings file keeps the default rule
  g_paths.human_queue = g_cfg.test_dir + "/omnium/HUMAN-queue";
  g_paths.ai_queue = g_cfg.test_dir + "/omnium/AI-queue";
  g_paths.builds_file = g_cfg.test_dir + "/omnium.builds";
  g_paths.seen_file = g_cfg.data_dir + "/omnium.seen";
  g_paths.known_bugs = g_paths.qa + "/known_bugs.strings";
  g_paths.known_bugs_san = g_paths.qa + "/known_bugs.strings.SAN";
  g_paths.bugs_dir = g_paths.qa + "/BUGS";
  g_paths.regex_scan = g_paths.qa + "/REGEX_ERRORS_SCAN";
  g_paths.regex_filter = g_paths.qa + "/REGEX_ERRORS_FILTER";
  g_paths.regex_lastline = g_paths.qa + "/REGEX_ERRORS_LASTLINE";
  g_paths.asan_filter = g_paths.qa + "/ASAN.filter";
  g_paths.ubsan_filter = g_paths.qa + "/UBSAN.filter";
  g_paths.tsan_filter = g_paths.qa + "/TSAN.filter";
  g_paths.san_opt = g_paths.repo + "/omnium.san.opt";
  g_paths.sql_filter = g_paths.qa + "/filter.sql";                // the framework's own list, read where it lives
  g_paths.adv_filter = g_paths.repo + "/filters/adv.filter";
  g_paths.assignees = g_paths.qa + "/skills/_shared/default_assignees.md";
  g_paths.aliases_file = home_dir() + "/.omnium_aliases";
  g_paths.history_file = home_dir() + "/.omnium_history";
}
