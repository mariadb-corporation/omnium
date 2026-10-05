// Created by Roel Van de Paar, MariaDB
// Basedirs (the /test name grammar, the version banner myver prints today), datadir templates,
// server instances (start, ready probe, shutdown, kill), ports and the sanitizer environment.
#include "common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "connect.h"

// ---- name grammar ----------------------------------------------------------------------------
// [FLAVOUR_][TAG_](E?MD|MS|PS)<ddmmyy>-(mariadb|mysql|percona-server)-<version>-linux-x86_64-(opt|dbg)
bool basedir_parse_name(const string& name, Basedir& b) {
  string n = name;
  b.name = name;
  b.flavour = Flavour::Plain;
  const std::pair<const char*, Flavour> flav[] = {
    {"UBASAN_", Flavour::UBASAN}, {"MSAN_", Flavour::MSAN}, {"TSAN_", Flavour::TSAN},
    {"VAL_", Flavour::VAL}, {"GAL_", Flavour::GAL}};
  for (auto& [p, f] : flav) if (starts_with(n, p)) { b.flavour = f; n = n.substr(strlen(p)); break; }
  // the vendor marker with its date: the first E?MD, MS or PS followed by six digits and a dash
  size_t mark = string::npos;
  for (size_t i = 0; i + 8 <= n.size(); i++) {
    size_t j = i;
    if (n.compare(j, 3, "EMD") == 0) j += 3;
    else if (n.compare(j, 2, "MD") == 0 || n.compare(j, 2, "MS") == 0 || n.compare(j, 2, "PS") == 0) j += 2;
    else continue;
    if (j + 7 <= n.size() && is_digits(n.substr(j, 6)) && n[j + 6] == '-') { mark = i; break; }
  }
  if (mark == string::npos) return false;
  b.tag = mark ? n.substr(0, mark) : "";
  while (!b.tag.empty() && (b.tag.back() == '_' || b.tag.back() == '-')) b.tag.pop_back();
  n = n.substr(mark);
  if (starts_with(n, "EMD")) { b.vendor = Vendor::MariaDB; b.es = true; n = n.substr(3); }
  else if (starts_with(n, "MD")) { b.vendor = Vendor::MariaDB; b.es = false; n = n.substr(2); }
  else if (starts_with(n, "MS")) { b.vendor = Vendor::MySQL; n = n.substr(2); }
  else { b.vendor = Vendor::Percona; n = n.substr(2); }
  b.date = n.substr(0, 6);
  n = n.substr(7);                                   // past the dash
  // <product>-<version>-<linux|windows>-x86_64-<opt|dbg>
  size_t lin = n.find("-linux-"), skip = 7;
  if (lin == string::npos) { lin = n.find("-windows-"); skip = 9; }
  if (lin == string::npos) return false;
  b.windows = (skip == 9);
  string tail = n.substr(lin + skip);                // x86_64-opt
  b.dbg = ends_with(tail, "-dbg");
  if (!b.dbg && !ends_with(tail, "-opt")) return false;
  string prodver = n.substr(0, lin);                 // mariadb-13.1.0 or mysql-8.0.36 or percona-server-8.0.x
  size_t dash = prodver.find('-');
  while (dash != string::npos && dash + 1 < prodver.size() && !isdigit((unsigned char)prodver[dash + 1]))
    dash = prodver.find('-', dash + 1);
  if (dash == string::npos) return false;
  b.version = prodver.substr(dash + 1);
  auto parts = split(b.version, '.');
  b.series = parts.size() >= 2 ? parts[0] + "." + parts[1] : b.version;
  return true;
}

string Basedir::vendor_str() const {
  switch (vendor) {
    case Vendor::MariaDB: return es ? "ES" : "CS";
    case Vendor::MySQL: return "MS";
    case Vendor::Percona: return "PS";
    default: return "";
  }
}
string Basedir::flavour_str() const {
  switch (flavour) {
    case Flavour::UBASAN: return "UBASAN";
    case Flavour::MSAN: return "MSAN";
    case Flavour::TSAN: return "TSAN";
    case Flavour::VAL: return "VAL";
    case Flavour::GAL: return "GAL";
    default: return "";
  }
}
string Basedir::short_name() const {
  string s;
  if (!tag.empty()) s = tag;
  else {
    if (es) s = "es-";
    else if (vendor == Vendor::MySQL) s = "ms-";
    else if (vendor == Vendor::Percona) s = "ps-";
    s += series;
  }
  switch (flavour) {
    case Flavour::UBASAN: s += "-uba"; break;
    case Flavour::MSAN: s += "-msan"; break;
    case Flavour::TSAN: s += "-tsan"; break;
    case Flavour::VAL: s += "-val"; break;
    case Flavour::GAL: s += "-gal"; break;
    default: break;
  }
  return s + (dbg ? "-dbg" : "-opt");
}

static string pick_exe(const string& dir, std::initializer_list<const char*> names) {
  for (auto n : names) if (is_executable(dir + "/" + n)) return dir + "/" + n;
  return {};
}

bool basedir_probe(const string& path_in, Basedir& b) {
  string path = abs_path(path_in);
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  b = Basedir();
  b.path = path;
  basedir_parse_name(basename_of(path), b);
  b.name = basename_of(path);
  b.bin = pick_exe(path + "/bin", {"mariadbd", "mysqld", "mysqld-debug"});
  if (b.bin.empty()) {                                 // an in-tree build: sql/mariadbd
    b.bin = pick_exe(path + "/sql", {"mariadbd", "mysqld"});
    if (b.bin.empty()) return false;
    b.in_tree = true;
    b.client = pick_exe(path + "/client", {"mariadb", "mysql"});
    b.admin = pick_exe(path + "/client", {"mariadb-admin", "mysqladmin"});
    b.dump = pick_exe(path + "/client", {"mariadb-dump", "mysqldump"});
  } else {
    b.client = pick_exe(path + "/bin", {"mariadb", "mysql"});
    b.admin = pick_exe(path + "/bin", {"mariadb-admin", "mysqladmin"});
    b.dump = pick_exe(path + "/bin", {"mariadb-dump", "mysqldump"});
    b.backup = pick_exe(path + "/bin", {"mariadb-backup", "mariabackup"});
  }
  if (b.vendor == Vendor::Unknown) {
    // no name grammar: ask the binary
    CmdResult r = run_capture({b.bin, "--version"}, 20);
    if (r.out.find("MariaDB") != string::npos) { b.vendor = Vendor::MariaDB; b.es = r.out.find("enterprise") != string::npos; }
    else if (r.out.find("Percona") != string::npos) b.vendor = Vendor::Percona;
    else b.vendor = Vendor::MySQL;
    string v;
    for (auto& tok : split_ws(r.out)) if (isdigit((unsigned char)tok[0]) && tok.find('.') != string::npos) { v = tok; break; }
    size_t dash = v.find("-MariaDB");
    if (dash != string::npos) v = v.substr(0, dash);
    b.version = v;
    auto parts = split(v, '.');
    b.series = parts.size() >= 2 ? parts[0] + "." + parts[1] : v;
    b.dbg = r.out.find("debug") != string::npos;
    b.name = basename_of(path);
  }
  // init tool: MariaDB and MySQL <= 5.6 use an install script, MySQL 5.7+ the server
  if (b.vendor == Vendor::MariaDB) {
    b.init_tool = pick_exe(path + "/scripts", {"mariadb-install-db", "mysql_install_db"});
    if (b.init_tool.empty()) b.init_tool = pick_exe(path + "/bin", {"mariadb-install-db", "mysql_install_db"});
  } else if (version_at_least(b.version, "5.7")) {
    b.init_tool = b.bin;
    b.init_via_bin = true;
  } else {
    b.init_tool = pick_exe(path + "/scripts", {"mysql_install_db"});
  }
  b.cmake_cmd = trim(read_file(path + "/BUILD_CMD_CMAKE"));
  b.git_rev = basedir_source_rev(b);
  return true;
}

vector<Basedir> basedirs_scan(const string& test_dir) {
  vector<Basedir> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(test_dir, ec)) {
    if (!e.is_directory(ec)) continue;
    string n = e.path().filename().string();
    Basedir b;
    if (!basedir_parse_name(n, b)) continue;
    if (!is_executable(e.path().string() + "/bin/mariadbd") && !is_executable(e.path().string() + "/bin/mysqld")) continue;
    b.path = e.path().string();
    b.bin = is_executable(b.path + "/bin/mariadbd") ? b.path + "/bin/mariadbd" : b.path + "/bin/mysqld";
    b.git_rev = basedir_source_rev(b, true);
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const Basedir& a, const Basedir& c) { return a.name < c.name; });
  return out;
}

static bool is_sha40(const string& s) {
  if (s.size() != 40) return false;
  for (char c : s) if (!isxdigit((unsigned char)c)) return false;
  return true;
}
string basedir_source_rev(const Basedir& b, bool files_only) {
  string r = trim(read_file(b.path + "/git_revision.txt"));
  if (is_sha40(r)) return r;
  string h = read_file(b.path + "/include/mysql/server/private/source_revision.h");
  if (!h.empty()) {
    size_t q = h.find('"');
    if (q != string::npos) { size_t q2 = h.find('"', q + 1); if (q2 != string::npos) { r = h.substr(q + 1, q2 - q - 1); if (is_sha40(r)) return r; } }
  }
  for (const char* f : {"/docs/INFO_SRC", "/Docs/INFO_SRC"}) {
    string s = read_file(b.path + f);
    size_t p = s.find("commit");
    if (p != string::npos) { auto w = split_ws(s.substr(p)); if (w.size() > 1 && is_sha40(w[1])) return w[1]; }
  }
  if (files_only) return "";
  // last resort: the string the server carries ("Source control revision id for MariaDB source code" then the id)
  CmdResult res = run_shell("strings " + sh_quote(b.bin) + " | grep -A1 -m1 '^Source control revision id for MariaDB source code' | tail -1", 60);
  r = trim(res.out);
  return is_sha40(r) ? r : "unknown";
}

// clang version as the banner shows it: from the .comment section of the binary, then as
// version_chk_helper shapes it (the git URL dropped, "(1)" to "-1", a local clang with the same
// SHA adds its build date)
static string clang_version_of(const Basedir& b) {
  CmdResult r = run_capture({"readelf", "-p", ".comment", b.bin}, 60);
  string line;
  for (auto& l : split_lines(r.out)) if (icontains(l, "clang version")) { line = l; break; }
  if (line.empty()) {
    if (icontains(b.cmake_cmd, "clang")) return " Clang";
    return {};
  }
  size_t cv = line.find("clang version");
  line = line.substr(cv);
  // drop "(http...)" 
  size_t par = line.find(" (http");
  string sha;
  if (par != string::npos) {
    string inside = line.substr(par);
    for (auto& t : split_ws(inside)) { string tt = t; while (!tt.empty() && (tt.back() == ')' || tt.back() == ',')) tt.pop_back(); if (is_sha40(tt)) sha = tt; }
    line = line.substr(0, par);
  }
  string ver;
  for (char c : line) { if (isdigit((unsigned char)c) || c == '.' || c == '(') ver += c; }
  ver = replace_all(ver, "(", "-");
  while (!ver.empty() && (ver.back() == '.' || ver.back() == '-')) ver.pop_back();
  while (!ver.empty() && !isdigit((unsigned char)ver[0])) ver.erase(0, 1);
  if (ver.empty()) return " Clang";
  if (!sha.empty()) {
    string local = is_executable("/usr/local/bin/clang") ? "/usr/local/bin/clang" : "";
    if (local.empty()) { CmdResult w = run_capture({"which", "clang"}, 10); local = trim(w.out); }
    if (!local.empty()) {
      CmdResult v = run_capture({local, "--version"}, 20);
      if (v.out.find(sha) != string::npos) {
        struct stat st;
        if (stat(local.c_str(), &st) == 0) {
          struct tm lt; localtime_r(&st.st_mtime, &lt);
          char d[16]; strftime(d, sizeof(d), "%Y%m%d", &lt);
          ver += string("-") + d;
        }
      }
    }
  }
  return " Clang " + ver;
}

string basedir_banner_title(const Basedir& b) {
  string svr = b.vendor_str();
  if (!b.tag.empty()) svr = b.tag + " " + svr;
  string version = b.version;
  if (b.vendor == Vendor::MySQL) version = "MySQL " + b.version;
  string rev = b.git_rev.empty() ? basedir_source_rev(b) : b.git_rev;
  string clang = clang_version_of(b);
  string type = b.dbg ? "Debug" : "Optimized";
  string san = b.flavour_str();
  string bt = " (" + type + (san.empty() ? "" : ", " + san) + (clang.empty() ? "" : "," + clang) + ")";
  string date;
  if (b.date.size() == 6) date = " Build " + b.date.substr(0, 2) + "/" + b.date.substr(2, 2) + "/20" + b.date.substr(4, 2);
  return svr + " " + version + " " + rev + bt + date;
}
string basedir_banner(const Basedir& b) {
  return "{noformat:title=" + basedir_banner_title(b) + "}\n\n{noformat}";
}

int version_cmp(const string& a, const string& b) {
  auto num = [](const string& v) {
    vector<long> out;
    string cur;
    for (char c : v + ".") {
      if (isdigit((unsigned char)c)) cur += c;
      else { if (!cur.empty()) out.push_back(to_long(cur)); cur.clear(); if (c == '-') break; }
    }
    return out;
  };
  auto x = num(a), y = num(b);
  for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
    long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}
bool version_at_least(const string& v, const string& floor) { return version_cmp(v, floor) >= 0; }

// ---- sanitizer environment and MYSAFE ---------------------------------------------------------
vector<string> san_env_for(const Basedir& b) {
  vector<string> out;
  if (!b.is_san()) return out;
  string text = read_file(g_paths.san_opt);
  for (auto& l : split_lines(text)) {
    string s = trim(l);
    if (s.empty() || s[0] == '#') continue;
    s = replace_all(s, "QA_DIR", g_paths.qa);
    bool want = (b.flavour == Flavour::UBASAN && (starts_with(s, "ASAN_OPTIONS=") || starts_with(s, "UBSAN_OPTIONS="))) ||
                (b.flavour == Flavour::MSAN && (starts_with(s, "MSAN_OPTIONS=") || starts_with(s, "UBSAN_OPTIONS="))) ||
                (b.flavour == Flavour::TSAN && starts_with(s, "TSAN_OPTIONS="));
    if (want) out.push_back(s);
  }
  return out;
}

string mysafe_options(const Basedir& b) {
  string s = "--no-defaults --loose-innodb-buffer-pool-in-core-dump=0 --max_allowed_packet=33554432 "
             "--maximum-bulk_insert_buffer_size=1M --maximum-join_buffer_size=1M --maximum-max_heap_table_size=1M "
             "--maximum-max_join_size=1M --maximum-myisam_max_sort_file_size=1M --maximum-myisam_mmap_size=1M "
             "--maximum-myisam_sort_buffer_size=1M --maximum-optimizer_trace_max_mem_size=1M --maximum-preload_buffer_size=1M "
             "--maximum-query_alloc_block_size=1M --maximum-query_prealloc_size=1M --maximum-range_alloc_block_size=1M "
             "--maximum-read_buffer_size=1M --maximum-read_rnd_buffer_size=1M --maximum-sort_buffer_size=1M "
             "--maximum-tmp_table_size=1M --maximum-transaction_alloc_block_size=1M --maximum-transaction_prealloc_size=1M "
             "--log-output=none --sql_mode=";
  // the optimizer trace came with MySQL 5.6 and MariaDB 10.4; an older server refuses the option
  if (!b.version.empty() && !version_at_least(b.version, b.vendor == Vendor::MariaDB ? "10.4" : "5.6"))
    s = replace_all(s, "--maximum-optimizer_trace_max_mem_size=1M ", "");
  // MSAN: the in-log symbolised trace is very slow on the huge binary and the core may never be
  // written before the server is stopped; the UID comes from gdb on the core instead
  if (b.flavour == Flavour::MSAN) s += " --skip-stack-trace";
  // TSAN and Valgrind cannot map the 8 TiB buffer pool reservation of 13.0+
  if (b.flavour == Flavour::TSAN || b.flavour == Flavour::VAL) s += " --loose-innodb-buffer-pool-size-max=2G";
  return s;
}

// ---- datadir templates -------------------------------------------------------------------------
static std::mutex g_tpl_mtx;
static std::map<string, string> g_tpl_ready;
static std::set<string> g_tpl_inflight;
static std::condition_variable g_tpl_cv;

vector<string> install_db_argv(const Basedir& b, const string& datadir, const string& tmpdir, const string& myinit) {
  vector<string> argv;
  if (b.windows) {
    // mariadb-install-db.exe has its own option set: no --no-defaults, no --basedir, no server
    // options but the page size. It writes a my.ini into the datadir; the server runs --no-defaults.
    argv = {b.init_tool, "--datadir=" + datadir};
    for (auto& o : split_ws(myinit)) if (starts_with(o, "--innodb-page-size=") || starts_with(o, "--innodb_page_size=")) argv.push_back("--innodb-page-size=" + o.substr(o.find('=') + 1));
    return argv;
  }
  if (b.init_via_bin) {
    argv = {b.init_tool, "--no-defaults", "--initialize-insecure", "--basedir=" + b.path, "--datadir=" + datadir, "--tmpdir=" + tmpdir};
  } else {
    argv = {b.init_tool, "--no-defaults", "--force", "--basedir=" + b.path, "--datadir=" + datadir, "--tmpdir=" + tmpdir};
    if (b.vendor == Vendor::MariaDB) argv.push_back("--auth-root-authentication-method=normal");
  }
  for (auto& o : split_ws(myinit)) argv.push_back(o);
  for (auto& o : split_ws(mysafe_options(b))) if (starts_with(o, "--loose-innodb-buffer-pool-size-max")) argv.push_back(o);
  return argv;
}
// The options of a server option string that shape the datadir when it is made, so they have to reach the
// template as well as the server: a server refuses a datadir made with another page size ("Data file
// './ibdata1' uses page size 16384, but the innodb_page_size start-up parameter is 4096"). That is MYINIT.
string myinit_from(const string& options) {
  string out;
  if (!kTakeFixes) return out;                                  // Linux has the same gap; it keeps its behaviour until it is decided
  for (auto& o : split_ws(options)) {
    string k = lower(o.substr(0, o.find('=')));
    for (char& c : k) if (c == '_') c = '-';
    if (k == "--innodb-page-size" || k == "--innodb-undo-tablespaces" || k == "--innodb-data-file-path" || k == "--lower-case-table-names")
      out += (out.empty() ? "" : " ") + o;
  }
  return out;
}
string template_for(const Basedir& b, const string& myinit, const string& templates_root) {
  // the key carries the root: two roots hold two templates, and a root removed after use
  // (the matrix does that) must not hand its path to the next caller
  string key = templates_root + "|" + b.path + "|" + myinit;
  {
    std::unique_lock<std::mutex> lk(g_tpl_mtx);
    for (;;) {
      auto it = g_tpl_ready.find(key);
      if (it != g_tpl_ready.end()) {
        if (dir_exists(it->second + "/mysql")) return it->second;
        g_tpl_ready.erase(it);                                   // the template is gone; make it again
      }
      if (!g_tpl_inflight.count(key)) { g_tpl_inflight.insert(key); break; }
      g_tpl_cv.wait(lk);
    }
  }
  string tpl = templates_root + "/" + b.name + "-" + fmt("%08x", (uint32_t)fnv1a(key));
  string done_mark = tpl + ".ready";
  auto finish = [&](const string& result) {
    std::lock_guard<std::mutex> lk(g_tpl_mtx);
    if (!result.empty()) g_tpl_ready[key] = result;
    g_tpl_inflight.erase(key);
    g_tpl_cv.notify_all();
    return result;
  };
  // a template a previous run of this workdir made (resume) is taken as it is
  if (file_exists(done_mark) && dir_exists(tpl + "/mysql")) return finish(tpl);
  mkdirs(templates_root);
  string tmp = tpl + ".tmp";
  string initlog = templates_root + "/" + b.name + "-init.log";
  if (b.init_tool.empty()) { logwarn("%s: no init tool (mariadb-install-db / mysql_install_db)", b.name.c_str()); return finish(""); }
  for (int attempt = 1; attempt <= 10; attempt++) {
    remove_tree(tpl); remove_tree(tmp);
    mkdirs(tmp);
    vector<string> argv = install_db_argv(b, tpl, tmp, myinit);
    unlink(initlog.c_str());
    pid_t pid = spawn_program(argv, initlog, templates_root, true, san_env_for(b));
    int st = pid > 0 ? wait_pid(pid, 600000) : -1;
    if (pid > 0 && st == -1) { kill_group(pid, SIGKILL); wait_pid(pid, 5000); }
    int rc = (pid > 0 && st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    size_t files = 0;
    std::error_code ec;
    if (dir_exists(tpl + "/mysql")) for (auto& e : fs::directory_iterator(tpl + "/mysql", ec)) { (void)e; files++; }
    bool sane = files > 50 || file_exists(tpl + "/mysql.ibd");
    if (rc == 0 && sane) {
      remove_tree(tmp);
      write_file(done_mark, now_stamp() + "\n");
      logline("%s: datadir template ready (%s)", b.short_name().c_str(), tpl.c_str());
      return finish(tpl);
    }
    logwarn("%s: template init attempt %d/10 failed (rc=%d, %zu files under mysql/), log: %s", b.short_name().c_str(), attempt, rc, files, initlog.c_str());
    sleep(3);
  }
  return finish("");
}

// ---- ports ------------------------------------------------------------------------------------
// A bind() probe on 127.0.0.1 in [13001, 65000]; 10001-13000 stay for the basedir helpers.
int port_pick() {
  Xoshiro256pp r = rng();
  for (int i = 0; i < 500; i++) {
    int port = (int)r.range(13001, 65000);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    bool ok = bind(fd, (struct sockaddr*)&a, sizeof(a)) == 0;
    close(fd);
    if (ok) return port;
  }
  return 0;
}

// ---- instances -------------------------------------------------------------------------------
void Instance::set_paths(const string& trial_root, const string& datadir_override) {
  root = trial_root;
  datadir = datadir_override.empty() ? root + "/data" : datadir_override;
  tmpdir = root + "/tmp";
  logdir = root + "/log";
  sock = root + "/socket.sock";
  errlog = logdir + "/master.err";
  pidfile = root + "/pid.pid";
  tcp = bd && endpoint_tcp(*bd);
}
vector<string> Instance::argv() const {
  vector<string> a = {bd->bin};
  for (auto& o : split_ws(mysafe_options(*bd))) a.push_back(o);
  for (auto& o : extra) if (!o.empty()) a.push_back(bd->windows ? native_option(o) : o);
  a.push_back("--basedir=" + bd->path);
  a.push_back("--datadir=" + datadir);
  a.push_back("--tmpdir=" + tmpdir);
  a.push_back("--core-file");
  a.push_back("--port=" + std::to_string(port));
  a.push_back("--pid_file=" + pidfile);
  if (!bd->windows) a.push_back("--socket=" + sock);        // on Windows that names a pipe nobody opens
  a.push_back("--log-output=none");
  a.push_back("--log-error=" + errlog);
  if (bd->in_tree) {
    string share = bd->path + "/sql/share";
    if (dir_exists(share)) a.push_back("--lc-messages-dir=" + share);
  }
  for (auto& o : split_ws(vendor_options(*bd))) a.push_back(o);
  return a;
}
// a MySQL or Percona server: no X plugin port (5.7+), and the server id that a 5.7 binlog needs (the
// framework's start scripts give 100)
string vendor_options(const Basedir& b) {
  string s;
  if (b.vendor == Vendor::MySQL && version_at_least(b.version, "5.7")) s += "--loose-mysqlx=OFF ";
  if (b.vendor != Vendor::MariaDB) s += "--server-id=100";
  return trim(s);
}
bool Instance::alive() {
  if (pid <= 0) return false;
  int st;
  pid_t r = waitpid(pid, &st, WNOHANG);
  if (r == pid) { pid = -1; if (!stopping) exit_status = st; return false; }
  return kill(pid, 0) == 0;
}
bool Instance::silent_death() const {
  if (!bd || !bd->windows || stopping || exit_status == -1) return false;
  if (WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0) return false;      // a plain exit
  return !has_dump();                                                              // a minidump says it crashed the ordinary way
}
string Instance::silent_death_uid() const {
  int code = WIFEXITED(exit_status) ? WEXITSTATUS(exit_status) : 128 + WTERMSIG(exit_status);
  return fmt("CRASH_NO_LOG|exit status %d", code);
}
bool Instance::start_fresh(const string& tpl, int timeout_s) {
  mkdirs(root); mkdirs(logdir);
  remove_tree(tmpdir); mkdirs(tmpdir);
  { int fd = open(errlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) close(fd); }
  string why;
  for (int attempt = 1; !copy_tree(tpl, datadir, &why); attempt++) {
    logwarn("datadir copy failed (attempt %d/5): %s", attempt, why.c_str());
    if (attempt == 5) { start_note = "datadir copy failed - " + why; start_failed = true; return false; }
    sleep(1);
  }
  if (port == 0) port = port_pick();
  if (port == 0) { start_note = "no free port"; start_failed = true; return false; }
  return start_only(timeout_s);
}
bool Instance::start_only(int timeout_s) {
  start_failed = false;
  stopping = false;
  exit_status = -1;
  start_note.clear();
  // The server changes to its datadir at startup, so a core lands there. The trial worker moves
  // it to /data as soon as the server is gone.
  pid = spawn_program(argv(), errlog, datadir, true, san_env_for(*bd), !detached);
  if (pid <= 0) { start_failed = true; start_note = "fork failed"; return false; }
  set_oom_score(pid, 500);                                      // Q201: short of memory, the kernel takes a server first
  bool ok = wait_ready(timeout_s);
  if (!ok) start_failed = true;
  return ok;
}
// the server gave up at startup: "[ERROR] Aborting", or "[ERROR] [MY-010119] [Server] Aborting" on MySQL 8.0+
bool log_aborted(const string& log) {
  return log.find("ERROR] Aborting") != string::npos || log.find("[MY-010119]") != string::npos;
}
bool Instance::wait_ready(int timeout_s) {
  double deadline = now_ms() + timeout_s * 1000.0;
  unsigned last_err = 0;
  string last_msg;
  while (now_ms() < deadline) {
    if (!alive()) { start_note = "server exited during start"; return false; }
    if (tcp || access(sock.c_str(), F_OK) == 0) {
      MYSQL* m = mysql_init(nullptr);
      unsigned t = 10;
      mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
      if (endpoint_connect(m, endpoint(), "root", nullptr, 0)) {
        // not every install-db makes a test database (ES does not), and every replay expects one
        mysql_query(m, "CREATE DATABASE IF NOT EXISTS test");
        mysql_close(m);
        return true;
      }
      last_err = mysql_errno(m);
      last_msg = mysql_error(m);
      mysql_close(m);
    }
    // a start that already failed on an option says so in the log; no point in waiting on
    string log = read_file(errlog);
    if (log_aborted(log)) {
      wait_pid(pid, 5000);
      // the [ERROR] lines before it say why
      vector<string> why;
      for (auto& l : split_lines(log)) {
        size_t k = l.find("[ERROR]");
        if (k == string::npos || l.find("Aborting") != string::npos || why.size() >= 3) continue;
        why.push_back(trim(l.substr(k + 7)));
      }
      start_note = "[ERROR] Aborting in the error log" + (why.empty() ? "" : ": " + join(why, " | ").substr(0, 400));
      return false;
    }
    usleep(100000);
  }
  start_note = last_err ? fmt("connect kept failing: %u %s", last_err, last_msg.c_str()) : fmt("not ready within %d s", timeout_s);
  return false;
}
bool Instance::shutdown(int timeout_s, string* note) {
  if (pid <= 0) return true;
  stopping = true;                                              // from here on its end is the one omnium asked for
  // a clean shutdown through the admin client, as the framework does, so shutdown asserts and
  // hangs show; SIGTERM would do the same but the client path is what today's runs exercise.
  // What the admin client says is kept: a refused shutdown reads the same as a hung one otherwise.
  pid_t ap = -1;
  static std::once_flag swept;
  std::call_once(swept, [] { sweep_stale_tmp("/tmp/omnium_shutdown_"); });   // what a killed process left behind
  string alog = fmt("/tmp/omnium_shutdown_%d_%d.log", (int)getpid(), (int)pid);
  if (!bd->admin.empty()) {
    vector<string> argv = {bd->admin, "-uroot"};
    for (auto& x : endpoint_args(endpoint())) argv.push_back(x);
    argv.push_back("shutdown");
    ap = spawn_program(argv, alog, "", true, {}, true);
  } else {
    kill(pid, SIGTERM);
  }
  double deadline = now_ms() + timeout_s * 1000.0;
  bool gone = false, termed = bd->admin.empty();
  int arc = -1;                                                // -1 while the admin client is still running
  while (now_ms() < deadline) {
    if (!alive()) { gone = true; break; }
    // the admin client can be turned away by the trial's own SQL, init_connect for one. That is not
    // a server that will not stop, so SIGTERM takes over: the same clean path, so a shutdown assert
    // or hang still shows.
    if (ap > 0 && arc == -1) {
      arc = wait_pid(ap, 0);
      if (arc != -1 && arc != 0 && !termed) { termed = true; kill(pid, SIGTERM); }
    }
    usleep(100000);
  }
  if (ap > 0 && arc == -1) { arc = wait_pid(ap, 0); if (arc == -1) { kill(ap, SIGKILL); wait_pid(ap, 3000); } }
  if (note) {
    string out = trim(read_file(alog));
    if (bd->admin.empty()) *note = "no admin client in the build, so SIGTERM was sent";
    else if (arc == -1) *note = "the admin client was still running when the wait ended" + (out.empty() ? "" : ": " + out);
    else *note = fmt("the admin client ended %d", arc) + string(termed ? " and SIGTERM was sent as well" : "") + (out.empty() ? "" : ": " + out);
  }
  unlink(alog.c_str());
  return gone;
}
void Instance::kill_hard() {
  if (pid <= 0) return;
  stopping = true;
  kill_group(pid, SIGKILL);
  wait_pid(pid, 10000);
  pid = -1;
}
// the test tree of a build: MariaDB names it mariadb-test, older and MySQL builds mysql-test
string basedir_test_dir(const Basedir& b) {
  for (const char* n : {"/mariadb-test", "/mysql-test"}) if (dir_exists(b.path + n)) return b.path + n;
  return "";
}

// ldd_files.sh in the binary: the server binary and every library it needs, into one directory, so
// the trial can be read on another box. With a core, the libraries gdb lists for that core come too.
bool ldd_copy(const string& binary, const string& dir, const string& core, vector<string>* copied, string* err) {
  if (!file_exists(binary)) { if (err) *err = "no binary at " + binary; return false; }
  mkdirs(dir);
  string base = basename_of(binary);
  auto take = [&](const string& src, const string& into, bool overwrite) {
    if (src.empty() || src[0] != '/' || !file_exists(src)) return;
    string dst = into + "/" + basename_of(src);
    if (!overwrite && file_exists(dst)) return;
    if (copy_file(src, dst) && copied) copied->push_back(basename_of(src));
  };
  if (abs_path(binary) != abs_path(dir + "/" + base)) copy_file(binary, dir + "/" + base);
  CmdResult r = run_capture({"ldd", binary}, 120);
  for (auto& l : split_lines(r.out)) {
    string s = trim(l);
    size_t arrow = s.find("=>");
    string path = arrow == string::npos ? s : trim(s.substr(arrow + 2));
    size_t sp = path.find(" (0x");
    if (sp != string::npos) path = trim(path.substr(0, sp));
    if (path.empty() || path[0] != '/') continue;              // "linux-vdso.so.1" and the like
    take(path, dir, false);
  }
  if (!core.empty() && file_exists(core)) {
    // gdb knows which libraries that core was made with; /lib64 ones go in a lib64 subdir
    CmdResult g = run_capture({"gdb", "-iex", "set debuginfod enabled off", binary, core, "-ex", "info sharedlibrary", "-ex", "quit"}, 600);
    for (auto& l : split_lines(g.out)) {
      if (l.find("0x") == string::npos || l.find("lib/mysql/plugin") != string::npos || starts_with(trim(l), "#0 ")) continue;
      size_t k = l.rfind(" /");
      if (k == string::npos) continue;
      string path = trim(l.substr(k + 1));
      if (path.empty() || path[0] != '/') continue;
      if (starts_with(path, "/lib64/")) { mkdirs(dir + "/lib64"); take(path, dir + "/lib64", false); }
      else take(path, dir, false);
    }
  }
  return true;
}

string Instance::core_path() const {
  std::error_code ec;
  string best;
  int64_t best_t = 0;
  for (auto& e : fs::directory_iterator(datadir, ec)) {
    string n = e.path().filename().string();
    if (n.find("core") == string::npos) continue;
    if (!e.is_regular_file(ec)) continue;
    int64_t t = file_mtime(e.path().string());
    if (best.empty() || t > best_t) { best = e.path().string(); best_t = t; }
  }
  return best;
}
bool Instance::has_core() const { return !core_path().empty(); }
// A Windows server writes no core: it prints its frames into the error log and leaves a minidump in the datadir.
// That dump is what a core is to the trial, the matrix and the MTR replay: the server crashed.
bool Instance::has_dump() const {
  if (!bd || !bd->windows) return false;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(datadir, ec))
    if (ends_with(lower(e.path().filename().string()), ".dmp") && e.is_regular_file(ec)) return true;
  return false;
}
