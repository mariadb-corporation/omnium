// Created by Roel Van de Paar, MariaDB
// Strings, files, time, box facts, process helpers.
#include "common.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>

const char* OMNIUM_VERSION = "1.0";

string trim(std::string_view v) {
  size_t a = 0, b = v.size();
  while (a < b && isspace((unsigned char)v[a])) a++;
  while (b > a && isspace((unsigned char)v[b - 1])) b--;
  return string(v.substr(a, b - a));
}
string upper(std::string_view v) { string r(v); for (auto& c : r) c = toupper((unsigned char)c); return r; }
string lower(std::string_view v) { string r(v); for (auto& c : r) c = tolower((unsigned char)c); return r; }
vector<string> split(std::string_view v, char sep) {
  vector<string> out;
  size_t start = 0;
  for (size_t i = 0; i <= v.size(); i++) {
    if (i == v.size() || v[i] == sep) { out.emplace_back(v.substr(start, i - start)); start = i + 1; }
  }
  return out;
}
vector<string> split_ws(std::string_view v) {
  vector<string> out;
  size_t i = 0;
  while (i < v.size()) {
    while (i < v.size() && isspace((unsigned char)v[i])) i++;
    size_t s = i;
    while (i < v.size() && !isspace((unsigned char)v[i])) i++;
    if (i > s) out.emplace_back(v.substr(s, i - s));
  }
  return out;
}
vector<string> split_lines(std::string_view v) {
  vector<string> out = split(v, '\n');
  if (!out.empty() && out.back().empty()) out.pop_back();
  for (auto& l : out) if (!l.empty() && l.back() == '\r') l.pop_back();
  return out;
}
string join(const vector<string>& v, std::string_view sep) {
  string r;
  for (size_t i = 0; i < v.size(); i++) { if (i) r += sep; r += v[i]; }
  return r;
}
bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }
bool icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
    size_t j = 0;
    while (j < needle.size() && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
    if (j == needle.size()) return true;
  }
  return false;
}
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}
string replace_all(string s, std::string_view from, std::string_view to) {
  if (from.empty()) return s;
  size_t p = 0;
  while ((p = s.find(from, p)) != string::npos) { s.replace(p, from.size(), to); p += to.size(); }
  return s;
}
bool is_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) if (!isdigit((unsigned char)c)) return false;
  return true;
}
long to_long(std::string_view s, long dflt) {
  string t = trim(s);
  if (t.empty()) return dflt;
  char* end = nullptr;
  long v = strtol(t.c_str(), &end, 10);
  return (end && *end == 0) ? v : dflt;
}
double to_double(std::string_view s, double dflt) {
  string t = trim(s);
  if (t.empty()) return dflt;
  char* end = nullptr;
  double v = strtod(t.c_str(), &end);
  return (end && *end == 0) ? v : dflt;
}
string fmt(const char* f, ...) {
  char buf[8192];
  va_list ap;
  va_start(ap, f);
  int n = vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  if (n < 0) return {};
  if ((size_t)n < sizeof(buf)) return string(buf, n);
  string big(n + 1, '\0');
  va_start(ap, f);
  vsnprintf(big.data(), n + 1, f, ap);
  va_end(ap);
  big.resize(n);
  return big;
}
string read_file(const string& path) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return {};
  string out;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
  fclose(fp);
  return out;
}
bool write_file(const string& path, std::string_view content) {
  static std::atomic<unsigned> seq{0};                          // two threads writing one path get two temporaries
  string tmp = path + ".tmp" + std::to_string(getpid()) + "_" + std::to_string(++seq);
  FILE* fp = fopen(tmp.c_str(), "wb");
  if (!fp) return false;
  bool ok = fwrite(content.data(), 1, content.size(), fp) == content.size();
  ok = (fclose(fp) == 0) && ok;
  if (!ok) { unlink(tmp.c_str()); return false; }
  if (rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
  return true;
}
bool append_file(const string& path, std::string_view content) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return false;
  ssize_t w = write(fd, content.data(), content.size());
  close(fd);
  return w == (ssize_t)content.size();
}
bool file_exists(const string& path) { struct stat st; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
bool dir_exists(const string& path) { struct stat st; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool is_executable(const string& path) { struct stat st; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0; }
int64_t file_size(const string& path) { struct stat st; return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1; }
int64_t file_mtime(const string& path) { struct stat st; return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_mtime : 0; }
bool mkdirs(const string& path) { std::error_code ec; fs::create_directories(path, ec); return dir_exists(path); }
bool remove_tree(const string& path) { std::error_code ec; fs::remove_all(path, ec); return !ec || !fs::exists(path); }
// <prefix><pid> files and directories this user left in /tmp, also with a _<n> or .<ext> after the
// pid: the owning process was killed before it could remove its own, so the next run of the same
// kind takes it away. Only this user's are touched, and only when that pid is gone.
int sweep_stale_tmp(const string& prefix) {
  string dir = dirname_of(prefix), lead = basename_of(prefix);
  int n = 0;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    string name = e.path().filename().string();
    if (name.size() <= lead.size() || name.compare(0, lead.size(), lead) != 0) continue;
    string rest = name.substr(lead.size());
    size_t end = rest.find_first_not_of("0123456789");
    string digits = rest.substr(0, end);
    if (digits.empty() || digits.size() > 10 || (end != string::npos && rest[end] != '_' && rest[end] != '.')) continue;
    pid_t pid = (pid_t)atol(digits.c_str());
    if (pid <= 0 || pid == getpid() || pid_alive(pid)) continue;
    struct stat st;
    if (lstat(e.path().c_str(), &st) != 0 || st.st_uid != getuid()) continue;
    if (remove_tree(e.path().string())) n++;
  }
  return n;
}
// files, directories and symlinks are copied; sockets, fifos and devices are left out
static bool copy_dir_rec(const fs::path& from, const fs::path& to, string* why) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (!fs::is_directory(to, ec)) { if (why) *why = "cannot make " + to.string(); return false; }
  for (auto& e : fs::directory_iterator(from, ec)) {
    fs::file_status st = e.symlink_status(ec);
    fs::path dst = to / e.path().filename();
    if (fs::is_symlink(st)) { fs::copy_symlink(e.path(), dst, ec); }
    else if (fs::is_directory(st)) { if (!copy_dir_rec(e.path(), dst, why)) return false; continue; }
    else if (fs::is_regular_file(st)) { fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec); }
    else continue;
    if (ec) { if (why) *why = ec.message() + " on " + e.path().string(); return false; }
  }
  fs::permissions(to, fs::status(from, ec).permissions(), ec);
  return true;
}
bool copy_tree(const string& from, const string& to, string* why) {
  std::error_code ec;
  if (fs::is_regular_file(from, ec)) return copy_file(from, to);
  fs::remove_all(to, ec);
  return copy_dir_rec(from, to, why);
}
string basename_of(string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t s = p.rfind('/');
  return s == string::npos ? p : p.substr(s + 1);
}
string dirname_of(const string& p) {
  string q = p;
  while (q.size() > 1 && q.back() == '/') q.pop_back();
  size_t s = q.rfind('/');
  if (s == string::npos) return ".";
  if (s == 0) return "/";
  return q.substr(0, s);
}
string mount_point_of(const string& p) {
  string cur = abs_path(p);
  struct stat st, up;
  if (stat(cur.c_str(), &st) != 0) return "/";
  while (cur != "/") {
    string parent = dirname_of(cur);
    if (stat(parent.c_str(), &up) != 0 || up.st_dev != st.st_dev) break;
    cur = parent;
  }
  return cur;
}
string abs_path(const string& p) {
  std::error_code ec;
  fs::path a = fs::absolute(p, ec);
  fs::path c = fs::weakly_canonical(a, ec);
  return ec ? a.string() : c.string();
}
string home_dir() {
  const char* h = getenv("HOME");
  return h && *h ? string(h) : string("/root");
}
string self_exe() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  return buf;
}
static string tm_fmt(const char* f) {
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[64];
  strftime(buf, sizeof(buf), f, &lt);
  return buf;
}
string now_hms() { return tm_fmt("%H:%M:%S"); }
string now_stamp() { return tm_fmt("%Y-%m-%d %H:%M:%S"); }
string date_ddmmyy() { return tm_fmt("%d%m%y"); }
int64_t now_s() { return (int64_t)time(nullptr); }
double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
string human_bytes(uint64_t b) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  return i == 0 ? fmt("%llu B", (unsigned long long)b) : fmt("%.1f %s", v, u[i]);
}
string human_secs(long s) {
  if (s < 0) s = 0;
  long d = s / 86400; s %= 86400;
  long h = s / 3600; s %= 3600;
  long m = s / 60; s %= 60;
  if (d) return fmt("%ldd %02ld:%02ld:%02ld", d, h, m, s);
  return fmt("%02ld:%02ld:%02ld", h, m, s);
}
uint64_t fnv1a(std::string_view s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}
string hex64(uint64_t v) { return fmt("%016llx", (unsigned long long)v); }
string sh_quote(const string& s) {
  string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}
string tail_lines(const string& text, size_t n) {
  if (n == 0) return {};
  size_t pos = text.size();
  size_t seen = 0;
  bool trailing_nl = !text.empty() && text.back() == '\n';
  if (trailing_nl) pos--;
  while (pos > 0) {
    size_t p = text.rfind('\n', pos - 1);
    seen++;
    if (seen >= n) { pos = (p == string::npos) ? 0 : p + 1; return text.substr(pos); }
    if (p == string::npos) return text;
    pos = p;
  }
  return text;
}
string cap_text(const string& text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  size_t half = max_bytes / 2;
  return text.substr(0, half) + "\n[... " + std::to_string(text.size() - max_bytes) + " bytes cut ...]\n" +
         text.substr(text.size() - half);
}
uint64_t dir_free_bytes(const string& path) {
  struct statvfs st;
  if (statvfs(path.c_str(), &st) != 0) return 0;
  return (uint64_t)st.f_bavail * st.f_frsize;
}
uint64_t dir_total_bytes(const string& path) {
  struct statvfs st;
  if (statvfs(path.c_str(), &st) != 0) return 0;
  return (uint64_t)st.f_blocks * st.f_frsize;
}
int dir_used_pct(const string& path) {
  uint64_t t = dir_total_bytes(path);
  if (!t) return 0;
  return (int)(100 - (dir_free_bytes(path) * 100) / t);
}
bool meminfo_parse(const string& text, uint64_t& total_kb, uint64_t& avail_kb) {
  uint64_t free_kb = 0;
  total_kb = avail_kb = 0;
  for (auto& l : split_lines(text)) {
    auto f = split_ws(l);
    if (f.size() < 2) continue;
    if (f[0] == "MemTotal:") total_kb = to_long(f[1]);
    else if (f[0] == "MemAvailable:") avail_kb = to_long(f[1]);
    else if (f[0] == "MemFree:") free_kb = to_long(f[1]);
  }
  if (avail_kb == 0) avail_kb = free_kb;                       // Cygwin's /proc/meminfo has no MemAvailable line
  return total_kb > 0;
}
static bool meminfo(uint64_t& total_kb, uint64_t& avail_kb) { return meminfo_parse(read_file("/proc/meminfo"), total_kb, avail_kb); }
int ram_used_pct() {
  uint64_t t, a;
  if (!meminfo(t, a)) return 0;
  return (int)(100 - (a * 100) / t);
}
uint64_t ram_total_bytes() { uint64_t t, a; return meminfo(t, a) ? t * 1024 : 0; }
uint64_t ram_available_bytes() { uint64_t t, a; return meminfo(t, a) ? a * 1024 : 0; }
int cpu_threads() { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
double load_average() { double l[1]; return getloadavg(l, 1) == 1 ? l[0] : 0; }

// fork+exec with stdout and stderr on one pipe, an optional deadline, an optional cwd and env
CmdResult run_capture_in(const vector<string>& argv, const string& stdin_text, int timeout_s, const string& cwd,
                         const vector<string>& env_add) {
  CmdResult r;
  if (argv.empty()) return r;
  // close-on-exec, so a child another thread forks meanwhile does not hold our pipe ends open;
  // the dup2 onto 0, 1 and 2 below clears the flag on the copies this child keeps
  int pfd[2], ifd[2] = {-1, -1};
  if (pipe2(pfd, O_CLOEXEC) != 0) return r;
  if (pipe2(ifd, O_CLOEXEC) != 0) { close(pfd[0]); close(pfd[1]); return r; }
  pid_t pid = fork();
  if (pid < 0) { close(pfd[0]); close(pfd[1]); close(ifd[0]); close(ifd[1]); return r; }
  if (pid == 0) {
    close(pfd[0]);
    close(ifd[1]);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);
    if (pfd[1] > 2) close(pfd[1]);
    dup2(ifd[0], 0);
    if (ifd[0] > 2) close(ifd[0]);
    for (int f = 0; f <= 2; f++) fcntl(f, F_SETFD, 0);          // a pipe end that already was 0, 1 or 2 kept its close-on-exec
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    for (auto& e : env_add) putenv(strdup(e.c_str()));
    setpgid(0, 0);
    vector<char*> av;
    for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    if (argv[0].find('/') != string::npos) execv(av[0], av.data());
    else execvp(av[0], av.data());
    _exit(127);
  }
  close(pfd[1]);
  // The input can be larger than the pipe buffer, so it is fed and the output drained in the one
  // loop, under the one deadline: neither side waits on the other. The parent holds the read end of
  // the input pipe open as well, so a child that never reads its stdin cannot break the pipe under
  // a write; the loop still ends when the child's output ends.
  fcntl(ifd[1], F_SETFL, O_NONBLOCK);
  if (stdin_text.empty()) { close(ifd[1]); ifd[1] = -1; }
  size_t off = 0;
  double deadline = timeout_s > 0 ? now_ms() + timeout_s * 1000.0 : 0;
  bool timed_out = false;
  char buf[65536];
  for (;;) {
    struct pollfd p[2] = {{pfd[0], POLLIN, 0}, {ifd[1], POLLOUT, 0}};   // a negative fd is skipped
    int wait_ms = deadline ? (int)std::max(0.0, deadline - now_ms()) : -1;
    int pr = poll(p, 2, wait_ms);
    if (pr == 0) { timed_out = true; kill(-pid, SIGKILL); kill(pid, SIGKILL); break; }
    if (pr < 0) { if (errno == EINTR) continue; break; }
    if (p[1].revents & POLLOUT) {
      ssize_t w = write(ifd[1], stdin_text.data() + off, stdin_text.size() - off);
      if (w > 0) off += (size_t)w;
      bool again = w < 0 && (errno == EINTR || errno == EAGAIN);
      if (!again && (w <= 0 || off == stdin_text.size())) { close(ifd[1]); ifd[1] = -1; }
    }
    if (p[0].revents) {
      ssize_t n = read(pfd[0], buf, sizeof(buf));
      if (n > 0) r.out.append(buf, n);
      else if (!(n < 0 && errno == EINTR)) break;
    }
  }
  if (ifd[1] >= 0) close(ifd[1]);
  close(ifd[0]);
  close(pfd[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (timed_out) r.rc = 124;
  else if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
  else if (WIFSIGNALED(st)) r.rc = 128 + WTERMSIG(st);
  else r.rc = -1;
  return r;
}
CmdResult run_capture(const vector<string>& argv, int timeout_s, const string& cwd,
                      const vector<string>& env_add) {
  CmdResult r;
  if (argv.empty()) return r;
  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC) != 0) return r;                    // close-on-exec: see run_capture_in
  pid_t pid = fork();
  if (pid < 0) { close(pfd[0]); close(pfd[1]); return r; }
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);
    if (pfd[1] > 2) close(pfd[1]);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); if (devnull > 2) close(devnull); }
    for (int f = 0; f <= 2; f++) fcntl(f, F_SETFD, 0);          // as in run_capture_in
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    for (auto& e : env_add) putenv(strdup(e.c_str()));
    setpgid(0, 0);
    vector<char*> av;
    for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    if (argv[0].find('/') != string::npos) execv(av[0], av.data());
    else execvp(av[0], av.data());
    _exit(127);
  }
  close(pfd[1]);
  double deadline = timeout_s > 0 ? now_ms() + timeout_s * 1000.0 : 0;
  bool timed_out = false;
  char buf[65536];
  for (;;) {
    struct pollfd p{pfd[0], POLLIN, 0};
    int wait_ms = deadline ? (int)std::max(0.0, deadline - now_ms()) : -1;
    int pr = poll(&p, 1, wait_ms);
    if (pr == 0) { timed_out = true; kill(-pid, SIGKILL); kill(pid, SIGKILL); break; }
    if (pr < 0) { if (errno == EINTR) continue; break; }
    ssize_t n = read(pfd[0], buf, sizeof(buf));
    if (n <= 0) break;
    r.out.append(buf, n);
  }
  close(pfd[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (timed_out) r.rc = 124;
  else if (WIFEXITED(st)) r.rc = WEXITSTATUS(st);
  else if (WIFSIGNALED(st)) r.rc = 128 + WTERMSIG(st);
  else r.rc = -1;
  return r;
}
CmdResult run_shell(const string& script, int timeout_s, const string& cwd) {
  return run_capture({"/bin/bash", "-c", script}, timeout_s, cwd);
}

// ---------------------------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------------------------
static std::mutex g_log_mtx;
static FILE* g_log_fp = nullptr;
static bool g_log_quiet = false;
static std::deque<string> g_log_ring;
static const size_t LOG_RING = 400;

void log_open(const string& path) {
  std::lock_guard<std::mutex> lk(g_log_mtx);
  if (g_log_fp) fclose(g_log_fp);
  g_log_fp = fopen(path.c_str(), "a");
}
void log_set_quiet(bool q) { g_log_quiet = q; }
static void log_emit(const char* level, const char* f, va_list ap) {
  char buf[8192];
  vsnprintf(buf, sizeof(buf), f, ap);
  string line = "[" + now_hms() + "] " + (level ? string(level) + " " : string()) + buf;
  std::lock_guard<std::mutex> lk(g_log_mtx);
  if (g_log_fp) { fputs(line.c_str(), g_log_fp); fputc('\n', g_log_fp); fflush(g_log_fp); }
  if (!g_log_quiet || !g_log_fp) { fputs(line.c_str(), stderr); fputc('\n', stderr); }
  g_log_ring.push_back(line);
  while (g_log_ring.size() > LOG_RING) g_log_ring.pop_front();
}
void logline(const char* f, ...) { va_list ap; va_start(ap, f); log_emit(nullptr, f, ap); va_end(ap); }
void logwarn(const char* f, ...) { va_list ap; va_start(ap, f); log_emit("WARNING:", f, ap); va_end(ap); }
void die(const char* f, ...) {
  va_list ap; va_start(ap, f); log_emit("FATAL:", f, ap); va_end(ap);
  exit(1);
}
vector<string> log_recent(size_t n) {
  std::lock_guard<std::mutex> lk(g_log_mtx);
  vector<string> out;
  size_t start = g_log_ring.size() > n ? g_log_ring.size() - n : 0;
  for (size_t i = start; i < g_log_ring.size(); i++) out.push_back(g_log_ring[i]);
  return out;
}

string stamp_of(int64_t epoch) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[64];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
  return buf;
}
bool copy_file(const string& from, const string& to) {
  std::error_code ec;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  return !ec;
}
bool move_tree(const string& from, const string& to, string* why) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) return true;
  if (ec.value() != EXDEV) { if (why) *why = ec.message(); return false; }
  if (!copy_tree(from, to, why)) return false;
  return remove_tree(from);
}
