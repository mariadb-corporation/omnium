// Created by Roel Van de Paar, MariaDB
// local.cpp - the by-hand server: what ./anc, ./cl and ./test do in a basedir, without writing in
// the basedir. The datadir lives on tmpfs under <shm>/Ofresh_<build>; a small file there holds the
// socket and the port, so cl, replay and stop find the server again. Also omnium trial: what one
// saved trial holds.
#include "verbs.h"
#include <signal.h>

namespace fs = std::filesystem;

namespace {
string fresh_root(const Basedir& b) { return g_cfg.shm_dir + "/Ofresh_" + b.short_name(); }
struct Fresh { string basedir, sock, root, errlog; int port = 0; bool tcp = false; pid_t pid = 0; };
bool fresh_read(const Basedir& b, Fresh& f) {
  string root = fresh_root(b);
  string text = read_file(root + "/server.txt");
  if (text.empty()) return false;
  for (auto& l : split_lines(text)) {
    size_t eq = l.find('=');
    if (eq == string::npos) continue;
    string k = l.substr(0, eq), v = l.substr(eq + 1);
    if (k == "basedir") f.basedir = v;
    else if (k == "sock") f.sock = v;
    else if (k == "port") f.port = (int)to_long(v);
    else if (k == "tcp") f.tcp = v == "1";
    else if (k == "pid") f.pid = (pid_t)to_long(v);
    else if (k == "errlog") f.errlog = v;
  }
  f.root = root;
  return !f.sock.empty();
}
Endpoint fresh_endpoint(const Fresh& f) { return {f.sock, f.port, f.tcp}; }
// the pid in server.txt is ours to kill only while it is still that server: after a reboot or a long
// time the number belongs to something else
bool fresh_server_alive(const Fresh& f) {
  if (f.pid <= 0 || !pid_alive(f.pid)) return false;
  string cmd = proc_cmdline(f.pid);
  return cmd.empty() || cmd.find(f.root) != string::npos;
}
}  // namespace

// omnium fresh [basedir] [--cl] [--keep] [--options "..."]: a fresh datadir and a server, as ./anc does
int cmd_fresh(const Args& a) {
  string bd;
  bool cl = false, keep = false;
  string options, datadir;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    if (s == "--cl") cl = true;
    else if (s == "--keep") keep = true;
    else if (s == "--options") { if (i + 1 >= a.size()) { fprintf(stderr, "--options needs a value\n"); return 2; } options = a[++i]; }
    else if (s == "--datadir") { if (i + 1 >= a.size()) { fprintf(stderr, "--datadir needs a value\n"); return 2; } datadir = a[++i]; }
    else if (starts_with(s, "--")) { fprintf(stderr, "usage: omnium fresh [basedir] [--cl] [--keep] [--options \"...\"] [--datadir DIR]; --keep leaves a server that is already up alone\n"); return 2; }
    else bd = s;
  }
  Basedir b;
  string err;
  if (!basedir_from_arg(bd, b, &err)) { fprintf(stderr, "omnium fresh: %s\n", err.c_str()); return 1; }
  string root = fresh_root(b);
  Fresh old;
  if (fresh_read(b, old) && fresh_server_alive(old)) {
    if (keep) { printf("server already running: pid %d, %s\n", (int)old.pid, old.tcp ? fmt("port %d", old.port).c_str() : ("socket " + old.sock).c_str()); return 0; }
    kill_group(old.pid, SIGKILL);
    for (int i = 0; i < 20 && pid_alive(old.pid); i++) usleep(100000);
  }
  remove_tree(root);
  mkdirs(root);
  Instance inst;
  inst.bd = &b;
  inst.detached = true;                                     // the server stays up after omnium returns
  inst.set_paths(root, datadir);
  for (auto& o : split_ws(options)) inst.extra.push_back(o);
  write_file(root + "/BASEDIR", b.path + "\n");
  string tpl = template_for(b, myinit_from(options), root + "/templates");
  if (tpl.empty()) { fprintf(stderr, "omnium fresh: no datadir template for %s\n", b.name.c_str()); return 1; }
  if (!inst.start_fresh(tpl, b.is_san() ? 240 : 90)) {
    fprintf(stderr, "omnium fresh: %s\n", inst.start_note.c_str());
    fprintf(stderr, "error log: %s\n", inst.errlog.c_str());
    return 1;
  }
  write_file(root + "/server.txt", fmt("basedir=%s\nsock=%s\nport=%d\ntcp=%d\npid=%d\nerrlog=%s\n", b.path.c_str(), inst.sock.c_str(), inst.port, inst.tcp ? 1 : 0, (int)inst.pid, inst.errlog.c_str()));
  if (inst.tcp) printf("%s up: 127.0.0.1 port %d, log %s\n", b.short_name().c_str(), inst.port, inst.errlog.c_str());
  else printf("%s up: socket %s, port %d, log %s\n", b.short_name().c_str(), inst.sock.c_str(), inst.port, inst.errlog.c_str());
  printf("datadir %s (tmpfs; omnium fresh again wipes it)\n", inst.datadir.c_str());
  if (cl) {
    vector<string> args = {b.client, "-uroot"};
    for (auto& x : endpoint_args(inst.endpoint())) args.push_back(x);
    args.push_back("test");
    vector<char*> argv;
    for (auto& s : args) argv.push_back((char*)s.c_str());
    argv.push_back(nullptr);
    execv(args[0].c_str(), argv.data());
    fprintf(stderr, "cannot start %s\n", args[0].c_str());
    return 1;
  }
  return 0;
}

// omnium cl [basedir] [args...]: the client on the server omnium fresh started here
int cmd_cl(const Args& a) {
  string bd = a.empty() || starts_with(a[0], "-") ? "" : a[0];
  Basedir b;
  string err;
  if (!basedir_from_arg(bd, b, &err)) { fprintf(stderr, "omnium cl: %s\n", err.c_str()); return 1; }
  Fresh f;
  if (!fresh_read(b, f)) { fprintf(stderr, "omnium cl: no server for %s; omnium fresh starts one\n", b.short_name().c_str()); return 1; }
  if (f.pid > 0 && !pid_alive(f.pid)) fprintf(stderr, "note: the server pid %d is gone; the socket may be stale\n", (int)f.pid);
  vector<string> args = {b.client, "-uroot"};
  for (auto& x : endpoint_args(fresh_endpoint(f))) args.push_back(x);
  args.push_back("test");
  for (size_t i = bd.empty() ? 0 : 1; i < a.size(); i++) args.push_back(a[i]);
  vector<char*> argv;
  for (auto& s : args) argv.push_back((char*)s.c_str());
  argv.push_back(nullptr);
  execv(args[0].c_str(), argv.data());
  fprintf(stderr, "cannot start %s\n", args[0].c_str());
  return 1;
}

// omnium replay <file> [basedir] [--out FILE]: the SQL of a file into that server, as ./test does
int cmd_replay(const Args& a) {
  string file, bd, out;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    if (s == "--out") { if (i + 1 >= a.size()) { fprintf(stderr, "--out needs a value\n"); return 2; } out = a[++i]; }
    else if (starts_with(s, "--")) { fprintf(stderr, "usage: omnium replay <file> [basedir] [--out FILE]\n"); return 2; }
    else if (file.empty()) file = s;
    else bd = s;
  }
  if (file.empty() || !file_exists(file)) { fprintf(stderr, "usage: omnium replay <file> [basedir] [--out FILE]\n"); return 2; }
  Basedir b;
  string err;
  if (!basedir_from_arg(bd, b, &err)) { fprintf(stderr, "omnium replay: %s\n", err.c_str()); return 1; }
  Fresh f;
  if (!fresh_read(b, f)) { fprintf(stderr, "omnium replay: no server for %s; omnium fresh starts one\n", b.short_name().c_str()); return 1; }
  if (out.empty()) out = "mysql.out";
  vector<string> cargs = {b.client, "-uroot"};
  for (auto& x : endpoint_args(fresh_endpoint(f))) cargs.push_back(x);
  for (const char* x : {"-f", "--binary-mode", "test"}) cargs.push_back(x);
  CmdResult r = run_capture_in(cargs, read_file(file), 3600);
  write_file(out, r.out);
  printf("%s -> %s (%zu bytes, rc %d)\n", file.c_str(), out.c_str(), r.out.size(), r.rc);
  string uid;
  {
    UidResult ur; UidOptions uo; uo.wait_core = false;
    uid_for_dir(f.root, ur, uo);
    uid = trim(ur.uid);
  }
  if (!uid.empty() && !starts_with(uid, "Assert:")) printf("UID: %s\n", uid.c_str());
  return r.rc == 0 ? 0 : 1;
}

// omnium trial [<workdir>] <n>: what one saved trial holds
int cmd_trial(const Args& a) {
  string wd = ".";
  long n = 0;
  if (a.size() == 1 && is_digits(a[0])) n = to_long(a[0]);
  else if (a.size() == 2 && is_digits(a[1])) { wd = a[0]; n = to_long(a[1]); }
  else { fprintf(stderr, "usage: omnium trial [<workdir>] <n>\n"); return 2; }
  if (wd.find('/') == string::npos && wd != ".") {
    if (dir_exists(g_cfg.data_dir + "/" + wd)) wd = g_cfg.data_dir + "/" + wd;
    else if (is_digits(wd) && dir_exists(g_cfg.data_dir + "/O" + wd)) wd = g_cfg.data_dir + "/O" + wd;
  }
  string tdir = abs_path(wd) + "/" + std::to_string(n);
  if (!dir_exists(tdir)) { fprintf(stderr, "omnium trial: no %s\n", tdir.c_str()); return 1; }
  string uid = trim(read_file(tdir + "/MYBUG"));
  uid = uid.substr(0, uid.find('\n'));
  printf("%s\n", tdir.c_str());
  if (!uid.empty()) {
    printf("UID        %s\n", uid.c_str());
    KbMatch km = kb_search(uid);
    KbVerdict v = kb_verdict(km);
    printf("known bugs %s\n", v == KbVerdict::Known || v == KbVerdict::KnownAndFixed ? "known" : v == KbVerdict::FixedOnly ? "fixed only, so new here" : v == KbVerdict::Partial ? "partial match" : "new");
    if (auto s = seen_lookup(uid)) printf("seen       %ld times, first %s, last run %s\n", s->count, stamp_of(s->first).c_str(), s->run.c_str());
  }
  string bdir = trim(read_file(tdir + "/BASEDIR"));
  if (!bdir.empty()) printf("build      %s\n", basename_of(bdir).c_str());
  for (const char* f : {"MYEXTRA", "MYINIT", "MYSAFE"}) {
    string t = trim(read_file(tdir + "/" + f));
    if (!t.empty()) printf("%-10s %s\n", f, (t.size() > 160 ? t.substr(0, 157) + "..." : t).c_str());
  }
  std::error_code ec;
  vector<std::pair<string, uintmax_t>> files;
  for (auto& e : fs::directory_iterator(tdir, ec)) if (e.is_regular_file(ec)) files.push_back({e.path().filename().string(), fs::file_size(e.path(), ec)});
  std::sort(files.begin(), files.end(), [](auto& x, auto& y) { return x.second > y.second; });
  printf("files      ");
  int shown = 0;
  for (auto& f : files) { if (shown++ >= 8) break; printf("%s%s (%s)", shown > 1 ? ", " : "", f.first.c_str(), human_bytes(f.second).c_str()); }
  printf("%s\n", files.size() > 8 ? ", ..." : "");
  string errlog = file_exists(tdir + "/log/master.err") ? tdir + "/log/master.err" : tdir + "/master.err";
  if (file_exists(errlog)) printf("error log  %s\n", errlog.c_str());
  for (auto& e : fs::directory_iterator(tdir, ec)) {
    string fn = e.path().filename().string();
    if (fn.find("_out") != string::npos && !ends_with(fn, ".prev")) printf("reduced    %s (%zu lines)\n", e.path().string().c_str(), split_lines(read_file(e.path().string())).size());
  }
  return 0;
}

// omnium ldd [dir]: the server binary in that directory (or named) plus every library it needs,
// gathered there, as ldd_files.sh does; a core in the directory adds the libraries gdb lists for it
int cmd_ldd(const Args& a) {
  string dir = a.empty() ? "." : a[0];
  dir = abs_path(dir);
  if (!dir_exists(dir)) { fprintf(stderr, "omnium ldd: no such dir %s\n", dir.c_str()); return 1; }
  string bin;
  for (const char* n : {"/mariadbd", "/mysqld"}) if (file_exists(dir + n)) { bin = dir + n; break; }
  if (bin.empty()) {
    Basedir b;
    string e;
    // a basedir given as the dir brings its own binary; else the basedir the cwd is in
    if ((basedir_from_arg(dir, b, &e) || basedir_from_arg("", b, &e)) && !b.bin.empty()) bin = b.bin;
  }
  if (bin.empty()) { fprintf(stderr, "omnium ldd: no mariadbd or mysqld in %s, which is not a basedir, and the cwd is not one either\n", dir.c_str()); return 1; }
  string core;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().filename().string().find("core") != string::npos && e.is_regular_file(ec)) core = e.path().string();
  vector<string> files;
  string err;
  if (!ldd_copy(bin, dir, core, &files, &err)) { fprintf(stderr, "omnium ldd: %s\n", err.c_str()); return 1; }
  printf("%s: %s and %zu librar%s%s\n", dir.c_str(), basename_of(bin).c_str(), files.size(), files.size() == 1 ? "y" : "ies",
         core.empty() ? "" : (" (core " + basename_of(core) + " read as well)").c_str());
  return 0;
}
