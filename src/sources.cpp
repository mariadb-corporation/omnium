// Created by Roel Van de Paar, MariaDB
// sources.cpp - the SQL of a trial: four sources, the filters, the transforms, the area table, the seeds
#include "common.h"
#include "verbs.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <fcntl.h>
#include <csignal>
#include <regex>

static const size_t PQUERY_MAX_SQL_LINES = 5141189;   // the pquery client maximum
static const size_t SQL_LINES_FIRST = 20000;          // before any trial has run
static const size_t SQL_LINES_MIN = 2000;
static const int DISK_POOL_EVERY = 45;                // trials one all-disk pool serves
static const size_t DISK_POOL_LINES = 100000;
static const int64_t DISK_FILE_MAX = 256LL << 20;     // bigger .sql files are skipped by the all-disk source
static const int RANDOM_OPTIONS_PCT = 10;             // trials that get 1-3 random server options added
static const int GEN_TIMEOUT_S = 900;

// ---------------------------------------------------------------------------------------------
// PCRE2
// ---------------------------------------------------------------------------------------------
RxSet::~RxSet() {
  if (md) pcre2_match_data_free((pcre2_match_data*)md);
  if (re) pcre2_code_free((pcre2_code*)re);
}
bool RxSet::compile(const vector<string>& patterns, string* err) {
  if (re) { pcre2_match_data_free((pcre2_match_data*)md); pcre2_code_free((pcre2_code*)re); re = md = nullptr; }
  count = patterns.size();
  if (patterns.empty()) return true;
  string all;
  for (auto& p : patterns) all += (all.empty() ? "(?:" : "|(?:") + p + ")";
  int ec = 0;
  PCRE2_SIZE eo = 0;
  pcre2_code* c = pcre2_compile((PCRE2_SPTR)all.c_str(), PCRE2_ZERO_TERMINATED, PCRE2_CASELESS, &ec, &eo, nullptr);
  if (!c) {
    PCRE2_UCHAR buf[256];
    pcre2_get_error_message(ec, buf, sizeof buf);
    if (err) *err = fmt("pattern error at offset %zu: %s", (size_t)eo, (const char*)buf);
    return false;
  }
  pcre2_jit_compile(c, PCRE2_JIT_COMPLETE);
  re = c;
  md = pcre2_match_data_create_from_pattern(c, nullptr);
  return true;
}
bool RxSet::hit(std::string_view line) {
  if (!re) return false;
  std::lock_guard<std::mutex> lk(mtx);
  return pcre2_match((pcre2_code*)re, (PCRE2_SPTR)line.data(), line.size(), 0, 0, (pcre2_match_data*)md, nullptr) >= 0;
}
// grep basic regex to PCRE: \( \) \? \+ \{ \} \| are operators, the bare characters are literals
string bre_to_pcre(const string& bre) {
  string out;
  for (size_t i = 0; i < bre.size(); i++) {
    char c = bre[i];
    if (c == '\\' && i + 1 < bre.size()) {
      char n = bre[i + 1];
      if (strchr("()?+{}|", n)) out += n; else { out += '\\'; out += n; }
      i++;
    } else if (c == '[') {
      size_t j = i + 1;
      if (j < bre.size() && bre[j] == '^') j++;
      if (j < bre.size() && bre[j] == ']') j++;
      while (j < bre.size() && bre[j] != ']') j++;
      out += bre.substr(i, j - i + 1);
      i = j;
    } else if (strchr("()?+{}|", c)) {
      out += '\\'; out += c;
    } else {
      out += c;
    }
  }
  return out;
}
vector<string> read_pattern_file(const string& path) {
  vector<string> out;
  for (auto& raw : split_lines(read_file(path))) {
    string l = trim(raw);
    if (l.empty() || l[0] == '#') continue;
    out.push_back(l);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// the area table: what the 42 conf files aimed one run at, now picked per trial
// ---------------------------------------------------------------------------------------------
static const char* DEADLOCK3 = "--log_bin --deadlock-timeout-short=3 --deadlock-timeout-long=3 --deadlock-search-depth-short=3 --sync_binlog=1 --innodb_flush_log_at_trx_commit=2";
static const char* DEADLOCK10 = "--log_bin --deadlock-timeout-short=10 --deadlock-timeout-long=10 --deadlock-search-depth-short=10 --deadlock-search-depth-long=33";
static const char* XA_EXTRA = "--innodb_file_per_table=1 --innodb_flush_method=O_DIRECT --innodb_stats_persistent=off --loose-idle_write_transaction_timeout=0 --loose-idle_transaction_timeout=0 --loose-idle_readonly_transaction_timeout=0 --connect_timeout=60 --interactive_timeout=28800 --slave_net_timeout=60 --net_read_timeout=30 --net_write_timeout=60 --wait_timeout=28800 --lock-wait-timeout=86400 --innodb-lock-wait-timeout=50 --log_output=FILE --log-bin --log_bin_trust_function_creators=1 --loose-max-statement-time=30 --loose-debug_assert_on_not_freed_memory=0 --innodb-buffer-pool-size=300M";
static const char* XA_INTERLEAVE =
  "SET autocommit=0;\nXA START 'a';\nXA END 'a';\nXA COMMIT 'a';\nXA COMMIT 'a' ONE PHASE;\nXA PREPARE 'a';\nXA ROLLBACK 'a';\n"
  "XA START 'a','b',2;\nXA END 'a','b',2;\nXA COMMIT 'a','b',2;\nXA COMMIT 'a','b',2 ONE PHASE;\nXA PREPARE 'a','b',2;\nXA ROLLBACK 'a','b',2;\n"
  "XA RECOVER;\nXA RECOVER FORMAT='RAW';\nXA RECOVER FORMAT='SQL';\nSET autocommit=1;";
static const char* ENGINES_INTERLEAVE =
  "CREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=InnoDB;\nCREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=Aria;\nCREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=MyISAM;\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=CSV;\nCREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=Archive;\nCREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=Blackhole;\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT) ENGINE=Memory;\nCREATE OR REPLACE TABLE t1 (c1 INT PRIMARY KEY) ENGINE=InnoDB;\nCREATE OR REPLACE TABLE t1 (c1 INT PRIMARY KEY) ENGINE=Aria;\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT PRIMARY KEY) ENGINE=MyISAM;";
static const char* ARIA_INTERLEAVE =
  "CREATE TABLE t1 (c1 INT KEY) ENGINE=Aria;\nCREATE TABLE t1 (c INT) ENGINE=Aria;\nCREATE TABLE t1 (c1 DATE) ENGINE=Aria;\nCREATE TABLE t1 (c BLOB) ENGINE=Aria;\n"
  "CREATE TABLE t (c1 INT KEY) ENGINE=Aria;\nCREATE TABLE t (c INT) ENGINE=Aria;\nCREATE TABLE t (c BLOB) ENGINE=Aria;";
static const char* PARTITION_INTERLEAVE =
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 VARCHAR(10)) PARTITION BY HASH(c1) PARTITIONS 4;\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 VARCHAR(10)) PARTITION BY KEY(c1) PARTITIONS 3;\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 VARCHAR(10)) PARTITION BY RANGE(c1) (PARTITION p0 VALUES LESS THAN (10), PARTITION p1 VALUES LESS THAN (100), PARTITION p2 VALUES LESS THAN MAXVALUE);\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 VARCHAR(10)) PARTITION BY LIST(c1) (PARTITION p0 VALUES IN (0,1,2), PARTITION p1 VALUES IN (3,4,5), PARTITION p2 DEFAULT);\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 DATE) PARTITION BY RANGE COLUMNS(c2) (PARTITION p0 VALUES LESS THAN ('2020-01-01'), PARTITION p1 VALUES LESS THAN MAXVALUE);\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 INT) PARTITION BY RANGE(c1) SUBPARTITION BY HASH(c2) SUBPARTITIONS 2 (PARTITION p0 VALUES LESS THAN (10), PARTITION p1 VALUES LESS THAN MAXVALUE);\n"
  "CREATE OR REPLACE TABLE t1 (c1 INT, c2 INT) ENGINE=Aria PARTITION BY HASH(c1) PARTITIONS 2;\n"
  "ALTER TABLE t1 ADD PARTITION (PARTITION p9 VALUES LESS THAN (1000));\nALTER TABLE t1 DROP PARTITION p0;\nALTER TABLE t1 REORGANIZE PARTITION p1 INTO (PARTITION p1a VALUES LESS THAN (50), PARTITION p1b VALUES LESS THAN (100));\n"
  "ALTER TABLE t1 COALESCE PARTITION 1;\nALTER TABLE t1 TRUNCATE PARTITION p1;\nALTER TABLE t1 ANALYZE PARTITION ALL;\nALTER TABLE t1 CHECK PARTITION ALL;\nALTER TABLE t1 OPTIMIZE PARTITION ALL;\nALTER TABLE t1 REBUILD PARTITION ALL;\nALTER TABLE t1 REMOVE PARTITIONING;\n"
  "ALTER TABLE t1 EXCHANGE PARTITION p0 WITH TABLE t2;\nALTER TABLE t1 CONVERT PARTITION p0 TO TABLE t3;\nALTER TABLE t1 CONVERT TABLE t3 TO PARTITION p0 VALUES LESS THAN (10);";
static const char* OPTIMIZER_INTERLEAVE =
  "SET SESSION optimizer_switch='index_merge=off';\nSET SESSION optimizer_switch='index_merge=on';\nSET SESSION optimizer_switch='mrr=on,mrr_cost_based=off';\nSET SESSION optimizer_switch='materialization=off';\n"
  "SET SESSION optimizer_switch='semijoin=off';\nSET SESSION optimizer_switch='semijoin=on,firstmatch=off,loosescan=off';\nSET SESSION optimizer_switch='derived_merge=off';\nSET SESSION optimizer_switch='derived_with_keys=off';\n"
  "SET SESSION optimizer_switch='condition_pushdown_for_derived=off';\nSET SESSION optimizer_switch='split_materialized=off';\nSET SESSION optimizer_switch='rowid_filter=off';\nSET SESSION optimizer_switch='in_to_exists=off';\n"
  "SET SESSION optimizer_switch='subquery_cache=off';\nSET SESSION optimizer_switch='join_cache_hashed=off';\nSET SESSION optimizer_switch='join_cache_bka=off';\nSET SESSION optimizer_switch='default';\n"
  "SET SESSION join_cache_level=8;\nSET SESSION join_cache_level=0;\nSET SESSION optimizer_use_condition_selectivity=5;\nSET SESSION optimizer_use_condition_selectivity=1;\nSET SESSION use_stat_tables=PREFERABLY;\nSET SESSION histogram_type=JSON_HB;\n"
  "ANALYZE TABLE t1 PERSISTENT FOR ALL;\nSET SESSION optimizer_max_sel_args=100;\nSET SESSION optimizer_search_depth=0;\nSET SESSION optimizer_prune_level=2;";
static const char* CHARSET_INTERLEAVE =
  "SET NAMES utf8mb4;\nSET NAMES latin1;\nSET NAMES utf8mb3;\nSET NAMES big5;\nSET NAMES sjis;\nSET NAMES cp1251;\nSET NAMES tis620;\nSET NAMES euckr;\nSET NAMES gbk;\nSET NAMES utf8mb4 COLLATE utf8mb4_uca1400_ai_ci;\n"
  "SET character_set_results=NULL;\nSET collation_connection=utf8mb4_bin;\nSET collation_connection=latin1_general_cs;\nSET NAMES DEFAULT;\n"
  "CREATE OR REPLACE TABLE t1 (c1 VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_uca1400_ai_ci, c2 TEXT CHARACTER SET latin1, c3 CHAR(5) CHARACTER SET ucs2, c4 VARCHAR(9) CHARACTER SET utf16, c5 VARCHAR(7) CHARACTER SET utf32) ;";
static const char* FEDERATED_INTERLEAVE =
  "INSTALL SONAME 'ha_federatedx';\nCREATE USER federatedx@localhost IDENTIFIED BY '';\nGRANT ALL ON test.* TO federatedx@localhost;\n"
  "CREATE OR REPLACE SERVER fsrv FOREIGN DATA WRAPPER mysql OPTIONS (__FEDERATED_CONN__, DATABASE 'test', USER 'federatedx', PASSWORD '');\n"
  "CREATE OR REPLACE TABLE tf (c INT PRIMARY KEY, c1 BLOB) ENGINE=InnoDB;\nCREATE OR REPLACE TABLE t1 (c INT PRIMARY KEY, c1 BLOB) ENGINE=FEDERATED CONNECTION='fsrv/tf';\n"
  "INSERT INTO t1 VALUES (1,'a'),(2,'b');\nSELECT * FROM t1;\nUPDATE t1 SET c1='c' WHERE c=1;\nDELETE FROM t1 WHERE c=2;\nFLUSH TABLES;";
static const char* CRASHREC_NOTE = "a mode of the trial, not an option set: the server is killed and restarted on the same datadir";

const vector<Area>& areas_all() {
  static vector<Area> v;
  if (!v.empty()) return v;
  auto add = [&](Area a) { v.push_back(std::move(a)); };
  { Area a; a.name = "default"; a.weight = 28; a.note = "the plain server with a binlog";
    a.myextra_choices = {DEADLOCK3, DEADLOCK10, "--sql_mode= --log_bin --deadlock-timeout-short=3 --deadlock-timeout-long=3 --deadlock-search-depth-short=13 --deadlock-search-depth-long=50"}; add(a); }
  { Area a; a.name = "engines"; a.weight = 7; a.note = "every engine in turn on t1"; a.myextra_choices = {DEADLOCK10}; a.interleave = ENGINES_INTERLEAVE; a.interleave_lines = 250; add(a); }
  { Area a; a.name = "rocksdb"; a.weight = 6; a.note = "RocksDB loaded, InnoDB tables swapped to RocksDB half the time"; a.needs = "rocksdb";
    a.myextra_choices = {string("--plugin_load_add=ha_rocksdb ") + DEADLOCK10}; a.engine_swap = "RocksDB"; a.engine_swap_pct = 50; add(a); }
  { Area a; a.name = "xa"; a.weight = 8; a.note = "XA transactions with the timeouts the XA runs used"; a.myextra_choices = {XA_EXTRA}; a.interleave = XA_INTERLEAVE; a.interleave_lines = 150; add(a); }
  { Area a; a.name = "spider"; a.weight = 5; a.note = "Spider tables over a self link; the spider preload runs first"; a.needs = "spider";
    a.myextra_choices = {"--sql_mode="}; a.preload_file = "spiderpreload.sql"; add(a); }
  { Area a; a.name = "federated"; a.weight = 4; a.note = "FederatedX tables over a self link"; a.needs = "federated";
    a.myextra_choices = {"--sql_mode="}; a.interleave = FEDERATED_INTERLEAVE; a.interleave_lines = 250; add(a); }
  { Area a; a.name = "aria-encryption"; a.weight = 5; a.note = "file-key-management with Aria encryption, Aria tables interleaved and swapped in";
    a.myextra_choices = {DEADLOCK10}; a.encryption = true; a.encryption_items = "--log-bin --aria-encrypt-tables=ON";
    a.interleave = ARIA_INTERLEAVE; a.interleave_lines = 400; a.engine_swap = "Aria"; a.engine_swap_pct = 50; add(a); }
  { Area a; a.name = "innodb-encryption"; a.weight = 5; a.note = "file-key-management with InnoDB table, log and Aria encryption";
    a.myextra_choices = {DEADLOCK10, "--sql_mode= --log_bin --deadlock-timeout-short=3 --deadlock-timeout-long=3 --deadlock-search-depth-short=13 --deadlock-search-depth-long=50"};
    a.encryption = true; a.encryption_items = "--aria-encrypt-tables=ON --innodb-encrypt-log=ON --innodb_encrypt_tables=ON"; add(a); }
  { Area a; a.name = "optimizer"; a.weight = 8; a.note = "optimizer_switch and statistics settings toggled through the run"; a.myextra_choices = {DEADLOCK10}; a.interleave = OPTIMIZER_INTERLEAVE; a.interleave_lines = 100; add(a); }
  { Area a; a.name = "partitioning"; a.weight = 6; a.note = "partitioned t1 shapes and partition maintenance interleaved"; a.myextra_choices = {DEADLOCK10}; a.interleave = PARTITION_INTERLEAVE; a.interleave_lines = 200; add(a); }
  { Area a; a.name = "sql-mode"; a.weight = 7; a.note = "one sql_mode per trial: empty, ORACLE, ANSI, TRADITIONAL, STRICT";
    a.myextra_choices = {string("--sql_mode= ") + DEADLOCK10, string("--sql_mode=ORACLE ") + DEADLOCK10, string("--sql_mode=ANSI ") + DEADLOCK10,
                         string("--sql_mode=TRADITIONAL ") + DEADLOCK10, string("--sql_mode=STRICT_ALL_TABLES,NO_ZERO_DATE,ONLY_FULL_GROUP_BY ") + DEADLOCK10}; add(a); }
  { Area a; a.name = "binlog"; a.weight = 8; a.note = "one binlog format per trial: ROW, STATEMENT, MIXED, plus the allb option set";
    string allb = "--sql_mode= --deadlock-timeout-short=10 --deadlock-timeout-long=10 --deadlock-search-depth-short=10 --deadlock-search-depth-long=33 --innodb_change_buffering=inserts --innodb_fast_shutdown=1 --innodb_file_per_table=1 --log-bin --log_bin_trust_function_creators=1 --innodb-buffer-pool-size=5242880 --loose-debug_assert_on_not_freed_memory=1 --innodb_use_native_aio=1";
    a.myextra_choices = {allb + " --binlog_format=ROW", allb + " --binlog_format=STATEMENT", allb + " --binlog_format=MIXED",
                         string("--log_bin --binlog_format=MIXED --plugin_load_add=ha_rocksdb ") + DEADLOCK10, "--log-bin --binlog_format=ROW --binlog_row_image=MINIMAL --log_bin_trust_function_creators=1 --sync_binlog=1"}; add(a); }
  { Area a; a.name = "charsets"; a.weight = 7; a.note = "one server character set per trial, SET NAMES interleaved";
    for (const char* cs : {"utf8mb4 --collation-server=utf8mb4_uca1400_ai_ci", "utf8mb4 --collation-server=utf8mb4_bin", "latin1", "utf8mb3", "big5", "sjis", "cp1251", "tis620", "euckr", "gbk", "utf16", "ucs2", "utf32"})
      a.myextra_choices.push_back(string("--character-set-server=") + cs + " " + DEADLOCK10);
    a.interleave = CHARSET_INTERLEAVE; a.interleave_lines = 150; add(a); }
  { Area a; a.name = "crash-recovery"; a.weight = 0; a.note = CRASHREC_NOTE; a.mode = true; a.myextra_choices = {DEADLOCK10}; add(a); }
  { Area a; a.name = "multi-thread"; a.weight = 0; a.note = "a mode of the trial, not an option set: several client threads on one server"; a.mode = true; a.myextra_choices = {DEADLOCK10}; add(a); }
  { Area a; a.name = "replication"; a.weight = 0; a.note = "not yet: a primary and a replica per trial"; a.mode = true; a.myextra_choices = {"--sql_mode= --deadlock-timeout-short=10 --deadlock-timeout-long=10 --deadlock-search-depth-short=10 --deadlock-search-depth-long=33 --loose-debug_assert_on_not_freed_memory=1 --innodb-buffer-pool-size=200M"}; add(a); }
  { Area a; a.name = "galera"; a.weight = 0; a.note = "not yet: a three node cluster per trial"; a.mode = true; add(a); }
  return v;
}
// a plugin by its base name: ha_rocksdb.so on Linux, ha_rocksdb.dll in a Windows build
static bool plugin_present(const Basedir& b, const string& name) {
  string so = name + (b.windows ? ".dll" : ".so");
  return file_exists(b.path + "/lib/plugin/" + so) || file_exists(b.path + "/lib64/plugin/" + so) ||
         file_exists(b.path + "/storage/" + (starts_with(name, "ha_") ? name.substr(3) : name) + "/" + so);
}
bool area_available(const Area& a, const Basedir& b, string* why) {
  if (a.mode && a.weight == 0) { if (why) *why = "a mode, not picked as an area"; return false; }
  if (a.needs == "rocksdb" && !plugin_present(b, "ha_rocksdb")) { if (why) *why = "no ha_rocksdb plugin in this build"; return false; }
  if (a.needs == "spider" && !plugin_present(b, "ha_spider")) { if (why) *why = "no ha_spider plugin in this build"; return false; }
  if (a.needs == "federated" && !plugin_present(b, "ha_federatedx")) { if (why) *why = "no ha_federatedx plugin in this build"; return false; }
  if (a.encryption && !plugin_present(b, "file_key_management")) { if (why) *why = "no file_key_management plugin in this build"; return false; }
  if (b.vendor != Vendor::MariaDB && a.name != "default") { if (why) *why = "MariaDB options; not for MySQL or Percona"; return false; }
  return true;
}
vector<const Area*> areas_available(const Basedir& b, const vector<string>& only) {
  vector<const Area*> out;
  for (auto& a : areas_all()) {
    if (!only.empty() && std::find(only.begin(), only.end(), a.name) == only.end()) continue;
    if (area_available(a, b, nullptr) && a.weight > 0) out.push_back(&a);
  }
  return out;
}
const Area* area_pick(const vector<const Area*>& v, Xoshiro256pp& r) {
  if (v.empty()) return nullptr;
  long total = 0;
  for (auto a : v) total += a->weight;
  long x = r.range(1, total);
  for (auto a : v) { x -= a->weight; if (x <= 0) return a; }
  return v.back();
}
const Area* area_by_name(const string& name) {
  for (auto& a : areas_all()) if (a.name == name) return &a;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// line clean-up and the transforms
// ---------------------------------------------------------------------------------------------
// the clean-up every source gets: file-name prefixes and the outcome markers of harvested SQL
string sql_line_cleanup(string l) {
  for (const char* pre : {"/data/", "/test/"}) {
    size_t p;
    while ((p = l.find(pre)) != string::npos) {
      size_t e = l.find(".sql:", p);
      if (e == string::npos || l.find(':', p) < e) break;
      l.erase(p, e + 5 - p);
    }
  }
  size_t p = l.find(";#NOERROR");
  if (p != string::npos && (p + 9 == l.size() || l[p + 9] == '#' || l[p + 9] == ':')) l.erase(p + 1);
  p = l.find(";#ERROR: ");
  if (p != string::npos) l.erase(p + 1);
  p = l.find("\r#NOERROR");
  if (p != string::npos) { l.erase(p); l += ';'; }
  return l;
}
static string ireplace_all(string s, const string& from, const string& to, bool first_only) {
  string ls = lower(s), lf = lower(from);
  size_t p = 0;
  while ((p = ls.find(lf, p)) != string::npos) {
    s.replace(p, from.size(), to);
    ls.replace(p, from.size(), lower(to));
    p += to.size();
    if (first_only) break;
  }
  return s;
}
void sql_engine_swap(vector<string>& lines, const string& engine, int pct) {
  static const char* engines[] = {"InnoDB", "Aria", "MyISAM", "BLACKHOLE", "RocksDB", "TokuDB"};
  if (engine.empty()) return;
  if (pct < 1) pct = 100;
  for (size_t i = 0; i < lines.size(); i++) {
    size_t nr = i + 1;
    if (pct < 100 && !((nr * pct / 100) > ((nr - 1) * pct / 100))) continue;
    for (const char* e : engines) if (e != engine) lines[i] = ireplace_all(lines[i], e, engine, false);
  }
}
void sql_interleave(vector<string>& lines, const string& block, int every) {
  if (block.empty() || every < 1) return;
  vector<string> b;
  for (auto& l : split(block, '\n')) if (!trim(l).empty()) b.push_back(l);
  if (b.empty()) return;
  vector<string> out;
  out.reserve(lines.size() + lines.size() / every * b.size() + 1);
  for (size_t i = 0; i < lines.size(); i++) {
    if ((i + 1) % every == 0) for (auto& x : b) out.push_back(x);
    out.push_back(lines[i]);
  }
  lines.swap(out);
}
void sql_swap_create_table_names(vector<string>& lines) {
  static const std::regex r1("CREATE TABLE([^(]*)\\(", std::regex::icase), r2("[ \\t][ \\t]+"), r3("CREATE TABLE [^ ]+ ", std::regex::icase);
  for (auto& l : lines) {
    l = std::regex_replace(l, r1, "CREATE TABLE $1 (");
    l = std::regex_replace(l, r2, " ");
    l = std::regex_replace(l, r3, "CREATE TABLE t1 ");
  }
}
void sql_swap_all_table_names(vector<string>& lines) {
  static const char* names[] = {"t[0-9]+", "t[itm]+", "t", "m[0-9]+", "articles", "foo", "bar", "db[0-9]+.t[0-9]+", "child", "parent",
                                "testdb_wl5522.t1", "tm[0-9]+", "src", "federated.t1", "variant"};
  static vector<std::regex> rx;
  if (rx.empty()) for (const char* n : names) rx.emplace_back(string("([ .]+)") + n + "([`', \\t();]+)");
  for (auto& l : lines)
    for (auto& r : rx) l = std::regex_replace(l, r, "$1t1$2", std::regex_constants::format_first_only);
}

// ---------------------------------------------------------------------------------------------
// the run-wide source state
// ---------------------------------------------------------------------------------------------
static string yacc_for(const Basedir& b) {
  string dir = g_paths.qa + "/yacc";
  string want = dir + "/" + b.series + "_sql_yacc.yy";
  if (file_exists(want)) return want;
  string best, best_v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    string n = e.path().filename().string();
    if (!ends_with(n, "_sql_yacc.yy")) continue;
    string v = n.substr(0, n.size() - 12);
    if (best.empty() || version_cmp(v, best_v) > 0) { best = e.path().string(); best_v = v; }
  }
  return best;
}
static bool index_infile(SqlSources& s, string* err) {
  string src = g_cfg.infile;
  if (src.empty()) src = g_paths.qa + "/pquery/main-ms-ps-md.sql.tar.xz";
  if (!file_exists(src)) { if (err) *err = "INFILE not found: " + src; return false; }
  string target = src;
  if (ends_with(src, ".tar.xz") || ends_with(src, ".tar.gz") || ends_with(src, ".tgz") || ends_with(src, ".xz")) {
    string dir = s.work + "/infile";
    mkdirs(dir);
    string have;                                              // extracted once per run (the driver does it)
    {
      std::error_code ec;
      for (auto& e : fs::recursive_directory_iterator(dir, ec)) if (e.is_regular_file() && ends_with(e.path().string(), ".sql")) { have = e.path().string(); break; }
    }
    if (have.empty()) {
      CmdResult r = ends_with(src, ".xz") && !ends_with(src, ".tar.xz") ? run_shell("xz -dkc " + sh_quote(src) + " > " + sh_quote(dir + "/infile.sql"), 1800)
                                                                         : run_capture({"tar", "-xf", src, "-C", dir}, 1800);
      if (r.rc != 0) { if (err) *err = "cannot extract " + src + ": " + tail_lines(r.out, 3); return false; }
    }
    target = "";
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(dir, ec)) if (e.is_regular_file() && ends_with(e.path().string(), ".sql")) { target = e.path().string(); break; }
    if (target.empty()) { if (err) *err = "no .sql file inside " + src; return false; }
  }
  s.infile = target;
  int fd = open(target.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { if (err) *err = "cannot open " + target; return false; }
  s.infile_fd = fd;
  s.infile_offsets.clear();
  s.infile_offsets.push_back(0);
  vector<char> buf(1 << 20);
  uint64_t pos = 0;
  ssize_t n;
  while ((n = read(fd, buf.data(), buf.size())) > 0) {
    for (ssize_t i = 0; i < n; i++) if (buf[i] == '\n') s.infile_offsets.push_back(pos + i + 1);
    pos += n;
  }
  if (s.infile_offsets.back() != pos) s.infile_offsets.push_back(pos);   // a last line without a newline
  s.infile_lines = s.infile_offsets.size() - 1;
  return true;
}
static string infile_line(SqlSources& s, size_t k) {
  uint64_t a = s.infile_offsets[k], b = s.infile_offsets[k + 1];
  if (b <= a) return "";
  string l(b - a, '\0');
  ssize_t n = pread(s.infile_fd, l.data(), l.size(), (off_t)a);
  if (n <= 0) return "";
  l.resize(n);
  while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
  return l;
}
static void index_disk(SqlSources& s) {
  string fp = s.work + "/disk_p.txt", fa = s.work + "/disk_a.txt";
  if (file_exists(fp) && file_exists(fa)) {                  // the driver's index, shared by every worker
    for (auto& l : split_lines(read_file(fp))) if (!l.empty()) s.disk_p.push_back(l);
    for (auto& l : split_lines(read_file(fa))) if (!l.empty()) s.disk_a.push_back(l);
    return;
  }
  string home = home_dir();
  CmdResult p = run_shell("/usr/bin/find " + sh_quote(home) + " /*/SQL /*/TESTCASES -maxdepth 3 -name '*.sql' -type f 2>/dev/null | grep -vi newbugs_dups", 900);
  CmdResult a = run_shell("/usr/bin/find / -maxdepth 5 -name '*.sql' -type f 2>/dev/null | grep -viE '/test/TESTCASES|newbugs_dups'", 1800);
  for (auto& l : split_lines(p.out)) if (!l.empty()) s.disk_p.push_back(l);
  for (auto& l : split_lines(a.out)) if (!l.empty()) s.disk_a.push_back(l);
  write_file(fp, join(s.disk_p, "\n") + "\n");
  write_file(fa, join(s.disk_a, "\n") + "\n");
}
static size_t collect_disk_pool(SqlSources& s, Xoshiro256pp& r, vector<string>& pool) {
  pool.clear();
  vector<string>* set = r.chance_pct(50) ? &s.disk_p : &s.disk_a;
  if (set->empty()) set = set == &s.disk_p ? &s.disk_a : &s.disk_p;
  if (set->empty()) return 0;
  vector<string> files = *set;
  r.shuffle(files);
  for (auto& f : files) {
    if (pool.size() >= DISK_POOL_LINES) break;
    int64_t sz = file_size(f);
    if (sz <= 0 || sz > DISK_FILE_MAX) continue;
    string txt = read_file(f);
    vector<string> lines = split_lines(txt);
    if (lines.empty()) continue;
    size_t take = (size_t)r.below(lines.size()) + 1;
    take = std::min(take, DISK_POOL_LINES - pool.size());
    r.shuffle(lines);
    for (size_t i = 0; i < take; i++) {
      string l = sql_line_cleanup(lines[i]);
      if (l.empty() || s.adv_filter.hit(l)) continue;
      pool.push_back(l);
    }
  }
  return pool.size();
}
bool sources_init(SqlSources& s, const Basedir& b, const string& work, string* err) {
  s.work = work;
  mkdirs(work);
  s.revgen_yacc = yacc_for(b);
  if (!s.sql_filter.compile([] { vector<string> v; for (auto& p : read_pattern_file(g_paths.sql_filter)) v.push_back(bre_to_pcre(p)); return v; }(), err)) return false;
  string adv = trim(read_file(g_paths.adv_filter));
  if (!adv.empty() && !s.adv_filter.compile({adv}, err)) return false;
  if (s.use_infile && !index_infile(s, err)) return false;
  if (s.use_disk) { index_disk(s); if (s.disk_p.empty() && s.disk_a.empty()) { s.use_disk = false; s.notes.push_back("all-disk source off: no .sql file found on the disk"); } }
  if (s.use_rev && s.revgen_yacc.empty()) { s.use_rev = false; s.notes.push_back("revgen off: no grammar under " + g_paths.qa + "/yacc"); }
  return true;
}
void sources_note_executed(SqlSources& s, size_t n) {
  std::lock_guard<std::mutex> lk(s.mtx);
  s.executed.push_back(n);
  while (s.executed.size() > 10) s.executed.pop_front();
}
size_t sources_target_lines(SqlSources& s) {
  std::lock_guard<std::mutex> lk(s.mtx);
  if (s.target) return std::min(s.target, PQUERY_MAX_SQL_LINES);   // given: the driver's figure, or sql --lines
  if (s.executed.empty()) return SQL_LINES_FIRST;
  double sum = 0;
  for (auto n : s.executed) sum += (double)n;
  double t = sum / (double)s.executed.size() * g_cfg.sql_size_factor;
  return (size_t)std::clamp(t, (double)SQL_LINES_MIN, (double)PQUERY_MAX_SQL_LINES);
}
// the random server options of the old ADD_RANDOM_OPTIONS: from the newest options list at or below the series
static string options_list_for(const Basedir& b) {
  string best, best_v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(g_paths.qa + "/pquery", ec)) {
    string n = e.path().filename().string();
    if (!starts_with(n, "mysqld_options_mariadb_") || !ends_with(n, ".txt")) continue;
    string v = n.substr(23);
    v = v.substr(0, v.find_first_not_of("0123456789."));
    if (version_cmp(v, b.series) > 0) continue;
    if (best.empty() || version_cmp(v, best_v) > 0) { best = e.path().string(); best_v = v; }
  }
  return best;
}
static bool encryption_files(const string& trial_dir, const Basedir& b, vector<string>& opts, string* err) {
  string key = trial_dir + "/key.key", pass = trial_dir + "/key.pass", enc = trial_dir + "/key.enc";
  CmdResult r = run_shell("echo \"$(echo -n '1;'; openssl rand -hex 32)\" > " + sh_quote(key) + " && openssl rand -hex 128 > " + sh_quote(pass), 60);
  if (r.rc != 0) { if (err) *err = "openssl key generation failed"; return false; }
  bool sha2 = b.vendor == Vendor::MariaDB && version_at_least(b.version, "12.0.1");
  string encc = sha2 ? "openssl enc -aes-256-cbc -md sha256 -pbkdf2 -iter 11000 -pass file:" + sh_quote(pass) + " -in " + sh_quote(key) + " -out " + sh_quote(enc)
                     : "openssl enc -aes-256-cbc -md sha1 -pass file:" + sh_quote(pass) + " -in " + sh_quote(key) + " -out " + sh_quote(enc) + " 2>/dev/null";
  r = run_shell(encc, 60);
  if (r.rc != 0) { if (err) *err = "openssl enc failed: " + tail_lines(r.out, 2); return false; }
  for (const char* o : {"--plugin_load_add=file_key_management", "--file-key-management=FORCE_PLUS_PERMANENT"}) opts.push_back(o);
  opts.push_back("--file-key-management-filekey=FILE:" + pass);
  opts.push_back("--file-key-management-filename=" + enc);
  opts.push_back("--file-key-management-encryption-algorithm=AES_CBC");
  if (sha2) { opts.push_back("--file_key_management_use_pbkdf2=11000"); opts.push_back("--file_key_management_digest=sha256"); }
  return true;
}
static bool run_generator(SqlSources& s, const string& out, size_t n, uint64_t seed, bool rocksdb, vector<string>& lines, string* err) {
  int threads = std::max(1, cpu_threads() / 4);
  string log = out + ".log";
  pid_t pid = spawn_program({self_exe(), "--role", "generator", "--threads", std::to_string(threads), "--seed", std::to_string(seed), "--output", out, std::to_string(n)},
                            log, s.work, true);
  if (pid <= 0) { if (err) *err = "cannot start the generator"; return false; }
  int st = wait_pid(pid, GEN_TIMEOUT_S * 1000);
  if (st == -1) { kill_group(pid, SIGKILL); wait_pid(pid, 5000); if (err) *err = "generator timed out"; return false; }
  if (st != 0) { if (err) *err = fmt("generator rc %d, see %s", st, log.c_str()); return false; }
  for (auto& l : split_lines(read_file(out))) {
    if (l.empty()) continue;
    string x = l;
    if (!rocksdb) x = ireplace_all(x, "RocksDB", "InnoDB", true);
    x = ireplace_all(x, "TokuDB", "InnoDB", true);
    lines.push_back(x);
  }
  unlink(out.c_str());
  unlink(log.c_str());
  return true;
}
static bool run_revgen(SqlSources& s, const string& out, size_t n, uint64_t seed, bool multi_thread, vector<string>& lines, string* err) {
  int threads = std::max(1, cpu_threads() / 4);
  string lex = s.revgen_yacc.substr(0, s.revgen_yacc.size() - 12) + "_lex.h";
  vector<string> argv = {self_exe(), "--role", "revgen", "--threads", std::to_string(threads), "--yacc", s.revgen_yacc, "--seed", std::to_string(seed),
                         "--output", out, "--queries", std::to_string(n), "--depth", "10", "--schema-every", "25"};
  if (file_exists(lex)) { argv.push_back("--lex"); argv.push_back(lex); }
  if (multi_thread) argv.push_back("--allow-locking");
  string log = out + ".log";
  pid_t pid = spawn_program(argv, log, s.work, true);
  if (pid <= 0) { if (err) *err = "cannot start revgen"; return false; }
  int st = wait_pid(pid, GEN_TIMEOUT_S * 1000);
  if (st == -1) { kill_group(pid, SIGKILL); wait_pid(pid, 5000); if (err) *err = "revgen timed out"; return false; }
  if (st != 0) { if (err) *err = fmt("revgen rc %d, see %s", st, log.c_str()); return false; }
  for (auto& l : split_lines(read_file(out))) if (!l.empty()) lines.push_back(l);
  unlink(out.c_str());
  unlink(log.c_str());
  return true;
}
string TrialSql::seed_text() const {
  return fmt("area=%s\ngenerator=%llu\nrevgen=%llu\nshuffle=%llu\noptions=%llu\n", area.c_str(), (unsigned long long)seed_gen,
             (unsigned long long)seed_rev, (unsigned long long)seed_shuffle, (unsigned long long)seed_opt);
}
bool sources_assemble(SqlSources& s, const Basedir& b, const Area& a, Xoshiro256pp& r, const string& trial_dir, bool multi_thread, TrialSql& t, string* err) {
  t = TrialSql();
  t.area = a.name;
  t.seed_gen = r.next(); t.seed_rev = r.next(); t.seed_shuffle = r.next(); t.seed_opt = r.next();
  mkdirs(trial_dir);
  // the server options
  Xoshiro256pp ro; ro.seed(t.seed_opt);
  if (!a.myextra_choices.empty()) {
    string choice = a.myextra_choices[ro.below(a.myextra_choices.size())];
    // the deadlock options are MariaDB's own; MySQL and Percona refuse them at startup
    for (auto& o : split_ws(choice)) if (b.vendor == Vendor::MariaDB || !starts_with(o, "--deadlock-")) t.myextra.push_back(o);
  }
  t.myinit = a.myinit;
  bool rocksdb = false;
  for (auto& o : t.myextra) if (icontains(o, "rocksdb")) rocksdb = true;
  if (a.encryption) {
    t.encryption = true;
    if (!encryption_files(trial_dir, b, t.myextra, err)) return false;
    for (auto& o : split_ws(a.encryption_items)) t.myextra.push_back(o);
  }
  if (ro.chance_pct(RANDOM_OPTIONS_PCT)) {
    string list = options_list_for(b);
    vector<string> opts;
    for (auto& l : split_lines(read_file(list))) if (!trim(l).empty() && l.find("query_alloc_block_size=1125899906842624") == string::npos) opts.push_back(trim(l));
    if (!opts.empty()) {
      int n = (int)ro.range(1, 3);
      for (int i = 0; i < n; i++) { t.random_options.push_back(opts[ro.below(opts.size())]); t.myextra.push_back(t.random_options.back()); }
    }
  }
  if (!a.preload_file.empty()) {
    string p = g_paths.qa + "/" + a.preload_file;
    if (file_exists(p)) t.preload_path = p;
  }
  // the SQL
  size_t target = sources_target_lines(s);
  int active = (s.use_gen ? 1 : 0) + (s.use_rev ? 1 : 0) + (s.use_infile ? 1 : 0) + (s.use_disk ? 1 : 0);
  if (active == 0) { if (err) *err = "no SQL source is on"; return false; }
  size_t share = std::max<size_t>(100, target / (size_t)active);
  vector<string> lines, gen, rev;
  string e1, e2;
  std::thread tg, tr;
  bool okg = true, okr = true;
  if (s.use_gen) tg = std::thread([&] { okg = run_generator(s, trial_dir + "/gen.sql", share, t.seed_gen, rocksdb, gen, &e1); });
  if (s.use_rev) tr = std::thread([&] { okr = run_revgen(s, trial_dir + "/rev.sql", share, t.seed_rev, multi_thread, rev, &e2); });
  if (tg.joinable()) tg.join();
  if (tr.joinable()) tr.join();
  if (!okg) { if (err) *err = e1; return false; }
  if (!okr) { if (err) *err = e2; return false; }
  t.gen_lines = gen.size(); t.rev_lines = rev.size();
  for (auto& l : gen) lines.push_back(sql_line_cleanup(l));
  for (auto& l : rev) lines.push_back(sql_line_cleanup(l));
  if (s.use_infile && s.infile_lines > 0) {
    for (size_t i = 0; i < share; i++) {
      string l = sql_line_cleanup(infile_line(s, (size_t)r.below(s.infile_lines)));
      if (l.empty() || s.adv_filter.hit(l)) { t.filtered++; continue; }
      lines.push_back(l);
      t.infile_lines++;
    }
  }
  if (s.use_disk) {
    std::lock_guard<std::mutex> lk(s.mtx);
    string pool_file = s.work + "/disk_pool.txt";
    if (file_exists(pool_file)) {                             // the driver's pool, refreshed every 45 trials
      int64_t mt = file_mtime(pool_file);
      if (s.disk_pool.empty() || mt != s.disk_pool_mtime) {
        s.disk_pool.clear();
        for (auto& l : split_lines(read_file(pool_file))) if (!l.empty()) s.disk_pool.push_back(l);
        s.disk_pool_mtime = mt;
      }
    } else if (s.disk_pool.empty() || s.disk_pool_age >= DISK_POOL_EVERY) {
      collect_disk_pool(s, r, s.disk_pool);
      s.disk_pool_age = 0;
    }
    s.disk_pool_age++;
    if (!s.disk_pool.empty()) {
      size_t n = std::min(share, s.disk_pool.size());
      size_t start = (size_t)r.below(s.disk_pool.size());
      for (size_t i = 0; i < n; i++) lines.push_back(s.disk_pool[(start + i) % s.disk_pool.size()]);
      t.disk_lines = n;
    }
  }
  if (lines.size() > PQUERY_MAX_SQL_LINES) lines.resize(PQUERY_MAX_SQL_LINES);
  {
    vector<string> kept;
    kept.reserve(lines.size());
    for (auto& l : lines) { if (s.sql_filter.hit(l)) { t.filtered++; continue; } kept.push_back(l); }
    lines.swap(kept);
  }
  Xoshiro256pp rs; rs.seed(t.seed_shuffle);
  rs.shuffle(lines);
  if (!a.engine_swap.empty()) sql_engine_swap(lines, a.engine_swap, a.engine_swap_pct);
  if (!a.interleave.empty()) sql_interleave(lines, replace_all(a.interleave, "__FEDERATED_CONN__", s.federated_conn), a.interleave_lines);
  if (a.table_swap_create) sql_swap_create_table_names(lines);
  if (a.table_swap_all) sql_swap_all_table_names(lines);
  t.lines = lines.size();
  t.sql_path = trial_dir + "/trial.sql";
  string out;
  out.reserve(lines.size() * 80);
  for (auto& l : lines) { out += l; out += '\n'; }
  if (!write_file(t.sql_path, out)) { if (err) *err = "cannot write " + t.sql_path; return false; }
  write_file(trial_dir + "/SEED", t.seed_text());
  write_file(trial_dir + "/MYEXTRA", join(t.myextra, " ") + "\n");
  write_file(trial_dir + "/MYINIT", t.myinit + "\n");
  return true;
}

// the per-run files every trial worker reads: the extracted INFILE, the disk index, the pool
bool sources_shared_prepare(const string& work, string* err) {
  mkdirs(work);
  SqlSources s;
  s.work = work;
  string adv = trim(read_file(g_paths.adv_filter));
  if (!adv.empty() && !s.adv_filter.compile({adv}, err)) return false;
  if (!index_infile(s, err)) return false;
  if (g_cfg.all_disk_sql) {
    index_disk(s);
    if (!s.disk_p.empty() || !s.disk_a.empty()) {
      Xoshiro256pp r = rng_stream(11);
      vector<string> pool;
      collect_disk_pool(s, r, pool);
      write_file(work + "/disk_pool.txt", join(pool, "\n") + "\n");
    }
  }
  return true;
}
void sources_shared_refresh_pool(const string& work, Xoshiro256pp& r) {
  SqlSources s;
  s.work = work;
  string adv = trim(read_file(g_paths.adv_filter));
  string err;
  if (!adv.empty()) s.adv_filter.compile({adv}, &err);
  index_disk(s);
  vector<string> pool;
  collect_disk_pool(s, r, pool);
  if (!pool.empty()) write_file(work + "/disk_pool.txt", join(pool, "\n") + "\n");
}

// ---------------------------------------------------------------------------------------------
// verbs: omnium areas, omnium sql
// ---------------------------------------------------------------------------------------------
int cmd_areas(const Args& a) {
  Basedir b;
  bool have = !a.empty() && basedir_probe(a[0].find('/') == string::npos ? g_cfg.test_dir + "/" + a[0] : a[0], b);
  if (!a.empty() && !have) { printf("not a basedir: %s\n", a[0].c_str()); return 1; }
  string head = have ? "on " + b.name : string("note (name a basedir to see what it can run)");
  printf("%-18s %6s  %s\n", "area", "weight", head.c_str());
  for (auto& x : areas_all()) {
    string why;
    bool ok = !have || area_available(x, b, &why);
    printf("%-18s %6d  %s%s\n", x.name.c_str(), x.weight, x.note.c_str(), have && !ok ? (" [off: " + why + "]").c_str() : "");
  }
  return 0;
}
int cmd_sql(const Args& a) {
  if (a.empty()) { printf("usage: omnium sql <basedir> [--area NAME] [--out FILE] [--no-disk] [--lines N]\n  assembles the SQL of one trial the way a run does, into FILE (default ./trial.sql)\n"); return 2; }
  string bd = a[0].find('/') == string::npos ? g_cfg.test_dir + "/" + a[0] : a[0], area, out = "trial.sql";
  bool disk = g_cfg.all_disk_sql;
  size_t lines = 0;
  for (size_t i = 1; i < a.size(); i++) {
    if (a[i] == "--area" && i + 1 < a.size()) area = a[++i];
    else if (a[i] == "--out" && i + 1 < a.size()) out = a[++i];
    else if (a[i] == "--no-disk") disk = false;
    else if (a[i] == "--lines" && i + 1 < a.size()) lines = (size_t)to_long(a[++i]);
    else { printf("unexpected argument %s\n", a[i].c_str()); return 2; }
  }
  Basedir b;
  if (!basedir_probe(bd, b)) { printf("not a basedir: %s\n", bd.c_str()); return 1; }
  string work = fmt("%s/omnium_sql_%d", g_cfg.shm_dir.c_str(), getpid());
  SqlSources s;
  s.use_disk = disk;
  string err;
  double t0 = now_ms();
  if (!sources_init(s, b, work, &err)) { printf("%s\n", err.c_str()); remove_tree(work); return 1; }
  for (auto& n : s.notes) printf("note: %s\n", n.c_str());
  s.target = lines;
  Xoshiro256pp r = rng();
  const Area* ar = area.empty() ? area_pick(areas_available(b, {}), r) : area_by_name(area);
  if (!ar) { printf("no such area: %s (omnium areas lists them)\n", area.c_str()); remove_tree(work); return 1; }
  TrialSql t;
  if (!sources_assemble(s, b, *ar, r, work + "/trial", false, t, &err)) { printf("%s\n", err.c_str()); remove_tree(work); return 1; }
  string sql = read_file(t.sql_path);
  if (!write_file(out, sql)) { printf("cannot write %s\n", out.c_str()); remove_tree(work); return 1; }
  printf("%s: %zu lines (generator %zu, revgen %zu, INFILE %zu, all-disk %zu; %zu filtered) in %.1f s\narea %s\nMYEXTRA %s\n%s",
         out.c_str(), t.lines, t.gen_lines, t.rev_lines, t.infile_lines, t.disk_lines, t.filtered, (now_ms() - t0) / 1000.0,
         t.area.c_str(), join(t.myextra, " ").c_str(), t.seed_text().c_str());
  if (!t.preload_path.empty()) printf("preload %s\n", t.preload_path.c_str());
  remove_tree(work);
  return 0;
}
