// Created by Roel Van de Paar, MariaDB
// registry.cpp - /test/omnium.builds, the known-bugs lists, the BUGS/ files and the regex lists
#include "common.h"
#include "verbs.h"
#include <sys/wait.h>
#include <regex>

// ---------------------------------------------------------------------------------------------
// /test/omnium.builds: the gendirs list of today in one plain file
// ---------------------------------------------------------------------------------------------
static const char* REG_HEADER =
  "# omnium basedir registry: every current release build under /test, one line each. Never a feature build.\n"
  "# Columns: name vendor version flavour type date test report origin commit\n"
  "# test=yes: omnium with no version named runs trials on it. report=yes: a bug is checked on it for the report matrix.\n"
  "# Edit by hand, or: omnium builds set <name> test=yes report=no. A newer build of the same version and flavour replaces the older line.\n";

string BuildEntry::path() const {
  return name.find('/') == string::npos ? g_cfg.test_dir + "/" + name : name;
}
string BuildEntry::key() const { return vendor + " " + series + " " + flavour + " " + type; }

static string series_of(const string& version) {
  size_t d1 = version.find('.');
  if (d1 == string::npos) return version;
  size_t d2 = version.find('.', d1 + 1);
  return d2 == string::npos ? version : version.substr(0, d2);
}
static string date_key(const string& d) {  // ddmmyy to yymmdd, so that string order is time order
  return d.size() == 6 ? d.substr(4, 2) + d.substr(2, 2) + d.substr(0, 2) : "";
}
bool entry_newer(const BuildEntry& a, const BuildEntry& b) {
  string da = date_key(a.date), db = date_key(b.date);
  if (da != db) return da > db;
  return version_cmp(a.version, b.version) > 0;
}
bool entry_from_basedir(const Basedir& b, BuildEntry& e) {
  if (b.vendor == Vendor::Unknown || b.version.empty() || b.date.size() != 6) return false;
  e = BuildEntry();
  e.name = b.name;
  e.vendor = b.vendor_str();
  e.version = b.version;
  e.series = b.series.empty() ? series_of(b.version) : b.series;
  e.flavour = b.flavour_str().empty() ? "plain" : b.flavour_str();
  e.type = b.dbg ? "dbg" : "opt";
  e.date = b.date;
  e.commit = b.git_rev.empty() ? "-" : b.git_rev;
  return true;
}
// the rule for a build that is new to the list: CS and ES test and report; MySQL and Percona report only
static void entry_defaults(BuildEntry& e) {
  bool ms = e.vendor == "MS" || e.vendor == "PS";
  e.test = !ms;
  e.report = true;
}

bool registry_load(Registry& r, const string& path_in) {
  string path = path_in.empty() ? g_paths.builds_file : path_in;
  r.entries.clear();
  if (!file_exists(path)) return false;
  string txt = read_file(path);
  for (auto& raw : split_lines(txt)) {
    string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;
    auto f = split_ws(line);
    if (f.size() < 8) { r.notices.push_back("omnium.builds: cannot read this line: " + line); continue; }
    BuildEntry e;
    e.name = f[0]; e.vendor = f[1]; e.version = f[2]; e.flavour = f[3]; e.type = f[4]; e.date = f[5];
    e.test = iequals(f[6], "yes");
    e.report = iequals(f[7], "yes");
    e.origin = f.size() > 8 ? f[8] : "hand";
    e.commit = f.size() > 9 ? f[9] : "-";
    e.series = series_of(e.version);
    r.entries.push_back(e);
  }
  return true;
}
static int vendor_rank(const string& v) { return v == "CS" ? 0 : v == "ES" ? 1 : v == "MS" ? 2 : v == "PS" ? 3 : 4; }
static int flavour_rank(const string& f) {
  static const char* order[] = {"plain", "UBASAN", "MSAN", "TSAN", "VAL", "GAL"};
  for (int i = 0; i < 6; i++) if (f == order[i]) return i;
  return 6;
}
static bool entry_order(const BuildEntry& a, const BuildEntry& b) {
  if (vendor_rank(a.vendor) != vendor_rank(b.vendor)) return vendor_rank(a.vendor) < vendor_rank(b.vendor);
  int c = version_cmp(a.series, b.series);
  if (c != 0) return c < 0;
  if (flavour_rank(a.flavour) != flavour_rank(b.flavour)) return flavour_rank(a.flavour) < flavour_rank(b.flavour);
  if (a.type != b.type) return a.type == "dbg";
  return a.name < b.name;
}
string registry_format(const Registry& r) {
  vector<BuildEntry> v = r.entries;
  std::sort(v.begin(), v.end(), entry_order);
  size_t w = 4, wv = 7;
  for (auto& e : v) { w = std::max(w, e.name.size()); wv = std::max(wv, e.version.size()); }
  string out = REG_HEADER;
  for (auto& e : v)
    out += fmt("%-*s %-2s %-*s %-6s %-3s %s %-3s %-3s %-6s %s\n", (int)w, e.name.c_str(), e.vendor.c_str(),
               (int)wv, e.version.c_str(), e.flavour.c_str(), e.type.c_str(), e.date.c_str(),
               e.test ? "yes" : "no", e.report ? "yes" : "no", e.origin.c_str(), e.commit.c_str());
  return out;
}
bool registry_save(const Registry& r, const string& path_in) {
  return write_file(path_in.empty() ? g_paths.builds_file : path_in, registry_format(r));
}

// Adds the builds the scan found and the list lacks, drops lines whose basedir is gone, and applies the
// rule: a newer build of a listed key replaces the older lines of that key (they stay on disk). Lines that
// are already listed are left as they are, so a hand-set mark survives. On the first import the old hand
// list /test/REGEX_EXCLUDE is honoured once, so the first list matches what gendirs.sh gave.
void registry_sync(Registry& r, const vector<Basedir>& scanned, bool first_import, bool check_disk) {
  if (check_disk)
    for (auto it = r.entries.begin(); it != r.entries.end();) {
      if (!dir_exists(it->path())) { r.notices.push_back("gone from disk, line removed: " + it->name); it = r.entries.erase(it); }
      else ++it;
    }
  std::optional<std::regex> excl;
  if (first_import) {
    string x = trim(read_file(g_cfg.test_dir + "/REGEX_EXCLUDE"));
    if (!x.empty()) { try { excl = std::regex(x); } catch (...) { r.notices.push_back("REGEX_EXCLUDE does not parse, not used"); } }
  }
  for (auto& b : scanned) {
    if (b.in_tree || !b.tag.empty()) continue;                 // a feature build is never listed
    bool listed = false;
    for (auto& e : r.entries) if (e.name == b.name) {
      listed = true;
      if ((e.commit.empty() || e.commit == "-") && !b.git_rev.empty()) e.commit = b.git_rev;   // a line written before the commit was known
      break;
    }
    if (listed) continue;
    BuildEntry e;
    if (!entry_from_basedir(b, e)) { r.notices.push_back("name does not parse, not listed: " + b.name); continue; }
    if (excl && std::regex_search(b.name, *excl)) continue;
    e.origin = "scan";
    entry_defaults(e);
    const BuildEntry* newest = nullptr;
    for (auto& x : r.entries) if (x.key() == e.key() && (!newest || entry_newer(x, *newest))) newest = &x;
    if (newest && !entry_newer(e, *newest)) continue;          // an older build of a listed key: not listed
    if (newest) {
      string old = newest->name;
      for (auto it = r.entries.begin(); it != r.entries.end();) {
        if (it->key() == e.key()) it = r.entries.erase(it); else ++it;
      }
      r.notices.push_back("new build " + e.name + " replaces " + old);
    } else if (!first_import) {
      r.notices.push_back("new build listed: " + e.name);
    }
    r.entries.push_back(e);
  }
}
Registry registry_current() {
  Registry r;
  bool had = registry_load(r);
  string before = had ? registry_format(r) : "";
  registry_sync(r, basedirs_scan(g_cfg.test_dir), !had, true);
  string after = registry_format(r);
  if (!had || after != before) {
    if (registry_save(r)) {
      if (!had) r.notices.push_back(fmt("created %s from the %s scan: %zu builds listed", g_paths.builds_file.c_str(), g_cfg.test_dir.c_str(), r.entries.size()));
    } else {
      r.notices.push_back("cannot write " + g_paths.builds_file);
    }
  }
  return r;
}
vector<string> windows_builds_without_mtr(const Registry& r) {
  vector<string> out;
  for (auto& e : r.entries) {
    Basedir b;
    if (!basedir_parse_name(e.name, b) || !b.windows) continue;
    b.path = e.path();
    if (dir_exists(b.path) && basedir_test_dir(b).empty()) out.push_back(e.name);
  }
  return out;
}
string mtr_suite_fix() {
  return "the Windows builds of C:\\test come from build.ps1, whose -DBUILD_CONFIG=mysql_release leaves INSTALL_MYSQLTESTDIR empty on Windows, "
         "so none has mariadb-test. Put '-DINSTALL_MYSQLTESTDIR=mariadb-test' into the cmake arguments of build.ps1 for new builds; "
         "for one that exists, configure its source again with that flag and install the Test component into it "
         "(cmake --install <builddir> --component Test): docs/windows.md";
}
string test_dir_empty_note() {
  if (!kTakeFixes || !basedirs_scan(g_cfg.test_dir).empty()) return "";       // Linux says nothing here until it is decided
  string note = "no build under TEST_DIR " + g_cfg.test_dir;
  if (kHostMsys2 && g_cfg.test_dir != "/c/test") {
    size_t n = basedirs_scan("/c/test").size();
    if (n) note += fmt(", but /c/test (C:\\test) holds %zu: omnium config TEST_DIR=/c/test", n);
  }
  return note;
}
vector<string> registry_names(const Registry& r, bool test_set) {
  vector<string> out;
  for (auto& e : r.entries) if (test_set ? e.test : e.report) out.push_back(e.name);
  return out;
}
const BuildEntry* registry_find(const Registry& r, const string& name_or_path) {
  string n = basename_of(name_or_path);
  for (auto& e : r.entries) if (e.name == n) return &e;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// known_bugs.strings and known_bugs.strings.SAN: the source of truth, grep -Fi semantics kept
// ---------------------------------------------------------------------------------------------
static const char* KB_HEADER = "##### CURRENT BUGS (Search key: Mac) #####";
static const size_t KB_KEY_COL = 175;   // the UID is padded to this width, then "## MDEV-n"

bool kb_uid_is_san(const string& uid) { return uid.find("SAN") != string::npos; }
string kb_file_for(const string& uid) { return kb_uid_is_san(uid) ? g_paths.known_bugs_san : g_paths.known_bugs; }
// a crash, assert or sanitizer UID goes right after the header; an error-log UID at the end of that block
static bool kb_uid_is_stack(const string& uid) {
  if (starts_with(uid, "SIG") || uid.find("|SIG") != string::npos) return true;
  for (const char* p : {"ASAN|", "UBSAN|", "LSAN|", "TSAN|", "MSAN|"}) if (starts_with(uid, p)) return true;
  return false;
}
string kb_format_line(const string& uid, const string& key) {
  string s = uid;
  if (s.size() < KB_KEY_COL) s.append(KB_KEY_COL - s.size(), ' '); else s += ' ';
  return s + "## " + key;
}
static string field_from_end(const vector<string>& f, size_t k, const string& dflt) {
  return f.size() > k ? f[f.size() - 1 - k] : dflt;
}
// the frame tt searches on: the first frame, or the second when the first is too generic
static string search_frame(const string& uid, int* pos) {
  auto f = split(uid, '|');
  string fx = f.size() >= 5 ? field_from_end(f, 3, uid) : uid;
  if (pos) *pos = 1;
  if (fx == "ut_dbg_assertion_failed" || fx == "mysql_execute_command") {
    fx = f.size() >= 5 ? field_from_end(f, 2, uid) : uid;
    if (pos) *pos = 2;
  }
  return fx;
}
// A UID is what a compiler and a debugger make of the source, and the two platforms spell some things
// differently where the meaning is the same. GCC prints the NULL macro as __null and MSVC as 0, so a Windows
// UID reads "thd->free_list == 0" for the assertion the list holds as "thd->free_list == __null". gdb prints
// a template argument as 64u, true and "List_iterator_fast, Item", and "> >" for a nested list, where MSVC's
// PDB has 64, 1, "List_iterator_fast,Item" and ">>"; a pointer as "Item*" where MSVC has "Item *"; int64_t
// as long where MSVC has __int64. A match reads both sides in one spelling. A UID is never rewritten, and a
// real 0 (x == 0, the leading 0 of a DBUG_ASSERT(0) UID) stays a 0: only the spellings are made one.
// Only a Windows box does this: a Linux box has one spelling, so its matching stays as it was.
//
// Two macros read differently as well. A Windows build is never built with WSREP, so WSREP(thd) is (0) in it, and
// so is wsrep_emulate_bin_log: the list's "((thd && (WSREP_PROVIDER_EXISTS_ && thd->variables.wsrep_on)) &&
// wsrep_emulate_bin_log) || mysql_bin_log.is_open()" is a Windows UID's "((0) && (0)) || mysql_bin_log.is_open()".
// And UINT_MAX is glibc's (2147483647 *2U +1U) where MSVC has 0xffffffff.
static string kb_macros_plain(string s) {
  static const std::regex wsrep_thd("\\([A-Za-z_][A-Za-z_0-9]* && \\(WSREP_PROVIDER_EXISTS_ && [A-Za-z_][A-Za-z_0-9]*->variables\\.wsrep_on\\)\\)");
  static const std::regex wsrep_on("\\(WSREP_PROVIDER_EXISTS_ && [A-Za-z_][A-Za-z_0-9]*->variables\\.wsrep_on\\)");
  if (s.find("WSREP_PROVIDER_EXISTS_") != string::npos) {
    s = std::regex_replace(s, wsrep_thd, "(0)");
    s = std::regex_replace(s, wsrep_on, "(0)");
  }
  s = replace_all(s, "wsrep_emulate_bin_log", "(0)");
  return replace_all(s, "(2147483647 *2U +1U)", "0xffffffff");
}
static string kb_canon(const string& in) {
  const string s = kb_macros_plain(in);
  string o;
  o.reserve(s.size());
  const size_t n = s.size();
  auto word = [&](size_t i) { return i < n && (isalnum((unsigned char)s[i]) || s[i] == '_'); };
  auto is = [&](size_t i, const char* w) { size_t k = strlen(w); return s.compare(i, k, w) == 0 && !word(i + k); };   // the whole word w at i
  for (size_t i = 0; i < n;) {
    bool start = i == 0 || !word(i - 1);                       // a word begins here
    char c = s[i];
    if (start && is(i, "__null")) { o += '0'; i += 6; }
    else if (start && is(i, "true")) { o += '1'; i += 4; }
    else if (start && is(i, "false")) { o += '0'; i += 5; }
    else if (start && is(i, "__int64")) { o += "long"; i += 7; }
    else if (start && is(i, "long long")) { o += "long"; i += 9; }
    else if (start && isdigit((unsigned char)c)) {             // a number: 64u, 4ul and 8ll are 64, 4 and 8
      size_t e = i, d = i;
      while (word(e)) e++;
      while (d < e && isdigit((unsigned char)s[d])) d++;
      string suffix = lower(s.substr(d, e - d));
      bool unit = suffix == "u" || suffix == "l" || suffix == "ul" || suffix == "ll" || suffix == "ull";
      o += s.substr(i, (unit ? d : e) - i);
      i = e;
    }
    else if (c == ',' && i + 1 < n && s[i + 1] == ' ') { o += ','; i += 2; }
    else if (c == '>' && i + 2 < n && s[i + 1] == ' ' && s[i + 2] == '>') { o += '>'; i += 2; }
    else if (c == ' ' && i + 1 < n && (s[i + 1] == '*' || s[i + 1] == '&')) i++;
    else { o += c; i++; }
  }
  return o;
}
// grep -Fi of a UID in a known-bugs line; on a Windows box both are read as kb_canon does
bool kb_line_has(const string& line, const string& uid) {
  return icontains(line, uid) || (kHostMsys2 && icontains(kb_canon(line), kb_canon(uid)));
}
// A frame without its template arguments, row_search_mvcc<InnoDBPolicy<1,1> > as row_search_mvcc; false (and nothing
// to strip) when its brackets do not pair, as those of operator<, operator<< and operator-> do not
static bool kb_strip_templates(const string& frame, string& bare) {
  bare.clear();
  int depth = 0;
  for (char c : frame) {
    if (c == '<') depth++;
    else if (c == '>') { if (!depth) return false; depth--; }
    else if (!depth) bare += c;
  }
  return depth == 0;
}
KbMatch kb_search(const string& uid) {
  KbMatch m;
  m.san = kb_uid_is_san(uid);
  m.frame = search_frame(uid, &m.frame_pos);
  string txt = read_file(m.san ? g_paths.known_bugs_san : g_paths.known_bugs);
  string uid_c = kHostMsys2 ? kb_canon(uid) : "", frame_c = kHostMsys2 ? kb_canon(m.frame) : "";
  vector<string> lines = split_lines(txt);
  vector<string> lines_c;                                      // the lines as kb_canon reads them, on a Windows box
  for (auto& line : lines) {
    string line_c = kHostMsys2 ? kb_canon(line) : "";
    if (icontains(line, uid) || (kHostMsys2 && icontains(line_c, uid_c))) m.exact.push_back(line);
    if (!m.frame.empty() && (icontains(line, m.frame) || (kHostMsys2 && icontains(line_c, frame_c)))) m.partial.push_back(line);
    if (kHostMsys2) lines_c.push_back(line_c);
  }
  // A Windows frame with template arguments, row_search_mvcc<InnoDBPolicy<1,1> >, where the list has the frame bare,
  // row_search_mvcc: gdb's output as the list holds it has them in some lines and not in others. So a frame's
  // arguments are also tried away, in every combination of the frames that have any (a line that has them still
  // matches by the arguments, a line that has none by the bare frame).
  if (kHostMsys2 && m.exact.empty()) {
    vector<string> f = split(uid, '|');
    size_t sig = 0;
    while (sig < f.size() && !starts_with(f[sig], "SIG")) sig++;       // the frames follow the signal
    vector<size_t> with;
    string bare;
    for (size_t i = sig + 1; i < f.size(); i++) if (f[i].find('<') != string::npos && kb_strip_templates(f[i], bare) && bare != f[i]) with.push_back(i);
    if (sig < f.size() && !with.empty() && with.size() <= 4) {
      for (unsigned mask = 1; mask < (1u << with.size()); mask++) {
        vector<string> g = f;
        for (size_t k = 0; k < with.size(); k++) if (mask & (1u << k)) { kb_strip_templates(f[with[k]], bare); g[with[k]] = bare; }
        string variant_c = kb_canon(join(g, "|"));
        for (size_t i = 0; i < lines.size(); i++)
          if (icontains(lines_c[i], variant_c) && std::find(m.exact.begin(), m.exact.end(), lines[i]) == m.exact.end()) m.exact.push_back(lines[i]);
      }
    }
    // and the first frame by itself, bare, as a partial match, when nothing else listed one
    if (m.partial.empty() && kb_strip_templates(m.frame, bare) && bare != m.frame && !bare.empty()) {
      string bare_c = kb_canon(bare);
      for (size_t i = 0; i < lines.size(); i++) if (icontains(lines_c[i], bare_c)) m.partial.push_back(lines[i]);
    }
  }
  // A frameless assertion, ASSERT|<text>, is what a plain assert() leaves in a Windows plugin: the CRT's line and no
  // frames. The list holds the same assertion as <text>|SIGABRT|frames, so those lines are its entry, when they are one
  // bug's: <text> followed by |SIGABRT| is exact on the text, and a key (MDEV-n) that every such line carries says that
  // the text belongs to one bug. A generic text ('length > 0') is several bugs' and says nothing about this one: its
  // lines are only offered as a partial match. Linux keeps its verdict until it is decided.
  if (kTakeFixes && m.exact.empty() && starts_with(uid, "ASSERT|") && uid.size() > 7) {
    string text = uid.substr(7), head = lower(text + "|SIGABRT|");
    vector<string> hits;
    for (auto& line : lines) {
      size_t b = line.find_first_not_of("# \t");
      if (b != string::npos && starts_with(lower(line.substr(b)), head)) hits.push_back(line);
    }
    if (!hits.empty()) {
      vector<string> common = kb_keys({hits[0]});
      for (auto& h : hits) {
        vector<string> ks = kb_keys({h});
        common.erase(std::remove_if(common.begin(), common.end(), [&](const string& k) { return std::find(ks.begin(), ks.end(), k) == ks.end(); }), common.end());
      }
      if (!common.empty()) m.exact = hits;
      else { m.partial = hits; m.frame = text; m.frame_pos = 0; }
    }
  }
  return m;
}
KbVerdict kb_verdict(const KbMatch& m) {
  if (m.exact.empty()) return m.partial.empty() ? KbVerdict::NotFound : KbVerdict::Partial;
  bool open = false, fixed = false;
  for (auto& l : m.exact) { if (starts_with(l, "#")) fixed = true; else open = true; }
  if (!open) return KbVerdict::FixedOnly;
  return fixed ? KbVerdict::KnownAndFixed : KbVerdict::Known;
}
vector<string> kb_keys(const vector<string>& lines) {
  static const std::regex re("(MDEV|MENT|SPECIAL)-[0-9]+");
  vector<string> out;
  for (auto& l : lines)
    for (std::sregex_iterator it(l.begin(), l.end(), re), end; it != end; ++it) {
      string k = it->str();
      if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    }
  return out;
}
string kb_verdict_text(const string& uid, const KbMatch& m) {
  string out = "----- String Scan -----\n";
  switch (kb_verdict(m)) {
    case KbVerdict::NotFound:
      out += "BUG NOT FOUND IN KNOWN BUGS LIST! POTENTIALLY NEW BUG TO LOG; SEARCH FIRST:\n";
      break;
    case KbVerdict::Partial:
      if (m.frame_pos == 0)                                    // a frameless assertion: the text of several bugs' entries
        out += fmt("BUG NOT FOUND (IDENTICALLY) IN KNOWN BUGS LIST! POTENTIALLY NEW BUG TO LOG. HOWEVER, THE ASSERTION TEXT ('%s'), WHICH HAS NO FRAMES, IS THAT OF THESE ENTRIES, WHICH DO NOT SHARE ONE BUG KEY: (PLEASE CHECK IT IS NOT THE SAME BUG):\n",
                   m.frame.c_str());
      else
        out += fmt("BUG NOT FOUND (IDENTICALLY) IN KNOWN BUGS LIST! POTENTIALLY NEW BUG TO LOG. HOWEVER, A PARTIAL MATCH BASED ON THE %s FRAME ('%s') WAS FOUND, AS FOLLOWS: (PLEASE CHECK IT IS NOT THE SAME BUG):\n",
                   m.frame_pos == 1 ? "1st" : "2nd", m.frame.c_str());
      out += join(m.partial, "\n") + "\n";
      break;
    case KbVerdict::KnownAndFixed:
      out += "ALREADY KNOWN BUG! NOTE: PREVIOUSLY FIXED (OR NON-FILTERED) BUG(S) AS WELL AS ALREADY KNOWN BUG(S) WERE FOUND\n";
      out += join(m.exact, "\n") + "\n";
      break;
    case KbVerdict::FixedOnly:
      out += "ALREADY KNOWN *PREVIOUSLY FIXED (OR NON-FILTERED)* BUG! POTENTIALLY NEW BUG TO LOG; SEARCH(/CHECK) FIRST:\n";
      out += join(m.exact, "\n") + "\n";
      break;
    case KbVerdict::Known:
      out += "ALREADY KNOWN BUG!\n";
      out += join(m.exact, "\n") + "\n";
      break;
  }
  (void)uid;
  return out;
}
// perl URI::Escape defaults: A-Z a-z 0-9 - _ . ! ~ * ' ( ) pass, all else is %XX
string uri_escape(const string& s) {
  string o;
  char buf[8];
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("-_.!~*'()", c)) o += (char)c;
    else { snprintf(buf, sizeof buf, "%%%02X", c); o += buf; }
  }
  return o;
}
// The assertion texts Jira may hold for a UID read on Windows. Jira has GCC's __null where the UID has
// MSVC's 0, and the UID cannot say which of its 0s is a NULL. Only a 0 that == or != compares can be
// one (a pointer is tested against NULL, not with >= or %), so those are read as __null: all of them
// together, and when there are several, each of them alone. The text with its 0s is searched as well,
// by the URL before. Nothing when no 0 qualifies, as for any UID without a comparison against 0.
static vector<string> kb_null_readings(const string& a) {
  static const std::regex zero("(==|!=) *0(?![A-Za-z0-9_.])");
  vector<size_t> at;                                           // the offset of each such 0
  for (std::sregex_iterator it(a.begin(), a.end(), zero), end; it != end; ++it) at.push_back((size_t)(it->position() + it->length() - 1));
  auto as_null = [&](const vector<size_t>& zeros) {
    string s = a;
    for (size_t i = zeros.size(); i-- > 0;) s.replace(zeros[i], 1, "__null");
    return s;
  };
  vector<string> out;
  if (at.empty()) return out;
  out.push_back(as_null(at));
  if (at.size() > 1 && at.size() <= 8) for (size_t z : at) out.push_back(as_null(vector<size_t>{z}));
  return out;
}
// The words of a frame without its template arguments: Bitmap<64>::set_bit is Bitmap and set_bit. Jira holds
// the frame as gdb printed it (Bitmap<64u>, InnoDBPolicy<true, true>) and a UID read on Windows has the MSVC
// spelling, so a phrase with the argument finds nothing, where its words find the issue. A frame whose
// brackets do not pair (operator<) comes back whole.
static vector<string> kb_frame_words(const string& frame) {
  string bare;
  vector<string> out;
  if (!kb_strip_templates(frame, bare)) { out.push_back(frame); return out; }
  for (size_t p = 0; p <= bare.size();) {
    size_t q = bare.find("::", p);
    string w = trim(bare.substr(p, q == string::npos ? string::npos : q - p));
    if (!w.empty()) out.push_back(w);
    if (q == string::npos) break;
    p = q + 2;
  }
  return out;
}
vector<string> kb_jira_urls(const string& uid) {
  static const string base = "https://jira.mariadb.org/issues/?jql=";
  static const string tail = "%20ORDER%20BY%20status%20ASC%2Cupdated%20DESC";
  auto f = split(uid, '|');
  string fx = search_frame(uid, nullptr);
  string fy = f.size() >= 4 ? field_from_end(f, 2, uid) : uid;
  string fz = f.size() >= 3 ? field_from_end(f, 1, uid) : uid;
  fx = replace_all(replace_all(fx, "MUTEX_ERROR|", ""), "MUTEX_ERROR", "");
  vector<string> urls;
  // a frameless assertion, ASSERT|<text> (what a plain assert() leaves in a Windows plugin), has no frames to search by:
  // its text is the search, and the URLs by "ASSERT|text" that Linux gets for it find nothing
  bool frameless = kTakeFixes && starts_with(uid, "ASSERT|") && uid.size() > 7;
  if (!frameless)
    urls.push_back(base + "text%20~%20%22%5C%22" + uri_escape(fx) + "%5C%22%22%20and%20text%20~%20%22%5C%22" +
                   uri_escape(fy) + "%5C%22%22%20and%20text%20~%20%22%5C%22" + uri_escape(fz) + "%5C%22%22" + tail);
  string a = replace_all(uid, "||", "\x01");
  a = a.substr(0, a.find('|'));
  a = replace_all(a, "\x01", "||");
  a = replace_all(replace_all(a, "MUTEX_ERROR|", ""), "MUTEX_ERROR", "");
  if (frameless) a = uid.substr(7);
  if (!a.empty() && (frameless || (a != "SIGSEGV" && a != "SIGABRT" && !kb_uid_is_san(uid)))) {
    urls.push_back(base + "text%20~%20%22%5C%22" + uri_escape(a) + "%5C%22%22" + tail);
    string alt;
    if (kHostMsys2)                                            // a Windows box: a 0 may be Jira's __null
      for (auto& v : kb_null_readings(a)) alt += (alt.empty() ? "" : "%20or%20") + string("text%20~%20%22%5C%22") + uri_escape(v) + "%5C%22%22";
    if (!alt.empty()) urls.push_back(base + alt + tail);
  }
  // on a Windows box, the frames that have a template argument, by their words, in one URL more
  if (kHostMsys2 && f.size() >= 5 && (fx.find('<') != string::npos || fy.find('<') != string::npos || fz.find('<') != string::npos)) {
    string q;
    for (const string* fr : {&fx, &fy, &fz})
      for (auto& w : kb_frame_words(*fr)) q += (q.empty() ? "" : "%20and%20") + string("text%20~%20%22%5C%22") + uri_escape(w) + "%5C%22%22";
    urls.push_back(base + q + tail);
  }
  return urls;
}
bool kb_add_to(const string& path, const string& uid, const string& key, string* err) {
  string txt = read_file(path);
  if (txt.empty()) { if (err) *err = "cannot read " + path; return false; }
  auto lines = split_lines(txt);
  for (auto& l : lines)
    if (!starts_with(l, "#") && kb_line_has(l, uid)) { if (err) *err = "already listed: " + l; return false; }
  size_t h = string::npos;
  for (size_t i = 0; i < lines.size(); i++) if (trim(lines[i]) == KB_HEADER) { h = i; break; }
  if (h == string::npos) { if (err) *err = string("header not found in ") + path + ": " + KB_HEADER; return false; }
  size_t at = h + 1;
  if (!kb_uid_is_stack(uid)) {
    size_t i = h + 1;
    while (i < lines.size() && !starts_with(lines[i], "#####")) i++;
    at = i;
    while (at > h + 1 && trim(lines[at - 1]).empty()) at--;
  }
  lines.insert(lines.begin() + (long)at, kb_format_line(uid, key));
  if (!write_file(path, join(lines, "\n") + "\n")) { if (err) *err = "cannot write " + path; return false; }
  return true;
}
bool kb_add(const string& uid, const string& key, bool force_san, string* err) {
  return kb_add_to(force_san ? g_paths.known_bugs_san : kb_file_for(uid), uid, key, err);
}
static bool has_key_token(const string& line, const string& key) {
  size_t p = 0;
  while ((p = line.find(key, p)) != string::npos) {
    size_t e = p + key.size();
    if (e >= line.size() || !isdigit((unsigned char)line[e])) return true;
    p = e;
  }
  return false;
}
// mark_as_fixed.sh: the line moves to the end of the file as "# <uid> ## Fixed ## MDEV-n", eleven pad blanks less
int kb_fixed_in(const string& path, const string& key) {
  string txt = read_file(path);
  if (txt.empty()) return 0;
  string kind = key.substr(0, key.find('-'));
  vector<string> keep, moved;
  for (auto& l : split_lines(txt)) {
    if (!starts_with(l, "##") && l.find("## Fixed") == string::npos && has_key_token(l, key)) {
      string m = l;
      size_t p = m.find("           ## " + kind);
      if (p != string::npos) m.erase(p, 11);
      p = m.find("## " + kind);
      if (p != string::npos) m.insert(p, "## Fixed ");
      moved.push_back("# " + m);
    } else {
      keep.push_back(l);
    }
  }
  if (moved.empty()) return 0;
  for (auto& m : moved) keep.push_back(m);
  if (!write_file(path, join(keep, "\n") + "\n")) return -1;
  return (int)moved.size();
}
int kb_fixed(const string& key, vector<string>* also_mentioned) {
  int n = kb_fixed_in(g_paths.known_bugs, key);
  int m = kb_fixed_in(g_paths.known_bugs_san, key);
  if (also_mentioned) {
    for (const string& f : {g_paths.asan_filter, g_paths.ubsan_filter, g_paths.qa + "/MSAN.ignorelist",
                            g_paths.qa + "/filter.sql.info", g_paths.qa + "/REGEX_ERRORS_FILTER.info"})
      if (icontains(read_file(f), key)) also_mentioned->push_back(f);
  }
  if (n < 0 || m < 0) return -1;
  return n + m;
}

// ---------------------------------------------------------------------------------------------
// BUGS/MDEV-n.sql: the stored testcase, plain SQL, one # header when the replay needs server options
// ---------------------------------------------------------------------------------------------
string bug_key_normalize(const string& in) {
  string u = upper(in), digits;
  for (char c : u) if (isdigit((unsigned char)c)) digits += c;
  if (digits.empty()) return "";
  string kind = "MDEV";
  if (u.find("MENT") != string::npos) kind = "MENT";
  else if (u.find("SPECIAL") != string::npos) kind = "SPECIAL";
  return kind + "-" + digits;
}
string bugs_file_for(const string& key) { return g_paths.bugs_dir + "/" + key + ".sql"; }
bool bugs_write(const string& key, const string& options, const string& sql, string* err) {
  string path = bugs_file_for(key);
  if (file_exists(path)) { if (err) *err = "exists: " + path; return false; }
  string body;
  if (!trim(options).empty()) body += "# mysqld options required for replay: " + trim(options) + "\n";
  body += sql;
  if (!body.empty() && body.back() != '\n') body += '\n';
  if (!write_file(path, body)) { if (err) *err = "cannot write " + path; return false; }
  return true;
}

// ---------------------------------------------------------------------------------------------
// REGEX_ERRORS_SCAN, REGEX_ERRORS_FILTER, REGEX_ERRORS_LASTLINE: one line, alternatives joined by |
// ---------------------------------------------------------------------------------------------
vector<string> regex_list_read(const string& path) {
  string t = trim(read_file(path));
  vector<string> out;
  string cur;
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '|' && (i == 0 || t[i - 1] != '\\')) { out.push_back(cur); cur.clear(); }
    else cur += t[i];
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
bool regex_list_add(const string& path, const string& alt) {
  string t = trim(read_file(path));
  for (auto& a : regex_list_read(path)) if (a == alt) return false;
  t = t.empty() ? alt : t + "|" + alt;
  return write_file(path, t + "\n");
}

// ---------------------------------------------------------------------------------------------
// the verbs
// ---------------------------------------------------------------------------------------------
int open_in_editor(const string& path) {
  string ed = g_cfg.editor;
  if (ed.empty()) { const char* e = getenv("EDITOR"); ed = (e && *e) ? e : "vi"; }
  int st = std::system((ed + " " + sh_quote(path)).c_str());   // a wait status, not an exit code
  return st == -1 ? 127 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static void print_notices(const Registry& r) {
  for (auto& n : r.notices) printf("note: %s\n", n.c_str());
}
int cmd_builds(const Args& a) {
  if (!a.empty() && a[0] == "edit") {
    if (!file_exists(g_paths.builds_file)) registry_current();
    return open_in_editor(g_paths.builds_file);
  }
  Registry r = registry_current();
  // builds newest <dbg|opt|uba-dbg|uba-opt|msan-dbg|...> [version or tag]: the path, for the cli
  if (!a.empty() && a[0] == "newest") {
    string want = a.size() > 1 ? lower(a[1]) : "dbg";
    vector<string> filters(a.begin() + (a.size() > 1 ? 2 : 1), a.end());
    bool dbg = want.find("opt") == string::npos;
    Flavour fl = Flavour::Plain;
    if (want.find("uba") != string::npos) fl = Flavour::UBASAN;
    else if (want.find("msan") != string::npos) fl = Flavour::MSAN;
    else if (want.find("tsan") != string::npos) fl = Flavour::TSAN;
    else if (want.find("val") != string::npos) fl = Flavour::VAL;
    const BuildEntry* best = nullptr;
    for (auto& e : r.entries) {
      Basedir b;
      if (!basedir_probe(e.path(), b)) continue;
      if (b.dbg != dbg || b.flavour != fl) continue;
      bool ok = true;
      for (auto& f : filters) {
        string lf = lower(f);
        if (lf == "es") { if (!b.es) ok = false; continue; }
        if (lf == "cs") { if (b.es) ok = false; continue; }
        if (lower(b.name).find(lf) == string::npos && lower(b.version).find(lf) == string::npos && lower(b.series).find(lf) == string::npos) ok = false;
      }
      if (!ok) continue;
      if (!best || entry_newer(e, *best)) best = &e;
    }
    if (!best) return 1;
    printf("%s\n", best->path().c_str());
    return 0;
  }
  if (!a.empty() && a[0] == "set") {
    if (a.size() < 3) { printf("usage: omnium builds set <name> test=yes|no [report=yes|no]\n"); return 2; }
    BuildEntry* e = nullptr;
    string n = basename_of(a[1]);
    for (auto& x : r.entries) if (x.name == n) e = &x;
    if (!e) { printf("not listed: %s\n", n.c_str()); return 1; }
    for (size_t i = 2; i < a.size(); i++) {
      size_t eq = a[i].find('=');
      string k = eq == string::npos ? a[i] : a[i].substr(0, eq), v = eq == string::npos ? "yes" : a[i].substr(eq + 1);
      bool yes = iequals(v, "yes");
      if (k == "test") e->test = yes; else if (k == "report") e->report = yes;
      else { printf("unknown mark %s (test or report)\n", k.c_str()); return 2; }
    }
    if (!registry_save(r)) { printf("cannot write %s\n", g_paths.builds_file.c_str()); return 1; }
    printf("saved %s\n", g_paths.builds_file.c_str());
    return 0;
  }
  if (!a.empty() && (a[0] == "test" || a[0] == "report")) {
    for (auto& n : registry_names(r, a[0] == "test")) printf("%s\n", n.c_str());
    return 0;
  }
  if (!a.empty() && a[0] != "scan") { printf("usage: omnium builds [scan|edit|test|report|set <name> test=yes|no report=yes|no]\n"); return 2; }
  fputs(registry_format(r).c_str(), stdout);
  print_notices(r);
  if (r.entries.empty()) { string n = test_dir_empty_note(); if (!n.empty()) printf("note: %s\n", n.c_str()); }
  if (kHostMsys2) {
    vector<string> nomtr = windows_builds_without_mtr(r);
    if (!nomtr.empty())
      printf("note: no mariadb-test in %zu of %zu builds, so omnium mtr cannot run a testcase on them: %s\n      %s\n",
             nomtr.size(), r.entries.size(), join(nomtr, ", ").c_str(), mtr_suite_fix().c_str());
  }
  return 0;
}
bool basedir_from_arg(const string& arg, Basedir& b, string* err) {
  string p = arg.empty() ? fs::current_path().string() : arg;
  if (!dir_exists(p) && arg.find('/') == string::npos) p = g_cfg.test_dir + "/" + arg;
  if (!dir_exists(p)) { if (err) *err = "no such directory: " + p; return false; }
  if (!basedir_probe(p, b)) { if (err) *err = "not a basedir (no server binary): " + p; return false; }
  return true;
}
int cmd_myver(const Args& a) {
  bool short_form = false;
  string arg;
  for (auto& s : a) { if (s == "--short") short_form = true; else arg = s; }
  Basedir b;
  string err;
  if (!basedir_from_arg(arg, b, &err)) { printf("%s\n", err.c_str()); return 1; }
  if (short_form) { printf("%s\n", b.short_name().c_str()); return 0; }
  fputs(basedir_banner(b).c_str(), stdout);
  return 0;
}
static int kb_verb(const Args& a, bool san) {
  const char* name = san ? "kba" : "kb";
  string file = san ? g_paths.known_bugs_san : g_paths.known_bugs;
  if (a.empty() || a[0] == "edit") return open_in_editor(file);
  if (a[0] == "search" || a[0] == "s") {
    if (a.size() < 2) { printf("usage: omnium %s search <uid>\n", name); return 2; }
    KbMatch m = kb_search(a[1]);
    fputs(kb_verdict_text(a[1], m).c_str(), stdout);
    for (auto& u : kb_jira_urls(a[1])) printf("%s\n", u.c_str());
    return kb_verdict(m) == KbVerdict::Known || kb_verdict(m) == KbVerdict::KnownAndFixed ? 0 : 1;
  }
  if (a[0] == "add") {
    if (a.size() < 3) { printf("usage: omnium %s add <uid> <MDEV-n>\n", name); return 2; }
    string key = bug_key_normalize(a[2]), err;
    if (key.empty()) { printf("not a ticket key: %s\n", a[2].c_str()); return 2; }
    if (!kb_add(a[1], key, san, &err)) { printf("%s\n", err.c_str()); return 1; }
    string f = san ? g_paths.known_bugs_san : kb_file_for(a[1]);
    printf("added to %s\n%s\ngit add %s\n", f.c_str(), kb_format_line(a[1], key).c_str(), f.c_str());
    return 0;
  }
  if (a[0] == "fixed") {
    if (a.size() < 2) { printf("usage: omnium %s fixed <MDEV-n>\n", name); return 2; }
    string key = bug_key_normalize(a[1]);
    vector<string> also;
    int n = kb_fixed(key, &also);
    if (n < 0) { printf("cannot write the known-bugs files\n"); return 1; }
    printf("%d line(s) marked fixed for %s\n", n, key.c_str());
    for (auto& f : also) printf("note: %s also mentions %s\n", f.c_str(), key.c_str());
    if (n > 0) printf("git add %s %s\n", g_paths.known_bugs.c_str(), g_paths.known_bugs_san.c_str());
    return 0;
  }
  printf("usage: omnium %s [search <uid> | add <uid> <MDEV-n> | fixed <MDEV-n> | edit]\n", name);
  return 2;
}
int cmd_kb(const Args& a) { return kb_verb(a, false); }
int cmd_kba(const Args& a) { return kb_verb(a, true); }
static int kbs_verb(const Args& a, bool san) {
  if (a.empty()) { printf("usage: omnium %s <text>\n", san ? "kbsa" : "kbs"); return 2; }
  string needle = join(a, " ");
  int n = 0;
  for (auto& l : split_lines(read_file(san ? g_paths.known_bugs_san : g_paths.known_bugs)))
    if (icontains(l, needle)) { printf("%s\n", l.c_str()); n++; }
  return n ? 0 : 1;
}
int cmd_kbs(const Args& a) { return kbs_verb(a, false); }
int cmd_kbsa(const Args& a) { return kbs_verb(a, true); }
int cmd_eb(const Args& a) {
  if (a.empty()) { printf("usage: omnium eb <MDEV-n | MENT-n | n>\n"); return 2; }
  string key = bug_key_normalize(a[0]);
  if (key.empty()) { printf("not a ticket key: %s\n", a[0].c_str()); return 2; }
  string path = bugs_file_for(key);
  bool existed = file_exists(path);
  int rc = open_in_editor(path);
  if (!existed && file_exists(path)) printf("git add %s\n", path.c_str());
  return rc;
}
