// Created by Roel Van de Paar, MariaDB
// client.cpp - the in-process SQL client with pquery semantics. One connection per thread, random
// (or sequential) statement order from a per-thread xoshiro stream, the per-thread trace
// <name>_thread-N.sql with #NOERROR / #ERROR: n - msg, the <name>_thread-N.last.sql witness when
// the connection is lost, the stop after 250 failures in a row, and <name>_general.log with the
// thread seeds and the NODE SUMMARY line the framework greps for.
#include "verbs.h"
#include "connect.h"

string client_plugin_dir(const string& test_dir) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(test_dir, ec)) {
    Basedir b;
    if (!basedir_parse_name(e.path().filename().string(), b) || b.vendor != Vendor::MariaDB || b.is_san()) continue;
    string d = e.path().string() + "/lib/plugin";
    if (file_exists(d + "/caching_sha2_password.so") || file_exists(d + "/caching_sha2_password.dll")) return d;
  }
  return {};
}
bool endpoint_connect(MYSQL* m, const Endpoint& e, const char* user, const char* db, unsigned long flags) {
  // MySQL 8.0+ logs root in with caching_sha2_password, a module the client library loads at run
  // time. A plain MariaDB build ships it. MARIADB_PLUGIN_DIR, when set, is left to the library.
  static const string plugin_dir = [] {
    const char* env = getenv("MARIADB_PLUGIN_DIR");
    return env && *env ? string() : client_plugin_dir(g_cfg.test_dir);
  }();
  if (!plugin_dir.empty()) mysql_options(m, MYSQL_PLUGIN_DIR, plugin_dir.c_str());
  if (e.tcp) {
    unsigned proto = MYSQL_PROTOCOL_TCP;
    mysql_options(m, MYSQL_OPT_PROTOCOL, &proto);
    return mysql_real_connect(m, "127.0.0.1", user, "", db, (unsigned)e.port, nullptr, flags) != nullptr;
  }
  return mysql_real_connect(m, nullptr, user, "", db, 0, e.sock.c_str(), flags) != nullptr;
}
bool endpoint_tcp(const Basedir& b) {
  const char* force = getenv("OMNIUM_TCP");
  return b.windows || (force && *force == '1');
}
vector<string> endpoint_args(const Endpoint& e) {
  // no TLS on the loopback: the client tool would otherwise warn on every passwordless TCP login
  if (e.tcp) return {"-h127.0.0.1", "-P" + std::to_string(e.port), "--protocol=tcp", "--skip-ssl"};
  return {"-S" + e.sock};
}

namespace {
struct ThreadOut {
  unsigned long long performed = 0, failed = 0;
  long gone_away = 0;
  bool lost = false, con_stop = false, connect_failed = false;
  uint64_t seed = 0;
  std::atomic<unsigned long> conn_id{0};
};
struct Trace {
  FILE* f = nullptr;
  vector<char> buf;
  bool open(const string& path) {
    f = fopen(path.c_str(), "w");
    if (!f) return false;
    buf.resize(1 << 20);
    setvbuf(f, buf.data(), _IOFBF, buf.size());
    return true;
  }
  void write(std::string_view s) { if (f) fwrite(s.data(), 1, s.size(), f); }
  void flush() { if (f) fflush(f); }
  void close() { if (f) { fclose(f); f = nullptr; } }
};
std::once_flag g_lib_once;
void lib_init() { std::call_once(g_lib_once, [] { mysql_library_init(0, nullptr, nullptr); }); }

void run_thread(const ClientParams& p, int number, const vector<std::pair<const char*, size_t>>& q, std::atomic<bool>& stop,
                ThreadOut& out, std::mutex& glog_mtx, FILE* glog) {
  mysql_thread_init();
  Xoshiro256pp rng;
  out.seed = p.seed ^ ((uint64_t)number * 0x9E3779B97F4A7C15ULL);
  rng.seed(out.seed);
  Trace trace, client_out;
  bool want_trace = p.log_failed || p.log_all || p.log_stats;
  string base = p.logdir + "/" + p.name + "_thread-" + std::to_string(number);
  {
    std::lock_guard<std::mutex> lk(glog_mtx);
    fprintf(glog, "- Thread #%d seed: %llu\n", number, (unsigned long long)out.seed);
    fflush(glog);
  }
  if (p.log_client_output && !client_out.open(base + ".out")) {
    std::lock_guard<std::mutex> lk(glog_mtx);
    fprintf(glog, "Unable to open logfile for client output %s.out: %s\n", base.c_str(), strerror(errno));
    mysql_thread_end();
    return;
  }
  if (want_trace && !trace.open(base + ".sql")) {
    std::lock_guard<std::mutex> lk(glog_mtx);
    fprintf(glog, "Unable to open thread logfile %s.sql: %s\n", base.c_str(), strerror(errno));
    mysql_thread_end();
    return;
  }
  MYSQL* conn = mysql_init(nullptr);
  if (!conn) {
    std::lock_guard<std::mutex> lk(glog_mtx);
    fprintf(glog, ": Thread #%d: mysql_init() failed, exiting abnormally\n", number);
    trace.close();
    mysql_thread_end();
    return;
  }
  unsigned long maxpacket = 33554432;
  mysql_options(conn, MYSQL_OPT_MAX_ALLOWED_PACKET, &maxpacket);
  if (!endpoint_connect(conn, p.ep, p.user.c_str(), p.db.c_str(), CLIENT_MULTI_STATEMENTS)) {
    trace.write(fmt("Error %u: %s\n", mysql_errno(conn), mysql_error(conn)));
    out.connect_failed = true;
    mysql_close(conn);
    trace.close();
    mysql_thread_end();
    return;
  }
  out.conn_id = mysql_thread_id(conn);
  const size_t witness_cap = p.crash_last_lines > 0 ? (size_t)p.crash_last_lines : 0;
  vector<size_t> witness(witness_cap, SIZE_MAX);
  size_t witness_pos = 0;
  int con_fail = 0;
  string line;
  for (unsigned long i = 0; i < p.queries_per_thread; i++) {
    if (stop.load(std::memory_order_relaxed)) break;
    size_t qn = p.shuffle ? (size_t)rng.below(q.size()) : (size_t)i;
    if (qn >= q.size()) break;                                  // sequential: the file is done
    double t0 = p.log_duration ? now_ms() : 0;
    int res = mysql_real_query(conn, q[qn].first, (unsigned long)q[qn].second);
    double t1 = p.log_duration ? now_ms() : 0;
    out.performed++;                                            // sent, so counted, also when it ends the thread below
    if (witness_cap) { witness[witness_pos % witness_cap] = qn; witness_pos++; }
    unsigned err = 0;
    if (res == 0) {
      con_fail = 0;
    } else {
      err = mysql_errno(conn);
      out.failed++;
      con_fail++;
      if (err == 2006) out.gone_away++;
      if (err == 2013) {
        out.lost = true;
        if (witness_cap && !stop.load()) {
          FILE* w = fopen((base + ".last.sql").c_str(), "w");
          if (w) {
            size_t have = witness_pos < witness_cap ? witness_pos : witness_cap;
            size_t start = witness_pos - have;
            for (size_t k = 0; k < have; k++) {
              size_t idx = witness[(start + k) % witness_cap];
              if (idx == SIZE_MAX) continue;
              fwrite(q[idx].first, 1, q[idx].second, w);
              fputc('\n', w);
            }
            fclose(w);
            std::lock_guard<std::mutex> lk(glog_mtx);
            fprintf(glog, ": Thread #%d: CR_SERVER_LOST; wrote %zu last queries to %s.last.sql\n", number, have, base.c_str());
            fflush(glog);
          }
        }
        trace.flush();
        client_out.flush();
        break;
      }
      if (con_fail >= p.max_con_failures) {
        string msg = fmt("* Last %d consecutive queries all failed. Likely crash/assert, user privileges drop, or similar. Ending run.", con_fail);
        { std::lock_guard<std::mutex> lk(glog_mtx); fprintf(glog, "%s\n", msg.c_str()); fflush(glog); }
        trace.write(msg + "\n");
        trace.flush();
        out.con_stop = true;
        break;
      }
    }
    if (trace.f) {
      bool log_this = res == 0 ? (p.log_all || p.log_stats) : (p.log_failed || p.log_all || p.log_stats);
      if (log_this) {
        line.assign(q[qn].first, q[qn].second);
        if (res == 0) line += "#NOERROR";
        else line += fmt("#ERROR: %u - %s", err, mysql_error(conn));
        if (p.log_stats) line += fmt("#WARNINGS: %u#CHANGED: %lld", mysql_warning_count(conn), (long long)mysql_affected_rows(conn));
        if (p.log_duration) line += fmt("#Duration: %.3f ms", t1 - t0);
        if (p.log_numbers) line += fmt("#%zu", qn + 1);
        line += '\n';
        trace.write(line);
      }
    }
    // every result set is drained, or the next query answers "Commands out of sync"
    do {
      MYSQL_RES* result = nullptr;
      if (p.log_client_output) result = mysql_use_result(conn);
      else if (mysql_field_count(conn) > 0) result = mysql_store_result(conn);
      if (p.log_client_output && result) {
        unsigned nf = mysql_num_fields(result);
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
          string o;
          for (unsigned j = 0; j < nf; j++) {
            if (row[j]) { if (row[j][0] == 0) o += "EMPTY#"; else { o += row[j]; o += '#'; } }
            else o += "#NO DATA#";
          }
          if (p.log_numbers) o += std::to_string(qn + 1);
          o += '\n';
          client_out.write(o);
        }
      }
      if (result) mysql_free_result(result);
    } while (mysql_next_result(conn) == 0);
  }
  trace.close();
  client_out.close();
  out.conn_id = 0;
  mysql_close(conn);
  mysql_thread_end();
}
}  // namespace

static std::mutex g_conn_mtx;
static std::map<const ClientParams*, vector<ThreadOut>*> g_live;   // for client_kill_connections

bool client_run(const ClientParams& p, const string& sql_path, std::atomic<bool>& stop, ClientResult& res, string* err) {
  lib_init();
  res = ClientResult();
  string sql = read_file(sql_path);
  if (sql.empty()) { if (err) *err = "no SQL in " + sql_path; return false; }
  vector<std::pair<const char*, size_t>> q;
  {
    size_t pos = 0;
    while (pos < sql.size()) {
      size_t nl = sql.find('\n', pos);
      size_t end = nl == string::npos ? sql.size() : nl;
      size_t len = end - pos;
      while (len > 0 && sql[pos + len - 1] == '\r') len--;
      if (len > 0) q.push_back({sql.data() + pos, len});
      if (nl == string::npos) break;
      pos = nl + 1;
    }
  }
  if (q.empty()) { if (err) *err = "no statements in " + sql_path; return false; }
  mkdirs(p.logdir);
  FILE* glog = fopen((p.logdir + "/" + p.name + "_general.log").c_str(), "w");
  if (!glog) { if (err) *err = "cannot write " + p.logdir + "/" + p.name + "_general.log"; return false; }
  std::mutex glog_mtx;
  // the first line pquery writes: what it read and from where
  {
    size_t bytes = 0;
    for (auto& l : q) bytes += l.second + 1;
    fprintf(glog, "- Read %zu lines (%zu bytes) from %s\n", q.size(), bytes, sql_path.c_str());
  }
  vector<ThreadOut> outs(std::max(1, p.threads));
  { std::lock_guard<std::mutex> lk(g_conn_mtx); g_live[&p] = &outs; }
  vector<std::thread> th;
  for (int i = 0; i < std::max(1, p.threads); i++) th.emplace_back(run_thread, std::cref(p), i, std::cref(q), std::ref(stop), std::ref(outs[i]), std::ref(glog_mtx), glog);
  for (auto& t : th) t.join();
  { std::lock_guard<std::mutex> lk(g_conn_mtx); g_live.erase(&p); }
  for (auto& o : outs) {
    res.performed += o.performed;
    res.failed += o.failed;
    res.gone_away += o.gone_away;
    if (o.lost) res.lost_connection++;
    if (o.con_stop) res.consecutive_stop++;
    if (o.connect_failed) res.connect_failed++;
    res.thread_seeds.push_back(o.seed);
  }
  fputs(node_summary_line(res.failed, res.performed).c_str(), glog);
  fclose(glog);
  return true;
}

// performed counts every query the client sent, failures included, so the total is performed
string node_summary_line(unsigned long long failed, unsigned long long performed) {
  double pct = performed > 0 ? (double)(performed - failed) * 100.0 / (double)performed : 0.0;
  return fmt("* NODE SUMMARY: %llu/%llu queries failed, (%.2f%% were successful)\n", failed, performed, pct);
}

// a client thread stuck in a long statement is freed by killing its connection from the outside
void client_kill_connections(const ClientParams& p) {
  vector<unsigned long> ids;
  {
    std::lock_guard<std::mutex> lk(g_conn_mtx);
    auto it = g_live.find(&p);
    if (it == g_live.end()) return;
    for (auto& o : *it->second) { unsigned long id = o.conn_id.load(); if (id) ids.push_back(id); }
  }
  if (ids.empty()) return;
  lib_init();
  mysql_thread_init();
  MYSQL* m = mysql_init(nullptr);
  unsigned t = 5;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
  mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &t);
  mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &t);
  if (endpoint_connect(m, p.ep, p.user.c_str(), nullptr, 0)) {
    for (auto id : ids) { string k = "KILL CONNECTION " + std::to_string(id); mysql_real_query(m, k.c_str(), (unsigned long)k.size()); }
  }
  mysql_close(m);
  mysql_thread_end();
}
