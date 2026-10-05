// Created by Roel Van de Paar, MariaDB
// matrix.cpp - the Bug Detection Matrix: one testcase replayed on every report build of the
// registry (or the builds named), each row the UID that build showed. The replay is the
// framework's: a fresh datadir, the server with the options the testcase names, the statements in
// order through the in-process client, then the UID chain on the error log and the core. A row
// reads "No bug found" when the chain finds nothing, as findbug's line script does.
#include "verbs.h"
#include <regex>
#include <sys/wait.h>

namespace {
const int SHUTDOWN_SECONDS = 25;

// the first line of a testcase can carry the server options it needs
string options_header(const string& sql) {
  size_t nl = sql.find('\n');
  string first = trim(sql.substr(0, nl));
  static const string tag = "mysqld options required for replay:";
  if (first.empty() || first[0] != '#') return "";
  size_t k = lower(first).find(tag);
  return k == string::npos ? "" : trim(first.substr(k + tag.size()));
}
int vendor_rank(const Basedir& b) {
  string v = b.vendor_str();
  return v == "CS" ? 0 : v == "ES" ? 1 : v == "MS" ? 2 : v == "PS" ? 3 : 4;
}
vector<long> version_parts(const string& v) {
  vector<long> out;
  string cur;
  for (char c : v + ".") {
    if (isdigit((unsigned char)c)) cur += c;
    else { if (!cur.empty()) out.push_back(to_long(cur)); cur.clear(); }
  }
  return out;
}
// one build: start, replay, stop, read the UID
void sweep_one(const string& sql_file, size_t statements, const string& options, const string& root, const string& templates,
               int timeout_s, MatrixRow& row) {
  const Basedir& b = row.b;
  bool san = b.flavour == Flavour::UBASAN || b.flavour == Flavour::MSAN || b.flavour == Flavour::TSAN || b.flavour == Flavour::VAL;
  Instance inst;
  inst.bd = &b;
  inst.set_paths(root);
  mkdirs(root);
  write_file(root + "/BASEDIR", b.path + "\n");
  for (auto& o : split_ws(options)) inst.extra.push_back(o);
  string tpl = template_for(b, myinit_from(options), templates);   // the page size of the testcase's header goes into the template too
  if (tpl.empty()) { row.note = "no datadir template"; row.uid = "No result (datadir init failed)"; return; }
  if (!inst.start_fresh(tpl, san ? 240 : 60)) {
    row.note = "server did not start: " + inst.start_note;
    // a start failure under the testcase's options is still an observation of the log
    UidResult r; UidOptions o; o.wait_core = false;
    uid_for_dir(root, r, o);
    // a crash at startup is an observation; the noise of a server that is up but was never reached (aborted connections) is not
    static const std::regex weak("^(INNODB_WARNING|SLAVE_WARNING|WARNING_ABORTED|WARNING|INNODB_NOTE|UNTYPED)\\|");
    row.uid = r.uid.empty() || starts_with(r.uid, "Assert:") || (kTakeFixes && std::regex_search(r.uid, weak)) ? "No result (server did not start)" : r.uid;
    inst.kill_hard();
    return;
  }
  ClientParams p;
  p.ep = inst.endpoint(); p.logdir = root; p.threads = 1; p.shuffle = false;
  p.queries_per_thread = statements; p.log_all = true; p.seed = 1;
  std::atomic<bool> stop{false};
  ClientResult res;
  string err;
  std::thread client([&] { client_run(p, sql_file, stop, res, &err); });
  double deadline = now_ms() + timeout_s * 1000.0;
  bool timed_out = false;
  {
    // the client returns when the statements are done or the server is gone; a hang is an outcome too
    std::mutex m; std::condition_variable cv; bool done = false;
    std::thread waiter([&] { client.join(); std::lock_guard<std::mutex> lk(m); done = true; cv.notify_all(); });
    std::unique_lock<std::mutex> lk(m);
    while (!done && now_ms() < deadline) cv.wait_for(lk, std::chrono::milliseconds(500));
    if (!done) {
      timed_out = true;
      stop = true;
      lk.unlock();
      client_kill_connections(p);
      inst.kill_hard();
      waiter.join();
    } else {
      lk.unlock();
      waiter.join();
    }
  }
  row.performed = res.performed;
  row.failed = res.failed;
  if (res.performed == 0 && res.failed == 0) {
    // the connect error is the first line of the trace
    string first = read_file(root + "/" + p.name + "_thread-0.sql");
    first = trim(first.substr(0, first.find('\n')));
    row.note = "client ran nothing: " + (!first.empty() ? first : err.empty() ? fmt("connect_failed=%d lost=%d", res.connect_failed, res.lost_connection) : err);
  }
  sleep(san ? 5 : 2);
  if (!timed_out && inst.alive() && !inst.shutdown(SHUTDOWN_SECONDS)) {
    // the framework waits a moment for a shutdown core before it kills
    for (int i = 0; i < 5 && !inst.has_core() && !inst.has_dump(); i++) sleep(1);
    if (!inst.has_core() && !inst.has_dump()) row.note = "shutdown did not finish in time";
  }
  inst.kill_hard();
  UidResult r; UidOptions o; o.wait_core = false;
  uid_for_dir(root, r, o);
  string uid = trim(r.uid);
  if (uid.empty() || starts_with(uid, "Assert:")) uid = "No bug found";
  if (uid.find("MARIADBD_ERROR|mariadbd: caching_sha2_password: failed to read private_key.pem: 2") != string::npos) uid = "No bug found";
  // a Windows release server can die with no banner in its log and no minidump (a failed /GS stack cookie check, a
  // __fastfail): the way it ended is all there is, and "No bug found" would hide a crash
  bool silent = uid == "No bug found" && inst.silent_death();
  if (silent) { uid = inst.silent_death_uid(); row.note = "the server ended on its own with no crash banner and no minidump"; }
  if (timed_out && uid == "No bug found") uid = "No result (hang: the client timed out)";
  row.uid = uid;
  row.crashed = inst.has_core() || inst.has_dump() || silent || r.san;
  row.san = r.san;
  if (row.crashed || (uid != "No bug found" && !starts_with(uid, "No result"))) {
    row.stack = stack_text(root, basedir_banner_title(b), &err);
    row.errlog = read_file(inst.errlog);
  }
}
}  // namespace

bool row_less(const MatrixRow& a, const MatrixRow& c) {
  int va = vendor_rank(a.b), vc = vendor_rank(c.b);
  if (va != vc) return va < vc;
  vector<long> pa = version_parts(a.b.version), pc = version_parts(c.b.version);
  if (pa != pc) return pa < pc;
  if (a.b.flavour != c.b.flavour) return (int)a.b.flavour < (int)c.b.flavour;
  return a.b.dbg && !c.b.dbg;                                 // dbg before opt
}

bool matrix_builds(const vector<string>& names, vector<Basedir>& out, string* err) {
  out.clear();
  if (names.empty()) {
    Registry r;
    if (!registry_load(r)) { *err = "no registry at " + g_paths.builds_file + " (omnium builds makes one)"; return false; }
    for (auto& e : r.entries) {
      if (!e.report) continue;
      Basedir b;
      if (basedir_probe(e.path(), b)) out.push_back(b);
    }
    if (out.empty()) { *err = "no report=yes build in " + g_paths.builds_file; return false; }
    return true;
  }
  for (auto& n : names) {
    Basedir b;
    string e;
    if (!basedir_from_arg(n, b, &e)) { *err = e; return false; }
    bool dup = false;
    for (auto& x : out) if (x.path == b.path) dup = true;
    if (!dup) out.push_back(b);
  }
  return true;
}

bool matrix_run(const string& sql_file, const vector<Basedir>& builds, const string& options_arg, int slots, MatrixResult& out, string* err) {
  string sql = read_file(sql_file);
  if (sql.empty()) { *err = "no SQL in " + sql_file; return false; }
  size_t statements = 0;
  for (auto& l : split_lines(sql)) if (!trim(l).empty()) statements++;
  out.options = trim(options_header(sql) + " " + options_arg);
  out.rows.clear();
  for (auto& b : builds) { MatrixRow r; r.b = b; out.rows.push_back(r); }
  string root = g_cfg.shm_dir + fmt("/Omatrix%d", (int)getpid());
  remove_tree(root);
  mkdirs(root + "/templates");
  Child hold;
  spawn_role(hold, "hold", {root}, "/dev/null");
  if (slots < 1) slots = 1;
  std::atomic<size_t> next{0};
  vector<std::thread> pool;
  int timeout_s = std::max(300, (int)statements / 10 + 300);
  for (int i = 0; i < std::min<int>(slots, (int)out.rows.size()); i++) {
    pool.emplace_back([&] {
      for (;;) {
        size_t k = next.fetch_add(1);
        if (k >= out.rows.size()) return;
        MatrixRow& row = out.rows[k];
        bool san = row.b.flavour != Flavour::Plain;
        sweep_one(sql_file, statements, out.options, root + "/" + row.b.name, root + "/templates", san ? timeout_s * 2 : timeout_s, row);
        remove_tree(root + "/" + row.b.name);
      }
    });
  }
  for (auto& t : pool) t.join();
  kill_group(hold.pid, SIGKILL);
  child_reap(hold, 2000);
  remove_tree(root);
  std::sort(out.rows.begin(), out.rows.end(), row_less);
  return true;
}

// the b layout: findbug's columns, widths from the content as corlogic aligns them
string matrix_format(const MatrixResult& m) {
  bool flavours = false;
  for (auto& r : m.rows) if (r.b.flavour != Flavour::Plain) flavours = true;
  vector<vector<string>> rows;
  vector<string> head = {"", "Rel", "o/d"};
  if (flavours) head.push_back("Flavour");
  head.push_back("Build"); head.push_back("Commit"); head.push_back("UniqueID observed");
  rows.push_back(head);
  for (auto& r : m.rows) {
    vector<string> c = {r.b.vendor_str(), r.b.series, r.b.dbg ? "dbg" : "opt"};
    if (flavours) c.push_back(r.b.flavour == Flavour::Plain ? "plain" : r.b.flavour_str());
    c.push_back(r.b.date.empty() ? "-" : r.b.date);
    c.push_back(r.b.git_rev.empty() ? "-" : r.b.git_rev);
    c.push_back(r.uid);
    rows.push_back(c);
  }
  vector<size_t> w(head.size(), 0);
  for (auto& r : rows) for (size_t j = 0; j < r.size(); j++) w[j] = std::max(w[j], r[j].size());
  string out = "{noformat:title=Bug Detection Matrix}\n";
  for (auto& r : rows) {
    string line;
    for (size_t j = 0; j < r.size(); j++) {
      line += r[j];
      if (j + 1 < r.size()) line.append(w[j] - r[j].size() + 2, ' ');
    }
    out += line + "\n";
  }
  out += "{noformat}\n";
  return out;
}

// omnium matrix <sql> [build ...] [--slots N] [--options "..."] [--out FILE]
int cmd_matrix(const Args& a) {
  string sql, options, outfile;
  vector<string> names;
  int slots = 0;
  bool verbose = false;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--slots") slots = (int)to_long(val(s), 0);
    else if (s == "--options") options = val(s);
    else if (s == "--out") outfile = val(s);
    else if (s == "--verbose") verbose = true;
    else if (starts_with(s, "--")) { fprintf(stderr, "omnium matrix: unknown flag %s\n", s.c_str()); return 2; }
    else if (sql.empty()) sql = s;
    else names.push_back(s);
  }
  if (sql.empty() || !file_exists(sql)) { fprintf(stderr, "usage: omnium matrix <sql-file> [build ...] [--slots N] [--options \"...\"] [--out FILE]\n"); return 2; }
  string err;
  vector<Basedir> builds;
  if (!matrix_builds(names, builds, &err)) { fprintf(stderr, "omnium matrix: %s\n", err.c_str()); return 1; }
  if (slots <= 0) slots = std::max(1, std::min((int)builds.size(), (int)(ram_available_bytes() / (3ull << 30))));
  fprintf(stderr, "replaying %s on %zu builds, %d at a time\n", sql.c_str(), builds.size(), slots);
  MatrixResult m;
  if (!matrix_run(abs_path(sql), builds, options, slots, m, &err)) { fprintf(stderr, "omnium matrix: %s\n", err.c_str()); return 1; }
  string text = matrix_format(m);
  for (auto& r : m.rows) if (!r.note.empty()) text += "# " + r.b.name + ": " + r.note + "\n";
  if (verbose) for (auto& r : m.rows) text += fmt("# %s: %llu statements ran, %llu failed\n", r.b.name.c_str(), r.performed, r.failed);
  if (!outfile.empty()) write_file(outfile, text);
  fputs(text.c_str(), stdout);
  return 0;
}
