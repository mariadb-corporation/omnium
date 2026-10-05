// Created by Roel Van de Paar, MariaDB
// build.cpp - omnium build: clone a branch, build its flavours, install each into /test/<name>, list it
#include "common.h"
#include "verbs.h"
#include <regex>
#include <thread>

static const char* CS_URL = "https://github.com/MariaDB/server.git";
static const char* ES_URL = "https://github.com/mariadb-corporation/MariaDBEnterprise.git";
static const char* MS_URL = "https://github.com/mysql/mysql-server.git";
static const char* PS_URL = "https://github.com/percona/percona-server.git";
static const char* MSAN_LIBDIR = "/MSAN_libs";
static const char* BOOST_DIR = "/tmp/omnium_boost";
static const int64_t BUILD_FREE_FLOOR = 8LL << 30;   // a build starts only with this much free on the build volume
static const int PATCH1_BEHIND = 50;                 // commits behind the base before the merge-base is built

// ---------------------------------------------------------------------------------------------
// the source tree
// ---------------------------------------------------------------------------------------------
static string git_out(const string& dir, const vector<string>& args, int timeout_s = 120) {
  vector<string> argv = {"git", "-C", dir};
  for (auto& a : args) argv.push_back(a);
  CmdResult r = run_capture(argv, timeout_s);
  return r.rc == 0 ? trim(r.out) : "";
}
static bool version_like(const string& s) {
  static const std::regex re("^[0-9]+\\.[0-9]+(\\.[0-9]+)?$");
  return std::regex_match(s, re);
}
static bool has_enterprise_spec(const string& dir) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir + "/support-files/rpm", ec))
    if (e.path().filename().string().find("enterprise") != string::npos) return true;
  return false;
}
bool source_info(const string& dir, SourceInfo& si, string* err) {
  si = SourceInfo();
  si.dir = abs_path(dir);
  string vf = si.dir + "/VERSION";
  if (!file_exists(vf) && file_exists(si.dir + "/MYSQL_VERSION")) vf = si.dir + "/MYSQL_VERSION";
  string v = read_file(vf);
  if (v.empty()) { if (err) *err = "no VERSION file in " + si.dir; return false; }
  for (auto& raw : split_lines(v)) {
    string l = trim(raw);
    size_t eq = l.find('=');
    if (eq == string::npos) continue;
    string k = trim(l.substr(0, eq)), val = trim(l.substr(eq + 1));
    if (k == "MYSQL_VERSION_MAJOR") si.major = (int)to_long(val);
    else if (k == "MYSQL_VERSION_MINOR") si.minor = (int)to_long(val);
    else if (k == "MYSQL_VERSION_PATCH") si.patch = (int)to_long(val);
    else if (k == "MYSQL_VERSION_EXTRA") si.extra = val;
    else if (k == "SERVER_MATURITY") si.maturity = true;
  }
  if (si.major == 0) { if (err) *err = "no MYSQL_VERSION_MAJOR in " + vf; return false; }
  si.has_rocksdb = dir_exists(si.dir + "/storage/rocksdb");
  if (si.major >= 10 && si.major <= 15) {
    si.vendor = Vendor::MariaDB;
    si.es = has_enterprise_spec(si.dir);
    si.product = "mariadb";
    si.version = fmt("%d.%d.%d%s", si.major, si.minor, si.patch, si.extra.c_str());
  } else if (si.extra.empty() || si.extra == "-dmr" || si.extra == "-rc") {
    si.vendor = Vendor::MySQL;
    si.product = "mysql";
    si.version = fmt("%d.%d.%d", si.major, si.minor, si.patch);
  } else {
    si.vendor = Vendor::Percona;
    si.product = "percona-server";
    si.version = fmt("%d.%d.%d%s", si.major, si.minor, si.patch, si.extra.c_str());
  }
  si.series = fmt("%d.%d", si.major, si.minor);
  si.git_rev = git_out(si.dir, {"rev-parse", "HEAD"});
  si.branch = git_out(si.dir, {"rev-parse", "--abbrev-ref", "HEAD"});
  return true;
}
// a branch that is not a version branch is a feature branch: its name becomes the basedir tag
string tag_from_branch(const string& branch) {
  string b = branch;
  if (ends_with(b, "-enterprise")) b = b.substr(0, b.size() - 11);
  if (b.empty() || b == "HEAD" || b == "main" || b == "trunk" || b == "master" || version_like(b)) return "";
  string t;
  for (char c : b) t += (c == '_' || c == '/' || c == ' ') ? '-' : c;
  return t;
}
static string flavour_suffix(const string& fl) {   // the scratch copy: <source>_opt, _dbg, _opt_san, _dbg_msan
  string t = ends_with(fl, "dbg") ? "dbg" : "opt";
  if (starts_with(fl, "ubasan")) return t + "_san";
  if (starts_with(fl, "msan")) return t + "_msan";
  return t;
}
static bool flavour_dbg(const string& fl) { return ends_with(fl, "dbg"); }
bool flavour_valid(const string& fl) {
  for (const char* f : {"opt", "dbg", "ubasan-opt", "ubasan-dbg", "msan-opt", "msan-dbg"}) if (fl == f) return true;
  return false;
}
string build_name(const SourceInfo& si, const string& flavour, const string& tag, const string& date) {
  string n;
  if (starts_with(flavour, "ubasan")) n += "UBASAN_";
  else if (starts_with(flavour, "msan")) n += "MSAN_";
  if (!tag.empty()) n += tag + "_";
  string marker = si.vendor == Vendor::MariaDB ? (si.es ? "EMD" : "MD") : si.vendor == Vendor::MySQL ? "MS" : "PS";
  n += marker + date + "-" + si.product + "-" + si.version + "-linux-x86_64-" + (flavour_dbg(flavour) ? "dbg" : "opt");
  return n;
}
static string find_tool(const vector<string>& candidates) {
  for (auto& c : candidates) if (is_executable(c)) return c;
  return "";
}
// the cmake line, ported from build_mdpsms_{opt,dbg}[_san|_msan].sh; one argv entry per option
vector<string> cmake_command(const SourceInfo& si, const string& flavour, const string& prefix, string* err) {
  bool dbg = flavour_dbg(flavour), san = starts_with(flavour, "ubasan"), msan = starts_with(flavour, "msan");
  vector<string> c = {"cmake", ".", "-G", "Ninja"};
  string cc, cxx;
  if (msan) { cc = find_tool({"/usr/bin/clang-20"}); cxx = find_tool({"/usr/bin/clang++-20"}); }
  else { cc = find_tool({"/usr/local/bin/clang", "/usr/bin/clang"}); cxx = find_tool({"/usr/local/bin/clang++", "/usr/bin/clang++"}); }
  if (cc.empty() || cxx.empty()) { if (err) *err = msan ? "clang-20 is needed for an MSAN build" : "clang not found"; return {}; }
  string lld = find_tool({"/usr/local/bin/ld.lld", "/usr/bin/ld.lld"});
  c.push_back("-DCMAKE_C_COMPILER=" + cc);
  c.push_back("-DCMAKE_CXX_COMPILER=" + cxx);
  if ((san || msan) && !lld.empty()) c.push_back("-DCMAKE_LINKER=" + lld);
  // SSL
  string ssl = "-DWITH_SSL=system";
  if (si.vendor == Vendor::MariaDB) ssl = (si.es && file_exists("/usr/include/openssl/ssl.h")) ? "-DWITH_SSL=system" : "-DWITH_SSL=bundled";
  if (msan) ssl = version_at_least(si.version, "11.8") ? string("-DWITH_SSL=") + MSAN_LIBDIR : "-DWITH_SSL=yes";
  c.push_back(ssl);
  c.push_back("-DBUILD_CONFIG=mysql_release");
  if (si.vendor == Vendor::MariaDB && file_exists("/usr/include/libpmem.h") && version_at_least(si.version, "10.5") && !msan) c.push_back("-DWITH_PMEM=1");
  for (const char* o : {"-DWITH_UNIT_TESTS=0", "-DWITH_TOKUDB=0", "-DWITH_JEMALLOC=no", "-DFEATURE_SET=community", "-DDEBUG_EXTNAME=OFF",
                        "-DWITH_EMBEDDED_SERVER=0", "-DWITH_DEBUG_SYNC=ON", "-DENABLE_DOWNLOADS=1", "-DDOWNLOAD_BOOST=1"}) c.push_back(o);
  c.push_back(string("-DWITH_BOOST=") + BOOST_DIR);
  for (const char* o : {"-DENABLED_LOCAL_INFILE=1", "-DENABLE_DTRACE=0", "-DWITH_SAFEMALLOC=OFF", "-DWITH_NUMA=OFF", "-DWITH_UNIT_TESTS=OFF",
                        "-DCONC_WITH_UNITTEST=OFF", "-DCONC_WITH_SSL=OFF"}) c.push_back(o);
  c.push_back((dbg || msan) ? "-DPLUGIN_PERFSCHEMA=NO" : "-DPLUGIN_PERFSCHEMA=YES");   // every dbg and the msan scripts say NO
  c.push_back("-DWITH_DBUG_TRACE=OFF");
  bool zlib_bundled = si.vendor == Vendor::MariaDB || si.major == 8;
  c.push_back(zlib_bundled ? "-DWITH_ZLIB=bundled" : "-DWITH_ZLIB=system");
  bool rocksdb = si.has_rocksdb && !msan;
  c.push_back(rocksdb ? "-DWITH_ROCKSDB=1" : "-DWITH_ROCKSDB=0");
  c.push_back("-DWITH_PAM=ON");
  c.push_back((san || msan) ? "-DWITH_MARIABACKUP=0" : "-DWITH_MARIABACKUP=1");
  c.push_back("-DFORCE_INSOURCE_BUILD=1");
  c.push_back("-DWITHOUT_GROUP_REPLICATION=1");
  string olevel = dbg ? "1" : "2";
  if (san) {
    string rt = trim(run_capture({cc, "-print-runtime-dir"}, 30).out);
    string cflags = "-O" + olevel + " -fPIC -march=native -mtune=native -fsanitize=address,undefined -fno-omit-frame-pointer";
    string lflags = (lld.empty() ? string("") : string("-fuse-ld=lld ")) + "-lm -fsanitize=address,undefined -shared-libasan -Wl,-rpath," + rt;
    for (const char* o : {"-DWITH_ASAN=ON", "-DWITH_ASAN_SCOPE=ON", "-DWITH_UBSAN=ON", "-DWSREP_LIB_WITH_ASAN=ON"}) c.push_back(o);
    c.push_back("-DCMAKE_C_FLAGS=" + cflags);
    c.push_back("-DCMAKE_CXX_FLAGS=" + cflags);
    for (const char* k : {"EXE", "SHARED", "MODULE"}) c.push_back(fmt("-DCMAKE_%s_LINKER_FLAGS=%s", k, lflags.c_str()));
    c.push_back("-DCMAKE_REQUIRED_FLAGS=-fsanitize=address,undefined -shared-libasan -Wl,-rpath," + rt);
  } else if (msan) {
    if (!dir_exists(MSAN_LIBDIR)) { if (err) *err = string(MSAN_LIBDIR) + " is missing: the MSAN instrumented libraries are needed"; return {}; }
    string ign = g_paths.qa + "/MSAN.ignorelist";
    if (!file_exists(ign)) { if (err) *err = ign + " is missing"; return {}; }
    for (const char* o : {"-DWITH_MSAN=ON", "-DSECURITY_HARDENED=OFF", "-DHAVE_CXX_NEW=1", "-DWITH_INNODB_BZIP2=OFF", "-DWITH_INNODB_LZ4=OFF",
                          "-DWITH_INNODB_LZMA=OFF", "-DWITH_INNODB_LZO=OFF", "-DWITH_INNODB_SNAPPY=OFF", "-DWITH_NUMA=NO", "-DWITH_SYSTEMD=no",
                          "-DPLUGIN_MROONGA=NO", "-DPLUGIN_ROCKSDB=NO", "-DPLUGIN_OQGRAPH=NO", "-DWITH_WSREP=OFF",
                          "-DCMAKE_DISABLE_FIND_PACKAGE_URING=1", "-DCMAKE_DISABLE_FIND_PACKAGE_LIBAIO=1", "-DHAVE_LIBAIO_H=0", "-DIGNORE_AIO_CHECK=YES"}) c.push_back(o);
    c.push_back(string("-DCMAKE_LIBRARY_PATH=") + MSAN_LIBDIR);
    c.push_back(string("-DCMAKE_PREFIX_PATH=") + MSAN_LIBDIR);
    string lflags = fmt("-fuse-ld=lld -L%s -Wl,-rpath=%s", MSAN_LIBDIR, MSAN_LIBDIR);
    for (const char* k : {"EXE", "SHARED", "MODULE"}) c.push_back(fmt("-DCMAKE_%s_LINKER_FLAGS=%s", k, lflags.c_str()));
    c.push_back("-DCMAKE_C_FLAGS=-fsanitize-ignorelist=" + ign);
    c.push_back("-DCMAKE_CXX_FLAGS=-fsanitize-ignorelist=" + ign);
  } else {
    c.push_back(dbg ? "-DCMAKE_CXX_FLAGS=-march=native" : "-DCMAKE_CXX_FLAGS=-fPIC");
  }
  c.push_back("-DMYSQL_MAINTAINER_MODE=OFF");
  c.push_back("-DWARNING_AS_ERROR=");
  c.push_back(dbg ? "-DCMAKE_BUILD_TYPE=Debug" : "-DCMAKE_BUILD_TYPE=RelWithDebInfo");
  if (dbg) { c.push_back("-DWITH_DEBUG=ON"); c.push_back("-DWITH_INNODB_EXTRA_DEBUG=ON"); }
  c.push_back("-DCMAKE_INSTALL_PREFIX=" + prefix);
  c.push_back("-DINSTALL_LAYOUT=STANDALONE");
  return c;
}
static string argv_line(const vector<string>& argv) {
  vector<string> q;
  for (auto& a : argv) q.push_back(a.find_first_of(" '\"$`") == string::npos ? a : sh_quote(a));
  return join(q, " ");
}

// ---------------------------------------------------------------------------------------------
// one flavour, start to finish: copy, patch, cmake, ninja, ninja install, tar, list
// ---------------------------------------------------------------------------------------------
static bool sh_logged(const string& script, const string& cwd, const string& log, int timeout_s, string* why) {
  CmdResult r = run_shell("(" + script + ") >> " + sh_quote(log) + " 2>&1", timeout_s, cwd);
  if (r.rc == 0) return true;
  if (why) *why = fmt("rc=%d, see %s", r.rc, log.c_str());
  return false;
}
int build_one(const BuildJob& job, BuildResult& res) {
  res = BuildResult();
  double t0 = now_ms();
  SourceInfo si;
  string err;
  if (!source_info(job.source, si, &err)) { res.note = err; return 2; }
  if (!flavour_valid(job.flavour)) { res.note = "unknown flavour " + job.flavour; return 2; }
  string tag = job.tag.empty() ? tag_from_branch(si.branch) : job.tag;
  string date = date_ddmmyy();
  res.name = build_name(si, job.flavour, tag, date);
  string prefix = g_cfg.test_dir + "/" + res.name;
  res.basedir = prefix;
  if (dir_exists(prefix)) {
    if (!job.rebuild) { res.note = "exists: " + prefix + " (a build of this tree today; --rebuild replaces it)"; return 3; }
    remove_tree(prefix);
  }
  int64_t free_b = (int64_t)dir_free_bytes(g_cfg.test_dir);
  if (free_b < BUILD_FREE_FLOOR) { res.note = fmt("only %s free on %s; a build needs %s", human_bytes(free_b).c_str(), g_cfg.test_dir.c_str(), human_bytes(BUILD_FREE_FLOOR).c_str()); return 4; }
  for (const char* t : {"cmake", "ninja", "git"}) if (run_capture({t, "--version"}, 30).rc != 0) { res.note = string(t) + " is not installed"; return 4; }
  vector<string> cm = cmake_command(si, job.flavour, prefix, &err);
  if (cm.empty()) { res.note = err; return 4; }
  string copy = si.dir + "_" + flavour_suffix(job.flavour);
  res.log = copy + "/omnium_build.log";
  remove_tree(copy);
  string why;
  if (!copy_tree(si.dir, copy, &why)) { res.note = "cannot copy the tree to " + copy + ": " + why; return 5; }
  write_file(res.log, fmt("omnium build %s %s\nsource %s at %s\n%s\n\n", res.name.c_str(), job.flavour.c_str(), si.dir.c_str(), si.git_rev.c_str(), argv_line(cm).c_str()));
  // the tree patches the old scripts apply
  string patch = "find . -type f -name CMakeLists.txt -exec sed -i 's|[ \\t]*CMAKE_MINIMUM_REQUIRED[ \\t]*([ \\t]*VERSION[ \\t]*2.*|CMAKE_MINIMUM_REQUIRED(VERSION 3.5)|i' {} \\; ; "
                 "rm -rf ./plugin/tokudb-backup-plugin; "
                 "[ ! -f VERSION ] && [ -f MYSQL_VERSION ] && cp MYSQL_VERSION VERSION; true";
  if (si.maturity) patch += "; sed -i 's:\\(sigaction(SIG[SABIF]\\)://\\1:' sql/mysqld.cc";
  if (!sh_logged(patch, copy, res.log, 600, &why)) { res.note = "tree patch failed: " + why; return 5; }
  vector<string> env;
  if (starts_with(job.flavour, "msan")) { env.push_back(string("CMAKE_LIBRARY_PATH=") + MSAN_LIBDIR); env.push_back(string("CMAKE_PREFIX_PATH=") + MSAN_LIBDIR); }
  string envs;
  for (auto& e : env) envs += "export " + e + "; ";
  mkdirs(BOOST_DIR);
  if (!sh_logged(envs + argv_line(cm), copy, res.log, 1800, &why)) { res.note = "cmake failed: " + why; return 6; }
  if (!sh_logged(envs + fmt("ninja -j%d", job.jobs > 0 ? job.jobs : cpu_threads()), copy, res.log, 6 * 3600, &why)) { res.note = "compile failed: " + why; return 7; }
  if (!sh_logged(envs + "ninja install", copy, res.log, 3600, &why)) { res.note = "install failed: " + why; return 8; }
  write_file(prefix + "/git_revision.txt", si.git_rev + "\n");
  write_file(prefix + "/BUILD_CMD_CMAKE", argv_line(cm) + "\n");
  Basedir b;
  if (!basedir_probe(prefix, b)) { res.note = "installed, but no server binary under " + prefix; return 9; }
  CmdResult v = run_capture({b.bin, "--version"}, 60);
  if (v.rc != 0) { res.note = "installed, but the server binary does not run: " + trim(v.out); return 9; }
  res.version_line = trim(v.out);
  if (job.tar) {
    string tars = g_cfg.data_dir + "/TARS";
    mkdirs(tars);
    string tarcmd = is_executable("/usr/bin/pigz") ? "tar -I pigz -cf " : "tar -czf ";
    if (!sh_logged(tarcmd + sh_quote(tars + "/" + res.name + ".tar.gz") + " -C " + sh_quote(g_cfg.test_dir) + " " + sh_quote(res.name), g_cfg.test_dir, res.log, 3600, &why))
      res.note = "built; tar failed: " + why;
    else res.tar = tars + "/" + res.name + ".tar.gz";
  }
  // the error_code form: a throw here would end every build of this call, not just this one
  std::error_code cec;
  fs::copy_file(res.log, prefix + "/omnium_build.log", fs::copy_options::overwrite_existing, cec);
  if (cec) res.note += (res.note.empty() ? "built; " : "; ") + string("the log could not go into the basedir (") + cec.message() + "), so the build tree with it stays";
  else { if (!getenv("KEEP_BUILD_TREE")) remove_tree(copy); res.log = prefix + "/omnium_build.log"; }
  if (tag.empty()) {
    static std::mutex reg_mtx;                                    // the builds of one call finish in their own threads
    std::lock_guard<std::mutex> lk(reg_mtx);
    Registry r = registry_current();
    for (auto& e : r.entries) if (e.name == res.name) { e.origin = "omnium"; e.commit = si.git_rev.empty() ? "-" : si.git_rev; }
    registry_save(r);
    res.listed = registry_find(r, res.name) != nullptr;
  }
  res.seconds = (now_ms() - t0) / 1000.0;
  res.ok = true;
  return 0;
}

// ---------------------------------------------------------------------------------------------
// clone
// ---------------------------------------------------------------------------------------------
static bool have_github_credentials() {
  return icontains(read_file(home_dir() + "/.git-credentials"), "github.com");
}
static bool remote_has_branch(const string& url, const string& branch) {
  CmdResult r = run_capture({"git", "ls-remote", "--heads", url, branch}, 120);
  return r.rc == 0 && !trim(r.out).empty();
}
// what: 13.1 | 12.3 --es | mysql 8.0 | percona 8.0 | <feature-branch> [--es]; returns the source dir
bool clone_source(const string& what, Vendor vendor, bool es, string& dir, string* err) {
  string url = vendor == Vendor::MySQL ? MS_URL : vendor == Vendor::Percona ? PS_URL : es ? ES_URL : CS_URL;
  if (es && !have_github_credentials()) { if (err) *err = "an ES clone needs GitHub credentials in ~/.git-credentials"; return false; }
  vector<string> argv = {"git", "clone", "--recurse-submodules", "-j8"};
  string branch;
  bool release = version_like(what);
  if (vendor == Vendor::MySQL) { dir = g_cfg.test_dir + "/mysql-" + what; branch = what; }
  else if (vendor == Vendor::Percona) { dir = g_cfg.test_dir + "/percona-" + what; branch = what; }
  else if (release) { dir = g_cfg.test_dir + "/" + what + (es ? "-es" : ""); branch = es ? what + "-enterprise" : what; }
  else { dir = g_cfg.test_dir + "/" + what; branch = what; }
  if (release) {
    argv.push_back("--depth=1");
    if (!es && !remote_has_branch(url, branch)) branch = "";   // the newest version lives on the default branch
  } else {
    argv.push_back("--filter=blob:none");                      // full history for merge-base, blobs on demand
  }
  if (!branch.empty()) argv.push_back("--branch=" + branch);
  argv.push_back(url);
  argv.push_back(dir);
  if (dir_exists(dir)) { if (err) *err = "exists: " + dir; return false; }
  printf("cloning %s%s into %s\n", url.c_str(), branch.empty() ? "" : (" branch " + branch).c_str(), dir.c_str());
  fflush(stdout);
  CmdResult r = run_capture(argv, 4 * 3600);
  if (r.rc != 0) { remove_tree(dir); if (err) *err = "git clone failed: " + tail_lines(r.out, 5); return false; }
  return true;
}
static bool pull_source(const string& dir, string* err) {
  CmdResult r = run_capture({"git", "-C", dir, "pull", "--ff-only"}, 3600);
  if (r.rc != 0) { if (err) *err = "git pull failed in " + dir + ": " + tail_lines(r.out, 3); return false; }
  run_capture({"git", "-C", dir, "submodule", "update", "--init", "--recursive"}, 3600);
  return true;
}

// ---------------------------------------------------------------------------------------------
// the patch-1 tree: the feature branch minus the feature, at its merge-base with the base branch
// ---------------------------------------------------------------------------------------------
bool patch1_tree(const string& feature_dir, string& tree, string& note, string* err) {
  SourceInfo si;
  if (!source_info(feature_dir, si, err)) return false;
  string base = si.series;
  if (si.es) base += "-enterprise";
  string url = git_out(feature_dir, {"remote", "get-url", "origin"});
  if (!remote_has_branch(url, base)) base = "main";
  CmdResult f = run_capture({"git", "-C", feature_dir, "fetch", "origin", base}, 3600);
  if (f.rc != 0) { if (err) *err = "git fetch origin " + base + " failed (a shallow clone cannot do this; clone the feature branch with omnium build): " + tail_lines(f.out, 3); return false; }
  string mb = git_out(feature_dir, {"merge-base", "HEAD", "FETCH_HEAD"});
  if (mb.empty()) { if (err) *err = "no merge-base between HEAD and origin/" + base; return false; }
  long behind = to_long(git_out(feature_dir, {"rev-list", "--count", mb + "..FETCH_HEAD"}), 0);
  note = fmt("merge-base %s, %ld commits behind origin/%s", mb.substr(0, 12).c_str(), behind, base.c_str());
  if (behind <= PATCH1_BEHIND) { tree = ""; return true; }  // close to the base: the registry's newest base build serves
  tree = feature_dir + "-patch-1";
  if (dir_exists(tree)) {
    if (git_out(tree, {"rev-parse", "HEAD"}) == mb) return true;
    run_capture({"git", "-C", feature_dir, "worktree", "remove", "--force", tree}, 600);
    remove_tree(tree);
  }
  CmdResult w = run_capture({"git", "-C", feature_dir, "worktree", "add", "--detach", tree, mb}, 3600);
  if (w.rc != 0) { if (err) *err = "git worktree add failed: " + tail_lines(w.out, 3); return false; }
  run_capture({"git", "-C", tree, "submodule", "update", "--init", "--recursive"}, 3600);
  return true;
}

// ---------------------------------------------------------------------------------------------
// omnium build
// ---------------------------------------------------------------------------------------------
static void print_result(const BuildJob& j, int rc, const BuildResult& r) {
  if (rc == 0) printf("built %s in %s%s%s\n  %s\n", r.basedir.c_str(), human_secs((long)r.seconds).c_str(),
                      r.listed ? (", listed in " + g_paths.builds_file).c_str() : "", r.tar.empty() ? "" : (", tar " + r.tar).c_str(), r.version_line.c_str());
  else printf("FAILED %s %s (rc %d): %s\n", j.source.c_str(), j.flavour.c_str(), rc, r.note.c_str());
  if (!r.note.empty() && rc == 0) printf("  note: %s\n", r.note.c_str());
}
static int run_jobs(vector<BuildJob>& jobs) {
  int total = 0;
  for (auto& j : jobs) if (j.jobs <= 0) j.jobs = std::max(4, cpu_threads() / (int)jobs.size());
  vector<BuildResult> res(jobs.size());
  vector<int> rcs(jobs.size(), 0);
  vector<std::thread> th;
  for (size_t i = 0; i < jobs.size(); i++) {
    printf("building %s %s with -j%d\n", jobs[i].source.c_str(), jobs[i].flavour.c_str(), jobs[i].jobs);
    fflush(stdout);
    th.emplace_back([&, i] { rcs[i] = build_one(jobs[i], res[i]); });
  }
  for (auto& t : th) t.join();
  for (size_t i = 0; i < jobs.size(); i++) { print_result(jobs[i], rcs[i], res[i]); if (rcs[i]) total = rcs[i]; }
  return total;
}
static vector<string> flavours_from_registry(const Registry& r, const string& vendor, const string& series) {
  vector<string> out;
  for (auto& e : r.entries) {
    if (e.vendor != vendor || e.series != series) continue;
    string f = e.flavour == "plain" ? e.type : e.flavour == "UBASAN" ? "ubasan-" + e.type : e.flavour == "MSAN" ? "msan-" + e.type : "";
    if (!f.empty() && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
  }
  return out;
}
// omnium build follow: pull every CS and ES series the registry lists and rebuild what moved
static int build_follow(bool tar) {
  Registry r = registry_current();
  std::map<string, string> series_vendor;   // "CS 13.1" -> newest commit listed
  for (auto& e : r.entries) if (e.vendor == "CS" || e.vendor == "ES") {
    string k = e.vendor + " " + e.series;
    if (!series_vendor.count(k) || (e.commit != "-" && series_vendor[k] == "-")) series_vendor[k] = e.commit;
  }
  int rc = 0;
  for (auto& [k, commit] : series_vendor) {
    string vendor = k.substr(0, 2), series = k.substr(3);
    bool es = vendor == "ES";
    string dir = g_cfg.test_dir + "/" + series + (es ? "-es" : "");
    string err;
    if (!dir_exists(dir) && !clone_source(series, Vendor::MariaDB, es, dir, &err)) { printf("%s: %s\n", k.c_str(), err.c_str()); rc = 1; continue; }
    if (!pull_source(dir, &err)) { printf("%s: %s\n", k.c_str(), err.c_str()); rc = 1; continue; }
    string head = git_out(dir, {"rev-parse", "HEAD"});
    if (!head.empty() && head == commit) { printf("%s: up to date at %s\n", k.c_str(), head.substr(0, 12).c_str()); continue; }
    vector<BuildJob> jobs;
    for (auto& f : flavours_from_registry(r, vendor, series)) {
      BuildJob j; j.source = dir; j.flavour = f; j.tar = tar;
      SourceInfo si;
      if (source_info(dir, si, nullptr) && dir_exists(g_cfg.test_dir + "/" + build_name(si, f, "", date_ddmmyy()))) continue;  // built today already
      jobs.push_back(j);
    }
    if (jobs.empty()) { printf("%s: nothing to build\n", k.c_str()); continue; }
    if (run_jobs(jobs) != 0) rc = 1;
  }
  return rc;
}
int cmd_build(const Args& a) {
  if (a.empty()) {
    printf("usage: omnium build <13.1 | 12.3 --es | mysql 8.0 | percona 8.0 | <feature-branch> [--es] | /path/to/tree> [flavours] [--tar] [--rebuild] [--jobs N] [--tag T] [--patch-1]\n"
           "       omnium build follow [--tar]        pull every CS and ES series in /test/omnium.builds, rebuild what moved\n"
           "flavours: opt dbg ubasan-opt ubasan-dbg msan-opt msan-dbg | --san (both UBASAN) | --msan (both MSAN) | --all (all six); default opt dbg\n");
    return 2;
  }
  bool es = false, tar = false, rebuild = false, patch1 = false, san = false, msan = false, all = false;
  int jobs = 0;
  string tag, what;
  Vendor vendor = Vendor::MariaDB;
  vector<string> flavours;
  for (size_t i = 0; i < a.size(); i++) {
    const string& x = a[i];
    if (x == "--es") es = true;
    else if (x == "--tar") tar = true;
    else if (x == "--rebuild") rebuild = true;
    else if (x == "--patch-1") patch1 = true;
    else if (x == "--san") san = true;
    else if (x == "--msan") msan = true;
    else if (x == "--all") all = true;
    else if (x == "--jobs" && i + 1 < a.size()) jobs = (int)to_long(a[++i]);
    else if (x == "--tag" && i + 1 < a.size()) tag = a[++i];
    else if (x == "--jobs" || x == "--tag") { printf("omnium build: %s needs a value\n", x.c_str()); return 2; }
    // a flag omnium does not know must not become the branch to clone
    else if (starts_with(x, "-")) { printf("omnium build: unknown flag %s (omnium build with no arguments prints the usage)\n", x.c_str()); return 2; }
    else if (x == "mysql" && what.empty()) vendor = Vendor::MySQL;
    else if (x == "percona" && what.empty()) vendor = Vendor::Percona;
    else if (flavour_valid(x)) flavours.push_back(x);
    else if (what.empty()) what = x;
    else { printf("unexpected argument %s\n", x.c_str()); return 2; }
  }
  if (what == "follow") return build_follow(tar);
  if (what.empty()) { printf("what to build? omnium build with no arguments prints the usage\n"); return 2; }
  if (all) flavours = {"opt", "dbg", "ubasan-opt", "ubasan-dbg", "msan-opt", "msan-dbg"};
  else {
    if (flavours.empty() && !san && !msan) flavours = {"opt", "dbg"};
    if (san) { flavours.push_back("ubasan-opt"); flavours.push_back("ubasan-dbg"); }
    if (msan) { flavours.push_back("msan-opt"); flavours.push_back("msan-dbg"); }
  }
  // a flavour named twice (dbg dbg, or --san with ubasan-opt) is one build: two would share a basedir
  { vector<string> u; for (auto& f : flavours) if (std::find(u.begin(), u.end(), f) == u.end()) u.push_back(f); flavours = u; }
  string dir, err;
  if (dir_exists(what)) dir = abs_path(what);
  else {
    // MSYS2: a branch name never starts with a slash or has a drive letter, so this is a path that is not there: cloned as
    // a branch, it would only leave its parent folders under TEST_DIR (Linux keeps its behaviour until it is decided)
    if (kTakeFixes && (what[0] == '/' || (what.size() > 1 && what[1] == ':'))) { printf("omnium build: %s is not a directory\n", what.c_str()); return 1; }
    dir = g_cfg.test_dir + "/" + (vendor == Vendor::MySQL ? "mysql-" : vendor == Vendor::Percona ? "percona-" : "") + what + ((es && version_like(what)) ? "-es" : "");
    if (!dir_exists(dir) && !clone_source(what, vendor, es, dir, &err)) { printf("%s\n", err.c_str()); return 1; }
  }
  vector<BuildJob> jobs_v;
  if (patch1) {
    string tree, note;
    if (!patch1_tree(dir, tree, note, &err)) { printf("%s\n", err.c_str()); return 1; }
    printf("%s\n", note.c_str());
    if (tree.empty()) { printf("close to the base: the registry's newest base build serves as the patch-1 build\n"); return 0; }
    SourceInfo si;
    source_info(dir, si, nullptr);
    string t = tag.empty() ? tag_from_branch(si.branch) : tag;
    if (t.empty()) t = basename_of(dir);
    for (auto& f : flavours) { BuildJob j; j.source = tree; j.flavour = f; j.tar = tar; j.rebuild = rebuild; j.jobs = jobs; j.tag = t + "-patch-1"; jobs_v.push_back(j); }
  } else {
    for (auto& f : flavours) { BuildJob j; j.source = dir; j.flavour = f; j.tar = tar; j.rebuild = rebuild; j.jobs = jobs; j.tag = tag; jobs_v.push_back(j); }
  }
  return run_jobs(jobs_v);
}
