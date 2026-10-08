// Created by Roel Van de Paar, MariaDB
// --selftest: the built-in checks. build.sh runs them on every build and keeps a failing binary
// as .failed.
#include "verbs.h"
#include "connect.h"
#include "winterm.h"
#include "winproc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <poll.h>
#include <fnmatch.h>
#include <cstring>
#include <set>

static std::atomic<long> g_st_pass{0}, g_st_fail{0};
static std::mutex g_st_mtx;
static vector<string> g_st_failed;               // named again under the summary, so a pipe cannot lose them

bool st_check_r(bool cond, const string& what);
void st_check(bool cond, const string& what) {
  if (cond) { g_st_pass++; return; }
  g_st_fail++;
  std::lock_guard<std::mutex> lk(g_st_mtx);
  g_st_failed.push_back(what);
  fprintf(stderr, "selftest FAIL: %s\n", what.c_str());
}
bool st_check_r(bool cond, const string& what) { st_check(cond, what); return cond; }
// a stand-in server child is asked to stop and only killed if it will not go
static void st_stub_stop(Child& c) {
  child_send(c, "stop");
  int rc = -1;
  for (int i = 0; i < 60 && rc == -1; i++) rc = child_reap(c, 100);
  if (rc == -1) { kill_group(c.pid, SIGKILL); child_reap(c, 5000); }
  child_close(c);
}
void st_eq(const string& got, const string& want, const string& what) {
  if (got == want) { g_st_pass++; return; }
  g_st_fail++;
  std::lock_guard<std::mutex> lk(g_st_mtx);
  g_st_failed.push_back(what);
  fprintf(stderr, "selftest FAIL: %s\n  got:  [%s]\n  want: [%s]\n", what.c_str(), got.c_str(), want.c_str());
}
// a check this box cannot run: it does not count, and it is named under the summary
static vector<string> g_st_skipped;
static void st_skip(const string& what) {
  std::lock_guard<std::mutex> lk(g_st_mtx);
  g_st_skipped.push_back(what);
}

static void st_util() {
  st_eq(trim("  a b \n"), "a b", "trim");
  st_eq(upper("aBc"), "ABC", "upper");
  st_eq(lower("aBc"), "abc", "lower");
  st_check(split("a,b,,c", ',').size() == 4, "split keeps empties");
  st_check(split_ws("  a   b c ").size() == 3, "split_ws");
  st_check(split_lines("a\nb\n").size() == 2, "split_lines drops the final empty");
  st_check(split_lines("a\r\nb").at(0) == "a", "split_lines strips CR");
  st_eq(join({"a", "b"}, "-"), "a-b", "join");
  st_check(starts_with("abc", "ab") && !starts_with("a", "ab"), "starts_with");
  st_check(ends_with("abc", "bc") && !ends_with("c", "bc"), "ends_with");
  st_check(icontains("Hello World", "o w") && !icontains("abc", "x"), "icontains");
  st_eq(replace_all("aXbXc", "X", "--"), "a--b--c", "replace_all");
  st_check(is_digits("123") && !is_digits("12a") && !is_digits(""), "is_digits");
  st_check(to_long("42") == 42 && to_long("x", 7) == 7, "to_long");
  st_check(to_double("1.5") == 1.5, "to_double");
  st_eq(basename_of("/a/b/c/"), "c", "basename_of");
  st_eq(dirname_of("/a/b/c"), "/a/b", "dirname_of");
  st_eq(dirname_of("/a"), "/", "dirname_of root");
  st_eq(human_secs(90061), "1d 01:01:01", "human_secs");
  st_eq(tail_lines("a\nb\nc\n", 2), "b\nc\n", "tail_lines");
  st_eq(tail_lines("a\nb", 5), "a\nb", "tail_lines short");
  st_check(cap_text(string(100, 'x'), 20).size() < 100, "cap_text cuts");
  st_eq(sh_quote("a'b"), "'a'\\''b'", "sh_quote");
  st_check(fnv1a("a") != fnv1a("b"), "fnv1a");
  st_eq(fmt("%d-%s", 1, "x"), "1-x", "fmt");
  string tmp = "/tmp/omnium_st_" + std::to_string(getpid());
  st_check(write_file(tmp, "abc\n") && read_file(tmp) == "abc\n", "write_file/read_file");
  st_check(append_file(tmp, "d\n") && read_file(tmp) == "abc\nd\n", "append_file");
  st_check(file_size(tmp) == 6, "file_size");
  st_eq(read_file_from(tmp, 4), "d\n", "read_file_from: what follows the offset");
  st_eq(read_file_from(tmp, 0), "abc\nd\n", "read_file_from: offset 0 is the whole file");
  st_eq(read_file_from(tmp, 100), "", "read_file_from: an offset past the end is empty");
  st_eq(read_file_from(tmp + ".none", 3), "", "read_file_from: no file, nothing");
  st_check(same_path("/a/b/c", "/a/b/c/") && !same_path("/a/b/c", "/a/b/d"), "same_path: a slash at the end does not count");
  if (kHostMsys2) {
    string np = native_path("/dev/shm/x");
    st_check(!np.empty() && same_path("/dev/shm/x", np), "same_path: an MSYS2 path and its Windows form are one");
    string win = np;
    for (char& c : win) if (c == '/') c = '\\';
    st_check(same_path("/dev/shm/x", win + "\\") && same_path(win, upper(win)), "same_path: backslashes, a slash at the end and case do not count");
  }
  unlink(tmp.c_str());
  st_check(file_size(tmp) == -1, "file_size absent");
  CmdResult r = run_capture({"/bin/echo", "hi"});
  st_check(r.rc == 0 && r.out == "hi\n", "run_capture");
  r = run_capture({"/bin/sh", "-c", "exit 3"});
  st_check(r.rc == 3, "run_capture exit code");
  r = run_capture({"/bin/sleep", "5"}, 1);
  st_check(r.rc == 124, "run_capture timeout");
  {
    // with 0 and 1 closed the output pipe is made as 0 and 1, and the command still writes to it;
    // a child of this still single-threaded process tries both capture forms
    pid_t c = fork();
    if (c == 0) {
      close(0);
      close(1);
      bool ok = run_capture({"/bin/echo", "hi"}, 10).out == "hi\n";
      ok = run_capture_in({"/bin/cat"}, "in\n", 10, "", {}).out == "in\n" && ok;
      _exit(ok ? 0 : 1);
    }
    int ws = 0;
    st_check(c > 0 && waitpid(c, &ws, 0) == c && WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "a capture works when stdin and stdout were closed");
  }
  {
    // A child forked from a thread has only that thread's stack under MSYS2, so what it takes from the
    // caller's strings must be copied before the fork (ExecPlan): the directory, the log and the
    // environment below are strings on THIS thread's stack, which the thread that forks does not have.
    string dir = "/tmp", log = "/tmp/omnium_st_forklog_" + std::to_string(getpid());
    CmdResult tr, te;
    pid_t sp = -1;
    std::thread th([&] {
      tr = run_capture({"/bin/pwd"}, 10, dir);
      te = run_capture({"/bin/sh", "-c", "echo $OMNIUM_ST_ENV"}, 10, dir, {"OMNIUM_ST_ENV=thread"});
      sp = spawn_program({"/bin/sh", "-c", "echo $OMNIUM_ST_ENV; pwd"}, log, dir, true, {"OMNIUM_ST_ENV=spawned"});
    });
    th.join();
    st_check(tr.rc == 0 && trim(tr.out) == dir, "run_capture from a thread: the child starts in its directory [" + trim(tr.out) + "]");
    st_check(te.rc == 0 && trim(te.out) == "thread", "and gets its environment [" + trim(te.out) + "]");
    int sst = sp > 0 ? wait_pid(sp, 10000) : -1;
    st_check(sp > 0 && sst == 0 && read_file(log) == "spawned\n" + dir + "\n",
             "spawn_program from a thread: the child gets its directory, its environment and its log [" + trim(read_file(log)) + "]");
    unlink(log.c_str());
    // a bare command is looked for on the PATH the child is given, and one that is not there ends with 127
    CmdResult pr = run_capture({"sh", "-c", "echo hi"}, 10, "", {"PATH=/usr/bin:/bin"});
    st_check(pr.rc == 0 && pr.out == "hi\n", "a bare command is found on the PATH it is given");
    st_check(run_capture({"omnium_no_such_command_xyz"}, 10).rc == 127, "a command that is not there ends with 127");
    st_eq(wait_status_text(3 << 8), "exit 3", "wait_status_text: an exit");
    st_eq(wait_status_text(SIGSEGV), "killed by signal 11 (SIGSEGV)", "wait_status_text: a segmentation fault");
    st_eq(wait_status_text(SIGKILL), "killed by signal 9 (SIGKILL)", "wait_status_text: a kill");
  }
  {
    // a gdb that is not on the PATH is named as such, not as its exit code; PATH changes here,
    // before any thread is started
    string path = getenv("PATH") ? getenv("PATH") : "";
    setenv("PATH", "/nonexistent", 1);
    string o1, o2, e;
    bool ok = gdb_backtraces("/bin/true", "/nonexistent/core", o1, o2, &e);
    setenv("PATH", path.c_str(), 1);
    st_check(!ok && e == "gdb could not be run (is it installed?)", "a gdb that cannot be started is named so");
  }
  st_check(ram_used_pct() > 0 && ram_used_pct() < 100, "ram_used_pct");
  st_check(cpu_threads() >= 1, "cpu_threads");
  st_check(dir_free_bytes("/") > 0, "dir_free_bytes");
}

static void st_rng() {
  Xoshiro256pp a, b;
  a.seed(42); b.seed(42);
  st_check(a.next() == b.next(), "same seed, same stream");
  b.jump();
  st_check(a.next() != b.next(), "jump gives another stream");
  Xoshiro256pp c; c.seed(7);
  bool ok = true;
  for (int i = 0; i < 1000; i++) { uint64_t v = c.below(10); if (v >= 10) ok = false; }
  st_check(ok, "below stays in range");
  st_check(c.below(0) == 0 && c.below(1) == 0, "below 0/1");
  st_check(c.range(5, 5) == 5, "range single");
  ok = true;
  for (int i = 0; i < 1000; i++) { long v = c.range(-3, 3); if (v < -3 || v > 3) ok = false; }
  st_check(ok, "range stays in range");
  ok = true;
  for (int i = 0; i < 1000; i++) { double u = c.unit(); if (u < 0 || u >= 1) ok = false; }
  st_check(ok, "unit in [0,1)");
  st_check(!c.chance_pct(0) && c.chance_pct(100), "chance_pct ends");
  string d = c.digits(6);
  st_check(d.size() == 6 && is_digits(d) && d[0] != '0', "digits");
  vector<int> v{1, 2, 3, 4, 5, 6, 7, 8};
  c.shuffle(v);
  std::sort(v.begin(), v.end());
  st_check(v == vector<int>({1, 2, 3, 4, 5, 6, 7, 8}), "shuffle keeps the set");
  Xoshiro256pp s0 = rng_stream(0), s1 = rng_stream(1);
  st_check(s0.next() != s1.next(), "worker streams differ");
  Xoshiro256pp s0b = rng_stream(0);
  Xoshiro256pp s0c = rng_stream(0);
  st_check(s0b.next() == s0c.next(), "worker stream is repeatable");
}

static void st_config() {
  Config saved = g_cfg;
  st_check(config_set("TRIAL_SECONDS", "33") && g_cfg.trial_seconds == 33, "config_set int");
  st_check(config_set("sql_size_factor", "2.5") && g_cfg.sql_size_factor == 2.5, "config_set lower-case key");
  st_check(!config_set("NO_SUCH_KEY", "1"), "config_set unknown");
  st_check(config_dump().find("TRIAL_SECONDS=33") != string::npos, "config_dump carries the value");
  config_set("QA_DIR", "/" + string(70, 'q'));
  for (auto& l : split_lines(config_dump()))
    if (starts_with(l, "QA_DIR=")) st_check(l.find("   #") != string::npos, "a long value still leaves the three spaces the reader splits the help text on");
  g_cfg = saved;
  // a worker process loads the settings file again; the command-line settings of the omnium that
  // started it reach it through OMNIUM_SET, and so do the paths built from them
  {
    Paths saved_paths = g_paths;
    const char* had = getenv("OMNIUM_SET");
    string had_s = had ? had : "";
    setenv("OMNIUM_SET", "KEEP_PER_UID=7\nQA_DIR=/tmp/omnium_st_qa\n", 1);
    config_load(false);
    st_check(g_cfg.keep_per_uid == 7, "config_load: a setting from OMNIUM_SET wins over the file");
    st_eq(g_paths.known_bugs, "/tmp/omnium_st_qa/known_bugs.strings", "config_load: the paths follow a QA_DIR from OMNIUM_SET");
    // the derived shapes, read here because this check has the paths as a worker process builds them
    st_check(!g_paths.builds_file.empty() && ends_with(g_paths.builds_file, "/omnium.builds"), "paths builds_file");
    st_check(ends_with(g_paths.human_queue, "/omnium/HUMAN-queue") && ends_with(g_paths.ai_queue, "/omnium/AI-queue"), "paths human_queue and ai_queue");
    if (had) setenv("OMNIUM_SET", had_s.c_str(), 1); else unsetenv("OMNIUM_SET");
    g_cfg = saved;
    g_paths = saved_paths;
  }

}
// qa_checkout.sh carries the one list of framework files omnium's own clone checks out. Every path
// the binary reads under QA_DIR must match it, and so must every sibling a listed shell helper
// sources, or a box without ~/mariadb-qa runs with a file missing.
static void st_qa_checkout() {
  string script = g_paths.repo + "/qa_checkout.sh";
  if (!st_check_r(is_executable(script), "qa_checkout.sh is in the repo and executable")) return;
  st_check(read_file(g_paths.repo + "/.gitignore").find("/mariadb-qa/") != string::npos, "the omnium repo ignores its own clone dir");
  vector<string> pats;
  bool in_list = false;
  for (auto& l : split_lines(read_file(script))) {
    string s = trim(l);
    if (s == "FILES='") { in_list = true; continue; }
    if (in_list && s == "'") break;
    if (in_list && !s.empty() && s[0] == '/') pats.push_back(s.substr(1));
  }
  st_check(pats.size() >= 30, "the list is read from the script");
  auto listed = [&](const string& rel) {
    for (auto& p : pats)
      if (p.back() == '/' ? starts_with(rel, p) : fnmatch(p.c_str(), rel.c_str(), FNM_PATHNAME) == 0) return true;
    return false;
  };
  auto rel = [&](const string& abs) { return abs.substr(g_paths.qa.size() + 1); };
  vector<string> need = {"generatorcpp/generator.cpp", "generatorcpp/pools.h", "revgen/revgen.cpp", "reducercpp/reducer.cpp",
                         rel(g_paths.known_bugs), rel(g_paths.known_bugs_san), rel(g_paths.bugs_dir) + "/MDEV-1.sql",
                         rel(g_paths.regex_scan), rel(g_paths.regex_filter), rel(g_paths.regex_lastline),
                         rel(g_paths.asan_filter), rel(g_paths.ubsan_filter), rel(g_paths.tsan_filter), rel(g_paths.sql_filter), rel(g_paths.assignees),
                         "MSAN.ignorelist", "REGEX_ERRORS_FILTER.info", "filter.sql.info", "spiderpreload.sql",
                         "new_text_string.sh", "san_text_string.sh", "fallback_text_string.sh", "error_log_scan.sh",
                         "stack.sh", "testcase_prettify.sh", "extract_query.gdb", "reducercpp/stages.tbl",
                         "pquery/pquery2-md", "pquery/main-ms-ps-md.sql.tar.xz", "pquery/mysqld_options_mariadb_10.6.txt",
                         "yacc/13.1_sql_yacc.yy", "yacc/13.1_lex.h", "yacc/13.1_coldefs.txt"};
  for (auto& n : need) st_check(listed(n), "qa_checkout.sh lists " + n);
  st_check(!listed("pquery/main-ms-ps-md.sql.prev") && !listed("generatorcpp/generator"), "the big siblings stay out: pquery/ is not pulled whole, nor the framework binaries");
  // what each listed shell helper reads from its own directory: ${..SCRIPT_PWD}/<sibling>
  std::set<string> seen;
  for (auto& p : pats) {
    if (p.find('*') != string::npos || p.back() == '/') continue;
    string text = read_file(g_paths.qa + "/" + p);
    for (size_t pos = 0; (pos = text.find("SCRIPT_PWD}/", pos)) != string::npos;) {
      pos += 12;
      if (text.compare(pos, 3, "../") == 0) pos += 3;
      size_t e = pos;
      while (e < text.size() && (isalnum((unsigned char)text[e]) || strchr("_.-", text[e]))) e++;
      string sib = text.substr(pos, e - pos);
      if (!sib.empty() && seen.insert(p + " " + sib).second) st_check(listed(sib), p + " reaches for " + sib + ", which qa_checkout.sh lists");
    }
  }
}

static void st_store() {
  st_check(pid_alive(getpid()), "pid_alive self");
  st_check(!pid_alive(-1), "pid_alive invalid");
  st_check(!proc_cmdline(getpid()).empty(), "proc_cmdline");
  st_check(proc_rss_bytes(getpid()) > 0, "proc_rss_bytes");
}

static void st_registry() {
  Basedir a, b, c, m;
  st_check(basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", a), "registry parse a");
  st_check(basedir_parse_name("MD010926-mariadb-13.1.1-linux-x86_64-opt", b), "registry parse b");
  st_check(basedir_parse_name("UBASAN_MD010926-mariadb-13.1.1-linux-x86_64-dbg", c), "registry parse c");
  st_check(basedir_parse_name("MS150826-mysql-8.0.36-linux-x86_64-opt", m), "registry parse m");
  Basedir w;
  st_check(basedir_parse_name("MD120926-mariadb-13.1.1-windows-x86_64-opt", w) && w.windows && !w.dbg, "registry parse windows opt");
  st_eq(w.version, "13.1.1", "windows version");
  st_check(basedir_parse_name("MD120926-mariadb-13.1.1-windows-x86_64-dbg", w) && w.windows && w.dbg, "registry parse windows dbg");
  st_check(!a.windows, "linux build not marked windows");
  BuildEntry ea, eb, ec, em;
  st_check(entry_from_basedir(a, ea) && entry_from_basedir(b, eb) && entry_from_basedir(c, ec) && entry_from_basedir(m, em), "entry_from_basedir");
  st_eq(ea.key(), "CS 13.1 plain opt", "entry key");
  st_eq(ec.key(), "CS 13.1 UBASAN dbg", "entry key san");
  st_check(entry_newer(eb, ea) && !entry_newer(ea, eb), "entry_newer by date");
  Registry r;
  registry_sync(r, {a, m}, true, false);
  st_check(r.entries.size() == 2, "sync first import lists both");
  const BuildEntry* fm = registry_find(r, "/test/MS150826-mysql-8.0.36-linux-x86_64-opt");
  st_check(fm && !fm->test && fm->report, "MS build report only");
  const BuildEntry* fa = registry_find(r, "MD180826-mariadb-13.1.0-linux-x86_64-opt");
  st_check(fa && fa->test && fa->report, "CS build test and report");
  registry_sync(r, {a, b, c}, false, false);
  st_check(r.entries.size() == 3, fmt("sync newer replaces older: %zu entries", r.entries.size()));
  st_check(!registry_find(r, a.name) && registry_find(r, b.name) && registry_find(r, c.name), "sync replaced a by b, added c");
  bool replaced = false;
  for (auto& n : r.notices) if (starts_with(n, "new build " + b.name + " replaces " + a.name)) replaced = true;
  st_check(replaced, "sync notice replaces");
  registry_sync(r, {a, b, c}, false, false);
  st_check(r.entries.size() == 3, "sync again: older a not re-added");
  Basedir f;
  st_check(basedir_parse_name("MDEV-1234_MD010926-mariadb-13.1.1-linux-x86_64-dbg", f) && !f.tag.empty(), "feature build tagged");
  registry_sync(r, {f}, false, false);
  st_check(r.entries.size() == 3, "feature build never listed");
  string tmp = fmt("/tmp/omnium_st_builds_%d", getpid());
  st_check(registry_save(r, tmp), "registry_save");
  Registry r2;
  st_check(registry_load(r2, tmp) && r2.entries.size() == 3, "registry_load round trip");
  st_eq(registry_format(r2), registry_format(r), "registry format stable");
  st_check(registry_names(r2, true).size() == 2 && registry_names(r2, false).size() == 3, "registry_names test/report sets");
  unlink(tmp.c_str());
}

static void st_kb() {
  string line = kb_format_line("SIGSEGV|a|b|c|d", "MDEV-1");
  st_check(line.find("## MDEV-1") == 175, "kb line: ## at column 176");
  string longuid(200, 'x');
  st_eq(kb_format_line(longuid, "MDEV-2"), longuid + " ## MDEV-2", "kb line: long UID gets one blank");
  st_check(kb_uid_is_san("UBSAN|x") && kb_uid_is_san("LSAN|memory leak|a") && !kb_uid_is_san("SIGSEGV|a|b|c|d"), "kb_uid_is_san");
  st_eq(bug_key_normalize("12345"), "MDEV-12345", "key normalize digits");
  st_eq(bug_key_normalize("mdev-7"), "MDEV-7", "key normalize lower");
  st_eq(bug_key_normalize("MENT-99"), "MENT-99", "key normalize MENT");
  st_eq(bug_key_normalize("x"), "", "key normalize none");
  st_eq(uri_escape("a b|c(d)"), "a%20b%7Cc(d)", "uri_escape");
  auto urls = kb_jira_urls("scale >= 0|SIGABRT|f1|f2|f3|f4");
  st_check(urls.size() == 2 && urls[0].find("%5C%22f1%5C%22") != string::npos && urls[0].find("%5C%22f2%5C%22") != string::npos &&
           urls[0].find("%5C%22f3%5C%22") != string::npos && urls[1].find("scale%20%3E%3D%200") != string::npos, "jira urls assert");
  st_check(kb_jira_urls("SIGSEGV|f1|f2|f3|f4").size() == 1, "jira urls sigsegv: frames only");
  st_check(kb_jira_urls("ASAN|heap-use-after-free|sql/x.cc|f1|f2|f3|f4").size() == 1, "jira urls san: frames only");
  auto u2 = kb_jira_urls("SIGSEGV|ut_dbg_assertion_failed|f2|f3|f4");
  st_check(u2.size() == 1 && u2[0].find("f2%5C%22%22%20and%20text%20~%20%22%5C%22f2") != string::npos, "jira urls generic first frame skipped");
  // A Windows box has UIDs that GCC and gdb would have spelled otherwise, and Jira holds the GCC text: MSVC writes NULL as 0
  // where GCC writes __null, and a template argument as 64 where gdb writes 64u. So a 0 that == or != compares gets one URL
  // more, for the __null reading (the text with its 0 stays the URL before it), and the frames that have a template
  // argument one more, by their words. A Linux box has one spelling: its URLs are what they were.
  auto wu = kb_jira_urls("thd->free_list == 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command");
  auto mu = kb_jira_urls("a == 0 && b != 0|SIGABRT|f1|f2|f3|f4");
  auto tu = kb_jira_urls("SIGSEGV|Bitmap<64>::set_bit|sort_and_filter_keyuse|make_join_statistics|JOIN::optimize_inner");
  if (kHostMsys2) {
    st_check(wu.size() == 3 && wu[1].find("free_list%20%3D%3D%200%5C") != string::npos && wu[2].find("free_list%20%3D%3D%20__null%5C") != string::npos &&
             wu[2].find("%20or%20") == string::npos, "jira urls: a 0 compared with == is also searched as __null");
    // several 0s, and the UID cannot say which is a NULL: all of them, then each alone, in one URL
    st_check(mu.size() == 3 && mu[2].find("a%20%3D%3D%20__null%20%26%26%20b%20!%3D%20__null") != string::npos &&
             mu[2].find("a%20%3D%3D%20__null%20%26%26%20b%20!%3D%200") != string::npos && mu[2].find("a%20%3D%3D%200%20%26%26%20b%20!%3D%20__null") != string::npos &&
             mu[2].find("%20or%20") != string::npos && mu[2].find("%20or%20", mu[2].find("%20or%20") + 1) != string::npos, "jira urls: several 0s are read as __null together and one by one");
    st_check(tu.size() == 2 && tu[1].find("%5C%22Bitmap%5C%22") != string::npos && tu[1].find("%5C%22set_bit%5C%22") != string::npos &&
             tu[1].find("%5C%22make_join_statistics%5C%22") != string::npos && tu[1].find("%3C") == string::npos && tu[1].find("%20and%20") != string::npos,
             "jira urls: a frame with a template argument is also searched by its words");
  } else {
    st_check(wu.size() == 2 && mu.size() == 2 && tu.size() == 1, "jira urls: a Linux box gets the URLs it always got");
  }
  // a frameless assertion, ASSERT|<text> (what a plain assert() leaves in a Windows plugin), has no frames to search by: where the
  // fixes are taken its text is the search. The URLs Linux gets for it, by "ASSERT|text", find nothing, and stay as they are
  auto fu = kb_jira_urls("ASSERT|sp > last_savepoint()");
  auto fn = kb_jira_urls("ASSERT|thd->free_list == 0");
  if (kTakeFixes) {
    st_check(fu.size() == 1 && fu[0].find("sp%20%3E%20last_savepoint()") != string::npos && fu[0].find("ASSERT") == string::npos, "jira urls: a frameless assertion is searched by its text");
    st_check(kHostMsys2 ? fn.size() == 2 && fn[1].find("free_list%20%3D%3D%20__null") != string::npos : fn.size() == 1, "jira urls: and by its __null reading where a 0 may be one");
  } else {
    st_check(fu.size() == 2 && fn.size() == 2, "jira urls: a Linux box gets the URLs it always got for a frameless assertion");
  }
  // no variant where a 0 is no pointer test, where __null is there already, or where the UID has no assertion text
  st_check(kb_jira_urls("scale >= 0|SIGABRT|f1|f2|f3|f4").size() == 2 && kb_jira_urls("x == 0x10|SIGABRT|f1|f2|f3|f4").size() == 2 &&
           kb_jira_urls("x % 2 == 10|SIGABRT|f1|f2|f3|f4").size() == 2 && kb_jira_urls("thd->free_list == __null|SIGABRT|f1|f2|f3|f4").size() == 2 &&
           kb_jira_urls("SIGABRT|f1|f2|f3|f4").size() == 1, "jira urls: no __null reading where there is no 0 to read");
  // The known-bugs match of a Windows box reads both sides in one spelling, so the UID of a Windows run finds the line GCC and gdb
  // made, and the other way round: __null is 0, a template argument 64u is 64 and true is 1, "A, B" is "A,B", "> >" is ">>",
  // "Item *" is "Item*" and __int64 is long. A real 0 stays a 0, and a UID is never rewritten. A Linux box has one spelling,
  // so there the match stays as it was: none of these finds the other.
  {
    string kbn = fmt("/tmp/omnium_st_kbnull_%d", getpid()), save_kb = g_paths.known_bugs;
    write_file(kbn, "##### CURRENT BUGS (Search key: Mac) #####\n"
                    "thd->free_list == __null|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command   ## MDEV-22013\n"
                    "\"invalid state\" == 0|SIGABRT|ha_innobase::external_lock|handler::ha_external_lock|lock_external|mysql_lock_tables   ## MDEV-15494\n"
                    "open_tables == 0 && lock == 0 && m_reprepare_observer == __null|SIGABRT|THD::restore_backup_open_tables_state|a|b   ## MDEV-24154\n"
                    "SIGSEGV|Bitmap<64u>::set_bit|sort_and_filter_keyuse|make_join_statistics|JOIN::optimize_inner   ## MDEV-24513\n"
                    "SIGSEGV|row_search_mvcc<InnoDBPolicy<true, true> >|ha_innobase::index_read|handler::ha_index_read_map|join_read_key2   ## MDEV-41023\n"
                    "error != DB_FOREIGN_DUPLICATE_KEY|SIGABRT|ha_innobase::delete_row|handler::ha_delete_row|TABLE::delete_row<false>|TABLE::delete_row   ## MDEV-38348\n"
                    "SIGSEGV|Item_equal_iterator<List_iterator_fast, Item>::get_curr_field|Item_equal::contains|Item_field::find_item_equal|eliminate_item_equal   ## MDEV-38879\n"
                    "n < m_size|SIGABRT|Bounds_checked_array<Item*>::operator[]|Item::split_sum_func2|Item_cond::split_sum_func|JOIN::prepare   ## MDEV-40560\n"
                    "SIGSEGV|std::__atomic_base<long>::store|Atomic_relaxed<long>::store|Atomic_relaxed<long>::operator=|trx_t::commit_tables   ## MDEV-30941\n"
                    "cursor->pos_state == BTR_PCUR_IS_POSITIONED|SIGABRT|btr_pcur_get_rec|row_search_mvcc|ha_innobase::index_read|handler::index_read_map   ## MDEV-36773\n"
                    "((thd && (WSREP_PROVIDER_EXISTS_ && thd->variables.wsrep_on)) && wsrep_emulate_bin_log) || mysql_bin_log.is_open()|SIGABRT|binlog_trans_log_savepos|THD::binlog_set_stmt_begin|a|b   ## MDEV-27296\n"
                    "select_lex->select_number == (2147483647 *2U +1U) || !output|SIGABRT|JOIN::save_explain_data|JOIN::build_explain|JOIN::optimize|subselect_single_select_engine::exec   ## MDEV-36750\n"
                    "\n###### FIXED BUGS ######\n");
    g_paths.known_bugs = kbn;
    auto one = [](const KbMatch& m, const char* key) { return kb_verdict(m) == KbVerdict::Known && m.exact.size() == 1 && m.exact[0].find(key) != string::npos; };
    // each UID is the other spelling of the line named: a Windows box finds it, a Linux box does not
    struct { const char* uid; const char* key; } other[] = {
      {"thd->free_list == 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command", "MDEV-22013"},
      {"\"invalid state\" == __null|SIGABRT|ha_innobase::external_lock|handler::ha_external_lock|lock_external|mysql_lock_tables", "MDEV-15494"},
      {"open_tables == 0 && lock == 0 && m_reprepare_observer == 0|SIGABRT|THD::restore_backup_open_tables_state|a|b", "MDEV-24154"},
      {"SIGSEGV|Bitmap<64>::set_bit|sort_and_filter_keyuse|make_join_statistics|JOIN::optimize_inner", "MDEV-24513"},
      {"SIGSEGV|row_search_mvcc<InnoDBPolicy<1,1> >|ha_innobase::index_read|handler::ha_index_read_map|join_read_key2", "MDEV-41023"},
      {"SIGSEGV|row_search_mvcc<InnoDBPolicy<1,1>>|ha_innobase::index_read|handler::ha_index_read_map|join_read_key2", "MDEV-41023"},
      {"error != DB_FOREIGN_DUPLICATE_KEY|SIGABRT|ha_innobase::delete_row|handler::ha_delete_row|TABLE::delete_row<0>|TABLE::delete_row", "MDEV-38348"},
      {"SIGSEGV|Item_equal_iterator<List_iterator_fast,Item>::get_curr_field|Item_equal::contains|Item_field::find_item_equal|eliminate_item_equal", "MDEV-38879"},
      {"n < m_size|SIGABRT|Bounds_checked_array<Item *>::operator[]|Item::split_sum_func2|Item_cond::split_sum_func|JOIN::prepare", "MDEV-40560"},
      {"SIGSEGV|std::__atomic_base<__int64>::store|Atomic_relaxed<__int64>::store|Atomic_relaxed<__int64>::operator=|trx_t::commit_tables", "MDEV-30941"},
      // a Windows frame with template arguments where the list has the frame bare
      {"cursor->pos_state == BTR_PCUR_IS_POSITIONED|SIGABRT|btr_pcur_get_rec|row_search_mvcc<InnoDBPolicy<1,1> >|ha_innobase::index_read|handler::index_read_map", "MDEV-36773"},
      // a build without WSREP: the macros are (0), and the list has them expanded
      {"((0) && (0)) || mysql_bin_log.is_open()|SIGABRT|binlog_trans_log_savepos|THD::binlog_set_stmt_begin|a|b", "MDEV-27296"},
      // UINT_MAX, as MSVC and as glibc write it
      {"select_lex->select_number == 0xffffffff || !output|SIGABRT|JOIN::save_explain_data|JOIN::build_explain|JOIN::optimize|subselect_single_select_engine::exec", "MDEV-36750"},
    };
    for (auto& o : other) {
      KbMatch m = kb_search(o.uid);
      st_check(kHostMsys2 ? one(m, o.key) : m.exact.empty(), string("kb_search: ") + (kHostMsys2 ? "finds " : "on a Linux box does not find ") + o.key + " by the other spelling " + o.uid);
    }
    // the spelling the line has finds it on every box
    st_check(one(kb_search("thd->free_list == __null|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command"), "MDEV-22013"), "kb_search: the list's own spelling finds the line");
    st_check(one(kb_search("SIGSEGV|Bitmap<64u>::set_bit|sort_and_filter_keyuse|make_join_statistics|JOIN::optimize_inner"), "MDEV-24513"), "kb_search: the list's own template spelling finds the line");
    // a different assertion, number or template argument is no match on any box
    for (const char* uid : {"thd->free_list != 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command", "thd->free_list == 10|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command",
                            "SIGSEGV|Bitmap<65>::set_bit|sort_and_filter_keyuse|make_join_statistics|JOIN::optimize_inner",
                            "error != DB_FOREIGN_DUPLICATE_KEY|SIGABRT|ha_innobase::delete_row|handler::ha_delete_row|TABLE::delete_row<1>|TABLE::delete_row",
                            "SIGSEGV|std::__atomic_base<int>::store|Atomic_relaxed<int>::store|Atomic_relaxed<int>::operator=|trx_t::commit_tables",
                            "cursor->pos_state == BTR_PCUR_IS_POSITIONED|SIGABRT|btr_pcur_get_rec|other_frame<1>|ha_innobase::index_read|handler::index_read_map",
                            "((0) && (1)) || mysql_bin_log.is_open()|SIGABRT|binlog_trans_log_savepos|THD::binlog_set_stmt_begin|a|b",
                            "select_lex->select_number == 0xfffffffe || !output|SIGABRT|JOIN::save_explain_data|JOIN::build_explain|JOIN::optimize|subselect_single_select_engine::exec"})
      st_check(kb_search(uid).exact.empty(), string("kb_search: no match for ") + uid);
    string kerr;
    bool dup = !kb_add_to(kbn, "thd->free_list == 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command", "MDEV-1", &kerr);
    st_check(kHostMsys2 ? dup && starts_with(kerr, "already listed") : !dup, "kb_add_to: the other spelling of a listed assertion is a duplicate on a Windows box only");
    st_check(kb_add_to(kbn, "thd->free_list == 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|other_frame", "MDEV-2", &kerr), "kb_add_to: another frame is not a duplicate");
    g_paths.known_bugs = save_kb;
    unlink(kbn.c_str());
  }
  // A frameless assertion, ASSERT|<text>, is what a plain assert() leaves in a Windows plugin. Where the fixes are taken it counts
  // as the list's <text>|SIGABRT|frames entry when the lines that start so share one bug key; the text of several bugs is only
  // a partial match, a text that merely starts another's is none, and a Linux box keeps its verdict (none of these is found).
  {
    string kba = fmt("/tmp/omnium_st_kbassert_%d", getpid()), save_kb = g_paths.known_bugs;
    write_file(kba, "##### CURRENT BUGS (Search key: Mac) #####\n"
                    "sp > last_savepoint()|SIGABRT|federatedx_io_mysql::savepoint_set|federatedx_txn::sp_acquire|federatedx_txn::txn_begin|ha_federatedx::external_lock   ## MDEV-29178\n"
                    "sp > last_savepoint()|SIGABRT|federatedx_io_mysql::savepoint_set|federatedx_txn::sp_acquire|federatedx_txn::stmt_begin|ha_federatedx::external_lock   ## MDEV-29178\n"
                    "length > 0|SIGABRT|a|b|c|d   ## MDEV-1\n"
                    "length > 0|SIGABRT|e|f|g|h   ## MDEV-2\n"
                    "x > 0 && y|SIGABRT|a|b|c|d   ## MDEV-3\n"
                    "ASSERT|typed text   ## MDEV-5\n"
                    "\n###### FIXED BUGS ######\n"
                    "# old text|SIGABRT|a|b|c|d   ## Fixed ## MDEV-4\n");
    g_paths.known_bugs = kba;
    KbMatch one = kb_search("ASSERT|sp > last_savepoint()");
    st_check(kTakeFixes ? kb_verdict(one) == KbVerdict::Known && one.exact.size() == 2 : kb_verdict(one) == KbVerdict::NotFound, "kb_search: a frameless assertion is the list's entry for that text");
    st_check(kTakeFixes == (kb_verdict(kb_search("ASSERT|SP > LAST_SAVEPOINT()")) == KbVerdict::Known), "kb_search: and the text is read without regard to case, as grep -Fi does");
    KbMatch many = kb_search("ASSERT|length > 0");
    st_check(kTakeFixes ? kb_verdict(many) == KbVerdict::Partial && many.partial.size() == 2 && kb_verdict_text("ASSERT|length > 0", many).find("ASSERTION TEXT") != string::npos
                        : kb_verdict(many) == KbVerdict::NotFound, "kb_search: the text of two bugs is only a partial match");
    st_check(kb_verdict(kb_search("ASSERT|x > 0")) == KbVerdict::NotFound, "kb_search: a text that only starts another's is no match, with or without the |SIGABRT|");
    st_check(kTakeFixes ? kb_verdict(kb_search("ASSERT|old text")) == KbVerdict::FixedOnly : kb_verdict(kb_search("ASSERT|old text")) == KbVerdict::NotFound,
             "kb_search: an entry that is only in the list as fixed reads as fixed-only");
    st_check(kb_verdict(kb_search("ASSERT|typed text")) == KbVerdict::Known, "kb_search: a typed ASSERT| line of the list is found as it always was");
    st_check(kb_verdict(kb_search("ASSERT|nothing like it")) == KbVerdict::NotFound && kb_verdict(kb_search("sp > last_savepoint()|SIGABRT|other_a|other_b|other_c|other_d")) != KbVerdict::Known,
             "kb_search: no match for an assertion nobody listed, nor for the text with other frames");
    g_paths.known_bugs = save_kb;
    unlink(kba.c_str());
  }
  string tmp = fmt("/tmp/omnium_st_kb_%d", getpid());
  write_file(tmp, "## header\n\n##### Filter dud #####\nSIGSEGV|old|a|b|c                ## SPECIAL-1\n\n"
                  "##### CURRENT BUGS (Search key: Mac) #####\nSIGSEGV|k1|k2|k3|k4      ## MDEV-100\nINNODB_ERROR|old error   ## MDEV-101\n\n"
                  "###### FIXED BUGS ######\n# SIGSEGV|f|f|f|f ## Fixed ## MDEV-5\n");
  string err;
  st_check(kb_add_to(tmp, "SIGABRT|n1|n2|n3|n4", "MDEV-200", &err), "kb_add_to stack " + err);
  st_check(kb_add_to(tmp, "MARIADBD_ERROR|new error text", "MDEV-201", &err), "kb_add_to error " + err);
  st_check(!kb_add_to(tmp, "SIGABRT|n1|n2|n3|n4", "MDEV-200", &err) && starts_with(err, "already listed"), "kb_add_to refuses a duplicate");
  auto lines = split_lines(read_file(tmp));
  size_t h = 0; for (size_t i = 0; i < lines.size(); i++) if (lines[i] == "##### CURRENT BUGS (Search key: Mac) #####") h = i;
  st_check(h > 0 && starts_with(lines[h + 1], "SIGABRT|n1|n2|n3|n4"), "stack UID right after the header");
  st_check(h + 4 < lines.size() && starts_with(lines[h + 4], "MARIADBD_ERROR|new error text") && trim(lines[h + 5]).empty() &&
           starts_with(lines[h + 6], "###### FIXED"), "error UID at the end of the block");
  int n = kb_fixed_in(tmp, "MDEV-100");
  st_check(n == 1, fmt("kb_fixed_in moves one line: %d", n));
  lines = split_lines(read_file(tmp));
  st_eq(lines.back(), "# SIGSEGV|k1|k2|k3|k4      ## Fixed ## MDEV-100", "fixed line format and place");
  st_check(kb_fixed_in(tmp, "MDEV-10") == 0, "kb_fixed_in whole key only");
  unlink(tmp.c_str());
  KbMatch m;
  st_check(kb_verdict(m) == KbVerdict::NotFound, "verdict not found");
  m.partial = {"x"};
  st_check(kb_verdict(m) == KbVerdict::Partial, "verdict partial");
  m.exact = {"# fixed line"};
  st_check(kb_verdict(m) == KbVerdict::FixedOnly, "verdict fixed only");
  m.exact.push_back("open line");
  st_check(kb_verdict(m) == KbVerdict::KnownAndFixed, "verdict known and fixed");
  m.exact = {"open line"};
  st_check(kb_verdict(m) == KbVerdict::Known, "verdict known");
  st_eq(join(kb_keys({"a ## MDEV-1", "b ## MENT-22 and MDEV-1"}), ","), "MDEV-1,MENT-22", "kb_keys");
  string rl = fmt("/tmp/omnium_st_rl_%d", getpid());
  write_file(rl, "aaa|b.*c|d\\|e\n");
  auto alts = regex_list_read(rl);
  st_check(alts.size() == 3 && alts[2] == "d\\|e", "regex_list_read keeps an escaped bar");
  st_check(regex_list_add(rl, "new one") && !regex_list_add(rl, "new one"), "regex_list_add once");
  st_eq(trim(read_file(rl)), "aaa|b.*c|d\\|e|new one", "regex_list_add appends");
  unlink(rl.c_str());
}

// the cmake line of omnium build, for a source tree that st_build has laid out
static void st_cmake_command(const SourceInfo& si, string& err) {
  auto cm = cmake_command(si, "opt", "/test/X", &err);
  string line = join(cm, " ");
  st_check(!cm.empty() && cm[0] == "cmake" && cm[2] == "-G" && cm[3] == "Ninja", "cmake_command shape " + err);
  for (const char* want : {"-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DPLUGIN_PERFSCHEMA=YES", "-DWITH_MARIABACKUP=1", "-DWITH_ROCKSDB=1",
                           "-DWITH_SSL=bundled", "-DINSTALL_LAYOUT=STANDALONE", "-DCMAKE_INSTALL_PREFIX=/test/X"})
    st_check(line.find(want) != string::npos, string("cmake_command opt has ") + want);
  st_check(line.find("-DWITH_DEBUG=ON") == string::npos, "cmake_command opt has no WITH_DEBUG");
  line = join(cmake_command(si, "dbg", "/test/X", &err), " ");
  st_check(line.find("-DCMAKE_BUILD_TYPE=Debug") != string::npos && line.find("-DWITH_DEBUG=ON") != string::npos &&
           line.find("-DWITH_INNODB_EXTRA_DEBUG=ON") != string::npos && line.find("-DPLUGIN_PERFSCHEMA=NO") != string::npos, "cmake_command dbg flags");
  line = join(cmake_command(si, "ubasan-dbg", "/test/X", &err), " ");
  st_check(line.find("-DWITH_ASAN=ON") != string::npos && line.find("-fsanitize=address,undefined") != string::npos &&
           line.find("-DWITH_MARIABACKUP=0") != string::npos && line.find("-O1 -fPIC") != string::npos && line.find("-shared-libasan") != string::npos, "cmake_command ubasan flags");
  if (is_executable("/usr/bin/clang-20") && dir_exists("/MSAN_libs")) {
    line = join(cmake_command(si, "msan-opt", "/test/X", &err), " ");
    st_check(line.find("-DWITH_MSAN=ON") != string::npos && line.find("clang-20") != string::npos && line.find("-DWITH_SSL=/MSAN_libs") != string::npos &&
             line.find("-fsanitize-ignorelist=") != string::npos && line.find("-DWITH_ROCKSDB=0") != string::npos &&
             line.find("-DPLUGIN_PERFSCHEMA=NO") != string::npos, "cmake_command msan flags");
  }
}

static void st_build() {
  string tree = fmt("/tmp/omnium_st_tree_%d", getpid());
  mkdirs(tree + "/support-files/rpm");
  mkdirs(tree + "/storage/rocksdb");
  write_file(tree + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\nSERVER_MATURITY=alpha\n");
  SourceInfo si;
  string err;
  st_check(source_info(tree, si, &err), "source_info cs " + err);
  st_check(si.vendor == Vendor::MariaDB && !si.es && si.maturity && si.has_rocksdb, "source_info cs fields");
  st_eq(si.version, "13.1.0", "source_info version");
  st_eq(si.series, "13.1", "source_info series");
  st_eq(build_name(si, "opt", "", "050926"), "MD050926-mariadb-13.1.0-linux-x86_64-opt", "build_name opt");
  st_eq(build_name(si, "ubasan-dbg", "", "050926"), "UBASAN_MD050926-mariadb-13.1.0-linux-x86_64-dbg", "build_name ubasan dbg");
  st_eq(build_name(si, "msan-opt", "MDEV-1234", "050926"), "MSAN_MDEV-1234_MD050926-mariadb-13.1.0-linux-x86_64-opt", "build_name tagged msan");
  Basedir b;
  st_check(basedir_parse_name(build_name(si, "msan-opt", "MDEV-1234", "050926"), b) && b.tag == "MDEV-1234" && b.flavour == Flavour::MSAN && !b.dbg, "build_name parses back");
  write_file(tree + "/support-files/rpm/mariadb-enterprise.spec.in", "x");
  write_file(tree + "/VERSION", "MYSQL_VERSION_MAJOR=12\nMYSQL_VERSION_MINOR=3\nMYSQL_VERSION_PATCH=2\nMYSQL_VERSION_EXTRA=-1\nSERVER_MATURITY=stable\n");
  st_check(source_info(tree, si, &err) && si.es, "source_info es");
  st_eq(build_name(si, "dbg", "", "050926"), "EMD050926-mariadb-12.3.2-1-linux-x86_64-dbg", "build_name es");
  write_file(tree + "/VERSION", "MYSQL_VERSION_MAJOR=8\nMYSQL_VERSION_MINOR=0\nMYSQL_VERSION_PATCH=36\nMYSQL_VERSION_EXTRA=\n");
  st_check(source_info(tree, si, &err) && si.vendor == Vendor::MySQL, "source_info mysql");
  st_eq(build_name(si, "opt", "", "050926"), "MS050926-mysql-8.0.36-linux-x86_64-opt", "build_name mysql");
  write_file(tree + "/VERSION", "MYSQL_VERSION_MAJOR=8\nMYSQL_VERSION_MINOR=0\nMYSQL_VERSION_PATCH=36\nMYSQL_VERSION_EXTRA=-28\n");
  st_check(source_info(tree, si, &err) && si.vendor == Vendor::Percona, "source_info percona");
  st_eq(build_name(si, "opt", "", "050926"), "PS050926-percona-server-8.0.36-28-linux-x86_64-opt", "build_name percona");
  write_file(tree + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\nSERVER_MATURITY=alpha\n");
  unlink((tree + "/support-files/rpm/mariadb-enterprise.spec.in").c_str());
  st_check(source_info(tree, si, &err) && !si.es, "source_info cs again");
  // omnium build drives clang and the Linux build scripts' cmake lines: it is not ported to Windows
  if (kHostMsys2) st_skip("cmake_command: omnium build is not ported to Windows");
  else st_cmake_command(si, err);
  st_eq(tag_from_branch("13.1"), "", "tag_from_branch version");
  st_eq(tag_from_branch("12.3-enterprise"), "", "tag_from_branch es version");
  st_eq(tag_from_branch("main"), "", "tag_from_branch main");
  st_eq(tag_from_branch("bb-13.1-MDEV-1234_x"), "bb-13.1-MDEV-1234-x", "tag_from_branch feature");
  st_check(flavour_valid("ubasan-opt") && !flavour_valid("san"), "flavour_valid");
  // the per-pid directories a killed run leaves in /tmp: the sweep takes the ones whose process is
  // gone and leaves everything else, so it can never reach into a run that is still going
  {
    string base = fmt("/tmp/omnium_st_sweep_%d", getpid());
    string dead = base + "/x2147480000", mine = base + "/x" + std::to_string(getpid()), other = base + "/xlater";
    for (auto& d : {dead, mine, other}) { mkdirs(d); write_file(d + "/f", "x\n"); }
    // the gdb log, the shutdown log and the report's testcase carry more after the pid
    string dead_file = base + "/x2147480000_3.log", mine_file = base + "/x" + std::to_string(getpid()) + "_1.log", digits_word = base + "/x2147480000abc";
    for (auto& f : {dead_file, mine_file, digits_word}) write_file(f, "x\n");
    int n = sweep_stale_tmp(base + "/x");
    st_check(n == 2, fmt("the stale sweep took two (%d)", n));
    st_check(!dir_exists(dead), "the directory whose process is gone");
    st_check(!file_exists(dead_file), "and the file whose process is gone, with _<n>.log after the pid");
    st_check(dir_exists(mine) && file_exists(mine_file), "and left this process's own alone");
    st_check(dir_exists(other), "and left a name that is not a pid alone");
    st_check(file_exists(digits_word), "and a pid followed by letters");
    st_check(sweep_stale_tmp(base + "/no_such_prefix") == 0, "and an empty sweep is not an error");
    remove_tree(base);
  }
  remove_tree(tree);
}

// the MTR conversion, text rules only (no server): guards, options, delimiter, cleanup, backticks
static void st_mtr() {
  MtrTest t;
  string err;
  string sql =
    "SET GLOBAL innodb_stats_persistent=0;\n"
    "CREATE DATABASE d1;\n"
    "CREATE TABLE `t1` (a INT) PARTITION BY HASH(a) PARTITIONS 2;\n"
    "CREATE OR REPLACE TABLE t2 (a INT) ENGINE=InnoDB;\n"
    "CREATE SEQUENCE s1;\n"
    "CREATE USER u1@localhost;\n"
    "CREATE PROCEDURE p1() BEGIN SELECT 1; SELECT 2; END;\n"
    "SET debug_dbug='+d,crash_now';\n"
    "SELECT 'a;b' FROM t1;\n";
  st_check(mtr_make(sql, "--sql_mode= --log-bin --binlog-format=ROW --innodb_lock_wait_timeout=3 --skip-name-resolve", nullptr, "", t, &err), "mtr_make runs");
  const string& x = t.test;
  st_check(x.find("--source include/have_log_bin.inc\n") != string::npos, "mtr log-bin include");
  st_check(x.find("--source include/have_binlog_format_row.inc\n") != string::npos, "mtr binlog format include");
  st_check(x.find("--source include/have_innodb.inc\n") != string::npos && x.find("--source include/have_partition.inc\n") != string::npos &&
           x.find("--source include/have_sequence.inc\n") != string::npos && x.find("--source include/have_debug.inc\n") != string::npos, "mtr guards from the SQL");
  st_check(x.find("SET sql_mode='';\n") != string::npos, "mtr sql_mode inline");
  st_check(x.find("CREATE TABLE t1 (a INT)") != string::npos, "mtr backticks dropped");
  st_check(x.find("DELIMITER |;\nCREATE PROCEDURE p1() BEGIN SELECT 1; SELECT 2; END|\nDELIMITER ;|\n") != string::npos, "mtr delimiter block");
  st_check(x.find("SELECT 'a;b' FROM t1;\n") != string::npos && x.find("DELIMITER |;\nSELECT") == string::npos, "mtr semicolon in a literal");
  {
    bool said = false;
    for (auto& n : t.notes) if (n.find("no replay") != string::npos) said = true;
    st_check(said, "with no build to replay on, the test says which checks did not run");
  }
  size_t d_user = x.find("DROP USER IF EXISTS u1@localhost;"), d_db = x.find("DROP DATABASE IF EXISTS d1;"), d_p = x.find("DROP PROCEDURE IF EXISTS p1;");
  st_check(d_user != string::npos && d_db != string::npos && d_p != string::npos && d_p < d_user && d_user < d_db, "mtr cleanup in reverse order");
  st_check(x.find("SET GLOBAL innodb_stats_persistent=DEFAULT;") != string::npos, "mtr SET GLOBAL restored");
  st_check(t.opt.find("--skip-name-resolve") != string::npos && t.opt.find("--innodb_lock_wait_timeout=3") != string::npos, "mtr .opt without a replay");
  st_check(t.debug_only, "mtr debug only");
  st_check(!t.replayed && t.gate_kind.empty(), "mtr no replay without a build");
  st_check(!mtr_make("# only a comment\n", "", nullptr, "", t, &err), "mtr_make rejects empty SQL");
  st_check(mtr_make("SELECT ROW_NUMBER() OVER (PARTITION BY a) FROM t1;\nSELECT SUM(a) OVER w FROM t1 WINDOW w AS ( PARTITION BY a);\n", "", nullptr, "", t, &err) &&
           t.test.find("have_partition") == string::npos, "mtr: a window's PARTITION BY needs no partitioning");
  st_check(mtr_make("CREATE TABLE t1 (a INT) PARTITION BY RANGE (a) SUBPARTITION BY HASH(a) (PARTITION p0 VALUES LESS THAN (5));\n", "", nullptr, "", t, &err) &&
           t.test.find("--source include/have_partition.inc") != string::npos, "mtr: a table's PARTITION BY does");
}

// The verdict the runner's own output gives. The blocks below are what mariadb-test-run.pl printed
// on a real run: the per-test log check names the offending lines, and the scan after shutdown
// repeats what it found there.
static void st_mtr_gate_units() {
  {
    // the replay's client caps one statement's answer, twice as long on a sanitizer build
    MYSQL* m = mysql_init(nullptr);
    unsigned plain = replay_client_options(m, false), rd = 0, wr = 0, co = 0;
    mysql_get_optionv(m, MYSQL_OPT_READ_TIMEOUT, &rd);
    mysql_get_optionv(m, MYSQL_OPT_WRITE_TIMEOUT, &wr);
    mysql_get_optionv(m, MYSQL_OPT_CONNECT_TIMEOUT, &co);
    st_check(plain == 300 && rd == 300 && wr == 300 && co == 30, "the replay client waits 300 s for an answer and 30 s for the connect");
    st_check(replay_client_options(m, true) == 600, "and 600 s on a sanitizer build");
    mysql_close(m);
  }
  const string head = "worker[01] Using MTR_BUILD_THREAD 300, with reserved ports 19000..19029\n";
  const string tail = " - saving '/dev/shm/Omtr1_x/var/log/main.omnium_x/' to '/dev/shm/Omtr1_x/var/log/main.omnium_x/'\n"
                      "--------------------------------------------------------------------------\n"
                      "The servers were restarted 0 times\n"
                      "Completed: Failed 1/1 tests, 0.00% were successful.\n";
  const string ubsan = "2026-09-12 13:00:00 0 [Note] /test/13.1/bin/mariadbd: /test/13.1/sql/item_func.cc:1234:5: runtime error: "
                       "signed integer overflow: 9223372036854775807 + 1 cannot be represented in type 'long long int'\n";
  MtrVerdict v;
  vector<string> sup;
  // a sanitizer line that leaves the server up: the log check fails the test, and that is the bug
  mtr_parse_verdict(head +
    "main.omnium_x                            [ fail ]  Found warnings/errors in server log file!\n"
    "        Test ended at 2026-09-12 13:48:34\n"
    "line\n" + ubsan +
    "^ Found warnings in /dev/shm/Omtr1_x/var/log/mysqld.1.err\n"
    "ok\n\n" + tail, 1, "omnium_x", false, v, &sup);
  st_check(v.fail && v.log_error && v.gate && !v.pass, "a sanitizer line in the server log fails the test the bug's way");
  st_check(sup.empty(), "and a line that carries the bug is never suppressed");
  // the same block, benign this time: the test's own SQL logs the lines, so they are suppressed
  mtr_parse_verdict(head +
    "main.omnium_x                            [ fail ]  Found warnings/errors in server log file!\n"
    "        Test ended at 2026-09-12 13:49:10\n"
    "line\n"
    "2026-09-12 13:00:00 0 [Warning] Sort aborted, host: localhost, user: root, thread: 5\n"
    "2026-09-12 13:00:00 0 [ERROR] Incorrect definition of table mysql.event: expected column 'definer' at position 3\n"
    "^ Found warnings in /dev/shm/Omtr1_x/var/log/mysqld.1.err\n"
    "ok\n\n" + tail, 1, "omnium_x", false, v, &sup);
  st_check(v.fail && !v.log_error && !v.gate, "a log line the testcase itself makes is not the bug's way of failing");
  st_check(sup.size() == 2 && sup[0] == "call mtr.add_suppression(\"Sort aborted, host: localhost, user: root, thread: 5\");",
           "and each one becomes an mtr.add_suppression line without its timestamp");
  st_check(sup.size() == 2 && sup[1] == "call mtr.add_suppression(\"Incorrect definition of table mysql\\\\.event: expected column 'definer' at position 3\");",
           "with the regexp characters escaped");
  // the scan after shutdown catches what the per-test check ran too early to see
  mtr_parse_verdict(head +
    "main.omnium_x                            [ pass ]     11\n"
    "***Warnings generated in error logs during shutdown after running tests: main.omnium_x\n\n" + ubsan + "\n" + tail,
    1, "omnium_x", false, v, &sup);
  st_check(v.fail && v.log_error && v.gate, "a sanitizer line found only at shutdown still fails the run");
  // a clean run
  mtr_parse_verdict(head + "main.omnium_x                            [ pass ]     11\n" + tail, 0, "omnium_x", false, v, &sup);
  st_check(v.pass && !v.fail && !v.gate && sup.empty(), "a run the runner passes is a pass");
  // the column the suppression goes in is VARCHAR(255)
  mtr_parse_verdict(head +
    "main.omnium_x                            [ fail ]  Found warnings/errors in server log file!\n"
    "line\n2026-09-12 13:00:00 0 [ERROR] " + string(400, 'x') + "\n"
    "^ Found warnings in /dev/shm/Omtr1_x/var/log/mysqld.1.err\n" + tail, 1, "omnium_x", false, v, &sup);
  st_check(sup.size() == 1 && sup[0].size() < 280, "a long log line is cut to fit the suppression column");
}


// ---------------------------------------------------------------------------------------------
// The verbs, called in this process. Every one runs against a fixture under a temporary directory
// or against a build that is already on the box; nothing here files a ticket, sends mail, or
// touches a real workdir. Output goes to /dev/null so the check list stays readable.
// ---------------------------------------------------------------------------------------------
namespace {
// A sanitizer writes its report to stderr. Parking stderr as well would hide it, and a build that
// stops on a report would then end with no output at all, so a sanitizer build keeps stderr.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#    define OMNIUM_KEEP_STDERR 1
#  endif
#endif
struct Quiet {                                   // stdout parked while a verb runs
  int o = -1, e = -1;
  Quiet() {
    fflush(stdout); fflush(stderr);
    o = dup(1);
    int n = open("/dev/null", O_WRONLY);
    if (n >= 0) {
      dup2(n, 1);
#ifndef OMNIUM_KEEP_STDERR
      e = dup(2);
      dup2(n, 2);
#endif
      close(n);
    }
  }
  ~Quiet() {
    fflush(stdout); fflush(stderr);
    if (o >= 0) { dup2(o, 1); close(o); }
    if (e >= 0) { dup2(e, 2); close(e); }
  }
};
int quiet_call(int (*fn)(const Args&), const Args& a) {
  Quiet q;
  return fn(a);
}
string st_tmp();
// what a verb printed on stdout, for a check on its words; stderr is parked as quiet_call parks it
string call_output(int (*fn)(const Args&), const Args& a, int* rc = nullptr) {
  string path = st_tmp() + "/call_output.txt";
  int r;
  {
    Quiet q;
    int f = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (f >= 0) { dup2(f, 1); close(f); }
    r = fn(a);
    fflush(stdout);
  }
  if (rc) *rc = r;
  string out = read_file(path);
  unlink(path.c_str());
  return out;
}
string st_tmp() {
  string d = "/tmp/omnium_selftest_" + std::to_string(getpid());
  mkdirs(d);
  return d;
}
// an error log with an assert and a stack, as a debug server writes it
string fixture_errlog() {
  return
    "2026-09-05  5:00:00 0 [Note] /test/x/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n"
    "2026-09-05  5:00:01 0 [Note] InnoDB: Buffer pool(s) load completed at 260905  5:00:01\n"
    "2026-09-05  5:00:02 4 [ERROR] mariadbd: Index for table 't1' is corrupt; try to repair it\n"
    "mariadbd: /test/13.1/sql/item_func.cc:1287: virtual void Item_func_additive_op::result_precision(): Assertion `arg2_int >= 0' failed.\n"
    "260905  5:00:02 [ERROR] mysqld got signal 6 ;\n"
    "Query (0x7f0000000000): SELECT 0-SUM(COALESCE(b'0'))/0 AS c\n"
    "Server version: 13.1.0-MariaDB-debug-log\n"
    "stack_bottom = 0x7f00 thread_stack 0x100000\n"
    "??:0(Item_func_additive_op::result_precision)[0x55c0d0]\n"
    "??:0(Item_num_op::fix_length_and_dec_decimal)[0x55c0e0]\n";
}
string fixture_san_log() {
  return
    "==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x606000000040 at pc 0x0000004a1b2c bp 0x7ffd0 sp 0x7ffc8\n"
    "READ of size 8 at 0x606000000040 thread T3\n"
    "    #0 0x4a1b2c in Item_func_case::val_int() /test/13.1/sql/item_cmpfunc.cc:2544:10\n"
    "    #1 0x4a2000 in Item::val_int_result() /test/13.1/sql/item.h:1200:12\n"
    "    #2 0x4a3000 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:22000:5\n"
    "SUMMARY: AddressSanitizer: heap-use-after-free /test/13.1/sql/item_cmpfunc.cc:2544:10 in Item_func_case::val_int()\n";
}
// a trial directory the way a run leaves one, minus the datadir and the core
string fixture_trial(const string& root, long n, const string& errlog_text, const string& uid) {
  string wd = root + "/O999999";
  string td = wd + "/" + std::to_string(n);
  mkdirs(td + "/log");
  write_file(wd + "/omnium.ledger", fmt("%lld run created 2026-09-05 05:00:00 seed 1\n%lld trial %ld build area saved-new %s\n",
                                        (long long)now_s(), (long long)now_s(), n, uid.c_str()));
  write_file(td + "/log/master.err", errlog_text);
  write_file(td + "/MYBUG", uid + "\n");
  write_file(td + "/MYEXTRA", "--sql_mode= --log_bin\n");
  write_file(td + "/MYSAFE", "--no-defaults --loose-innodb-buffer-pool-in-core-dump=0\n");
  write_file(td + "/default.node.tld_thread-0.sql", "SELECT 1;\nCREATE TABLE t1 (a INT);\nSELECT 0-SUM(COALESCE(b'0'))/0 AS c;\n");
  write_file(td + "/default.node.tld_thread-0.sql_out", "# mysqld options required for replay: --sql_mode= --log_bin\nCREATE TABLE t1 (a INT);\nSELECT 0-SUM(COALESCE(b'0'))/0 AS c;\n");
  write_file(td + "/default.node.tld_general.log", "- Read 3 lines (60 bytes) from x.sql\n* NODE SUMMARY: 1/3 queries failed, (66.67% were successful)\n");
  return td;
}
}  // namespace

// the read-only verbs: they print and return, and every one has to survive a plain call
static void st_verbs_read_only() {
  string tmp = st_tmp();
  st_check(quiet_call(cmd_help, {}) == 0, "help");
  st_check(quiet_call(cmd_help, {"report"}) == 0, "help of one verb");
  st_check(quiet_call(cmd_version, {}) == 0, "version");
  st_check(quiet_call(cmd_config, {}) == 0, "config list");
  st_check(quiet_call(cmd_config, {"DATA_DIR"}) == 0, "config one key");
  st_check(quiet_call(cmd_config, {"NO_SUCH_KEY"}) != 0, "config rejects an unknown key");
  st_check(quiet_call(cmd_areas, {}) == 0, "areas");
  st_check(quiet_call(cmd_builds, {}) == 0, "builds list");
  st_check(quiet_call(cmd_status, {}) == 0, "status");
  st_check(quiet_call(cmd_inbox, {"--dir", tmp + "/inbox"}) == 0, "inbox on an empty dir");
  {
    int crc = -1;
    string rcf = call_output(cmd_cli, {"--rcfile-only"}, &crc);
    st_check(crc == 0, "cli rcfile");
    st_check(rcf.find("\ncomplete -F _omnium_complete omnium\n") != string::npos, "the cli completes omnium only, not the framework's o");
    st_check(rcf.find("\nomnium_pr() {") != string::npos && rcf.find("\npr() { omnium_pr \"$@\"; }\n") != string::npos, "and its pr shows the run it is in");
  }
  st_check(quiet_call(cmd_mail, {"nobody@example.invalid", "--dry-run"}) == 0, "mail dry run");
  {
    // the two searches on lists of their own, so what the real lists hold does not matter
    string save_kb = g_paths.known_bugs, save_kbsan = g_paths.known_bugs_san;
    g_paths.known_bugs = tmp + "/kbs.strings";
    g_paths.known_bugs_san = tmp + "/kbs.strings.SAN";
    write_file(g_paths.known_bugs, "SIGSEGV|Item_func_additive_op::result_precision|a|b|c           ## MDEV-100\n");
    write_file(g_paths.known_bugs_san, "ASAN|heap-use-after-free|sql/item.cc|f|g|h           ## MDEV-101\n");
    int krc = -1;
    st_check(call_output(cmd_kbs, {"item_func_additive_op"}, &krc).find("MDEV-100") != string::npos && krc == 0, "kbs prints the line that matches, in any case");
    st_check(quiet_call(cmd_kbs, {"no_such_text_at_all"}) == 1, "and says no when no line matches");
    st_check(call_output(cmd_kbsa, {"heap-use-after-free"}, &krc).find("MDEV-101") != string::npos && krc == 0, "kbsa searches the sanitizer list");
    g_paths.known_bugs = save_kb;
    g_paths.known_bugs_san = save_kbsan;
  }
  st_check(quiet_call(cmd_kb, {"search", "no|such|uid|at|all"}) != 0, "kb search of an unknown UID");
  st_check(quiet_call(cmd_kba, {"search", "no|such|uid|at|all"}) != 0, "kba search of an unknown UID");
  st_check(quiet_call(cmd_trial, {}) == 2, "trial without a number");
  st_check(quiet_call(cmd_ldd, {tmp + "/nowhere"}) != 0, "ldd on a dir that is not there");
  st_check(quiet_call(cmd_reduce, {}) == 2, "reduce without a trial");
  st_check(quiet_call(cmd_report, {}) == 2, "report without a trial");
  st_check(quiet_call(cmd_matrix, {}) == 2, "matrix without a testcase");
  st_check(quiet_call(cmd_mtr, {}) == 2, "mtr without a testcase");
  st_check(quiet_call(cmd_adopt, {}) == 2, "adopt without a workdir");
  st_check(quiet_call(cmd_tui, {"O000000"}) != 0, "tui on a run that is not there");
  // the ldd port: a binary named mariadbd and its libraries, gathered
  string ldir = tmp + "/ldd";
  mkdirs(ldir);
  if (file_exists("/bin/true")) {
    copy_file("/bin/true", ldir + "/mariadbd");
    chmod((ldir + "/mariadbd").c_str(), 0755);
    vector<string> files;
    string e;
    st_check(ldd_copy(ldir + "/mariadbd", ldir, "", &files, &e), "ldd_copy runs");
    st_check(!files.empty(), "ldd_copy took at least one library");
    st_check(quiet_call(cmd_ldd, {ldir}) == 0, "ldd verb");
    // a basedir named as the dir: its own binary, gathered with the libraries in the basedir
    string lbd = tmp + "/ldd_basedir";
    mkdirs(lbd + "/bin");
    copy_file("/bin/true", lbd + "/bin/mariadbd");
    chmod((lbd + "/bin/mariadbd").c_str(), 0755);
    st_check(quiet_call(cmd_ldd, {lbd}) == 0 && file_exists(lbd + "/mariadbd"), "ldd takes a basedir for the dir");
  }
  st_check(!ldd_copy(tmp + "/no_binary_here", ldir, "", nullptr, nullptr), "ldd_copy says no when the binary is missing");
  remove_tree(tmp);
}

// the UniqueID chain and the report, on fixtures: no server, no core
static void st_fixtures() {
  string tmp = st_tmp();
  string uid = "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y";
  string td = fixture_trial(tmp, 1, fixture_errlog(), uid);
  // the error-log scan modes: what counts is that this port says what the shell script says
  string script = script_path("error_log_scan.sh");
  for (const char* mode : {"errors", "lastline", "top", "check"}) {
    string mine;
    bool ok = els_run(mode, {td + "/log/master.err"}, false, mine, nullptr);
    if (!file_exists(script)) { st_skip(string("els ") + mode + " against error_log_scan.sh, which is not in " + g_paths.qa); continue; }
    CmdResult sh = run_capture({script, mode, td + "/log/master.err"}, 300);
    st_check((sh.rc == 0) == ok, string("els ") + mode + ": the same verdict as error_log_scan.sh");
    st_eq(trim(mine), trim(sh.out), string("els ") + mode + ": the same output as error_log_scan.sh");
  }
  // a log with a byte the locale cannot decode: one grep in the script's chain says so on stderr,
  // and the parity verb has to read that as noise, not as an answer
  if (file_exists(script)) {
    string hd = tmp + "/highbyte";
    mkdirs(hd + "/9/log");
    string hl = string("2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n") +
                "safe_mutex: Found wrong usage of mutex 'LOCK_\xc0\xf8" "a' and 'LOCK_b'\n";
    write_file(hd + "/9/log/master.err", hl);
    CmdResult sh = run_capture({script, "errors", "./9/log/master.err"}, 300, hd);
    string mine;
    bool ok = els_run("errors", {hd + "/9/log/master.err"}, false, mine, nullptr);
    // Grep 3.5 and later send the note to stderr and the scan fails. The grep 3.0 of MSYS2 prints
    // "Binary file (standard input) matches" on stdout, so the script answers UNTYPED instead.
    if (kHostMsys2)
      st_skip("the shell scan drops the whole answer on a log line with a byte the locale cannot "
              "read (MSYS2's grep 3.0 puts its note on stdout, so the script answers UNTYPED)");
    else
      st_check(sh.rc == 1 && sh.out.find("binary file matches") != string::npos &&
               sh.out.find("MUTEX_ERROR") == string::npos,
               "the shell scan drops the whole answer on a log line with a byte the locale "
               "cannot read");
    st_check(ok && starts_with(trim(mine), "MUTEX_ERROR|safe_mutex:"), "omnium reads that line all the same [" + trim(mine) + "]");
  }
  // the fallback chain on a log with no core
  string ferr;
  string fb = uid_fallback(td + "/log/master.err", &ferr);
  st_eq(fb, "FALLBACK|arg2_int >= 0", "uid_fallback takes the assertion of a log with no core");
  // a sanitizer log
  string sd = fixture_trial(tmp, 2, fixture_san_log(), "ASAN|heap-use-after-free|Item_func_case::val_int|Item::val_int_result|sub_select|x");
  string serr;
  string san = uid_san(capped_logs({sd + "/log/master.err"}), &serr);
  st_check(san.find("ASAN") != string::npos, "uid_san reads an ASAN report");
  // aggregate takes the trial from a ./<trial>/ path, the way pquery-results.sh gives it the logs
  if (!file_exists(script)) st_skip("els aggregate against error_log_scan.sh, which is not in " + g_paths.qa);
  else {
    string ad = tmp + "/aggregate", start = "2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n";
    mkdirs(ad + "/3/log");
    mkdirs(ad + "/4/log");
    write_file(ad + "/3/log/master.err", start + "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n"
                                                 "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n");
    write_file(ad + "/4/log/master.err", start + "safe_mutex: Found wrong usage of mutex 'LOCK_a' and 'LOCK_b'\n"
                                                 "mariadbd: /test/13.1/storage/innobase/btr/btr0cur.cc:1000: void f(): Assertion `x >= 0' failed.\n");
    string cwd0 = abs_path(".");
    if (chdir(ad.c_str()) == 0) {
      string mine;
      bool ok = els_run("aggregate", {"./3/log/master.err", "./4/log/master.err"}, false, mine, nullptr);
      CmdResult sh = run_capture({script, "aggregate", "./3/log/master.err", "./4/log/master.err"}, 300);
      st_check(!trim(sh.out).empty() && (sh.rc == 0) == ok, "els aggregate: the same verdict as error_log_scan.sh");
      st_eq(trim(mine), trim(sh.out), "els aggregate: the same rows as error_log_scan.sh");
      if (chdir(cwd0.c_str()) != 0) { /* the tmp dir goes anyway */ }
    }
  }
  // the UID of a whole trial dir, no core to wait for
  UidResult r;
  UidOptions o;
  o.wait_core = false;
  uid_for_dir(td, r, o);
  st_eq(r.uid, "MARIADBD_ERROR|mariadbd: Index for table table is corrupt; try to repair it", "uid_for_dir with no core: the typed error line comes first, as in new_text_string.sh");
  // CORE_DIR=data: the datadir, and the core in it, sit outside the trial dir the chain reads
  {
    string elsewhere = tmp + "/elsewhere_core";
    write_file(elsewhere, "not a real core");
    UidResult rc;
    UidOptions oc;
    oc.wait_core = false;
    oc.core = elsewhere;
    uid_for_dir(td, rc, oc);
    st_eq(rc.core, elsewhere, "uid_for_dir works on the core it is given");
  }
  // the known-bug lists
  KbMatch m = kb_search(uid);
  st_check(!kb_verdict_text(uid, m).empty(), "kb_verdict_text");
  st_check(!kb_jira_urls(uid).empty(), "kb_jira_urls");
  // in the trial dir, the identification verbs
  string cwd = abs_path(".");
  if (chdir(td.c_str()) == 0) {
    int vrc = -1;
    st_check(trim(call_output(cmd_t, {}, &vrc)) == r.uid && vrc == 0, "t in a trial dir prints its UID");
    st_check(starts_with(call_output(cmd_tt, {}, &vrc), r.uid + "\n----- String Scan -----\n") && vrc == 0, "tt adds the known-bug scan");
    st_check(call_output(cmd_sts, {}, &vrc).empty() && vrc == 0, "sts in a trial dir with no sanitizer report prints nothing");
    st_check(trim(call_output(cmd_fts, {"log/master.err"}, &vrc)) == fb && vrc == 0, "fts reads a log given by a relative path");
    write_file(tmp + "/scan_rel.err", "2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n"
                                      "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n");
    st_check(trim(call_output(cmd_els, {"errors", "../../scan_rel.err"}, &vrc)) == "MUTEX_ERROR|safe_mutex: Found wrong usage of mutex X and Y" && vrc == 0,
             "and so does els");
    st_check(quiet_call(cmd_trial, {"1"}) != 0, "trial verb inside the trial dir has no sub-trial");
    if (chdir(cwd.c_str()) != 0) { /* the tmp dir goes anyway */ }
  }
  st_check(quiet_call(cmd_trial, {dirname_of(td), "1"}) == 0, "trial verb on the workdir");
  st_check(quiet_call(cmd_adopt, {dirname_of(td)}) == 0, "adopt lists the fixture trials");
  st_check(quiet_call(cmd_status, {dirname_of(td)}) == 0, "status on a fixture workdir");
  // the reduction plan, without running a reducer
  st_check(quiet_call(cmd_reduce, {dirname_of(td), "1", "--plan"}) == 1, "reduce --plan on a trial that names no build says so");
  // a report with no matrix and no MTR: the header fields and the body come from the fixture alone
  string inbox = tmp + "/inbox";
  int rc = quiet_call(cmd_report, {dirname_of(td), "1", "--no-matrix", "--no-mtr", "--out", inbox});
  st_check(rc == 0, "report on a fixture");
  string rp = inbox + "/O999999_bug1.report";
  string text = read_file(rp);
  st_check(!text.empty(), "the report file is written");
  auto kv = report_header(text, nullptr);
  st_eq(report_field(kv, "UID"), uid, "the report carries the UID");
  st_check(text.find("Bug Detection Matrix") != string::npos, "the report always carries a matrix block");
  st_check(text.find("{code:sql}") != string::npos, "the report carries the testcase");
  JiraFields f;
  string ferr2;
  st_check(report_to_fields(text, f, &ferr2), "report_to_fields");
  st_check(!f.summary.empty() && !f.description.empty(), "the ticket fields are filled");
  st_check(f.project == "MDEV" || f.project == "MENT", "the project is one of the two");
  string payload = jira_create_payload(f);
  st_check(payload.find("\"summary\"") != string::npos && payload.find("\"project\"") != string::npos, "the Jira payload has the fields");
  JsonValue jv;
  st_check(json_parse(payload, jv), "the payload parses back as JSON");
  // the inbox reads it, and a dry run builds the payload without filing anything
  st_check(quiet_call(cmd_inbox, {"--dir", inbox}) == 0, "inbox lists the item");
  quiet_call(cmd_inbox, {"--dir", inbox, "--dry-run", "--file", "O999999_bug1"});
  st_check(file_exists(inbox + "/O999999_bug1.preview"), "the dry run leaves the payload to read");
  st_check(!file_exists(inbox + "/O999999_bug1.filed"), "the dry run files nothing");
  // the MTR form of that testcase
  st_check(quiet_call(cmd_mtr, {dirname_of(td), "1", "--no-verify", "--out", tmp + "/bug1.test"}) == 0, "mtr on a fixture");
  st_check(!read_file(tmp + "/bug1.test").empty(), "the .test file is written");
  remove_tree(tmp);
}


// ---------------------------------------------------------------------------------------------
// --selftest --deep: the same checks plus the parts that need a real server. It runs a short run
// on the fastest build it can find, then the matrix, the report, the MTR form and the by-hand
// server verbs against it. Everything it makes is removed again.
// ---------------------------------------------------------------------------------------------
// A run started by the deep checks makes a workdir under /data. When it saved nothing the workdir
// is removed again; when a trial was saved it stays, because that is a real finding and it is not
// the selftest's to throw away. Returns the workdir, or "" when none was made.
static string st_deep_run(const vector<string>& args, const string& what) {
  std::set<string> before;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) before.insert(e.path().filename().string());
  st_check(quiet_call(cmd_run, args) == 0, what);
  string wd;
  for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) {
    string n = e.path().filename().string();
    if (n.size() == 7 && n[0] == 'O' && is_digits(n.substr(1)) && !before.count(n)) wd = e.path().string();
  }
  return wd;
}
static vector<string> g_deep_kept;                           // run directories the checks left in place
static void st_deep_clean(const string& wd) {
  if (wd.empty() || !dir_exists(wd)) return;
  // only ever a run directory of its own making: /<DATA_DIR>/O<6 digits>
  string base = basename_of(wd);
  if (dirname_of(wd) != g_cfg.data_dir || base.size() != 7 || base[0] != 'O' || !is_digits(base.substr(1))) {
    fprintf(stderr, "selftest: %s is not a run directory of this check, leaving it\n", wd.c_str());
    return;
  }
  std::error_code ec;
  bool keep = false;
  for (auto& e : fs::directory_iterator(wd, ec)) {
    if (!e.is_directory(ec) || !is_digits(e.path().filename().string())) continue;
    string td = e.path().string();
    string uid = trim(read_file(td + "/MYBUG"));
    uid = uid.substr(0, uid.find('\n'));
    bool core = false;
    for (auto& f : fs::directory_iterator(td, ec)) if (f.path().filename().string().find("core") != string::npos) core = true;
    if (!core && file_exists(td + "/ERROR_LOG_SCAN_ISSUE")) continue;   // a line the scan flagged, no crash: noise, not a find
    KbVerdict v = uid.empty() ? KbVerdict::NotFound : kb_verdict(kb_search(uid));
    if (v != KbVerdict::Known && v != KbVerdict::KnownAndFixed) keep = true;   // not a known bug: not the selftest's to delete
  }
  if (keep) { g_deep_kept.push_back(wd); return; }
  remove_tree(wd);
  remove_tree(g_cfg.shm_dir + "/" + basename_of(wd));
}

// the build the live checks use: the fastest kind, a plain optimised CS build
static bool st_deep_build(Basedir& b, const string& what) {
  vector<Basedir> all = basedirs_scan(g_cfg.test_dir);
  const Basedir* pick = nullptr;
  for (auto& x : all) {
    if (x.flavour != Flavour::Plain || x.dbg || x.bin.empty() || x.es) continue;   // the fastest kind: a plain optimised CS build
    if (!pick || version_cmp(x.version, pick->version) > 0) pick = &x;
  }
  if (!pick) { st_skip(what + ": the live checks, as there is no plain optimised build under " + g_cfg.test_dir); return false; }
  st_check(basedir_probe(pick->path, b), what + ": the build probes clean");
  return true;
}
// A second server on the port of the first: it dies at the bind, and its start says so instead of finding the first server there and
// calling itself up (a trial then ran its SQL on the first one and was saved as a crash of the second, which had none). Only a server
// that is found by its port can be mistaken for another, so a socket build has nothing to check.
static void st_deep_port_clash(const Basedir& b, Instance& ci, const string& ctpl, const string& cdir) {
  if (!ci.tcp) return;
  string sdir = cdir + "/second";
  mkdirs(sdir);
  Instance si;
  si.bd = &b;
  si.set_paths(sdir);
  si.port = ci.port;
  double t0 = now_ms();
  bool up = si.start_fresh(ctpl, 120);
  st_check(!up, "deep: a server whose port another server holds does not count as started");
  st_check(port_clash_in_log(read_file(si.errlog)), "deep: its log says the port was taken [" + si.start_note + "]");
  st_check(now_ms() - t0 < 60000, "deep: and the start ends when the server does, not at its timeout");
  si.kill_hard();
  st_check(ci.alive(), "deep: the first server goes on");
  MYSQL* am = mysql_init(nullptr);
  bool conn = endpoint_connect(am, ci.endpoint(), "root", nullptr, 0);
  st_check(conn, "deep: and it still answers");
  mysql_close(am);
}
// --selftest --deep-ports: that check alone, on a real server. The whole deep suite takes an hour, and its "fresh" step wipes the
// fresh server of the build (a directory shared with whoever runs `omnium fresh`); this one touches nothing outside its own temp dir.
static void st_deep_ports() {
  Basedir b;
  if (!st_deep_build(b, "deep-ports")) return;
  if (!endpoint_tcp(b)) { st_skip("deep-ports: the build is a socket one (a Windows build, or OMNIUM_TCP=1, is found by its port), so nothing can answer for another"); return; }
  string cdir = st_tmp() + "/ports";
  mkdirs(cdir);
  Instance ci;
  ci.bd = &b;
  ci.set_paths(cdir);
  string ctpl = template_for(b, "", cdir + "/templates");
  bool up = !ctpl.empty() && ci.start_fresh(ctpl, 120);
  st_check(up, "deep-ports: the first server starts [" + ci.start_note + "]");
  if (up) st_deep_port_clash(b, ci, ctpl, cdir);
  ci.kill_hard();
}

static void st_deep() {
  Basedir b;
  if (!st_deep_build(b, "deep")) return;
  vector<Basedir> all = basedirs_scan(g_cfg.test_dir);        // the later checks pick other builds from it
  string tmp = st_tmp();
  string sql = tmp + "/deep.sql";
  {
    string text;
    for (int i = 0; i < 60; i++) {
      text += "SELECT 1;\nCREATE TABLE IF NOT EXISTS t1 (a INT);\nINSERT INTO t1 VALUES (1);\nSELECT COUNT(*) FROM t1;\nDROP TABLE IF EXISTS t1;\n";
    }
    write_file(sql, text);
  }
  // a server by hand, a replay through it, and the trial helpers
  st_check(quiet_call(cmd_fresh, {b.path}) == 0, "deep: fresh starts a server");
  st_check(quiet_call(cmd_replay, {sql, b.path, "--out", tmp + "/replay.out"}) == 0, "deep: replay runs the SQL");
  {
    // the libraries of a real server, gathered beside a copy of it: never into the build itself
    string ld = tmp + "/ldd_real";
    mkdirs(ld);
    int lrc = -1;
    string lo = copy_file(b.bin, ld + "/mariadbd") ? call_output(cmd_ldd, {ld}, &lrc) : "";
    st_check(lrc == 0 && lo.find(" librar") != string::npos, "deep: ldd gathers the libraries of a real server");
    remove_tree(ld);
  }
  // a short run: two trials, one slot, the trial and client and server code all get used
  string infile_save = g_cfg.infile, before_auto = g_cfg.auto_pipeline ? "1" : "0";
  g_cfg.infile = sql;
  g_cfg.auto_pipeline = false;
  std::set<string> before;
  {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) before.insert(e.path().filename().string());
  }
  int rc = quiet_call(cmd_run, {b.path, "--trials", "2", "--slots", "1", "--seconds", "10", "--no-disk", "--area", "default"});
  st_check(rc == 0, "deep: a two-trial run finishes");
  string wd;
  {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) {
      string n = e.path().filename().string();
      if (n.size() == 7 && n[0] == 'O' && is_digits(n.substr(1)) && !before.count(n)) wd = e.path().string();
    }
  }
  st_check(!wd.empty(), "deep: the run made a workdir");
  if (!wd.empty()) {
    st_check(workdir_is_omnium(wd), "deep: the workdir is an omnium one");
    auto kv = status_read(wd);
    st_check(!kv.empty(), "deep: the run wrote a status");
    {
      auto get = [&](const string& k) { for (auto& p : kv) if (p.first == k) return to_long(p.second, 0); return 0L; };
      long perf = get("performed"), lines = get("sql_lines");
      // the average is over the trials that ran any SQL: both, or one when the other did not start
      auto want = [&](long n) { return n > 0 && perf > 0 ? (long)std::clamp((double)perf / (double)n * g_cfg.sql_size_factor, 2000.0, 5141189.0) : 20000L; };
      st_check(lines == want(2) || lines == want(1), fmt("deep: the SQL size follows what the trials ran (%ld; %ld or %ld)", lines, want(2), want(1)));
    }
    st_check(!read_file(wd + "/run.conf").empty(), "deep: the run wrote its settings for a resume");
    st_check(quiet_call(cmd_status, {basename_of(wd)}) == 0, "deep: status of the run");
    st_check(quiet_call(cmd_tui, {basename_of(wd), "--frame"}) == 0, "deep: the TUI draws a frame");
    st_check(quiet_call(cmd_adopt, {wd}) == 0, "deep: adopt reads the run");
    st_check(trim(read_file(wd + "/omnium.pids")).empty(), "deep: every trial and server pid left the pid list with its trial");
    // resume it for one more trial: the numbers carry on
    long before_max = 0;
    {
      std::error_code ec;
      for (auto& e : fs::directory_iterator(wd, ec)) { string n = e.path().filename().string(); if (is_digits(n)) before_max = std::max(before_max, to_long(n, 0)); }
    }
    // what a crashed run leaves: a listed process that still is the trial the list names goes at
    // the resume; a listed pid that belongs to something else now stays, one on the run's log too.
    // "; exit 0" keeps the shell itself running, so its command line stays the one given here
    pid_t ours = spawn_program({"/bin/sh", "-c", "sleep 60; exit 0", "trial", "--role", "trial", "--workdir", wd}, "/dev/null", "", true, {}, false);
    pid_t other = spawn_program({"/bin/sh", "-c", "sleep 60; exit 0"}, "/dev/null", "", true, {}, false);
    pid_t reader = spawn_program({"/bin/sh", "-c", "sleep 60; exit 0", "tail", wd + "/log/omnium.log"}, "/dev/null", "", true, {}, false);
    write_file(wd + "/omnium.pids", fmt("%d trial 1 left over\n%d server 1 reused\n%d trial 2 reused\n", (int)ours, (int)other, (int)reader));
    // the run's INFILE comes back with it, over a different one in this process's settings
    g_cfg.infile = tmp + "/not_the_runs.sql";
    st_check(quiet_call(cmd_run, {"--resume", basename_of(wd), "--trials", "3", "--seconds", "10"}) == 0, "deep: the run resumes");
    st_check(wait_pid(ours, 5000) != -1, "deep: the resume killed the stopped run's own leftover");
    st_check(pid_alive(other) && pid_alive(reader), "deep: and left alone the listed pids that are something else now");
    for (pid_t q : {other, reader}) { kill_group(q, SIGKILL); wait_pid(q, 5000); }
    st_check(read_file(wd + "/run.conf").find("infile=" + sql + "\n") != string::npos, "deep: the resumed run keeps the INFILE of the run");
    g_cfg.infile = sql;
    // a trial samples the INFILE it is given, not the one of its own settings file
    {
      string twd = tmp + "/infile_arg_wd", trd = tmp + "/infile_arg_rd";
      mkdirs(twd);
      mkdirs(trd);
      CmdResult tr = run_capture({self_exe(), "--role", "trial", "--basedir", b.path, "--workdir", twd, "--rundir", trd, "--trial", "1",
                                  "--infile", tmp + "/no_such_infile.sql"}, 120);
      st_check(tr.out.find("INFILE not found: " + tmp + "/no_such_infile.sql") != string::npos, "deep: the trial takes --infile [" + tail_lines(tr.out, 1) + "]");
    }
    bool carried = false;
    for (auto& l : split_lines(read_file(wd + "/omnium.ledger"))) if (l.find(" resumed ") != string::npos) carried = true;
    st_check(carried, "deep: the ledger says it resumed");
    // the control file: the words a run takes, and the one it does not. The run has already reached
    // its trial target, so each of these is one turn of the loop.
    for (const char* word : {"pause", "resume", "nonsense", "stop", "stop-now"}) {
      write_file(wd + "/omnium.ctl", string(word) + "\n");
      st_check(quiet_call(cmd_run, {"--resume", basename_of(wd), "--trials", "3", "--seconds", "5"}) == 0, string("deep: the run reads ") + word + " from the control file");
      st_check(!file_exists(wd + "/omnium.ctl"), string("deep: the control file is read once and taken away (") + word + ")");
    }
    // --resume with no name picks the newest stopped run
    st_check(quiet_call(cmd_run, {"--resume", "--trials", "3", "--seconds", "5"}) == 0, "deep: resume with no name picks the newest stopped run");
    (void)before_max;
  }
  // stop-now, the TUI's S, while a trial runs: the trial is killed and the run ends at once, not
  // after the trial's 60 seconds
  {
    std::set<string> before;
    std::error_code sec;
    for (auto& e : fs::directory_iterator(g_cfg.data_dir, sec)) before.insert(e.path().filename().string());
    std::atomic<bool> ended{false};
    std::thread stopper([&] {
      for (int i = 0; i < 1200 && !ended; i++) {
        usleep(100000);
        for (auto& e : fs::directory_iterator(g_cfg.data_dir, sec)) {
          string n = e.path().filename().string();
          if (n.size() != 7 || n[0] != 'O' || !is_digits(n.substr(1)) || before.count(n)) continue;
          if (!dir_exists(g_cfg.shm_dir + "/" + n + "/1")) continue;    // trial 1 is on the tmpfs: it runs
          sleep(2);
          write_file(e.path().string() + "/omnium.ctl", "stop-now\n");
          return;
        }
      }
    });
    double t0 = now_ms();
    string swd = st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "60", "--no-disk", "--mode", "normal"}, "deep: a run told to stop now ends");
    ended = true;
    stopper.join();
    st_check(now_ms() - t0 < 45000, "deep: and it does not wait for the trial to finish");
    if (!swd.empty()) {
      st_check(read_file(swd + "/" + basename_of(swd) + ".log").find("second stop request: killing the running trials") != string::npos,
               "deep: the run log says the running trial was killed");
      st_deep_clean(swd);
    }
  }
  // the trial modes: several client threads, and a kill with a recovery afterwards
  {
    st_deep_clean(st_deep_run({b.path, "--trials", "1", "--seconds", "10", "--no-disk", "--mode", "multi", "--threads", "4"},
                              "deep: a multi-thread trial runs, with the slots sized by the box"));
    st_deep_clean(st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "10", "--no-disk", "--mode", "crash"},
                              "deep: a crash-recovery trial runs"));
    const Area* spider = area_by_name("spider");
    if (spider && area_available(*spider, b, nullptr))
      st_deep_clean(st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "10", "--no-disk", "--area", "spider"},
                                "deep: a spider trial runs, its preload first"));
    if (!b.backup.empty()) {
      // the round trip runs on random SQL, so its outcome is the server's to decide: a match, an issue that
      // is kept with its files and UID (or dropped as a known one), or a skip when the SQL locked root out
      string bwd = st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "10", "--no-disk", "--mode", "backup"}, "deep: a backup round-trip trial runs");
      string line;
      std::error_code ec;
      if (!bwd.empty()) for (auto& e : fs::directory_iterator(bwd + "/log", ec)) for (auto& l : split_lines(read_file(e.path().string()))) if (l.find("backup round trip") != string::npos) line = l;
      st_check(!line.empty(), "deep: the trial log says the backup round trip ran");
      if (line.find("round trip issue") != string::npos) {
        bool kept = false;
        for (auto& e : fs::directory_iterator(bwd, ec)) {
          string td = e.path().string();
          if (is_digits(e.path().filename().string()) && file_exists(td + "/BACKUP_ISSUE") && !trim(read_file(td + "/MYBUG")).empty()) kept = true;
        }
        bool known = read_file(bwd + "/omnium.ledger").find(" known BACKUP_ISSUE|") != string::npos;
        st_check(kept || known, "deep: a backup issue is kept with BACKUP_ISSUE and a UID, or dropped as a known one");
      }
      st_deep_clean(bwd);
    }
  }
  // the client on its own: several threads against one server, and what it says when there is none
  {
    string cdir = tmp + "/client";
    mkdirs(cdir);
    Instance ci;
    ci.bd = &b;
    ci.set_paths(cdir);
    string ctpl = template_for(b, "", cdir + "/templates");
    if (!ctpl.empty() && ci.start_fresh(ctpl, 120)) {
      ClientParams cp;
      cp.ep = ci.endpoint();
      cp.logdir = cdir;
      cp.threads = 2;
      cp.seed = 42;
      cp.queries_per_thread = 300;                            // the driver bounds a trial by time; here a count does it
      std::atomic<bool> stop{false};
      ClientResult cr;
      string cerr;
      st_check(client_run(cp, sql, stop, cr, &cerr), "deep: the client runs the SQL on two threads");
      st_check(cr.performed > 0, "deep: the client performed queries");
      st_check(cr.thread_seeds.size() == 2, "deep: each thread has its own seed");
      st_check(file_exists(cdir + "/default.node.tld_thread-0.sql"), "deep: the client wrote the per-thread SQL");
      client_kill_connections(cp);
      // a thread stuck in a long statement: killing its connection from the outside ends the run
      {
        string kdir = cdir + "/kill";
        mkdirs(kdir);
        write_file(kdir + "/sleep.sql", "SELECT SLEEP(60);\n");
        ClientParams kp;
        kp.ep = ci.endpoint();
        kp.logdir = kdir;
        kp.shuffle = false;
        kp.queries_per_thread = 1;
        std::atomic<bool> kstop{false};
        ClientResult kr;
        auto t0 = std::chrono::steady_clock::now();
        std::thread killer([&] { sleep(3); client_kill_connections(kp); });
        string kerr;
        client_run(kp, kdir + "/sleep.sql", kstop, kr, &kerr);
        killer.join();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        st_check(secs < 30 && kr.failed == 1, "deep: killing a client connection ends a long statement");
      }
      // the client's other shapes, on the same server: writing every answer it gets, giving up
      // after a run of failures, and a log directory it cannot write to
      {
        string odir = cdir + "/out";
        mkdirs(odir);
        write_file(odir + "/rows.sql", "SELECT 1 AS a, NULL AS b, '' AS c;\nSELECT 2;\n");
        ClientParams op;
        op.ep = ci.endpoint();
        op.logdir = odir;
        op.threads = 1;
        op.seed = 11;
        op.shuffle = false;
        op.queries_per_thread = 2;
        op.log_client_output = true;
        op.log_numbers = true;
        std::atomic<bool> ostop{false};
        ClientResult orr;
        string oerr;
        st_check(client_run(op, odir + "/rows.sql", ostop, orr, &oerr), "deep: the client runs with every answer written down");
        string rows = read_file(odir + "/default.node.tld_thread-0.out");
        st_check(rows.find("1#") != string::npos, "deep: a value is written with its separator");
        st_check(rows.find("#NO DATA#") != string::npos, "deep: a NULL is written as no data");
        st_check(rows.find("EMPTY#") != string::npos, "deep: an empty string is written as empty");

        string fdir = cdir + "/failrun";
        mkdirs(fdir);
        write_file(fdir + "/bad.sql", "SELECT no_such_column_zzz;\nSELECT no_such_column_zzz;\nSELECT no_such_column_zzz;\nSELECT no_such_column_zzz;\n");
        ClientParams fp;
        fp.ep = ci.endpoint();
        fp.logdir = fdir;
        fp.threads = 1;
        fp.seed = 12;
        fp.shuffle = false;
        fp.queries_per_thread = 4;
        fp.max_con_failures = 2;
        std::atomic<bool> fstop{false};
        ClientResult fr;
        string ferr2;
        client_run(fp, fdir + "/bad.sql", fstop, fr, &ferr2);
        st_check(fr.consecutive_stop > 0, "deep: the client gives up after a run of failures");
        st_check(read_file(fdir + "/default.node.tld_general.log").find("consecutive queries all failed") != string::npos,
                 "deep: and says so in its log");
        // the query that ends the thread was sent too, so the summary cannot show more failed than sent
        st_check(fr.failed == 2 && fr.performed == 2, fmt("deep: both failed queries count as sent (%llu of %llu)", fr.failed, fr.performed));
        st_check(read_file(fdir + "/default.node.tld_general.log").find("* NODE SUMMARY: 2/2 queries failed, (0.00% were successful)") != string::npos,
                 "deep: and the summary line reads 2/2");

        ClientParams np;
        np.ep = ci.endpoint();
        np.logdir = "/proc/omnium_cannot_write_here";
        np.threads = 1;
        np.seed = 13;
        np.queries_per_thread = 1;
        np.log_client_output = true;
        std::atomic<bool> nstop{false};
        ClientResult nr;
        string nerr;
        client_run(np, odir + "/rows.sql", nstop, nr, &nerr);
        st_check(nr.performed == 0, "deep: a log directory it cannot write to stops the thread");
      }
      st_deep_port_clash(b, ci, ctpl, cdir);
      ci.kill_hard();
    }
    // the client when the server goes away mid-run: the threads see the connection drop and write
    // the last statements they sent, which is what a crashed trial leaves behind
    {
      string kdir = tmp + "/client_lost";
      mkdirs(kdir);
      Instance ki;
      ki.bd = &b;
      ki.set_paths(kdir);
      string ktpl = template_for(b, "", kdir + "/templates");
      if (!ktpl.empty() && ki.start_fresh(ktpl, 120)) {
        ClientParams kp;
        kp.ep = ki.endpoint();
        kp.logdir = kdir;
        kp.threads = 2;
        kp.seed = 7;
        kp.queries_per_thread = 2000000;                      // it never gets there: the server is killed
        std::atomic<bool> stop{false};
        ClientResult kr;
        string kerr;
        std::thread killer([&] { sleep(3); ki.kill_hard(); });
        client_run(kp, sql, stop, kr, &kerr);
        killer.join();
        st_check(kr.lost_connection > 0 || kr.connect_failed > 0 || kr.gone_away > 0, "deep: the client notices the server is gone");
        st_check(file_exists(kdir + "/default.node.tld_thread-0.last.sql") || kr.performed > 0, "deep: it wrote what it had sent");
      }
      ki.kill_hard();
    }
    // a server that turns new connections away: the trial SQL can set init_connect to something
    // that fails and take root's SUPER away, and then the shutdown client cannot get in. That is
    // not a server that will not stop, so the stop has to fall back to SIGTERM and still succeed.
    {
      string rdir = tmp + "/refuse";
      mkdirs(rdir);
      Instance ri;
      ri.bd = &b;
      ri.set_paths(rdir);
      string rtpl = template_for(b, "", rdir + "/templates");
      if (!rtpl.empty() && ri.start_fresh(rtpl, 120)) {
        // init_connect is skipped for a connection that holds SUPER or CONNECTION ADMIN, so both
        // go, and SHUTDOWN stays: exactly the state a trial can leave the server in
        vector<string> cargs = {b.client, "--no-defaults", "-uroot"};
        for (auto& x : endpoint_args(ri.endpoint())) cargs.push_back(x);
        for (const char* x : {"--force", "test"}) cargs.push_back(x);
        vector<string> pargs = {b.admin, "--no-defaults", "-uroot"};
        for (auto& x : endpoint_args(ri.endpoint())) pargs.push_back(x);
        pargs.push_back("ping");
        CmdResult setup = run_capture_in(cargs,
                                         "SET GLOBAL init_connect='SELECT no_such_column_xyz';\n"
                                         "REVOKE SUPER, CONNECTION ADMIN ON *.* FROM 'root'@'localhost';\n"
                                         "FLUSH PRIVILEGES;\n", 60);
        CmdResult ping = run_capture(pargs, 60);
        bool locked_out = ping.rc != 0;
        st_check(locked_out, "deep: with init_connect broken the admin client cannot get in [" + trim(setup.out) + "|" + trim(ping.out) + "]");
        string note;
        int64_t t0 = now_s();
        bool gone = ri.shutdown(25, &note);
        st_check(gone, "deep: the server still stops when the shutdown client is refused");
        st_check(now_s() - t0 < 25, "deep: it does not sit out the whole shutdown wait");
        if (locked_out) st_check(note.find("SIGTERM") != string::npos, "deep: the note says SIGTERM was sent [" + note + "]");
      }
      ri.kill_hard();
    }
    ClientParams dead;
    dead.ep.sock = cdir + "/no_such.sock";
    dead.logdir = cdir;
    std::atomic<bool> stop2{false};
    ClientResult cr2;
    string cerr2;
    client_run(dead, sql, stop2, cr2, &cerr2);
    st_check(cr2.performed == 0, "deep: the client performs nothing when there is no server");
  }
  // A stand-in server that refuses to start: a basedir whose binary prints an option error and
  // exits. This is how the trial worker behaves when a server option is not accepted, which is a
  // configured value and not a bug, so the trial has to be dropped and nothing saved.
  {
    string fake = tmp + "/MD010926-mariadb-13.1.0-linux-x86_64-opt";
    mkdirs(fake + "/bin");
    mkdirs(fake + "/scripts");
    write_file(fake + "/scripts/mariadb-install-db",
               "#!/bin/sh\nd=\"\"\nfor a in \"$@\"; do case \"$a\" in --datadir=*) d=${a#--datadir=};; esac; done\n"
               "[ -n \"$d\" ] || exit 1\nmkdir -p \"$d/mysql\" \"$d/test\"\n: > \"$d/mysql/user.frm\"\n: > \"$d/mysql.ibd\"\n: > \"$d/ibdata1\"\nexit 0\n");
    write_file(fake + "/bin/mariadbd",
               "#!/bin/sh\ncase \"$1\" in --version) echo \"mariadbd  Ver 13.1.0-MariaDB for Linux\"; exit 0;; esac\n"
               "echo \"2026-09-07  8:00:00 0 [ERROR] mariadbd: unknown variable 'omnium_selftest_option=1'\"\n"
               "echo \"2026-09-07  8:00:00 0 [ERROR] Aborting\"\nexit 1\n");
    write_file(fake + "/bin/mariadb", "#!/bin/sh\nexit 0\n");
    for (const char* f : {"/scripts/mariadb-install-db", "/bin/mariadbd", "/bin/mariadb"}) chmod((fake + f).c_str(), 0755);
    Basedir fb;
    st_check(basedir_probe(fake, fb), "deep: the stand-in basedir probes");
    st_check(fb.vendor == Vendor::MariaDB && fb.version == "13.1.0" && !fb.init_tool.empty(), "deep: and its name gives the version and the init tool");
    Instance fi;
    fi.bd = &fb;
    fi.set_paths(tmp + "/fakerun");
    string ftpl = template_for(fb, "", tmp + "/fakerun/templates");
    st_check(!ftpl.empty(), "deep: a datadir template is made from the stand-in install tool");
    if (!ftpl.empty()) {
      st_check(!fi.start_fresh(ftpl, 30), "deep: the stand-in server does not come up");
      st_check(!fi.start_note.empty(), "deep: and the start note says so");
      st_check(read_file(fi.errlog).find("Aborting") != string::npos, "deep: its error log holds the reason");
      fi.kill_hard();
    }
    string fwd = st_deep_run({fake, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                             "deep: a run against a server that refuses an option ends by itself");
    if (!fwd.empty()) {
      string led = read_file(fwd + "/omnium.ledger");
      st_check(led.find("outcome=options") != string::npos || led.find(" options ") != string::npos || led.find("start-failed") != string::npos,
               "deep: the ledger says the trial was dropped over the option");
      std::error_code ec;
      bool any_trial = false;
      for (auto& e : fs::directory_iterator(fwd, ec)) if (e.is_directory(ec) && is_digits(e.path().filename().string())) any_trial = true;
      st_check(!any_trial, "deep: a refused option saves no trial");
      st_deep_clean(fwd);
    }
    // the by-hand verbs against the same stand-in: fresh cannot bring it up, and cl has no server
    st_check(quiet_call(cmd_fresh, {fake}) != 0, "deep: fresh says so when the server will not come up");
    st_check(quiet_call(cmd_cl, {fake}) != 0, "deep: cl says so when there is no server to connect to");
    // the report matrix on the same stand-in: the row records that the server did not start
    {
      string mtd = fixture_trial(tmp + "/refusematrix", 1, fixture_errlog(),
                                 "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y");
      write_file(mtd + "/BASEDIR", fake + "\n");
      string mbox = tmp + "/inbox_refuse";
      int rc = quiet_call(cmd_report, {dirname_of(mtd), "1", fake, "--slots", "1", "--no-mtr", "--out", mbox});
      st_check(rc == 0, "deep: a report whose matrix build refuses to start still comes out");
      if (rc == 0) {
        string rt = read_file(mbox + "/O999999_bug1.report");
        st_check(rt.find("did not start") != string::npos || rt.find("No result") != string::npos,
                 "deep: and the matrix row says the server did not start");
      }
    }
  }
  // The settings a trial child reads for itself. HOME points at a file this check writes, so the
  // child puts its core on DATA_DIR instead of the tmpfs and keeps one trial per UniqueID. The
  // driver side is set here as well, because the run itself is in this process.
  {
    string h = tmp + "/trialhome";
    mkdirs(h);
    write_file(h + "/.omnium.conf",
               "TEST_DIR=" + g_cfg.test_dir + "\nDATA_DIR=" + g_cfg.data_dir + "\nSHM_DIR=" + g_cfg.shm_dir + "\nQA_DIR=" + g_cfg.qa_dir +
                   "\nCORE_DIR=data\nKEEP_PER_UID=1\nEMAIL=\nAUTO_PIPELINE=0\n");
    const char* oldhome = getenv("HOME");
    string keep = oldhome ? oldhome : "";
    setenv("HOME", h.c_str(), 1);
    string save_core = g_cfg.core_dir;
    int save_keep = g_cfg.keep_per_uid;
    g_cfg.core_dir = "data";
    g_cfg.keep_per_uid = 1;
    string dwd = st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                             "deep: a trial with its datadir on the results disk runs");
    g_cfg.core_dir = save_core;
    g_cfg.keep_per_uid = save_keep;
    setenv("HOME", keep.c_str(), 1);
    if (!dwd.empty()) {
      st_check(read_file(dwd + "/omnium.ledger").find("trial 1") != string::npos, "deep: and the run records it");
      st_deep_clean(dwd);
    }
  }
  // A stand-in basedir that is the real server with one line added to its error log: a corruption
  // line the error-log scan flags and the known-bug list does not have. Nothing is wrong with the
  // server. The point is the path a run takes when a trial has to be kept: the helper scripts, the
  // binary copy, the ledger line, and the trial directory under the data dir.
  {
    string sanbd = tmp + "/MD020926-mariadb-13.1.0-linux-x86_64-opt";
    string binname = basename_of(b.bin);
    mkdirs(sanbd + "/bin");
    std::error_code lec;
    for (auto& e : fs::directory_iterator(b.path, lec)) {
      string n = e.path().filename().string();
      if (n == "bin") continue;
      if (symlink(e.path().c_str(), (sanbd + "/" + n).c_str()) != 0) { /* already there */ }
    }
    for (auto& e : fs::directory_iterator(b.path + "/bin", lec)) {
      string n = e.path().filename().string();
      if (n == binname) continue;
      if (symlink(e.path().c_str(), (sanbd + "/bin/" + n).c_str()) != 0) { /* already there */ }
    }
    write_file(sanbd + "/bin/" + binname,
               "#!/bin/sh\nLE=\nfor a in \"$@\"; do case \"$a\" in --log-error=*) LE=${a#--log-error=};; esac; done\n"
               "if [ -n \"$LE\" ]; then mkdir -p \"$(dirname \"$LE\")\"; "
               "echo '2026-09-07  8:00:00 0 [ERROR] InnoDB: omnium selftest marker: corrupted page 4294967295 in file ./omnium/selftest.ibd' >> \"$LE\"; fi\n"
               "exec '" + b.bin + "' \"$@\"\n");
    chmod((sanbd + "/bin/" + binname).c_str(), 0755);
    Basedir sb;
    st_check(basedir_probe(sanbd, sb), "deep: the stand-in with a flagged error log probes");
    string swd = st_deep_run({sanbd, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                             "deep: a run whose trial has to be kept ends by itself");
    if (!swd.empty()) {
      st_check(read_file(swd + "/omnium.ledger").find("saved-errlog") != string::npos, "deep: the ledger says the trial was kept over the error log");
      string td;
      for (auto& e : fs::directory_iterator(swd, lec)) if (e.is_directory(lec) && is_digits(e.path().filename().string())) td = e.path().string();
      st_check(!td.empty(), "deep: the trial is under the run directory");
      if (!td.empty()) {
        st_check(!trim(read_file(td + "/MYBUG")).empty(),
                 "deep: the kept trial has its UniqueID [" + trim(read_file(td + "/MYBUG")) + "]");
        st_check(file_exists(td + "/ERROR_LOG_SCAN_ISSUE"), "deep: and the note saying what the error-log scan found");
        for (const char* f : {"/start", "/stop", "/cl", "/pquery.log", "/trial.sql", "/BASEDIR", "/MYEXTRA", "/MYSAFE", "/SEED"})
          st_check(file_exists(td + f), string("deep: the kept trial has ") + (f + 1));
        st_check(dir_exists(td + "/data"), "deep: the kept trial has its datadir");
        struct stat sst;
        st_check(stat((td + "/start").c_str(), &sst) == 0 && (sst.st_mode & S_IXUSR), "deep: its start script can be run");
        string pq = read_file(td + "/pquery.log");
        st_check(pq.find("NODE SUMMARY") != string::npos && pq.find("> Infile:") != string::npos, "deep: and a pquery.log with the run record");
      }
      // the binary copy: the server and every library it needs, so the trial reads on another box
      string mdir = swd + "/mysqld/" + sb.short_name();
      st_check(file_exists(mdir + "/" + binname), "deep: the server binary is copied into the run directory");
      long libs = 0;
      for (auto& e : fs::directory_iterator(mdir, lec)) { (void)e; libs++; }
      // the stand-in binary is a shell script, so ldd finds no libraries for it; the library copy
      // itself is checked on the real binary further up
      st_check(libs >= 1, fmt("deep: the copy directory holds it (%ld files)", libs));
      st_check(file_exists(swd + "/mysqld/" + binname), "deep: and the framework's own name points at it");
      // every log of this run carries a line this check planted, so no trial of it is a finding:
      // the sweep sees the flag, no core, and takes the run away
      st_deep_clean(swd);
      st_check(!dir_exists(swd), "deep: a trial kept only for a planted error-log line is not a finding");
    }
    // the same trial with its datadir on the results disk: when it is kept, only the part on the
    // tmpfs moves across. The trial children take the setting from OMNIUM_SET, as from a command line.
    {
      const char* os = getenv("OMNIUM_SET");
      string saved_set = os ? os : "";
      setenv("OMNIUM_SET", (saved_set + "CORE_DIR=data\n").c_str(), 1);
      string save_core = g_cfg.core_dir;
      g_cfg.core_dir = "data";
      string dwd = st_deep_run({sanbd, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                               "deep: a kept trial with its datadir on the results disk");
      g_cfg.core_dir = save_core;
      if (os) setenv("OMNIUM_SET", saved_set.c_str(), 1);
      else unsetenv("OMNIUM_SET");
      if (!dwd.empty()) {
        string td;
        for (auto& e : fs::directory_iterator(dwd, lec)) if (e.is_directory(lec) && is_digits(e.path().filename().string())) td = e.path().string();
        st_check(!td.empty() && dir_exists(td + "/data") && file_exists(td + "/log/master.err") && !trim(read_file(td + "/MYBUG")).empty(),
                 "deep: the kept trial has its datadir, its error log and its UniqueID");
        st_deep_clean(dwd);
      }
    }
    // a trial whose server crashes: this stand-in sends its server SIGABRT a few seconds in (not the
    // install-db bootstrap). The core moves from the tmpfs to the results disk once the server is
    // gone, and the UID comes from gdb on it. A planted crash is not a finding, so the run goes.
    {
      string crashbd = tmp + "/MD040926-mariadb-13.1.0-linux-x86_64-opt";
      mkdirs(crashbd + "/bin");
      for (auto& e : fs::directory_iterator(b.path, lec)) {
        string n = e.path().filename().string();
        if (n != "bin" && symlink(e.path().c_str(), (crashbd + "/" + n).c_str()) != 0) { /* already there */ }
      }
      for (auto& e : fs::directory_iterator(b.path + "/bin", lec)) {
        string n = e.path().filename().string();
        if (n != binname && symlink(e.path().c_str(), (crashbd + "/bin/" + n).c_str()) != 0) { /* already there */ }
      }
      write_file(crashbd + "/bin/" + binname, "#!/bin/sh\ncase \" $* \" in *\" --bootstrap \"*) exec '" + b.bin + "' \"$@\";; esac\n"
                                              "(sleep 8; kill -ABRT $$) &\nexec '" + b.bin + "' \"$@\"\n");
      chmod((crashbd + "/bin/" + binname).c_str(), 0755);
      string cwd = st_deep_run({crashbd, "--trials", "1", "--slots", "1", "--seconds", "30", "--no-disk"}, "deep: a run whose server crashes ends by itself");
      if (!cwd.empty()) {
        string td = cwd + "/1";
        bool core = false;
        for (auto& e : fs::directory_iterator(td, lec)) if (e.path().filename().string().find("core") != string::npos && e.is_regular_file(lec)) core = true;
        string uid = trim(read_file(td + "/MYBUG"));
        st_check(core, "deep: the crashed trial is kept, its core beside it");
        st_check(uid.find("SIGABRT") != string::npos || (uid.find("|") != string::npos && !starts_with(uid, "Assert:")), "deep: and its UniqueID comes from the core [" + uid + "]");
        remove_tree(cwd);
        remove_tree(g_cfg.shm_dir + "/" + basename_of(cwd));
      }
    }
    // the same again with a sanitizer report as the whole error log, and with an error log over
    // the size cap: two more reasons a run has to keep a trial
    {
      string sanonly = tmp + "/MD030926-mariadb-13.1.0-linux-x86_64-opt";
      mkdirs(sanonly + "/bin");
      for (auto& e : fs::directory_iterator(b.path, lec)) {
        string n = e.path().filename().string();
        if (n == "bin") continue;
        if (symlink(e.path().c_str(), (sanonly + "/" + n).c_str()) != 0) { /* already there */ }
      }
      for (auto& e : fs::directory_iterator(b.path + "/bin", lec)) {
        string n = e.path().filename().string();
        if (n == binname) continue;
        if (symlink(e.path().c_str(), (sanonly + "/bin/" + n).c_str()) != 0) { /* already there */ }
      }
      write_file(sanonly + "/bin/" + binname,
                 "#!/bin/sh\nLE=\nfor a in \"$@\"; do case \"$a\" in --log-error=*) LE=${a#--log-error=};; esac; done\n"
                 "if [ -n \"$LE\" ]; then mkdir -p \"$(dirname \"$LE\")\"; cat > \"$LE\" <<'SANEOF'\n" +
                 fixture_san_log() + "SANEOF\n"
                 "exec '" + b.bin + "' \"$@\" --log-error=\"$LE.server\"\nfi\nexec '" + b.bin + "' \"$@\"\n");
      chmod((sanonly + "/bin/" + binname).c_str(), 0755);
      string awd = st_deep_run({sanonly, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                               "deep: a run whose error log is a sanitizer report ends by itself");
      if (!awd.empty()) {
        st_check(read_file(awd + "/omnium.ledger").find("saved-san") != string::npos, "deep: the ledger says the trial was kept for the sanitizer report");
        remove_tree(awd);
        remove_tree(g_cfg.shm_dir + "/" + basename_of(awd));
      }
      string bigbd = tmp + "/MD040926-mariadb-13.1.0-linux-x86_64-opt";
      mkdirs(bigbd + "/bin");
      for (auto& e : fs::directory_iterator(b.path, lec)) {
        string n = e.path().filename().string();
        if (n == "bin") continue;
        if (symlink(e.path().c_str(), (bigbd + "/" + n).c_str()) != 0) { /* already there */ }
      }
      for (auto& e : fs::directory_iterator(b.path + "/bin", lec)) {
        string n = e.path().filename().string();
        if (n == binname) continue;
        if (symlink(e.path().c_str(), (bigbd + "/bin/" + n).c_str()) != 0) { /* already there */ }
      }
      write_file(bigbd + "/bin/" + binname,
                 "#!/bin/sh\nLE=\nfor a in \"$@\"; do case \"$a\" in --log-error=*) LE=${a#--log-error=};; esac; done\n"
                 "if [ -n \"$LE\" ]; then mkdir -p \"$(dirname \"$LE\")\"; "
                 "seq 1 130000 | sed 's/^/2026-09-07  8:00:00 0 [Note] padding line /' > \"$LE\"; fi\n"
                 "exec '" + b.bin + "' \"$@\"\n");
      chmod((bigbd + "/bin/" + binname).c_str(), 0755);
      string bwd = st_deep_run({bigbd, "--trials", "1", "--slots", "1", "--seconds", "5", "--no-disk"},
                               "deep: a run whose error log is over the size cap ends by itself");
      if (!bwd.empty()) {
        // an error log this size is always kept; which reason wins depends on what else the run's
        // own SQL left in the log, so the note is what this checks
        st_check(read_file(bwd + "/omnium.ledger").find("saved-") != string::npos, "deep: the trial with the oversized log is kept");
        string btd;
        for (auto& e : fs::directory_iterator(bwd, lec)) if (e.is_directory(lec) && is_digits(e.path().filename().string())) btd = e.path().string();
        st_check(!btd.empty(), "deep: and it is under the run directory");
        if (!btd.empty()) st_check(file_exists(btd + "/LARGE_ERROR_LOG_ISSUE"), "deep: with the note saying the log is over the cap");
        remove_tree(bwd);
        remove_tree(g_cfg.shm_dir + "/" + basename_of(bwd));
      }
    }
  }
  // the build side, as far as it goes without a real source tree: the job runner is pointed at a
  // tree that holds only a VERSION file, so cmake fails at once and the failure path is walked.
  // Nothing is cloned and nothing is compiled here.
  {
    string save_test = g_cfg.test_dir;
    string bt = tmp + "/testdir";
    mkdirs(bt);
    g_cfg.test_dir = bt;
    string src = tmp + "/src13.1";
    mkdirs(src);
    write_file(src + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\n");
    BuildJob j;
    j.source = src;
    j.flavour = "opt";
    j.jobs = 2;
    BuildResult br;
    int rc = build_one(j, br);
    st_check(rc != 0, "deep: a build of a tree with no cmake files fails");
    st_check(!br.note.empty(), "deep: and says where the log is");
    BuildJob bad = j;
    bad.flavour = "nonsense";
    BuildResult br2;
    st_check(build_one(bad, br2) == 2, "deep: an unknown flavour is refused before anything is built");
    BuildJob nosrc = j;
    nosrc.source = tmp + "/no_such_tree";
    BuildResult br3;
    st_check(build_one(nosrc, br3) == 2, "deep: a source tree that is not there is refused");
    st_check(quiet_call(cmd_build, {"follow"}) == 0, "deep: build follow with nothing listed has nothing to do");
    // A build that goes all the way through. The source tree is one C file, so cmake, ninja,
    // ninja install, the probe of what came out and the registry entry all run, in seconds
    // instead of the half hour a real server takes.
    {
      string fsrc = tmp + "/src_tiny";
      mkdirs(fsrc);
      write_file(fsrc + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\n");
      write_file(fsrc + "/mariadbd.c",
                 "#include <stdio.h>\nint main(int argc, char** argv) { (void)argc; (void)argv;\n"
                 "  printf(\"mariadbd  Ver 13.1.0-MariaDB for Linux (x86_64)\\n\"); return 0; }\n");
      for (const char* t : {"/mariadb", "/mariadb-admin", "/mariadb-install-db"}) write_file(fsrc + t, "#!/bin/sh\nexit 0\n");
      write_file(fsrc + "/CMakeLists.txt",
                 "cmake_minimum_required(VERSION 3.10)\nproject(tiny C)\n"
                 "add_executable(mariadbd mariadbd.c)\n"
                 "install(TARGETS mariadbd DESTINATION bin)\n"
                 "install(PROGRAMS mariadb mariadb-admin DESTINATION bin)\n"
                 "install(PROGRAMS mariadb-install-db DESTINATION scripts)\n");
      string save_builds = g_paths.builds_file, save_data = g_cfg.data_dir;
      g_paths.builds_file = bt + "/omnium.builds";
      g_cfg.data_dir = tmp + "/builddata";
      mkdirs(g_cfg.data_dir);
      BuildJob tj;
      tj.source = fsrc;
      tj.flavour = "opt";
      tj.jobs = 2;
      tj.tar = true;
      BuildResult tr;
      int trc = build_one(tj, tr);
      st_check(trc == 0, "deep: a build that compiles and installs comes back clean [" + tr.note + "]");
      if (trc == 0) {
        st_check(dir_exists(tr.basedir) && file_exists(tr.basedir + "/bin/mariadbd"), "deep: the server binary is installed");
        st_check(tr.version_line.find("13.1.0") != string::npos, "deep: and its version line was read back");
        st_check(file_exists(tr.basedir + "/BUILD_CMD_CMAKE") && file_exists(tr.basedir + "/git_revision.txt"), "deep: the build leaves its cmake line and revision");
        st_check(file_exists(tr.log), "deep: and the build log is kept beside it");
        st_check(!tr.tar.empty() && file_exists(tr.tar), "deep: the tarball is written to TARS");
        st_check(tr.listed, "deep: and the build is listed in the registry");
        BuildResult again;
        st_check(build_one(tj, again) == 3, "deep: the same build again says it is there already");
        BuildJob rb = tj;
        rb.rebuild = true;
        rb.tar = false;
        BuildResult rbr;
        st_check(build_one(rb, rbr) == 0, "deep: --rebuild replaces it");
        // a basedir the build log cannot go into: the build still comes back, and keeps its tree
        string lsrc = tmp + "/src_tiny_logdir";
        std::error_code cec;
        fs::copy(fsrc, lsrc, fs::copy_options::recursive, cec);
        mkdirs(lsrc + "/logdir");
        write_file(lsrc + "/logdir/keep", "x\n");
        write_file(lsrc + "/CMakeLists.txt", read_file(lsrc + "/CMakeLists.txt") + "install(DIRECTORY logdir/ DESTINATION omnium_build.log)\n");
        CmdResult lb = run_capture({self_exe(), "TEST_DIR=" + g_cfg.test_dir, "build", lsrc, "dbg"}, 600, tmp);
        st_check(lb.rc == 0 && lb.out.find("the log could not go into the basedir") != string::npos && file_exists(lsrc + "_dbg/omnium_build.log"),
                 "deep: a build log that cannot go into the basedir is named, and the tree with it stays [" + trim(tail_lines(lb.out, 1)) + "]");
        st_check(lb.out.find(", listed in " + g_cfg.test_dir + "/omnium.builds") != string::npos, "deep: and the build says which registry lists it");
      }
      g_paths.builds_file = save_builds;
      g_cfg.data_dir = save_data;
    }
    // The patch-1 tree: a feature branch minus the feature, taken at its merge-base with the base
    // branch. Every repository here is a local one, so nothing goes over the network.
    {
      string groot = tmp + "/git";
      string origin = groot + "/origin";
      mkdirs(origin);
      auto g = [&](const vector<string>& args, const string& cwd) {
        vector<string> argv = {"git", "-c", "user.email=selftest@omnium", "-c", "user.name=omnium selftest", "-c", "commit.gpgsign=false"};
        for (auto& x : args) argv.push_back(x);
        return run_capture(argv, 300, cwd);
      };
      g({"init", "-q", "-b", "13.1"}, origin);
      write_file(origin + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\n");
      g({"add", "VERSION"}, origin);
      g({"commit", "-q", "-m", "base"}, origin);
      g({"checkout", "-q", "-b", "MDEV-1234-feature"}, origin);
      g({"commit", "-q", "--allow-empty", "-m", "the feature"}, origin);
      g({"checkout", "-q", "13.1"}, origin);
      for (int i = 0; i < 55; i++) g({"commit", "-q", "--allow-empty", "-m", "base moves on"}, origin);
      string feat = groot + "/MDEV-1234-feature";
      CmdResult cl = run_capture({"git", "clone", "-q", "--branch", "MDEV-1234-feature", origin, feat}, 600);
      st_check(cl.rc == 0 && dir_exists(feat + "/.git"), "deep: the stand-in feature branch is cloned");
      if (cl.rc == 0) {
        string tree, note, perr;
        bool ok = patch1_tree(feat, tree, note, &perr);
        st_check(ok, "deep: the patch-1 tree is worked out [" + perr + "]");
        st_check(note.find("commits behind") != string::npos, "deep: and it says how far behind the base is [" + note + "]");
        st_check(!tree.empty() && dir_exists(tree), "deep: a base far enough ahead gets a tree of its own");
        if (!tree.empty() && dir_exists(tree)) {
          st_check(file_exists(tree + "/VERSION"), "deep: the tree holds the source at the merge-base");
          string tree2, note2, perr2;
          st_check(patch1_tree(feat, tree2, note2, &perr2) && tree2 == tree, "deep: asking again gives the same tree");
          run_capture({"git", "-C", feat, "worktree", "remove", "--force", tree}, 300);
        }
        // omnium build --patch-1 on the same branch: the job it makes points at that tree. The
        // tree is not a MariaDB source, so the build stops at once and nothing is compiled.
        {
          string berr = tmp + "/build_patch1.log";
          CmdResult bp = run_capture({self_exe(), "TEST_DIR=" + g_cfg.test_dir, "build", feat, "opt", "--patch-1"}, 600, tmp);
          st_check(bp.out.find("commits behind") != string::npos, "deep: build --patch-1 works out the base tree first [" + trim(tail_lines(bp.out, 1)) + "]");
          st_check(bp.rc != 0 && bp.out.find("FAILED") != string::npos, "deep: and a tree that is not a server source fails the build");
          run_capture({"git", "-C", feat, "worktree", "remove", "--force", feat + "-patch-1"}, 300);
          remove_tree(feat + "-patch-1");
          (void)berr;
        }
        // omnium build follow: every series the registry lists is pulled, and one that cannot be
        // pulled is reported. The registry here holds one series, pointing at this local repo.
        {
          string fh = tmp + "/followhome";
          mkdirs(fh);
          string ftest = tmp + "/followtest";
          mkdirs(ftest);
          write_file(fh + "/.omnium.conf", "TEST_DIR=" + ftest + "\nDATA_DIR=" + tmp + "/followdata\nQA_DIR=" + g_cfg.qa_dir + "\n");
          write_file(ftest + "/omnium.builds",
                     "# omnium basedir registry\nMD010126-mariadb-13.1.0-linux-x86_64-opt CS 13.1.0 plain opt 010126 yes yes hand -\n");
          mkdirs(ftest + "/MD010126-mariadb-13.1.0-linux-x86_64-opt");   // the line stays only while the basedir is there
          // the series dir the follow pulls: a copy of the local repo with no upstream to pull from
          copy_tree(origin, ftest + "/13.1", nullptr);
          CmdResult fo = run_capture({self_exe(), "build", "follow"}, 900, tmp, {"HOME=" + fh});
          st_check(fo.out.find("CS 13.1") != string::npos, "deep: build follow walks each series the registry lists [" + trim(tail_lines(fo.out, 1)) + "]");
          st_check(fo.out.find("pull failed") != string::npos || fo.out.find("up to date") != string::npos,
                   "deep: and says what happened to each one");
        }
        string near = groot + "/MDEV-2222-near";
        if (run_capture({"git", "clone", "-q", "--branch", "13.1", origin, near}, 600).rc == 0) {
          g({"checkout", "-q", "-b", "MDEV-2222-near"}, near);
          g({"commit", "-q", "--allow-empty", "-m", "one commit"}, near);
          string ntree, nnote, nerr;
          st_check(patch1_tree(near, ntree, nnote, &nerr) && ntree.empty(), "deep: a branch close to its base needs no tree of its own [" + nnote + nerr + "]");
        }
      }
    }
    g_cfg.test_dir = save_test;
  }
  // the by-hand server verbs in their other shapes
  {
    string ddir = tmp + "/own_datadir";
    st_check(quiet_call(cmd_fresh, {b.path, "--keep"}) == 0, "deep: fresh --keep leaves a running server alone");
    st_check(quiet_call(cmd_fresh, {b.path, "--datadir", ddir}) == 0, "deep: fresh with a datadir of its own");
    st_check(dir_exists(ddir), "deep: that datadir is there");
    st_check(quiet_call(cmd_fresh, {b.path, "--options"}) == 2, "deep: fresh says --options needs a value");
    st_check(quiet_call(cmd_fresh, {b.path, "--options", "--sql_mode= --log_bin"}) == 0, "deep: fresh with server options of its own");
  }
  // the matrix on one build, with a testcase that says what it needs
  string mx = tmp + "/mx.sql";
  write_file(mx, "# mysqld options required for replay: --sql_mode=\nSELECT 1;\nSELECT 2;\n");
  vector<Basedir> set;
  string err;
  st_check(matrix_builds({b.path}, set, &err), "deep: matrix_builds by path");
  MatrixResult m;
  st_check(matrix_run(mx, set, "", 1, m, &err), "deep: matrix_run");
  st_check(m.rows.size() == 1 && !m.rows[0].uid.empty(), "deep: the matrix row carries a verdict");
  string mtext = matrix_format(m);
  st_check(mtext.find("Bug Detection Matrix") != string::npos && mtext.find("UniqueID observed") != string::npos, "deep: the matrix block");
  st_check(quiet_call(cmd_matrix, {mx, b.path, "--slots", "1", "--out", tmp + "/mx.txt"}) == 0, "deep: matrix verb");
  st_check(!read_file(tmp + "/mx.txt").empty(), "deep: the matrix file is written");
  // a report with the matrix, on a fixture trial of that build
  string ftd = fixture_trial(tmp, 7, fixture_errlog(), "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|a|b|c");
  write_file(ftd + "/BASEDIR", b.path + "\n");
  st_check(quiet_call(cmd_report, {dirname_of(ftd), "7", b.path, "--slots", "1", "--no-mtr", "--out", tmp + "/inbox2"}) == 0, "deep: report with a matrix");
  // the same report with the MTR step left in: it writes the test, runs it and says what it saw
  {
    st_check(quiet_call(cmd_report, {dirname_of(ftd), "7", b.path, "--slots", "1", "--out", tmp + "/inbox_mtr"}) == 0, "deep: report with the MTR step");
    string mtext = read_file(tmp + "/inbox_mtr/O999999_bug7.report");
    st_check(!mtext.empty(), "deep: that report is written");
    st_check(file_exists(dirname_of(ftd) + "/bug7.test") || mtext.find("not made") != string::npos, "deep: it either wrote the .test file or said why not");
  }
  string rtext = read_file(tmp + "/inbox2/O999999_bug7.report");
  st_check(rtext.find("Bug Detection Matrix") != string::npos, "deep: the report carries the matrix");
  st_check(rtext.find(b.series) != string::npos, "deep: the matrix names the build under test");
  st_check(rtext.find("\nAffects ES: \n") != string::npos, "deep: with no ES build in the matrix, Affects ES stays empty");
  // an ES build in the matrix that does not show the bug: checked and not affected, so N/A
  {
    const Basedir* eb = nullptr;
    for (auto& x : all) if (x.es && x.flavour == Flavour::Plain && !x.dbg && !x.bin.empty() && x.tag.empty()) { eb = &x; break; }
    if (eb) {
      st_check(quiet_call(cmd_report, {dirname_of(ftd), "7", b.path, eb->path, "--slots", "2", "--no-mtr", "--out", tmp + "/inbox_es"}) == 0, "deep: report with an ES build in the matrix");
      string et = read_file(tmp + "/inbox_es/O999999_bug7.report");
      st_check(et.find("\nAffects ES: N/A\n") != string::npos && et.find("\nProject: MDEV\n") != string::npos, "deep: an ES build that ran without the bug gives Affects ES N/A");
    }
  }
  // a report whose matrix runs on a sanitizer build: that is what puts the Setup block in
  {
    const Basedir* sb = nullptr;
    for (auto& x : all) if (x.flavour == Flavour::UBASAN && !x.bin.empty() && !x.dbg) sb = &x;
    if (sb) {
      Basedir ub;
      if (basedir_probe(sb->path, ub)) {
        string std_ = fixture_trial(tmp + "/sanrep", 9, fixture_san_log(), "ASAN|heap-use-after-free|Item_func_case::val_int|a|b|c");
        write_file(std_ + "/BASEDIR", ub.path + "\n");
        string inbox = tmp + "/inbox_san";
        if (quiet_call(cmd_report, {dirname_of(std_), "9", ub.path, "--slots", "1", "--no-mtr", "--out", inbox}) == 0) {
          string text = read_file(inbox + "/O999999_bug9.report");
          st_check(text.find("Bug Detection Matrix") != string::npos, "deep: the sanitizer report has a matrix");
          st_check(text.find("UBASAN") != string::npos || text.find("12.3") != string::npos, "deep: the matrix names the sanitizer build");
          // the Setup block is only written when the sanitizer build reproduced the bug; the
          // testcase here does not, so its absence is right and only its shape is checked
          if (text.find("Setup:") != string::npos) st_check(text.find("WITH_ASAN") != string::npos, "deep: the Setup block names the sanitizer build flags");
        }
      }
    }
  }
  // The Setup block of a report. It is written only when a sanitizer build in the matrix shows the
  // bug, so the build here is the real one with a sanitizer report prepended to its error log by a
  // wrapper, and a name and cmake line that say it is a UBSAN+ASAN build.
  {
    // one stand-in per sanitizer flavour: the real server, its error log replaced by a report of
    // that flavour, under a name and a cmake line that say which build it is
    struct San { const char* dir; const char* cmake; const char* log; const char* want; const char* compiler; };
    string tsan_log =
        "==1==WARNING: ThreadSanitizer: data race (pid=1)\n"
        "  Write of size 8 at 0x7b04 by thread T3:\n"
        "    #0 my_thread_var_ptr /test/13.1/mysys/my_thr_init.c:100 (mariadbd+0x1)\n"
        "  Previous read of size 8 at 0x7b04 by thread T2:\n"
        "    #0 my_thread_var_ptr /test/13.1/mysys/my_thr_init.c:100 (mariadbd+0x1)\n"
        "SUMMARY: ThreadSanitizer: data race /test/13.1/mysys/my_thr_init.c:100 in my_thread_var_ptr\n";
    string msan_log =
        "==1==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
        "    #0 0x1 in Item_func_case::val_int() /test/13.1/sql/item_cmpfunc.cc:2000:5\n"
        "SUMMARY: MemorySanitizer: use-of-uninitialized-value /test/13.1/sql/item_cmpfunc.cc:2000:5 in Item_func_case::val_int()\n";
    const San sans[] = {
      {"UBASAN_MD030926-mariadb-13.1.0-linux-x86_64-opt", "cmake . -DCMAKE_C_COMPILER=/usr/bin/clang -O1 -DWITH_ASAN=ON\n", nullptr, "ASAN_OPTIONS", "Clang"},
      {"TSAN_MD030926-mariadb-13.1.0-linux-x86_64-opt", "cmake . -DCMAKE_C_COMPILER=/usr/bin/clang -O1 -DWITH_TSAN=ON\n", nullptr, "TSAN_OPTIONS", "Clang"},
      {"MSAN_MD030926-mariadb-13.1.0-linux-x86_64-opt", "cmake . -DCMAKE_C_COMPILER=/usr/bin/clang -O1 -DWITH_MSAN=ON\n", nullptr, "MSAN_OPTIONS", "Clang"},
      {"VAL_MD030926-mariadb-13.1.0-linux-x86_64-opt", "cmake . -DWITH_VALGRIND=ON\n", nullptr, "WITH_VALGRIND=ON", "GCC"},
    };
    int sidx = 0;
    for (auto& sn : sans) {
      string flav(sn.dir, 3);
      string sanlog = flav == "TSA" ? tsan_log : flav == "MSA" ? msan_log : fixture_san_log();
      string sanuid = flav == "TSA" ? "TSAN|data race|my_thread_var_ptr|a|b|c"
                      : flav == "MSA" ? "MSAN|use-of-uninitialized-value|Item_func_case::val_int|a|b|c"
                                      : "ASAN|heap-use-after-free|Item_func_case::val_int|a|b|c";
      string ubd = tmp + "/" + sn.dir;
      string ubin = basename_of(b.bin);
      mkdirs(ubd + "/bin");
      std::error_code uec;
      for (auto& e : fs::directory_iterator(b.path, uec)) {
        string n = e.path().filename().string();
        if (n == "bin") continue;
        if (symlink(e.path().c_str(), (ubd + "/" + n).c_str()) != 0) { /* already there */ }
      }
      for (auto& e : fs::directory_iterator(b.path + "/bin", uec)) {
        string n = e.path().filename().string();
        if (n == ubin) continue;
        if (symlink(e.path().c_str(), (ubd + "/bin/" + n).c_str()) != 0) { /* already there */ }
      }
      // the sanitizer report is all that is in the error log the tools read: the server's own lines
      // go to a second file, because a report followed by ordinary log lines yields no UniqueID
      write_file(ubd + "/bin/" + ubin,
                 "#!/bin/sh\nLE=\nfor a in \"$@\"; do case \"$a\" in --log-error=*) LE=${a#--log-error=};; esac; done\n"
                 "if [ -n \"$LE\" ]; then mkdir -p \"$(dirname \"$LE\")\"; cat > \"$LE\" <<'SANEOF'\n" +
                 sanlog + "SANEOF\n"
                 "exec '" + b.bin + "' \"$@\" --log-error=\"$LE.server\"\nfi\nexec '" + b.bin + "' \"$@\"\n");
      chmod((ubd + "/bin/" + ubin).c_str(), 0755);
      write_file(ubd + "/BUILD_CMD_CMAKE", sn.cmake);
      Basedir wb;
      if (!basedir_probe(ubd, wb)) { st_check(false, string("deep: the ") + flav + " stand-in probes"); continue; }
      long tn = 20 + sidx++;
      string wtd = fixture_trial(tmp + "/setupblk" + std::to_string(tn), tn, sanlog, sanuid);
      write_file(wtd + "/BASEDIR", ubd + "\n");
      string wbox = tmp + "/inbox_setup" + std::to_string(tn);
      if (quiet_call(cmd_report, {dirname_of(wtd), std::to_string(tn), ubd, "--slots", "1", "--no-mtr", "--out", wbox}) != 0) continue;
      string wt = read_file(wbox + "/O999999_bug" + std::to_string(tn) + ".report");
      st_check(wt.find("Setup:") != string::npos, string("deep: a ") + flav + " build that shows the bug puts the Setup block in the report");
      st_check(wt.find(sn.want) != string::npos, string("deep: and the block names what a ") + flav + " build needs");
      st_check(wt.find(sn.compiler) != string::npos, string("deep: and that a ") + flav + " build is compiled with " + sn.compiler);
    }
  }
  // the MTR form, made and verified on that build
  MtrTest t;
  st_check(mtr_make("SELECT 1;\nCREATE TABLE t1 (a INT);\nDROP TABLE t1;\n", "", &b, "", t, &err), "deep: mtr_make with a replay");
  st_check(t.replayed, "deep: the MTR conversion replayed the SQL");
  MtrVerdict v;
  // what a killed verify left three hours ago goes; a file of a verify that may still run stays
  string left = basedir_test_dir(b) + "/main/omnium_st_left_" + std::to_string(getpid()) + ".test";
  string fresh = basedir_test_dir(b) + "/main/omnium_st_fresh_" + std::to_string(getpid()) + ".test";
  write_file(left, "SELECT 1;\n");
  write_file(fresh, "SELECT 1;\n");
  { struct timeval tv[2] = {{(time_t)now_s() - 10800, 0}, {(time_t)now_s() - 10800, 0}}; utimes(left.c_str(), tv); }
  st_check(mtr_verify(b, t, "selftest", v, &err), "deep: mtr_verify runs the test");
  st_check(v.pass, "deep: a testcase without a bug passes under MTR");
  st_check(!file_exists(basedir_test_dir(b) + "/main/omnium_selftest.test"), "deep: the test file is removed again");
  st_check(!file_exists(left), "deep: a test file a killed verify left behind is swept");
  st_check(file_exists(fresh), "deep: and a new one is left alone");
  fs::remove(fresh);
  // server options: a dynamic one becomes SET GLOBAL with its restore, a read-only one an .opt line
  {
    MtrTest to;
    st_check(mtr_make("SELECT 1;\n", "--max_connections=100 --innodb_page_size=16k", &b, "", to, &err), "deep: mtr_make with server options");
    st_check(to.test.find("SET GLOBAL max_connections=100;") != string::npos && to.test.find("SET GLOBAL max_connections=DEFAULT;") != string::npos,
             "deep: a dynamic server option becomes SET GLOBAL and is set back at the end");
    st_check(to.opt.find("--innodb_page_size=16k") != string::npos, "deep: a read-only one goes in the .opt file");
  }
  // the MTR verb on a plain SQL file, which is the form a testcase from anywhere takes
  {
    string mfile = tmp + "/hand.sql";
    write_file(mfile, "# mysqld options required for replay: --sql_mode=\n"
                      "CREATE TABLE t1 (a INT, b INT) ENGINE=InnoDB PARTITION BY HASH(a) PARTITIONS 2;\n"
                      "INSERT INTO t1 VALUES (1,1),(2,2);\n"
                      "SELECT COUNT(*) FROM t1;\n"
                      "DROP TABLE t1;\n");
    st_check(quiet_call(cmd_mtr, {mfile, "--basedir", b.path, "--no-verify", "--out", tmp + "/hand.test"}) == 0, "deep: mtr on a plain SQL file");
    string t = read_file(tmp + "/hand.test");
    st_check(t.find("CREATE TABLE") != string::npos, "deep: the .test file holds the testcase");
    // verified: the test passes, and with no bug to show it has no gate, which is an answer of 1
    int mrc = -1;
    string mo = call_output(cmd_mtr, {mfile, "--basedir", b.path, "--out", tmp + "/hand2.test"}, &mrc);
    st_check(mrc == 1 && mo.find("# verify on " + b.name + ": pass\n") != string::npos && mo.find("the test has no gate") != string::npos,
             "deep: mtr on a plain SQL file, verified");
    st_check(quiet_call(cmd_mtr, {tmp + "/no_such.sql", "--basedir", b.path, "--no-verify"}) != 0, "deep: mtr says no for a file that is not there");
    // a testcase where some statements fail: the MTR form has to carry the error each one gave,
    // by name, or the test would fail on the error instead of on the bug
    string efile = tmp + "/errs.sql";
    write_file(efile, "# mysqld options required for replay: --sql_mode=\n"
                      "CREATE TABLE t1 (a INT);\n"
                      "SELECT no_such_column_here FROM t1;\n"
                      "SELECT ROW_NUMBER() OVER w FROM t1;\n"
                      "INSERT INTO t1 VALUES (1);\n"
                      "CREATE TABLE t1 (a INT);\n"
                      "DROP TABLE t1;\n"
                      "DROP TABLE t1;\n");
    string eo = call_output(cmd_mtr, {efile, "--basedir", b.path, "--out", tmp + "/errs.test"}, &mrc);
    st_check(mrc == 1 && eo.find("# verify on " + b.name + ": pass\n") != string::npos, "deep: mtr on a testcase whose statements fail: the test passes with them");
    string et = read_file(tmp + "/errs.test");
    st_check(et.find("--error ") != string::npos, "deep: and the .test file carries the errors by name [" + et.substr(0, 0) + "]");
    st_check(et.find("ER_BAD_FIELD_ERROR") != string::npos || et.find("ER_TABLE_EXISTS_ERROR") != string::npos || et.find("ER_BAD_TABLE_ERROR") != string::npos,
             "deep: the error names are the server's own");
    st_check(et.find("--error ER_WRONG_WINDOW_SPEC_NAME") != string::npos, "deep: an error code of 4000 and up gets its line too");
    // a stored routine needs the delimiter lines around it
    string dfile = tmp + "/deli.sql";
    write_file(dfile, "# mysqld options required for replay: --sql_mode=\n"
                      "CREATE PROCEDURE p1() BEGIN SELECT 1; SELECT 2; END;\n"
                      "CALL p1();\n"
                      "DROP PROCEDURE p1;\n");
    st_check(quiet_call(cmd_mtr, {dfile, "--basedir", b.path, "--no-verify", "--out", tmp + "/deli.test"}) == 0, "deep: mtr on a testcase with a stored routine");
    string dt = read_file(tmp + "/deli.test");
    st_check(dt.find("DELIMITER |;") != string::npos, "deep: and it sets the delimiter around the routine");
  }
  // the reduction plan of a fixture trial
  {
    int prc = -1;
    string po = call_output(cmd_reduce, {dirname_of(ftd), "7", "--plan", "--basedir", b.path}, &prc);
    st_check(prc == 0 && po.find("server     " + b.path + "\n") != string::npos && po.find("mode       3, UID compare\n") != string::npos,
             "deep: reduce --plan names the build and the mode");
  }
  // a real core: a server started by hand, then SIGABRT. Nothing here is a bug in the server; the
  // point is the core-reading chain: gdb, the stack, the UniqueID, the ldd bundle.
  {
    const Basedir* dbgb = nullptr;
    for (auto& x : all) if (x.flavour == Flavour::Plain && x.dbg && !x.es && !x.bin.empty()) if (!dbgb || version_cmp(x.version, dbgb->version) > 0) dbgb = &x;
    Basedir cb;
    if (dbgb && basedir_probe(dbgb->path, cb)) {
      string croot = g_cfg.shm_dir + "/Ocore" + std::to_string(getpid());
      remove_tree(croot);
      mkdirs(croot);
      Instance inst;
      inst.bd = &cb;
      inst.set_paths(croot);
      string ctpl = template_for(cb, "", croot + "/templates");
      if (!ctpl.empty() && inst.start_fresh(ctpl, 120)) {
        kill(inst.pid, SIGABRT);
        for (int i = 0; i < 120 && !inst.has_core(); i++) usleep(500000);
        st_check(inst.has_core(), "deep: the killed server left a core");
        inst.kill_hard();
        if (inst.has_core()) {
          UidResult r;
          UidOptions o;
          o.wait_core = true;
          bool ok = uid_for_dir(croot, r, o);
          st_check(ok && !r.uid.empty(), "deep: the UniqueID chain reads a real core");
          st_check(r.uid.find("SIGABRT") != string::npos || r.uid.find("|") != string::npos, "deep: the UID has the shape of a UID");
          string serr;
          string stack = stack_text(croot, basedir_banner_title(cb), &serr);
          st_check(stack.find("noformat") != string::npos, "deep: the stack comes back as a Jira block");
          // the same UID through the shell chain: this port has to agree with it
          CmdResult sh = run_capture({script_path("new_text_string.sh")}, 3600, croot);
          if (sh.rc == 0 && !trim(sh.out).empty()) st_eq(trim(r.uid), trim(sh.out), "deep: the UID matches new_text_string.sh");
          // the ldd bundle with a core: gdb lists the libraries that core was made with
          string bdir = croot + "/bundle";
          mkdirs(bdir);
          vector<string> files;
          string lerr;
          st_check(ldd_copy(cb.bin, bdir, inst.core_path(), &files, &lerr), "deep: ldd_copy with a core");
          st_check(files.size() > 3, "deep: the bundle holds the libraries");
          // a trial dir with a real core, taken through report and reduce planning
          string ctd = croot + "/O999998/1";
          mkdirs(ctd + "/log");
          write_file(croot + "/O999998/omnium.ledger", "1 run created x\n");
          copy_file(inst.errlog, ctd + "/log/master.err");
          copy_file(inst.core_path(), ctd + "/core");
          write_file(ctd + "/MYBUG", r.uid + "\n");
          write_file(ctd + "/BASEDIR", cb.path + "\n");
          write_file(ctd + "/default.node.tld_thread-0.sql", "SELECT 1;\nSELECT 2;\n");
          int prc = -1;
          string po = call_output(cmd_reduce, {croot + "/O999998", "1", "--plan"}, &prc);
          st_check(prc == 0 && po.find("core       " + ctd + "/core\n") != string::npos && po.find("server     " + cb.path + "\n") != string::npos,
                   "deep: reduce --plan on a trial with a core names the core and its build");
          st_check(quiet_call(cmd_report, {croot + "/O999998", "1", "--no-matrix", "--no-mtr", "--out", tmp + "/inbox3"}) == 0, "deep: report on a trial with a core");
          string ctext = read_file(tmp + "/inbox3/O999998_bug1.report");
          st_check(ctext.find("{noformat:title=") != string::npos, "deep: that report carries the stack block");
        }
      }
      inst.kill_hard();
      remove_tree(croot);
    }
  }
  // the SQL sources: one file per area, as a trial assembles it
  {
    // an area that encrypts: the key files are made per trial with openssl
    string eout = tmp + "/sql_encrypted.sql";
    if (area_by_name("aria-encryption")) {
      st_check(quiet_call(cmd_sql, {b.path, "--area", "aria-encryption", "--out", eout, "--no-disk", "--lines", "200"}) == 0, "deep: sql for an area that encrypts");
      st_check(!read_file(eout).empty(), "deep: that area wrote SQL");
    }
    // every .sql file on the disk as a fourth source
    bool save_disk = g_cfg.all_disk_sql;
    g_cfg.all_disk_sql = true;
    string dout = tmp + "/sql_disk.sql";
    st_check(quiet_call(cmd_sql, {b.path, "--area", "default", "--out", dout, "--lines", "300"}) == 0, "deep: sql with the disk source on");
    st_check(!read_file(dout).empty(), "deep: the disk source wrote SQL");
    g_cfg.all_disk_sql = save_disk;
  }
  for (const char* an : {"default", "engines", "optimizer"}) {
    const Area* ar = area_by_name(an);
    if (!ar) continue;
    string out = tmp + "/sql_" + an + ".sql";
    st_check(quiet_call(cmd_sql, {b.path, "--area", an, "--out", out, "--no-disk", "--lines", "200"}) == 0, string("deep: sql for the ") + an + " area");
    st_check(!read_file(out).empty(), string("deep: the ") + an + " area wrote SQL");
  }
  // the governor: with the RAM cap at 1% no trial may start, and the run says so and ends
  {
    int cap = g_cfg.ram_cap_pct;
    g_cfg.ram_cap_pct = 1;
    st_deep_clean(st_deep_run({b.path, "--trials", "1", "--slots", "1", "--seconds", "5", "--for", "8", "--no-disk"},
                              "deep: a run under a closed governor waits, then ends on its --for window"));
    g_cfg.ram_cap_pct = cap;
  }
  {                                                            // the sweep only ever touches its own run directories
    string outside = st_tmp() + "/not_a_run";
    mkdirs(outside);
    st_deep_clean(outside);
    st_check(dir_exists(outside), "deep: the sweep leaves a directory that is not one of its runs");
    remove_tree(outside);
  }
  st_deep_clean(wd);
  for (auto& k : g_deep_kept)
    if (dir_exists(k)) fprintf(stderr, "selftest: %s saved a trial that is not a known bug, so it stays; omnium status %s\n", k.c_str(), basename_of(k).c_str());
  g_deep_kept.clear();
  g_cfg.infile = infile_save;
  g_cfg.auto_pipeline = before_auto == "1";
  {                                                            // the by-hand server goes too
    string root = g_cfg.shm_dir + "/Ofresh_" + b.short_name();
    string pidtxt = read_file(root + "/server.txt");
    for (auto& l : split_lines(pidtxt)) if (starts_with(l, "pid=")) { pid_t p = (pid_t)to_long(l.substr(4), 0); if (p > 0) kill_group(p, SIGKILL); }
    remove_tree(root);
  }
  remove_tree(tmp);
}

static void st_detect_classes();
static void st_detect_parity();
static void st_jira_stub();
static void st_sources_plain();
static void st_plumbing();
static void st_build_plain();
static void st_registry_plain();
static void st_view_and_config();
static void st_detect_more();
static void st_reduce_and_report();
static void st_corners();
static void st_units();
static void st_units_tail();
static void st_inbox_corners();
static void st_fix_version_units();
static void st_windows();
static void st_portability();

// the UID a backup issue gets: stable across the numbers in a mariadb-backup line

// the paths a Windows box takes, run here: the TCP endpoint, the install tool's option set, the
// /proc/meminfo without MemAvailable, the MX answer read by hand, and the other fixes with a unit
static void st_portability() {
  string tmp = st_tmp() + "/port";
  mkdirs(tmp);
  // the endpoint: socket by default, TCP for a Windows build or OMNIUM_TCP=1
  {
    Endpoint sk{"/tmp/x/socket.sock", 13001, false}, tc{"/tmp/x/socket.sock", 13001, true};
    st_eq(join(endpoint_args(sk), " "), "-S/tmp/x/socket.sock", "endpoint_args: the socket");
    st_eq(join(endpoint_args(tc), " "), "-h127.0.0.1 -P13001 --protocol=tcp --skip-ssl", "endpoint_args: TCP on the loopback, without TLS");
    Basedir lin, win;
    basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", lin);
    basedir_parse_name("MD120926-mariadb-13.1.1-windows-x86_64-dbg", win);
    unsetenv("OMNIUM_TCP");
    st_check(!endpoint_tcp(lin) && endpoint_tcp(win), "endpoint_tcp: a Windows build is TCP, a Linux one is not");
    setenv("OMNIUM_TCP", "1", 1);
    st_check(endpoint_tcp(lin), "endpoint_tcp: OMNIUM_TCP=1 makes a Linux build TCP");
    Instance i;
    i.bd = &lin;
    i.port = 14001;
    i.set_paths(tmp + "/inst");
    st_check(i.tcp && i.endpoint().tcp && i.endpoint().port == 14001, "an instance takes the endpoint from its build and the environment");
    string argv = join(i.argv(), " ");
    st_check(argv.find("--socket=") != string::npos, "a Linux server still gets --socket= under OMNIUM_TCP (the default socket would clash between trials)");
    unsetenv("OMNIUM_TCP");
    i.set_paths(tmp + "/inst");
    st_check(!i.tcp, "and without it the instance is back on the socket");
    Instance w;
    w.bd = &win;
    w.port = 14002;
    w.set_paths(tmp + "/winst");
    argv = join(w.argv(), " ");
    st_check(w.tcp && argv.find("--socket=") == string::npos && argv.find("--port=14002") != string::npos, "a Windows server gets --port= and no --socket=");
    Basedir ms;
    basedir_parse_name("MS070525-mysql-5.7.44-linux-x86_64-opt", ms);
    Instance mi;
    mi.bd = &ms;
    mi.port = 14003;
    mi.set_paths(tmp + "/minst");
    st_check(join(mi.argv(), " ").find("--server-id=100") != string::npos && argv.find("--server-id") == string::npos,
             "a MySQL server gets the server id its binlog needs; a MariaDB one keeps its own");
    // the helpers of a saved trial: the start one carries the same MySQL options, and a trial with a
    // core gets the gdb one, which prefers the copy of the binary in the workdir
    string hdir = tmp + "/helpers";
    mkdirs(hdir);
    write_helpers(hdir, ms, mi, "--log_bin", true);
    st_check(read_file(hdir + "/start").find("--server-id=100") != string::npos, "write_helpers: the start helper of a MySQL trial has the server id");
    string g = read_file(hdir + "/gdb");
    st_check(is_executable(hdir + "/gdb") && g.find("../mysqld/") != string::npos && g.find("./data*/*core*") != string::npos, "write_helpers: a trial with a core gets the gdb helper");
    st_check(is_executable(hdir + "/stop") && is_executable(hdir + "/cl"), "write_helpers: and the stop and cl ones");
  }
  // the install tool's command line
  {
    Basedir lin, win;
    basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", lin);
    lin.path = "/test/L"; lin.init_tool = "/test/L/scripts/mariadb-install-db";
    basedir_parse_name("MD120926-mariadb-13.1.1-windows-x86_64-dbg", win);
    win.path = "/c/test/W"; win.init_tool = "/c/test/W/bin/mariadb-install-db";
    string l = join(install_db_argv(lin, "/tmp/tpl", "/tmp/tpl.tmp", "--innodb-page-size=4k --innodb_undo_tablespaces=3"), " ");
    st_eq(l, "/test/L/scripts/mariadb-install-db --no-defaults --force --basedir=/test/L --datadir=/tmp/tpl --tmpdir=/tmp/tpl.tmp --auth-root-authentication-method=normal --innodb-page-size=4k --innodb_undo_tablespaces=3",
          "install_db_argv: the Linux script with MYINIT");
    Basedir tsan = lin;
    basedir_parse_name("TSAN_MD180826-mariadb-13.1.0-linux-x86_64-dbg", tsan);
    tsan.path = "/test/L"; tsan.init_tool = "/test/L/scripts/mariadb-install-db";
    st_check(join(install_db_argv(tsan, "/tmp/tpl", "/tmp/tpl.tmp", ""), " ").find("--loose-innodb-buffer-pool-size-max") != string::npos, "install_db_argv: a TSAN build gets the buffer pool cap");
    st_eq(join(install_db_argv(win, "/c/tpl", "/c/tpl.tmp", "--innodb_page_size=4k --innodb_undo_tablespaces=3"), " "),
          "/c/test/W/bin/mariadb-install-db --datadir=/c/tpl --innodb-page-size=4k", "install_db_argv: the Windows tool takes the datadir and the page size only");
    st_eq(join(install_db_argv(win, "/c/tpl", "/c/tpl.tmp", ""), " "), "/c/test/W/bin/mariadb-install-db --datadir=/c/tpl", "install_db_argv: the Windows tool with no MYINIT");
    // a testcase's page size reaches the template through MYINIT, and so do the other options that shape a datadir
    if (kTakeFixes) {
      st_eq(myinit_from("--log-bin --innodb_page_size=4k --sql_mode= --innodb-buffer-pool-size=5M"), "--innodb_page_size=4k", "myinit_from: the page size out of a header's options");
      st_eq(myinit_from("--Innodb-Undo-Tablespaces=3 --lower_case_table_names=1 --log-bin"), "--Innodb-Undo-Tablespaces=3 --lower_case_table_names=1", "myinit_from: either spelling, any case");
      st_eq(myinit_from("--log-bin --innodb_file_per_table=0"), "", "myinit_from: no init option, no MYINIT");
    } else {
      st_eq(myinit_from("--innodb_page_size=4k"), "", "myinit_from: Linux keeps its behaviour, no MYINIT from the options");
    }
  }
  // the trial's backup and crash-recovery stages: a root turned away is no finding, and the prepare of an encrypted backup
  // gets the key plugin the server ran with (--no-defaults leaves out the backup-my.cnf that has it)
  {
    st_check(root_turned_away("connect kept failing: 1045 Access denied for user 'root'@'localhost' (using password: NO)"), "root_turned_away: error 1045");
    st_check(root_turned_away("connect kept failing: 1130 Host 'localhost' is not allowed to connect to this MariaDB server") == kTakeFixes, "root_turned_away: error 1130, where the fixes are taken");
    st_check(!root_turned_away("[ERROR] Aborting in the error log: Plugin 'file_key_management' init function returned error"), "root_turned_away: a start that really failed is not a turned-away root");
    st_check(backup_is_encrypted({"--log-bin", "--plugin_load_add=file_key_management", "--file-key-management-filename=/x/key.enc"}), "backup_is_encrypted: the key plugin loaded");
    st_check(backup_is_encrypted({"--file_key_management_use_pbkdf2=11000"}), "backup_is_encrypted: a parameter of it alone");
    st_check(!backup_is_encrypted({"--log-bin", "--plugin_load_add=other_plugin", "--innodb-encrypt-tables=1"}), "backup_is_encrypted: other plugins and options are not it");
  }
  // a Windows server's crash leaves a minidump in its datadir, which counts as the core of a Linux one; no other build has it
  {
    Basedir wb, lb;
    basedir_parse_name("MD180826-mariadb-13.1.0-windows-x86_64-opt", wb);
    basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", lb);
    Instance ins;
    ins.bd = &wb;
    ins.datadir = tmp + "/dumpdata";
    mkdirs(ins.datadir);
    st_check(!ins.has_dump(), "has_dump: no minidump, no crash");
    write_file(ins.datadir + "/mariadbd.dmp", "MDMP");
    st_check(ins.has_dump() && !ins.has_core(), "has_dump: a minidump of a Windows server, and it is no core");
    ins.bd = &lb;
    st_check(!ins.has_dump(), "has_dump: a Linux build never has one");
    // a Windows release server that dies of a failed /GS check or a __fastfail leaves no banner and no dump, and Cygwin
    // reports it as exit status 127: that is a crash, unless omnium stopped it, it exited cleanly, or it left a dump
    Instance sd;
    sd.bd = &wb;
    sd.datadir = tmp + "/silentdata";
    mkdirs(sd.datadir);
    sd.exit_status = 127 << 8;
    st_check(sd.silent_death() && sd.silent_death_uid() == "CRASH_NO_LOG|exit status 127", "silent_death: a server that ended on its own with status 127 [" + sd.silent_death_uid() + "]");
    sd.exit_status = 11;                                         // killed by a signal
    st_check(sd.silent_death() && sd.silent_death_uid() == "CRASH_NO_LOG|exit status 139", "and one that a signal ended");
    // with the native process's own status the UID says what killed it, as Cygwin's 139 and 127 do not
    sd.win_status = 0xC00000FD;
    sd.win_status_known = true;
    st_check(sd.silent_death_uid() == "CRASH_NO_LOG|exit code 0xC00000FD" && sd.silent_death_note() == " (a stack overflow)", "silent_death: the NTSTATUS of a stack overflow [" + sd.silent_death_uid() + "]");
    sd.win_status = 0xC0000409;
    st_check(sd.silent_death_uid() == "CRASH_NO_LOG|exit code 0xC0000409" && icontains(sd.silent_death_note(), "stack cookie"), "and the one of a failed stack cookie check");
    sd.win_status_known = false;
    sd.exit_status = 0;
    st_check(!sd.silent_death(), "silent_death: a clean exit is no crash");
    sd.exit_status = 127 << 8;
    sd.stopping = true;
    st_check(!sd.silent_death(), "silent_death: nor is the end omnium asked for");
    sd.stopping = false;
    write_file(sd.datadir + "/mariadbd.dmp", "MDMP");
    st_check(!sd.silent_death(), "silent_death: nor one that left a minidump, which is the ordinary crash");
    sd.bd = &lb;
    st_check(!sd.silent_death(), "silent_death: and never a Linux build");
  }
  // the copy of the binary that a long-running verb starts from under MSYS2, where a replaced exe stops fork(): made once
  // per exe, the same path the second time, and of the same size; nothing off MSYS2
  {
    string fake = tmp + "/fake_omnium.exe";
    copy_file(kHostMsys2 ? "/bin/true.exe" : "/bin/true", fake);
    string c1 = private_exe_copy(fake);
    if (kHostMsys2) {
      st_check(starts_with(c1, "/tmp/omnium_exe/omnium-") && ends_with(c1, ".exe") && file_size(c1) == file_size(fake), "private_exe_copy: a copy of the same size, in /tmp/omnium_exe [" + c1 + "]");
      st_eq(private_exe_copy(fake), c1, "and the same copy the second time");
      st_check(private_exe_copy(tmp + "/no_such_exe.exe").empty(), "and none for a file that is not there");
      unlink(c1.c_str());
    } else {
      st_check(c1.empty(), "private_exe_copy: nothing off MSYS2");
    }
    unlink(fake.c_str());
    // Windows will not rename a folder that holds a file another process has open (a crashed server's minidump, still being
    // written or scanned); move_tree tries again until the file is let go. A native process holds one here for a few seconds.
    if (kHostMsys2 && file_exists("/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")) {
      string d = tmp + "/mv_held", marker = tmp + "/mv_marker.txt";
      mkdirs(d);
      write_file(d + "/held.dmp", "MDMP");
      string script = "$fs = [System.IO.File]::Open('" + native_path(d + "/held.dmp") + "', 'Open', 'ReadWrite', 'Read'); Set-Content '" + native_path(marker) +
                      "' 'locked'; Start-Sleep -Seconds 4; $fs.Close()";
      pid_t hp = spawn_program({"/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "-NoProfile", "-Command", script}, tmp + "/mv_ps.log", "", true);
      for (int i = 0; i < 200 && hp > 0 && !file_exists(marker); i++) usleep(100000);
      string mvwhy;
      double t0 = now_ms();
      bool moved = file_exists(marker) && move_tree(d, tmp + "/mv_moved", &mvwhy);
      double took = (now_ms() - t0) / 1000.0;
      st_check(moved && dir_exists(tmp + "/mv_moved") && !dir_exists(d), "move_tree: a folder with a file another process holds is moved once the file is let go [" + mvwhy + "]");
      st_check(took > 1.0, fmt("and it waited for that (%.1f s)", took));
      if (hp > 0) wait_pid(hp, 20000);
      // a lock that outlasts the retries (a crashed server held by Windows Error Reporting, for half a minute): the
      // folder is copied, which works, and what cannot be removed yet is left
      string d2 = tmp + "/mv_held2", marker2 = tmp + "/mv_marker2.txt";
      mkdirs(d2);
      write_file(d2 + "/held.dmp", "MDMP2");
      string script2 = "$fs = [System.IO.File]::Open('" + native_path(d2 + "/held.dmp") + "', 'Open', 'ReadWrite', 'Read'); Set-Content '" + native_path(marker2) +
                       "' 'locked'; Start-Sleep -Seconds 16; $fs.Close()";
      pid_t hp2 = spawn_program({"/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", "-NoProfile", "-Command", script2}, tmp + "/mv_ps2.log", "", true);
      for (int i = 0; i < 200 && hp2 > 0 && !file_exists(marker2); i++) usleep(100000);
      string mvwhy2;
      bool moved2 = file_exists(marker2) && move_tree(d2, tmp + "/mv_copied", &mvwhy2);
      st_check(moved2 && read_file(tmp + "/mv_copied/held.dmp") == "MDMP2", "move_tree: a lock that outlasts the retries is got round by a copy [" + mvwhy2 + "]");
      if (hp2 > 0) wait_pid(hp2, 30000);
      remove_tree(d2);
    }
    // a frameless silent-death UID is shared by every cause, so the per-UID cap keys it by what the server was asked last
    {
      string sd = tmp + "/sdstmt";
      mkdirs(sd);
      auto key_of = [&](const string& sql) { write_file(sd + "/default.node.tld_thread-0.last.sql", sql); return silent_death_statement(sd); };
      st_eq(key_of("CALL sp1;\n(((SELECT SQL_BUFFER_RESULT * FROM t4 WHERE 1) FOR UPDATE))\n"), "SELECT SQL_BUFFER_RESULT", "silent_death_statement: the last statement, past its opening brackets");
      st_eq(key_of("INSERT INTO t VALUES (1);\nDROP TABLE `aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa`.`b`;\n"), "DROP TABLE", "and a DROP TABLE of any name is one cause");
      st_eq(key_of("with recursive x as (select 1) select * from x;\n"), "WITH RECURSIVE", "and any case");
      st_eq(key_of("INSERT HIGH_PRIORITY INTO t1 VALUES (1) ON DUPLICATE KEY UPDATE a = 2;"), "INSERT HIGH_PRIORITY", "and the second word of an INSERT with a modifier");
      st_eq(silent_death_statement(tmp + "/no_such_trial"), "", "and nothing for a trial with no last statement");
    }
    // a HOME that is the Windows profile, with no settings file, while the MSYS2 home has one, is a mistake of the environment
    {
      st_eq(msys2_home_fix("/c/Users/R", "/c/Users/R", "/home/R", false, true), "/home/R", "msys2_home_fix: the profile as HOME with no settings file, the MSYS2 home has one: the MSYS2 home");
      st_eq(msys2_home_fix("/c/Users/R", "/c/Users/R", "/home/R", true, true), "", "msys2_home_fix: not when the profile has a settings file of its own");
      st_eq(msys2_home_fix("/c/Users/R", "/c/Users/R", "/home/R", false, false), "", "msys2_home_fix: not when the MSYS2 home has none either");
      st_eq(msys2_home_fix("/home/R", "/c/Users/R", "/home/R", false, true), "", "msys2_home_fix: not for the MSYS2 home itself");
      st_eq(msys2_home_fix("/tmp/omnium_st_home", "/c/Users/R", "/home/R", false, true), "", "msys2_home_fix: not for any other HOME, a check's own for one");
    }
    // the CPU time of a process: readable, and a busy loop moves it
    {
      long c0 = proc_cpu_ms(getpid());
      double until = now_ms() + 300;
      volatile unsigned long spin = 0;
      while (now_ms() < until) spin = spin + 1;
      long c1 = proc_cpu_ms(getpid());
      st_check(c0 >= 0 && c1 > c0 && proc_cpu_ms(-1) == -1, fmt("proc_cpu_ms: the CPU time of a process, which a busy loop moves (%ld, %ld ms)", c0, c1));
    }
    // the native process behind a Cygwin pid: a handle to it while it runs, and its exit status after it ended
    if (kHostMsys2 && file_exists("/c/Windows/System32/ping.exe")) {
      pid_t pp = spawn_program({"/c/Windows/System32/ping.exe", "-n", "3", "127.0.0.1"}, tmp + "/ping.log", "", true);
      uintptr_t wh = 0;
      for (int i = 0; i < 50 && pp > 0 && !wh; i++) { wh = winproc_open(pp, "ping"); if (!wh) usleep(100000); }
      uint32_t wcode = 99;
      st_check(wh != 0 && !winproc_exit_status(wh, &wcode), "winproc: a handle to the native process, which runs");
      st_check(winproc_open(pp, "no_such_image") == 0, "winproc: and none when the image is not the one asked for");
      if (pp > 0) wait_pid(pp, 20000);
      st_check(wh != 0 && winproc_exit_status(wh, &wcode) && wcode == 0, "winproc: its exit status after it ended");
      winproc_close(wh);
    }
    // a Windows path given as an argument is absolute there, not a name under the working directory
    if (kHostMsys2) {
      st_eq(abs_path("C:/Windows"), "/c/Windows", "abs_path: C:/x is the drive's path");
      st_eq(abs_path("C:\\Windows"), "/c/Windows", "abs_path: so is C:\\x");
    }
  }
  // a server option with a path, for a native Windows server: FILE:/path is converted here, as the MSYS2 runtime
  // only converts a plain --opt=/path, and a value with no path stays as it is
  {
    string n = native_option("--file-key-management-filekey=FILE:/dev/shm/O1/9/key.pass");
    if (kHostMsys2) {
      st_check(starts_with(n, "--file-key-management-filekey=FILE:") && n.find("FILE:/") == string::npos && ends_with(n, "/dev/shm/O1/9/key.pass") && n.find('\\') == string::npos,
               "native_option: the path after FILE: is native [" + n + "]");
      string p = native_option("--file-key-management-filename=/dev/shm/O1/9/key.enc");
      st_check(starts_with(p, "--file-key-management-filename=") && p.find("=/dev") == string::npos && ends_with(p, "/dev/shm/O1/9/key.enc"), "and so is a plain path [" + p + "]");
    } else {
      st_eq(n, "--file-key-management-filekey=FILE:/dev/shm/O1/9/key.pass", "native_option: off MSYS2 the option stays as it is");
    }
    st_eq(native_option("--sql_mode="), "--sql_mode=", "native_option: an empty value stays");
    st_eq(native_option("--log-bin"), "--log-bin", "and an option with no value");
    st_eq(native_option("--innodb_page_size=4k"), "--innodb_page_size=4k", "and a value that is no path");
    st_eq(native_option("--plugin-dir=/a/b:/c/d"), "--plugin-dir=/a/b:/c/d", "and a list of paths, which the runtime converts itself");
  }
  // the optimizer trace cap only where the server has an optimizer trace
  {
    Basedir m55, m56, md103, md131;
    basedir_parse_name("MS070123-mysql-5.5.62-linux-x86_64-opt", m55);
    basedir_parse_name("MS070123-mysql-5.6.51-linux-x86_64-opt", m56);
    basedir_parse_name("MD180826-mariadb-10.3.39-linux-x86_64-opt", md103);
    basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", md131);
    const string trace = "--maximum-optimizer_trace_max_mem_size=1M";
    st_check(mysafe_options(m55).find(trace) == string::npos, "mysafe_options: no optimizer trace cap on MySQL 5.5");
    st_check(mysafe_options(m56).find(trace) != string::npos, "mysafe_options: the optimizer trace cap on MySQL 5.6");
    st_check(mysafe_options(md103).find(trace) == string::npos, "mysafe_options: no optimizer trace cap on MariaDB 10.3");
    st_check(mysafe_options(md131).find(trace) != string::npos, "mysafe_options: the optimizer trace cap on MariaDB 13.1");
    st_check(mysafe_options(m55).find("  ") == string::npos, "mysafe_options: no double space where the cap went");
    st_check(log_aborted("2026-09-07  8:00:00 0 [ERROR] Aborting\n"), "log_aborted: the MariaDB line");
    st_check(log_aborted("2026-09-23T21:25:19.657260Z 0 [ERROR] [MY-010119] [Server] Aborting\n"), "log_aborted: the MySQL 8.0 line");
    st_check(!log_aborted("2026-09-07  8:00:00 0 [Note] mariadbd: ready for connections.\n"), "log_aborted: not on a clean start");
    st_check(port_clash_in_log("2026-10-05 21:09:11 0 [ERROR] Can't start server: Bind on TCP/IP port. Got error: 10048: Only one usage of each socket address (protocol/network address/port) is normally permitted.\n"),
             "port_clash_in_log: the Windows bind failure");
    st_check(port_clash_in_log("2026-09-07  8:00:00 0 [ERROR] Can't start server: Bind on TCP/IP port: Address already in use\n"), "port_clash_in_log: the Linux bind failure");
    st_check(!port_clash_in_log("2026-09-07  8:00:00 0 [ERROR] Aborting\n2026-10-05 21:09:11 0 [Note] Server socket created on IP: '::', port: '13555'.\n"),
             "port_clash_in_log: not on an Aborting alone, nor on the note of the socket");
    st_check(server_log_ready("2026-10-05 21:09:09 0 [Note] mariadbd.exe: ready for connections.\n") && !server_log_ready("2026-09-07  8:00:00 0 [ERROR] Aborting\n"),
             "server_log_ready: the line says it, and a log without it does not");
  }
  // the client module MySQL 8.0+ needs comes from a plain MariaDB build
  {
    string td = fmt("/tmp/omnium_st_plugins_%d", (int)getpid());
    for (auto n : {"UBASAN_MD180826-mariadb-13.1.0-linux-x86_64-opt", "MS060224-mysql-8.0.36-linux-x86_64-opt", "MD180826-mariadb-12.3.2-linux-x86_64-opt"}) {
      mkdirs(td + "/" + n + "/lib/plugin");
      write_file(td + "/" + n + "/lib/plugin/caching_sha2_password.so", "");
    }
    mkdirs(td + "/MD180826-mariadb-13.1.0-linux-x86_64-opt/lib/plugin");
    st_eq(client_plugin_dir(td), td + "/MD180826-mariadb-12.3.2-linux-x86_64-opt/lib/plugin", "client_plugin_dir: the plain MariaDB build that has the module");
    st_eq(client_plugin_dir(td + "/none"), "", "client_plugin_dir: no test dir, no plugin dir");
    remove_tree(td);
  }
  // /proc/meminfo as Linux and as Cygwin write it
  {
    uint64_t t = 0, a = 0;
    st_check(meminfo_parse("MemTotal:       131072 kB\nMemFree:         1000 kB\nMemAvailable:   65536 kB\nBuffers: 5 kB\n", t, a) && t == 131072 && a == 65536, "meminfo_parse: MemAvailable when the line is there");
    st_check(meminfo_parse("MemTotal:       131072 kB\nMemFree:        40000 kB\nHighTotal: 0 kB\n", t, a) && t == 131072 && a == 40000, "meminfo_parse: MemFree stands in without MemAvailable");
    st_check(!meminfo_parse("nothing here\n", t, a), "meminfo_parse: no MemTotal is no reading");
  }
  // an MX answer, built by hand: two records, the lower preference first whatever the order
  {
    vector<unsigned char> m = {0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 0};
    auto name = [&](const string& dotted) { for (auto& lab : split(dotted, '.')) { m.push_back((unsigned char)lab.size()); for (char c : lab) m.push_back((unsigned char)c); } m.push_back(0); };
    auto u16 = [&](int v) { m.push_back((unsigned char)(v >> 8)); m.push_back((unsigned char)(v & 0xff)); };
    name("example.org"); u16(15); u16(1);                                   // the question
    auto rr = [&](int pref, const string& host) {
      name("example.org"); u16(15); u16(1); u16(0); u16(300);                // type MX, class IN, TTL
      vector<unsigned char> rd; { std::swap(rd, m); u16(pref); name(host); std::swap(rd, m); }
      u16((int)rd.size()); m.insert(m.end(), rd.begin(), rd.end());
    };
    rr(20, "mx2.example.org"); rr(10, "mx1.example.org");
    vector<string> hosts;
    st_check(mx_parse(m.data(), (int)m.size(), hosts) && hosts.size() == 2, "mx_parse: two MX records are read");
    st_check(hosts.size() == 2 && hosts[0] == "mx1.example.org" && hosts[1] == "mx2.example.org", "mx_parse: the lower preference comes first");
    st_check(!mx_parse(m.data(), 20, hosts), "mx_parse: a cut answer is refused");
    st_check(!mx_parse(m.data(), 5, hosts), "mx_parse: a short header is refused");
    vector<unsigned char> a_only = {0, 1, 0x81, 0x80, 0, 0, 0, 1, 0, 0, 0, 0};
    std::swap(m, a_only); name("example.org"); u16(1); u16(1); u16(0); u16(60); u16(4); m.insert(m.end(), {127, 0, 0, 1}); std::swap(m, a_only);
    st_check(!mx_parse(a_only.data(), (int)a_only.size(), hosts) && hosts.empty(), "mx_parse: an A record alone gives no MX host");
  }
  // a \u escape pair above the BMP is one character of four bytes
  {
    JsonValue v;
    st_check(json_parse("\"a\\ud83d\\ude00b\"", v) && v.str == "a\xF0\x9F\x98\x80" "b", "json_parse: a surrogate pair becomes four UTF-8 bytes");
    st_check(json_parse("\"\\u00e9\"", v), "json_parse: a two-byte escape parses");
    st_eq(v.str, "\xC3\xA9", "json_parse: a two-byte escape still reads");
    st_check(json_parse("\"\\ud83dx\"", v), "json_parse: a lone high surrogate parses");
    st_eq(v.str, "\xED\xA0\xBD" "x", "json_parse: a lone high surrogate is kept as three bytes, not dropped");
  }
  // the pids file: the note is everything after the kind, digits or not
  {
    string save = g_wd.pids;
    string wd = tmp + "/wd";
    mkdirs(wd);
    g_wd.pids = wd + "/omnium.pids";
    write_file(g_wd.pids, "1234 trial 1 of build 12\n99 reduce\n7 hold 7 7\n");
    auto pr = pids_read(wd);
    st_check(pr.size() == 3, "pids_read: three lines");
    if (pr.size() == 3) {
      st_check(std::get<0>(pr[0]) == 1234 && std::get<1>(pr[0]) == "trial" && std::get<2>(pr[0]) == "1 of build 12", "pids_read: a note that starts with a digit of the pid stays whole");
      st_check(std::get<2>(pr[1]).empty(), "pids_read: no note is empty");
      st_check(std::get<0>(pr[2]) == 7 && std::get<2>(pr[2]) == "7 7", "pids_read: a note equal to the pid is not cut at the pid");
    }
    g_wd.pids = save;
  }
  // the TUI frame stays inside the terminal, whatever the run has to show
  {
    st_check(tui_frame_lines(40, 120, 2, 3, 2, 2, 8) <= 39, "tui: a small run fits a 40-row terminal");
    st_check(tui_frame_lines(24, 100, 6, 30, 20, 20, 8) <= 23, "tui: a run with 30 slots and a long inbox fits 24 rows");
    st_check(tui_frame_lines(20, 80, 6, 30, 20, 20, 8) <= 19, "tui: and 20 rows, where the log tail gives way");
    st_check(tui_frame_lines(60, 120, 6, 30, 20, 20, 8) > 40, "tui: a tall terminal shows more of the lists");
    // the log takes what the lists leave, so a frame fills the screen to the last line but one
    for (int rows : {24, 30, 40, 60, 100})
      st_check(tui_frame_lines(rows, 120, 2, 3, 0, 0, 8) == rows - 1,
               fmt("tui: the log fills what the lists leave of a %d-row terminal", rows));
    // a window taller than the screen: the rows under the taskbar are left empty (50 rows of 20 px)
    st_check(rows_below_work_area(100, 1000, 900, 50) == 10, "tui: the rows of a window that lie below the work area");
    st_check(rows_below_work_area(100, 1000, 905, 50) == 10, "tui: a row cut in half counts as hidden");
    st_check(rows_below_work_area(100, 1000, 1100, 50) == 0, "tui: a window that ends at the work area hides nothing");
    st_check(rows_below_work_area(100, 1000, 1500, 50) == 0, "tui: a window inside the work area hides nothing");
    st_check(rows_below_work_area(2000, 1000, 900, 50) == 50, "tui: a window off the bottom of the screen hides all its rows");
    st_check(rows_below_work_area(100, 0, 900, 50) == 0 && rows_below_work_area(100, 1000, 900, 0) == 0,
             "tui: a window of no size hides nothing");
  }
  // a settings file from before a key was added, under a home of its own
  {
    string keep = home_dir(), h = tmp + "/home";
    mkdirs(h);
    setenv("HOME", h.c_str(), 1);
    string p = config_path();
    write_file(p, "TEST_DIR=/test\n# a comment\nDATA_DIR=/data\n");
    vector<string> missing = config_missing_keys();
    st_check(!missing.empty() && std::find(missing.begin(), missing.end(), "TEST_DIR") == missing.end() &&
             std::find(missing.begin(), missing.end(), "DATA_DIR") == missing.end(), "config_missing_keys: the keys the file has are not missing");
    st_check(std::find(missing.begin(), missing.end(), "RAM_CAP_PCT") != missing.end(), "config_missing_keys: a key the file lacks is");
    write_file(p, config_dump());
    st_check(config_missing_keys().empty(), "config_missing_keys: a full dump has them all");
    setenv("HOME", keep.c_str(), 1);
  }
  // TEST_DIR is where the builds are: /test on Linux, and C:\test as /c/test under MSYS2, whose own
  // /test is C:\msys64\test and holds none. A TEST_DIR with no build says so, and one with a build
  // says nothing.
  {
    st_eq(Config().test_dir, kHostMsys2 ? "/c/test" : "/test", "the build dir defaults to where this box keeps its builds");
    string save = g_cfg.test_dir, td = tmp + "/notd";
    mkdirs(td);
    g_cfg.test_dir = td;
    if (kTakeFixes) st_check(starts_with(test_dir_empty_note(), "no build under TEST_DIR " + td), "test_dir_empty_note: a TEST_DIR with no build says so");
    else st_check(test_dir_empty_note().empty(), "test_dir_empty_note: says nothing off MSYS2");
    string bd = td + "/MD180826-mariadb-13.1.0-linux-x86_64-opt";
    mkdirs(bd + "/bin");
    write_file(bd + "/bin/mariadbd", "#!/bin/sh\nexit 0\n");
    chmod((bd + "/bin/mariadbd").c_str(), 0755);
    st_check(test_dir_empty_note().empty(), "and one with a build says nothing");
    // a registered Windows build with no mariadb-test is named (omnium mtr cannot run on it); one with it, and a Linux build, are not
    {
      string wo = "MD180826-mariadb-13.1.0-windows-x86_64-opt", wd = "MD180826-mariadb-13.1.0-windows-x86_64-dbg";
      mkdirs(td + "/" + wo + "/bin");
      mkdirs(td + "/" + wd + "/mariadb-test");
      Registry rr;
      for (const string& n : {wo, wd, string("MD180826-mariadb-13.1.0-linux-x86_64-opt")}) { BuildEntry e; e.name = n; rr.entries.push_back(e); }
      vector<string> miss = windows_builds_without_mtr(rr);
      st_check(miss.size() == 1 && miss[0] == wo, "windows_builds_without_mtr: the Windows build with no mariadb-test, and no other [" + join(miss, " ") + "]");
      st_check(mtr_suite_fix().find("INSTALL_MYSQLTESTDIR=mariadb-test") != string::npos, "and the fix names the flag");
    }
    // a PERL setting that names no perl is refused with its name in the answer, and never falls back to another
    {
      string save_perl = g_cfg.perl, why;
      g_cfg.perl = "/nonexistent/omnium_perl.exe";
      st_check(native_perl_find(&why).empty() && why.find("PERL=/nonexistent/omnium_perl.exe is not there") != string::npos, "native_perl_find: a PERL that is not there says so [" + why + "]");
      g_cfg.perl = save_perl;
    }
    g_cfg.test_dir = save;
  }
  // the mount point of a path: a parent on the same device, never one on another
  {
    string mp = mount_point_of(tmp);
    st_check(!mp.empty() && starts_with(tmp, mp) && dir_exists(mp), "mount_point_of: a parent of the path that is a directory");
    st_eq(mount_point_of("/"), "/", "mount_point_of: the root is its own mount");
    // /proc/<pid> is on a device of its own under MSYS2, so there /proc/self is no path on /proc
    if (kHostMsys2)
      st_skip("mount_point_of: a path on a mounted filesystem gives that mount "
              "(/proc/<pid> is on a device of its own under MSYS2)");
    else
      st_eq(mount_point_of("/proc/self"), "/proc",
            "mount_point_of: a path on a mounted filesystem gives that mount");
    st_eq(mount_point_of(tmp + "/no_such_dir_here"), "/", "mount_point_of: a path that is not there gives the root");
  }
  // the Jira token file: a dotfile in the home directory, or the ~/jira one if only that exists
  {
    string ph = tmp + "/pathome";
    mkdirs(ph + "/.config/mariadb-qa");
    st_eq(default_pat_file(ph), ph + "/.omnium_jira_pat",
          "the Jira token defaults to a dotfile in the home directory");
    write_file(ph + "/.config/mariadb-qa/jira.pat", "x\n");
    st_eq(default_pat_file(ph), ph + "/.config/mariadb-qa/jira.pat",
          "and to the file ~/jira reads when only that one is there");
    write_file(ph + "/.omnium_jira_pat", "x\n");
    st_eq(default_pat_file(ph), ph + "/.omnium_jira_pat",
          "and to its own file as soon as that is there");
    // the home of a person: $HOME on Linux, and on Windows the profile, which is no folder of the
    // shared MSYS2 install, unless HOME was set to something other than MSYS2's own home
    string wh = windows_home();
    if (kHostMsys2)
      st_check(wh.empty() || (starts_with(wh, "/") &&
                              (user_home() == wh || user_home() == home_dir())),
               "under MSYS2 the home is the Windows profile (an MSYS2 path), or the HOME set");
    else
      st_check(wh.empty() && user_home() == home_dir(), "off MSYS2 the home is $HOME");
    {
      // a HOME of its own is the home: every check that starts an omnium with one stays away from
      // the real files
      string keep = home_dir();
      setenv("HOME", (tmp + "/pathome").c_str(), 1);
      st_eq(user_home(), tmp + "/pathome",
            "a HOME that was set wins, so a check's temp home keeps the real token out of reach");
      setenv("HOME", keep.c_str(), 1);
    }
  }
  // two rng() calls are two streams
  {
    Xoshiro256pp a = rng(), b = rng();
    st_check(a.next() != b.next(), "rng: two calls give two streams");
  }
  // the FederatedX SERVER clause follows the endpoint: a mark in the area's SQL, filled per trial
  {
    const Area* fa = area_by_name("federated");
    st_check(fa && fa->interleave.find("OPTIONS (__FEDERATED_CONN__, DATABASE 'test'") != string::npos, "the federated area's SQL carries the connection mark");
    st_check(fa && fa->interleave.find("INSTALL SONAME 'ha_federatedx';") != string::npos, "and names the plugin without an extension, so a .dll build loads it too");
    if (fa) {
      string tcp = replace_all(fa->interleave, "__FEDERATED_CONN__", "HOST '127.0.0.1', PORT 14003");
      st_check(tcp.find("OPTIONS (HOST '127.0.0.1', PORT 14003, DATABASE 'test'") != string::npos, "filled in for a TCP server");
      SqlSources d;
      st_eq(d.federated_conn, "SOCKET '../socket.sock'", "and the default clause is the trial's socket");
    }
  }
  remove_tree(tmp);
}

static void st_backup_uid() {
  st_eq(backup_issue_uid("tables differ after the round trip", "DUCKDB"), "BACKUP_ISSUE|tables differ after the round trip|DUCKDB", "a backup UID names the step and the engines");
  st_eq(backup_issue_uid("the incremental backup", "[00] 2026-09-12 13:59:30 failed to copy datafile 123"), "BACKUP_ISSUE|the incremental backup|failed to copy datafile N", "a backup UID drops the line prefix and makes numbers N");
  st_eq(backup_issue_uid("the incremental backup", "[01] 2026-10-01 01:02:03 failed to copy datafile 9"), backup_issue_uid("the incremental backup", "[00] 2026-09-12 13:59:30 failed to copy datafile 123"), "the same cause with other numbers is the same backup UID");
  st_eq(backup_issue_uid("the full backup", "hang"), "BACKUP_ISSUE|the full backup|hang", "a hang is its own backup UID");
  // the line a failed step is named by: the first that says what failed, not the shutdown that ends the output
  string aria = "[00] 2026-10-05 16:40:01 mariadb-backup.exe: Error 176 reading index file `test`.`t4` block 1\n"
                "[00] 2026-10-05 16:40:01 error: aria_read index from .\\test\\t4#P#p2.MAI failed with error 176\n"
                "Aria data files backup process is finished with error\n"
                "mariabackup: Stopping log copying thread.\n"
                "2026-10-05 16:40:02 0 [Note] InnoDB: Starting shutdown...\n";
  st_eq(backup_failure_line(aria), "[00] 2026-10-05 16:40:01 mariadb-backup.exe: Error 176 reading index file `test`.`t4` block 1", "a failed step is named by the first line that says what failed");
  st_eq(backup_issue_uid("the full backup of the busy server", backup_failure_line(aria)),
        "BACKUP_ISSUE|the full backup of the busy server|mariadb-backup: Error N reading index file `test`.`tN` block N", "and its UID reads the cause, numbers as N and the .exe of the tool left out");
  st_eq(backup_issue_uid("the full backup", "mariadb-backup: Error 176 reading index file"), backup_issue_uid("the full backup", "mariadb-backup.exe: Error 176 reading index file"),
        "a Linux and a Windows tool's failure are one UID");
  st_eq(backup_failure_line("[00] 2026-10-05 16:41:13 Start copying aria log file tail\nmy_setwd() failed , C:\\x\n"), "my_setwd() failed , C:\\x", "a failing last line is still the one");
  st_eq(backup_failure_line("$ mariadb-backup --backup --error-log\n2026-10-05 16:40:00 0 [Note] InnoDB: a read failed, retrying\ncannot open file x\nInnoDB: Starting shutdown...\n"),
        "cannot open file x", "a prompt line and a server [Note] do not count");
  st_eq(backup_failure_line("copying a\ncopying b\nall done\n\n"), "all done", "with no line that says so, the last one is named");
  st_eq(backup_failure_line(""), "", "and an empty output names nothing");
}

int cmd_selftest(const Args& a) {
  // everything the checks write goes under one directory, and it goes when they are done
  struct TmpSweep { ~TmpSweep() { remove_tree("/tmp/omnium_selftest_" + std::to_string(getpid())); } } sweep;
  // and what an earlier run left behind when it was killed: a deep run's tree is gigabytes
  sweep_stale_tmp("/tmp/omnium_selftest_");
  sweep_stale_tmp("/tmp/omnium_cap_");
  // what the checks make reaches nothing real: no mail, no Jira but the stand-in one, and queues
  // of their own. OMNIUM_SET takes the first two to every omnium the checks start
  const char* os = getenv("OMNIUM_SET");
  setenv("OMNIUM_SET", (string(os ? os : "") + "\nEMAIL=\nJIRA_URL=http://127.0.0.1:9\n").c_str(), 1);
  config_load(false);
  // The stand-in Jira takes any token, so every check that talks to it carries a stand-in one (the
  // omniums the checks start inherit it): the real token stays in its file, and a box that has
  // none can run the checks. The note under the summary says so, as only a real search or filing
  // needs one.
  string pat_note;
  if (jira_pat().empty())
    pat_note = "Jira PAT missing: " + g_cfg.pat_file + " is absent or empty and $JIRA_PAT is not "
               "set. These checks do not need one (the stand-in Jira takes any token), but a real "
               "search or filing does: jira.mariadb.org, avatar, Profile, Personal Access Tokens, "
               "and omnium init says how to store it";
  const char* env_pat = getenv("JIRA_PAT");
  const string keep_pat = env_pat ? env_pat : "";
  const bool had_env_pat = env_pat != nullptr;
  setenv("JIRA_PAT", "omnium-selftest-stand-in", 1);
  double t0 = now_ms();
  // nothing this check files may reach the box's own queues, so they point into the temp dir from
  // here on; a check that reloads the settings puts the pair back afterwards
  g_paths.human_queue = st_tmp() + "/HUMAN-queue";
  g_paths.ai_queue = st_tmp() + "/AI-queue";
  st_check(starts_with(g_paths.human_queue, st_tmp()) && starts_with(g_paths.ai_queue, st_tmp()), "this check files into its own temp queues, never the box's");
  st_util();
  st_rng();
  st_config();
  st_qa_checkout();
  st_store();
  st_registry();
  st_kb();
  st_build();
  st_mtr();
  st_mtr_gate_units();
  st_verbs_read_only();
  st_fixtures();
  st_detect_classes();
  st_jira_stub();
  st_sources_plain();
  st_plumbing();
  st_build_plain();
  st_registry_plain();
  st_view_and_config();
  st_detect_more();
  st_reduce_and_report();
  st_corners();
  st_units();
  st_units_tail();
  st_inbox_corners();
  st_fix_version_units();
  st_backup_uid();
  st_windows();
  st_portability();
  for (auto& x : a) if (x == "--deep") { st_detect_parity(); st_deep(); }
  for (auto& x : a) if (x == "--deep-ports") st_deep_ports();
  long p = g_st_pass.load(), f = g_st_fail.load();
  printf("selftest: %ld checks, %ld failed (%.1f s)\n", p + f, f, (now_ms() - t0) / 1000.0);
  for (auto& w : g_st_failed) printf("  failed: %s\n", w.c_str());
  for (auto& w : g_st_skipped) printf("  skipped: %s\n", w.c_str());
  if (!pat_note.empty()) printf("  note: %s\n", pat_note.c_str());
  if (had_env_pat) setenv("JIRA_PAT", keep_pat.c_str(), 1); else unsetenv("JIRA_PAT");
  return f == 0 ? 0 : 1;
}

// One error log per detection class, with the UniqueID the mariadb-qa chain gives for it. Every one
// of these was compared against new_text_string.sh and matched; the deep run repeats that check.
namespace {
// windows: the log is a Windows server's, which the mariadb-qa scripts have no rule for, so the deep
// run has no shell answer to compare it with and leaves it out
struct UidFixture { const char* name; const char* log; const char* uid; bool windows = false; };
const char* FX_HEAD = "2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n";
const UidFixture UID_FIXTURES[] = {
  {"asan_poison",
      "==1440680==ERROR: AddressSanitizer: use-after-poison on address 0x76041a1b7f93 at pc 0x6b3307126fdb bp 0x6b33150ff800 sp 0x6b33150ff7f8\n"
      "READ of size 1 at 0x76041a1b7f93 thread T15\n"
      "    #0 0x6b3307126fda in TYPVAL<char*>::SetValue_char(char const*, int) /test/13.1/storage/connect/value.cpp:1381:8\n"
      "    #1 0x6b3306f3072e in CntIndexRead(_global*, TDB*, OPVAL, st_key_range const*, bool) /test/13.1/storage/connect/connect.cc:1000:5\n"
      "    #2 0x5e46f560c494 in join_read_const(st_join_table*) /test/13.1/sql/sql_select.cc:25311:9\n"
      "    #3 0x5e46f5464b59 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:24716:12\n"
      "SUMMARY: AddressSanitizer: use-after-poison /test/13.1/storage/connect/value.cpp:1381:8 in TYPVAL<char*>::SetValue_char(char const*, int)\n",
   "ASAN|use-after-poison|storage/connect/value.cpp|TYPVAL<char*>::SetValue_char|CntIndexRead|join_read_const|sub_select"},
  {"asan_segv",
      "==999==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000 (pc 0x0000005a1b2c bp 0x7ffd0 sp 0x7ffc8 T7)\n"
      "==999==The signal is caused by a READ memory access.\n"
      "==999==Hint: address points to the zero page.\n"
      "    #0 0x5a1b2c in Field::val_str(String*) /test/13.1/sql/field.cc:1000:3\n"
      "    #1 0x5a2000 in Item_field::val_str(String*) /test/13.1/sql/item.cc:2000:24\n"
      "    #2 0x5a3000 in evaluate_join_record(JOIN*, st_join_table*, int) /test/13.1/sql/sql_select.cc:24000:9\n"
      "SUMMARY: AddressSanitizer: SEGV /test/13.1/sql/field.cc:1000:3 in Field::val_str(String*)\n",
   "ASAN|SEGV /test/13.1/sql/field.cc:1000:3 in Field::val_str(String*)"},
  {"asan_uaf",
      "==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x606000000040 at pc 0x4a1b2c bp 0x7ffd0 sp 0x7ffc8\n"
      "READ of size 8 at 0x606000000040 thread T3\n"
      "    #0 0x4a1b2c in Item_func_case::val_int() /test/13.1/sql/item_cmpfunc.cc:2544:10\n"
      "    #1 0x4a2000 in Item::val_int_result() /test/13.1/sql/item.h:1200:12\n"
      "    #2 0x4a3000 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:22000:5\n"
      "    #3 0x4a4000 in do_select(JOIN*, Procedure*) /test/13.1/sql/sql_select.cc:24230:14\n"
      "SUMMARY: AddressSanitizer: heap-use-after-free /test/13.1/sql/item_cmpfunc.cc:2544:10 in Item_func_case::val_int()\n",
   "ASAN|heap-use-after-free|sql/item_cmpfunc.cc|Item_func_case::val_int|Item::val_int_result|sub_select|do_select"},
  {"checktable",
      "2026-09-05  5:00:02 0 [ERROR] CHECKTABLE t1 is corrupt\n",
   "CHECKTABLE|CHECKTABLE t1 is corrupt"},
  {"crashed",
      "mariadbd: Table './test/t1' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table ./test/t1 is marked as crashed and should be repaired"},
  {"duckdb",
      "2026-09-05  5:00:02 0 [ERROR] DuckDB: Table t1 could not be found\n",
   "DUCKDB_ERROR|Table X could not be found"},
  {"errcode",
      "MariaDB error code: 1032\n",
   "MARIADB_ERROR_CODE|MariaDB error code: 1032"},
  {"glibc",
      "double free or corruption (out)\n",
   "GLIBC|double free or corruption (out)"},
  {"goterror",
      "mariadbd: Got error '175 \"Number of columns does not match\"' for 't1'\n",
   "GOT_ERROR|Got error 175|Number of columns does not match"},
  {"gotfatal",
      "[ERROR] Got fatal error 1236 from master when reading data from binary log: 'Could not find GTID'\n",
   "GOT_FATAL_ERROR|Got fatal error 1236"},
  {"haread",
      "2026-09-05  5:00:02 0 [ERROR] mysql_ha_read: Got error 1032 when reading table 't1'\n",
   "GOT_ERROR|Got error 1032|when reading table X"},
  {"innoassert",
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Assertion failure in file /test/13.1/storage/innobase/btr/btr0cur.cc line 1234\n"
      "InnoDB: Failing assertion: page_get_n_recs(page) > 1\n",
   "INNODB_ERROR|Assertion failure in file /test/13.1/storage/innobase/btr/btr0cur.cc line 1234"},
  {"innodb",
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Table test/t1 index a is corrupt\n",
   "INNODB_ERROR|Table test/t1 index a is corrupt"},
  {"lsan",
      "==5555==ERROR: LeakSanitizer: detected memory leaks\n"
      "Direct leak of 32 byte(s) in 1 object(s) allocated from:\n"
      "    #0 0x4d1000 in operator new(unsigned long) /llvm/asan_new_delete.cpp:95:3\n"
      "    #1 0x5e1000 in Item_func_conv_charset::Item_func_conv_charset(THD*, Item*) /test/13.1/sql/item_strfunc.h:1000:9\n"
      "    #2 0x5e2000 in MYSQLparse(THD*) /test/13.1/sql/sql_yacc.yy:5000:9\n"
      "SUMMARY: AddressSanitizer: 32 byte(s) leaked in 1 allocation(s).\n",
   "ASAN|32 byte(s) leaked in 1 allocation(s)."},
  // four leaks whose frames sit in a module that is gone (a plugin unloaded before the check): each
  // has a rule of its own that names the leak from the frames further down
  {"lsan_plugin_foreach",
      "2026-09-05  5:00:01 0 [Note] /test/13.1/bin/mariadbd: ready for connections.\n"
      "==1234==ERROR: LeakSanitizer: detected memory leaks\n"
      "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
      "    #0 0x55d000000000 in operator new(unsigned long) (/usr/lib/x86_64-linux-gnu/libasan.so.8+0xdc)\n"
      "    #1 0x7f1000000001  (<unknown module>)\n"
      "    #2 0x7f1000000002  (<unknown module>)\n"
      "    #3 0x7f1000000003  (<unknown module>)\n"
      "    #4 0x7f1000000004 in dlopen (/lib/x86_64-linux-gnu/libc.so.6+0x90e2)\n"
      "    #5 0x55d000000005 in plugin_dl_add(st_mysql_const_lex_string const*, int) /test/13.1/sql/sql_plugin.cc:800:3\n"
      "    #6 0x55d000000006 in plugin_dl_foreach(THD*, st_mysql_const_lex_string const*, st_plugin_int**, void*) /test/13.1/sql/sql_plugin.cc:2400:3\n",
   "LSAN|memory leak|sql/sql_plugin.cc|operator new|dlopen|plugin_dl_add|plugin_dl_foreach"},
  {"lsan_plugin_dl_open_worker",
      "2026-09-05  5:00:01 0 [Note] /test/13.1/bin/mariadbd: ready for connections.\n"
      "==1234==ERROR: LeakSanitizer: detected memory leaks\n"
      "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
      "    #0 0x55d000000000 in operator new(unsigned long) (/usr/lib/x86_64-linux-gnu/libasan.so.8+0xdc)\n"
      "    #1 0x7f1000000001  (<unknown module>)\n"
      "    #2 0x7f1000000002  (<unknown module>)\n"
      "    #3 0x7f1000000003  (<unknown module>)\n"
      "    #4 0x7f1000000004 in dl_open_worker (/lib64/ld-linux-x86-64.so.2+0x1)\n"
      "    #5 0x7f1000000005 in dlopen (/lib/x86_64-linux-gnu/libc.so.6+0x90e2)\n"
      "    #6 0x55d000000006 in plugin_dl_add(st_mysql_const_lex_string const*, int) /test/13.1/sql/sql_plugin.cc:800:3\n",
   "LSAN|memory leak|sql/sql_plugin.cc|operator new|dl_open_worker|dlopen|plugin_dl_add"},
  {"lsan_calloc_rnd_init",
      "2026-09-05  5:00:01 0 [Note] /test/13.1/bin/mariadbd: ready for connections.\n"
      "==1234==ERROR: LeakSanitizer: detected memory leaks\n"
      "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
      "    #0 0x55d000000000 in calloc (/usr/lib/x86_64-linux-gnu/libasan.so.8+0xdc)\n"
      "    #1 0x7f1000000001  (<unknown module>)\n"
      "    #2 0x7f1000000002  (<unknown module>)\n"
      "    #3 0x7f1000000003  (<unknown module>)\n"
      "    #4 0x55d000000004 in handler::ha_rnd_init(bool) /test/13.1/sql/handler.h:3300:5\n"
      "    #5 0x55d000000005 in handler::ha_rnd_init_with_error(bool) /test/13.1/sql/handler.cc:3500:3\n"
      "    #6 0x55d000000006 in init_read_record(READ_RECORD*, THD*, TABLE*, SQL_SELECT*, SORT_INFO*, int, bool, bool) /test/13.1/sql/records.cc:250:3\n",
   "LSAN|memory leak|<unknown_module>|calloc|handler::ha_rnd_init(bool)|handler::ha_rnd_init_with_error(bool)|init_read_record"},
  {"lsan_malloc_update",
      "2026-09-05  5:00:01 0 [Note] /test/13.1/bin/mariadbd: ready for connections.\n"
      "==1234==ERROR: LeakSanitizer: detected memory leaks\n"
      "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
      "    #0 0x55d000000000 in malloc (/usr/lib/x86_64-linux-gnu/libasan.so.8+0xdc)\n"
      "    #1 0x7f1000000001  (<unknown module>)\n"
      "    #2 0x7f1000000002  (<unknown module>)\n"
      "    #3 0x7f1000000003  (<unknown module>)\n"
      "    #4 0x55d000000004 in Sql_cmd_update::update_single_table(THD*) /test/13.1/sql/sql_update.cc:900:7\n"
      "    #5 0x55d000000005 in Sql_cmd_update::execute_inner(THD*) /test/13.1/sql/sql_update.cc:3100:3\n"
      "    #6 0x55d000000006 in Sql_cmd_dml::execute(THD*) /test/13.1/sql/sql_select.cc:600:7\n",
   "LSAN|memory leak|<unknown_module>|malloc|Sql_cmd_update::update_single_table|Sql_cmd_update::execute_inner|Sql_cmd_dml::execute"},
  {"mariadbderr",
      "2026-09-05  5:00:02 0 [ERROR] mariadbd: Incorrect information in file: './test/t1.frm'\n",
   "MARIADBD_ERROR|mariadbd: Incorrect information in frm file"},
  {"memfree",
      "Warning: Memory not freed: 1024\n",
   "GENERIC_ISSUE-DO_NOT_ADD_TO_KB_OR_KBA|MEMORY_NOT_FREED|Warning: Memory not freed"},
  {"msan",
      "==4321==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
      "    #0 0x55d123 in Field_long::val_int() /test/13.1/sql/field.cc:4000:10\n"
      "    #1 0x55d456 in Item_field::val_int() /test/13.1/sql/item.cc:3000:24\n"
      "    #2 0x55d789 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:24716:12\n"
      "SUMMARY: MemorySanitizer: use-of-uninitialized-value /test/13.1/sql/field.cc:4000:10 in Field_long::val_int()\n",
   "MSAN|use-of-uninitialized-value /test/13.1/sql/field.cc:4000:10 in Field_long::val_int()"},
  {"mutex",
      "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n",
   "MUTEX_ERROR|safe_mutex: Found wrong usage of mutex LOCK_open and LOCK_status"},
  {"mutex_line",
      "safe_mutex: Trying to lock mutex at /test/13.1/sql/x.cc, line 55 more than 1 time\n",
   "MUTEX_ERROR|safe_mutex: Trying to lock mutex at sql/x.cc"},
  {"opentable",
      "OpenTable: Open(r+b) error 2 on /test/13.1/data/test/t1.dat:\n",
   "OPENTABLE|OpenTable: Open(r+b) error 2 on X:"},
  {"rocksdb",
      "2026-09-05  5:00:02 0 [ERROR] RocksDB: Status 5 IOError\n",
   "ROCKSDB_ERROR|RocksDB: Status 5 IOError"},
  {"servererrno",
      "server_errno: 2013\n",
   "SERVER_ERRNO|server_errno: 2013"},
  {"slave",
      "2026-09-05  5:00:02 0 [ERROR] Slave SQL: Error 'Duplicate entry' on query. Default database: 'test'\n",
   "SLAVE_ERROR|Slave SQL: Error"},
  {"tableerr",
      "2026-09-05  5:00:02 0 [ERROR] Table t1 is not a view\n",
   "MARIADBD_ERROR|Table t1 is not a view"},
  {"tsan",
      "WARNING: ThreadSanitizer: data race (pid=1234)\n"
      "  Write of size 8 at 0x7b0400000000 by thread T3:\n"
      "    #0 my_thread_var_ptr /test/13.1/mysys/my_thr_init.c:100 (mariadbd+0x123456)\n"
      "    #1 mysql_mutex_lock /test/13.1/include/mysql/psi/mysql_thread.h:200 (mariadbd+0x123457)\n"
      "  Previous read of size 8 at 0x7b0400000000 by thread T2:\n"
      "    #0 my_thread_var_ptr /test/13.1/mysys/my_thr_init.c:100 (mariadbd+0x123456)\n"
      "SUMMARY: ThreadSanitizer: data race /test/13.1/mysys/my_thr_init.c:100 in my_thread_var_ptr\n",
   "TSAN|data race /test/13.1/mysys/my_thr_init.c:100 in my_thread_var_ptr"},
  {"ubsan",
      "/test/13.1/strings/ctype-ascii.h:110:27: runtime error: applying non-zero offset 4 to null pointer\n"
      "    #0 0x594e8b75fd6b in my_strcoll_ascii_4bytes_found /test/13.1/strings/ctype-ascii.h:110:27\n"
      "    #1 0x594e8b75fd6b in my_strnncollsp_utf8mb4_bin /test/13.1/strings/strcoll.inl:350:24\n"
      "    #2 0x594e8882c204 in group_concat_key_cmp_with_order /test/13.1/sql/item_sum.cc:3736:21\n"
      "    #3 0x594e8b5a705d in tree_insert /test/13.1/mysys/tree.c:249:9\n"
      "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /test/13.1/strings/ctype-ascii.h:110:27\n",
   "UBSAN|applying non-zero offset X to null pointer|strings/ctype-ascii.h|my_strcoll_ascii_4bytes_found|my_strnncollsp_utf8mb4_bin|group_concat_key_cmp_with_order|tree_insert"},
  {"assert_plain",
      "mariadbd: /test/13.1/sql/sql_select.cc:9999: void JOIN::cleanup(bool): Assertion `table_count > 0' failed.\n",
   "ASSERT|table_count > 0"},
  {"dict0dd",
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Assertion failure: dict0dd.cc:5001:error\n"
      "InnoDB: Failing assertion: !error\n",
   "INNODB_ERROR|Assertion failure: dict0dd.cc:5001:error"},
  {"fallback_stack",
      "260905  5:00:02 [ERROR] mysqld got signal 11 ;\n"
      "Query (0x7f0000000000): SELECT 1\n"
      "Server version: 13.1.0-MariaDB\n"
      "stack_bottom = 0x7f00 thread_stack 0x100000\n"
      "/test/13.1/bin/mariadbd(my_print_stacktrace+0x32)[0x55c0d0]\n"
      "/test/13.1/bin/mariadbd(handle_fatal_signal+0x2f5)[0x55c0e0]\n"
      "/test/13.1/bin/mariadbd(_ZN4JOIN7cleanupEb+0x55)[0x55c0f0]\n",
   "Assert: no core file found in */*core*, and fallback_text_string.sh returned an empty output for all logs"},
  {"glibc_bof",
      "*** buffer overflow detected ***: terminated\n",
   "Assert: no core file found in */*core*, and fallback_text_string.sh returned an empty output for all logs"},
  {"innodb_record",
      "2026-09-05  5:00:02 8 [Warning] InnoDB: Record in index `PRIMARY` of table `test`.`t1` was not found on update: TUPLE (info_bits=0, 2 fields): {[4]    1(0x80000001),[6]    x(0x000000000200)} at: COMPACT RECORD(info_bits=0, 2 fields): {[4]    1(0x80000001),[6]    x(0x000000000200)}\n",
   "INNODB_ERROR|Record in index X of table Y was not found on update: TUPLE Z at: COMPACT RECORD"},
  {"innodb_rename",
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Cannot rename test/t1 to test2/t1 because the target schema directory doesnt exist\n",
   "INNODB_ERROR|Cannot rename X to Y because the target schema directory doesnt exist"},
  {"munmap",
      "munmap_chunk(): invalid pointer\n",
   "GLIBC|munmap_chunk(): invalid pointer"},
  {"ps_version",
      "mariadbd: /test/13.1/sql/dd/impl/dictionary_impl.cc:100: virtual uint dd::Dictionary_impl::get_actual_P_S_version(THD*): Assertion `!error' failed.\n",
   "ASSERT|!error"},
  // A Windows server's log. Its binary is mariadbd.exe, so a line a Linux log writes with "mariadbd:" reads
  // "mariadbd.exe:", and the UID has to come out as the Linux twin's, with no .exe in it
  {"win_crashed",
      "2026-10-05 15:06:56 4 [ERROR] mariadbd.exe: Table 't1' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table X is marked as crashed and should be repaired", true},
  {"win_mysqld_crashed",
      "2026-10-05 15:06:56 4 [ERROR] mysqld.exe: Table 't1' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table X is marked as crashed and should be repaired", true},
  {"win_mariadbd_error",
      "2026-10-05 15:06:56 4 [ERROR] mariadbd.exe: Table 't1' has a corrupted index\r\n",
   "MARIADBD_ERROR|mariadbd: Table table has a corrupted index", true},
  // a mysql.* table that is corrupted on purpose: known as SPECIAL-47, and in the Linux list as this UID
  {"win_cannot_load",
      "2026-10-05 15:26:39 7 [ERROR] mariadbd.exe: Cannot load from mysql.procs_priv. The table is probably corrupted\n",
   "MARIADBD_ERROR|mariadbd: Cannot load from mysql.procs_priv. The table is probably corrupted", true},
  // A failed assert, as the Windows CRT prints it: MSVC writes NULL as 0 where GCC writes __null, and the UID keeps
  // the 0 as printed. The walk starts at the handler and the abort route is left out; the first four frames remain
  {"win_assert_zero",
      "Assertion failed: thd->free_list == 0, file C:/test/13.1/sql/sql_yacc.yy, line 3756\n"
      "260912 10:00:07 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception 0x80000003 ;\n"
      "Sorry, we probably made a mistake, and this is a bug.\n\n"
      "Attempting backtrace. Include this in the bug report.\n"
      "(note: Retrieving this information may fail)\n\n"
      "Thread pointer: 0x1f2e3d4c\n"
      "server.dll!my_sigabrt_handler()[my_thr_init.c:449]\n"
      "ucrtbased.dll!raise()\n"
      "ucrtbased.dll!abort()\n"
      "ucrtbased.dll!_wassert()\n"
      "server.dll!MYSQLparse()[sql_yacc.yy:3756]\n"
      "server.dll!parse_sql()[sql_parse.cc:10100]\n"
      "server.dll!mysql_parse()[sql_parse.cc:7800]\n"
      "server.dll!dispatch_command()[sql_parse.cc:1900]\n"
      "server.dll!do_command()[sql_parse.cc:1400]\n"
      "KERNEL32.DLL!BaseThreadInitThunk()\n"
      "ntdll.dll!RtlUserThreadStart()\n",
   "thd->free_list == 0|SIGABRT|MYSQLparse|parse_sql|mysql_parse|dispatch_command", true},
  // MSVC's deleting destructor, THD::`scalar deleting destructor', is gdb's second THD::~THD frame; this is the UID the list
  // holds for MDEV-25972 (the thread pool frames below it are Linux's too, with --thread-handling=pool-of-threads)
  {"win_deleting_destructor",
      "Assertion failed: status_var.local_memory_used == 0 || !debug_assert_on_not_freed_memory, file C:/test/13.1/sql/sql_class.cc, line 1706\n"
      "260912 10:00:07 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception 0x80000003 ;\n"
      "Attempting backtrace. Include this in the bug report.\n\n"
      "server.dll!my_sigabrt_handler()[my_thr_init.c:449]\n"
      "ucrtbased.dll!raise()\n"
      "ucrtbased.dll!abort()\n"
      "ucrtbased.dll!_wassert()\n"
      "server.dll!THD::~THD()[sql_class.cc:1706]\n"
      "server.dll!THD::`scalar deleting destructor'()\n"
      "server.dll!threadpool_remove_connection()[threadpool_common.cc:210]\n"
      "server.dll!tp_callback()[threadpool_common.cc:252]\n"
      "server.dll!tp_callback()[threadpool_win.cc:279]\n"
      "server.dll!io_completion_callback()[threadpool_win.cc:300]\n"
      "ntdll.dll!RtlUserThreadStart()\n",
   "status_var.local_memory_used == 0 || !debug_assert_on_not_freed_memory|SIGABRT|THD::~THD|THD::~THD|threadpool_remove_connection|tp_callback", true},
  // a /RTC1 run-time check that failed (a stack variable overwritten, as in MDEV-37154): the abort route of the check, failwithmessage and
  // _RTC_*, is no more a frame than abort() is, and the check that failed stands where an assertion's text does. This is the
  // server's own log of that bug, CRLF line ends included
  {"win_rtc_stack",
      "261005 15:45:57 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception 0x80000003 ;\r\n"
      "Sorry, we probably made a mistake, and this is a bug.\r\n\r\n"
      "Attempting backtrace. Include this in the bug report.\r\n"
      "(note: Retrieving this information may fail)\r\n\r\n"
      "Thread pointer: 0x1ffe5755c30\r\n"
      "ha_archive.dll!failwithmessage()[error.cpp:210]\r\n"
      "ha_archive.dll!_RTC_StackFailure()[error.cpp:263]\r\n"
      "ha_archive.dll!_RTC_CheckStackVars()[stack.cpp:69]\r\n"
      "ha_archive.dll!archive_discover()[ha_archive.cc:311]\r\n"
      "server.dll!discover_handlerton()[handler.cc:7028]\r\n"
      "server.dll!plugin_foreach_with_mask()[sql_plugin.cc:2584]\r\n"
      "server.dll!ha_discover_table()[handler.cc:7072]\r\n"
      "server.dll!open_table_def()[table.cc:696]\r\n"
      "server.dll!do_command()[sql_parse.cc:1437]\r\n"
      "server.dll!threadpool_process_request()[threadpool_common.cc:438]\r\n"
      "ntdll.dll!RtlUserThreadStart()\r\n"
      "\r\n"
      "Connection ID (thread ID): 6\r\n",
   "_RTC_StackFailure|SIGABRT|archive_discover|discover_handlerton|plugin_foreach_with_mask|ha_discover_table", true},
  // the standard library's atomic storage, std::_Atomic_storage<T,N> to MSVC, is std::__atomic_base<T> to libstdc++ (the frame an
  // atomic load fails in), and an anonymous namespace is `anonymous namespace' to MSVC and (anonymous namespace) to gdb
  {"win_atomic_frames",
      "260912 10:00:07 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception 0xc0000005 ;\n"
      "Attempting backtrace. Include this in the bug report.\n\n"
      "server.dll!std::_Atomic_storage<unsigned int,4>::load()[atomic:1203]\n"
      "server.dll!Atomic_relaxed<unsigned int>::operator unsigned int()[my_atomic_wrapper.h:60]\n"
      "server.dll!`anonymous namespace'::helper()[buf0buf.cc:99]\n"
      "server.dll!buf_page_t::state()[buf0buf.h:700]\n"
      "server.dll!buf_page_t::in_file()[buf0buf.h:710]\n"
      "ntdll.dll!RtlUserThreadStart()\n",
   "SIGSEGV|std::__atomic_base<unsigned int>::load|Atomic_relaxed<unsigned int>::operator unsigned int|(anonymous namespace)::helper|buf_page_t::state", true},
  // the thread entry of the pthread emulation, pthread_start, is glibc's start_thread: the list's line for a crash in a RocksDB
  // thread ends in start_thread|clone, and this UID, which stops at start_thread, is a prefix of it
  {"win_thread_start",
      "260912 10:00:07 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception 0xc0000005 ;\n"
      "Attempting backtrace. Include this in the bug report.\n\n"
      "ha_rocksdb.dll!myrocks::Rdb_drop_index_thread::run()[rdb_threads.cc:120]\n"
      "ha_rocksdb.dll!myrocks::Rdb_thread::thread_func()[rdb_threads.cc:30]\n"
      "server.dll!pthread_start()[my_winthread.c:60]\n"
      "ucrtbase.dll!thread_start<unsigned int (__cdecl*)(void *),1>()\n"
      "KERNEL32.DLL!BaseThreadInitThunk()\n"
      "ntdll.dll!RtlUserThreadStart()\n",
   "SIGSEGV|myrocks::Rdb_drop_index_thread::run|myrocks::Rdb_thread::thread_func|start_thread", true},
  // a crashed temporary table by its Windows path, relative to the server's directory or in full, is the list's
  // Table sql-temptable-X, as the Linux path /dev/shm/.../tmp/#sql-temptable-... is
  {"win_temptable_crashed",
      "2026-10-05 20:30:00 5 [ERROR] mariadbd.exe: Table '26-mariadb-13.1.1-windows-x86_64-dbg\\tmp\\#sql-temptable-a758-4-0' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table sql-temptable-X is marked as crashed and should be repaired", true},
  {"win_temptable_crashed_full",
      "2026-10-05 20:30:00 5 [ERROR] mariadbd.exe: Table 'C:\\msys64\\dev\\shm\\Omatrix1\\26-mariadb\\tmp\\#sql-temptable-a758-4-0' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table sql-temptable-X is marked as crashed and should be repaired", true},
};
string fixture_dir(const string& root, const UidFixture& f) {
  string d = root + "/" + f.name;
  mkdirs(d + "/log");
  write_file(d + "/log/master.err", string(FX_HEAD) + f.log);
  return d;
}
}  // namespace

// every detection class, on a log of its own: the UID has to come out exactly as the chain gives it
// The last resort of the UniqueID chain. These logs hold nothing the typed scan knows, so the
// string is picked by the fallback, and several of them are rewritten by a rule of their own.
struct FbFixture { const char* name; const char* log; const char* uid; bool fix = false; };   // fix: a fix Linux has not taken, checked where kTakeFixes
const FbFixture FALLBACK_FIXTURES[] = {
  {"binlog_rollback",
      "mariadbd: /test/13.1/sql/log.cc:2323: virtual int MYSQL_BIN_LOG::rollback(THD*, bool): Assertion `all' failed.\n",
   "FALLBACK|all"},
  {"ps_version",
      "mariadbd: /test/13.1/sql/dd/impl/dictionary_impl.cc:120: virtual uint dd::Dictionary_impl::get_actual_P_S_version(THD*): Assertion `!error' failed.\n",
   "FALLBACK|uint dd..Dictionary_impl..get_actual_P_S_version.THD... Assertion ..error. failed"},
  {"dict0dd_error",
      "2026-09-05  5:00:01 0 [ERROR] InnoDB: Assertion failure: dict0dd.cc:5123:!error\n",
   "FALLBACK|dict0dd.cc:5.....error"},
  {"undo_slot_discard",
      "2026-09-05  5:00:01 0 [ERROR] InnoDB: Cannot find a free slot for an undo log\n"
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Assertion failure: dict0dd.cc:5123:dd_table_discard_tablespace\n",
   "FALLBACK|RSEG.....dd_table_discard_tablespace"},
  {"undo_slot_strcmp",
      "2026-09-05  5:00:01 0 [ERROR] InnoDB: Cannot find a free slot for an undo log\n"
      "2026-09-05  5:00:02 0 [ERROR] InnoDB: Assertion failure: dict0dd.cc:5123:strcmp(table->name.m_name, table_name) == 0\n",
   "FALLBACK|RSEG.....strcmp.table->name.m_name, table_name. == 0"},
  // the older InnoDB wording, which names the file and the line
  {"innodb_in_file",
      "2026-09-05  5:00:05 0x7f0a InnoDB: Assertion failure in file /test/10.6/storage/innobase/btr/btr0cur.cc line 336\n"
      "InnoDB: We intentionally generate a memory trap.\n",
   "FALLBACK|storage/innobase/btr/btr0cur.cc line 336"},
  // no assertion at all: the first stack frame that is not the signal handler
  {"frames_only",
      "260905  5:00:05 [ERROR] mysqld got signal 11 ;\n"
      "mysqld(my_print_stacktrace+0x2e)[0x55d000000001]\n"
      "mysqld(handle_fatal_signal+0x30)[0x55d000000002]\n"
      "/lib/x86_64-linux-gnu/libc.so.6(+0x42520)[0x7f1000000003]\n"
      "mysqld(Item_func::fix_fields(THD*, Item**)+0x10)[0x55d000000004]\n",
   "FALLBACK|Item_func..fix_fields"},
  // an InnoDB assertion whose condition is only "0": the file and the line are kept instead
  {"innodb_zero",
      "2026-09-05  5:00:05 0x7f0a InnoDB: Assertion failure: page0zip.cc:1234:0\n"
      "InnoDB: We intentionally generate a memory trap.\n",
   "FALLBACK|page0zip.cc.1234.0"},
  // the same older wording from a Windows server, whose path has a drive: the script's main pipeline leaves the colon of
  // C:\ alone (its dots are for the quotes, brackets and the like), and so does the port; the CRLF line ends stay out of the UID
  {"innodb_in_file_windows",
      "2026-10-05 15:30:00 0 [ERROR] InnoDB: Assertion failure in file C:\\test\\13.1\\storage\\innobase\\fts\\fts0fts.cc line 2108\r\n"
      "InnoDB: Failing assertion: result != FTS_INVALID\r\n",
   "FALLBACK|C:\\test\\13.1\\storage\\innobase\\fts\\fts0fts.cc line 2108", true},
};
// One log per rule of the error-log scan, so every typed prefix and every severity tier is walked.
// The UID is what error_log_scan.sh top gives; the deep run compares the two.
struct ElsFixture { const char* name; const char* log; const char* uid; bool windows = false; };   // windows: as UidFixture
const ElsFixture ELS_FIXTURES[] = {
  {"els_assert",
      "mariadbd: /test/13.1/sql/item_func.cc:1234: void Item_func_x::fix(): Assertion `page not corrupted' failed.\n",
   "ASSERT|sql/item_func.cc|Assertion `page not corrupted' failed"},
  {"els_asan",
      "==123==ERROR: AddressSanitizer: allocator is out of memory trying to allocate 0x10 bytes\n",
   "ASAN|==X==ERROR: AddressSanitizer: allocator is out of memory trying to allocate 0x10 bytes"},
  {"els_msan",
      "==123==WARNING: MemorySanitizer: use-of-uninitialized-value in alloc_root\n",
   "MSAN|==X==WARNING: MemorySanitizer: use-of-uninitialized-value in alloc_root"},
  {"els_lsan",
      "Indirect leak of 128 byte(s) in 1 object(s) allocated from:\n",
   "LSAN|Indirect leak of N bytes in M objects allocated from:"},
  {"els_glibc",
      "corrupted size vs. prev_size\n",
   "GLIBC|corrupted size vs. prev_size"},
  {"els_mutex_line",
      "safe_mutex: Trying to lock mutex at /test/13.1/sql/x.cc, line 55 more than 1 time\n",
   "MUTEX_ERROR|safe_mutex: Trying to lock mutex at /test/X/sql/x.cc, line 55 more than 1 time"},
  {"els_uninit_mutex",
      "Trying to lock uninitialized mutex at /test/13.1/sql/x.cc, allocated at line 55\n",
   "MUTEX_ERROR|Trying to lock uninitialized mutex at /test/X/sql/x.cc, allocated at line 55"},
  {"els_innodb_rollback",
      "2026-09-05  5:00:01 0 [Warning] InnoDB: Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: COMPACT RECORD\n",
   "INNODB_ERROR|Record in index X of table Y was not found on rollback, trying to insert: TUPLE Z at: COMPACT RECORD"},
  {"els_innodb_note",
      "2026-09-05  5:00:01 0 [Note] InnoDB: allocated a new page for the change buffer\n",
   "INNODB_NOTE|allocated a new page for the change buffer"},
  {"els_opentable",
      "OpenTable: table t1 is corrupted\n",
   "OPENTABLE|OpenTable: table t1 is corrupted"},
  {"els_lrecl",
      "Table/File lrecl mismatch (80,100)\n",
   "OPENTABLE|Table/File lrecl mismatch (X,Y)"},
  {"els_index_init",
      "index_init CONNECT: the index is corrupted\n",
   "OPENTABLE|index_init CONNECT: the index is corrupted"},
  {"els_marked_crashed",
      "2026-09-05  5:00:01 0 [ERROR] mariadbd: Table './test/t1' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table './test/X' is marked as crashed and should be repaired"},
  {"els_wsrep_warning",
      "2026-09-05  5:00:01 0 [Warning] WSREP: the node fell behind and is corrupted\n",
   "WSREP_WARNING|the node fell behind and is corrupted"},
  // a Windows server's lines: mariadbd.exe: where a Linux log has mariadbd:. The scan, the filter and the
  // typed prefixes are written against the Linux line, so each of these has to come out as its twin does
  {"els_win_marked_crashed",
      "2026-10-05 15:06:56 4 [ERROR] mariadbd.exe: Table 't1' is marked as crashed and should be repaired\n",
   "MARKED_AS_CRASHED|Table X is marked as crashed and should be repaired", true},
  {"els_win_typed",
      "2026-10-05 15:06:56 4 [ERROR] mariadbd.exe: Table 't1' has a corrupted index\r\n",
   "MARIADBD_ERROR|Table X has a corrupted index", true},
  // the filter lists "mariadbd: Cannot load from mysql\..*The table is probably corrupted", so this line is dropped
  {"els_win_filtered",
      "2026-10-05 15:26:39 7 [ERROR] mariadbd.exe: Cannot load from mysql.user. The table is probably corrupted\n",
   "", true},
};
static string els_fixture_log(const string& root, const ElsFixture& f) {
  string d = root + "/" + f.name;
  mkdirs(d + "/log");
  string p = d + "/log/master.err";
  write_file(p, string(FX_HEAD) + f.log);
  return p;
}
static string fb_fixture_dir(const string& root, const FbFixture& f) {
  string d = root + "/" + f.name;
  mkdirs(d + "/log");
  write_file(d + "/log/master.err", string(FX_HEAD) + f.log);
  return d;
}
// The path scrub reads the build root off TEST_DIR, and the fixtures below are Linux logs that name
// /test/13.1/..., so they run with TEST_DIR=/test whatever the box has: under MSYS2 it is /c/test.
struct TestDirPin {
  string saved;
  explicit TestDirPin(const string& d) : saved(g_cfg.test_dir) { g_cfg.test_dir = d; }
  ~TestDirPin() { g_cfg.test_dir = saved; }
};
static void st_detect_classes() {
  TestDirPin pin("/test");
  string tmp = st_tmp() + "/classes";
  for (auto& f : UID_FIXTURES) {
    string d = fixture_dir(tmp, f);
    UidResult r;
    UidOptions o;
    o.wait_core = false;
    bool ok = uid_for_dir(d, r, o);
    // what the chain answers: the UniqueID, or the Assert line it stops on
    st_eq(ok ? trim(r.uid) : trim(r.err), f.uid, string("UID of a ") + f.name + " log");
  }
  for (auto& f : FALLBACK_FIXTURES) {
    if (f.fix && !kTakeFixes) continue;
    string d = fb_fixture_dir(tmp, f);
    string e;
    st_eq(trim(uid_fallback(d + "/log/master.err", &e)), f.uid, string("the fallback UID of a ") + f.name + " log");
  }
  for (auto& f : ELS_FIXTURES) {
    string out;
    els_run("top", {els_fixture_log(tmp, f)}, false, out, nullptr);
    st_eq(trim(out), f.uid, string("the error-log scan types a ") + f.name + " line");
  }
  // s/mariadbd.exe/mariadbd/ is done as the lines are read, so every rule sees a Windows line as its Linux twin, the
  // cleaned text included: both come out the same
  for (auto& f : ELS_FIXTURES) {
    string name = f.name, out;
    if (name != "els_win_marked_crashed" && name != "els_marked_crashed") continue;
    els_run("clean", {els_fixture_log(tmp, f)}, false, out, nullptr);
    st_eq(trim(out), "ERROR. mariadbd: Table .* is marked as crashed and should be repaired", "els clean on a " + name + " line");
  }
  // the verbs that print a UID, on the sanitizer fixture: that path has no wait for a core
  string sd = tmp + "/asan_uaf";
  st_check(quiet_call(cmd_t, {sd}) == 0, "t on a sanitizer log");
  st_check(quiet_call(cmd_tt, {sd}) == 0, "tt on a sanitizer log");
  st_check(quiet_call(cmd_sts, {sd}) == 0, "sts on a sanitizer log");
  // and the error-log scan and the fallback on it end as the two scripts do
  string els_sh = script_path("error_log_scan.sh"), fts_sh = script_path("fallback_text_string.sh");
  if (!file_exists(els_sh) || !file_exists(fts_sh)) st_skip("els and fts on a sanitizer log against the scripts, which are not in " + g_paths.qa);
  else {
    int vrc = -1;
    for (const char* mode : {"errors", "lastline", "check"}) {
      string mine = call_output(cmd_els, {mode, sd + "/log/master.err"}, &vrc);
      CmdResult sh = run_capture({els_sh, mode, sd + "/log/master.err"}, 300);
      st_check(vrc == (sh.rc == 0 ? 0 : 1) && trim(mine) == trim(sh.out), string("els ") + mode + " on a sanitizer log says what error_log_scan.sh says");
    }
    string mine = call_output(cmd_fts, {sd + "/log/master.err"}, &vrc);
    CmdResult sh = run_capture({fts_sh, sd + "/log/master.err"}, 300);
    string want;
    for (auto& l : split_lines(sh.out)) if (starts_with(l, "FALLBACK|")) want = l;
    st_check(vrc == sh.rc && trim(mine) == want, "fts on a sanitizer log ends as fallback_text_string.sh does");
  }
  remove_tree(tmp);
}
// the same logs through the mariadb-qa shell chain: the port and the scripts have to agree
static void st_detect_parity() {
  string tmp = st_tmp() + "/parity";
  string script = script_path("new_text_string.sh");
  if (!file_exists(script)) return;
  for (auto& f : UID_FIXTURES) {
    if (f.windows) continue;
    string d = fixture_dir(tmp, f);
    CmdResult c = run_capture({script}, 600, d);
    string shell_uid = c.out.empty() ? "" : trim(split_lines(c.out)[0]);
    st_eq(shell_uid, f.uid, string("new_text_string.sh agrees on a ") + f.name + " log");
  }
  {
    string els = script_path("error_log_scan.sh");
    if (file_exists(els))
      for (auto& f : ELS_FIXTURES) {
        if (f.windows) continue;
        CmdResult c = run_capture({els, "top", els_fixture_log(tmp, f)}, 600);
        st_eq(c.out.empty() ? "" : trim(split_lines(c.out)[0]), f.uid, string("error_log_scan.sh agrees on a ") + f.name + " line");
      }
  }
  string fbs = script_path("fallback_text_string.sh");
  if (file_exists(fbs))
    for (auto& f : FALLBACK_FIXTURES) {
      if (f.fix && !kTakeFixes) continue;
      string d = fb_fixture_dir(tmp, f);
      CmdResult c = run_capture({fbs}, 600, d);
      string shell_uid = c.out.empty() ? "" : trim(split_lines(c.out)[0]);
      st_eq(shell_uid, f.uid, string("fallback_text_string.sh agrees on a ") + f.name + " log");
    }
  remove_tree(tmp);
}

// the SQL side that needs no server: the pattern files, the line cleanup and the four transforms
static void st_sources_plain() {
  string tmp = st_tmp() + "/src";
  mkdirs(tmp);
  // a grep basic regex becomes a PCRE
  st_eq(bre_to_pcre("a\\|b"), "a|b", "bre_to_pcre: alternation");
  st_eq(bre_to_pcre("x\\+"), "x+", "bre_to_pcre: plus");
  st_eq(bre_to_pcre("a+b"), "a\\+b", "bre_to_pcre: a plain plus is a literal");
  st_check(!bre_to_pcre("\\(a\\)").empty(), "bre_to_pcre: groups");
  // a pattern file: comments and blank lines are left out
  write_file(tmp + "/pat", "# a comment\n\nAssertion.*failed\nsignal 11\n");
  auto pats = read_pattern_file(tmp + "/pat");
  st_check(pats.size() == 2, "read_pattern_file drops comments and blanks");
  RxSet set;
  string rerr;
  st_check(set.compile(pats, &rerr), "RxSet compiles a pattern list");
  st_check(set.hit("mariadbd: Assertion `x' failed."), "RxSet matches");
  st_check(!set.hit("nothing to see"), "RxSet says no when nothing matches");
  RxSet bad;
  st_check(!bad.compile({"("}, &rerr), "RxSet refuses a broken pattern");
  st_check(!rerr.empty(), "the broken pattern comes with a reason");
  // the per-line cleanup
  st_eq(sql_line_cleanup("/test/main/t/x.sql:SELECT 1;"), "SELECT 1;", "cleanup takes the file prefix off");
  st_eq(sql_line_cleanup("SELECT 1;#NOERROR"), "SELECT 1;", "cleanup takes ;#NOERROR off");
  st_eq(sql_line_cleanup("SELECT 1;#ERROR: 1064"), "SELECT 1;", "cleanup takes ;#ERROR off");
  st_eq(sql_line_cleanup("SELECT 1;"), "SELECT 1;", "cleanup leaves a plain line alone");
  // the transforms
  vector<string> lines = {"CREATE TABLE t1 (a INT) ENGINE=InnoDB;", "CREATE TABLE t2 (b INT) ENGINE=MyISAM;"};
  sql_engine_swap(lines, "Aria", 100);
  st_check(lines[0].find("Aria") != string::npos && lines[1].find("Aria") != string::npos, "engine swap reaches every line");
  vector<string> half = {"a InnoDB", "b InnoDB", "c InnoDB", "d InnoDB"};
  sql_engine_swap(half, "Aria", 50);
  int swapped = 0;
  for (auto& l : half) if (l.find("Aria") != string::npos) swapped++;
  st_check(swapped == 2, "a 50% engine swap reaches half the lines");
  vector<string> il = {"SELECT 1;", "SELECT 2;", "SELECT 3;", "SELECT 4;"};
  sql_interleave(il, "FLUSH TABLES;", 2);
  st_check(il.size() == 6 && il[1] == "FLUSH TABLES;", "interleave puts the block in every second line");
  vector<string> keep = {"SELECT 1;"};
  sql_interleave(keep, "", 2);
  st_check(keep.size() == 1, "interleave with nothing to insert leaves the SQL alone");
  vector<string> ct = {"CREATE TABLE t7 (a INT);"};
  sql_swap_create_table_names(ct);
  st_check(ct[0].find("t1") != string::npos, "create-table names collapse to t1");
  vector<string> at = {"SELECT a FROM t7;"};
  sql_swap_all_table_names(at);
  st_check(at[0].find("t1") != string::npos, "table names collapse to t1");
  // the area table
  st_check(!areas_all().empty(), "the area table is filled");
  st_check(area_by_name("default") != nullptr, "the default area is there");
  st_check(area_by_name("no such area") == nullptr, "an unknown area name gives nothing");
  Xoshiro256pp r;
  r.seed(12345);
  vector<const Area*> some;
  for (auto& a : areas_all()) some.push_back(&a);
  st_check(area_pick(some, r) != nullptr, "area_pick picks one");
  st_check(area_pick({}, r) == nullptr, "area_pick on an empty list gives nothing");
  Basedir fake;
  fake.path = tmp;
  fake.name = "fake";
  fake.vendor = Vendor::MariaDB;
  string why;
  bool needy_on = false, plain_on = false;
  for (auto& a : areas_all()) {
    bool ok = area_available(a, fake, &why);
    if (!a.needs.empty() && ok) needy_on = true;
    if (a.name == "default" && ok) plain_on = true;
  }
  st_check(!needy_on, "an area that needs a plugin is off on a build with no plugins");
  st_check(plain_on, "and the default area is on");
  st_check(areas_available(fake, {"default"}).size() <= 1, "areas_available narrows to the name given");
  remove_tree(tmp);
}

// an input past the size of the pipe buffer, the way `omnium replay` hands a large in.sql over
static void st_run_capture_in_large() {
  string big;
  while (big.size() < (1u << 20)) big += "SELECT 1;\n";
  CmdResult c = run_capture_in({"cat"}, big, 10);
  st_check(c.rc == 0 && c.out == big, "run_capture_in feeds a stdin larger than the pipe buffer");
  CmdResult e = run_capture_in({"/bin/sh", "-c", "exit 3"}, big, 10);
  st_check(e.rc == 3, "a child that never reads that stdin still gives its exit code");
  CmdResult t = run_capture_in({"sleep", "5"}, "", 1);
  st_check(t.rc == 124, "and the timeout still ends a child that outlives it");
}
// A server that refuses an option logs why, then "Aborting", and is still up for a moment. The
// start does not wait out its timeout for that, and says why from the log. The stand-in server is
// a script that writes those lines, in the MariaDB and in the MySQL 8.0 form.
static void st_start_refused() {
  string tmp = st_tmp() + "/refused";
  const char* forms[][2] = {
    {"[ERROR] mariadbd: unknown variable 'omnium_st=1'", "[ERROR] Aborting"},
    {"[ERROR] [MY-000067] [Server] unknown variable 'omnium_st=1'.", "[ERROR] [MY-010119] [Server] Aborting"},
  };
  for (auto& f : forms) {
    remove_tree(tmp);
    mkdirs(tmp + "/bin");
    write_file(tmp + "/bin/mariadbd", string("#!/bin/sh\necho \"2026-09-05  5:00:00 0 ") + f[0] + "\"\necho \"2026-09-05  5:00:00 0 " + f[1] + "\"\nexec sleep 2\n");
    chmod((tmp + "/bin/mariadbd").c_str(), 0755);
    Basedir fb;
    fb.path = tmp;
    fb.name = "refused";
    fb.bin = tmp + "/bin/mariadbd";
    fb.vendor = Vendor::MariaDB;
    fb.version = "13.1.0";
    Instance in;
    in.bd = &fb;
    in.set_paths(tmp + "/trial");
    in.port = port_pick();
    mkdirs(in.datadir);
    mkdirs(in.logdir);
    double t0 = now_ms();
    bool ok = in.start_only(60);
    in.kill_hard();
    st_check(!ok && now_ms() - t0 < 30000, string("a start the server refuses ends at once: ") + f[1]);
    st_check(in.start_note.find("unknown variable 'omnium_st=1'") != string::npos, "and says why [" + in.start_note + "]");
  }
  remove_tree(tmp);
}
// a socket that listens on 127.0.0.1:<port> and counts what connects to it
struct StCounter {
  int fd = -1;
  std::atomic<int> seen{0};
  std::atomic<bool> stop{false};
  std::thread th;
  bool open(int port) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 16) != 0) { close(fd); fd = -1; return false; }
    th = std::thread([this] {
      while (!stop) {
        struct pollfd pf{fd, POLLIN, 0};
        if (poll(&pf, 1, 100) > 0 && (pf.revents & POLLIN)) {
          int c = accept(fd, nullptr, nullptr);
          if (c >= 0) { seen++; close(c); }
        }
      }
    });
    return true;
  }
  ~StCounter() { stop = true; if (th.joinable()) th.join(); if (fd >= 0) close(fd); }
};
// A TCP server counts as up when its own log, from where its start began, says it listens: another server may answer on its port, and a
// start that lost the port dies at the bind. (A trial once ran its SQL on the server that held the port, and was saved as a crash of the one
// that never had it.) The stand-in servers are scripts that write what a server writes; a listener on the port counts what connects, and a
// start whose log has not said it listens must not have connected.
static void st_start_tcp_gate() {
  string tmp = st_tmp() + "/tcpgate";
  struct Case { const char* name; const char* body; bool old_line; int timeout; bool probed; };
  const Case cases[] = {
    {"a server that has not said it listens", "echo \"2026-10-05 21:00:00 0 [Note] Plugin 'FEEDBACK' is disabled.\"\nexec sleep 8\n", false, 3, false},
    {"a server that says it listens", "echo \"2026-10-05 21:00:00 0 [Note] mariadbd: ready for connections.\"\nexec sleep 8\n", false, 3, true},
    {"a restart on a log that has the first start's line", "echo \"2026-10-05 21:00:00 0 [Note] InnoDB: Starting crash recovery.\"\nexec sleep 8\n", true, 3, false},
    {"a server that lost its port",
     "echo \"2026-10-05 21:00:00 0 [ERROR] Can't start server: Bind on TCP/IP port. Got error: 10048: Only one usage of each socket address (protocol/network address/port) is normally permitted.\"\n"
     "echo \"2026-10-05 21:00:00 0 [ERROR] Aborting\"\nexit 1\n", false, 30, false},
  };
  for (auto& c : cases) {
    remove_tree(tmp);
    mkdirs(tmp + "/bin");
    write_file(tmp + "/bin/mariadbd", string("#!/bin/sh\n") + c.body);
    chmod((tmp + "/bin/mariadbd").c_str(), 0755);
    Basedir fb;
    fb.path = tmp;
    fb.name = "tcpgate";
    fb.bin = tmp + "/bin/mariadbd";
    fb.vendor = Vendor::MariaDB;
    fb.version = "13.1.0";
    Instance in;
    in.bd = &fb;
    in.set_paths(tmp + "/trial");
    in.tcp = true;
    in.port = port_pick();
    StCounter lc;
    if (in.port == 0 || !lc.open(in.port)) { st_skip(string("the TCP start gate: no port to listen on for ") + c.name); continue; }
    mkdirs(in.datadir);
    mkdirs(in.logdir);
    if (c.old_line) write_file(in.errlog, "2026-10-05 20:59:00 0 [Note] mariadbd: ready for connections.\n");
    double t0 = now_ms();
    bool ok = in.start_only(c.timeout);
    double secs = (now_ms() - t0) / 1000.0;
    in.kill_hard();
    string what = string(c.name) + " [" + in.start_note + "]";
    st_check(!ok, "TCP start: " + what + " does not come up");
    st_check(c.probed ? lc.seen > 0 : lc.seen == 0, string("TCP start: ") + (c.probed ? "the port is tried once the log says it listens: " : "the port is not tried before the log says it listens: ") + what + fmt(" (%d connections)", lc.seen.load()));
    if (string(c.name) == "a server that lost its port") {
      st_check(port_clash_in_log(read_file(in.errlog)), "TCP start: the log of a server that lost its port says so");
      st_check(secs < 20, "TCP start: and the start ends with the server, not at its timeout");
    } else if (!c.probed) {
      st_check(in.start_note == fmt("not ready within %d s", c.timeout), "TCP start: and the note says it never said it listens [" + in.start_note + "]");
    }
  }
  remove_tree(tmp);
}
// The ports a server is given: below those Windows gives its clients, and free by the bind the server makes itself (a bind on 127.0.0.1 is told
// "free" for a port that a server listens on, and for the source port of an open connection)
static void st_ports() {
  int lo = 65535, hi = 0;
  for (int i = 0; i < 100; i++) {
    int p = port_pick();
    if (p > 0) { lo = std::min(lo, p); hi = std::max(hi, p); }
  }
  st_check(lo >= 13001 && hi <= (kHostMsys2 ? 49151 : 65000), fmt("port_pick stays in its range (%d to %d)", lo, hi));
  if (!kHostMsys2) { st_skip("port_free: the server's own bind is what MSYS2 picks by; Linux keeps its probe on 127.0.0.1"); return; }
  int port = port_pick();
  int ls = socket(AF_INET6, SOCK_STREAM, 0);
  int off = 0;
  if (ls >= 0) setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  struct sockaddr_in6 a6{};
  a6.sin6_family = AF_INET6;
  a6.sin6_addr = in6addr_any;
  a6.sin6_port = htons((uint16_t)port);
  bool listening = ls >= 0 && bind(ls, (struct sockaddr*)&a6, sizeof(a6)) == 0 && listen(ls, 4) == 0;
  st_check(listening, "port_free: a socket holds a port the way a server does");
  if (listening) {
    st_check(!port_free(port), "port_free: a port that a server listens on is not free");
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4{};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a4.sin_port = htons((uint16_t)port);
    if (c >= 0 && connect(c, (struct sockaddr*)&a4, sizeof(a4)) == 0) {
      struct sockaddr_in l{};
      socklen_t n = sizeof(l);
      getsockname(c, (struct sockaddr*)&l, &n);
      st_check(!port_free(ntohs(l.sin_port)), "port_free: nor is the source port of an open connection");
    } else {
      st_check(false, "port_free: a client connects to the port");
    }
    if (c >= 0) close(c);
  }
  if (ls >= 0) close(ls);
  bool again = false;
  for (int i = 0; i < 30 && !again; i++) { again = port_free(port); if (!again) usleep(100000); }
  st_check(again, "port_free: the port is free again once the server is gone");
}

// the plumbing: the workdir and its files, the small helpers, the process helpers, the mail text.
// Everything here works on a temporary /data and /dev/shm, so a real run is never touched.
static void st_plumbing() {
  string tmp = st_tmp() + "/plumb";
  mkdirs(tmp + "/data");
  mkdirs(tmp + "/shm");
  // --- a build laid out as a source tree, and one whose name says nothing ------------------------
  {
    auto stub = [&](const string& path, const char* version_line) {
      mkdirs(dirname_of(path));
      write_file(path, string("#!/bin/sh\necho \"") + version_line + "\"\nexit 0\n");
      chmod(path.c_str(), 0755);
    };
    string it = tmp + "/intree";
    stub(it + "/sql/mariadbd", "mariadbd  Ver 13.1.0-MariaDB-debug for Linux (x86_64)");
    stub(it + "/client/mariadb", "");
    stub(it + "/client/mariadb-admin", "");
    Basedir ib;
    st_check(basedir_probe(it, ib), "a build with sql/mariadbd and no bin/ probes");
    st_check(ib.in_tree, "and it is read as an in-tree build");
    st_check(!ib.client.empty() && !ib.admin.empty(), "its client tools come from client/");
    // the directory name says nothing, so the version, vendor and debug flag come from the binary
    st_check(ib.vendor == Vendor::MariaDB, "the vendor comes from the binary");
    st_eq(ib.version, "13.1.0", "the version comes from the binary");
    st_eq(ib.series, "13.1", "and the series with it");
    st_check(ib.dbg, "a debug binary is seen as one");
    string pc = tmp + "/perconatree";
    stub(pc + "/bin/mysqld", "mysqld  Ver 8.0.40-31 for Linux on x86_64 (Percona Server (GPL))");
    stub(pc + "/bin/mysql", "");
    Basedir pb;
    st_check(basedir_probe(pc, pb) && pb.vendor == Vendor::Percona, "a Percona binary is read as Percona");
    string ms = tmp + "/mysqltree";
    stub(ms + "/bin/mysqld", "mysqld  Ver 8.4.3 for Linux on x86_64 (MySQL Community Server - GPL)");
    stub(ms + "/bin/mysql", "");
    Basedir mb;
    st_check(basedir_probe(ms, mb) && mb.vendor == Vendor::MySQL, "a MySQL binary is read as MySQL");
  }
  // --- the small process helpers ----------------------------------------------------------------
  {
    st_check(!role_stop_requested(), "no stop has been asked for outside a run");
    raise_fd_limit();
    struct rlimit rl{};
    st_check(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur == rl.rlim_max, "the open-file limit is lifted to the hard limit");
    if (kHostMsys2)
      st_skip("the out-of-memory score of a child: /proc/<pid>/oom_score_adj is the Linux "
              "kernel's, and Windows has no OOM killer");
    else {
      pid_t sp = spawn_program({"/bin/sh", "-c", "sleep 30; exit 0"}, "/dev/null", "", true, {}, false);
      st_check(set_oom_score(sp, 500), "the out-of-memory score of a child is raised");
      st_eq(trim(read_file(fmt("/proc/%d/oom_score_adj", (int)sp))), "500", "and the kernel holds the new score");
      kill_group(sp, SIGKILL);
      wait_pid(sp, 5000);
      if (geteuid() != 0) st_check(!set_oom_score(getpid(), -1000), "a score below 0 is refused without root, and says so");
    }
    // the core size cap works by inheritance: a trial sets it, and the server it starts gets it
    struct rlimit save{}, cap{};
    if (getrlimit(RLIMIT_CORE, &save) == 0) {
      cap.rlim_cur = (rlim_t)12 * 1024 * 1024 * 1024;
      cap.rlim_max = save.rlim_max;
      if (setrlimit(RLIMIT_CORE, &cap) == 0) {
        CmdResult c = run_capture({"/bin/sh", "-c", "ulimit -c"}, 60);
        long blocks = to_long(trim(c.out), 0);                 // the shell reports 512 or 1024 byte blocks
        st_check(blocks == 25165824 || blocks == 12582912, "a child inherits the core size cap [" + trim(c.out) + "]");
      }
      setrlimit(RLIMIT_CORE, &save);
    }
  }
  // --- the client summary line -----------------------------------------------------------------
  {
    // performed is every query sent, failures included, so a total of 7767 with 6018 failed is
    // 22.52% successful. pquery.log and default.node.tld_general.log both use this one line.
    st_check(node_summary_line(6018, 7767) == "* NODE SUMMARY: 6018/7767 queries failed, (22.52% were successful)\n",
             "the NODE SUMMARY line counts the failures inside the total");
    st_check(node_summary_line(0, 0) == "* NODE SUMMARY: 0/0 queries failed, (0.00% were successful)\n",
             "the NODE SUMMARY line survives a trial where no query ran");
    st_check(node_summary_line(5, 5) == "* NODE SUMMARY: 5/5 queries failed, (0.00% were successful)\n",
             "the NODE SUMMARY line reads 0% when every query failed");
  }
  // --- the workdir -----------------------------------------------------------------------------
  {
    string save_data = g_cfg.data_dir, save_shm = g_cfg.shm_dir, save_seen = g_paths.seen_file;
    Workdir save_wd = g_wd;
    g_cfg.data_dir = tmp + "/data";
    g_cfg.shm_dir = tmp + "/shm";
    g_paths.seen_file = tmp + "/data/omnium.seen";
    st_check(workdir_create(), "workdir_create makes a run directory");
    st_check(dir_exists(g_wd.dir) && starts_with(g_wd.id, "O"), "the workdir has an O id");
    st_check(file_exists(g_wd.lock), "the run holds a pid file");
    st_check(workdir_is_omnium(g_wd.dir), "workdir_is_omnium says yes to its own");
    st_check(!workdir_is_omnium(tmp), "workdir_is_omnium says no to a plain directory");
    ledger_append("trial", "1 build area saved-new UID|a|b|c|d");
    ledger_append("note", "a second line");
    auto led = ledger_read();
    st_check(led.size() >= 2, "the ledger reads back what was written");
    status_write({{"state", "running"}, {"trials", "2"}});
    auto stt = status_read(g_wd.dir);
    bool found = false;
    for (auto& kv : stt) if (kv.first == "state" && kv.second == "running") found = true;
    st_check(found, "the status file reads back");
    st_check(!status_read_file(tmp + "/no_such_file").size(), "status_read_file on nothing gives nothing");
    pids_add(getpid(), "selftest", "this process");
    auto pr = pids_read(g_wd.dir);
    st_check(pr.size() == 1 && std::get<0>(pr[0]) == getpid(), "a pid goes in the pid list");
    pids_remove(getpid());
    st_check(pids_read(g_wd.dir).empty(), "and comes out again");
    st_check(ends_with(workdir_trial_dir(7), "/7"), "workdir_trial_dir names the trial directory");
    string here = abs_path(".");
    if (chdir(g_wd.dir.c_str()) == 0) {
      st_eq(workdir_id_from_cwd(), g_wd.id, "the workdir id is read back from the cwd");
      if (chdir(here.c_str()) != 0) { /* the temp dir goes anyway */ }
    }
    st_check(!seen_lookup("no|such|uid|at|all").has_value(), "seen_lookup on an unknown UID gives nothing");
    seen_record("SIGSEGV|a|b|c|d", g_wd.id, "saved-new");
    auto se = seen_lookup("SIGSEGV|a|b|c|d");
    st_check(se.has_value() && se->count == 1, "seen_record writes the first sighting");
    seen_record("SIGSEGV|a|b|c|d", g_wd.id, "known");
    se = seen_lookup("SIGSEGV|a|b|c|d");
    st_check(se.has_value() && se->count == 2, "a second sighting counts up");
    string wid = g_wd.id, wdir = g_wd.dir;
    release_lock();
    st_check(!file_exists(wdir + "/omnium.pid"), "release_lock takes the pid file away");
    st_check(workdir_open(wid, false), "workdir_open finds the run by id");
    st_check(workdir_open(wdir, false), "workdir_open finds the run by path");
    st_check(!workdir_open("O000000", false), "workdir_open says no to a run that is not there");
    release_lock();
    g_cfg.data_dir = save_data;
    g_cfg.shm_dir = save_shm;
    g_paths.seen_file = save_seen;
    g_wd = save_wd;
  }
  // --- the small helpers -----------------------------------------------------------------------
  {
    write_file(tmp + "/a.txt", "one\ntwo\n");
    st_check(file_mtime(tmp + "/a.txt") > 0, "file_mtime reads a time");
    st_check(file_mtime(tmp + "/not_there") == 0, "file_mtime of nothing is 0");
    mkdirs(tmp + "/tree/sub");
    write_file(tmp + "/tree/sub/b.txt", "x\n");
    string cerr;
    st_check(copy_tree(tmp + "/tree", tmp + "/tree_copy", &cerr), "copy_tree copies a directory");
    st_check(file_exists(tmp + "/tree_copy/sub/b.txt"), "the copy has the files");
    st_check(move_tree(tmp + "/tree_copy", tmp + "/tree_moved", &cerr), "move_tree moves it");
    st_check(!dir_exists(tmp + "/tree_copy") && dir_exists(tmp + "/tree_moved"), "and the old name is gone");
    st_check(!now_hms().empty() && !now_stamp().empty() && !date_ddmmyy().empty(), "the clock helpers print");
    st_eq(hex64(255), "00000000000000ff", "hex64 pads to 16 digits");
    st_check(dir_total_bytes("/dev/shm") > 0, "dir_total_bytes");
    int pct = dir_used_pct("/dev/shm");
    st_check(pct >= 0 && pct <= 100 && dir_used_pct(tmp + "/no_such_dir") == 0, "dir_used_pct gives a percentage, and 0 for a path that is not there");
    st_check(ram_total_bytes() > 0 && ram_available_bytes() > 0, "the RAM helpers read /proc/meminfo");
    bool load_ok = false;                                        // two reads of a figure that moves: on a busy box one of a few pairs agrees
    for (int i = 0; i < 6 && !load_ok; i++) {
      double la = load_average(), pl = atof(read_file("/proc/loadavg").c_str());
      load_ok = (la > pl ? la - pl : pl - la) < 0.5 + pl / 10;
      if (!load_ok) usleep(300000);
    }
    st_check(load_ok, "load_average reads the one-minute load");
    st_check(!stamp_of(now_s()).empty(), "stamp_of prints a time");
    CmdResult sh = run_shell("echo hello", 10, tmp);
    st_check(sh.rc == 0 && trim(sh.out) == "hello", "run_shell runs a command line");
    CmdResult in = run_capture_in({"cat"}, "from stdin\n", 10, tmp, {});
    st_check(trim(in.out) == "from stdin", "run_capture_in feeds stdin");
    {
      // two threads writing one path: each write has a temporary of its own, so each one lands
      string wf = tmp + "/two_writers.txt";
      std::atomic<int> bad{0};
      auto writer = [&](char c) { for (int i = 0; i < 500; i++) if (!write_file(wf, string(64, c))) bad++; };
      std::thread t1(writer, 'a'), t2(writer, 'b');
      t1.join();
      t2.join();
      string got = read_file(wf);
      int left = 0;
      std::error_code ec;
      for (auto& e : fs::directory_iterator(tmp, ec)) if (starts_with(e.path().filename().string(), "two_writers.txt.tmp")) left++;
      st_check(bad == 0 && left == 0 && (got == string(64, 'a') || got == string(64, 'b')), "two threads writing one file both get their write in");
    }
    st_run_capture_in_large();
    st_start_refused();
    st_start_tcp_gate();
    st_ports();
    log_open(tmp + "/log.txt");
    logline("a line %d", 1);
    logwarn("a warning %s", "here");
    st_check(!log_recent(5).empty(), "log_recent gives the last lines back");
    st_check(read_file(tmp + "/log.txt").find("a line 1") != string::npos, "the log file has the line");
    log_set_quiet(true);
    logline("not on the screen");
    log_set_quiet(false);
    log_open("");
  }
  // --- the process helpers ---------------------------------------------------------------------
  {
    // two commands, so that a shell which runs the last one of a -c string in its own process
    // (bash does) stays the /bin/sh the line below names
    pid_t p = spawn_program({"/bin/sh", "-c", "sleep 30; exit 0"}, tmp + "/sleep.log", tmp, true,
                            {}, true);
    st_check(p > 0, "spawn_program starts a program");
    bool child_up = false;                                      // a loaded box can take a moment to show the new process
    for (int i = 0; i < 50 && !child_up; i++) { child_up = pid_alive(p); if (!child_up) usleep(100000); }
    st_check(child_up, "the child is alive");
    // right after the fork the child is still a copy of this omnium, and in its exec the line reads empty
    string pcl;
    for (int i = 0; i < 100 && (pcl = proc_cmdline(p)).find("sleep 30") == string::npos; i++) usleep(20000);
    st_check(pcl == "/bin/sh -c sleep 30; exit 0", "proc_cmdline reads the command line");
    st_check(proc_rss_bytes(p) > 0, "proc_rss_bytes reads the memory use");
    st_check(wait_pid(p, 200) == -1, "wait_pid says still running");
    {
      vector<pid_t> kids = proc_children(getpid());
      st_check(std::find(kids.begin(), kids.end(), p) != kids.end(), "proc_children finds the child");
    }
    kill_group(p, SIGKILL);
    st_check(wait_pid(p, 5000) != -1, "and the killed child is reaped");
    st_check(!pid_alive(999999), "pid_alive says no to a pid that is not there");
    // a role child with the event pipe: hold sleeps until it is killed
    Child hc;
    st_check(spawn_role(hc, "hold", {tmp}, tmp + "/hold.log"), "spawn_role starts a hold child");
    bool hold_up = false;                                        // a loaded box can take a moment to show the new process
    for (int i = 0; i < 50 && !hold_up; i++) { hold_up = pid_alive(hc.pid); if (!hold_up) usleep(100000); }
    st_check(hold_up, "the hold child is alive");
    string line;
    st_check(!child_read(hc, line, 200), "the hold child sends no events");
    st_check(child_send(hc, "stop"), "a command goes down to the child");
    st_check(child_reap(hc, 5000) == 0, "and the hold child ends on it");
    child_close(hc);
    Child hc2;
    st_check(spawn_role(hc2, "hold", {tmp}, tmp + "/hold2.log"), "a second hold child starts");
    kill_group(hc2.pid, SIGKILL);
    child_reap(hc2, 5000);
    child_close(hc2);
    st_check(!child_send(hc2, "stop"), "sending to a child that is gone says no");
  }
  // --- the mail text ---------------------------------------------------------------------------
  {
    string err;
    st_check(!mail_send("not-an-address", "s", "b", &err, true), "mail_send refuses a string with no @");
    st_check(err.find("not an address") != string::npos, "and says why");
    int mrc = -1;
    string msg = call_output([](const Args&) { string e; return mail_send("nobody@example.invalid", "omnium selftest", "body\n", &e, true) ? 0 : 1; }, {}, &mrc);
    st_check(mrc == 0 && msg.find("To: <nobody@example.invalid>\r\n") != string::npos && msg.find("Subject: omnium selftest\r\n") != string::npos,
             "a dry-run mail prints the message it would send");
    vector<string> hosts;
    st_check(mail_mx("localhost", hosts, &err) && !hosts.empty(), "mail_mx falls back to the address when there is no MX");
    hosts.clear();
    st_check(!mail_mx("no-such-domain.invalid", hosts, &err), "mail_mx says no for a domain that does not resolve");
    // the real send, to a stand-in mail server on the loopback: nothing leaves the box
    {
      string mdir = st_tmp() + "/smtp";
      mkdirs(mdir);
      string portfile = mdir + "/port", savefile = mdir + "/message";
      Child sc;
      if (spawn_role(sc, "smtpstub", {portfile, savefile}, mdir + "/stub.log")) {
        string port;
        for (int i = 0; i < 200 && port.empty(); i++) { port = trim(read_file(portfile)); if (port.empty()) usleep(25000); }
        st_check(!port.empty(), "the stand-in mail server took a port");
        if (!port.empty()) {
          int save_port = g_cfg.smtp_port;
          g_cfg.smtp_port = (int)to_long(port, 25);
          string serr;
          bool sent = mail_send("omnium@localhost", "omnium selftest subject", "line one\n.\nline two\n", &serr, false);
          st_check(sent, "a mail is sent to the stand-in server [" + serr + "]");
          string got;
          for (int i = 0; i < 200 && got.empty(); i++) { got = read_file(savefile); if (got.empty()) usleep(25000); }
          st_check(got.find("Subject: omnium selftest subject") != string::npos, "the server got the subject");
          st_check(got.find("To: <omnium@localhost>") != string::npos, "and the address");
          st_check(got.find("line one") != string::npos && got.find("line two") != string::npos, "and both lines of the body");
          st_check(got.find("\n..\n") != string::npos, "a body line of one dot arrives dot-stuffed, so it does not end the message");
          st_check(got.find("Message-ID: <omnium.") != string::npos, "and a message id of its own");
          g_cfg.smtp_port = save_port;
        }
        child_reap(sc, 5000);
        kill_group(sc.pid, SIGKILL);
        child_reap(sc, 2000);
        child_close(sc);
      }
      remove_tree(mdir);
    }
    {                                                          // a port nothing is listening on
      int save_port = g_cfg.smtp_port;
      g_cfg.smtp_port = 9;
      string serr;
      st_check(!mail_send("omnium@localhost", "s", "b", &serr, false) && !serr.empty(), "a send to a closed port comes back with the reason");
      g_cfg.smtp_port = save_port;
    }
    st_check(quiet_call(cmd_mail, {"--mx", "localhost"}) == 0, "mail --mx prints the hosts");
    st_check(quiet_call(cmd_mail, {"--mx", "no-such-domain.invalid"}) != 0, "mail --mx says no for a bad domain");
    st_check(quiet_call(cmd_mail, {"--nonsense"}) == 2, "mail refuses an unknown option");
    write_file(tmp + "/body.txt", "a body from a file\n");
    st_check(quiet_call(cmd_mail, {"nobody@example.invalid", "--subject", "s", "--body-file", tmp + "/body.txt", "--dry-run"}) == 0, "mail takes a body file");
  }
  // --- the box check and the binary's own front door -------------------------------------------
  {
    string exe = self_exe();
    if (!exe.empty()) {
      st_check(run_capture({exe, "--version"}, 30).rc == 0, "the binary prints its version");
      st_check(run_capture({exe, "no_such_verb"}, 30).rc != 0, "an unknown verb is refused");
      st_check(run_capture({exe, "--role"}, 30).rc != 0, "--role with no name is refused");
      st_check(run_capture({exe, "--role", "no_such_role"}, 30).rc != 0, "an unknown role is refused");
      st_check(run_capture({exe, "help"}, 30).rc == 0, "help runs");
      // the embedded generators keep their own command line; the role passes it straight through
      CmdResult g = run_capture({exe, "--role", "generator", "--help"}, 60);
      st_check(!g.out.empty() || g.rc != 0, "the generator role answers");
      st_check(omnium_revgen_main != nullptr, "revgen is compiled in: the link kept its entry point");
      CmdResult rv = run_capture({exe, "--role", "revgen", "--help"}, 60);
      st_check(!rv.out.empty() || rv.rc != 0, "the revgen role answers");
      st_check(run_capture({exe, "--role", "trial"}, 30).rc != 0, "the trial role needs its arguments");
      st_check(run_capture({exe, "config"}, 30).rc == 0, "config runs");
    }
  }
  remove_tree(tmp);
}

// the build side that needs no compiler: the version file, the names, the cmake line, the argument
// checks. Cloning and compiling are left out; they need the network and half an hour.
static void st_build_plain() {
  string tmp = st_tmp() + "/build";
  // a source tree as far as omnium reads one
  string src = tmp + "/13.1";
  mkdirs(src + "/storage/rocksdb");
  mkdirs(src + "/support-files/rpm");
  write_file(src + "/VERSION", "MYSQL_VERSION_MAJOR=13\nMYSQL_VERSION_MINOR=1\nMYSQL_VERSION_PATCH=0\nMYSQL_VERSION_EXTRA=\n");
  SourceInfo si;
  string err;
  st_check(source_info(src, si, &err), "source_info reads a VERSION file");
  st_eq(si.version, "13.1.0", "the version comes out of the file");
  st_eq(si.series, "13.1", "the series is major.minor");
  st_check(si.vendor == Vendor::MariaDB && si.product == "mariadb", "13.x is a MariaDB tree");
  st_check(si.has_rocksdb, "the RocksDB directory is noticed");
  st_check(!si.es, "no enterprise spec file, so it is a Community tree");
  write_file(src + "/support-files/rpm/enterprise-server.spec", "\n");
  SourceInfo esi;
  st_check(source_info(src, esi, &err) && esi.es, "an enterprise spec file makes it an ES tree");
  // MySQL and Percona come out of the same file
  string msrc = tmp + "/8.0";
  mkdirs(msrc);
  write_file(msrc + "/MYSQL_VERSION", "MYSQL_VERSION_MAJOR=8\nMYSQL_VERSION_MINOR=0\nMYSQL_VERSION_PATCH=36\nMYSQL_VERSION_EXTRA=\n");
  SourceInfo msi;
  st_check(source_info(msrc, msi, &err) && msi.vendor == Vendor::MySQL, "8.0 with no extra is MySQL");
  write_file(msrc + "/MYSQL_VERSION", "MYSQL_VERSION_MAJOR=8\nMYSQL_VERSION_MINOR=0\nMYSQL_VERSION_PATCH=36\nMYSQL_VERSION_EXTRA=-28\n");
  SourceInfo psi;
  st_check(source_info(msrc, psi, &err) && psi.vendor == Vendor::Percona, "8.0 with an extra is Percona");
  SourceInfo bad;
  st_check(!source_info(tmp, bad, &err), "a directory with no VERSION file is refused");
  st_check(!err.empty(), "and it says what is missing");
  write_file(tmp + "/VERSION", "NOT_A_VERSION=1\n");
  st_check(!source_info(tmp, bad, &err), "a VERSION file with no major number is refused");
  // the basedir names
  st_eq(build_name(si, "opt", "", "180826"), "MD180826-mariadb-13.1.0-linux-x86_64-opt", "an optimised Community name");
  st_eq(build_name(si, "dbg", "", "180826"), "MD180826-mariadb-13.1.0-linux-x86_64-dbg", "a debug name");
  st_eq(build_name(si, "ubasan-dbg", "", "180826"), "UBASAN_MD180826-mariadb-13.1.0-linux-x86_64-dbg", "a UBSAN+ASAN name");
  st_eq(build_name(si, "msan-opt", "", "180826"), "MSAN_MD180826-mariadb-13.1.0-linux-x86_64-opt", "an MSAN name");
  st_eq(build_name(si, "opt", "MDEV-12345", "180826"), "MDEV-12345_MD180826-mariadb-13.1.0-linux-x86_64-opt", "a feature-branch name");
  st_eq(build_name(esi, "opt", "", "180826"), "EMD180826-mariadb-13.1.0-linux-x86_64-opt", "an Enterprise name");
  // a feature branch becomes the tag, a version branch does not
  st_eq(tag_from_branch("MDEV-12345-fix"), "MDEV-12345-fix", "a feature branch is a tag");
  st_eq(tag_from_branch("bb/13.1-mdev"), "bb-13.1-mdev", "slashes become hyphens");
  st_eq(tag_from_branch("13.1"), "", "a version branch is no tag");
  st_eq(tag_from_branch("main"), "", "main is no tag");
  st_eq(tag_from_branch("12.3-enterprise"), "", "an enterprise version branch is no tag");
  // the flavour names
  for (const char* f : {"opt", "dbg", "ubasan-opt", "ubasan-dbg", "msan-opt", "msan-dbg"}) st_check(flavour_valid(f), string("flavour ") + f + " is known");
  st_check(!flavour_valid("asan"), "an unknown flavour is refused");
  // the cmake line
  for (const char* f : {"opt", "dbg", "ubasan-dbg", "msan-dbg"}) {
    vector<string> cm = cmake_command(si, f, tmp + "/prefix", &err);
    if (cm.empty()) { st_check(!err.empty(), string("cmake_command for ") + f + " says what is missing"); continue; }
    st_check(cm.size() > 5, string("the cmake line for ") + f + " has its options");
    bool has_prefix = false, has_type = false;
    for (auto& o : cm) {
      if (o.find("CMAKE_INSTALL_PREFIX") != string::npos) has_prefix = true;
      if (o.find("CMAKE_BUILD_TYPE") != string::npos) has_type = true;
    }
    st_check(has_prefix && has_type, string("the cmake line for ") + f + " sets the prefix and the build type");
  }
  // the argument checks of the verb
  st_check(quiet_call(cmd_build, {}) == 2, "build with no arguments prints the usage");
  st_check(quiet_call(cmd_build, {"13.1", "opt", "extra", "words"}) == 2, "build refuses an argument it cannot place");
  st_check(quiet_call(cmd_build, {"--help"}) == 2 && quiet_call(cmd_build, {"--rebiuld", "13.1"}) == 2, "build refuses a flag it does not know, instead of cloning a branch of that name");
  st_check(quiet_call(cmd_build, {"13.1", "--jobs"}) == 2 && quiet_call(cmd_build, {"13.1", "--tag"}) == 2, "and says when a flag is missing its value");
  st_check(quiet_call(cmd_parity, {"--nogdb"}) == 2, "parity refuses a flag it does not know, instead of running the whole corpus with gdb");
  st_check(quiet_call(cmd_build, {tmp + "/no_such_tree"}) != 0, "build refuses a path that is not a source tree");
  if (kTakeFixes) st_check(!dir_exists(g_cfg.test_dir + st_tmp()), "and does not try it as a branch, which left its parent folders under TEST_DIR");
  st_check(quiet_call(cmd_build, {tmp}) != 0, "build refuses a directory with no VERSION file");
  // a flavour named twice is one build: the tree has no CMakeLists.txt, so the one build stops at cmake
  {
    CmdResult br = run_capture({self_exe(), "build", src, "dbg", "dbg"}, 300);
    size_t n = 0;
    for (auto& l : split_lines(br.out)) if (starts_with(l, "building ")) n++;
    st_check(n == 1, fmt("build dbg dbg starts one build (%zu)", n));
    remove_tree(src + "_dbg");
  }
  remove_tree(tmp);
}

// the builds file, the known-bug helpers and the filter lists, all on temporary copies
static void st_registry_plain() {
  string tmp = st_tmp() + "/reg";
  mkdirs(tmp);
  // the builds file: parse, order, format, save, read back
  Registry r;
  st_check(!registry_load(r, tmp + "/no_such_file"), "registry_load says no when the file is missing");
  Basedir b;
  st_check(basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", b), "a basedir name parses");
  b.name = "MD180826-mariadb-13.1.0-linux-x86_64-opt";
  b.path = "/test/" + b.name;
  BuildEntry e;
  st_check(entry_from_basedir(b, e), "a basedir name parses into a builds entry");
  st_eq(e.vendor, "CS", "MD is Community");
  st_eq(e.series, "13.1", "the series comes from the version");
  st_eq(e.type, "opt", "the type comes off the name");
  st_eq(e.date, "180826", "the date comes off the name");
  Basedir b2;
  st_check(basedir_parse_name("UBASAN_EMD010926-mariadb-12.3.2-1-linux-x86_64-dbg", b2), "a UBASAN Enterprise name parses");
  b2.name = "UBASAN_EMD010926-mariadb-12.3.2-1-linux-x86_64-dbg";
  b2.path = "/test/" + b2.name;
  BuildEntry e2;
  st_check(entry_from_basedir(b2, e2), "the UBASAN Enterprise name becomes an entry");
  st_eq(e2.vendor, "ES", "EMD is Enterprise");
  st_eq(e2.flavour, "UBASAN", "the sanitizer prefix is the flavour");
  st_eq(e2.type, "dbg", "and it is a debug build");
  Basedir b3;
  basedir_parse_name("not-a-basedir-name", b3);
  b3.name = "not-a-basedir-name";
  b3.path = "/test/" + b3.name;
  BuildEntry e3;
  st_check(!entry_from_basedir(b3, e3), "a name that is not a basedir is refused");
  BuildEntry older = e;
  older.date = "010826";
  st_check(entry_newer(e, older) && !entry_newer(older, e), "the newer build of a key wins");
  st_eq(e.key(), "CS 13.1 plain opt", "the key is vendor, series, flavour, type");
  Registry r2;
  e.test = e.report = true;
  e2.test = false;
  e2.report = true;
  r2.entries = {e, e2};
  st_check(!registry_format(r2).empty(), "the builds file is formatted");
  st_check(registry_save(r2, tmp + "/builds"), "the builds file is written");
  Registry r3;
  st_check(registry_load(r3, tmp + "/builds"), "and read back");
  st_check(r3.entries.size() == 2, "with both builds");
  st_check(registry_find(r3, e.name) != nullptr, "a build is found by name");
  st_check(registry_find(r3, "no such build") == nullptr, "an unknown name gives nothing");
  st_check(registry_names(r3, false).size() == 2, "the report set holds both builds");
  st_check(registry_names(r3, true).size() == 1, "the test set holds only the build marked test");
  registry_sync(r3, {b}, false, false);
  st_check(!r3.entries.empty(), "a sync against one scanned build keeps the file usable");
  // version_cmp
  st_check(version_cmp("13.1.0", "12.3.2") > 0, "13.1.0 is newer than 12.3.2");
  st_check(version_cmp("10.11.19", "11.4.13") < 0, "10.11.19 is older than 11.4.13");
  st_check(version_cmp("13.1.0", "13.1.0") == 0, "the same version is the same");
  // the known-bug helpers on a temporary list
  string kbf = tmp + "/known_bugs.strings";
  write_file(kbf, "# a copy for the selftest\n##### CURRENT BUGS (Search key: Mac) #####\nSIGSEGV|a|b|c|d           ## MDEV-100\n\n##### FIXED BUGS #####\n");
  st_check(kb_add_to(kbf, "SIGABRT|q|w|e|r", "MDEV-101", nullptr), "a UID is added to the list");
  st_check(!kb_add_to(kbf, "SIGSEGV|a|b|c|d", "MDEV-100", nullptr), "a UID already in the list is refused");
  st_check(read_file(kbf).find("MDEV-101") != string::npos, "the new line is in the file");
  auto keys = kb_keys(split_lines(read_file(kbf)));
  st_check(!keys.empty(), "kb_keys reads the ticket keys");
  st_check(kb_fixed_in(kbf, "MDEV-100") == 1, "a bug can be marked as fixed");
  st_check(read_file(kbf).find("## Fixed") != string::npos, "the fixed marker is written");
  st_check(kb_uid_is_san("ASAN|heap-use-after-free|a|b|c"), "a sanitizer UID goes in the SAN list");
  st_check(!kb_uid_is_san("SIGSEGV|a|b|c|d"), "a plain UID does not");
  st_check(!kb_format_line("SIGSEGV|a|b|c|d", "MDEV-1").empty(), "a list line is formatted");
  st_eq(bug_key_normalize("12345"), "MDEV-12345", "a bare number is an MDEV key");
  st_eq(bug_key_normalize("ment-999"), "MENT-999", "a lower-case key is normalised");
  st_eq(bug_key_normalize("MDEV-7"), "MDEV-7", "a key already in shape is left alone");
  st_check(!uri_escape("a b|c").empty() && uri_escape("a b|c").find(' ') == string::npos, "uri_escape takes the spaces out");
  // BUGS/ and the filter lists
  string save_bugs = g_paths.bugs_dir;
  g_paths.bugs_dir = tmp + "/BUGS";
  mkdirs(g_paths.bugs_dir);
  string berr;
  st_check(bugs_write("MDEV-12345", "--sql_mode=", "SELECT 1;\n", &berr), "a testcase is written to BUGS/");
  st_check(file_exists(bugs_file_for("MDEV-12345")), "and the file is there");
  st_check(read_file(bugs_file_for("MDEV-12345")).find("--sql_mode=") != string::npos, "the options line is kept");
  g_paths.bugs_dir = save_bugs;
  string rf = tmp + "/REGEX_LIST";
  write_file(rf, "alpha|beta\n");
  auto alts = regex_list_read(rf);
  st_check(alts.size() == 2, "a one-line regex list reads as its alternatives");
  st_check(regex_list_add(rf, "gamma"), "a new alternative is added");
  st_check(!regex_list_add(rf, "gamma"), "and is not added twice");
  st_check(regex_list_read(rf).size() == 3, "the list has three now");
  remove_tree(tmp);
}

// the screen view, the settings and the by-hand verbs, on a made-up run directory
static void st_view_and_config() {
  string tmp = st_tmp() + "/view";
  // a run directory with the files the view reads
  string save_data = g_cfg.data_dir;
  g_cfg.data_dir = tmp + "/data";
  string wd = g_cfg.data_dir + "/O123456";
  mkdirs(wd + "/log");
  mkdirs(wd + "/1");
  write_file(wd + "/O123456.log", "[05:00:00] run started\n[05:00:10] trial 1 saved\n");
  write_file(wd + "/status.txt", "state=running\nstarted=" + std::to_string(now_s() - 300) +
             "\nlaunched=12\nsaved=2\nknown=7\nperformed=1200\nram_pct=41\nslot_01=trial 12 13.1-opt 20s\njobs=reduce 1\nqueue=1\n"
             "build_13.1-opt=12 trials, 2 saved\n");
  write_file(wd + "/omnium.ledger",
             fmt("%lld run created 2026-09-05 05:00:00 seed 1\n%lld trial 1 13.1-opt default saved-new SIGSEGV|a|b|c|d\n"
                 "%lld trial 2 13.1-opt engines known SIGABRT|q|w|e|r MDEV-100\n%lld bug 1 reported O123456_bug1\n",
                 (long long)now_s() - 300, (long long)now_s() - 200, (long long)now_s() - 100, (long long)now_s() - 50));
  write_file(wd + "/1/MYBUG", "SIGSEGV|a|b|c|d\n");
  st_check(quiet_call(cmd_tui, {wd, "--frame"}) == 0, "the view draws one frame of a run");
  st_check(quiet_call(cmd_tui, {wd, "--nonsense"}) == 2, "the view refuses an unknown option");
  // the view on a real terminal: a pty, every key it takes, then q. Nothing else reaches the
  // interactive loop, so it is driven here the way a person drives it.
  {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    bool pty = m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0;
    const char* sn = pty ? ptsname(m) : nullptr;
    if (pty && sn) {
      string sname = sn;
      pid_t kid = fork();
      if (kid == 0) {
        setsid();
        int sfd = open(sname.c_str(), O_RDWR);
        if (sfd >= 0) { ioctl(sfd, TIOCSCTTY, 0); dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2); if (sfd > 2) close(sfd); }
        close(m);
        execl("/proc/self/exe", "omnium", "tui", wd.c_str(), (char*)nullptr);
        _exit(127);
      }
      st_check(kid > 0, "the view starts on a terminal");
      if (kid > 0) {
        usleep(400000);                                     // the first frame
        const char* keys = "?lL\x0c" "rpsS";                   // Ctrl+L redraws
        for (const char* k = keys; *k; k++) { ssize_t n = write(m, k, 1); (void)n; usleep(150000); }
        string seen;
        int fl = fcntl(m, F_GETFL, 0);
        fcntl(m, F_SETFL, fl | O_NONBLOCK);
        int64_t until = now_s() + 6;
        while (now_s() < until) { char buf[4096]; ssize_t n = read(m, buf, sizeof(buf)); if (n > 0) seen.append(buf, (size_t)n); else usleep(100000); }
        size_t mark = seen.size();                          // P, the old key, still resumes
        { ssize_t n = write(m, "P", 1); (void)n; }
        until = now_s() + 8;
        while (now_s() < until && seen.find("resume asked", mark) == string::npos) { char buf[4096]; ssize_t n = read(m, buf, sizeof(buf)); if (n > 0) seen.append(buf, (size_t)n); else usleep(100000); }
        bool p_resumes = seen.find("resume asked", mark) != string::npos;
        ssize_t n = write(m, "q", 1); (void)n;
        int st = -1, waited = 0;
        bool reaped = false;
        while (waited < 8000 && !reaped) {
          if (waitpid(kid, &st, WNOHANG) == kid) { reaped = true; break; }
          char buf[4096];
          while (read(m, buf, sizeof(buf)) > 0) {}
          usleep(100000);
          waited += 100;
        }
        if (!reaped) { kill(kid, SIGKILL); waitpid(kid, &st, 0); }
        st_check(reaped && WIFEXITED(st) && WEXITSTATUS(st) == 0, "the view leaves on q");
        st_check(seen.find(basename_of(wd)) != string::npos, "the view drew the run it was given");
        st_check(seen.find("pause asked") != string::npos, "the view answers p with a message");
        st_check(seen.find("resume asked") != string::npos, "the view answers r with a message");
        st_check(p_resumes, "the view still takes P, the old key, for resume");
        st_check(seen.find("stop now asked") != string::npos, "the view answers S with a message");
        st_check(seen.find("q quit") != string::npos, "the view answers ? with the key list");
        st_check(file_exists(wd + "/omnium.ctl"), "the keys wrote the run's command file");
      }
    }
    if (m >= 0) close(m);
  }
  st_check(quiet_call(cmd_status, {wd}) == 0, "status reads the same run");
  st_check(quiet_call(cmd_adopt, {wd}) == 0, "adopt reads the same run");
  g_cfg.data_dir = save_data;
  // the settings file: written, read back, and laid out the way it is meant to be. The whole thing
  // runs in a child with its own HOME, so the real ~/.omnium.conf is never touched.
  {
    string h = tmp + "/home";
    mkdirs(h);
    CmdResult w = run_capture({self_exe(), "config", "CORE_DIR=data"}, 120, "", {"HOME=" + h});
    st_check(w.rc == 0 && w.out.find("saved") != string::npos, "config KEY=VALUE saves the setting [" + trim(w.out) + "]");
    string written = read_file(h + "/.omnium.conf");
    st_check(written.find("CORE_DIR=data") != string::npos, "and the file holds the new value");
    long lines = 0, aligned = 0;
    for (auto& l : split_lines(written)) {
      if (l.empty() || l[0] == '#') continue;
      size_t hash = l.find('#');
      if (hash == string::npos) continue;
      lines++;
      // column 55, or three spaces after a setting too long to fit (the reader splits on those three)
      size_t setting_end = l.find_last_not_of(' ', hash - 1) + 1;
      if (hash == 55 || hash == setting_end + 3) aligned++;
    }
    st_check(lines > 10 && aligned == lines, fmt("every comment in it starts at column 55 (%ld of %ld)", aligned, lines));
    CmdResult r = run_capture({self_exe(), "config", "CORE_DIR"}, 120, "", {"HOME=" + h});
    st_check(trim(r.out) == "CORE_DIR=data", "config KEY reads it back [" + trim(r.out) + "]");
  }
  // the settings: every key reads back, and a bad value is refused
  {
    string dump = config_dump();
    st_check(dump.find("DATA_DIR=") != string::npos, "the settings print with their keys");
    for (auto& line : split_lines(dump)) {
      size_t eq = line.find('=');
      if (eq == string::npos || line.empty() || line[0] == '#') continue;
      string key = line.substr(0, eq), value;
      st_check(config_get(key, value), "the setting " + key + " reads back");
    }
    string save_core = g_cfg.core_dir;
    st_check(config_set("CORE_DIR", "data"), "CORE_DIR takes data");
    st_check(config_set("CORE_DIR", "shm"), "CORE_DIR takes shm");
    st_check(config_set("CORE_DIR", "auto"), "CORE_DIR takes auto");
    st_check(!config_set("CORE_DIR", "somewhere else"), "CORE_DIR refuses anything else");
    g_cfg.core_dir = save_core;
    int save_step = g_cfg.shm_stepdown_pct, save_max = g_cfg.core_max_gb;
    st_check(config_set("SHM_STEPDOWN_PCT", "60") && g_cfg.shm_stepdown_pct == 60, "SHM_STEPDOWN_PCT reads back");
    st_check(config_set("CORE_MAX_GB", "12") && g_cfg.core_max_gb == 12, "CORE_MAX_GB reads back");
    // where a trial's datadir goes, and with it the core the kernel writes there
    g_cfg.shm_stepdown_pct = 75;
    g_cfg.core_dir = "shm";
    st_check(!core_on_data(0) && !core_on_data(99), "CORE_DIR=shm always runs the trial on the tmpfs");
    g_cfg.core_dir = "data";
    st_check(core_on_data(0) && core_on_data(99), "CORE_DIR=data always runs it on the data disk");
    g_cfg.core_dir = "auto";
    st_check(!core_on_data(74), "CORE_DIR=auto keeps the tmpfs while it is under the step-down mark");
    st_check(core_on_data(75) && core_on_data(90), "and steps down to the data disk at the mark");
    g_cfg.core_dir = save_core;
    g_cfg.shm_stepdown_pct = save_step;
    g_cfg.core_max_gb = save_max;
    st_check(!config_set("NO_SUCH_KEY", "1"), "an unknown key is refused");
    {                                                          // a value the file cannot carry back is refused, not cut at the write
      string keep;
      config_get("INFILE", keep);
      st_check(!config_set("INFILE", "a\nb") && !config_set("INFILE", "a   #b"), "a setting value with a newline or a comment marker is refused");
      string after;
      config_get("INFILE", after);
      st_eq(after, keep, "and the setting keeps the value it had");
      st_check(config_refusal("INFILE", "a   #b").find("one line") != string::npos, "and says why [" + config_refusal("INFILE", "a   #b") + "]");
    }
    string v;
    st_check(!config_get("NO_SUCH_KEY", v), "and cannot be read");
    int save_ram = g_cfg.ram_cap_pct;
    st_check(config_set("ram_cap_pct", "70"), "a key is case-insensitive");
    st_check(g_cfg.ram_cap_pct == 70, "and the value lands");
    g_cfg.ram_cap_pct = save_ram;
    // the framework shell helpers, read from the mariadb-qa checkout at QA_DIR
    for (const char* sc : {"new_text_string.sh", "san_text_string.sh", "fallback_text_string.sh", "error_log_scan.sh", "stack.sh", "reducercpp/stages.tbl"})
      st_check(file_exists(script_path(sc)), string("the script ") + sc + " is there");
    st_check(script_path("stack.sh").find(g_paths.qa) == 0, "a script path points into QA_DIR, so there is no second copy to drift");
    st_check(script_path("no_such_script.sh").find("no_such_script.sh") != string::npos, "an unknown script still gives a path back");
    ensure_dirs();
    st_check(dir_exists(g_paths.human_queue) && dir_exists(g_paths.ai_queue), "ensure_dirs makes the queues");
  }
  // the by-hand verbs: what they say when the arguments are wrong
  st_check(quiet_call(cmd_fresh, {"--nonsense"}) != 0, "fresh refuses an unknown option");
  st_check(quiet_call(cmd_replay, {}) == 2, "replay without a file");
  st_check(quiet_call(cmd_replay, {tmp + "/no_such.sql"}) != 0, "replay of a file that is not there");
  st_check(quiet_call(cmd_cl, {"--nonsense"}) != 0, "cl refuses an unknown option");
  st_check(quiet_call(cmd_sql, {"--nonsense"}) != 0, "sql refuses an unknown option");
  st_check(quiet_call(cmd_kb, {"add"}) == 2, "kb add without a UID says what it takes");
  st_check(quiet_call(cmd_kba, {"add"}) == 2, "kba add without a UID says what it takes");
  st_check(quiet_call(cmd_kb, {"fixed"}) == 2, "kb fixed without a key says what it takes");
  st_check(quiet_call(cmd_kb, {"nonsense"}) != 0, "kb refuses an unknown subcommand");
  st_check(quiet_call(cmd_builds, {"--nonsense"}) != 0, "builds refuses an unknown option");
  st_check(quiet_call(cmd_run, {"--nonsense"}) != 0, "run refuses an unknown option");
  st_check(quiet_call(cmd_run, {"--help"}) == 0, "run --help prints the usage");
  st_check(quiet_call(cmd_reduce, {"--help"}) == 2, "reduce takes --help for a flag it does not know; omnium help reduce has the usage");
  st_check(quiet_call(cmd_inbox, {"--nonsense"}) == 2, "inbox refuses an unknown option");
  remove_tree(tmp);
}

// the ELF test the chain puts every candidate path through, and what it costs on a real binary
static void st_binary_for_dir_elf_test() {
  string tmp = st_tmp() + "/elf";
  string trial = tmp + "/trial", bin = tmp + "/build/bin";
  mkdirs(trial);
  mkdirs(bin);
  write_file(trial + "/BASEDIR", tmp + "/build\n");
  write_file(bin + "/mariadbd", "not a binary\n");
  st_check(binary_for_dir(trial) != bin + "/mariadbd", "binary_for_dir passes over a file with no ELF header");
  remove_tree(bin + "/mariadbd");
  mkdirs(bin + "/mysqld");
  st_check(binary_for_dir(trial) != bin + "/mysqld", "and over a directory carrying the name of a binary");
  remove_tree(bin + "/mysqld");
  write_file(bin + "/mariadbd", string("\x7f" "ELF", 4));
  st_eq(binary_for_dir(trial), bin + "/mariadbd", "and takes the one with the header");
  // a server binary is around 100 MB; this stand-in is that size but costs no disk
  st_check(truncate((bin + "/mariadbd").c_str(), 256LL << 20) == 0, "the stand-in binary is grown to 256 MB");
  struct rusage r0, r1;
  getrusage(RUSAGE_SELF, &r0);
  double t0 = now_ms();
  string got = binary_for_dir(trial);
  getrusage(RUSAGE_SELF, &r1);
  st_eq(got, bin + "/mariadbd", "the big one is found by its header too");
  st_check(r1.ru_maxrss - r0.ru_maxrss < 4096, "and finding it does not cost the memory of the whole file");
  st_check(now_ms() - t0 < 500, "and it does not cost the time of reading it");
  remove_tree(tmp);
}

// the rest of the detection chain: the capped log, the gdb-trace form, the sanitizer block that is
// already known, the stack block, and what each identification verb does when it is pointed wrong
static void st_detect_more() {
  string tmp = st_tmp() + "/det";
  mkdirs(tmp + "/log");
  string log = tmp + "/log/master.err";
  // a log over the cap is read as its head and its tail
  {
    string big = tmp + "/big";
    mkdirs(big + "/log");
    string line = string(200, 'x') + "\n";
    string chunk;
    chunk.reserve(1 << 20);
    while (chunk.size() < (1u << 20)) chunk += line;
    string blog = big + "/log/master.err";
    FILE* f = fopen(blog.c_str(), "w");
    if (f) {
      fputs("2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n", f);
      for (int i = 0; i < 21; i++) fwrite(chunk.data(), 1, chunk.size(), f);
      fputs("mariadbd: /test/13.1/sql/item_func.cc:1287: virtual void f(): Assertion `x >= 0' failed.\n", f);
      fclose(f);
    }
    st_check(file_size(blog) > 20 * 1024 * 1024, "the big log is over the cap");
    auto capped = capped_logs({blog});
    st_check(!capped.empty() && capped[0] != blog, "a log over the cap is read from a copy");
    st_check(file_size(capped[0]) < file_size(blog), "and the copy is smaller than the log");
    st_check(starts_with(capped[0], "/tmp/omnium_cap_"), "the copy sits in this process's own directory, which goes when it ends");
    remove_tree(big);
  }
  // a pasted gdb backtrace, as a file
  {
    string gt = tmp + "/gdb.txt";
    write_file(gt, "#0  0x00005 in kill () at kill.c:10\n#1  0x00006 in my_write_core (sig=6) at /test/13.1/sql/signal_handler.cc:100\n"
                   "#2  0x00007 in handle_fatal_signal (sig=6) at /test/13.1/sql/signal_handler.cc:200\n"
                   "#3  0x00008 in Item_func_case::val_int (this=0x1) at /test/13.1/sql/item_cmpfunc.cc:2544\n"
                   "#4  0x00009 in sub_select (join=0x2) at /test/13.1/sql/sql_select.cc:22000\n");
    string u = uid_raw_gdb(gt);
    st_check(starts_with(u, "RAW_GDB_UID|"), "a pasted gdb trace gives a RAW_GDB_UID");
    st_check(quiet_call(cmd_t, {gt}) == 0, "t on a pasted gdb trace");
  }
  // a sanitizer log with two blocks: the first is already known, so it is dropped
  {
    string sd = tmp + "/san";
    mkdirs(sd + "/log");
    string first =
      "==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x606000000040 at pc 0x4a1b2c bp 0x7ffd0 sp 0x7ffc8\n"
      "READ of size 8 at 0x606000000040 thread T3\n"
      "    #0 0x4a1b2c in Item_func_case::val_int() /test/13.1/sql/item_cmpfunc.cc:2544:10\n"
      "    #1 0x4a2000 in Item::val_int_result() /test/13.1/sql/item.h:1200:12\n"
      "    #2 0x4a3000 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:22000:5\n"
      "    #3 0x4a4000 in do_select(JOIN*, Procedure*) /test/13.1/sql/sql_select.cc:24230:14\n"
      "SUMMARY: AddressSanitizer: heap-use-after-free /test/13.1/sql/item_cmpfunc.cc:2544:10 in Item_func_case::val_int()\n";
    string second =
      "==1==ERROR: AddressSanitizer: use-after-poison on address 0x76041a1b7f93 at pc 0x6b3307126fdb bp 0x6b33150ff800 sp 0x6b33150ff7f8\n"
      "READ of size 1 at 0x76041a1b7f93 thread T15\n"
      "    #0 0x6b3307126fda in TYPVAL<char*>::SetValue_char(char const*, int) /test/13.1/storage/connect/value.cpp:1381:8\n"
      "    #1 0x6b3306f3072e in CntIndexRead(_global*, TDB*, OPVAL, st_key_range const*, bool) /test/13.1/storage/connect/connect.cc:1000:5\n"
      "    #2 0x5e46f560c494 in join_read_const(st_join_table*) /test/13.1/sql/sql_select.cc:25311:9\n"
      "    #3 0x5e46f5464b59 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:24716:12\n"
      "SUMMARY: AddressSanitizer: use-after-poison /test/13.1/storage/connect/value.cpp:1381:8 in TYPVAL<char*>::SetValue_char(char const*, int)\n";
    write_file(sd + "/log/master.err", first + second);
    UidResult r;
    UidOptions o;
    o.wait_core = false;
    st_check(uid_for_dir(sd, r, o) && !r.uid.empty(), "the first block gives the UID");
    string save_san = g_paths.known_bugs_san;
    g_paths.known_bugs_san = tmp + "/known_bugs.strings.SAN";
    write_file(g_paths.known_bugs_san, "# a copy for the selftest\n##### CURRENT BUGS (Search key: Mac) #####\n" + r.uid + "           ## MDEV-100\n\n##### FIXED BUGS #####\n");
    int dropped = san_drop_known(sd, "log/master.err");
    st_check(dropped >= 1, "the known sanitizer block is dropped");
    st_check(file_exists(sd + "/log/master.err.pre_known_san_removal"), "the log is kept as it was first");
    UidResult r2;
    st_check(uid_for_dir(sd, r2, o) && r2.uid != r.uid, "what is left is the next issue");
    g_paths.known_bugs_san = save_san;
    // the stack block of a sanitizer trial, for a report
    string serr;
    string stack = stack_text(sd, "13.1.0 MariaDB (dbg)", &serr);
    st_check(stack.find("{noformat") != string::npos, "the stack block comes back for the report");
  }
  // the server binary the chain would use
  {
    write_file(log, "2026-09-05  5:00:00 0 [Note] /test/13.1/bin/mariadbd (server 13.1.0-MariaDB-debug) starting as process 1 ...\n"
                    "mariadbd: /test/13.1/sql/item_func.cc:1287: virtual void f(): Assertion `x >= 0' failed.\n");
    string bin = binary_for_dir(tmp);
    st_check(bin.empty() || is_executable(bin), "binary_for_dir with no BASEDIR file gives a server binary or nothing");
    string fbd = tmp + "/fake_basedir";
    mkdirs(fbd + "/bin");
    copy_file("/bin/true", fbd + "/bin/mariadbd");
    write_file(tmp + "/BASEDIR", fbd + "\n");
    // the chain reads a core with gdb, so it takes an ELF binary; a Windows server is a PE file
    // and writes no core
    if (kHostMsys2)
      st_skip("a BASEDIR file points the chain at that build (the chain takes ELF server "
              "binaries, and a Windows server is a PE file)");
    else
      st_eq(binary_for_dir(tmp), fbd + "/bin/mariadbd",
            "a BASEDIR file points the chain at that build");
    remove_tree(tmp + "/BASEDIR");
    st_binary_for_dir_elf_test();
  }
  // the assertion text and the no-core fallback
  {
    st_check(!assert_from_logs({log}).empty(), "the assertion text comes out of the log");
    st_check(assert_from_logs({tmp + "/no_such_log"}).empty(), "no log, no assertion text");
    string ferr;
    st_eq(uid_fallback(log, &ferr), "FALLBACK|x >= 0", "uid_fallback takes the assertion of an assert log");
    st_check(uid_other_strings({log}).find("ASSERT") != string::npos, "the typed strings end at the assertion");
  }
  // the verbs, pointed at nothing
  string scanlog = tmp + "/scan.err";
  write_file(scanlog, string(FX_HEAD) + "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n");
  st_check(quiet_call(cmd_els, {}) != 0, "els with no arguments says what it takes");
  st_check(quiet_call(cmd_els, {"errors"}) != 0, "els with no log says what it takes");
  {
    // an InnoDB assertion on the last line is the one the scan types ASSERT; --exclude-assert passes over it
    string alog = tmp + "/assert_last.err";
    write_file(alog, string(FX_HEAD) + "safe_mutex: Found wrong usage of mutex 'LOCK_open' and 'LOCK_status'\n"
                                       "mariadbd: /test/13.1/storage/innobase/btr/btr0cur.cc:1000: void f(): Assertion `x >= 0' failed.\n");
    int erc = -1;
    st_check(trim(call_output(cmd_els, {"top", alog}, &erc)) == "ASSERT|storage/innobase/btr/btr0cur.cc|Assertion `x >= 0' failed" && erc == 0,
             "els top takes the assertion first");
    st_check(trim(call_output(cmd_els, {"top", alog, "--exclude-assert"}, &erc)) == "MUTEX_ERROR|safe_mutex: Found wrong usage of mutex X and Y" && erc == 0,
             "els can leave the assert out");
  }
  st_check(quiet_call(cmd_els, {"nonsense", scanlog}) == 2, "els refuses an unknown mode");
  {
    int erc = -1;
    st_check(trim(call_output(cmd_els, {"clean", scanlog}, &erc)) == "safe_mutex: Found wrong usage of mutex .LOCK_open. and .LOCK_status" && erc == 0,
             "els clean gives the line with its quotes as dots");
    st_check(trim(call_output(cmd_els, {"top", scanlog}, &erc)) == "MUTEX_ERROR|safe_mutex: Found wrong usage of mutex X and Y" && erc == 0, "els top types it");
  }
  st_check(quiet_call(cmd_sts, {tmp + "/no_such_dir"}) == 1, "sts on a path that is not there");
  st_check(quiet_call(cmd_sts, {tmp + "/log"}) == 1, "sts on a directory with no master.err");
  st_check(quiet_call(cmd_fts, {tmp + "/no_such_log"}) != 0, "fts on a log that is not there");
  st_check(quiet_call(cmd_t, {tmp + "/no_such_dir"}) == 1, "t on a directory that is not there");
  st_check(quiet_call(cmd_tt, {tmp + "/no_such_dir"}) == 1, "tt on a directory that is not there");
  remove_tree(tmp);
}

// the reduction plan in each of its shapes, and the report on a sanitizer trial and on an
// enterprise build. Nothing runs a reducer here: --plan writes the settings and stops.
static void st_reduce_and_report() {
  string tmp = st_tmp() + "/rr";
  string uid = "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y";
  string td = fixture_trial(tmp, 1, fixture_errlog(), uid);
  string wd = dirname_of(td);
  // the plan needs a real build to point the reducer at: the newest one on the box
  string bpath;
  for (auto& x : basedirs_scan(g_cfg.test_dir)) if (!x.bin.empty()) { bpath = x.path; break; }
  st_check(quiet_call(cmd_reduce, {wd, "1", "--plan"}) != 0, "reduce says no when the trial names no build");
  if (!bpath.empty()) {
    for (const char* v : {"quick", "onethd", "onethd-rnd"})
      st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", bpath, "--variant", v}) == 0, string("reduce --plan, variant ") + v);
    st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", bpath, "--mode", "4"}) == 0, "reduce --plan with a mode of its own");
    st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", bpath, "--text", "Assertion `arg2_int >= 0' failed"}) == 0, "reduce --plan with a text to look for");
    st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", bpath, "--skipv", "--sporadic", "--repeats", "3", "--stage1-lines", "500"}) == 0, "reduce --plan with the other flags");
    string conf0 = read_file(td + "/reduce.conf");
    st_check(!conf0.empty() && conf0.find("MODE=") != string::npos && conf0.find("TEXT=") != string::npos, "the plan writes the reducer settings, with the mode and the text");
    st_check(conf0.find("WORKDIR_LOCATION=1\n") != string::npos, "a trial without O_DIRECT is reduced on the tmpfs");
    {                                                          // O_DIRECT fails on tmpfs, so that trial is reduced on the disk under the trial
      string saved = read_file(td + "/MYEXTRA");
      write_file(td + "/MYEXTRA", "--sql_mode= --innodb_flush_method=O_DIRECT\n");
      st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", bpath}) == 0, "reduce --plan with O_DIRECT in the options");
      string conf1 = read_file(td + "/reduce.conf");
      st_check(conf1.find("WORKDIR_LOCATION=3\n") != string::npos && conf1.find("WORKDIR_M3_DIRECTORY=" + mount_point_of(td) + "\n") != string::npos, "an O_DIRECT trial is reduced on the disk the trial is on");
      write_file(td + "/MYEXTRA", saved);
    }
    st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--basedir", tmp + "/no_such_build"}) != 0, "reduce says no when the build is not there");
    // a pquery-run workdir names its build in the run log only
    write_file(wd + "/pquery-run.log", "[13:40:02] [0] Workdir: /data/999999 | Rundir: /dev/shm/999999 | Basedir: " + bpath + " | MDG Mode: TRUE\n");
    st_check(quiet_call(cmd_reduce, {wd, "1", "--plan"}) == 0, "reduce --plan takes the build from the run log");
    unlink((wd + "/pquery-run.log").c_str());
    // a trial with nothing in MYBUG, and a trial whose MYBUG says nothing was found: in both the
    // plan falls back to what the error-log scan says, and it is that text the reducer looks for
    {
      string ntd = fixture_trial(tmp + "/nomybug", 5, fixture_errlog(), "");
      fs::remove(ntd + "/MYBUG");
      write_file(ntd + "/log/master.err", string(FX_HEAD) + "2026-09-05  5:00:01 0 [ERROR] InnoDB: Table test/t1 index a is corrupt\n");
      st_check(quiet_call(cmd_reduce, {dirname_of(ntd), "5", "--plan", "--basedir", bpath}) == 0, "reduce --plan on a trial with no UniqueID of its own");
      string conf = read_file(ntd + "/reduce.conf");
      st_check(conf.find("Table test/t1 index a is corrupt") != string::npos, "the plan takes the text from the error-log scan");
      st_check(file_exists(ntd + "/MYBUG"), "and writes the UniqueID it found into the trial");

      string atd = fixture_trial(tmp + "/notfound", 6, fixture_errlog(), "");
      write_file(atd + "/MYBUG", "Assert: no core file found in */*core*, and fallback_text_string.sh returned an empty output for all logs\n");
      write_file(atd + "/log/master.err", string(FX_HEAD) + "2026-09-05  5:00:01 0 [ERROR] InnoDB: Table test/t2 index b is corrupt\n");
      st_check(quiet_call(cmd_reduce, {dirname_of(atd), "6", "--plan", "--basedir", bpath}) == 0, "reduce --plan on a trial whose MYBUG says nothing was found");
      st_check(read_file(atd + "/MYBUG").find("Table test/t2 index b is corrupt") != string::npos, "MYBUG is replaced by what the scan found");
      st_check(read_file(atd + "/reduce.conf").find("Table test/t2 index b is corrupt") != string::npos, "and that is what the reducer looks for");
    }
  }
  st_check(quiet_call(cmd_reduce, {wd, "1", "--plan", "--variant", "nonsense"}) == 2, "reduce refuses a variant it does not know");
  st_check(quiet_call(cmd_reduce, {wd, "1", "--nonsense"}) == 2, "reduce refuses an unknown flag");
  st_check(quiet_call(cmd_reduce, {wd, "not_a_number"}) == 2, "reduce wants a trial number");
  st_check(quiet_call(cmd_reduce, {wd, "1", "2", "3"}) == 2, "reduce takes at most a workdir and a trial");
  st_check(quiet_call(cmd_reduce, {wd, "9999", "--plan"}) != 0, "reduce says no for a trial that is not there");
  // what the reducer is told to look for: a crash with no text to match, a sanitizer trial, and a
  // binlog marker each pick a different mode
  if (!bpath.empty()) {
    struct Case { const char* name; const char* uid; const char* log; int mode; };
    const Case cases[] = {
      {"nocore", "Assert: no core file found in */*core*", "2026-09-05  5:00:00 0 [Note] starting\n", 4},
      {"san", "ASAN|heap-use-after-free|a|b|c|d", nullptr, 3},
      {"binlog", "BINLOG_RECOVERY_ERROR|ERROR N (X) at line N in file: X", "2026-09-05  5:00:00 0 [Note] starting\n", 11},
    };
    for (auto& c : cases) {
      string ctd = fixture_trial(tmp + "/mode_" + c.name, 4, c.log ? string(c.log) : fixture_san_log(), c.uid);
      if (string(c.name) == "binlog") write_file(ctd + "/BINLOG_RECOVERY_ERROR", "ERROR 1594 (HY000) at line 12 in file: 'x.sql': Relay log read failure\n");
      st_check(quiet_call(cmd_reduce, {dirname_of(ctd), "4", "--plan", "--basedir", bpath}) == 0, string("reduce --plan on a ") + c.name + " trial");
      string conf = read_file(ctd + "/reduce.conf");
      st_check(conf.find("MODE=" + std::to_string(c.mode)) != string::npos, string("a ") + c.name + " trial reduces in mode " + std::to_string(c.mode));
    }
  }
  // the plans the reducer refuses, and the ones where the text comes from somewhere other than MYBUG
  if (!bpath.empty()) {
    // a Valgrind trial and a query-correctness trial are both out of scope
    string vtd = fixture_trial(tmp + "/val", 8, fixture_errlog(), uid);
    write_file(vtd + "/VALGRIND", "");
    st_check(quiet_call(cmd_reduce, {dirname_of(vtd), "8", "--plan", "--basedir", bpath}) != 0, "reduce says no to a Valgrind trial");
    string qtd = fixture_trial(tmp + "/qc", 9, fixture_errlog(), uid);
    write_file(qtd + "/diff.result", "a difference\n");
    st_check(quiet_call(cmd_reduce, {dirname_of(qtd), "9", "--plan", "--basedir", bpath}) != 0, "reduce says no to a query-correctness trial");
    // the trace file under a name an older run used
    string ttd = fixture_trial(tmp + "/trace", 10, fixture_errlog(), uid);
    fs::rename(ttd + "/default.node.tld_thread-0.sql", ttd + "/other.node_thread-0.sql");
    st_check(quiet_call(cmd_reduce, {dirname_of(ttd), "10", "--plan", "--basedir", bpath}) == 0, "reduce finds a trace file under an older name");
    st_check(read_file(ttd + "/reduce.conf").find("other.node_thread-0.sql") != string::npos, "and reduces that file");
    // a sanitizer report with no core: the sanitizer line is what the reducer looks for
    string std2 = fixture_trial(tmp + "/sannocore", 11, fixture_san_log(), "Assert: no core file found in */*core*");
    st_check(quiet_call(cmd_reduce, {dirname_of(std2), "11", "--plan", "--basedir", bpath}) == 0, "reduce --plan on a sanitizer trial with no core");
    st_check(read_file(std2 + "/reduce.conf").find("ASAN|") != string::npos, "the sanitizer line becomes the text");
    // the workdir named without a path: under DATA_DIR, with and without the leading O
    {
      string save = g_cfg.data_dir;
      g_cfg.data_dir = tmp;
      st_check(quiet_call(cmd_reduce, {"O999999", "1", "--plan", "--basedir", bpath}) == 0, "reduce takes a workdir name on its own");
      st_check(quiet_call(cmd_reduce, {"999999", "1", "--plan", "--basedir", bpath}) == 0, "reduce takes the workdir number on its own");
      g_cfg.data_dir = save;
    }
    // a known bug, so the error-log line becomes the text the reducer matches
    {
      string save_kb = g_paths.known_bugs, save_kbsan = g_paths.known_bugs_san;
      g_paths.known_bugs = tmp + "/kb.strings";
      g_paths.known_bugs_san = tmp + "/kb.strings.SAN";
      string ktd = fixture_trial(tmp + "/known", 12, string(FX_HEAD) + "2026-09-05  5:00:01 0 [ERROR] InnoDB: Table test/t3 index c is corrupt\n", uid);
      write_file(g_paths.known_bugs, "MDEV-11111 " + uid + "\n");
      write_file(g_paths.known_bugs_san, "");
      st_check(quiet_call(cmd_reduce, {dirname_of(ktd), "12", "--plan", "--basedir", bpath}) == 0, "reduce --plan on a trial already in the known-bug list");
      st_check(read_file(ktd + "/reduce.conf").find("Table test/t3 index c is corrupt") != string::npos, "the error-log line is what the reducer looks for");
      st_check(file_exists(ktd + "/MYBUG.orig"), "and the UniqueID the trial had is kept beside it");
      // the error log only adds an ASSERT| shadow of the same crash: the override is skipped
      string atd2 = fixture_trial(tmp + "/shadow", 13,
                                  string(FX_HEAD) + "mariadbd: /test/13.1/sql/item_func.cc:1234: void f(): Assertion `page not corrupted' failed.\n", uid);
      st_check(quiet_call(cmd_reduce, {dirname_of(atd2), "13", "--plan", "--basedir", bpath}) == 0, "reduce --plan when the error log holds only an ASSERT shadow");
      st_eq(trim(split_lines(read_file(atd2 + "/MYBUG"))[0]), uid, "the UniqueID with the frames is kept");
      // a flagged error-log line on a trial whose MYBUG says nothing was found
      string ftd2 = fixture_trial(tmp + "/flagged", 14,
                                  string(FX_HEAD) + "2026-09-05  5:00:01 0 [ERROR] InnoDB: Table test/t4 index d is corrupt\n",
                                  "Assert: no core file found in */*core*");
      write_file(ftd2 + "/ERROR_LOG_SCAN_ISSUE", "the scan flagged a line\n");
      st_check(quiet_call(cmd_reduce, {dirname_of(ftd2), "14", "--plan", "--basedir", bpath}) == 0, "reduce --plan on a trial with a flagged error-log line");
      st_check(read_file(ftd2 + "/reduce.conf").find("Table test/t4 index d is corrupt") != string::npos, "the flagged line is what the reducer looks for");
      g_paths.known_bugs = save_kb;
      g_paths.known_bugs_san = save_kbsan;
    }
    // --screen hands the reduction to a screen session. PATH is emptied for this one call, so
    // screen is not found and no reducer starts: what is checked is the handover, not a reduction.
    {
      const char* pathsave = getenv("PATH");
      string keep = pathsave ? pathsave : "";
      setenv("PATH", (tmp + "/nowhere").c_str(), 1);
      int rc = quiet_call(cmd_reduce, {wd, "1", "--screen", "--basedir", bpath});
      setenv("PATH", keep.c_str(), 1);
      st_check(rc == 0, "reduce --screen hands the trial to a screen session");
    }
  }
  // the title of a report names the statement the bug is in
  {
    struct Op { const char* sql; const char* want; };
    const Op ops[] = {
      {"CREATE OR REPLACE VIEW v1 AS SELECT 1;\n", "CREATE VIEW"},
      {"ALTER TABLE t1 ADD COLUMN b INT;\n", "ALTER TABLE"},
      {"SET GLOBAL innodb_flush_log_at_trx_commit=0;\n", "SET"},
      {"INSERT INTO t1 VALUES (1);\n", "INSERT"},
    };
    int i = 0;
    for (auto& op : ops) {
      // no Query line in this log, so the last statement of the testcase names the operation
      string plainlog = string(FX_HEAD) + "mariadbd: /test/13.1/sql/item_func.cc:1287: virtual void f(): Assertion `x >= 0' failed.\n";
      string otd = fixture_trial(tmp + "/op" + std::to_string(++i), 6, plainlog, "x >= 0|SIGABRT|f|g|h|i");
      write_file(otd + "/default.node.tld_thread-0.sql_out", string("# mysqld options required for replay: --sql_mode=\n") + op.sql);
      string inbox = tmp + "/inbox_op" + std::to_string(i);
      if (quiet_call(cmd_report, {dirname_of(otd), "6", "--no-matrix", "--no-mtr", "--out", inbox}) != 0) continue;
      auto kv = report_header(read_file(inbox + "/O999999_bug6.report"), nullptr);
      string title = report_field(kv, "Title");
      st_check(title.find(op.want) != string::npos, string("the title of a ") + op.want + " testcase names it");
    }
  }
  // the report of a sanitizer trial
  {
    string sanuid = "ASAN|heap-use-after-free|sql/item_cmpfunc.cc|Item_func_case::val_int|Item::val_int_result|sub_select|do_select";
    string std_ = fixture_trial(tmp + "/san", 3, fixture_san_log(), sanuid);
    string inbox = tmp + "/inbox_san";
    st_check(quiet_call(cmd_report, {dirname_of(std_), "3", "--no-matrix", "--no-mtr", "--out", inbox}) == 0, "a report on a sanitizer trial");
    string text = read_file(inbox + "/O999999_bug3.report");
    st_check(text.find("ASAN") != string::npos, "the sanitizer report names the sanitizer");
    JiraFields f;
    string ferr;
    st_check(report_to_fields(text, f, &ferr), "the sanitizer report gives ticket fields");
    st_check(!f.labels.empty() || !f.components.empty(), "and it carries labels or components");
  }
  // the report takes a testcase file of its own
  {
    write_file(tmp + "/own.sql", "SELECT 1;\nSELECT 2;\n");
    st_check(quiet_call(cmd_report, {wd, "1", "--no-matrix", "--no-mtr", "--sql", tmp + "/own.sql", "--out", tmp + "/inbox_sql"}) == 0, "a report with a testcase given by hand");
    st_check(read_file(tmp + "/inbox_sql/O999999_bug1.report").find("SELECT 2;") != string::npos, "that testcase is the one in the report");
  }
  // what the report says no to
  st_check(quiet_call(cmd_report, {"--nonsense"}) == 2, "report refuses an unknown flag");
  st_check(quiet_call(cmd_report, {"not_a_number"}) == 2, "report wants a trial number");
  st_check(quiet_call(cmd_report, {wd, "9999", "--no-matrix", "--no-mtr"}) != 0, "report says no for a trial that is not there");
  // the ticket fields: what a report has to carry
  {
    string text = read_file(tmp + "/inbox_sql/O999999_bug1.report");
    JiraFields f;
    string ferr;
    st_check(report_to_fields(text, f, &ferr), "report_to_fields on a full report");
    st_check(!report_to_fields("no header at all\n", f, &ferr), "a text with no header is refused");
    st_check(!ferr.empty(), "and it says what is missing");
    auto kv = report_header(text, nullptr);
    st_check(!report_field(kv, "Title").empty(), "the report has a title");
    st_check(report_field(kv, "No Such Field").empty(), "a field that is not there reads as empty");
  }
  // the MTR form: the same testcase, and what it says no to
  st_check(quiet_call(cmd_mtr, {wd, "1", "--no-verify", "--out", tmp + "/t1.test"}) == 0, "mtr writes a .test file");
  st_check(read_file(tmp + "/t1.test").find("SELECT") != string::npos, "the .test file holds the SQL");
  {                                                            // the workdir named without a path
    string save = g_cfg.data_dir;
    g_cfg.data_dir = dirname_of(wd);
    st_check(quiet_call(cmd_mtr, {basename_of(wd), "1", "--no-verify", "--out", tmp + "/t2.test"}) == 0, "mtr takes a workdir name on its own");
    st_check(quiet_call(cmd_mtr, {basename_of(wd).substr(1), "1", "--no-verify", "--out", tmp + "/t3.test"}) == 0, "and the workdir number on its own");
    g_cfg.data_dir = save;
  }
  st_check(quiet_call(cmd_mtr, {"--nonsense"}) != 0, "mtr refuses an unknown option");
  st_check(quiet_call(cmd_mtr, {wd, "9999", "--no-verify"}) != 0, "mtr says no for a trial that is not there");
  remove_tree(tmp);
}

// a holder started through a link named o: its command line has no omnium in it, and its binary,
// which /proc/<pid>/exe gives on Linux, still tells it is a live omnium
static void st_holder_through_link(const string& tmp) {
  string ldir = fmt("/tmp/olk_%d", (int)getpid());
  mkdirs(ldir);
  std::error_code ec;
  fs::create_symlink(self_exe(), ldir + "/o", ec);
  string save_exe = g_exe_override;
  g_exe_override = ldir + "/o";
  Child lh;
  bool have = spawn_role(lh, "hold", {"x"}, "/dev/null");
  g_exe_override = save_exe;
  for (int i = 0; have && i < 100 && proc_cmdline(lh.pid).find(ldir) == string::npos; i++) usleep(20000);   // until the exec is done
  string lwd = tmp + "/O888888";
  mkdirs(lwd);
  write_file(lwd + "/omnium.ledger", "1 run created\n");
  write_file(lwd + "/omnium.pid", std::to_string((int)(have ? lh.pid : 0)) + "\n");
  st_check(have && proc_cmdline(lh.pid).find("omnium") == string::npos, "a holder started through the o link has no omnium in its command line");
  st_check(have && pid_is_live_omnium(lh.pid), "and its binary still tells it is a live omnium");
  st_check(!workdir_open(lwd, true), "so the workdir it holds is not opened");
  if (have) st_stub_stop(lh);
  remove_tree(ldir);
}

// the last corners: the binlog marker files, the stack verb, the parity verb on a corpus of its
// own, the view's own run picker, and the builds and known-bug verbs on temporary copies
// The small branches that need no server: the JSON reader, the build registry order, the workdir
// lock, the client logfiles it cannot open, and the shapes each formatter has to get right.
static void st_units() {
  string tmp = st_tmp() + "/units";
  mkdirs(tmp);
  // ---- the JSON reader and writer -------------------------------------------------------------
  {
    string raw = string("a\tb\r\nc") + char(1) + "\"d\\e";
    string esc = json_escape(raw);
    st_check(esc.find("\\t") != string::npos && esc.find("\\r") != string::npos && esc.find("\\n") != string::npos,
             "json_escape writes tab, carriage return and newline as escapes");
    st_check(esc.find("\\u0001") != string::npos, "and a control byte as \\u0001");
    st_check(esc.find("\\\"") != string::npos && esc.find("\\\\") != string::npos, "and escapes the quote and the backslash");
    JsonValue v;
    st_check(json_parse("{\"a\":\"x\\ty\\r\\n\\b\\f\\u0041z\",\"b\":[1,2,{\"c\":true}],\"d\":null,\"e\":-1.5e2}", v), "json_parse reads the escapes and the nesting");
    st_eq(v.str_at("a"), "x\ty\r\n\b\fAz", "every escape comes back as its byte");
    st_check(v.get("b") && v.get("b")->arr.size() == 3, "the array has its three members");
    st_check(v.get("b")->arr[2].str_at("c") == "true", "and the object inside it reads back");
    st_check(v.get("nosuchkey") == nullptr, "a key that is not there gives nothing");
    st_check(v.str_at("b.9.c").empty() && v.str_at("a.b").empty(), "a path that leads nowhere gives an empty string");
    for (const char* bad : {"{\"a\":1", "[1,2", "{\"a\" 1}", "{\"a\":\"\\u00\"}", "tru", "{,}"})
      st_check(!json_parse(bad, v), string("json_parse says no to ") + bad);
  }
  // ---- a fixed seed for a repeatable run ------------------------------------------------------
  {
    rng_seed_process(0x0123456789abcdefULL);
    st_check(rng_seed_used() == 0x0123456789abcdefULL, "rng_seed_process takes the seed it is given");
    uint64_t first = rng().next();
    rng_seed_process(0x0123456789abcdefULL);
    st_check(rng().next() == first, "and the same seed gives the same first number");
  }
  // ---- the workdir lock and the id read from the cwd ------------------------------------------
  {
    string save_data = g_cfg.data_dir;
    g_cfg.data_dir = tmp;
    string wd = tmp + "/O123456";
    mkdirs(wd);
    write_file(wd + "/omnium.ledger", "1 run created 2026-09-05 05:00:00 seed 1\n");
    Child holder;                                              // a live omnium that is not us
    bool have_holder = spawn_role(holder, "hold", {wd}, "/dev/null");
    write_file(wd + "/omnium.pid", std::to_string((int)(have_holder ? holder.pid : getppid())) + "\n");
    st_check(!workdir_open(wd, true), "a workdir held by a live omnium is not opened");
    fs::remove(wd + "/omnium.pid");
    if (have_holder) st_stub_stop(holder);
    st_check(workdir_open(wd, true), "and it opens once the holder is gone");
    string cwd = abs_path(".");
    if (chdir(wd.c_str()) == 0) {
      st_eq(workdir_id_from_cwd(), "O123456", "the workdir id is read from the directory the shell is in");
      if (chdir(cwd.c_str()) != 0) { /* the tmp dir goes anyway */ }
    }
    mkdirs(tmp + "/plain");
    st_check(!workdir_open(tmp + "/plain", false), "a directory that is not an omnium workdir is refused");
    {                                                          // a lock left by a pid that is now some other program: stale, so the workdir opens
      string swd = tmp + "/O777777";
      mkdirs(swd);
      write_file(swd + "/omnium.ledger", "1 run created\n");
      write_file(swd + "/omnium.pid", "1\n");                 // pid 1 is alive and is not omnium
      st_check(workdir_open(swd, true), "a lock held by a live pid that is not an omnium is stale and taken over");
      st_eq(trim(read_file(swd + "/omnium.pid")), std::to_string((long)getpid()), "and the lock now names this process");
      release_lock();
    }
    {
      // a live omnium whose command line does not name it: a run started through the o link.
      // /proc/<pid>/exe is the resolved binary on Linux; MSYS2's names the path the process was
      // started by, link and all
      if (kHostMsys2)
        st_skip("a holder started through the o link: its binary tells it is a live omnium "
                "(MSYS2's /proc/<pid>/exe does not resolve the link)");
      else
        st_holder_through_link(tmp);
      // a live pid that is some other program: what a lock names after a reboot
      pid_t sp = spawn_program({"sleep", "30"}, "/dev/null", "", true);
      for (int i = 0; sp > 0 && i < 100 && !starts_with(proc_cmdline(sp), "sleep"); i++) usleep(20000);
      st_check(sp > 0 && pid_alive(sp) && !pid_is_live_omnium(sp), "a live pid that is not an omnium holds no lock");
      if (sp > 0) { kill(sp, SIGKILL); wait_pid(sp, 5000); }
    }
    st_check(!workdir_open("O654321", false), "and a workdir number with no directory behind it");
    g_cfg.data_dir = save_data;
  }
  // ---- the build registry: the order of the list, and a file it cannot write ------------------
  {
    // one entry per flavour, vendor and type, added out of order: the list has to come back sorted
    Registry r;
    struct E { const char* name; const char* vendor; const char* series; const char* flav; const char* type; };
    const E rows[] = {
      {"MD010126-mariadb-13.1.0-linux-x86_64-opt", "CS", "13.1", "plain", "opt"},
      {"VAL_MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "VAL", "dbg"},
      {"GAL_MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "GAL", "dbg"},
      {"TSAN_MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "TSAN", "dbg"},
      {"MSAN_MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "MSAN", "dbg"},
      {"UBASAN_MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "UBASAN", "dbg"},
      {"MD010126-mariadb-13.1.0-linux-x86_64-dbg", "CS", "13.1", "plain", "dbg"},
      {"MD010126-mariadb-12.3.0-linux-x86_64-opt", "CS", "12.3", "plain", "opt"},
      {"PS010126-percona-8.4.0-linux-x86_64-opt", "PS", "8.4", "plain", "opt"},
      {"EMD010126-mariadb-13.1.0-linux-x86_64-opt", "ES", "13.1", "plain", "opt"},
      {"XX010126-other-1.0.0-linux-x86_64-opt", "XX", "1.0", "OTHER", "opt"},
    };
    for (auto& x : rows) {
      BuildEntry e;
      e.name = x.name; e.vendor = x.vendor; e.series = x.series; e.version = string(x.series) + ".0";
      e.flavour = x.flav; e.type = x.type; e.date = "010126"; e.origin = "hand"; e.commit = "-";
      e.test = string(x.flav) == "plain"; e.report = e.test;
      r.entries.push_back(e);
    }
    string listed = registry_format(r);
    auto at = [&](const char* n) { return listed.find(n); };
    st_check(at("MD010126-mariadb-12.3.0") < at("MD010126-mariadb-13.1.0-linux-x86_64-dbg"), "the build list runs oldest series first");
    st_check(at("MD010126-mariadb-13.1.0-linux-x86_64-opt") < at("EMD010126-mariadb-13.1.0") &&
             at("EMD010126-mariadb-13.1.0") < at("PS010126-percona-8.4.0") && at("PS010126-percona-8.4.0") < at("XX010126-other-1.0.0"),
             "then Community, Enterprise, Percona and anything else");
    st_check(at("MD010126-mariadb-13.1.0-linux-x86_64-dbg") < at("MD010126-mariadb-13.1.0-linux-x86_64-opt"), "debug before optimised");
    st_check(at("MD010126-mariadb-13.1.0-linux-x86_64-opt") < at("UBASAN_MD010126") && at("UBASAN_MD010126") < at("MSAN_MD010126"),
             "plain, then UBSAN+ASAN, then MSAN");
    st_check(at("MSAN_MD010126") < at("TSAN_MD010126") && at("TSAN_MD010126") < at("VAL_MD010126") && at("VAL_MD010126") < at("GAL_MD010126"),
             "then TSAN, Valgrind and Galera");
    st_check(registry_names(r, true).size() == 5, "five of these builds would be tested");
    // the verbs, against a list of its own so the real one is untouched
    string save_bf = g_paths.builds_file;
    g_paths.builds_file = tmp + "/builds.txt";
    write_file(g_paths.builds_file, registry_format(r));
    st_check(quiet_call(cmd_builds, {"test"}) == 0, "builds test lists what a run would test");
    st_check(quiet_call(cmd_builds, {"report"}) == 0, "builds report lists what a report would compare");
    st_check(quiet_call(cmd_builds, {"nonsense"}) == 2, "builds refuses a word it does not know");
    // EDITOR is set to a command that does nothing, so nothing opens on the screen
    {
      string save_ed = g_cfg.editor;
      g_cfg.editor = "true";
      st_check(quiet_call(cmd_builds, {"edit"}) == 0, "builds edit opens the list in the editor");
      string save_bugs = g_paths.bugs_dir;
      g_paths.bugs_dir = tmp + "/BUGS";
      mkdirs(g_paths.bugs_dir);
      st_check(quiet_call(cmd_eb, {}) == 2, "eb without a ticket says how to call it");
      st_check(quiet_call(cmd_eb, {"not-a-key"}) == 2, "eb refuses something that is not a ticket key");
      st_check(quiet_call(cmd_eb, {"MDEV-12345"}) == 0, "eb opens the stored SQL of a ticket");
      st_check(quiet_call(cmd_eb, {"12345"}) == 0, "eb takes the number on its own");
      g_cfg.editor = "false";                                  // an editor that ends 1: the verb says 1, not a wait status
      st_check(quiet_call(cmd_eb, {"MDEV-12345"}) == 1 && quiet_call(cmd_builds, {"edit"}) == 1, "an editor that fails gives its exit code back");
      g_cfg.editor = "true";
      g_paths.bugs_dir = save_bugs;
      g_cfg.editor = save_ed;
    }
    // a list omnium cannot write: the scan still answers, with a note
    g_paths.builds_file = tmp + "/no_such_dir/builds.txt";
    Registry r2 = registry_current();
    bool said = false;
    for (auto& n : r2.notices) if (n.find("cannot write") != string::npos) said = true;
    st_check(said, "a build list that cannot be written is reported as such");
    g_paths.builds_file = save_bf;
  }
  // ---- the trial verb, with the workdir named on its own ---------------------------------------
  {
    string save = g_cfg.data_dir;
    g_cfg.data_dir = tmp;
    string td = fixture_trial(tmp, 1, fixture_errlog(), "SIGSEGV|a|b|c|d");
    st_check(quiet_call(cmd_trial, {"O999999", "1"}) == 0, "the trial verb takes a workdir name on its own");
    st_check(quiet_call(cmd_trial, {"999999", "1"}) == 0, "and the workdir number on its own");
    st_check(quiet_call(cmd_trial, {"O999999", "1", "2"}) == 2, "the trial verb takes at most a workdir and a number");
    g_cfg.data_dir = save;
  }
  // ---- the client logfiles it cannot open -----------------------------------------------------
  {
    string logdir = tmp + "/clogs";
    mkdirs(logdir);
    // a directory where the client wants to write a file: the open fails and the thread stops
    mkdirs(logdir + "/default.node.tld_thread-0.sql");
    write_file(tmp + "/one.sql", "SELECT 1;\n");
    ClientParams p;
    p.ep.sock = tmp + "/no_such.sock";
    p.logdir = logdir;
    p.threads = 1;
    p.queries_per_thread = 1;
    std::atomic<bool> stop{false};
    ClientResult res;
    string err;
    client_run(p, tmp + "/one.sql", stop, res, &err);
    st_check(res.performed == 0, "a trace file that cannot be opened stops that thread");
    st_check(read_file(logdir + "/default.node.tld_general.log").find("Unable to open thread logfile") != string::npos,
             "and the general log says which file");
    // the same for the client output file
    string logdir2 = tmp + "/clogs2";
    mkdirs(logdir2 + "/default.node.tld_thread-0.out");
    ClientParams p2;
    p2.ep.sock = tmp + "/no_such.sock";
    p2.logdir = logdir2;
    p2.threads = 1;
    p2.queries_per_thread = 1;
    p2.log_client_output = true;
    ClientResult res2;
    client_run(p2, tmp + "/one.sql", stop, res2, &err);
    st_check(read_file(logdir2 + "/default.node.tld_general.log").find("Unable to open logfile for client output") != string::npos,
             "an output file that cannot be opened is reported the same way");
  }
  // ---- run_capture on a child killed by a signal ----------------------------------------------
  {
    CmdResult k = run_capture({"/bin/sh", "-c", "kill -9 $$"}, 30);
    st_check(k.rc == 128 + 9, "a command killed by a signal comes back as 128 plus the signal");
    CmdResult e = run_capture({}, 30);
    st_check(e.rc == -1 && e.out.empty(), "run_capture with nothing to run says it could not run");
    CmdResult c = run_capture({"/bin/sh", "-c", "pwd"}, 30, tmp + "/no_such_cwd");
    st_check(c.rc == 126, "a working directory that is not there gives 126");
  }
  // ---- the verbs a child process runs, with settings and a HOME of its own --------------------
  // Every omnium below runs as its own process with HOME pointing into this check's directory, so
  // it reads a settings file the check wrote and writes nothing near the real /test, /data or
  // ~/.omnium.conf. That is also what lets a run's own reduce and report children be checked.
  {
    string h = tmp + "/chome";
    mkdirs(h);
    string ctest = tmp + "/ctest", cdata = tmp + "/cdata";
    mkdirs(ctest);
    mkdirs(cdata);
    write_file(h + "/.omnium.conf", "TEST_DIR=" + ctest + "\nDATA_DIR=" + cdata + "\nQA_DIR=" + g_cfg.qa_dir + "\nEMAIL=\nAUTO_PIPELINE=0\n");
    auto child = [&](const vector<string>& args, int timeout_s) {
      vector<string> argv = {self_exe()};
      for (auto& x : args) argv.push_back(x);
      return run_capture(argv, timeout_s, tmp, {"HOME=" + h});
    };
    // init reports what is missing, so with a fresh HOME it ends non-zero; what is checked is
    // that it makes the directories it can and says what a person still has to do
    CmdResult ini = child({"init"}, 120);
    st_check(ini.out.find("queues") != string::npos, "omnium init lays out the directories it needs");
    st_check(dir_exists(ctest + "/omnium/HUMAN-queue"), "the human queue is one of them");
    st_check(file_exists(h + "/.omnium_aliases"), "and the shell aliases are written");
    {
      string al = read_file(h + "/.omnium_aliases");
      st_check(al.find("alias orun='" + g_paths.repo + "/omnium run'") != string::npos && al.find("alias o=") == string::npos,
               "each alias names the binary by its full path, and none takes the framework's o");
      // the README lists these short names: a name added here and not there sends a reader looking
      string readme = read_file(g_paths.repo + "/README.md");
      vector<string> absent;
      for (auto& l : split_lines(al)) {
        if (!starts_with(l, "alias ")) continue;
        string name = l.substr(6, l.find('=', 6) - 6);
        if (readme.find(" " + name + " ") == string::npos && readme.find(" " + name + "\n") == string::npos) absent.push_back(name);
      }
      st_check(absent.empty(), "and the README lists every one of them [" + join(absent, " ") + "]");
    }
    {  // every verb the help prints is in the README's verb table
      string readme = read_file(g_paths.repo + "/README.md");
      vector<string> absent;
      for (auto& l : split_lines(call_output(cmd_help, {}))) {
        if (!starts_with(l, "  ") || l.size() < 4 || l[2] == ' ') continue;
        string verb = l.substr(2, l.find(' ', 2) - 2);
        bool named = false;                                  // `verb` or `verb <args>`, not a longer word that starts with it
        for (size_t k = readme.find("`" + verb); k != string::npos; k = readme.find("`" + verb, k + 1)) {
          char after = readme[k + verb.size() + 1];
          if (!islower((unsigned char)after) && after != '-') { named = true; break; }
        }
        if (!named) absent.push_back(verb);
      }
      st_check(absent.empty(), "the README names every verb omnium has [" + join(absent, " ") + "]");
    }
    st_check(ini.out.find("MISS") != string::npos && ini.out.find("items missing") != string::npos, "and it names what is still missing");
    CmdResult again = child({"init", "--fix"}, 120);
    st_check(again.out.find("aliases written") != string::npos, "omnium init --fix writes the aliases again");
    // a settings file from before a key was added: the new keys go at its end, and its own lines
    // stay as they are, whatever this one call was given
    {
      string own = "TEST_DIR=" + ctest + "\nDATA_DIR=" + cdata + "\nQA_DIR=" + g_cfg.qa_dir + "\nEMAIL=\nAUTO_PIPELINE=0\n# a note of the user's own\nKEEP_PER_UID=3\n";
      write_file(h + "/.omnium.conf", own);
      child({"KEEP_PER_UID=9", "init"}, 120);
      string conf = read_file(h + "/.omnium.conf");
      st_check(starts_with(conf, own) && conf.find("\nRAM_CAP_PCT=") != string::npos && conf.find("KEEP_PER_UID=9") == string::npos,
               "init adds the keys a settings file lacks, and leaves the lines it has alone");
      // the same for the config verb: the key it is given changes in place, and nothing else
      child({"KEEP_PER_UID=9", "config", "core_dir=data"}, 60);
      string saved = read_file(h + "/.omnium.conf");
      st_check(starts_with(saved, own) && saved.find("\nCORE_DIR=data ") != string::npos && saved.find("\nCORE_DIR=auto") == string::npos &&
                   saved.find("KEEP_PER_UID=9") == string::npos,
               "config saves the key it is given in its own line, and leaves the other lines alone");
      write_file(h + "/.omnium.conf", "TEST_DIR=" + ctest + "\nDATA_DIR=" + cdata + "\nQA_DIR=" + g_cfg.qa_dir + "\nEMAIL=\nAUTO_PIPELINE=0\n");
    }
    st_check(child({"help", "no_such_verb"}, 60).rc == 1, "help says so for a verb that does not exist");
    {
      // a known key with a value it does not take is named as that, not as an unknown key
      CmdResult bv = child({"CORE_DIR=bogus", "version"}, 60);
      st_check(bv.rc == 2 && bv.out.find("omnium: bad value for CORE_DIR: bogus (it stays ") != string::npos, "a setting with a bad value is named so");
      CmdResult cv = child({"config", "SMTP_PORT=0"}, 60);
      st_check(cv.rc == 1 && cv.out.find("bad value for SMTP_PORT: 0 (it stays ") != string::npos, "and config says the same");
      CmdResult uk = child({"NO_SUCH_KEY=1", "version"}, 60);
      st_check(uk.rc == 2 && uk.out.find("omnium: unknown key NO_SUCH_KEY") != string::npos, "an unknown key is named as unknown");
    }
    {
      CmdResult nr = child({"run", "--trials", "1"}, 60);
      st_check(nr.rc == 1 && nr.out.find("no build is marked test=yes") != string::npos, "run with no build named, and none marked for test, says so");
    }
    // QA_DIR on the command line moves the files read from the checkout with it
    {
      string q = tmp + "/cqa";
      mkdirs(q);
      write_file(q + "/known_bugs.strings", "SIGSEGV|omnium_st_only_here|b|c|d   ## MDEV-1\n");
      st_check(child({"QA_DIR=" + q, "kbs", "omnium_st_only_here"}, 60).out.find("omnium_st_only_here") != string::npos, "QA_DIR on the command line is the checkout the call reads");
    }
    st_check(child({"help", "run"}, 60).rc == 0, "help on one verb prints its line");
    st_check(child({"NO_SUCH_SETTING=1", "help"}, 60).rc == 2, "a setting omnium does not know stops it");
    st_check(child({"cli", "--rcfile-only"}, 60).out.find("omnium") != string::npos, "cli --rcfile-only prints the shell file it would use");
    st_check(child({"cli", "--nonsense"}, 60).rc == 2, "cli refuses an unknown option");
    // h in the cli: the shortcut box the binary draws. It has to stay inside the width it was
    // given, whatever a shortcut is called, or the frame breaks up on screen.
    {
      const int widths[] = {58, 80, 120, 200};
      for (int w : widths) {
        CmdResult box = child({"cli", "--shortcuts", "--width", std::to_string(w)}, 60);
        vector<string> ln = split_lines(box.out);
        while (!ln.empty() && ln.back().empty()) ln.pop_back();
        size_t widest = 0, narrowest = string::npos, framed = 0;
        for (auto& l : ln) {
          size_t vis = 0;                                  // the frame characters are three bytes each
          for (size_t i = 0; i < l.size(); i++) if (((unsigned char)l[i] & 0xc0) != 0x80) vis++;
          widest = std::max(widest, vis);
          narrowest = std::min(narrowest, vis);
          if (starts_with(l, "\xe2\x94\x82") || starts_with(l, "\xe2\x94\x8c") || starts_with(l, "\xe2\x94\x9c") || starts_with(l, "\xe2\x94\x94")) framed++;
        }
        st_check(box.rc == 0 && !ln.empty(), fmt("the cli shortcut box is drawn at width %d", w));
        st_check(framed == ln.size(), fmt("every line of it is inside the frame at width %d", w));
        st_check(widest == narrowest, fmt("and every line is the same width (%zu..%zu) at %d", narrowest, widest, w));
        st_check((int)widest <= std::max(58, std::min(w, 140)), fmt("which is the width asked for, not more (%zu at %d)", widest, w));
        st_check(box.out.find("rplan") != string::npos && box.out.find("known bugs") != string::npos, fmt("and it names the shortcuts at width %d", w));
        size_t cap = w >= 112 ? 24 : w >= 76 ? 34 : 55;      // three columns, two, then one
        st_check(ln.size() <= cap, fmt("the box is short enough to read at width %d (%zu lines, cap %zu)", w, ln.size(), cap));
        st_check(box.out.find("~") == string::npos, fmt("no help text is cut short at width %d", w));
      }
      st_check(child({"cli", "--shortcut", "tt"}, 60).out.find("known-bug verdict") != string::npos, "h <name> gives one shortcut in full");
      st_check(child({"cli", "--shortcut", "tt"}, 60).out.find("$OMNIUM") == string::npos, "and says what it runs as omnium, not as the shell variable");
      st_check(child({"cli", "--shortcut", "sanitizer"}, 60).out.find("sts") != string::npos, "a word that is not a shortcut name searches the list");
      st_check(child({"cli", "--shortcut", "no_such_shortcut"}, 60).rc == 1, "and an unknown one says so");
      st_check(child({"cli", "--rcfile-only"}, 60).out.find("$OMNIUM\" cli --shortcuts") != string::npos, "the cli shell file calls the binary for h, so there is one list");
    }
    st_check(child({"--role", "generator", "--help"}, 60).out.find("Usage: generator") != string::npos, "the generator takes its own options");
    st_check(child({"--role", "revgen", "--help"}, 60).out.find("usage: revgen") != string::npos, "and so does revgen");
    // omnium cli starts a shell with a startup file of its own. Its stdin is closed, so the shell
    // reads the file, finds nothing to run and leaves at once.
    CmdResult sh = child({"cli"}, 120);
    st_check(file_exists(h + "/.omnium_cli_rc"), "omnium cli writes the shell startup file");
    st_check(read_file(h + "/.omnium_cli_rc").find("omnium") != string::npos, "and the file knows where omnium is [" + trim(tail_lines(sh.out, 1)) + "]");
    // a --role hold child: it does nothing until it is told to stop, and then it stops
    {
      Child hc;
      if (st_check_r(spawn_role(hc, "hold", {tmp + "/held"}, tmp + "/hold.log"), "a hold child starts")) {
        usleep(200000);
        st_check(pid_alive(hc.pid), "and stays there with nothing to do");
        child_send(hc, "stop");
        int rc = -1;
        for (int i = 0; i < 100 && rc == -1; i++) { rc = child_reap(hc, 100); }
        st_check(rc == 0, fmt("and leaves when it is told to stop (rc %d)", rc));
        if (rc == -1) { kill_group(hc.pid, SIGKILL); child_reap(hc, 2000); }
        child_close(hc);
      }
    }
    // a trial saved by hand, then adopted: the reduce goes to a screen that is not there, and the
    // report is written into the child's own queue
    string wd = cdata + "/O777777";
    string td = fixture_trial(cdata, 1, fixture_errlog(), "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y");
    fs::rename(dirname_of(td), wd);
    // a second trial with the same UniqueID and a longer trace: adopt reduces the shorter one
    string td2 = fixture_trial(tmp + "/second", 2, fixture_errlog(), "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y");
    fs::rename(td2, wd + "/2");
    write_file(wd + "/2/default.node.tld_thread-0.sql", "SELECT 1;\nSELECT 2;\nSELECT 3;\nSELECT 4;\nSELECT 5;\n");
    fs::remove(wd + "/1/default.node.tld_thread-0.sql_out");
    fs::remove(wd + "/2/default.node.tld_thread-0.sql_out");
    string bpath2;
    for (auto& x : basedirs_scan(g_cfg.test_dir)) if (!x.bin.empty()) { bpath2 = x.path; break; }
    if (!bpath2.empty()) {
      write_file(wd + "/1/BASEDIR", bpath2 + "\n");
      write_file(wd + "/2/BASEDIR", bpath2 + "\n");
    }
    CmdResult ad = child({"adopt", "O777777"}, 120);
    st_check(ad.rc == 0 && ad.out.find("2 saved trials, 1 UID") != string::npos, "adopt takes a workdir name on its own and groups the trials by UniqueID [" + trim(ad.out) + "]");
    st_check(child({"adopt", "777777"}, 120).rc == 0, "and the workdir number on its own");
    if (!bpath2.empty()) {
      // PATH holds nothing, so the screen the reduce asks for is never found and no reducer starts. Under
      // MSYS2 omnium finds its runtime DLL through PATH, so there /usr/bin stays on it, and has no screen.
      CmdResult rd = run_capture({self_exe(), "adopt", wd, "--reduce", "--screen"}, 300, tmp, {"HOME=" + h, "PATH=" + tmp + "/nowhere" + (kHostMsys2 ? ":/usr/bin" : "")});
      st_check(rd.out.find("reduce:") != string::npos, "adopt --reduce hands each trial to a reducer [" + trim(tail_lines(rd.out, 1)) + "]");
    }
    // the report a run writes for itself: into the child's queue, and named after the trial
    write_file(wd + "/1/default.node.tld_thread-0.sql_out", "# mysqld options required for replay: --sql_mode=\nCREATE TABLE t1 (a INT);\nSELECT 0-SUM(COALESCE(b'0'))/0 AS c;\n");
    CmdResult rp = child({"report", wd, "1", "--no-matrix", "--no-mtr"}, 600);
    st_check(rp.rc == 0, "the report verb runs as its own process [" + trim(tail_lines(rp.out, 1)) + "]");
    st_check(file_exists(ctest + "/omnium/HUMAN-queue/O777777_bug1.report"), "and puts the report in the queue");
    st_check(file_exists(wd + "/bug1.report"), "and a copy beside the trial");
    // the same report again once it is filed: the text is left as the person left it
    write_file(ctest + "/omnium/HUMAN-queue/O777777_bug1.filed", "MDEV-12345 http://example.invalid/browse/MDEV-12345\n");
    write_file(ctest + "/omnium/HUMAN-queue/O777777_bug1.report", "edited by hand\n");
    CmdResult rp2 = child({"report", wd, "1", "--no-matrix", "--no-mtr"}, 600);
    st_check(rp2.rc == 0 && rp2.out.find("already filed") != string::npos, "a report that is already filed is not rewritten [" + trim(tail_lines(rp2.out, 1)) + "]");
    st_eq(trim(read_file(ctest + "/omnium/HUMAN-queue/O777777_bug1.report")), "edited by hand", "and the text stays as it was");
    // a run picked up again: the saved trial has a reduced testcase, so the report is what is
    // left. The resume needs a build to test, so a box with none under TEST_DIR cannot run this
    if (bpath2.empty())
      st_skip("a stopped run is picked up again: there is no build with a server binary under " +
              g_cfg.test_dir);
    else {
      string rwd = cdata + "/O888888";
      mkdirs(rwd);
      write_file(rwd + "/omnium.ledger", fmt("%lld run created 2026-09-05 05:00:00 seed 1\n", (long long)now_s()));
      copy_tree(wd + "/1", rwd + "/1", nullptr);
      // the run needs a build to test, and it is told to stop before it starts a trial: what is
      // checked is the work the resume found, not a trial
      write_file(rwd + "/omnium.ctl", "stop\n");
      write_file(h + "/.omnium.conf", "TEST_DIR=" + ctest + "\nDATA_DIR=" + cdata + "\nQA_DIR=" + g_cfg.qa_dir + "\nEMAIL=\nAUTO_PIPELINE=1\nWORKERS=1\n");
      // The resume runs the report work it found. That copies the server binary and its libraries,
      // which allows ldd 120 s and gdb 600 s of its own, so a 900 s budget here left 180 s for
      // everything else and ran out on a loaded box. The budget has to clear the inner ones.
      CmdResult rr = child({"run", "--resume", "O888888", bpath2}, 1800);
      write_file(h + "/.omnium.conf", "TEST_DIR=" + ctest + "\nDATA_DIR=" + cdata + "\nQA_DIR=" + g_cfg.qa_dir + "\nEMAIL=\nAUTO_PIPELINE=0\n");
      string ledger = read_file(rwd + "/omnium.ledger");
      st_check(rr.rc == 0, fmt("a stopped run is picked up again [rc %d: ", rr.rc) + replace_all(trim(tail_lines(rr.out, 5)), "\n", " | ") + "]");
      st_check(ledger.find("resumed") != string::npos || read_file(rwd + "/omnium.log").find("resumed") != string::npos,
               "and says what it found still to do");
      st_check(ledger.find("report") != string::npos, "the report of the saved trial is what it runs [" + trim(tail_lines(ledger, 1)) + "]");
      st_check(rr.out.find("] trial ") == string::npos, "and the stop it was given launches no trial at all [" + replace_all(trim(tail_lines(rr.out, 3)), "\n", " | ") + "]");
    }
  }
  // ---- the report on UniqueIDs and layouts of other shapes ------------------------------------
  {
    // a UniqueID with no signal in it at all: the title falls back to the class it starts with
    string mtd = fixture_trial(tmp + "/notsig", 1, fixture_errlog(), "MUTEX_ERROR|safe_mutex: Found wrong usage|a|b");
    string mbox = tmp + "/inbox_notsig";
    if (quiet_call(cmd_report, {dirname_of(mtd), "1", "--no-matrix", "--no-mtr", "--out", mbox}) == 0) {
      auto kv = report_header(read_file(mbox + "/O999999_bug1.report"), nullptr);
      string title = report_field(kv, "Title");
      st_check(title.find("MUTEX_ERROR") != string::npos, "a UniqueID with no signal names its class in the title [" + title + "]");
    }
    // a workdir in the pquery-run layout: the build comes from the run log, or from BASEDIR.template
    string save = g_cfg.data_dir;
    g_cfg.data_dir = tmp;
    string ltd = fixture_trial(tmp + "/legacy", 3, fixture_errlog(), "SIGSEGV|a|b|c|d");
    string lwd = dirname_of(ltd);
    fs::remove(lwd + "/omnium.ledger");
    fs::remove(ltd + "/BASEDIR");
    write_file(lwd + "/pquery-run.log", "[14:00:00] Basedir: /test/no-such-build-13.1 | 13.1.0\n");
    TrialTestcase ltc;
    st_check(trial_testcase(lwd, 3, ltc, nullptr) && ltc.basedir == "/test/no-such-build-13.1", "a pquery-run workdir names its build in its run log");
    st_check(quiet_call(cmd_report, {lwd, "3", "--no-matrix", "--no-mtr", "--out", tmp + "/inbox_legacy"}) == 0, "and a report on it is written");
    fs::remove(lwd + "/pquery-run.log");
    write_file(lwd + "/BASEDIR.template", "/test/no-such-build-13.1\n");
    st_check(trial_testcase(lwd, 3, ltc, nullptr) && ltc.basedir == "/test/no-such-build-13.1", "or in BASEDIR.template when there is no run log");
    st_check(quiet_call(cmd_report, {lwd, "3", "--no-matrix", "--no-mtr", "--out", tmp + "/inbox_legacy2"}) == 0, "and that report is written too");
    g_cfg.data_dir = save;
    // a stack with more frames than the report shows: the rest is counted, not printed
    st_check(report_cap_frames(string("{noformat}\n") + "#0  a ()\n#1  b ()\n#2  c ()\n#3  d ()\n#4  e ()\n{noformat}\n", 3).find("more frames not shown") != string::npos,
             "a stack longer than the report shows says how many frames are left out");
    st_check(report_san_label("SUMMARY: AddressSanitizer: heap-use-after-free /a/b.cc:1 in f()") == "heap-use-after-free",
             "the sanitizer class is read from the SUMMARY line");
    st_check(report_san_label("no summary here").empty(), "and a log with no SUMMARY line has no class");
    {
      auto first = [](const string& uid) { auto c = guess_components(uid, {}); return c.empty() ? string() : c[0]; };
      st_eq(first("subq_pred->test_set_strategy(8)|SIGABRT|setup_jtbm_semi_joins|JOIN::optimize_inner|JOIN::optimize|mysql_select"), "Optimizer",
            "components: a semi-join frame is the optimizer's, not MyISAM's");
      st_eq(first("SIGSEGV|mi_write|ha_myisam::write_row|handler::ha_write_row|write_record"), "Storage Engine - MyISAM", "components: a MyISAM frame is MyISAM's");
      st_eq(first("SIGSEGV|_mi_ck_write|mi_write|write_record|mysql_insert"), "Storage Engine - MyISAM", "components: and so is an internal _mi_ frame");
    }
  }
  // ---- the inbox: how old an item is, the daily cap, and the Jira names it fits ----------------
  {
    string inbox = tmp + "/inbox_age";
    mkdirs(inbox);
    // three items, one minutes old, one hours old, one days old
    struct Age { const char* name; int64_t secs; const char* want; };
    const Age ages[] = {{"m_item", 120, "m"}, {"h_item", 7200, "h"}, {"d_item", 200000, "d"}};
    for (auto& x : ages) {
      string f = inbox + "/" + x.name + ".report";
      write_file(f, "Title: an item\nUID: SIGSEGV|a|b|c|d\n-----\nbody\n");
      auto t = fs::file_time_type::clock::now() - std::chrono::seconds(x.secs);
      std::error_code ec;
      fs::last_write_time(f, t, ec);
    }
    st_check(quiet_call(cmd_inbox, {"--dir", inbox}) == 0, "the inbox lists items of every age");
  }
  remove_tree(tmp);
}

// The filing corners: the day's cap, the Jira version and component names an item is fitted to, and
// what happens when the Jira it talks to answers nothing. All of it against the stand-in Jira.
static void st_inbox_corners() {
  string tmp = st_tmp() + "/inboxc";
  mkdirs(tmp);
  string portfile = tmp + "/port";
  Child c;
  if (!spawn_role(c, "jirastub", {portfile}, tmp + "/stub.log")) { st_check(false, "the Jira stub starts for the inbox corners"); return; }
  string port;
  for (int i = 0; i < 200 && port.empty(); i++) { port = trim(read_file(portfile)); if (port.empty()) usleep(25000); }
  st_check(!port.empty(), "the Jira stub took a port for the inbox corners");
  if (port.empty()) { kill_group(c.pid, SIGKILL); child_reap(c, 5000); child_close(c); return; }
  string save_url = g_cfg.jira_url, save_kb = g_paths.known_bugs, save_kbsan = g_paths.known_bugs_san, save_bugs = g_paths.bugs_dir;
  g_cfg.jira_url = "http://127.0.0.1:" + port;
  g_paths.known_bugs = tmp + "/known_bugs.strings";
  g_paths.known_bugs_san = tmp + "/known_bugs.strings.SAN";
  g_paths.bugs_dir = tmp + "/BUGS";
  const char* kb_skeleton = "# omnium selftest copy, never the real list\n##### CURRENT BUGS (Search key: Mac) #####\n\n##### FIXED BUGS #####\n";
  write_file(g_paths.known_bugs, kb_skeleton);
  write_file(g_paths.known_bugs_san, kb_skeleton);
  mkdirs(g_paths.bugs_dir);
  string inbox = tmp + "/inbox";
  mkdirs(inbox);
  auto item = [&](const string& name, const string& extra) {
    write_file(inbox + "/" + name + ".report",
               "Title: SIGABRT in f on SELECT\nProject: MDEV\nType: Bug\nUID: SIGABRT|f|g|h|i\nComponents: Server\n" + extra + "-----\nbody\n");
    write_file(inbox + "/" + name + ".ok", "");
  };
  // a version and a component Jira does not have: both are dropped, and the item still files
  item("fit1", "Affects: 13.1, 99.9\nFix: 13.1\n");
  write_file(inbox + "/fit1.report", replace_all(read_file(inbox + "/fit1.report"), "Components: Server", "Components: Server, No Such Component"));
  CmdResult unused;
  (void)unused;
  st_check(quiet_call(cmd_inbox, {"--dir", inbox, "--process"}) == 0, "an item with a version Jira does not have still files");
  st_check(read_file(inbox + "/fit1.filed").find("MDEV-") != string::npos, "and gets a key");
  // a version Jira has only as an EOL branch goes in neither field
  item("eol1", "Affects: 11.4, 13.1\nFix Version: 11.4\n");
  st_check(quiet_call(cmd_inbox, {"--dir", inbox, "--file", "eol1"}) == 0, "an item with an EOL branch in both fields files");
  {
    string pv = read_file(inbox + "/eol1.preview");
    st_check(pv.find("EOL") == string::npos && pv.find("\"versions\":[{\"name\":\"13.1\"}]") != string::npos,
             "and the EOL branch is in neither field [" + pv.substr(0, 0) + "]");
  }
  // the cap on how many are filed in one day
  {
    string capbox = tmp + "/capbox";
    mkdirs(capbox);
    for (int i = 0; i < 10; i++) write_file(capbox + fmt("/old%d.filed", i), "MDEV-1 http://example.invalid\n");
    write_file(capbox + "/new1.report", "Title: SIGABRT in f on SELECT\nProject: MDEV\nType: Bug\nUID: SIGABRT|f|g|h|j\nComponents: Server\nAffects: 13.1\n-----\nbody\n");
    write_file(capbox + "/new1.ok", "");
    st_check(quiet_call(cmd_inbox, {"--dir", capbox, "--process"}) != 0, "the eleventh filing of a day is held back");
    st_check(read_file(capbox + "/new1.error").find("daily cap") != string::npos, "and says it is the daily cap");
    st_check(file_exists(capbox + "/new1.ok"), "the approval stays, so it is tried again tomorrow");
  }
  // a Jira that answers nothing: the version list cannot be read, and the filing fails
  {
    string offbox = tmp + "/offbox";
    mkdirs(offbox);
    write_file(offbox + "/off1.report", "Title: SIGABRT in f on SELECT\nProject: MDEV\nType: Bug\nUID: SIGABRT|f|g|h|k\nComponents: Server\nAffects: 13.1\n-----\nbody\n");
    write_file(offbox + "/off1.ok", "");
    g_cfg.jira_url = "http://127.0.0.1:9";                      // nothing listens there
    st_check(quiet_call(cmd_inbox, {"--dir", offbox, "--process"}) != 0, "a Jira that does not answer makes the filing fail");
    string why = read_file(offbox + "/off1.error");
    st_check(why.find("filing failed") != string::npos, "and the item says the filing failed [" + trim(why) + "]");
    st_check(read_file(offbox + "/off1.preview").find("\"project\"") != string::npos, "the payload is still written, so it can be looked at");
    g_cfg.jira_url = "http://127.0.0.1:" + port;
  }
  g_cfg.jira_url = save_url;
  g_paths.known_bugs = save_kb;
  g_paths.known_bugs_san = save_kbsan;
  g_paths.bugs_dir = save_bugs;
  st_stub_stop(c);
  remove_tree(tmp);
}

// A Windows build: the server's own backtrace in the error log gives the UID a core gives on Linux,
// the two CRT assert forms give the expression, and a native C:\test path is scrubbed as /test/X/ is
static void st_windows() {
  string tmp = st_tmp() + "/win";
  mkdirs(tmp);
  st_eq(windows_signal("0xc0000005"), "SIGSEGV", "windows_signal: access violation");
  st_eq(windows_signal("0x80000003"), "SIGABRT", "windows_signal: the breakpoint of the abort route");
  st_eq(windows_signal("0xC0000094"), "SIGFPE", "windows_signal: divide by zero, upper case hex");
  st_eq(windows_signal("0xc0000409"), "exception 0xc0000409", "windows_signal: an unknown code stays as printed");
  const char* head =
    "2026-09-12 10:00:00 0 [Note] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe (server 13.1.1-MariaDB-debug) starting as process 4242 ...\n"
    "2026-09-12 10:00:05 0 [Note] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe: ready for connections.\n";
  const char* tail =
    "\n"
    "Trying to get some variables.\n"
    "Some pointers may be invalid and cause the dump to abort.\n"
    "Query (0x1f2e3d4c): SELECT CASE WHEN a THEN 1 END FROM t1\n"
    "\n"
    "Connection ID (thread ID): 4\n"
    "Status: NOT_KILLED\n"
    "Minidump written to C:\\test\\data\\mariadbd.dmp\n";
  auto crash = [&](const char* code) {
    return fmt("260912 10:00:07 [ERROR] C:\\test\\MD120926-mariadb-13.1.1-windows-x86_64-dbg\\bin\\mariadbd.exe got exception %s ;\n"
               "Sorry, we probably made a mistake, and this is a bug.\n\n"
               "Server version: 13.1.1-MariaDB-debug-log source revision: abc\n\n"
               "Attempting backtrace. Include this in the bug report.\n"
               "(note: Retrieving this information may fail)\n\n"
               "Thread pointer: 0x1f2e3d4c\n", code);
  };
  // an access violation: the walk starts at the faulting instruction; do_command sits deep enough
  // for the frames above it to be the UID, as with gdb
  string segv = string(head) + crash("0xc0000005") +
    "server.dll!Item_func_case::val_int()[item_cmpfunc.cc:2544]\n"
    "server.dll!Item::val_int_result()[item.h:1200]\n"
    "server.dll!sub_select()[sql_select.cc:22000]\n"
    "server.dll!do_select()[sql_select.cc:21000]\n"
    "server.dll!JOIN::exec_inner()[sql_select.cc:5000]\n"
    "server.dll!mysql_execute_command()[sql_parse.cc:4000]\n"
    "server.dll!do_command()[sql_parse.cc:1400]\n"
    "server.dll!threadpool_process_request()[threadpool_common.cc:400]\n"
    "KERNEL32.DLL!BaseThreadInitThunk()\n"
    "ntdll.dll!RtlUserThreadStart()\n" + tail;
  UidOptions o;
  o.wait_core = false;
  {
    string td = fixture_trial(tmp + "/segv", 1, segv, "");
    UidResult r;
    st_check(uid_for_dir(td, r, o), "windows crash: the error log gives a UID");
    st_eq(r.uid, "SIGSEGV|Item_func_case::val_int|Item::val_int_result|sub_select|do_select", "windows crash: the signal name and the first four frames");
    st_check(r.core.empty() && !r.san, "windows crash: no core and no sanitizer were involved");
    UidOptions fo = o;
    fo.frames_only = true;
    st_check(uid_for_dir(td, r, fo) && r.uid == "Item_func_case::val_int|Item::val_int_result|sub_select|do_select", "windows crash: frames only");
  }
  // the same log with CRLF line ends, as the Windows C runtime writes the error log
  {
    string td = fixture_trial(tmp + "/segv_crlf", 3, replace_all(segv, "\n", "\r\n"), "");
    UidResult r;
    st_check(uid_for_dir(td, r, o), "windows crash, CRLF log: the error log gives a UID");
    st_eq(r.uid, "SIGSEGV|Item_func_case::val_int|Item::val_int_result|sub_select|do_select", "windows crash, CRLF log: the same UID as with LF");
  }
  // a failed assert: the CRT prints the expression with a CRLF, abort() reaches the handler through
  // __debugbreak, and the abort route and the CRT frames are left out as gdb's __GI_abort is
  string assert_log = string(head) +
    "Assertion failed: arg2_int >= 0, file C:\\test\\13.1\\sql\\item_func.cc, line 1287\r\n" + crash("0x80000003") +
    "server.dll!my_sigabrt_handler()[my_thr_init.c:449]\n"
    "ucrtbased.dll!raise()\n"
    "ucrtbased.dll!abort()\n"
    "ucrtbased.dll!_wassert()\n"
    "server.dll!Item_func_additive_op::result_precision()[item_func.cc:1287]\n"
    "server.dll!Item_num_op::fix_length_and_dec_decimal()[item_func.cc:1450]\n"
    "server.dll!Item_func_plus::fix_length_and_dec()[item_func.cc:1500]\n"
    "server.dll!Item_func::fix_fields()[item_func.cc:300]\n"
    "server.dll!setup_fields()[sql_base.cc:8000]\n"
    "mariadbd.exe!???\n" + tail;
  {
    string td = fixture_trial(tmp + "/abort", 2, assert_log, "");
    UidResult r;
    st_check(uid_for_dir(td, r, o), "windows assert: the error log gives a UID");
    st_eq(r.uid, "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|Item_func_plus::fix_length_and_dec|Item_func::fix_fields",
          "windows assert: the expression, SIGABRT and the frames below the abort route");
  }
  // the debug CRT's form of the same line
  {
    string dl = tmp + "/dbgcrt.err";
    write_file(dl, string(head) + "C:\\test\\13.1\\sql\\item_func.cc(1287) : Assertion failed: arg2_int >= 0\r\n");
    st_eq(assert_from_logs({dl}), "arg2_int >= 0", "windows assert: the debug CRT form gives the expression");
  }
  // a walk with no usable frame, and a Linux log: neither is a Windows backtrace
  {
    string sig;
    string nl = tmp + "/noframes.err";
    write_file(nl, string(head) + crash("0xc0000005") + "mariadbd.exe!???\nntdll.dll!RtlUserThreadStart()\n" + tail);
    st_check(frames_from_windows_log({nl}, &sig).empty(), "windows: a walk with no usable frame gives no frames");
    string ll = tmp + "/linux.err";
    write_file(ll, fixture_errlog());
    st_check(frames_from_windows_log({ll}, &sig).empty() && sig.empty(), "a Linux log is no Windows backtrace");
  }
  // the path scrub follows TEST_DIR: with /c/test the native C:\test\13.1\ reads as the Linux /test/13.1/ does
  {
    string lin = tmp + "/mutex_linux.err", win = tmp + "/mutex_win.err";
    write_file(lin, "safe_mutex: Trying to lock uninitialized mutex at /test/13.1/sql/sql_class.cc, line 123\n");
    write_file(win, "safe_mutex: Trying to lock uninitialized mutex at C:\\test\\13.1\\sql\\sql_class.cc, line 123\n");
    string saved = g_cfg.test_dir;
    g_cfg.test_dir = "/test";
    string ul = uid_other_strings({lin});
    st_eq(ul, "MUTEX_ERROR|safe_mutex: Trying to lock uninitialized mutex at sql/sql_class.cc", "scrub: the Linux path under /test");
    st_check(uid_other_strings({win}).find("C:\\test\\13.1") != string::npos, "scrub: a native path is not TEST_DIR=/test, so it stays");
    g_cfg.test_dir = "/c/test";
    st_eq(uid_other_strings({win}), ul, "scrub: with TEST_DIR=/c/test the native path gives the Linux UID");
    string fwd = tmp + "/mutex_fwd.err";
    write_file(fwd, "safe_mutex: Trying to lock uninitialized mutex at c:/test/13.1/sql/sql_class.cc, line 123\n");
    st_eq(uid_other_strings({fwd}), ul, "scrub: the forward-slash form with a lower-case drive too");
    g_cfg.test_dir = saved;
  }
}

// Fix Version: with several affected branches it stops below the newest, which gets the fix by
// up-merge; a single affected branch is the Fix Version itself, and the field is never left empty
// while Affects has a value.
static void st_fix_version_units() {
  st_check(join(fix_versions({"13.1"}), ", ") == "13.1", "one affected branch is the Fix Version");
  st_check(join(fix_versions({"10.11", "11.4", "11.8", "12.3", "13.0", "13.1"}), ", ") == "10.11, 11.4, 11.8, 12.3, 13.0",
           "several affected branches: Fix Version stops below the newest");
  st_check(fix_versions({}).empty(), "with nothing affected there is no Fix Version");
  JiraFields f;
  string err;
  st_check(report_to_fields("Title: t\nProject: MDEV\nAffects: 13.1\nFix Version: 13.1\n-----\nbody\n", f, &err) && join(f.fix, ", ") == "13.1",
           "the report's Fix Version line reaches the Jira fields");
  // an item whose Fix Version is empty: it is filled from Affects before the payload goes out
  string box = st_tmp() + "/fixv";
  mkdirs(box);
  write_file(box + "/fix1.report", "Title: SIGABRT in f on SELECT\nProject: MDEV\nType: Bug\nUID: SIGABRT|f|g|h|m\nComponents: Server\n"
                                   "Affects: 12.3, 13.0, 13.1\nFix Version: \n-----\nbody\n");
  string save_url = g_cfg.jira_url;
  g_cfg.jira_url = "http://127.0.0.1:9";                        // nothing listens there; a dry run files nothing
  st_check(quiet_call(cmd_inbox, {"--dir", box, "--dry-run", "--file", "fix1"}) == 0, "a dry run of an item with an empty Fix Version");
  g_cfg.jira_url = save_url;
  st_check(read_file(box + "/fix1.preview").find("\"fixVersions\":[{\"name\":\"12.3\"},{\"name\":\"13.0\"}]") != string::npos,
           "its Fix Version is taken from Affects, stopping below the newest");
  remove_tree(box);
}

static void st_units_tail() {
  string tmp = st_tmp() + "/unitstail";
  mkdirs(tmp);
  // ---- the SQL size per trial: the driver's list of what ran, and a size that is given ----------
  {
    SqlSources pace;
    st_check(sources_target_lines(pace) == 20000, "the first trial of a run gets 20000 lines");
    for (int i = 0; i < 10; i++) sources_note_executed(pace, 4000);
    size_t want = (size_t)std::clamp(4000.0 * g_cfg.sql_size_factor, 2000.0, 5141189.0);
    st_check(sources_target_lines(pace) == want, fmt("then SQL_SIZE_FACTOR times what the last trials ran (%zu, want %zu)", sources_target_lines(pace), want));
    SqlSources given;
    given.target = 20000;
    st_check(sources_target_lines(given) == 20000, "a size that is given is kept to the line");
    given.target = 300;
    st_check(sources_target_lines(given) == 300, "also below the least the driver picks");
  }
  // ---- every build name shape, and the short name each one gets -------------------------------
  {
    struct N { const char* name; const char* vendor; const char* flav; const char* shortn; };
    const N names[] = {
      {"MD010926-mariadb-13.1.0-linux-x86_64-opt", "CS", "", "13.1-opt"},
      {"EMD010926-mariadb-12.3.2-1-linux-x86_64-dbg", "ES", "", "es-12.3-dbg"},
      {"MS010926-mysql-8.0.36-linux-x86_64-opt", "MS", "", "ms-8.0-opt"},
      {"PS010926-percona-server-8.0.36-linux-x86_64-opt", "PS", "", "ps-8.0-opt"},
      {"UBASAN_MD010926-mariadb-13.1.0-linux-x86_64-dbg", "CS", "UBASAN", "13.1-uba-dbg"},
      {"MSAN_MD010926-mariadb-13.1.0-linux-x86_64-dbg", "CS", "MSAN", "13.1-msan-dbg"},
      {"TSAN_MD010926-mariadb-13.1.0-linux-x86_64-dbg", "CS", "TSAN", "13.1-tsan-dbg"},
      {"VAL_MD010926-mariadb-13.1.0-linux-x86_64-dbg", "CS", "VAL", "13.1-val-dbg"},
      {"GAL_MD010926-mariadb-13.1.0-linux-x86_64-dbg", "CS", "GAL", "13.1-gal-dbg"},
    };
    for (auto& x : names) {
      Basedir b2;
      bool parsed = basedir_parse_name(x.name, b2);
      st_check(parsed, string("the name ") + x.name + " parses");
      if (!parsed) continue;
      b2.name = x.name;
      st_eq(b2.vendor_str(), x.vendor, string("and it is a ") + x.vendor + " build");
      st_eq(b2.flavour_str(), x.flav, string("with the flavour ") + (x.flav[0] ? x.flav : "plain"));
      st_eq(b2.short_name(), x.shortn, string("and the short name ") + x.shortn);
    }
    Basedir bad;
    st_check(!basedir_parse_name("MD010926-nodashesnumbers-linux-x86_64-opt", bad), "a name with no version in it is refused");
  }
  // ---- the SQL sources over many trials, so the random parts all show up -----------------------
  {
    Basedir sb;
    bool have = false;
    for (auto& x : basedirs_scan(g_cfg.test_dir)) if (!x.bin.empty()) { sb = x; have = true; break; }
    if (have) {
      string work = tmp + "/sqlwork";
      mkdirs(work);
      SqlSources ss;
      ss.work = work;
      ss.use_disk = false;
      string serr;
      // a pool file put there by hand: the trial reads it instead of walking the disk again
      write_file(work + "/disk_pool.txt", tmp + "/one.sql\n");
      write_file(tmp + "/one.sql", "SELECT 1;\n");
      bool ready = sources_init(ss, sb, work, &serr);
      st_check(ready, "the SQL sources are made ready [" + serr + "]");
      if (ready) {
        Xoshiro256pp r = rng_stream(7);
        const Area* ar = area_by_name("default");
        st_check(ar != nullptr, "the default area is there");
        // random options land on about one trial in ten, so the loop keeps going until it has seen
        // both kinds and then stops; there is no need to build all eighty
        int with_options = 0, made = 0;
        sources_note_executed(ss, 20);                          // a small SQL target, so each round is quick
        if (ar)
          for (int i = 0; i < 80 && (with_options == 0 || made < 3); i++) {
            TrialSql t;
            string e;
            string tdir = tmp + "/sqltrial";
            remove_tree(tdir);
            mkdirs(tdir);
            if (!sources_assemble(ss, sb, *ar, r, tdir, false, t, &e)) continue;
            made++;
            if (!t.random_options.empty()) with_options++;
          }
        st_check(made >= 3, fmt("a run of trials gets its SQL put together (%d)", made));
        st_check(with_options > 0, fmt("and some of them carry random server options (%d of %d)", with_options, made));
        // a MySQL build keeps the area's options but the MariaDB-only deadlock ones
        if (ar) {
          Basedir mb = sb;
          mb.vendor = Vendor::MySQL; mb.version = "8.0.36"; mb.series = "8.0";
          TrialSql t;
          string e, tdir = tmp + "/sqltrial_ms";
          mkdirs(tdir);
          bool ok = sources_assemble(ss, mb, *ar, r, tdir, false, t, &e);
          bool deadlock = false, binlog = false;
          for (auto& o : t.myextra) { if (starts_with(o, "--deadlock-")) deadlock = true; if (o == "--log_bin") binlog = true; }
          st_check(ok && !deadlock && binlog, "a MySQL build gets the area's options without the MariaDB deadlock ones [" + join(t.myextra, " ") + "]");
        }
      }
      // a series with no grammar of its own: the newest grammar below it is used
      Basedir ob = sb;
      ob.series = "99.9";
      SqlSources os;
      os.use_disk = false;
      string oerr;
      string owork = tmp + "/sqlwork2";
      mkdirs(owork);
      st_check(sources_init(os, ob, owork, &oerr) || !oerr.empty(), "a series with no grammar of its own falls back to the newest one there is");
    }
  }
  // ---- the frames of a UniqueID, from a backtrace pasted in -----------------------------------
  {
    // the shape gdb prints: the signal line, then the frames, with the noise the chain drops
    string out1 = "Program terminated with signal SIGABRT, Aborted.\n";
    string bt =
        "#0  __pthread_kill_implementation () at ./nptl/pthread_kill.c:44\n"
        "#1  __GI_raise (sig=6) at ../sysdeps/posix/raise.c:26\n"
        "#2  __GI_abort () at ./stdlib/abort.c:79\n"
        "#3  __assert_fail (assertion=0x1 \"x\") at ./assert/assert.c:101\n"
        "#4  Item_func_x::fix_length_and_dec (this=0x1) at /test/13.1/sql/item_func.cc:1234\n"
        "#5  Item_func_y::fix_fields (this=0x2) at /test/13.1/sql/item.cc:100\n"
        "#6  setup_fields (thd=0x3) at /test/13.1/sql/sql_base.cc:200\n"
        "#7  mysql_select (thd=0x4) at /test/13.1/sql/sql_select.cc:300\n"
        "#8  do_command (thd=0x5) at /test/13.1/sql/sql_parse.cc:400\n";
    string sig;
    bool none = true;
    string frames = frames_from_backtraces(out1, bt, &sig, &none);
    st_eq(sig, "SIGABRT", "the signal is read from the line gdb prints");
    st_check(!none, "and the backtrace does have frames");
    st_eq(frames, "Item_func_x::fix_length_and_dec|Item_func_y::fix_fields|setup_fields|mysql_select",
          "the four frames are the ones left after the noise is dropped");
    // no "Program terminated" line: the (sig=N) form is what is left to read
    string out2 = "Thread 1 received signal SIGSEGV (sig=11)\n";
    string sig2;
    frames_from_backtraces(out2, bt, &sig2, nullptr);
    st_eq(sig2, "sig=11", "with no terminated line the sig= form is used");
    // a backtrace with do_command in the first frames: the trim does not apply
    string shortbt = "#0  do_command (thd=0x1) at /test/13.1/sql/sql_parse.cc:400\n"
                     "#1  handle_one_connection (arg=0x2) at /test/13.1/sql/sql_connect.cc:100\n";
    string sig3;
    st_eq(frames_from_backtraces(out1, shortbt, &sig3, nullptr), "do_command|handle_one_connection",
          "a backtrace that starts at do_command keeps every frame");
    // nothing that parses as a frame
    string sig4;
    bool none4 = false;
    st_check(frames_from_backtraces("", "no frames in here\n", &sig4, &none4).empty(), "a backtrace with no frames gives none");
    st_check(none4, "and says so");
  }
  // ---- the odd file kinds and the long shapes of the formatters -------------------------------
  {
    // a copied tree with something in it that is neither a file nor a directory: it is skipped
    string src = tmp + "/copysrc", dst = tmp + "/copydst";
    mkdirs(src + "/sub");
    write_file(src + "/a.txt", "a\n");
    write_file(src + "/sub/b.txt", "b\n");
    mkfifo((src + "/pipe").c_str(), 0644);
    string why;
    st_check(copy_tree(src, dst, &why), "a tree with a named pipe in it copies [" + why + "]");
    st_check(file_exists(dst + "/a.txt") && file_exists(dst + "/sub/b.txt"), "and the files come with it");
    st_check(!file_exists(dst + "/pipe"), "the named pipe is left behind");
    // a run of days, and counts big enough to be shortened
    st_check(human_secs(200000).find("d ") != string::npos, "a run over a day says how many days [" + human_secs(200000) + "]");
    st_eq(human_secs(3661), "01:01:01", "and under a day it is hours, minutes and seconds");
  }
  // ---- the MX hosts of a domain ---------------------------------------------------------------
  {
    // a plain DNS lookup, no mail sent. A box with no DNS gives nothing back, and that is not a
    // failure of omnium, so the check only asks that the answer and the verdict agree.
    vector<string> hosts;
    string err;
    bool ok = mail_mx("mariadb.org", hosts, &err);
    st_check(ok == !hosts.empty(), fmt("the MX lookup of a real domain agrees with what it found (%zu host(s))", hosts.size()));
  }
  // ---- a repo checked out two levels down, and the settings file it prints ---------------------
  {
    string bin = g_paths.repo + "/build/coverage/omnium_cov";
    if (!is_executable(bin)) bin = g_paths.repo + "/build/release/omnium";
    if (!is_executable(bin)) st_skip("a binary run from build/<mode>/: " + g_paths.repo + " has no build/coverage or build/release binary");
    else {
      CmdResult r = run_capture({bin, "config", "QA_DIR"}, 60, abs_path("."));
      st_check(r.rc == 0 && r.out.find("QA_DIR=") != string::npos, "the binary run from build/<mode>/ still finds the repo files");
    }
  }
  remove_tree(tmp);
}

static void st_corners() {
  string tmp = st_tmp() + "/corners";
  mkdirs(tmp);
  // the two binlog marker files a trial can carry
  {
    string bd = tmp + "/binlog";
    mkdirs(bd + "/log");
    write_file(bd + "/log/master.err", FX_HEAD);
    write_file(bd + "/BINLOG_RECOVERY_ERROR", "ERROR 1594 (HY000) at line 12 in file: 'x.sql': Relay log read failure\n");
    UidResult r;
    UidOptions o;
    o.wait_core = false;
    st_check(uid_for_dir(bd, r, o) && starts_with(r.uid, "BINLOG_RECOVERY_ERROR|"), "a binlog recovery marker gives its own UID");
    remove_tree(bd + "/BINLOG_RECOVERY_ERROR");
    write_file(bd + "/BINLOG_CHECKSUM_DIFF", "binlog checksum differs at 1234567 for the second dump\n");
    UidResult r2;
    st_check(uid_for_dir(bd, r2, o) && starts_with(r2.uid, "BINLOG_CHECKSUM_DIFF|"), "a binlog checksum marker gives its own UID");
    write_file(bd + "/BINLOG_CHECKSUM_DIFF", "");
    UidResult r3;
    st_check(uid_for_dir(bd, r3, o) && starts_with(r3.uid, "BINLOG_CHECKSUM_DIFF|"), "an empty checksum marker still says what it is");
  }
  // the stack verb on a sanitizer trial
  {
    string sd = tmp + "/stack";
    mkdirs(sd + "/log");
    write_file(sd + "/log/master.err", fixture_san_log());
    st_check(quiet_call(cmd_stack, {sd}) == 0, "the stack verb prints the block of a sanitizer trial");
    st_check(quiet_call(cmd_stack, {tmp + "/no_such_dir"}) == 1, "the stack verb says no when there is nothing to read");
  }
  // the parity verb against a corpus of its own: the shell chain and this port, side by side
  {
    string save_data = g_cfg.data_dir;
    g_cfg.data_dir = tmp + "/data";
    string wd = g_cfg.data_dir + "/O111111";
    for (int i = 1; i <= 2; i++) {
      string td = wd + "/" + std::to_string(i);
      mkdirs(td + "/log");
      write_file(td + "/log/master.err", string(FX_HEAD) + (i == 1 ? "safe_mutex: Found wrong usage of mutex 'LOCK_a' and 'LOCK_b'\n" : ""));
      if (i == 2) write_file(td + "/log/master.err", fixture_san_log());
      UidResult r;
      UidOptions o;
      o.wait_core = false;
      uid_for_dir(td, r, o);
      write_file(td + "/MYBUG", r.uid + "\n");
    }
    st_check(quiet_call(cmd_parity, {"1", "--no-gdb"}) == 0, "the parity verb finds no difference on a corpus");
    st_check(quiet_call(cmd_parity, {"1", "--no-gdb", "--verbose"}) == 0, "the parity verb has a verbose form");
    st_check(quiet_call(cmd_parity, {"1"}) == 0, "and with the UniqueID chain of the whole trial included");
    st_check(quiet_call(cmd_parity, {"all"}) == 0, "all takes every trial of each class");
    g_cfg.data_dir = save_data;
  }
  // what the identification verbs do with something other than a trial directory
  {
    string idir = tmp + "/ident";
    mkdirs(idir + "/log");
    // t on a file that holds a pasted gdb trace
    string pasted = idir + "/pasted.txt";
    write_file(pasted, "Program terminated with signal SIGSEGV, Segmentation fault.\n"
                       "#0  Item_func_x::val_int (this=0x1) at /test/13.1/sql/item_func.cc:1234\n"
                       "#1  Item_func_y::fix_fields (this=0x2) at /test/13.1/sql/item.cc:100\n"
                       "#2  setup_fields (thd=0x3) at /test/13.1/sql/sql_base.cc:200\n"
                       "#3  mysql_select (thd=0x4) at /test/13.1/sql/sql_select.cc:300\n");
    st_check(quiet_call(cmd_t, {pasted}) == 0, "t reads a gdb trace pasted into a file");
    st_check(uid_raw_gdb(pasted).find("RAW_GDB_UID|") != string::npos, "and the UniqueID says it came from a pasted trace");
    // sts on a directory that has a replica log as well
    write_file(idir + "/log/master.err", fixture_san_log());
    write_file(idir + "/log/slave.err", string(FX_HEAD));
    st_check(quiet_call(cmd_sts, {idir}) == 0, "sts reads the replica log beside the primary one");
    st_check(quiet_call(cmd_sts, {idir + "/log/master.err"}) == 0, "sts takes a log file by name");
    st_check(quiet_call(cmd_sts, {idir + "/no_such_file"}) == 1, "sts says so when the path is not there");
    string bare = tmp + "/bare";
    mkdirs(bare);
    st_check(quiet_call(cmd_sts, {bare}) == 1, "and when a directory holds no error log");
    // fts with no argument, from inside a directory that has a log where it looks
    string cwd = abs_path(".");
    write_file(idir + "/log/master.err", string(FX_HEAD) + "mariadbd: /test/13.1/sql/log.cc:2323: virtual int f(): Assertion `in_log' failed.\n");
    int frc = -1;
    if (chdir(idir.c_str()) == 0) {
      st_check(trim(call_output(cmd_fts, {}, &frc)) == "FALLBACK|in_log" && frc == 0, "fts with no argument reads ./log/master.err");
      if (chdir(cwd.c_str()) != 0) { /* the tmp dir goes anyway */ }
    }
    string mtrdir = tmp + "/mtrlike";
    mkdirs(mtrdir + "/var/log");
    write_file(mtrdir + "/var/log/mysqld.1.err", string(FX_HEAD) + "mariadbd: /test/13.1/sql/log.cc:2323: virtual int f(): Assertion `all' failed.\n");
    if (chdir(mtrdir.c_str()) == 0) {
      st_check(trim(call_output(cmd_fts, {}, &frc)) == "FALLBACK|all" && frc == 0, "and ./var/log/mysqld.1.err in an MTR layout");
      if (chdir(cwd.c_str()) != 0) { /* the tmp dir goes anyway */ }
    }
    string nolog = tmp + "/nolog";
    mkdirs(nolog);
    if (chdir(nolog.c_str()) == 0) {
      st_check(quiet_call(cmd_fts, {}) == 1, "with no log anywhere fts says so");
      if (chdir(cwd.c_str()) != 0) { /* the tmp dir goes anyway */ }
    }
    // a Valgrind report: the sanitizer reader has a start and end of its own for it
    {
      string vdir = tmp + "/valg";
      mkdirs(vdir + "/log");
      write_file(vdir + "/log/master.err",
                 string(FX_HEAD) +
                 "==1234== Invalid read of size 8\n"
                 "==1234==    at 0x1: Item_func_case::val_int() (item_cmpfunc.cc:2000)\n"
                 "==1234==    by 0x2: Item::val_int_result() (item.h:100)\n"
                 "==1234==  Address 0x3 is 8 bytes inside a block of size 16 free'd\n"
                 "==1234== ERROR SUMMARY: 1 errors from 1 contexts (suppressed: 0 from 0)\n");
      string verr;
      string vuid = uid_san(capped_logs({vdir + "/log/master.err"}), &verr);
      // Valgrind has a reader of its own (bv, gval); the sanitizer reader walks the block with the
      // start and end lines Valgrind uses and comes back with nothing, which is what the shell
      // script does as well
      st_check(vuid.empty() && verr.empty(), "a Valgrind report gives the sanitizer reader nothing [" + vuid + verr + "]");
      string vsh = script_path("san_text_string.sh");
      if (file_exists(vsh)) {
        CmdResult vc = run_capture({vsh, "log/master.err"}, 300, vdir);
        st_eq(trim(vc.out), trim(vuid), "and san_text_string.sh says the same");
      }
    }
    // a sanitizer block already in the known list, on a trial that has a MYBUG file: the file is
    // rewritten to the issue that is left
    {
      string ddir = tmp + "/sandrop2";
      mkdirs(ddir + "/log");
      string blk1 = fixture_san_log();
      string blk2 =
          "==2==ERROR: AddressSanitizer: use-after-poison on address 0x1 at pc 0x2 bp 0x3 sp 0x4\n"
          "READ of size 1 at 0x1 thread T1\n"
          "    #0 0x1 in TYPVAL<char*>::SetValue_char(char const*, int) /test/13.1/storage/connect/value.cpp:1381:8\n"
          "    #1 0x2 in CntIndexRead(_global*, TDB*, OPVAL, st_key_range const*, bool) /test/13.1/storage/connect/connect.cc:1000:5\n"
          "    #2 0x3 in join_read_const(st_join_table*) /test/13.1/sql/sql_select.cc:25311:9\n"
          "    #3 0x4 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:24716:12\n"
          "SUMMARY: AddressSanitizer: use-after-poison /test/13.1/storage/connect/value.cpp:1381:8 in TYPVAL<char*>::SetValue_char(char const*, int)\n";
      write_file(ddir + "/log/master.err", blk1 + blk2);
      write_file(ddir + "/MYEXTRA", "--sql_mode=\n");
      UidResult dr;
      UidOptions dopt;
      dopt.wait_core = false;
      uid_for_dir(ddir, dr, dopt);
      write_file(ddir + "/MYBUG", dr.uid + "\n");
      string save_san = g_paths.known_bugs_san;
      g_paths.known_bugs_san = tmp + "/kb_drop.SAN";
      write_file(g_paths.known_bugs_san,
                 "# a copy for the selftest\n##### CURRENT BUGS (Search key: Mac) #####\n" + dr.uid + "           ## MDEV-100\n\n##### FIXED BUGS #####\n");
      int dropped = san_drop_known(ddir, "log/master.err");
      st_check(dropped >= 1, "a known sanitizer block is dropped from a trial that has a UniqueID file");
      st_check(file_exists(ddir + "/TOP_SAN_ISSUES_REMOVED"), "and the trial is marked as having had one taken out");
      st_check(trim(read_file(ddir + "/MYBUG")) != trim(dr.uid), "the UniqueID file now names what is left");
      g_paths.known_bugs_san = save_san;
    }
  }
  // the view picks the newest run itself, and follows a finished one without a terminal
  {
    string save_data = g_cfg.data_dir, save_hq = g_paths.human_queue;
    g_cfg.data_dir = tmp + "/viewdata";
    g_paths.human_queue = tmp + "/queue";
    mkdirs(g_paths.human_queue);
    string wd = g_cfg.data_dir + "/O222222";
    mkdirs(wd + "/log");
    write_file(wd + "/O222222.log", "[05:00:00] run started\n[05:00:20] run ended\n");
    write_file(wd + "/status.txt", "state=finished\nstarted=" + std::to_string(now_s() - 60) + "\nlaunched=2\nsaved=1\nknown=1\nperformed=200\n");
    write_file(wd + "/omnium.ledger", fmt("%lld run created 2026-09-05 05:00:00 seed 1\n%lld trial 1 build area saved-new SIGSEGV|a|b|c|d\n"
                                          "%lld reduce 1 done rc=0\n%lld reduce 2 no result rc=1\n",
                                          (long long)now_s() - 60, (long long)now_s() - 40, (long long)now_s() - 30, (long long)now_s() - 20));
    write_file(g_paths.human_queue + "/O222222_bug1.report", "Title: a made-up report\nUID: SIGSEGV|a|b|c|d\n-----\nbody\n");
    write_file(g_paths.human_queue + "/O222222_bug1.ok", "");
    st_check(quiet_call(cmd_tui, {"--frame"}) == 0, "the view picks the newest run when none is named");
    st_check(quiet_call(cmd_tui, {"O222222", "--plain"}) == 0, "the view follows a finished run and stops");
    int trc = -1;
    string frame = call_output(cmd_tui, {"O222222", "--frame"}, &trc);
    st_check(trc == 0, "the view draws that run with its inbox item");
    st_check(frame.find("reduced 1 ") != string::npos, "and counts the reduction that gave a testcase, not the one that did not");
    {
      // a pid file whose number now belongs to another program: not a live run
      pid_t sp = spawn_program({"sleep", "30"}, "/dev/null", "", true);
      for (int i = 0; i < 100 && !starts_with(proc_cmdline(sp), "sleep"); i++) usleep(20000);   // until it is sleep, not the fork of this omnium
      write_file(wd + "/omnium.pid", std::to_string((int)sp) + "\n");
      string so = call_output(cmd_status, {wd});
      st_check(sp > 0 && so.find("O222222  finished") != string::npos && so.find("  pid ") == string::npos, "status takes a pid that is not an omnium for a finished run");
      kill_group(sp, SIGKILL);
      wait_pid(sp, 5000);
      fs::remove(wd + "/omnium.pid");
    }
    g_cfg.data_dir = save_data;
    g_paths.human_queue = save_hq;
  }
  // the builds verb: the read-only shapes, then a write against a copy of the builds file
  {
    // what it names is a build of the kind asked for, or nothing on a box with none
    auto newest = [&](const Args& args, bool dbg, Flavour fl, const string& what) {
      int brc = -1;
      string path = trim(call_output(cmd_builds, args, &brc));
      Basedir b;
      bool ok = brc == 0 ? basedir_probe(path, b) && b.dbg == dbg && b.flavour == fl : brc == 1 && path.empty();
      st_check(ok, what);
      return b;
    };
    newest({"newest", "dbg"}, true, Flavour::Plain, "builds newest names a debug build");
    Basedir cs = newest({"newest", "opt", "cs"}, false, Flavour::Plain, "builds newest narrows to optimised builds");
    st_check(cs.path.empty() || !cs.es, "and to Community ones");
    newest({"newest", "ubasan-dbg"}, true, Flavour::UBASAN, "builds newest reaches the sanitizer builds");
    st_check(quiet_call(cmd_builds, {"newest", "opt", "no-such-filter-at-all"}) == 1, "builds newest says no when nothing matches");
    string save_bf = g_paths.builds_file, save_td = g_cfg.test_dir;
    g_paths.builds_file = tmp + "/omnium.builds";
    // a test directory of its own: no build of the box can supersede the entry and unlist it
    g_cfg.test_dir = tmp + "/test";
    Registry r;
    Basedir nb;
    basedir_parse_name("MD180826-mariadb-13.1.0-linux-x86_64-opt", nb);
    nb.name = "MD180826-mariadb-13.1.0-linux-x86_64-opt";
    nb.path = g_cfg.test_dir + "/" + nb.name;
    fs::create_directories(nb.path);
    BuildEntry ne;
    if (entry_from_basedir(nb, ne)) {
      ne.test = ne.report = true;
      r.entries.push_back(ne);
      registry_save(r, g_paths.builds_file);
      st_check(quiet_call(cmd_builds, {"set", ne.name, "test=no", "report=yes"}) == 0, "builds set writes the marks");
      st_check(read_file(g_paths.builds_file).find(ne.name) != string::npos, "the build is still listed");
      st_check(quiet_call(cmd_builds, {"set", "no-such-build", "test=no"}) == 1, "builds set says no for a build that is not listed");
      st_check(quiet_call(cmd_builds, {"set", ne.name, "nonsense=1"}) == 2, "builds set refuses a mark it does not know");
      st_check(quiet_call(cmd_builds, {"set"}) == 2, "builds set says what it takes");
    }
    g_paths.builds_file = save_bf;
    g_cfg.test_dir = save_td;
  }
  // the matrix table itself: rows of several builds, sorted and formatted, without running one
  {
    MatrixResult m;
    m.options = "--sql_mode=";
    const char* names[] = {"MD180826-mariadb-13.1.0-linux-x86_64-opt", "MD180826-mariadb-13.1.0-linux-x86_64-dbg",
                           "EMD180826-mariadb-12.3.2-1-linux-x86_64-dbg", "UBASAN_MD180826-mariadb-13.1.0-linux-x86_64-dbg",
                           "MS150826-mysql-8.0.36-linux-x86_64-opt"};
    for (const char* n : names) {
      MatrixRow row;
      basedir_parse_name(n, row.b);
      row.b.name = n;
      row.b.path = string("/test/") + n;
      row.uid = starts_with(row.b.name, "UBASAN") ? "ASAN|heap-use-after-free|a|b|c|d" : "No bug found";
      row.crashed = row.uid != "No bug found";
      row.san = row.crashed;
      row.performed = 100;
      row.failed = 2;
      m.rows.push_back(row);
    }
    std::sort(m.rows.begin(), m.rows.end(), [](const MatrixRow& a, const MatrixRow& c) { return row_less(a, c); });
    string t = matrix_format(m);
    st_check(t.find("Bug Detection Matrix") != string::npos, "the matrix block has its title");
    st_check(t.find("UniqueID observed") != string::npos, "and the UniqueID column");
    st_check(t.find("12.3") != string::npos && t.find("8.0") != string::npos, "every build has a row");
    st_check(t.find("UBASAN") != string::npos && t.find("Flavour") != string::npos, "the flavour column appears when a sanitizer build is in");
    size_t es_at = t.find("ES "), cs_at = t.find("CS ");
    st_check(cs_at != string::npos && es_at != string::npos && cs_at < es_at, "Community rows come before Enterprise");
    st_check(t.find("MS ") != string::npos && t.find("ES ") < t.find("MS "), "MySQL comes after the MariaDB rows");
    MatrixResult empty;
    st_check(!matrix_format(empty).empty(), "an empty matrix still says so");
    // which testcase a report carries: the prettified one while the trial's own build shows the bug on it, else the reduced one when that does
    {
      auto row_of = [](const char* name, const char* uid) {
        MatrixRow r;
        basedir_parse_name(name, r.b);
        r.b.name = name;
        r.b.path = string("/test/") + name;
        r.uid = uid;
        return r;
      };
      const char* own = "MD180826-mariadb-13.1.0-linux-x86_64-dbg";
      const char* other = "MD180826-mariadb-12.3.2-linux-x86_64-opt";
      const string own_path = string("/test/") + own;
      auto matrix_of = [&](const char* own_uid, const char* other_uid) {
        MatrixResult x;
        x.rows.push_back(row_of(own, own_uid));
        x.rows.push_back(row_of(other, other_uid));
        return x;
      };
      st_check(!matrix_row_shows_bug(row_of(own, "No bug found")) && !matrix_row_shows_bug(row_of(own, "No result (server did not start)")) &&
                   matrix_row_shows_bug(row_of(own, "SIGSEGV|a|b|c|d")),
               "matrix_row_shows_bug: a UID is a bug, and no bug and no result are not");
      int calls = 0;
      MatrixResult shown = matrix_of("SIGSEGV|x|y|z|w", "No bug found");
      st_check(!report_prefer_reduced(shown, own_path, [&](MatrixResult&) { calls++; return true; }) && calls == 0,
               "a testcase that still shows the bug on the trial's build is kept, and the reduced one is not replayed");
      MatrixResult lost = matrix_of("No bug found", "No bug found");
      bool took = report_prefer_reduced(lost, own_path, [&](MatrixResult& out) { out = matrix_of("SIGSEGV|x|y|z|w", "SIGSEGV|x|y|z|w"); return true; });
      st_check(took && matrix_row_shows_bug(lost.rows[0]) && matrix_row_shows_bug(lost.rows[1]),
               "a testcase that lost the bug on the trial's build gives way to the reduced one, and its matrix is the report's");
      MatrixResult elsewhere = matrix_of("No bug found", "SIGSEGV|other|build|only|1");
      took = report_prefer_reduced(elsewhere, own_path, [&](MatrixResult& out) { out = matrix_of("No bug found", "No bug found"); return true; });
      st_check(!took && elsewhere.rows[1].uid == "SIGSEGV|other|build|only|1",
               "when the reduced one shows nothing on the trial's build either, the matrix stays the prettified one's (another build's bug does not count)");
      MatrixResult failed_run = matrix_of("No bug found", "No bug found");
      took = report_prefer_reduced(failed_run, own_path, [&](MatrixResult&) { return false; });
      st_check(!took && !matrix_row_shows_bug(failed_run.rows[0]), "a replay of the reduced testcase that cannot run changes nothing");
      MatrixResult anywhere = matrix_of("No bug found", "SIGSEGV|other|build|only|1");
      calls = 0;
      st_check(!report_prefer_reduced(anywhere, "", [&](MatrixResult&) { calls++; return true; }) && calls == 0,
               "with no build of the trial known, a bug on any build keeps the testcase");
    }
    // the build list: names given, a name that is not there, and the report set from the registry
    vector<Basedir> set;
    string merr;
    st_check(!matrix_builds({"no-such-build-at-all"}, set, &merr), "matrix_builds says no for a build that is not there");
    st_check(!merr.empty(), "and says which");
    set.clear();
    Registry reg;
    size_t marked = 0;
    if (registry_load(reg)) for (auto& e : reg.entries) if (e.report) marked++;
    bool mb = matrix_builds({}, set, &merr);
    st_check(mb ? !set.empty() && set.size() <= marked : set.empty(), "matrix_builds with no name takes the report=yes builds of the registry");
  }
  // the known-bug verbs, on copies
  {
    string save_kb = g_paths.known_bugs, save_kbsan = g_paths.known_bugs_san;
    g_paths.known_bugs = tmp + "/kb.strings";
    g_paths.known_bugs_san = tmp + "/kb.strings.SAN";
    const char* skeleton = "# a copy for the selftest\n##### CURRENT BUGS (Search key: Mac) #####\n\n##### FIXED BUGS #####\n";
    write_file(g_paths.known_bugs, skeleton);
    write_file(g_paths.known_bugs_san, skeleton);
    st_check(quiet_call(cmd_kb, {"add", "SIGABRT|f1|f2|f3|f4", "MDEV-555"}) == 0, "kb add takes a first UID");
    st_check(quiet_call(cmd_kb, {"fixed", "MDEV-555"}) == 0, "and it can be marked fixed");
    st_check(quiet_call(cmd_kb, {"add", "SIGABRT|f1|q1|q2|q3", "MDEV-556"}) == 0, "a UID sharing its first frame is added too");
    st_check(quiet_call(cmd_kb, {"add", "SIGSEGV|a|b|c|d", "MDEV-12345"}) == 0, "kb add puts a UID in the list");
    st_check(read_file(g_paths.known_bugs).find("MDEV-12345") != string::npos, "and the line is there");
    st_check(quiet_call(cmd_kb, {"add", "SIGSEGV|a|b|c|d", "MDEV-12345"}) == 1, "kb add refuses the same UID twice");
    st_check(quiet_call(cmd_kb, {"add", "SIGSEGV|q|w|e|r", "not-a-key"}) == 2, "kb add refuses something that is not a ticket key");
    st_check(quiet_call(cmd_kba, {"add", "ASAN|heap-use-after-free|a|b|c", "MDEV-999"}) == 0, "kba add writes to the sanitizer list");
    st_check(quiet_call(cmd_kb, {"search", "SIGSEGV|a|b|c|d"}) == 0, "kb search finds it");
    st_check(quiet_call(cmd_kb, {"fixed", "MDEV-12345"}) == 0, "kb fixed marks it fixed");
    st_check(read_file(g_paths.known_bugs).find("## Fixed") != string::npos, "and the marker is written");
    // the four verdicts a search can come back with, and the wording of each
    {
      string only_fixed = "SIGABRT|f1|f2|f3|f4";
      KbMatch fm = kb_search(only_fixed);
      st_check(kb_verdict(fm) == KbVerdict::FixedOnly, "a UID that is only in the list as fixed reads as fixed-only");
      st_check(kb_verdict_text(only_fixed, fm).find("PREVIOUSLY FIXED") != string::npos, "and the wording says so");
      st_check(quiet_call(cmd_kb, {"add", only_fixed, "MDEV-777"}) == 0, "the same UID is added again as open");
      KbMatch bm = kb_search(only_fixed);
      st_check(kb_verdict(bm) == KbVerdict::KnownAndFixed, "with both lines it reads as known and fixed");
      st_check(kb_verdict_text(only_fixed, bm).find("PREVIOUSLY FIXED") != string::npos, "and the wording names both");
      string other = "SIGABRT|f1|zz|yy|xx";
      KbMatch pm = kb_search(other);
      st_check(kb_verdict(pm) == KbVerdict::Partial, "a UID sharing only the first frame reads as a partial match");
      st_check(kb_verdict_text(other, pm).find("PARTIAL MATCH") != string::npos, "and the wording says which frame matched");
      KbMatch nm = kb_search("SIGSEGV|nothing|like|this|at_all");
      st_check(kb_verdict(nm) == KbVerdict::NotFound, "a UID nobody has seen reads as not found");
      st_check(kb_verdict_text("SIGSEGV|nothing|like|this|at_all", nm).find("NOT FOUND") != string::npos, "and the wording says so");
    }
    // the version banner verbs, on a stand-in build
    {
      string vb = tmp + "/MD030926-mariadb-13.1.0-linux-x86_64-opt";
      mkdirs(vb + "/bin");
      write_file(vb + "/bin/mariadbd", "#!/bin/sh\necho \"mariadbd  Ver 13.1.0-MariaDB for Linux (x86_64)\"\nexit 0\n");
      write_file(vb + "/bin/mariadb", "#!/bin/sh\nexit 0\n");
      for (const char* f : {"/bin/mariadbd", "/bin/mariadb"}) chmod((vb + f).c_str(), 0755);
      st_check(quiet_call(cmd_myver, {vb}) == 0, "myver prints the banner of a build");
      st_check(quiet_call(cmd_myver, {vb, "--short"}) == 0, "myver --short prints the short name");
      st_check(quiet_call(cmd_myver, {tmp + "/no_such_build"}) == 1, "myver says no for a build that is not there");
    }
    g_paths.known_bugs = save_kb;
    g_paths.known_bugs_san = save_kbsan;
  }
  remove_tree(tmp);
}

// The filing path end to end, against a stand-in Jira on 127.0.0.1 (--role jirastub). Nothing here
// reaches the real Jira: the base URL points at the stub for the length of this check, and the
// known-bug lists and BUGS/ are temporary copies, so the real ones are untouched.
static void st_jira_stub() {
  string tmp = st_tmp() + "/jira";
  mkdirs(tmp);
  string portfile = tmp + "/port";
  Child c;
  if (!spawn_role(c, "jirastub", {portfile}, tmp + "/stub.log")) { st_check(false, "the Jira stub starts"); return; }
  string port;
  for (int i = 0; i < 200 && port.empty(); i++) { port = trim(read_file(portfile)); if (port.empty()) usleep(25000); }
  st_check(!port.empty(), "the Jira stub took a port");
  if (port.empty()) { kill_group(c.pid, SIGKILL); child_reap(c, 5000); child_close(c); return; }
  string save_url = g_cfg.jira_url, save_kb = g_paths.known_bugs, save_kbsan = g_paths.known_bugs_san, save_bugs = g_paths.bugs_dir;
  g_cfg.jira_url = "http://127.0.0.1:" + port;
  g_paths.known_bugs = tmp + "/known_bugs.strings";
  g_paths.known_bugs_san = tmp + "/known_bugs.strings.SAN";
  g_paths.bugs_dir = tmp + "/BUGS";
  // the two lists the filing writes to: the header line kb_add looks for, then a section end
  const char* kb_skeleton = "# omnium selftest copy, never the real list\n##### CURRENT BUGS (Search key: Mac) #####\n\n##### FIXED BUGS #####\n";
  write_file(g_paths.known_bugs, kb_skeleton);
  write_file(g_paths.known_bugs_san, kb_skeleton);
  mkdirs(g_paths.bugs_dir);
  {
    string err, me;
    st_check(jira_whoami(me, &err), "jira whoami against the stub");
    st_eq(me, "omnium-stub", "whoami gives the stub name");
    vector<string> versions, components;
    st_check(jira_project_names("MDEV", "versions", versions, &err), "the version list comes back");
    st_check(std::find(versions.begin(), versions.end(), "13.1") != versions.end(), "13.1 is in the version list");
    st_check(jira_project_names("MDEV", "components", components, &err), "the component list comes back");
    st_check(std::find(components.begin(), components.end(), "Server") != components.end(), "Server is in the component list");
    vector<JiraHit> hits;
    st_check(jira_search_url(g_cfg.jira_url + "/issues/?jql=text%20~%20%22x%22", hits, &err), "a JQL search runs");
    st_check(hits.size() == 1 && hits[0].key == "MDEV-11111", "the search gives the stub issue");
    st_check(!jira_search_url("not a search url", hits, &err), "a URL with no jql is refused");
    JiraFields f;
    f.project = "MDEV";
    f.issuetype = "Bug";
    f.summary = "omnium selftest: never filed anywhere real";
    f.description = "body";
    f.affects = {"13.1"};
    string key;
    st_check(jira_create(f, key, &err), "jira_create against the stub");
    st_eq(key, "MDEV-99999", "the stub gives a key back");
    st_check(jira_comment(key, "a comment", &err), "jira_comment");
    st_check(jira_link(key, "MDEV-11111", "Relates", &err), "jira_link");
    st_check(quiet_call(cmd_jira, {"whoami"}) == 0, "jira whoami verb");
    st_check(quiet_call(cmd_jira, {"versions", "MDEV"}) == 0, "jira versions verb");
    st_check(quiet_call(cmd_jira, {"components", "MDEV"}) == 0, "jira components verb");
    st_check(quiet_call(cmd_jira, {"search", "SIGABRT|a|b|c|d"}) == 0, "jira search verb");
    st_check(quiet_call(cmd_jira, {"nonsense"}) != 0, "jira refuses an unknown subcommand");
  }
  // a report through the inbox: approved, then filed at the stub, then registered
  {
    string uid = "arg2_int >= 0|SIGABRT|Item_func_additive_op::result_precision|Item_num_op::fix_length_and_dec_decimal|x|y";
    string td = fixture_trial(tmp, 1, fixture_errlog(), uid);
    string inbox = tmp + "/inbox";
    st_check(quiet_call(cmd_report, {dirname_of(td), "1", "--no-matrix", "--no-mtr", "--out", inbox}) == 0, "the report for the filing check");
    string item = inbox + "/O999999_bug1";
    st_check(quiet_call(cmd_inbox, {"--dir", inbox, "--process"}) == 0, "inbox --process leaves an item with no .ok alone");
    st_check(!file_exists(item + ".filed"), "nothing is filed without a .ok file");
    write_file(item + ".ok", "");
    int frc = -1;
    string fout = call_output(cmd_inbox, {"--dir", inbox, "--process"}, &frc);
    st_check(frc == 0, "inbox --process files the approved item");
    st_eq(trim(read_file(item + ".filed")).substr(0, 10), "MDEV-99999", "the .filed file holds the key");
    st_check(fout.find("git add " + kb_file_for(uid) + " " + g_paths.bugs_dir + "/MDEV-99999.sql") != string::npos,
             "the filing names the two changed files in a git add line [" + tail_lines(fout, 1) + "]");
    st_check(!file_exists(item + ".ok"), "the .ok file is taken away once filed");
    st_check(read_file(g_paths.known_bugs).find("MDEV-99999") != string::npos, "the UID went in the known-bug list");
    st_check(file_exists(g_paths.bugs_dir + "/MDEV-99999.sql"), "the testcase went in BUGS/");
    st_check(quiet_call(cmd_inbox, {"--dir", inbox, "--process"}) == 0, "a filed item is not filed twice");
    st_check(quiet_call(cmd_inbox, {"--dir", inbox}) == 0, "inbox lists the filed item");
    // a memory-safety report is never filed automatically
    string sd = fixture_trial(tmp + "/san", 2, fixture_san_log(), "ASAN|heap-use-after-free|Item_func_case::val_int|a|b|c");
    string inbox2 = tmp + "/inbox2";
    if (quiet_call(cmd_report, {dirname_of(sd), "2", "--no-matrix", "--no-mtr", "--out", inbox2}) == 0) {
      string item2 = inbox2 + "/O999999_bug2";
      write_file(item2 + ".ok", "");
      quiet_call(cmd_inbox, {"--dir", inbox2, "--process"});
      st_check(!file_exists(item2 + ".filed"), "a memory-safety report is held back");
      st_check(read_file(item2 + ".error").find("held") != string::npos, "the hold says why");
    }
    // and so is an MSAN uninitialised read (Q401)
    string md = fixture_trial(tmp + "/msan", 3,
                              "==4321==WARNING: MemorySanitizer: use-of-uninitialized-value\n"
                              "    #0 0x55d123 in Field_long::val_int() /test/13.1/sql/field.cc:4000:10\n"
                              "    #1 0x55d456 in Item_field::val_int() /test/13.1/sql/item.cc:3000:24\n"
                              "    #2 0x55d789 in sub_select(JOIN*, st_join_table*, bool) /test/13.1/sql/sql_select.cc:24716:12\n"
                              "SUMMARY: MemorySanitizer: use-of-uninitialized-value /test/13.1/sql/field.cc:4000:10 in Field_long::val_int()\n",
                              "MSAN|use-of-uninitialized-value|Field_long::val_int|Item_field::val_int|sub_select");
    string inbox4 = tmp + "/inbox4";
    st_check(quiet_call(cmd_report, {dirname_of(md), "3", "--no-matrix", "--no-mtr", "--out", inbox4}) == 0, "the report of an MSAN trial");
    write_file(inbox4 + "/O999999_bug3.ok", "");
    quiet_call(cmd_inbox, {"--dir", inbox4, "--process"});
    st_check(!file_exists(inbox4 + "/O999999_bug3.filed"), "an MSAN uninitialised read is held back as well");
  }
  // a report written while the stub answers: the Jira hits land in .possible_dup, the AI queue gets
  // the item, and the mail path is walked with an address that cannot be delivered
  {
    string save_ai = g_paths.ai_queue, save_email = g_cfg.email;
    g_paths.ai_queue = tmp + "/AI-queue";
    mkdirs(g_paths.ai_queue);
    g_cfg.email = "nobody@no-such-domain.invalid";
    string duid = "arg2_int >= 0|SIGABRT|dupfix_marker|Item_num_op::fix_length_and_dec_decimal|x|y";
    string dtd = fixture_trial(tmp + "/dup", 5, fixture_errlog(), duid);
    string inbox3 = tmp + "/inbox3";
    st_check(quiet_call(cmd_report, {dirname_of(dtd), "5", "--no-matrix", "--no-mtr", "--out", inbox3}) == 0, "a report while the stub answers");
    st_check(file_exists(inbox3 + "/O999999_bug5.possible_dup"), "the Jira hits go in the .possible_dup file");
    st_check(read_file(inbox3 + "/O999999_bug5.possible_dup").find("MDEV-22222") != string::npos, "and name the issue the search found");
    st_check(file_exists(inbox3 + "/O999999_bug5.preview"), "the payload is written beside the report");
    st_check(dir_exists(g_paths.ai_queue + "/O999999_bug5"), "the AI queue holds what the item still needs");
    string info = read_file(g_paths.ai_queue + "/O999999_bug5/info.txt");
    st_check(info.find("Closed/Fixed") != string::npos, "the queue says a Jira hit is closed as fixed while the bug reproduces");
    g_paths.ai_queue = save_ai;
    g_cfg.email = save_email;
  }
  g_cfg.jira_url = save_url;
  g_paths.known_bugs = save_kb;
  g_paths.known_bugs_san = save_kbsan;
  g_paths.bugs_dir = save_bugs;
  st_stub_stop(c);
  remove_tree(tmp);
}

// --role jirastub <portfile>: a stand-in Jira on 127.0.0.1, used by the selftest only. It answers
// the REST calls omnium makes with fixed replies, so the whole filing path can run without the real
// Jira ever being contacted. The port it took goes in <portfile>; it serves until it is killed.
// selftest only: a stand-in mail server on 127.0.0.1, so the send path is walked without a
// message ever leaving the box. It serves one message and writes it where the check can read it.
int role_smtpstub(const Args& a) {
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) return 1;
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = 0;
  socklen_t sl = sizeof sa;
  if (bind(ls, (sockaddr*)&sa, sl) != 0 || getsockname(ls, (sockaddr*)&sa, &sl) != 0 || listen(ls, 4) != 0) { close(ls); return 1; }
  if (!a.empty()) write_file(a[0], std::to_string(ntohs(sa.sin_port)) + "\n");
  int c = accept(ls, nullptr, nullptr);
  if (c < 0) { close(ls); return 1; }
  auto say = [&](const char* line) { string t = string(line) + "\r\n"; ssize_t w = write(c, t.data(), t.size()); (void)w; };
  say("220 omnium-stub ESMTP");
  string line, message;
  bool in_data = false;
  char ch;
  while (read(c, &ch, 1) == 1) {
    if (ch != '\n') { if (ch != '\r') line += ch; continue; }
    if (in_data) {
      if (line == ".") { in_data = false; say("250 OK message stored"); line.clear(); continue; }
      message += line + "\n";
      line.clear();
      continue;
    }
    string cmd = upper(line.substr(0, 4));
    if (cmd == "EHLO") { say("250-omnium-stub"); say("250 SIZE 10485760"); }
    else if (cmd == "HELO") say("250 omnium-stub");
    else if (cmd == "MAIL" || cmd == "RCPT" || cmd == "RSET" || cmd == "NOOP") say("250 OK");
    else if (cmd == "DATA") { in_data = true; say("354 go ahead"); }
    else if (cmd == "QUIT") { say("221 bye"); break; }
    else say("502 not here");
    line.clear();
  }
  close(c);
  close(ls);
  if (a.size() > 1) write_file(a[1], message);
  return 0;
}

int role_jirastub(const Args& a) {
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls < 0) return 1;
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port = 0;
  socklen_t sl = sizeof sa;
  if (bind(ls, (sockaddr*)&sa, sl) != 0 || getsockname(ls, (sockaddr*)&sa, &sl) != 0 || listen(ls, 16) != 0) { close(ls); return 1; }
  if (!a.empty()) write_file(a[0], std::to_string(ntohs(sa.sin_port)) + "\n");
  for (;;) {
    // the listening socket is polled rather than blocked on, so a "stop" on the command channel
    // ends the stub in the ordinary way
    struct pollfd lp{ls, POLLIN, 0};
    int pr = poll(&lp, 1, 200);
    if (pr <= 0) { if (role_stop_requested()) break; continue; }
    int c = accept(ls, nullptr, nullptr);
    if (c < 0) { if (errno == EINTR) continue; break; }
    string req;
    char buf[4096];
    ssize_t n;
    long want = -1;
    size_t hdr_end = string::npos;
    while ((n = read(c, buf, sizeof buf)) > 0) {
      req.append(buf, (size_t)n);
      if (hdr_end == string::npos) hdr_end = req.find("\r\n\r\n");
      if (hdr_end == string::npos) continue;
      if (want < 0) {
        want = 0;
        string h = lower(req.substr(0, hdr_end));
        size_t p = h.find("content-length:");
        if (p != string::npos) want = to_long(trim(req.substr(p + 15, h.find('\r', p) - p - 15)), 0);
      }
      if (req.size() >= hdr_end + 4 + (size_t)want) break;
    }
    string first = req.substr(0, req.find('\r'));
    vector<string> w = split(first, ' ');
    string method = w.size() > 0 ? w[0] : "", path = w.size() > 1 ? w[1] : "";
    string body = "{}";
    int code = 200;
    const char* status = "OK";
    if (path.find("/myself") != string::npos) body = "{\"name\":\"omnium-stub\",\"displayName\":\"omnium stub\"}";
    else if (path.find("/search") != string::npos)
      body = path.find("dupfix") != string::npos
               ? "{\"total\":1,\"issues\":[{\"key\":\"MDEV-22222\",\"fields\":{\"summary\":\"a stub issue that is closed\",\"status\":{\"name\":\"Closed\"},\"resolution\":{\"name\":\"Fixed\"}}}]}"
               : "{\"total\":1,\"issues\":[{\"key\":\"MDEV-11111\",\"fields\":{\"summary\":\"a stub issue\",\"status\":{\"name\":\"Open\"},\"resolution\":null}}]}";
    else if (ends_with(path, "/versions")) body = "[{\"name\":\"13.1\"},{\"name\":\"13.0\"},{\"name\":\"12.3\"},{\"name\":\"11.4(EOL)\"}]";
    else if (ends_with(path, "/components")) body = "[{\"name\":\"Optimizer\"},{\"name\":\"Server\"},{\"name\":\"Storage Engine - InnoDB\"}]";
    else if (method == "POST" && ends_with(path, "/comment")) { body = "{\"id\":\"10001\"}"; code = 201; status = "Created"; }
    else if (method == "POST" && ends_with(path, "/issueLink")) { body = "{}"; code = 201; status = "Created"; }
    else if (method == "POST" && ends_with(path, "/issue")) { body = "{\"id\":\"1\",\"key\":\"MDEV-99999\"}"; code = 201; status = "Created"; }
    else { body = "{\"errorMessages\":[\"stub: no route for " + path + "\"]}"; code = 404; status = "Not Found"; }
    string resp = fmt("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, status, body.size()) + body;
    size_t off = 0;
    while (off < resp.size()) {
      ssize_t wn = write(c, resp.data() + off, resp.size() - off);
      if (wn <= 0) break;
      off += (size_t)wn;
    }
    close(c);
  }
  close(ls);
  return 0;
}
