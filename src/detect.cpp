// Created by Roel Van de Paar, MariaDB
// detect.cpp - the UniqueID chain: new_text_string.sh, san_text_string.sh, fallback_text_string.sh,
// error_log_scan.sh, capped_error_log.sh, drop_one_or_more_san_from_log.sh and stack.sh as one
// module. Every text rule is the rule of the script it came from, in the same order, so a trial gets
// the same UniqueID from omnium as from the bash chain. `omnium parity` proves that on saved trials.
#include "verbs.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <sys/wait.h>
#include <sys/stat.h>
#include <glob.h>
#include <fcntl.h>

// The system libpcre2 is not instrumented, so under MemorySanitizer what pcre2 writes for us reads
// as never written. Each value omnium takes out of a pcre2 call is marked written at the boundary.
#define OMNIUM_PCRE2_WROTE(p, n) OMNIUM_LIB_WROTE(p, n)

namespace {

// ---------------------------------------------------------------------------------------------
// a compiled pattern with the three uses the scripts make of one: grep, grep -o and sed s///
// ---------------------------------------------------------------------------------------------
struct Rx {
  pcre2_code* re = nullptr;
  explicit Rx(const char* p, bool caseless = false) {
    int ec = 0;
    PCRE2_SIZE eo = 0;
    re = pcre2_compile((PCRE2_SPTR)p, PCRE2_ZERO_TERMINATED, caseless ? PCRE2_CASELESS : 0, &ec, &eo, nullptr);
    if (!re) {
      PCRE2_UCHAR buf[256];
      pcre2_get_error_message(ec, buf, sizeof buf);
      die("detect: bad pattern %s at %zu: %s", p, (size_t)eo, (const char*)buf);
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
  }
  ~Rx() { if (re) pcre2_code_free(re); }
  Rx(const Rx&) = delete;
  Rx& operator=(const Rx&) = delete;
  bool find(std::string_view s, size_t from, size_t* mb, size_t* me) const {
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    int rc = pcre2_match(re, (PCRE2_SPTR)s.data(), s.size(), from, 0, md, nullptr);
    if (rc >= 0) {
      PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
      OMNIUM_PCRE2_WROTE(ov, 2 * sizeof(PCRE2_SIZE));
      if (mb) *mb = ov[0];
      if (me) *me = ov[1];
    }
    pcre2_match_data_free(md);
    return rc >= 0;
  }
  bool hit(std::string_view s) const { return find(s, 0, nullptr, nullptr); }
  string first(std::string_view s) const {                    // grep -o ... | head -n1 on one line
    size_t b = 0, e = 0;
    size_t from = 0;
    while (from <= s.size() && find(s, from, &b, &e)) {
      if (e > b) return string(s.substr(b, e - b));
      from = e + 1;                                            // grep -o skips an empty match
    }
    return "";
  }
  // sed s/pat/repl/[g]: $1..$9 in repl name a group, $$ is a literal dollar
  string sub(std::string_view s, const char* repl, bool global) const {
    uint32_t opt = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_SUBSTITUTE_UNSET_EMPTY | (global ? PCRE2_SUBSTITUTE_GLOBAL : 0);
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    PCRE2_SIZE outlen = s.size() + 128;
    string out(outlen, '\0');
    int rc = pcre2_substitute(re, (PCRE2_SPTR)s.data(), s.size(), 0, opt, md, nullptr, (PCRE2_SPTR)repl,
                              PCRE2_ZERO_TERMINATED, (PCRE2_UCHAR*)out.data(), &outlen);
    if (rc == PCRE2_ERROR_NOMEMORY) {
      out.assign(outlen + 1, '\0');
      rc = pcre2_substitute(re, (PCRE2_SPTR)s.data(), s.size(), 0, opt, md, nullptr, (PCRE2_SPTR)repl,
                            PCRE2_ZERO_TERMINATED, (PCRE2_UCHAR*)out.data(), &outlen);
    }
    pcre2_match_data_free(md);
    if (rc < 0) return string(s);
    OMNIUM_PCRE2_WROTE(out.data(), outlen);
    out.resize(outlen);
    return out;
  }
};
// one compiled pattern per call site, compiled once
#define RX(name, pat) static const Rx name(pat)
#define RXI(name, pat) static const Rx name(pat, true)

// per line, like sed reads its input
string per_line(const string& text, const std::function<string(const string&)>& f) {
  string out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    string line = text.substr(pos, nl == string::npos ? string::npos : nl - pos);
    out += f(line);
    if (nl == string::npos) break;
    out += '\n';
    pos = nl + 1;
  }
  return out;
}
// the lines grep sees: every line, a final one without a newline included
vector<string> grep_lines(const string& text) {
  vector<string> v;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == string::npos) { v.push_back(text.substr(pos)); break; }
    v.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  for (auto& l : v) if (!l.empty() && l.back() == '\r') l.pop_back();   // a Windows error log ends its lines in CRLF
  return v;
}
// the lines `while read` sees: a final line without a newline is dropped
vector<string> read_lines(const string& text) {
  vector<string> v = grep_lines(text);
  if (!text.empty() && text.back() != '\n' && !v.empty()) v.pop_back();
  return v;
}
size_t newline_count(const string& text) { return (size_t)std::count(text.begin(), text.end(), '\n'); }

// grep -h [-o] over the logs in order, first hit
string grep_first(const vector<string>& logs, const Rx& re, bool only_matching) {
  for (auto& lg : logs) {
    for (auto& l : grep_lines(read_file(lg))) {
      if (!re.hit(l)) continue;
      return only_matching ? re.first(l) : l;
    }
  }
  return "";
}
bool grep_any(const vector<string>& logs, const Rx& re) {
  for (auto& lg : logs) for (auto& l : grep_lines(read_file(lg))) if (re.hit(l)) return true;
  return false;
}
bool grep_any_text(const vector<string>& logs, const char* needle) {      // grep -qi "text"
  for (auto& lg : logs) if (icontains(read_file(lg), needle)) return true;
  return false;
}
// grep -A n (after) / -B n (before), with GNU grep's "--" between groups that do not touch
vector<string> grep_context(const vector<string>& v, const Rx& re, int before, int after, bool first_only) {
  vector<string> out;
  long last_out = -1;
  for (long i = 0; i < (long)v.size(); i++) {
    if (!re.hit(v[i])) continue;
    long s = std::max<long>(0, i - before), e = std::min<long>((long)v.size() - 1, i + after);
    if (s <= last_out) s = last_out + 1;
    else if (last_out >= 0) out.push_back("--");
    for (long k = s; k <= e; k++) out.push_back(v[k]);
    if (e > last_out) last_out = e;
    if (first_only) break;
  }
  return out;
}
string join_nl(const vector<string>& v) { return join(v, "\n"); }

// the source-tree prefix list every *SAN file preparse shares (san_text_string.sh)
const char* const PREFIX_DIRS[] = {"client", "cmake", "dbug", "debian", "extra", "include", "libmariadb", "libmysqld", "libservices",
                                   "mysql-test", "mysys", "mysys_ssl", "plugin", "scripts", "sql", "sql-bench", "sql-common", "storage",
                                   "strings", "support-files", "tests", "tpool", "unittest", "vio", "win", "wsrep-lib", "zlib",
                                   "components", "libbinlogevents", "libbinlogstandalone", "libmysql", "router", "share", "testclients",
                                   "utilities", "regex"};
string strip_source_prefix(string s, const char* extra_dir) {
  RX(build_src, ".*/build/[^ ]*/src/");
  s = build_src.sub(s, "", false);
  static std::map<string, std::unique_ptr<Rx>> cache;
  static std::mutex mtx;
  auto rx_for = [&](const string& d) -> const Rx& {
    std::lock_guard<std::mutex> lk(mtx);
    auto& p = cache[d];
    if (!p) p = std::make_unique<Rx>((".*/" + d + "/").c_str());
    return *p;
  };
  for (auto d : PREFIX_DIRS) s = rx_for(d).sub(s, (string(d) + "/").c_str(), false);
  if (extra_dir) s = rx_for(extra_dir).sub(s, (string(extra_dir) + "/").c_str(), false);
  return s;
}
string cxx_version_rule(const string& s) {
  RX(cxx, "/c\\+\\+/[0-9]+/");
  return cxx.sub(s, "/c++/current_version/", true);
}
string strip_buildid(const string& s) {
  RX(bid, " \\(BuildId: [0-9a-f]+\\)");
  return bid.sub(s, "", false);
}

// ---------------------------------------------------------------------------------------------
// capped_error_log.sh: a log over 10 MB is read as its first 5 MB and its last 5 MB
// ---------------------------------------------------------------------------------------------
const int64_t CAP_MAX = 10485760, CAP_HALF = 5242880;
string g_cap_dir;                                            // this process's copies, removed when it ends
string cap_copy_dir() {
  static std::mutex mtx;
  std::lock_guard<std::mutex> lk(mtx);
  if (g_cap_dir.empty()) {
    sweep_stale_tmp("/tmp/omnium_cap_");                     // what a killed run left behind
    g_cap_dir = fmt("/tmp/omnium_cap_%d", (int)getpid());
    mkdirs(g_cap_dir);
    std::atexit([] { if (!g_cap_dir.empty()) remove_tree(g_cap_dir); });
  }
  return g_cap_dir;
}
}  // namespace

vector<string> capped_logs(const vector<string>& logs) {
  vector<string> out;
  static std::atomic<int> n{0};
  for (auto& lg : logs) {
    int64_t sz = file_size(lg);
    if (sz <= CAP_MAX) { out.push_back(lg); continue; }
    string text = read_file(lg);
    if (text.empty()) { out.push_back(lg); continue; }
    string head = text.substr(0, (size_t)CAP_HALF);
    // head -n -1: everything but the last line of the chunk
    size_t last_nl = head.rfind('\n');
    if (last_nl == string::npos) head.clear();
    else if (last_nl + 1 == head.size()) { size_t p = head.rfind('\n', last_nl - 1); head = p == string::npos ? "" : head.substr(0, p + 1); }
    else head = head.substr(0, last_nl + 1);
    string tail = text.substr(text.size() - (size_t)CAP_HALF);
    // tail -n +2: from the second line on
    size_t first_nl = tail.find('\n');
    tail = first_nl == string::npos ? "" : tail.substr(first_nl + 1);
    string dst = cap_copy_dir() + fmt("/cap%d_%s", ++n, basename_of(lg).c_str());
    if (!write_file(dst, head + tail) || (head.empty() && tail.empty())) { out.push_back(lg); continue; }
    out.push_back(dst);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// error_log_scan.sh
// ---------------------------------------------------------------------------------------------
namespace {
struct ElsLists {
  std::unique_ptr<Rx> scan, filter, lastline;
  string err;
};
const ElsLists& els_lists() {
  static ElsLists e = [] {
    ElsLists r;
    auto load = [](const string& p) { string s = read_file(p); s.erase(std::remove(s.begin(), s.end(), '\n'), s.end()); return s; };
    string scan = load(g_paths.regex_scan), last = load(g_paths.regex_lastline), filt = load(g_paths.regex_filter);
    if (!file_exists(g_paths.regex_scan)) r.err = "Error: " + g_paths.regex_scan + " not readable";
    else if (scan.empty()) r.err = "Error: " + g_paths.regex_scan + " is empty";
    else if (!file_exists(g_paths.regex_lastline)) r.err = "Error: " + g_paths.regex_lastline + " not readable";
    else if (last.empty()) r.err = "Error: " + g_paths.regex_lastline + " is empty";
    if (!r.err.empty()) return r;
    if (filt.empty()) filt = "NOFILTERDUMMY";
    r.scan = std::make_unique<Rx>(scan.c_str(), true);
    r.filter = std::make_unique<Rx>(filt.c_str());
    r.lastline = std::make_unique<Rx>(last.c_str());
    return r;
  }();
  return e;
}
string collapse_line(string l) {
  // INNODB_RECORD_COLLAPSE (sed -E, first occurrence each)
  RX(c1, "Record in index.*of table.*was not found on update: TUPLE.*at: COMPACT RECORD.*");
  RX(c2, "Record in index.*of table.*was not found on update: TUPLE.*at: RECORD.*");
  RX(c3, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: COMPACT RECORD.*");
  RX(c4, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: RECORD.*");
  l = c1.sub(l, "Record in index X of table Y was not found on update: TUPLE Z at: COMPACT RECORD", false);
  l = c2.sub(l, "Record in index X of table Y was not found on update: TUPLE Z at: RECORD", false);
  l = c3.sub(l, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: COMPACT RECORD", false);
  l = c4.sub(l, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: RECORD", false);
  // UNIVERSAL_COLLAPSE
  RX(u1, " {5,}");
  RX(u2, "0x[0-9A-Fa-f]{12,}");
  l = u1.sub(l, "...", true);
  l = u2.sub(l, "0x...", true);
  return l;
}
// The scrubs below replace the version directory under the builds' root with X, so one UID serves
// every version: /test/13.1/sql/x.cc reads /test/X/sql/x.cc. The root is TEST_DIR, and a Windows
// server prints it the native way: for an MSYS2 TEST_DIR such as /c/test that is C:\test\ or
// C:/test/, either case of the drive letter. A line scrubbed through the native form also has its
// backslashes turned into slashes, so it reads as the Linux line does.
string rx_escape(const string& s) {
  string o;
  for (char c : s) { if (!isalnum((unsigned char)c) && c != '_') o += '\\'; o += c; }
  return o;
}
struct TestRoot {
  std::unique_ptr<Rx> component;   // the root, one directory and a separator: /test/13.1/
  std::unique_ptr<Rx> quoted;      // the same behind a double quote, closed by a separator or a quote
  std::unique_ptr<Rx> win;         // the native Windows root alone; null when TEST_DIR has no drive letter
};
const TestRoot& test_root() {
  static std::map<string, std::unique_ptr<TestRoot>> cache;   // one entry per TEST_DIR seen, never evicted, so a reference stays valid
  static std::mutex mtx;
  string td = g_cfg.test_dir;
  while (td.size() > 1 && td.back() == '/') td.pop_back();
  if (td.empty()) td = "/test";
  std::lock_guard<std::mutex> lk(mtx);
  auto& p = cache[td];
  if (!p) {
    p = std::make_unique<TestRoot>();
    string lin = rx_escape(td) + "/", win;
    if (td.size() > 3 && td[0] == '/' && isalpha((unsigned char)td[1]) && td[2] == '/') {   // /c/test: C:\test\ and C:/test/
      win = string("[") + (char)toupper((unsigned char)td[1]) + (char)tolower((unsigned char)td[1]) + "]:[\\\\/]";
      for (char c : td.substr(3)) win += c == '/' ? string("[\\\\/]") : rx_escape(string(1, c));
      win += "[\\\\/]";
    }
    auto alt = [&](const char* stop, const char* end) {
      string a = "(?:" + lin + "[^/" + stop + "]+[/" + end + "]";
      if (!win.empty()) a += "|" + win + "[^\\\\/" + stop + "]+[\\\\/" + end + "]";
      return a + ")";
    };
    p->component = std::make_unique<Rx>(alt("", "").c_str());
    p->quoted = std::make_unique<Rx>(("\"" + alt("\"", "\"")).c_str());
    if (!win.empty()) p->win = std::make_unique<Rx>(win.c_str());
  }
  return *p;
}
// s/<root>\/<dir>\//<repl>/ over the line; a Windows path comes out with slashes
string scrub_test_root(const string& l, const char* repl, bool global) {
  const TestRoot& tr = test_root();
  bool win = tr.win && tr.win->hit(l);
  string o = tr.component->sub(l, repl, global);
  return win ? replace_all(o, "\\", "/") : o;
}
string uid_normalize_line(string l) {
  RX(ts, "^[0-9]{4}-[0-9]{2}-[0-9]{2}  *[0-9]+:[0-9]+:[0-9]+ +[0-9]+ +");
  RX(tt, "#sql-temptable-[0-9a-f]+-[0-9]+-[0-9a-f]+");
  RX(backup, "#sql-backup-[0-9a-f]+-[0-9]+");
  RX(shm, "/dev/shm/[0-9]+/[0-9]+/");
  RX(ibd, "'/[^']*/test/t[0-9]+\\.ibd'");
  RX(q2, "'[a-zA-Z_][a-zA-Z0-9_]*'\\.'[a-zA-Z_][a-zA-Z0-9_]*'");
  RX(q1, "'[a-zA-Z_][a-zA-Z0-9_]*'");
  RX(b2, "`[a-zA-Z_][a-zA-Z0-9_]*`\\.`[a-zA-Z_][a-zA-Z0-9_]*`");
  RX(b1, "`[a-zA-Z_][a-zA-Z0-9_]*`");
  RX(tref, "'\\./test/[^']+'");
  RX(forpath, "for '[^']*/[^']*'");
  RX(ibdrel, "\\./test/[A-Za-z0-9_#]+\\.ibd");
  RX(port, "127\\.0\\.0\\.1:[0-9]+");
  RX(binlog, "binlog\\.[0-9]+");
  RX(position, "(^| )position [0-9]+");
  RX(endlog, "end_log_pos [0-9]+");
  RX(gtid, "Gtid [0-9]+-[0-9]+-[0-9]+(,[0-9]+-[0-9]+-[0-9]+)*");
  RX(gtidpos, "GTID position '[^']*'");
  RX(failedopen, "'Failed to open [^']+'");
  RX(datalen, "data_len: [0-9]+");
  RX(evtype, "event_type: [0-9]+");
  RX(pagenum, "page number=[0-9]+");
  RX(pageid, "page id: space=[0-9]+");
  RX(indexpage, "Index root page [0-9]+ in ([^ ]+) is corrupted at [0-9]+");
  RX(undopage, "corrupted page [0-9]+ in file [./]*undo[0-9]+");
  RX(aborted, "Aborted connection [0-9]+ ");
  RX(leak, "Indirect leak of [0-9]+ byte\\(s\\) in [0-9]+ object\\(s\\)");
  RX(asanpid, "^==[0-9]+==");
  RX(dblwait, "Long wait \\([0-9]+ seconds\\)");
  RX(datetime, "[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]+)?");
  RX(lrecl, "Table/File lrecl mismatch \\([0-9]+,[0-9]+\\)");
  RX(help1, "We detected index corruption in an InnoDB type table\\..*");
  RX(help2, "The file .* already exists though the corresponding table did not exist in the InnoDB data dictionary\\. You can resolve the problem by removing the file\\.?");
  l = ts.sub(l, "", false);
  l = tt.sub(l, "#sql-temptable-X", true);
  l = backup.sub(l, "#sql-backup-X", true);
  l = shm.sub(l, "/dev/shm/X/N/", true);
  l = ibd.sub(l, "X", true);
  l = q2.sub(l, "'X'", true);
  l = q1.sub(l, "'X'", true);
  l = b2.sub(l, "`X`", true);
  l = b1.sub(l, "`X`", true);
  l = tref.sub(l, "'./test/X'", true);
  l = forpath.sub(l, "for 'X'", true);
  l = ibdrel.sub(l, "./test/X", true);
  l = port.sub(l, "127.0.0.1:X", true);
  l = binlog.sub(l, "binlog.X", true);
  l = position.sub(l, "$1position X", true);
  l = endlog.sub(l, "end_log_pos X", true);
  l = gtid.sub(l, "Gtid X", true);
  l = gtidpos.sub(l, "GTID position 'X'", true);
  l = failedopen.sub(l, "'Failed to open X'", true);
  l = datalen.sub(l, "data_len: X", true);
  l = evtype.sub(l, "event_type: X", true);
  l = pagenum.sub(l, "page number=X", true);
  l = pageid.sub(l, "page id: space=X", true);
  l = indexpage.sub(l, "Index root page N in $1 is corrupted at M", true);
  l = undopage.sub(l, "corrupted page N in file undoX", true);
  l = aborted.sub(l, "Aborted connection N ", true);
  l = leak.sub(l, "Indirect leak of N bytes in M objects", true);
  l = asanpid.sub(l, "==X==", false);
  l = dblwait.sub(l, "Long wait (N seconds)", true);
  l = datetime.sub(l, "YYYY-MM-DD HH:MM:SS", true);
  l = lrecl.sub(l, "Table/File lrecl mismatch (X,Y)", true);
  l = scrub_test_root(l, "/test/X/", true);
  l = help1.sub(l, "We detected index corruption in an InnoDB type table", false);
  l = help2.sub(l, "The file X already exists though the corresponding table did not exist in the InnoDB data dictionary. You can resolve the problem by removing the file", false);
  // the awk renamer: the n-th quoted X becomes X, Y, Z, A, B, ...
  {
    size_t pos = 0;
    int n = 0;
    for (;;) {
      size_t qp = l.find("'X'", pos), bp = l.find("`X`", pos);
      if (qp == string::npos && bp == string::npos) break;
      char c = n < 3 ? (char)('X' + n) : (char)('A' + n - 3);
      size_t p = (qp != string::npos && (bp == string::npos || qp < bp)) ? qp : bp;
      l[p + 1] = c;
      pos = p + 3;
      n++;
    }
  }
  RX(unq1, "`([A-Z])`");
  RX(unq2, "'([A-Z])'");
  l = unq1.sub(l, "$1", true);
  l = unq2.sub(l, "$1", true);
  return l;
}
vector<string> uid_normalize(const vector<string>& lines, bool dedup) {
  vector<string> out;
  std::set<string> seen;
  for (auto& l : lines) {
    string n = uid_normalize_line(l);
    if (dedup && !seen.insert(n).second) continue;
    out.push_back(n);
  }
  return out;
}
string uid_prefix(const string& l) {
  RX(assert_re, "^mariadbd: /test/[^/]+/.*: Assertion .* failed");
  if (assert_re.hit(l)) {
    size_t pos = l.find(": Assertion ");
    if (pos != string::npos) {
      RX(p1, "^mariadbd: /test/[^/]+/");
      RX(p2, ":[0-9]+: .+$");
      RX(t1, "\\.$");
      string prefix = p2.sub(p1.sub(l.substr(0, pos), "", false), "", false);
      string tail = t1.sub("Assertion " + l.substr(pos + 12), "", false);
      return "ASSERT|" + prefix + "|" + tail;
    }
  }
  auto pre = [&](const char* from, const char* to, string* out) {
    if (!starts_with(l, from)) return false;
    *out = string(to) + l.substr(strlen(from));
    return true;
  };
  string o;
  if (pre("[ERROR] InnoDB: ", "INNODB_ERROR|", &o)) return o;
  if (pre("[Warning] InnoDB: ", "INNODB_WARNING|", &o)) {
    RX(rb, "^INNODB_WARNING\\|Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: (COMPACT )?RECORD$");
    if (rb.hit(o)) o = "INNODB_ERROR|" + o.substr(15);
    return o;
  }
  if (pre("[Note] InnoDB: ", "INNODB_NOTE|", &o)) return o;
  RX(crashed, "^\\[ERROR\\] mariadbd: Table .* is marked as crashed");
  if (crashed.hit(l)) return "MARKED_AS_CRASHED|" + l.substr(strlen("[ERROR] mariadbd: "));
  if (pre("[ERROR] mariadbd: ", "MARIADBD_ERROR|", &o)) return o;
  if (pre("[ERROR] mysql_ha_read: ", "MYSQL_HA_READ|", &o)) return o;
  if (pre("[ERROR] Got error ", "GOT_ERROR|Got error ", &o)) return o;
  if (pre("[ERROR] Got an error ", "GOT_ERROR|Got an error ", &o)) return o;
  if (pre("[ERROR] Slave I/O: ", "SLAVE_ERROR|Slave I/O: ", &o)) return o;
  if (pre("[ERROR] Slave SQL: ", "SLAVE_ERROR|Slave SQL: ", &o)) return o;
  if (pre("[ERROR] Slave (additional info): ", "SLAVE_ERROR|Slave (additional info): ", &o)) return o;
  if (pre("[ERROR] Slave: ", "SLAVE_ERROR|Slave: ", &o)) return o;
  if (pre("[ERROR] Master ", "SLAVE_ERROR|Master ", &o)) return o;
  if (pre("[ERROR] Error running query", "SLAVE_ERROR|Error running query", &o)) return o;
  if (pre("[ERROR] Error in Log_event", "SLAVE_ERROR|Error in Log_event", &o)) return o;
  if (pre("[ERROR] Error reading master", "SLAVE_ERROR|Error reading master", &o)) return o;
  if (pre("[ERROR] Error reading packet", "SLAVE_ERROR|Error reading packet", &o)) return o;
  RX(b64, "^\\[ERROR\\][ ]+BINLOG_BASE64_EVENT: ");
  if (b64.hit(l)) return b64.sub(l, "SLAVE_ERROR|BINLOG_BASE64_EVENT: ", false);
  if (pre("[Warning] Slave I/O: ", "SLAVE_WARNING|Slave I/O: ", &o)) return o;
  if (pre("[Warning] Slave SQL: ", "SLAVE_WARNING|Slave SQL: ", &o)) return o;
  if (pre("[Warning] Slave: ", "SLAVE_WARNING|Slave: ", &o)) return o;
  if (pre("[Warning] Aborted connection", "WARNING_ABORTED|Aborted connection", &o)) return o;
  RX(altered, "^\\[Warning\\] Table .* was altered WITHOUT VALIDATION.*$");
  if (altered.hit(l)) return "WARNING|Table X was altered WITHOUT VALIDATION: the table might be corrupted";
  RX(illegal, "^\\[Warning\\] WSREP: Illegal character in variable: .*$");
  if (illegal.hit(l)) return "WSREP_WARNING|Illegal character in variable: X (Y)";
  if (pre("[ERROR] WSREP: ", "WSREP_ERROR|", &o)) return o;
  if (pre("[Warning] WSREP: ", "WSREP_WARNING|", &o)) return o;
  if (pre("[ERROR] RocksDB: ", "ROCKSDB_ERROR|RocksDB: ", &o)) return o;
  if (pre("[ERROR] CHECKTABLE ", "CHECKTABLE|CHECKTABLE ", &o)) return o;
  if (pre("[ERROR] Table ", "MARIADBD_ERROR|Table ", &o)) return o;
  if (starts_with(l, "OpenTable: ")) return "OPENTABLE|" + l;
  if (starts_with(l, "Table/File lrecl mismatch")) return "OPENTABLE|" + l;
  if (starts_with(l, "index_init CONNECT: ")) return "OPENTABLE|" + l;
  if (starts_with(l, "safe_mutex: ")) return "MUTEX_ERROR|" + l;
  if (starts_with(l, "Trying to lock uninitialized mutex")) return "MUTEX_ERROR|" + l;
  if (starts_with(l, "Indirect leak of")) return "LSAN|" + l;
  if (l.find("AddressSanitizer") != string::npos) return "ASAN|" + l;
  if (l.find("MemorySanitizer") != string::npos) return "MSAN|" + l;
  RX(glibc1, "^(corrupted|malloc\\(|free\\(|double free|munmap_chunk)");
  RX(glibc2, "\\*\\*\\* (glibc detected|Error in `)");
  if (glibc1.hit(l) || glibc2.hit(l)) return "GLIBC|" + l;
  return "UNTYPED|Please add a typed prefix rule to error_log_scan.sh uid_prefix() for: " + l;
}
string els_clean_line(string l) {
  RX(ab, "Aborted connection [0-9]+ to db: '[^']*' user: '[^']*' host: '[^']*'");
  l = ab.sub(l, "Aborted connection X to db: 'Y' user: 'Z' host: 'H'", false);
  RX(lead, "^[-0-9: ]*");
  l = lead.sub(l, "", false);
  RX(punct, "[]\\['@/}{#\\\\!$%^&*)(]");
  l = punct.sub(l, ".", true);
  RX(quotes, "[`\"]");
  l = quotes.sub(l, ".", true);
  std::replace(l.begin(), l.end(), '-', '.');
  RX(proc, "PROCEDURE [^ ]+ ");
  l = proc.sub(l, "PROCEDURE.*", false);
  RX(colon, ":[0-9][0-9]+\\.");
  l = colon.sub(l, ".*", false);
  RXI(binlog1, "binlog\\.0.*end_log_pos.*gtid.*Internal MariaDB error");
  l = binlog1.sub(l, "binlog.*end_log_pos.*gtid.*Internal MariaDB error", false);
  RXI(gtid, "(gtid) [.0-9 ]+");
  l = gtid.sub(l, "$1.*", false);
  RX(binlog2, "binlog.[0-9]+\\. at [0-9]+");
  l = binlog2.sub(l, "binlog.*", true);
  RX(at, " at [0-9]+");
  l = at.sub(l, ".*", true);
  RX(d1, "\\.\\*[.]+");
  l = d1.sub(l, ".*", true);
  RX(d2, "[.]+\\.\\*");
  l = d2.sub(l, ".*", true);
  RX(d3, "^[.]+");
  l = d3.sub(l, "", false);
  RX(d4, "[.]+$");
  l = d4.sub(l, "", false);
  RX(t1, "\\.test\\.1[^ $]+");
  l = t1.sub(l, ".*", false);
  RX(t2, "\\.dev\\.shm\\.[^ $]+");
  l = t2.sub(l, ".*", false);
  RX(line, "line [0-9]+");
  l = line.sub(l, "line ", false);
  RX(trail, "[. ]+$");
  l = trail.sub(l, "", false);
  RX(dd, "\\.\\.\\*");
  l = dd.sub(l, ".*", false);
  RX(lock, "for table.*Lock");
  l = lock.sub(l, "for table.*Lock", false);
  RX(mac, "ERROR. mariadbd: Table .* is marked as crashed and should be repaired");
  l = mac.sub(l, "ERROR. mariadbd: Table .* is marked as crashed and should be repaired", false);
  return l;
}
}  // namespace

// error_log_scan.sh <mode> <log>...: false where the script exits 1 (nothing found or no logs)
bool els_run(const string& mode, const vector<string>& logs_in, bool exclude_assert, string& out, string* err) {
  out.clear();
  const ElsLists& L = els_lists();
  if (!L.err.empty()) { if (err) *err = L.err; return false; }
  vector<string> logs;
  for (auto& l : logs_in) if (file_exists(l)) logs.push_back(l);
  if (logs.empty()) return false;
  vector<string> given = logs;                                 // capped_logs keeps the order, one out per log in
  logs = capped_logs(logs);
  RX(blank_or_head, "^[ \t]*$|^==>");
  if (mode == "aggregate") {
    // <UID><tab><trial> rows for pquery-results.sh, deduped per trial and UID. The trial is the first
    // path part of a log given as ./<trial>/...; a log given any other way gives no row
    vector<std::pair<string, string>> cand;                  // the log as given, one of its lines
    {
      // grep -H | sort -u: path:line sorted as one string. A capped copy is named from /tmp there,
      // and '/' sorts after the '.' of ./, so its rows come after those of the logs read in place
      vector<std::pair<string, std::pair<string, string>>> hits;
      for (size_t i = 0; i < logs.size(); i++)
        for (auto& l : grep_lines(read_file(logs[i])))
          if (L.scan->hit(l)) hits.push_back({(logs[i] != given[i] ? "/" : "") + given[i] + ":" + l, {given[i], l}});
      std::sort(hits.begin(), hits.end());
      hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
      for (auto& h : hits) cand.push_back(h.second);
    }
    for (size_t i = 0; i < logs.size(); i++) {                 // then the last line of each log, when it is the kind LASTLINE names
      vector<string> v = grep_lines(read_file(logs[i]));
      if (!v.empty() && !v.back().empty() && L.lastline->hit(v.back())) cand.push_back({given[i], v.back()});
    }
    RX(trial_path, "^\\./[0-9]+/");
    vector<string> trials, content;
    for (auto& c : cand) {
      if (L.filter->hit(c.second) || blank_or_head.hit(c.second) || !trial_path.hit(c.first)) continue;
      trials.push_back(c.first.substr(2, c.first.find('/', 2) - 2));
      content.push_back(collapse_line(c.second));
    }
    vector<string> uids = uid_normalize(content, false), o;
    std::set<std::pair<string, string>> seen;
    for (size_t i = 0; i < uids.size(); i++) {
      string u = uid_prefix(uids[i]);
      if (seen.insert({trials[i], u}).second) o.push_back(u + "\t" + trials[i]);
    }
    out = join_nl(o);
    return true;
  }
  vector<string> errors, last;
  {
    vector<string> hits;
    for (auto& lg : logs) for (auto& l : grep_lines(read_file(lg))) if (L.scan->hit(l)) hits.push_back(l);
    std::sort(hits.begin(), hits.end());                       // sort -u under LANG=C.UTF-8: byte order
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    for (auto& l : hits) if (!L.filter->hit(l) && !blank_or_head.hit(l)) errors.push_back(collapse_line(l));
    for (auto& lg : logs) {
      string text = read_file(lg);
      vector<string> v = grep_lines(text);
      if (v.empty()) continue;
      const string& tl = v.back();
      if (L.lastline->hit(tl) && !L.filter->hit(tl) && !blank_or_head.hit(tl)) last.push_back(collapse_line(tl));
    }
  }
  auto finish = [&](const vector<string>& lines) {
    vector<string> o;
    for (auto& l : uid_normalize(lines, true)) o.push_back(uid_prefix(l));
    return join_nl(o);
  };
  if (mode == "errors") { if (errors.empty()) return false; out = finish(errors); return true; }
  if (mode == "lastline") { if (last.empty()) return false; out = finish(last); return true; }
  if (errors.empty() && last.empty()) return false;
  if (mode == "check") return true;
  vector<string> all = errors;
  for (auto& l : last) all.push_back(l);
  if (mode == "top") {
    string best;
    int bp = 0;
    RX(tier1, "^ASSERT\\|");
    RX(tier2, "^(ASAN|LSAN|MSAN)\\|");
    RX(tier3, "^(GLIBC|MUTEX_ERROR)\\|");
    RX(warn, "^(INNODB_WARNING|SLAVE_WARNING|WARNING_ABORTED|WARNING|INNODB_NOTE)\\|");
    RX(warn_or_untyped, "^(INNODB_WARNING|SLAVE_WARNING|WARNING_ABORTED|WARNING|INNODB_NOTE|UNTYPED)\\|");
    RX(untyped, "^UNTYPED\\|");
    for (auto& l : uid_normalize(all, true)) {
      string u = uid_prefix(l);
      if (exclude_assert && tier1.hit(u)) continue;
      int p = 9;
      if (tier1.hit(u)) p = 1;
      else if (tier2.hit(u)) p = 2;
      else if (tier3.hit(u)) p = 3;
      else if (u.find('|') != string::npos && !warn_or_untyped.hit(u)) p = 4;
      else if (warn.hit(u)) p = 5;
      else if (untyped.hit(u)) p = 6;
      if (!bp || p < bp) { best = u; bp = p; }
    }
    out = best;
    return true;
  }
  if (mode == "clean") {
    vector<string> o;
    std::set<string> seen;
    for (auto& l : all) { string c = els_clean_line(l); if (seen.insert(c).second) o.push_back(c); }
    out = join_nl(o);
    return true;
  }
  if (err) *err = "Usage: error_log_scan.sh {errors|lastline|top|check|clean|aggregate} <log>...";
  return false;
}

// ---------------------------------------------------------------------------------------------
// san_text_string.sh
// ---------------------------------------------------------------------------------------------
namespace {
string asan_frame(const string& line) {
  RX(a, "^[^i]+in[ ]+");
  RX(b, "\\(.*\\)");
  RX(c, "[ ]+.*");
  return c.sub(b.sub(a.sub(line, "", false), "", true), "", false);
}
string tsan_frame(const string& line) {
  RX(a, "\\(.*\\)");
  RX(b, ",*#[0-9]+ ");
  RX(c, " ");
  RX(d, "[.]+/.*");
  RX(e, "/.*");
  RX(f, "<.*");
  string s = a.sub(line, "", true);
  s = b.sub(s, "", false);
  s = c.sub(s, "", true);
  s = d.sub(s, "", false);
  s = e.sub(s, "", false);
  s = f.sub(s, "", false);
  return s;
}
string asan_preparse(const string& line) {
  RX(last_tok, ".* ([^ ]+)$");
  RX(lc2, ":[0-9]+:[0-9]+$");
  RX(lc1, ":[0-9]+$");
  RX(so, "\\.so\\+0x[)0-9a-f]+");
  RX(asan_rt, ".*asan/asan_");
  RX(paren, "^\\(.*\\)$");
  string s = strip_buildid(line);
  s = last_tok.sub(s, "$1", false);
  s = lc2.sub(s, "", false);
  s = lc1.sub(s, "", false);
  s = strip_source_prefix(s, nullptr);
  s = cxx_version_rule(s);
  s = so.sub(s, ".so", true);
  s = asan_rt.sub(s, "asan_", true);
  if (paren.hit(s)) s.clear();
  return s;
}
string tsan_preparse(const string& line) {
  RX(lastcolon, ":[^:]*$");
  RX(lc, ":[0-9]+:[0-9]+:[ ]*$");
  RX(paren, "^\\(.*\\)$");
  string s = strip_buildid(line);
  s = lastcolon.sub(s, "", false);
  s = lc.sub(s, "", false);
  s = strip_source_prefix(s, "tsan");
  s = cxx_version_rule(s);
  if (paren.hit(s) || starts_with(s, "tsan/")) s.clear();
  return s;
}
string msan_preparse(const string& line) {
  RX(lastcolon, ":[^:]*$");
  RX(lc, ":[0-9]+:[0-9]+:[ ]*$");
  RX(lc1, ":[0-9]+$");
  RX(frame_no, "#[0-9]+[ ]+0x[a-f0-9]+");
  RX(in, "[ ]+in[ ]+");
  RX(cfile, "[^ ]+[ ]+([^ ]+\\.[c]+)");
  string s = strip_buildid(line);
  s = lastcolon.sub(s, "", false);
  s = lc.sub(s, "", false);
  s = lc1.sub(s, "", false);
  s = strip_source_prefix(s, "msan");
  s = cxx_version_rule(s);
  s = frame_no.sub(s, "", false);
  s = in.sub(s, "", false);
  s = cfile.sub(s, "$1", false);
  if ((s.find("+0x") != string::npos && ends_with(s, ")")) || starts_with(s, "msan/")) s.clear();
  return s;
}
string ubsan_preparse(const string& line) {
  RX(rt, " runtime error:.*");
  RX(lc, ":[0-9]+:[0-9]+:[ ]*$");
  string s = strip_buildid(line);
  s = rt.sub(s, "", false);
  s = lc.sub(s, "", false);
  s = strip_source_prefix(s, nullptr);
  s = cxx_version_rule(s);
  return s;
}
string asan_error(const string& line) {
  RX(e1, ".*ERROR:[ ]*");
  RX(e2, ".*AddressSanitizer:[ ]*");
  RX(e3, " on address.*");
  RX(e4, "thread T[0-9]+");
  RX(e5, "allocation size 0x[0-9a-f]+");
  RX(e6, "memory ranges .0x.* overlap");
  RX(e7, "\\(0x[0-9a-f]+ after adjustment");
  RX(e8, "supported size of 0x[0-9a-f]+");
  string s = e1.sub(line, "", false);
  s = e2.sub(s, "", false);
  s = e3.sub(s, "", false);
  s = e4.sub(s, "thread Tx", true);
  s = e5.sub(s, "allocation size X", true);
  s = e6.sub(s, "memory ranges X and Y overlap", true);
  s = e7.sub(s, "(Y after adjustment", true);
  s = e8.sub(s, "supported size of Z", true);
  return s;
}
string ubsan_error(const string& line) {
  RX(e0, ".*runtime error:[ ]*");
  RX(e1, "[-\\.\\+0-9e]+ is outside the range");
  RX(e2, "load of value (-*)[0-9]+");
  RX(e3, "negation of ([-]*)[0-9]+");
  RX(e4, "applying non-zero offset ([-+]*)[0-9]+");
  RX(e5, "overflow: (-*)[0-9]+ ([-+:\\\\*]) (-*)[0-9]+ ");
  RX(e6, "shift exponent ([-+]*)[0-9]+");
  RX(e7, "index (-*)[0-9]+ out of bounds");
  RX(e8, " address 0x[^ ]+");
  RX(e9, "with base 0x[0-9a-f]+");
  RX(e10, "overflowed to 0x[0-9a-f]+");
  RX(e11, " offset to 0x[0-9a-f]+");
  string s = e0.sub(line, "", false);
  s = e1.sub(s, "X is outside the range", false);
  s = e2.sub(s, "load of value $1X", true);
  s = e3.sub(s, "negation of $1X", true);
  s = e4.sub(s, "applying non-zero offset $1X", true);
  s = e5.sub(s, "overflow: $1X $2 $3Y ", true);
  s = e6.sub(s, "shift exponent $1X", true);
  s = e7.sub(s, "index $1X out of bounds", true);
  s = e8.sub(s, " address X", true);
  s = e9.sub(s, "with base X", true);
  s = e10.sub(s, "overflowed to Y", true);
  s = e11.sub(s, " offset to X", true);
  return s;
}
string tsan_error(const string& line) {
  RX(w, ".*WARNING:");
  RX(t, ".*ThreadSanitizer:[ ]*");
  RX(p, " \\(pid=.*");
  RXI(hex, "0x[0-9a-f]+");
  RXI(thr, "thread T[0-9]+");
  string s = w.sub(line, "", false);
  s = t.sub(s, "", false);
  s = p.sub(s, "", false);
  s = hex.sub(s, "X", true);
  s = thr.sub(s, "thread Tx", true);
  return s;
}
string msan_error(const string& line) {
  RX(w, ".*WARNING:");
  RX(m, ".*MemorySanitizer:[ ]*");
  RX(p, " \\(pid=.*");
  return p.sub(m.sub(w.sub(line, "", false), "", false), "", false);
}
}  // namespace

// san_text_string.sh over the logs (already capped by the caller). "" when no complete report is
// in them; err carries the script's Assert text where it would exit 1.
string uid_san(const vector<string>& logs, string* err) {
  if (err) err->clear();
  vector<string> texts;
  size_t total_lines = 0;
  for (auto& lg : logs) { texts.push_back(read_file(lg)); total_lines += newline_count(texts.back()); }
  string all;
  for (auto& t : texts) all += t;
  if (total_lines == 0) { if (err) *err = "Assert: the error log at " + join(logs, " ") + " contains 0 lines."; return ""; }
  if (total_lines < 3) { if (err) *err = "Assert: the error log at " + join(logs, " ") + " contains less then 3 lines."; return ""; }
  bool asan_present = icontains(all, "=ERROR:") || icontains(all, "LeakSanitizer:");
  bool tsan_present = icontains(all, "ThreadSanitizer:");
  bool ubsan_present = icontains(all, "runtime error:");
  bool msan_present = icontains(all, "MemorySanitizer:");
  if (!asan_present && !tsan_present && !ubsan_present && !msan_present) return "";
  bool started = !icontains(all, "ready for connections");
  size_t counter = 0;
  bool a_prog = false, t_prog = false, u_prog = false, m_prog = false;
  bool a_ready = false, t_ready = false, u_ready = false, m_ready = false;
  auto flag_ready_check = [&] { a_ready = a_prog; t_ready = t_prog; u_ready = u_prog; m_ready = m_prog; };
  for (auto& text : texts) {
    a_prog = t_prog = u_prog = m_prog = false;
    string af[4], uf[4], tf[4], mf[4], a_pre, u_pre, t_pre, m_pre, a_err, u_err, t_err, m_err;
    for (auto line : read_lines(text)) {
      counter++;
      line = replace_all(line, "(<unknown module>)", "<unknown_module>");
      if (!started) {
        if (line.find("ready for connections") != string::npos) started = true;
        continue;
      }
      if (asan_present) {
        if (line.find("AddressSanitizer:") != string::npos || line.find("LeakSanitizer:") != string::npos) {
          flag_ready_check();
          a_prog = true; t_prog = u_prog = m_prog = false;
          for (auto& f : af) f.clear();
          a_pre.clear();
          a_err = asan_error(line);
        }
        if (a_prog) {
          for (int k = 0; k < 4; k++) {
            if (line.find(fmt(" #%d 0x", k)) != string::npos) {
              af[k] = asan_frame(line);
              if (a_pre.empty()) a_pre = asan_preparse(line);
              if (k == 3) a_ready = true;
            }
          }
        }
      }
      if (ubsan_present) {
        if (line.find("runtime error:") != string::npos) {
          flag_ready_check();
          u_prog = true; a_prog = t_prog = m_prog = false;
          for (auto& f : uf) f.clear();
          u_pre = ubsan_preparse(line);
          u_err = ubsan_error(line);
        }
        if (u_prog) {
          for (int k = 0; k < 4; k++) {
            if (line.find(fmt(" #%d 0x", k)) != string::npos) {
              uf[k] = asan_frame(line);
              if (k == 3) u_ready = true;
            }
          }
        }
      }
      if (tsan_present) {
        if (line.find("ThreadSanitizer:") != string::npos) {
          flag_ready_check();
          t_prog = true; a_prog = u_prog = m_prog = false;
          for (auto& f : tf) f.clear();
          t_pre.clear();
          t_err = tsan_error(line);
        }
        if (t_prog) {
          for (int k = 0; k < 4; k++) {
            if (line.find(fmt(" #%d ", k)) != string::npos) {
              tf[k] = tsan_frame(line);
              if (t_pre.empty()) t_pre = tsan_preparse(line);
              if (k == 3) t_ready = true;
            }
          }
        }
      }
      if (msan_present) {
        if (line.find("MemorySanitizer:") != string::npos) {
          flag_ready_check();
          m_prog = true; a_prog = t_prog = u_prog = false;
          for (auto& f : mf) f.clear();
          m_pre.clear();
          m_err = msan_error(line);
        }
        if (m_prog) {
          RX(frame_line, " #[0-9] | #[0-9][0-9] ");
          if (frame_line.hit(line)) {
            RX(in_fn, "[ ]+in[ ]+[^ \\\\()]+");
            RX(in_pre, "[ ]+in[ ]+");
            string cur = in_pre.sub(in_fn.first(line), "", false);
            if (!cur.empty()) {
              if (mf[0].empty()) mf[0] = cur;
              else if (mf[1].empty()) mf[1] = cur;
              else if (mf[2].empty()) mf[2] = cur;
              else if (mf[3].empty()) { mf[3] = cur; m_ready = true; }
            }
            if (m_pre.empty()) m_pre = msan_preparse(line);
          }
        }
      }
      if (counter == total_lines) flag_ready_check();
      auto assemble = [](const char* tag, const string& e, const string& pre, const string* f) {
        string u = tag;
        if (!e.empty()) u += "|" + e;
        if (!pre.empty()) u += "|" + pre;
        for (int k = 0; k < 4; k++) if (!f[k].empty()) u += "|" + f[k];
        return u;
      };
      if (asan_present && a_ready) {
        string u = assemble("ASAN", a_err, a_pre, af);
        size_t p = u.find("ASAN|LeakSanitizer: detected memory leaks");
        if (p != string::npos) u.replace(p, strlen("ASAN|LeakSanitizer: detected memory leaks"), "LSAN|memory leak");
        if (u == "LSAN|memory leak|<unknown_module>|operator") {
          if (grep_any_text(logs, "dlopen") && grep_any_text(logs, "plugin_dl_add")) {
            if (grep_any_text(logs, "plugin_dl_foreach")) u = "LSAN|memory leak|sql/sql_plugin.cc|operator new|dlopen|plugin_dl_add|plugin_dl_foreach";
            else if (grep_any_text(logs, "dl_open_worker")) u = "LSAN|memory leak|sql/sql_plugin.cc|operator new|dl_open_worker|dlopen|plugin_dl_add";
          }
        }
        if (u == "LSAN|memory leak|<unknown_module>|calloc") {
          if (grep_any_text(logs, "handler::ha_rnd_init(bool)") && grep_any_text(logs, "handler::ha_rnd_init_with_error(bool)") && grep_any_text(logs, "init_read_record"))
            u = "LSAN|memory leak|<unknown_module>|calloc|handler::ha_rnd_init(bool)|handler::ha_rnd_init_with_error(bool)|init_read_record";
        }
        if (u == "LSAN|memory leak|<unknown_module>|malloc") {
          RXI(r1, "Sql_cmd_update::update_single_table.*sql_update.cc");
          RXI(r2, "Sql_cmd_update::execute_inner.*sql_update.cc");
          RXI(r3, "Sql_cmd_dml::execute.*sql_select.cc");
          if (grep_any(logs, r1) && grep_any(logs, r2) && grep_any(logs, r3))
            u = "LSAN|memory leak|<unknown_module>|malloc|Sql_cmd_update::update_single_table|Sql_cmd_update::execute_inner|Sql_cmd_dml::execute";
        }
        return u;
      }
      if (ubsan_present && u_ready) return assemble("UBSAN", u_err, u_pre, uf);
      if (tsan_present && t_ready) return assemble("TSAN", t_err, t_pre, tf);
      if (msan_present && m_ready) return assemble("MSAN", m_err, m_pre, mf);
    }
    flag_ready_check();
  }
  return "";
}

// ---------------------------------------------------------------------------------------------
// fallback_text_string.sh
// ---------------------------------------------------------------------------------------------
namespace {
bool poor_string(const string& s) {
  return s.empty() || s == "my_print_stacktrace" || s == "my_print_stacktrace.unsigned" || s == "0" || s == "NULL" || s == "start" || s == "ut_dbg_assertion_failed";
}
string fts_dots(string s) {            // s/|/./g;s/\&/./g;s/:/./g;s|"|.|g;s|\!|.|g;s|&|.|g;s|\*|.|g;s|\]|.|g;s|\[|.|g;s|)|.|g;s|(|.|g
  for (char& c : s) if (c == '|' || c == '&' || c == ':' || c == '"' || c == '!' || c == '*' || c == ']' || c == '[' || c == ')' || c == '(') c = '.';
  return s;
}
string collapse_ws(const string& s) { return join(split_ws(s), " "); }   // an unquoted echo ${STRING}
}  // namespace

// fallback_text_string.sh <log>: "FALLBACK|<string>" or "" (err = the script's stderr text)
string uid_fallback(const string& log_in, string* err) {
  if (err) err->clear();
  if (!file_exists(log_in)) { if (err) *err = "Assert: " + log_in + " does not exist or could not be read by fallback_text_string.sh"; return ""; }
  string log = capped_logs({log_in})[0];
  vector<string> lines = grep_lines(read_file(log));
  vector<string> pieces;
  {
    RX(as1, "Assertion.*failed");
    RX(as_zero, "Assertion .0. failed");
    RX(s1, "^.*Assertion .");
    RX(s2, ". failed.*$");
    for (auto& l : lines) {
      if (!as1.hit(l) || as_zero.hit(l)) continue;
      string s = l;
      for (char& c : s) if (c == '|' || c == '&' || c == '"' || c == ':') c = '.';
      s = s1.sub(s, "", false);
      s = s2.sub(s, "", false);
      s = replace_all(s, " ", "DUMMY");
      pieces.push_back(s);
    }
    RX(g2, "libgalera_smm\\.so\\(_|mysqld\\(_|ha_rocksdb.so\\(_|ha_tokudb.so\\(_");
    for (auto& l : lines) if (g2.hit(l)) pieces.push_back(l);
    RX(g3, "libgalera_smm\\.so\\(|mysqld\\(|ha_rocksdb.so\\(|ha_tokudb.so\\(");
    RX(g3x, "mysqld\\(_|ha_rocksdb.so\\(_|ha_tokudb.so\\(_");
    for (auto& l : lines) if (g3.hit(l) && !g3x.hit(l)) pieces.push_back(l);
    RXI(g4, "Assertion failure.*in file.*line");
    RX(f1, ".*in file ");
    RX(f2, ".*/10.[1-9][^/]*/");
    for (auto& l : lines) {
      if (!g4.hit(l)) continue;
      string s = f2.sub(f1.sub(l, "", false), "", false);
      pieces.push_back(replace_all(s, " ", "DUMMY"));
    }
  }
  string joined = join_nl(pieces);
  string text;
  {
    string tokens_src = joined;
    std::replace(tokens_src.begin(), tokens_src.end(), ' ', '\n');
    RX(r1, ".*libgalera_smm\\.so[\\\\(_]*");
    RX(r2, ".*mysqld[\\\\(_]*");
    RX(r3, ".*ha_rocksdb.so[\\\\(_]*");
    RX(r4, ".*ha_tokudb.so[\\\\(_]*");
    RX(r5, "\\).*");
    RX(r6, "\\+.*$");
    RX(r7, "\\($");
    RX(blank, "^[ \t]*$");
    RX(ltrim, "^[ \t]+");
    RX(rtrim, "[ \t]+$");
    for (auto& tok : grep_lines(tokens_src)) {
      string s = r1.sub(tok, "", false);
      s = r2.sub(s, "", false);
      s = r3.sub(s, "", false);
      s = r4.sub(s, "", false);
      s = r5.sub(s, "", false);
      s = r6.sub(s, "", false);
      s = replace_all(s, "DUMMY", " ");
      s = r7.sub(s, "", false);
      s = fts_dots(s);
      if (blank.hit(s)) continue;
      text = rtrim.sub(ltrim.sub(s, "", false), "", false);
      break;
    }
  }
  if (poor_string(text)) {
    RX(af, "Assertion failure:");
    RX(af1, ".*Assertion failure:[ \t]+");
    RX(af2, "[ \t]\\+$");
    RX(af3, ".*c:[0-9]+:");
    string last_af;
    for (auto& l : lines) if (af.hit(l)) last_af = l;
    string cand;
    if (!last_af.empty()) cand = fts_dots(af3.sub(af2.sub(af1.sub(last_af, "", false), "", false), "", false));
    if (!poor_string(cand)) text = cand;
    if (poor_string(text)) {
      RX(fr, "libgalera_smm\\.so\\(.*|mysqld\\(.*|ha_rocksdb.so\\(.*|ha_tokudb.so\\(.*");
      RX(fr1, "[^(]+\\(");
      RX(fr2, "\\).*");
      RX(fr3, "\\(.*");
      RX(fr4, "\\+0x.*");
      RX(frx, "my_print_stacktrace|handle.*signal|^[ \t]*$");
      cand.clear();
      for (auto& l : lines) {
        if (!fr.hit(l)) continue;
        bool found = false;
        size_t from = 0, b, e;
        while (fr.find(l, from, &b, &e)) {
          if (e == b) { from = e + 1; continue; }
          string s = fr4.sub(fr3.sub(fr2.sub(fr1.sub(l.substr(b, e - b), "", false), "", false), "", false), "", false);
          if (!frx.hit(s)) { cand = fts_dots(s); found = true; break; }
          from = e;
        }
        if (found) break;
      }
      if (!poor_string(cand)) text = cand;
      if (poor_string(text) && !last_af.empty()) {
        cand = fts_dots(af2.sub(af1.sub(last_af, "", false), "", false));
        if (!poor_string(cand)) text = cand;
      }
    }
  }
  RX(tell, "info->end_of_file == inline_mysql_file_tell.*");
  text = tell.sub(collapse_ws(text), "info->end_of_file == inline_mysql_file_tel", false);
  string logtext = read_file(log);
  RX(ball, "MYSQL_BIN_LOG..rollback.THD.. bool.. Assertion ..all");
  if (text == ".all" && ball.hit(logtext)) text = "MYSQL_BIN_LOG..rollback.THD.. bool.. Assertion ..all";
  if (text == ".error") {
    RX(e1, "virtual uint dd::Dictionary_impl::get_actual_P_S_version\\(THD.\\): Assertion ..error' failed");
    RX(e2, "InnoDB: Assertion failure: dict0dd.cc:5.....error$");
    if (e1.hit(logtext)) text = "uint dd..Dictionary_impl..get_actual_P_S_version.THD... Assertion ..error. failed";
    for (auto& l : lines) if (e2.hit(l)) { text = "dict0dd.cc:5.....error"; break; }
  }
  if (text == "dd_table_discard_tablespace" && logtext.find("Cannot find a free slot for an undo log") != string::npos && logtext.find("InnoDB: Assertion failure: dict0dd.cc:") != string::npos)
    text = "RSEG.....dd_table_discard_tablespace";
  if (text == "strcmp.table->name.m_name, table_name. == 0" && logtext.find("Cannot find a free slot for an undo log") != string::npos && logtext.find("InnoDB: Assertion failure: dict0dd.cc:") != string::npos)
    text = "RSEG.....strcmp.table->name.m_name, table_name. == 0";
  if (text == "status.ok") {
    RX(ro, "rocksdb::Status rocksdb::BlockCacheTier::Open.*status.ok");
    if (ro.hit(logtext)) text = "rocksdb::Status rocksdb::BlockCacheTier::Open status.ok";
  }
  RX(thr, " thread [0-9]{15}");
  text = thr.sub(collapse_ws(text), "", false);
  RX(sda, "/sda/[PM]S[0-9]+[^ ]+/bin/mysqld");
  text = sda.sub(text, "", true);
  if (text.empty()) { if (err) *err = "No relevant strings were found in " + log_in + " by fallback_text_string.sh"; return ""; }
  return "FALLBACK|" + text;
}

// ---------------------------------------------------------------------------------------------
// new_text_string.sh: find_other_possible_issue_strings (the no-core error-log tiers)
// ---------------------------------------------------------------------------------------------
namespace {
string strip_quotes(string s) { s.erase(std::remove(s.begin(), s.end(), '"'), s.end()); s.erase(std::remove(s.begin(), s.end(), '\''), s.end()); return s; }
string got_error_common(string s) {
  RXI(g1, "Got error ([0-9]+)[ ]*");
  RX(g2, "/dev/shm/[^ ]*sql-temptable[^ ]*MAI");
  RX(g3, "/(data|test)/[^ ]*sql-temptable[^ ]*MAI");
  RX(g4, "#sql-temptable-[0-9a-f-]+");
  RX(g5, "the event.s master log [^,]*, end_log_pos [0-9]+");
  RX(g6, "Gtid [0-9]+-[0-9]+-[0-9]+");
  s = g1.sub(s, "Got error $1|", false);
  s = g2.sub(s, "X/sql-temptable-Y.MAI", false);
  s = g3.sub(s, "X/sql-temptable-Y.MAI", false);
  s = g4.sub(s, "#sql-temptable-X", true);
  s = g5.sub(s, "the event's master log X, end_log_pos Y", false);
  s = g6.sub(s, "Gtid Z", true);
  return s;
}
}  // namespace

string uid_other_strings(const vector<string>& logs) {
  {
    RXI(m1, "safe_mutex: Found wrong usage of mutex .* and .*");
    string s = grep_first(logs, m1, true);
    if (!s.empty()) return "MUTEX_ERROR|" + strip_quotes(s);
    RXI(m2, "safe_mutex: .*");
    s = grep_first(logs, m2, true);
    if (!s.empty()) {
      RX(l1, ", line.*");
      return "MUTEX_ERROR|" + scrub_test_root(l1.sub(s, "", false), "", false);
    }
  }
  {
    RX(glibc, "^(corrupted|malloc\\(|free\\(|double free|munmap_chunk).*|\\*\\*\\* (glibc detected|Error in `).*");
    string s = grep_first(logs, glibc, true);
    if (!s.empty()) return "GLIBC|" + s;
  }
  {
    RXI(ot, "OpenTable:.*");
    string s = grep_first(logs, ot, true);
    if (!s.empty()) {
      string t = "OPENTABLE|" + strip_quotes(s);
      RX(o1, "Open\\(r\\+b\\) error ([0-9]+) on [^:]+:");
      RX(o2, "Invalid flag [0-9]+ for column [a-zA-Z_][a-zA-Z0-9_]*");
      RX(o3, "Table/File lrecl mismatch \\([0-9]+,[0-9]+\\)");
      t = o1.sub(t, "Open(r+b) error $1 on X:", false);
      t = o2.sub(t, "Invalid flag X for column Y", false);
      t = o3.sub(t, "Table/File lrecl mismatch (X,Y)", false);
      return t;
    }
  }
  {
    RXI(gf, "Got fatal error [0-9]+");
    string s = grep_first(logs, gf, true);
    if (!s.empty()) return "GOT_FATAL_ERROR|" + s;
  }
  {
    RXI(mn, "Warning: Memory not freed");
    string s = grep_first(logs, mn, false);
    if (!s.empty()) {
      RX(n1, ": [0-9]+");
      return "GENERIC_ISSUE-DO_NOT_ADD_TO_KB_OR_KBA|MEMORY_NOT_FREED|" + n1.sub(s, "", false);
    }
  }
  {
    RXI(ge1, "m(ariadb|ysql)d: Got error[^\"]+\"[^\"]+\"");
    string s = grep_first(logs, ge1, true);
    if (!s.empty()) {
      RXI(ge1b, "Got error [0-9]+[^\\\\.]+");
      string t = ge1b.first(strip_quotes(s));
      if (!t.empty()) return "GOT_ERROR|" + got_error_common(t);
    }
    RXI(ge2, "Got error.*");
    s = grep_first(logs, ge2, true);
    if (!s.empty()) {
      RX(a1, "Got error '([0-9]+) \"[^\"]*\"' for '[^']*#sql-temptable[^']*'");
      RX(a2, "Got error '([0-9]+) \"[^\"]*\"' for '[^']*'");
      RX(a3, "when reading table '[^']*'");
      string t = a1.sub(s, "Got error $1 when reading table (temptable)", false);
      t = a2.sub(t, "Got error $1 when reading table X", false);
      t = a3.sub(t, "when reading table X", false);
      t = "GOT_ERROR|" + got_error_common(t);
      RX(mac, "marked as crashed and should be repaired\"' for .*");
      return mac.sub(t, "marked as crashed and should be repaired\" for X", false);
    }
  }
  {
    RXI(mc, "m(ariadb|ysql)d: Table.*is marked as crashed and should be repaired");
    string s = grep_first(logs, mc, true);
    if (!s.empty()) {
      RX(c1, "^mariadbd: ");
      RX(c2, "^mysqld: ");
      RX(c3, "Table /[^#]+#sql-temptable[^ ]+ ");
      RX(c4, "Table t[0-9]+ is marked as crashed");
      string t = c3.sub(c2.sub(c1.sub(strip_quotes(s), "", false), "", false), "Table sql-temptable-X ", false);
      return c4.sub("MARKED_AS_CRASHED|" + t, "Table X is marked as crashed", true);
    }
  }
  {
    RXI(wr, "\\[Warning\\] InnoDB: Record in index.*was not found on (update|rollback, trying to insert): TUPLE.*at: (COMPACT )?RECORD.*");
    string s = grep_first(logs, wr, true);
    if (!s.empty()) {
      RX(w0, ".*\\[Warning\\] InnoDB: ");
      string t = w0.sub(strip_quotes(s), "", false);
      RX(r1, "Record in index.*of table.*was not found on update: TUPLE.*at: COMPACT RECORD.*");
      RX(r2, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: COMPACT RECORD.*");
      RX(r3, "Record in index.*of table.*was not found on update: TUPLE.*at: RECORD\\(.*");
      RX(r4, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: RECORD\\(.*");
      t = r1.sub(t, "Record in index X of table Y was not found on update: TUPLE Z at: COMPACT RECORD", false);
      t = r2.sub(t, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: COMPACT RECORD", false);
      t = r3.sub(t, "Record in index X of table Y was not found on update: TUPLE Z at: RECORD", false);
      t = r4.sub(t, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: RECORD", false);
      return "INNODB_ERROR|" + t;
    }
  }
  {
    RXI(ie, "ERROR\\] InnoDB.*");
    string s = grep_first(logs, ie, true);
    if (!s.empty()) {
      s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
      RX(i0, "ERROR\\] InnoDB[: ]*");
      RX(i1, "table.*index.*stat[^:]+");
      RX(i2, "User stopword table.*does not exist");
      RX(i3, "\\.$");
      string t = i3.sub(i2.sub(i1.sub(i0.sub(s, "", false), "table X index Y stat Z", false), "User stopword table X does not exist", false), "", false);
      t = "INNODB_ERROR|" + t;
      RX(n1, "Cannot rename.*to.*because the target schema directory doesnt exist");
      RX(n2, "Record in index.*of table.*was not found on update: TUPLE.*at: COMPACT RECORD.*");
      RX(n3, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: COMPACT RECORD.*");
      RX(n4, "Record in index.*of table.*was not found on update: TUPLE.*at: RECORD\\(.*");
      RX(n5, "Record in index.*of table.*was not found on rollback, trying to insert: TUPLE.*at: RECORD\\(.*");
      RX(n6, "In ALTER TABLE [^ ]+ has or is referenced in foreign key constraints which are not compatible with the new table definition");
      RX(n7, "Unable to flag corruption of `[^`]+` in table `[^`]+`\\.`[^`]+` in CHECK TABLE; Wrong count");
      RX(n8, "We detected index corruption in an InnoDB type table\\. You have to dump.*forcing recovery");
      RX(n9, "for table [^ ]+");
      RX(n10, "The table [^ ]+ doesnt have");
      RX(n11, "The file .* already exists though the corresponding table did not exist in the InnoDB data dictionary");
      RX(n12, "Unable to import tablespace .* because it already exists.  Please DISCARD the tablespace before IMPORT");
      RX(n13, "Cannot add field.*in table.*because after adding it, the row size is.*which is greater than maximum allowed size.*for a record on index leaf page");
      RX(n14, "Failed to read page [0-9]+ from file [^:]+:");
      RX(n15, "Invalid column name for stopword table.*Its first column must be named as value");
      RX(n16, "Cannot rename [^ ]+ to [^ ]+ because the source file does not exist");
      RX(n17, "#sql-backup-[0-9a-f]+-[0-9]+");
      RX(n18, "`[^`]+`\\.`[^`]+`");
      t = n1.sub(t, "Cannot rename X to Y because the target schema directory doesnt exist", false);
      t = n2.sub(t, "Record in index X of table Y was not found on update: TUPLE Z at: COMPACT RECORD", false);
      t = n3.sub(t, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: COMPACT RECORD", false);
      t = n4.sub(t, "Record in index X of table Y was not found on update: TUPLE Z at: RECORD", false);
      t = n5.sub(t, "Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: RECORD", false);
      t = n6.sub(t, "In ALTER TABLE X has or is referenced in foreign key constraints which are not compatible with the new table definition", false);
      t = n7.sub(t, "Unable to flag corruption of X in table Y in CHECK TABLE; Wrong count", false);
      t = n8.sub(t, "We detected index corruption in an InnoDB type table", false);
      t = n9.sub(t, "for table X", true);
      t = n10.sub(t, "The table X doesnt have", true);
      t = n11.sub(t, "The file X already exists though the corresponding table did not exist in the InnoDB data dictionary", false);
      t = n12.sub(t, "Unable to import tablespace X because it already exists.  Please DISCARD the tablespace before IMPORT", false);
      t = n13.sub(t, "Cannot add field X in table Y because after adding it, the row size is greater than the maximum allowed size for a record on index leaf page", false);
      t = n14.sub(t, "Failed to read page X from file Y:", false);
      t = n15.sub(t, "Invalid column name for stopword table X. Its first column must be named as value", false);
      t = n16.sub(t, "Cannot rename X to Y because the source file does not exist", false);
      t = n17.sub(t, "#sql-backup-X", true);
      t = n18.sub(t, "X.X", true);
      return t;
    }
  }
  {
    RXI(dd, "\\[ERROR\\] DuckDB: .*");
    string s = grep_first(logs, dd, true);
    if (!s.empty()) {
      RX(d0, "^\\[ERROR\\] DuckDB: ");
      RX(d1, "Table.*could not be found");
      return "DUCKDB_ERROR|" + d1.sub(d0.sub(s, "", false), "Table X could not be found", false);
    }
  }
  {
    RXI(ha, "\\[ERROR\\] mysql_ha_read: .*");
    string s = grep_first(logs, ha, true);
    if (!s.empty()) { RX(h0, "^\\[ERROR\\] mysql_ha_read: "); return "MYSQL_HA_READ|" + h0.sub(s, "", false); }
  }
  {
    RXI(rk, "\\[ERROR\\] RocksDB: .*");
    string s = grep_first(logs, rk, true);
    if (!s.empty()) { RX(r0, "^\\[ERROR\\] RocksDB: "); return "ROCKSDB_ERROR|" + r0.sub(s, "RocksDB: ", false); }
  }
  {
    RXI(ct, "\\[ERROR\\] CHECKTABLE .*");
    string s = grep_first(logs, ct, true);
    if (!s.empty()) { RX(c0, "^\\[ERROR\\] CHECKTABLE "); return "CHECKTABLE|" + c0.sub(s, "CHECKTABLE ", false); }
  }
  {
    RXI(ec, "MariaDB error code: [0-9]+");
    string s = grep_first(logs, ec, true);
    if (!s.empty()) return "MARIADB_ERROR_CODE|" + s;
  }
  {
    RXI(me, "ERROR] mariadbd: .*");
    string s = grep_first(logs, me, true);
    if (!s.empty()) {
      RX(m0, "^ERROR] ");
      RX(m1, "'t[0-9]*'");
      RX(m2, "Incorrect information in file: '[^']*.frm'");
      RX(m3, "writing file '[^']*' ");
      string t = m1.sub(m0.sub(s, "", false), "table", false);
      t = m2.sub(t, "Incorrect information in frm file", false);
      t = m3.sub(t, "writing file ", false);
      return "MARIADBD_ERROR|" + t;
    }
  }
  {
    RXI(tb, "\\[ERROR\\] Table .*");
    string s = grep_first(logs, tb, true);
    if (!s.empty()) { RX(t0, "^\\[ERROR\\] Table "); return "MARIADBD_ERROR|" + t0.sub(s, "Table ", false); }
  }
  {
    RXI(se, "server_errno: [0-9]+");
    string s = grep_first(logs, se, true);
    if (!s.empty()) return "SERVER_ERRNO|" + s;
  }
  {
    RXI(sl1, "ERROR.*Slave[^:]*:[^0-9]*[Ee]rror[: 0-9]+");
    RX(sl0, "ERROR[] ]*");
    string s = grep_first(logs, sl1, true);
    if (!s.empty()) return "SLAVE_ERROR|" + sl0.sub(s, "", false);
    RXI(sl2, "ERROR.*Slave[^:]*:[^0-9]*[Ee]rror_code[: 0-9]+");
    s = grep_first(logs, sl2, true);
    if (!s.empty()) return "SLAVE_ERROR|" + sl0.sub(s, "", false);
  }
  {
    string top;
    if (els_run("top", logs, true, top, nullptr) && !top.empty()) return grep_lines(top)[0];
  }
  {
    string a = assert_from_logs(logs);
    if (!a.empty()) return "ASSERT|" + a;
  }
  return "";
}

// the assertion text the way new_text_string.sh reads it from the logs
string assert_from_logs(const vector<string>& logs) {
  RX(a1, "Assertion.*failed.$");
  RX(s1, "\\.$");
  RX(s2, "^Assertion [`]");
  RX(s3, "['] failed$");
  for (auto& lg : logs) {
    for (auto& l : grep_lines(read_file(lg))) {
      if (!a1.hit(l)) continue;
      return s3.sub(s2.sub(s1.sub(a1.first(l), "", false), "", false), "", false);
    }
  }
  RX(f1, "Failing assertion:");
  RX(f2, ".*Failing assertion:[ \t]*");
  for (auto& lg : logs) {
    for (auto& l : grep_lines(read_file(lg))) {
      if (!f1.hit(l)) continue;
      return f2.sub(l, "", false);
    }
  }
  // a Windows server: the release CRT writes "Assertion failed: expr, file X, line N", the debug
  // CRT "X(N) : Assertion failed: expr"; the expression is what a Linux log gives above
  RX(w1, "^Assertion failed: (.*), file .*, line [0-9]+\r?$");
  RX(w2, "^.*\\([0-9]+\\) : Assertion failed: (.*?)\r?$");
  for (auto& lg : logs) {
    for (auto& l : grep_lines(read_file(lg))) {
      if (w1.hit(l)) return w1.sub(l, "$1", false);
      if (w2.hit(l)) return w2.sub(l, "$1", false);
    }
  }
  return "";
}

// ---------------------------------------------------------------------------------------------
// gdb on a core: the two backtraces new_text_string.sh takes, under the GDB_PARALLEL cap
// ---------------------------------------------------------------------------------------------
namespace {
std::mutex g_gdb_mtx;
std::condition_variable g_gdb_cv;
int g_gdb_running = 0;
struct GdbSlot {
  GdbSlot() {
    std::unique_lock<std::mutex> lk(g_gdb_mtx);
    int cap = std::max(1, g_cfg.gdb_parallel);
    while (g_gdb_running >= cap) g_gdb_cv.wait(lk);
    g_gdb_running++;
  }
  ~GdbSlot() {
    std::lock_guard<std::mutex> lk(g_gdb_mtx);
    g_gdb_running--;
    g_gdb_cv.notify_one();
  }
};
const char* GDB_COMMANDS =
    "set pagination off\n"
    "set trace-commands off\n"
    "set frame-info short-location\n"
    "bt\n"
    "set print frame-arguments none\n"
    "set print repeats 0\n"
    "set print max-depth 0\n"
    "set print null-stop\n"
    "set print demangle on\n"
    "set print object off\n"
    "set print static-members off\n"
    "set print address off\n"
    "set print symbol-filename off\n"
    "set print symbol off\n"
    "set filename-display basename\n"
    "set print array off\n"
    "set print array-indexes off\n"
    "set print elements 1\n"
    "set logging file %s\n"
    "set logging enabled on\n"
    "bt\n"
    "set logging enabled off\n"
    "quit\n";
std::atomic<long> g_gdb_seq{0};
}  // namespace

// out1 = everything gdb printed (the 'Program terminated with' line and the first bt), out2 = the
// logged second bt with any "(gdb) " prompt removed
bool gdb_backtraces(const string& binary, const string& core, string& out1, string& out2, string* err) {
  static std::once_flag swept;
  std::call_once(swept, [] { sweep_stale_tmp("/tmp/omnium_gdb_"); });   // what a killed process left behind
  string logfile = fmt("/tmp/omnium_gdb_%d_%ld.gdb2", (int)getpid(), (long)++g_gdb_seq);
  unlink(logfile.c_str());
  {
    // the commands go in on stdin, as the script's heredoc does: gdb then reports a command it
    // does not know and carries on (a -x command file would stop at the first error)
    GdbSlot slot;
    CmdResult r = run_capture_in({"gdb", "-q", "-iex", "set debuginfod enabled off", binary, core}, fmt(GDB_COMMANDS, logfile.c_str()), 3600, "",
                                 {"DEBUGINFOD_TIMEOUT=13", "DEBUGINFOD_PROGRESS=0"});
    out1 = r.out;
    if (r.rc == -1 || r.rc == 124 || (r.rc != 0 && out1.empty())) {
      // 127 with no output is the exec that failed: the child's own exit code (run_capture_in)
      bool not_run = r.rc == -1 || (r.rc == 127 && out1.empty());
      if (err) *err = r.rc == 124 ? "gdb did not finish within the hour" : not_run ? "gdb could not be run (is it installed?)" : fmt("gdb exited with %d", r.rc);
      unlink(logfile.c_str());
      return false;
    }
  }
  RX(prompt, "^\\(gdb\\)[ ]*");
  out2 = per_line(read_file(logfile), [&](const string& l) { return prompt.sub(l, "", false); });
  unlink(logfile.c_str());
  return true;
}

// the frames part of the UniqueID from the two backtraces; "" when no frame parses
string frames_from_backtraces(const string& out1, const string& out2, string* sig, bool* no_frames_at_all) {
  RX(term, "Program terminated with");
  RXI(sigeq, "\\(sig=[0-9]+\\)");
  if (sig) {
    sig->clear();
    for (auto& l : grep_lines(out1)) {
      if (!term.hit(l)) continue;
      RX(ws, "with signal.*");
      RX(w1, "with signal ");
      RX(w2, "\\.$");
      RX(w3, "^([^,]+),.*$");
      string s = ws.first(l);
      if (s.empty()) continue;
      *sig = w3.sub(w2.sub(w1.sub(s, "", false), "", false), "$1", false);
      break;
    }
    if (sig->empty()) {
      for (auto& l : grep_lines(out1)) {
        string s = sigeq.first(l);
        if (s.empty()) continue;
        s.erase(std::remove(s.begin(), s.end(), '('), s.end());
        s.erase(std::remove(s.begin(), s.end(), ')'), s.end());
        *sig = s;
        break;
      }
    }
  }
  RXI(filter, "__interceptor_strcmp.part.0|std::terminate.*from|fprintf|__pthread_kill_.*|__GI___pthread_kill|__GI_raise |__GI_abort |__assert_fail|memmove|memcpy|\\?\\? \\(\\)|\\(gdb\\)|signal handler called|uw_update_context_1|uw_init_context_1|_Unwind_Resume|Warning: .set logging enabled off|Use .set logging enabled|__sanitizer::|__sanitizer_|__msan_|__msan::|__asan_|__asan::|__ubsan_|__ubsan::|__lsan_|__lsan::|__tsan_|__tsan::");
  RX(frameno, "^#[0-9]+[ \t]+");
  RX(at, "\\(.*\\) at ");
  RX(lineno, ":[ 0-9]+$");
  RX(handler, "signal handler called");
  RX(zero, "^#0");
  vector<string> bt = grep_lines(out2);
  auto clean = [&](vector<string> v) {
    vector<string> o;
    for (auto& l : v) {
      if (filter.hit(l)) continue;
      o.push_back(lineno.sub(at.sub(frameno.sub(l, "", false), "", false), "", false));
    }
    return o;
  };
  vector<string> gdb4 = clean(grep_context(bt, handler, 0, 100, false));
  if (gdb4.empty()) gdb4 = clean(grep_context(bt, zero, 0, 100, true));
  if (no_frames_at_all) *no_frames_at_all = gdb4.empty();
  vector<string> gdb3;
  RX(do_command, "^do_command");
  long dc = -1;
  for (size_t i = 0; i < gdb4.size(); i++) if (do_command.hit(gdb4[i])) { dc = (long)i + 1; break; }
  if (dc >= 5) {
    for (auto& l : grep_context(gdb4, do_command, 100, 0, false)) if (!do_command.hit(l)) gdb3.push_back(l);
  } else {
    gdb3 = gdb4;
  }
  RX(last_tok, " [^ ]+$");
  RX(paren, "[ ]*\\(.*");
  vector<string> frames;
  for (size_t i = 0; i < gdb3.size() && i < 4; i++) frames.push_back(paren.sub(last_tok.sub(gdb3[i], "", false), "", false));
  return join(frames, "|");
}

// the exception code of a Windows crash as the signal name a Linux build gives the same fault, so
// the UID stays comparable with the known-bugs lists; a code without a counterpart stays as printed
string windows_signal(const string& code_in) {
  string code = lower(code_in);
  if (code == "0xc0000005" || code == "0xc0000006" || code == "0xc00000fd") return "SIGSEGV";   // access violation, in-page error, stack overflow
  if (code == "0x80000003") return "SIGABRT";                                                    // the __debugbreak in my_sigabrt_handler: abort() and a failed assert
  if (code == "0xc000001d" || code == "0xc0000096") return "SIGILL";                            // illegal, privileged instruction
  if (code == "0xc0000094" || code == "0xc0000095" || code == "0xc000008e" || code == "0xc0000090" || code == "0xc0000091") return "SIGFPE";
  if (code == "0x80000002") return "SIGBUS";                                                    // datatype misalignment
  return "exception " + code;
}

// A Windows server has no core and no gdb: it walks its own stack into the error log, one frame per
// line as module!symbol()[file:line], from the faulting instruction down. The frames are read the way
// frames_from_backtraces reads gdb's: the abort route and the CRT and OS modules go, as __GI_raise,
// __GI_abort and __assert_fail go there, ??? is gdb's ?? (), and the do_command rule is the same.
// The first crash in the logs is the one read, as assert_from_logs reads the first assertion. ""
// when the logs hold no such backtrace.
string frames_from_windows_log(const vector<string>& logs, string* sig) {
  if (sig) sig->clear();
  RX(got, "got exception (0x[0-9a-fA-F]+) ;");
  RX(code_re, "0x[0-9a-fA-F]+");
  RX(frame, "^([A-Za-z0-9_.+-]+)!([^\r]*?)(?:\\(\\))?(?:\\[[^\\]]*\\])?\r?$");
  RX(blank, "^[ \t\r]*$");
  RXI(skip_mod, "^(ucrtbase|ucrtbased|vcruntime[0-9]*d?|msvcp[0-9]*d?|kernel32|kernelbase|ntdll)\\.dll$");
  RX(skip_sym, "^(my_sigabrt_handler|my_parameter_handler|raise|abort|_wassert|_assert|common_assert_to_stderr.*|_invalid_parameter.*|_CrtDbgReport.*|_CrtDbgBreak|memmove|memcpy|\\?\\?\\?)$");
  vector<string> gdb4;
  for (auto& lg : logs) {
    vector<string> v = grep_lines(read_file(lg));
    size_t i = 0;
    for (; i < v.size(); i++) if (got.hit(v[i])) break;
    if (i == v.size()) continue;
    if (sig) *sig = windows_signal(code_re.first(got.first(v[i])));
    bool begun = false;
    for (i++; i < v.size(); i++) {
      const string& l = v[i];
      if (!frame.hit(l)) {
        if (blank.hit(l) || !begun) continue;
        break;
      }
      begun = true;
      string mod = frame.sub(l, "$1", false), symb = frame.sub(l, "$2", false);
      if (skip_mod.hit(mod) || skip_sym.hit(symb) || symb.empty()) continue;
      gdb4.push_back(symb);
    }
    break;
  }
  if (gdb4.empty()) return "";
  RX(do_command, "^do_command");
  long dc = -1;
  for (size_t i = 0; i < gdb4.size(); i++) if (do_command.hit(gdb4[i])) { dc = (long)i + 1; break; }
  vector<string> gdb3;
  if (dc >= 5) {
    for (auto& l : grep_context(gdb4, do_command, 100, 0, false)) if (!do_command.hit(l)) gdb3.push_back(l);
  } else {
    gdb3 = gdb4;
  }
  vector<string> frames;
  for (size_t i = 0; i < gdb3.size() && i < 4; i++) frames.push_back(gdb3[i]);
  return join(frames, "|");
}

// ---------------------------------------------------------------------------------------------
// new_text_string.sh on a trial (or basedir) directory
// ---------------------------------------------------------------------------------------------
namespace {
vector<string> glob_files(const string& pattern) {   // ls <pattern>, regular files, newest first
  glob_t g{};
  vector<std::pair<int64_t, string>> v;
  if (glob(pattern.c_str(), 0, nullptr, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      struct stat st;
      if (stat(g.gl_pathv[i], &st) == 0 && S_ISREG(st.st_mode)) v.push_back({(int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec, g.gl_pathv[i]});
    }
  }
  globfree(&g);
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  vector<string> out;
  for (auto& p : v) out.push_back(p.second);
  return out;
}
string newest_core(const string& loc) {
  for (auto& f : glob_files(loc + "/*/*core*")) if (f.find("data.PREV") == string::npos) return f;
  for (auto pat : {"/var/log/*/mysqld*/data*/*core*", "/var/*/log/*/mysqld*/data*/*core*", "/var/mysqld*/data*/*core*"}) {
    auto v = glob_files(loc + pat);
    if (!v.empty()) return v[0];
  }
  return "";
}
bool is_elf_file(const string& p) {
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  int fd = open(p.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char head[4];   // the header only: a server binary is around 100 MB
  bool elf = read(fd, head, 4) == 4 && memcmp(head, "\x7f" "ELF", 4) == 0;
  close(fd);
  return elf;
}
string binary_from_log(const string& log) {
  // grep "ready for connections" log | sed 's|: .*||;s|^.* ||' | head -n1
  RX(rd, "ready for connections");
  RX(s1, ": .*");
  RX(s2, "^.* ");
  for (auto& l : grep_lines(read_file(log))) {
    if (!rd.hit(l)) continue;
    string p = s2.sub(s1.sub(l, "", false), "", false);
    if (file_exists(p)) return p;
    return "";
  }
  return "";
}
}  // namespace

// the server binary for a trial or basedir directory, the way new_text_string.sh looks for it
string binary_for_dir(const string& loc) {
  string bd = trim(read_file(loc + "/BASEDIR"));
  if (!bd.empty()) {
    for (auto b : {"/bin/mariadbd", "/bin/mysqld"}) if (is_elf_file(bd + b)) return bd + b;
  }
  for (auto rel : {"/bin/mariadbd", "/bin/mysqld", "/mysqld/mariadbd", "/mysqld/mysqld", "/../bin/mariadbd", "/../mariadbd", "/../mysqld", "/../bin/mysqld",
                   "/../mysqld/mariadbd", "/../mysqld/mysqld", "/../../mysqld/mariadbd", "/../../mysqld/mysqld", "/../../../../../bin/mariadbd",
                   "/../../../../../bin/mysqld"}) {
    string p = loc + rel;
    if (is_elf_file(p)) return p;
  }
  for (auto lg : {"/log/mysqld.out", "/log/master.err", "/log/slave.err", "/node1/node1.err", "/node2/node2.err", "/node3/node3.err"}) {
    if (!file_exists(loc + lg)) continue;
    string p = binary_from_log(loc + lg);
    if (!p.empty()) return p;
    break;
  }
  return "";
}

bool uid_for_dir(const string& loc_in, UidResult& r, const UidOptions& o) {
  r = UidResult();
  string loc = abs_path(loc_in.empty() ? "." : loc_in);
  r.loc = loc;
  // the logs
  vector<string> logs;
  if (file_exists(loc + "/log/master.err")) logs.push_back(loc + "/log/master.err");
  if (file_exists(loc + "/log/slave.err")) logs.push_back(loc + "/log/slave.err");
  if (logs.empty()) {
    for (auto rel : {"/../../mysqld.1.err", "/../../mysqld.2.err", "/var/log/mysqld.1.err", "/var/log/mysqld.2.err", "/var/log/mysqld.3.1.err", "/var/log/mysqld.2.1.err",
                     "/var/log/mysqld.1.1.err", "/log/mysqld.out"})
      if (file_exists(loc + rel)) logs.push_back(loc + rel);
    if (logs.empty()) for (auto& f : glob_files(loc + "/var/*/log/mysqld.[12].err")) logs.push_back(f);
  }
  // the marker files of the binlog recovery test come first
  if (file_exists(loc + "/BINLOG_RECOVERY_ERROR")) {
    string l = read_file(loc + "/BINLOG_RECOVERY_ERROR");
    l = grep_lines(l).empty() ? "" : grep_lines(l)[0];
    l.erase(std::remove(l.begin(), l.end(), '\r'), l.end());
    if (!l.empty()) {
      RX(b1, "(ERROR [0-9]+) \\([0-9A-Z]{5}\\)");
      RX(b2, "'[^']*'");
      RX(b3, "[0-9]+");
      RX(b4, "[ \t]+");
      RX(b5, "^ ");
      RX(b6, " $");
      l = b1.sub(l, "$1 (X)", false);
      l = b2.sub(l, "X", true);
      l = b3.sub(l, "N", true);
      l = b6.sub(b5.sub(b4.sub(l, " ", true), "", false), "", false);
      r.uid = "BINLOG_RECOVERY_ERROR|" + l;
      return true;
    }
  }
  if (file_exists(loc + "/BINLOG_CHECKSUM_DIFF")) {
    string l = read_file(loc + "/BINLOG_CHECKSUM_DIFF");
    l = grep_lines(l).empty() ? "" : grep_lines(l)[0];
    l.erase(std::remove(l.begin(), l.end(), '\r'), l.end());
    if (!l.empty()) {
      RX(c1, "[0-9]{6,}");
      RX(c2, "[ \t]+");
      RX(c3, "^ ");
      RX(c4, " $");
      l = c4.sub(c3.sub(c2.sub(c1.sub(l, "N", true), " ", true), "", false), "", false);
      r.uid = "BINLOG_CHECKSUM_DIFF|" + l;
    } else {
      r.uid = "BINLOG_CHECKSUM_DIFF|generic";
    }
    return true;
  }
  if (logs.empty()) { r.err = "Assert: no error log(s) found - exiting"; return false; }
  // a core still being written gets time to finish (the script's first block)
  string all_logs;
  for (auto& lg : logs) all_logs += read_file(lg);
  RXI(san_any, "=ERROR:|ThreadSanitizer:|runtime error:|LeakSanitizer:|MemorySanitizer:");
  bool san_marker = false;
  for (auto& l : grep_lines(all_logs)) if (san_any.hit(l)) { san_marker = true; break; }
  if (o.wait_core && !san_marker) {
    string c = o.core.empty() ? newest_core(loc) : o.core;
    if (c.empty()) sleep(2);
    else if (now_s() - file_mtime(c) <= 10) {
      int64_t size = -1;
      int stable = 0;
      for (int i = 0; i < 50; i++) {
        int64_t n = file_size(c);
        if (n == size) { if (++stable >= 3) break; } else stable = 0;
        size = n;
        usleep(200000);
      }
    }
  }
  r.binary = o.binary.empty() ? binary_for_dir(loc) : o.binary;
  r.core = o.core.empty() ? newest_core(loc) : o.core;
  if (r.core.find("mysqld.1") != string::npos) for (auto& l : logs) l = replace_all(l, "mysqld.2", "mysqld.1");
  else if (r.core.find("mysqld.2") != string::npos) for (auto& l : logs) l = replace_all(l, "mysqld.1", "mysqld.2");
  if (r.core.find("data_slave") != string::npos) for (auto& l : logs) l = replace_all(l, "master.err", "slave.err");
  else if (r.core.find("data/core") != string::npos) for (auto& l : logs) l = replace_all(l, "slave.err", "master.err");
  {
    std::set<string> seen;
    vector<string> u;
    for (auto& l : logs) if (seen.insert(l).second) u.push_back(l);
    std::sort(u.begin(), u.end());
    logs = u;
  }
  r.logs = logs;
  vector<string> caplogs = capped_logs(logs);
  // *SAN first
  bool san_bug = false;
  {
    string all_capped;
    for (auto& lg : caplogs) all_capped += read_file(lg);
    if (all_capped.find("=ERROR:") != string::npos || icontains(all_capped, "ThreadSanitizer:") || icontains(all_capped, "runtime error:") ||
        icontains(all_capped, "LeakSanitizer:") || icontains(all_capped, "MemorySanitizer:"))
      san_bug = true;
  }
  if (san_bug) {
    r.san = true;
    string e;
    r.uid = uid_san(caplogs, &e);
    if (r.uid.empty() && !e.empty()) { r.err = e; return false; }
    return true;
  }
  if (r.core.empty()) {
    // a Windows server: no core and no gdb, the frames are in the log and rank as a core's would
    string wsig, wframes = frames_from_windows_log(caplogs, &wsig);
    if (!wframes.empty()) {
      string text = assert_from_logs(caplogs);
      text = text.empty() ? wsig : text + "|" + wsig;
      r.uid = o.frames_only ? wframes : text + "|" + wframes;
      return true;
    }
    string other = uid_other_strings(caplogs);
    if (!other.empty()) { r.uid = other; return true; }
    if (loc.find("SAN") != string::npos) { r.err = "Assert: no core file found in */*core*, and this is a SAN build, so fallback_text_string.sh was not attempted"; return false; }
    for (auto& lg : caplogs) {
      string e;
      string f = uid_fallback(lg, &e);
      if (!f.empty()) { r.uid = f; return true; }
    }
    r.err = "Assert: no core file found in */*core*, and fallback_text_string.sh returned an empty output for all logs";
    return false;
  }
  // no binary found: the script still runs gdb (with an empty name) and ends in the frames assert
  string out1, out2, e;
  if (!gdb_backtraces(r.binary, r.core, out1, out2, &e)) { r.err = e; return false; }
  r.gdb_first_bt = out1;
  r.gdb_second_bt = out2;
  string text = assert_from_logs(caplogs);
  string sig;
  bool none = false;
  string frames = frames_from_backtraces(out1, out2, &sig, &none);
  if (!sig.empty()) text = text.empty() ? sig : text + "|" + sig;
  if (frames.empty()) { r.err = none ? "Assert: No parsable frames? (may be improved upon)" : "Assert: No parsable frames?"; return false; }
  if (o.frames_only || text.empty()) text = frames;
  else text = text + "|" + frames;
  text = replace_all(text, "__cxa_pure_virtual () from", "__cxa_pure_virtual");
  text = test_root().quoted->sub(text, "\"", false);
  if (text == "SIGABRT|Backtrace stopped: Cannot access memory at address|")
    text = "GENERIC_MEMORY_CORRUPTION_ISSUE|DO_NOT_ADD_TO_KNOWN_BUGS|SIGABRT|Backtrace stopped: Cannot access memory at address|";
  string lead = trim(text);
  if (!lead.empty() && lead[0] == '#') {
    r.err = "Assert: leading character of unique bug id (" + text + ") is a '#', which will lead to issues in other scripts. This would normally never happen, but it did. Please improve new_text_string.sh to handle this situation!";
    return false;
  }
  r.uid = text;
  return true;
}

// a pasted raw gdb trace (new_text_string.sh <file>): RAW_GDB_UID|f1|f2|f3|f4
string uid_raw_gdb(const string& file) {
  string text = read_file(file);
  string one;
  for (auto& l : grep_lines(text)) { RX(blank, "^[ \t]*$"); if (!blank.hit(l)) { one += l; one += ' '; } }
  RX(split_at, "(#[0-9]+[ ]+)");
  one = split_at.sub(one, "\n$1", true);
  RX(f1, "^#[0-9]+[ ]+");
  RX(f2, "^0x[0-9A-Fa-f]+[ ]+");
  RX(f3, " [^ ]+$");
  RX(f4, "[ ]*\\(.*");
  RX(blank, "^[ \t]*$");
  vector<string> frames;
  for (auto& l : grep_lines(one)) {
    if (blank.hit(l)) continue;
    if (frames.size() >= 4) break;
    frames.push_back(f4.sub(f3.sub(f2.sub(f1.sub(l, "", false), "", false), "", false), "", false));
  }
  return "RAW_GDB_UID|" + join(frames, "|");
}

// ---------------------------------------------------------------------------------------------
// drop_one_or_more_san_from_log.sh: known *SAN reports leave the top of the log, the trial's
// MYBUG follows the new top. Returns how many blocks went.
// ---------------------------------------------------------------------------------------------
namespace {
bool del_first_san_block(const string& log) {
  RX(start_rx, "=ERROR:|runtime error:|AddressSanitizer:|ThreadSanitizer:|LeakSanitizer:|MemorySanitizer:");
  RX(end_rx, "^SUMMARY:|=ABORTING");
  string text = read_file(log);
  vector<string> v = grep_lines(text);
  long e = -1, s = -1;
  for (size_t i = 0; i < v.size(); i++) if (end_rx.hit(v[i])) { e = (long)i; break; }
  if (e < 0) return false;
  for (long i = 0; i <= e; i++) if (start_rx.hit(v[i])) { s = i; break; }
  if (s < 0) return false;
  v.erase(v.begin() + s, v.begin() + e + 1);
  string out = join_nl(v);
  if (!text.empty() && text.back() == '\n') out += '\n';
  return write_file(log, out);
}
}  // namespace

int san_drop_known(const string& trial_dir, const string& log_rel) {
  string log = trial_dir + "/" + log_rel;
  if (!file_exists(log)) return 0;
  RXI(start_rx, "=ERROR:|runtime error:|AddressSanitizer:|ThreadSanitizer:|LeakSanitizer:|MemorySanitizer:");
  RXI(end_rx, "^SUMMARY:|=ABORTING");
  int dropped = 0;
  for (int loop = 0; loop < 70; loop++) {
    UidResult r;
    UidOptions o;
    o.wait_core = false;
    uid_for_dir(trial_dir, r, o);
    if (r.uid.empty()) break;
    KbVerdict v = kb_verdict(kb_search(r.uid));
    if (!(v == KbVerdict::Known || v == KbVerdict::KnownAndFixed)) break;
    if (!grep_any(vector<string>{log}, start_rx) || !grep_any(vector<string>{log}, end_rx)) break;
    if (!file_exists(log + ".pre_known_san_removal")) copy_file(log, log + ".pre_known_san_removal");
    if (!del_first_san_block(log)) break;
    dropped++;
    if (file_exists(trial_dir + "/MYBUG") || file_exists(trial_dir + "/pquery.log") || file_exists(trial_dir + "/MYEXTRA") || file_exists(trial_dir + "/ERROR_LOG_SCAN_ISSUE")) {
      UidResult r2;
      uid_for_dir(trial_dir, r2, o);
      write_file(trial_dir + "/MYBUG", r2.uid.empty() ? (r2.err.empty() ? "" : r2.err + "\n") : r2.uid + "\n");
      write_file(trial_dir + "/TOP_SAN_ISSUES_REMOVED", "");
    }
  }
  return dropped;
}

// ---------------------------------------------------------------------------------------------
// stack.sh: the *SAN or Valgrind block, or the assertion plus a full gdb backtrace, in a noformat
// block titled with the version banner
// ---------------------------------------------------------------------------------------------
string stack_text(const string& dir_in, const string& title, string* err) {
  string dir = abs_path(dir_in.empty() ? "." : dir_in);
  string core = newest_core(dir);
  string log;
  for (auto rel : {"/log/master.err", "/log/slave.err", "/var/log/mysqld.2.err", "/var/log/mysqld.1.err"}) if (file_exists(dir + rel)) { log = dir + rel; break; }
  if (core.find("mysqld.1") != string::npos) log = replace_all(log, "mysqld.2", "mysqld.1");
  else if (core.find("mysqld.2") != string::npos) log = replace_all(log, "mysqld.1", "mysqld.2");
  if (core.find("data_slave") != string::npos) log = replace_all(log, "master.err", "slave.err");
  else if (core.find("data/core") != string::npos) log = replace_all(log, "slave.err", "master.err");
  string assert_line;
  vector<string> lines;
  if (!log.empty()) {
    lines = grep_lines(read_file(log));
    RX(a1, "Assertion.*failed.$");
    RX(a2, "Failing assertion:");
    for (auto& l : lines) if (a1.hit(l)) { assert_line = l; break; }
    if (assert_line.empty()) for (auto& l : lines) if (a2.hit(l)) { assert_line = l; break; }
  }
  string head = "{noformat:title=" + title + "}";
  if (!log.empty()) {
    string text = read_file(log);
    const char* start_pat = nullptr;
    const char* end_pat = "^SUMMARY:|=ABORTING$";
    RX(valgrind, "==[0-9]+== ERROR SUMMARY:");
    if (grep_any(vector<string>{log}, valgrind)) {
      start_pat = "==[0-9]+==.*(Invalid read|Invalid write|Invalid free|Mismatched free|uninitialised|Syscall param|Source and destination overlap|Jump to the invalid address|Process terminating with default action)";
      end_pat = "==[0-9]+== ERROR SUMMARY:";
    } else if (text.find("MemorySanitizer:") != string::npos) {
      start_pat = "^SUMMARY:|=ERROR:|MemorySanitizer:";
    } else if (text.find("ThreadSanitizer:") != string::npos) {
      start_pat = "WARNING: ThreadSanitizer:|SUMMARY: ThreadSanitizer:";
    } else if (text.find("runtime error:") != string::npos || text.find("AddressSanitizer:") != string::npos || text.find("LeakSanitizer:") != string::npos || text.find("=ERROR:") != string::npos) {
      start_pat = "^SUMMARY:|=ERROR:|runtime error:|AddressSanitizer:|LeakSanitizer:";
    }
    if (start_pat) {
      Rx srx(start_pat), erx(end_pat);
      long s = -1, e = -1;
      for (size_t i = 0; i < lines.size(); i++) if (srx.hit(lines[i])) { s = (long)i; break; }
      for (size_t i = 0; i < lines.size(); i++) if (erx.hit(lines[i])) e = (long)i;
      if (s >= 0) {
        if (e < 0 || e < s) e = s + 200;
        string out = head + "\n";
        for (long i = s; i <= e && i < (long)lines.size(); i++) out += lines[i] + "\n";
        return out + "{noformat}\n";
      }
    }
  }
  if (core.empty()) {
    if (err) *err = "INFO: no cores found at data*/*core* nor at node*/*core*, and no *SAN or Valgrind report found in " + (log.empty() ? string("the error log") : log);
    return "";
  }
  string bin = binary_for_dir(dir);
  if (bin.empty()) { if (err) *err = "Assert: no server binary found for " + dir; return ""; }
  string out;
  if (!assert_line.empty()) out += head + "\n" + assert_line + "\n{noformat}\n\n";
  CmdResult r;
  {
    GdbSlot slot;
    r = run_capture_in({"gdb", "-q", "-iex", "set debuginfod enabled off", bin, core}, " set pagination off\n set print pretty on\n set print frame-arguments all\n bt\n quit\n",
                       3600, "", {"DEBUGINFOD_TIMEOUT=13", "DEBUGINFOD_PROGRESS=0"});
  }
  out += head + "\n";
  // stack.sh: grep -A999 'Core was generated by' | grep -v 'No such file or directory' |
  //   sed 's|(gdb) (gdb) |(gdb) bt\n|' | sed 's|(gdb) (gdb) ||' | awk (join the 4-space continuation
  //   lines) | grep -v '^(gdb)[ \t]*$' | grep -viE 'Downloading source file|Download failed'
  vector<string> stage;
  bool started = false;
  for (auto& l : grep_lines(r.out)) {
    if (!started) { if (l.find("Core was generated by") != string::npos) started = true; else continue; }
    if (l.find("No such file or directory") != string::npos) continue;
    string x = l;
    size_t p = x.find("(gdb) (gdb) ");
    if (p != string::npos) {
      x.replace(p, 12, "(gdb) bt\n");
      size_t p2 = x.find("(gdb) (gdb) ");
      if (p2 != string::npos) x.erase(p2, 12);
    }
    for (auto& part : grep_lines(x)) stage.push_back(part);
  }
  string joined;
  for (size_t i = 0; i < stage.size(); i++) {
    const string& l = stage[i];
    if (starts_with(l, "    ")) joined += l.substr(4);
    else { if (i > 0) joined += "\n"; joined += l; }
  }
  joined += "\n";
  RX(prompt_only, "^\\(gdb\\)[ \t]*$");
  RXI(dl, "Downloading source file|Download failed");
  for (auto& l : grep_lines(joined)) {
    if (prompt_only.hit(l) || dl.hit(l)) continue;
    out += l + "\n";
  }
  out += "{noformat}\n";
  return out;
}

// ---------------------------------------------------------------------------------------------
// verbs: omnium t, tt, els, sts, fts, stack, parity
// ---------------------------------------------------------------------------------------------
static string dir_arg(const Args& a, size_t i = 0) { return a.size() > i && !starts_with(a[i], "--") ? a[i] : "."; }

int cmd_t(const Args& a) {
  UidOptions o;
  for (auto& x : a) if (x == "FRAMESONLY" || x == "--frames-only") o.frames_only = true;
  string d = dir_arg(a);
  if (file_exists(d) && !dir_exists(d)) {
    string text = read_file(d);
    RX(gdbtrace, "#[0-9]  ");
    if (gdbtrace.hit(text)) { printf("%s\n", uid_raw_gdb(d).c_str()); return 0; }
    if (is_elf_file(d)) { o.binary = d; d = "."; }
  }
  UidResult r;
  if (!uid_for_dir(d, r, o)) { printf("%s\n", r.err.c_str()); return 1; }
  printf("%s\n", r.uid.c_str());
  return 0;
}
int cmd_tt(const Args& a) {
  UidResult r;
  UidOptions o;
  string d = dir_arg(a);
  if (!uid_for_dir(d, r, o)) { printf("%s\n", r.err.c_str()); return 1; }
  printf("%s\n", r.uid.c_str());
  if (r.uid.empty()) return 0;
  KbMatch m = kb_search(r.uid);
  fputs(kb_verdict_text(r.uid, m).c_str(), stdout);
  for (auto& u : kb_jira_urls(r.uid)) printf("%s\n", u.c_str());
  if (auto s = seen_lookup(r.uid)) printf("----- omnium.seen -----\nfirst %s, last %s, %ld times, last run %s, outcome %s\n", stamp_of(s->first).c_str(), stamp_of(s->last).c_str(), s->count, s->run.c_str(), s->outcome.c_str());
  return 0;
}
int cmd_els(const Args& a) {
  if (a.size() < 2) { printf("usage: omnium els {errors|lastline|top|check|clean|aggregate} <log>... [--exclude-assert]\n"); return 1; }
  vector<string> logs;
  bool exa = getenv("EXCLUDE_ASSERT") && string(getenv("EXCLUDE_ASSERT")) == "1";
  for (size_t i = 1; i < a.size(); i++) { if (a[i] == "--exclude-assert") exa = true; else logs.push_back(a[i]); }
  string out, err;
  bool ok = els_run(a[0], logs, exa, out, &err);
  if (!err.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!ok) return 1;
  if (!out.empty()) printf("%s\n", out.c_str());
  return 0;
}
int cmd_sts(const Args& a) {
  vector<string> logs;
  string d = dir_arg(a);
  if (dir_exists(d)) {
    if (file_exists(d + "/log/master.err")) logs.push_back(d + "/log/master.err");
    else if (file_exists(d + "/master.err")) logs.push_back(d + "/master.err");
    if (file_exists(d + "/log/slave.err")) logs.push_back(d + "/log/slave.err");
    else if (file_exists(d + "/slave.err")) logs.push_back(d + "/slave.err");
    if (logs.empty()) { printf("Assert: a directory was passed to this script, and %s/log/master.err does not exist within it.\n", d.c_str()); return 1; }
  } else if (file_exists(d)) {
    for (auto& x : a) if (file_exists(x)) logs.push_back(x);
  } else {
    printf("Assert: %s does not exist.\n", d.c_str());
    return 1;
  }
  string err;
  string uid = uid_san(capped_logs(logs), &err);
  if (!err.empty()) { printf("%s\n", err.c_str()); return 1; }
  if (!uid.empty()) printf("%s\n", uid.c_str());
  return 0;
}
int cmd_fts(const Args& a) {
  string log = a.empty() ? "" : a[0];
  if (log.empty()) {
    if (file_exists("./log/master.err")) log = "./log/master.err";
    else if (file_exists("./var/log/mysqld.1.err")) log = "./var/log/mysqld.1.err";
    else { fprintf(stderr, "Assert: no error log file name was passed to fallback_text_string.sh\n"); return 1; }
  }
  string err;
  string uid = uid_fallback(log, &err);
  if (uid.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  printf("%s\n", uid.c_str());
  return 0;
}
int cmd_stack(const Args& a) {
  string d = dir_arg(a);
  string title;
  Basedir b;
  string bin = binary_for_dir(abs_path(d));
  if (!bin.empty()) {
    string bd = dirname_of(dirname_of(bin));
    if (basedir_probe(bd, b)) title = basedir_banner_title(b);
  }
  if (title.empty()) title = "stack";
  string err;
  string out = stack_text(d, title, &err);
  if (out.empty()) { printf("%s\n", err.c_str()); return 1; }
  fputs(out.c_str(), stdout);
  return 0;
}

// omnium parity [N|all] [--no-gdb] [--verbose]: the bash chain against this port on the saved
// trials under /data, N per bug class (default 25), output and exit code both compared
int cmd_parity(const Args& a) {
  long per_class = 25;
  bool use_gdb = true, verbose = false;
  for (auto& x : a) {
    if (x == "all") per_class = 0;
    else if (x == "--no-gdb") use_gdb = false;
    else if (x == "--verbose" || x == "-v") verbose = true;
    else if (is_digits(x)) per_class = to_long(x);
    // a typo must not read as "every trial, with gdb", which is the long way round
    else { printf("omnium parity: %s is not a number, all, --no-gdb or --verbose\n", x.c_str()); return 2; }
  }
  string qa = g_paths.qa;
  for (auto s : {"/error_log_scan.sh", "/new_text_string.sh", "/san_text_string.sh", "/fallback_text_string.sh"})
    if (!is_executable(qa + s)) { printf("%s%s is missing, nothing to compare against\n", qa.c_str(), s); return 2; }
  // the corpus: trial dirs with a MYBUG and a readable master.err, grouped by class
  std::map<string, vector<string>> classes;
  {
    CmdResult r = run_shell("/usr/bin/find " + sh_quote(g_cfg.data_dir) + " -maxdepth 3 -name MYBUG 2>/dev/null", 600);
    for (auto& mb : split_lines(r.out)) {
      if (mb.empty()) continue;
      string dir = dirname_of(mb);
      if (!file_exists(dir + "/log/master.err")) continue;
      string u = trim(read_file(mb));
      size_t nl = u.find('\n');
      if (nl != string::npos) u = u.substr(0, nl);
      string k;
      if (starts_with(u, "SIG")) k = "SIGNAL";
      else if (icontains(u, "assert")) k = "ASSERT";
      else { size_t p = u.find('|'); k = p != string::npos && p > 0 ? u.substr(0, p) : "OTHER"; }
      for (char& c : k) if (!isalnum((unsigned char)c) && c != '_') c = '_';
      classes[k].push_back(dir);
    }
  }
  if (classes.empty()) { printf("no trials with a MYBUG file and a readable error log under %s\n", g_cfg.data_dir.c_str()); return 1; }
  vector<string> trials;
  printf("=== Bug classes found under %s ===\n", g_cfg.data_dir.c_str());
  for (auto& [k, v] : classes) {
    printf("%6zu  %s\n", v.size(), k.c_str());
    vector<string> s = v;
    std::sort(s.begin(), s.end());
    for (size_t i = 0; i < s.size() && (per_class == 0 || (long)i < per_class); i++) trials.push_back(s[i]);
  }
  printf("=== Comparing %zu trials (%s) ===\n", trials.size(), use_gdb ? "gdb included" : "--no-gdb: new_text_string.sh left out");
  long checks = 0, diffs = 0;
  auto compare = [&](const string& trial, const string& tool, const CmdResult& bash, int rc_cpp, const string& out_cpp) {
    checks++;
    string bo = bash.out;
    while (!bo.empty() && bo.back() == '\n') bo.pop_back();
    string co = out_cpp;
    while (!co.empty() && co.back() == '\n') co.pop_back();
    bool same = (bo == co) && (bash.rc == rc_cpp);
    if (!same) {
      diffs++;
      printf("DIFF %s %s\n  bash rc=%d: %s\n  omnium rc=%d: %s\n", trial.c_str(), tool.c_str(), bash.rc, bo.c_str(), rc_cpp, co.c_str());
    } else if (verbose) {
      printf("same %s %s: %s\n", trial.c_str(), tool.c_str(), co.c_str());
    }
  };
  for (auto& trial : trials) {
    string parent = dirname_of(trial), name = basename_of(trial);
    string log_rel = "./" + name + "/log/master.err";
    for (auto mode : {"errors", "lastline", "top", "check", "clean", "aggregate"}) {
      CmdResult b = run_capture({qa + "/error_log_scan.sh", mode, log_rel}, 600, parent);
      // run_capture merges both streams; a caller reads the script through $(...), which is stdout
      // alone. One grep in the script's chain has no --binary-files=text, so a log with a byte the
      // locale cannot decode makes it say so on stderr. That line is not part of the answer.
      {
        vector<string> keep;
        for (auto& l : grep_lines(b.out)) if (!(starts_with(l, "grep: ") && ends_with(l, "binary file matches"))) keep.push_back(l);
        b.out = join_nl(keep);
      }
      string out, err;
      bool ok;
      if (string(mode) == "aggregate") {
        // its trial column comes from the ./<trial>/ path, so this side reads the log as the script does
        string cwd = abs_path(".");
        ok = chdir(parent.c_str()) == 0 && els_run(mode, {log_rel}, false, out, &err);
        if (chdir(cwd.c_str()) != 0) { /* the next trial's paths are absolute */ }
      } else {
        ok = els_run(mode, {trial + "/log/master.err"}, false, out, &err);
      }
      compare(trial, string("els ") + mode, b, ok ? 0 : 1, out);
    }
    {
      CmdResult b = run_capture({qa + "/fallback_text_string.sh", log_rel}, 600, parent);
      string err;
      string out = uid_fallback(trial + "/log/master.err", &err);
      // the script writes its asserts to stderr and run_capture merges both streams: only the
      // FALLBACK line counts on the bash side
      CmdResult bb = b;
      string keep;
      for (auto& l : grep_lines(b.out)) if (starts_with(l, "FALLBACK|")) keep = l;
      bb.out = keep;
      compare(trial, "fts", bb, out.empty() ? 1 : 0, out);
    }
    {
      CmdResult b = run_capture({qa + "/san_text_string.sh", log_rel}, 600, parent);
      vector<string> logs = {trial + "/log/master.err"};
      string err;
      string out = uid_san(capped_logs(logs), &err);
      int rc = err.empty() ? 0 : 1;
      if (!err.empty()) out = replace_all(err, trial + "/log/master.err", log_rel);   // the script names the log as it was given it
      compare(trial, "sts", b, rc, out);
    }
    if (use_gdb) {
      CmdResult b = run_capture({script_path("new_text_string.sh")}, 3600, trial);
      UidResult r;
      UidOptions o;
      o.wait_core = false;
      bool ok = uid_for_dir(trial, r, o);
      compare(trial, "nts", b, ok ? 0 : 1, ok ? r.uid : r.err);
    }
  }
  printf("=== %ld checks, %ld differences ===\n", checks, diffs);
  return diffs ? 1 : 0;
}
