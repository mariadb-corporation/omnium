// Created by Roel Van de Paar, MariaDB
// The process model. The driver forks and execs /proc/self/exe --role <name> for every job, one
// pipe pair per child (events up on fd 3, commands down on fd 4), every child in its own process
// group with PR_SET_PDEATHSIG, so nothing outlives the driver.
#include "common.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static std::mutex g_spawn_mtx;   // fork and the fd juggling around it are serialised

static void redirect_log(const string& logfile) {
  if (logfile.empty()) return;
  int lfd = open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) return;
  dup2(lfd, 1);
  dup2(lfd, 2);
  if (lfd > 2) close(lfd);
}

// A child dies with its parent. Linux has PR_SET_PDEATHSIG for it; elsewhere the process group
// kill on a stop does the same work, and only a SIGKILL of omnium itself leaves the child behind.
static void die_with_parent() {
#if defined(__linux__) && defined(PR_SET_PDEATHSIG)
  prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
}

string g_exe_override;

bool spawn_role(Child& c, const string& role, const vector<string>& args, const string& logfile,
                const string& cwd) {
  int ev[2], cmd[2];
  if (pipe2(ev, O_CLOEXEC) != 0) return false;
  if (pipe2(cmd, O_CLOEXEC) != 0) { close(ev[0]); close(ev[1]); return false; }
  string exe = g_exe_override.empty() ? self_exe() : g_exe_override;
  vector<string> argv = {exe, "--role", role};
  for (auto& a : args) argv.push_back(a);
#ifdef OMNIUM_FORK_PLAN
  ExecPlan plan = exec_plan(argv, {}, cwd, logfile);            // MSYS2: all the child needs, made here (see ExecPlan)
#endif
  std::lock_guard<std::mutex> lk(g_spawn_mtx);
  pid_t pid = fork();
  if (pid < 0) { close(ev[0]); close(ev[1]); close(cmd[0]); close(cmd[1]); return false; }
  if (pid == 0) {
    die_with_parent();
    setpgid(0, 0);
    if (getppid() == 1) _exit(127);
    // fd 3 = events up, fd 4 = commands down; dup clears CLOEXEC on the copies
    if (dup2(ev[1], 3) < 0 || dup2(cmd[0], 4) < 0) _exit(127);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); if (devnull > 4) close(devnull); }
#ifdef OMNIUM_FORK_PLAN
    redirect_log(plan.log);
    if (!plan.cwd.empty() && chdir(plan.cwd.c_str()) != 0) _exit(126);
    exec_plan_run(plan);
#else
    redirect_log(logfile);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    vector<char*> av;
    for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
    _exit(127);
#endif
  }
  close(ev[1]);
  close(cmd[0]);
  c.pid = pid;
  c.ev_fd = ev[0];
  c.cmd_fd = cmd[1];
  c.role = role;
  c.logfile = logfile;
  c.exited = false;
  c.status = 0;
  c.buf.clear();
  c.started = now_s();
  return true;
}

pid_t spawn_program(const vector<string>& argv, const string& logfile, const string& cwd,
                    bool own_group, const vector<string>& env_add, bool die_with_us) {
  if (argv.empty()) return -1;
#ifdef OMNIUM_FORK_PLAN
  ExecPlan plan = exec_plan(argv, env_add, cwd, logfile);       // MSYS2: all the child needs, made here (see ExecPlan)
#endif
  std::lock_guard<std::mutex> lk(g_spawn_mtx);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (die_with_us) {
      die_with_parent();
      if (getppid() == 1) _exit(127);
    }
    if (own_group) setpgid(0, 0);
#ifdef __linux__
    // the framework's coredump_filter: anonymous and private file pages only, so a core holds the
    // stacks and the locals but not the buffer pool. Without it a core runs to tens of GB.
    { int f = open("/proc/self/coredump_filter", O_WRONLY); if (f >= 0) { ssize_t w = write(f, "0x11\n", 5); (void)w; close(f); } }
#endif
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); if (devnull > 2) close(devnull); }
#ifdef OMNIUM_FORK_PLAN
    redirect_log(plan.log);
    if (!plan.cwd.empty() && chdir(plan.cwd.c_str()) != 0) _exit(126);
    exec_plan_run(plan);
#else
    redirect_log(logfile);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    for (auto& e : env_add) putenv(strdup(e.c_str()));
    vector<char*> av;
    for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    if (argv[0].find('/') != string::npos) execv(av[0], av.data());
    else execvp(av[0], av.data());
    _exit(127);
#endif
  }
  return pid;
}

bool child_send(Child& c, const string& line) {
  if (c.cmd_fd < 0) return false;
  string l = line + "\n";
  ssize_t w = write(c.cmd_fd, l.data(), l.size());
  return w == (ssize_t)l.size();
}

static bool pop_line(string& buf, string& line) {
  size_t nl = buf.find('\n');
  if (nl == string::npos) return false;
  line = buf.substr(0, nl);
  buf.erase(0, nl + 1);
  return true;
}

bool child_read(Child& c, string& line, int timeout_ms) {
  if (pop_line(c.buf, line)) return true;
  if (c.ev_fd < 0) return false;
  double deadline = now_ms() + timeout_ms;
  for (;;) {
    int wait_ms = (int)std::max(0.0, deadline - now_ms());
    struct pollfd p{c.ev_fd, POLLIN, 0};
    int pr = poll(&p, 1, wait_ms);
    if (pr == 0) return false;
    if (pr < 0) { if (errno == EINTR) continue; return false; }
    char buf[16384];
    ssize_t n = read(c.ev_fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {                      // EOF: the child closed or died
      close(c.ev_fd);
      c.ev_fd = -1;
      if (!c.buf.empty()) { line = c.buf; c.buf.clear(); return true; }
      return false;
    }
    c.buf.append(buf, n);
    if (pop_line(c.buf, line)) return true;
  }
}

int child_reap(Child& c, int timeout_ms) {
  if (c.exited) return WIFEXITED(c.status) ? WEXITSTATUS(c.status) : (WIFSIGNALED(c.status) ? 128 + WTERMSIG(c.status) : -1);
  if (c.pid <= 0) return -1;
  int st = wait_pid(c.pid, timeout_ms);
  if (st == -1) return -1;
  c.exited = true;
  c.status = st;
  return WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
}

void child_close(Child& c) {
  if (c.ev_fd >= 0) { close(c.ev_fd); c.ev_fd = -1; }
  if (c.cmd_fd >= 0) { close(c.cmd_fd); c.cmd_fd = -1; }
}

void kill_group(pid_t pid, int sig) {
  if (pid <= 0) return;
  kill(-pid, sig);
  kill(pid, sig);
}

bool pid_alive(pid_t pid) {
  if (pid <= 0) return false;
  if (kill(pid, 0) != 0) return false;
  string st = read_file(fmt("/proc/%d/stat", (int)pid));
  size_t rp = st.rfind(')');
  if (rp != string::npos && rp + 2 < st.size() && st[rp + 2] == 'Z') return false;
  return true;
}

int wait_pid(pid_t pid, int timeout_ms) {
  double deadline = now_ms() + timeout_ms;
  for (;;) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) return st;
    if (r < 0) return -1;               // not our child (adopted after a resume): caller polls pid_alive
    if (now_ms() >= deadline) return -1;
    usleep(20000);
  }
}

string proc_cmdline(pid_t pid) {
  string c = read_file(fmt("/proc/%d/cmdline", (int)pid));
  for (auto& ch : c) if (ch == 0) ch = ' ';
  return trim(c);
}
long proc_cpu_ms(pid_t pid) {
  if (pid <= 0) return -1;
  string s = read_file(fmt("/proc/%d/stat", (int)pid));
  size_t rp = s.rfind(')');
  if (rp == string::npos) return -1;
  vector<string> f = split_ws(s.substr(rp + 1));               // state ppid pgrp session tty tpgid flags 4 fault counts, then utime and stime
  if (f.size() < 13) return -1;
  long hz = sysconf(_SC_CLK_TCK);
  if (hz <= 0) hz = 100;
  return (to_long(f[11]) + to_long(f[12])) * 1000 / hz;
}
uint64_t proc_rss_bytes(pid_t pid) {
  string s = read_file(fmt("/proc/%d/status", (int)pid));
  for (auto& l : split_lines(s))
    if (starts_with(l, "VmRSS:")) return (uint64_t)to_long(split_ws(l)[1]) * 1024;
  return 0;
}
vector<pid_t> proc_children(pid_t pid) {
  vector<pid_t> out;
  std::error_code ec;
  for (auto& e : fs::directory_iterator("/proc", ec)) {
    string n = e.path().filename().string();
    if (!is_digits(n)) continue;
    string st = read_file("/proc/" + n + "/stat");
    size_t rp = st.rfind(')');
    if (rp == string::npos) continue;
    auto f = split_ws(st.substr(rp + 1));
    if (f.size() > 2 && to_long(f[1]) == pid) out.push_back((pid_t)to_long(n));
  }
  return out;
}

// ---- role side -------------------------------------------------------------------------------
static bool g_role_fds = false;
static std::mutex g_emit_mtx;
static std::atomic<bool> g_role_stop{false};
static string g_role_cmd_buf;

void role_init() {
  g_role_fds = fcntl(3, F_GETFD) != -1 && fcntl(4, F_GETFD) != -1;
}
void role_emit(const string& event) {
  std::lock_guard<std::mutex> lk(g_emit_mtx);
  string l = event + "\n";
  if (g_role_fds) {
    size_t off = 0;
    while (off < l.size()) {
      ssize_t w = write(3, l.data() + off, l.size() - off);
      if (w <= 0) break;
      off += w;
    }
  } else {
    fputs(l.c_str(), stderr);
  }
}
bool role_cmd(string& line, int timeout_ms) {
  if (pop_line(g_role_cmd_buf, line)) { if (line == "stop") g_role_stop = true; return true; }
  if (!g_role_fds) { if (timeout_ms > 0) usleep(timeout_ms * 1000); return false; }
  struct pollfd p{4, POLLIN, 0};
  int pr = poll(&p, 1, timeout_ms);
  if (pr <= 0) return false;
  char buf[4096];
  ssize_t n = read(4, buf, sizeof(buf));
  if (n <= 0) { g_role_stop = true; return false; }   // the driver is gone
  g_role_cmd_buf.append(buf, n);
  if (pop_line(g_role_cmd_buf, line)) { if (line == "stop") g_role_stop = true; return true; }
  return false;
}
bool role_stop_requested() {
  if (g_role_stop) return true;
  string l;
  if (role_cmd(l, 0)) { if (l == "stop") return true; g_role_cmd_buf = l + "\n" + g_role_cmd_buf; }
  return g_role_stop;
}

void raise_fd_limit() {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
}
// written in place: /proc takes no temporary and rename, so write_file cannot do this
bool set_oom_score(pid_t pid, int adj) {
  int f = open(fmt("/proc/%d/oom_score_adj", (int)pid).c_str(), O_WRONLY | O_CLOEXEC);
  if (f < 0) return false;
  string v = std::to_string(adj) + "\n";
  bool ok = write(f, v.data(), v.size()) == (ssize_t)v.size();
  close(f);
  return ok;
}
static std::atomic<int>* g_stop_counter = nullptr;
static void on_stop_signal(int) { if (g_stop_counter) g_stop_counter->fetch_add(1); }
void install_stop_signals(std::atomic<int>& counter) {
  g_stop_counter = &counter;
  struct sigaction sa{};
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
}
