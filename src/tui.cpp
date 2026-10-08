// Created by Roel Van de Paar, MariaDB
// tui.cpp - the live view of a run: the panels corlogic draws, plain ANSI on a 256-colour terminal,
// no ncurses. It reads what the run writes (status.txt, the ledger, the log) and asks for a pause or
// a stop through <workdir>/omnium.ctl, so the view never touches the run's own state. Not a
// terminal, or TUI=0: the log is followed instead.
#include "verbs.h"
#include "winterm.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

namespace fs = std::filesystem;

namespace {
const char* C_RESET = "\033[0m";
const char* C_DIM = "\033[38;5;244m";
const char* C_HEAD = "\033[38;5;39m";
const char* C_OK = "\033[38;5;35m";
const char* C_WARN = "\033[38;5;214m";
const char* C_BAD = "\033[38;5;196m";
const char* C_NEW = "\033[38;5;226m";

struct Term {
  struct termios saved;
  bool raw = false;
  void enter() {
    if (tcgetattr(0, &saved) != 0) return;
    struct termios t = saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);                   // Ctrl+C arrives as a key, so the terminal is put back on the way out
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) == 0) raw = true;
    printf("\033[?25l");                                  // cursor off
    fflush(stdout);
  }
  void leave() {
    if (raw) tcsetattr(0, TCSANOW, &saved);
    raw = false;
    printf("\033[?25h\033[0m\n");
    fflush(stdout);
  }
};
void term_size(int& rows, int& cols) {
  struct winsize w{};
  rows = 40; cols = 120;
  if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
    rows = w.ws_row;
    cols = w.ws_col;
    // the rows under the taskbar, or off the screen's edge, are the terminal's but nobody's to see
    rows = std::min(rows, std::max(12, rows - term_rows_hidden(rows)));
  }
}
string clip(const string& s, size_t w) {
  if (s.size() <= w) return s;
  return w > 1 ? s.substr(0, w - 1) + "~" : s.substr(0, w);
}
string kvget(const vector<std::pair<string, string>>& kv, const string& k) {
  for (auto& p : kv) if (p.first == k) return p.second;
  return "";
}
string dur(int64_t s) {
  if (s < 60) return fmt("%llds", (long long)s);
  if (s < 3600) return fmt("%lldm%02llds", (long long)(s / 60), (long long)(s % 60));
  return fmt("%lldh%02lldm", (long long)(s / 3600), (long long)((s % 3600) / 60));
}
vector<string> tail_of(const string& path, size_t n) {
  vector<string> out;
  string text = tail_lines(read_file(path), n);
  for (auto& l : split_lines(text)) if (!l.empty()) out.push_back(l);
  return out;
}
// the newest run under /data, or the one named
string pick_run(const string& want) {
  if (!want.empty()) {
    string d = want;
    if (d.find('/') == string::npos) d = g_cfg.data_dir + "/" + (starts_with(d, "O") ? d : "O" + d);
    return d;
  }
  string best;
  int64_t best_t = 0;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(g_cfg.data_dir, ec)) {
    string n = e.path().filename().string();
    if (n.size() != 7 || n[0] != 'O' || !is_digits(n.substr(1))) continue;
    string sf = e.path().string() + "/status.txt";
    if (!file_exists(sf)) continue;
    // file_mtime, not fs::last_write_time: libstdc++'s file_time_type has an epoch of its own
    int64_t secs = file_mtime(sf);
    bool live = pid_is_live_omnium((pid_t)to_long(trim(read_file(e.path().string() + "/omnium.pid")), 0));
    if (live) secs += 1LL << 40;                          // a live run wins over any finished one
    if (secs > best_t) { best_t = secs; best = e.path().string(); }
  }
  return best;
}
struct View {
  string dir, id;
  bool live = false;
  int64_t started = 0;
  vector<std::pair<string, string>> kv;
  vector<string> slots, builds, log;
  vector<std::pair<string, long>> dups;                   // known UID -> trials it ate
  vector<std::pair<string, string>> inbox;                // item -> title
  long saved = 0, candidates = 0, reducing = 0, filed = 0;
};
void collect(View& v) {
  v.kv = status_read(v.dir);
  v.id = basename_of(v.dir);
  pid_t pid = (pid_t)to_long(trim(read_file(v.dir + "/omnium.pid")), 0);
  v.live = pid_is_live_omnium(pid);
  v.slots.clear(); v.builds.clear(); v.dups.clear();
  for (auto& p : v.kv) {
    if (starts_with(p.first, "slot_")) v.slots.push_back(p.second);
    else if (starts_with(p.first, "build_")) v.builds.push_back(p.first.substr(6) + "  " + p.second);
  }
  // the ledger: the start, the known-bug counts, the saved trials
  std::map<string, long> known;
  v.saved = v.candidates = v.reducing = 0;
  for (auto& l : split_lines(read_file(v.dir + "/omnium.ledger"))) {
    vector<string> f = split_ws(l);
    if (f.size() < 3) continue;
    if (f[1] == "run" && f[2] == "created") v.started = to_long(f[0], 0);
    if (f[1] == "trial" && f.size() >= 6) {
      const string& outcome = f[5];                        // ts trial <n> <build> <area> <outcome> <uid>
      if (starts_with(outcome, "saved")) v.saved++;
      if (outcome == "known") { string uid = trim(l.substr(l.find(" known ") + 7)); if (!uid.empty()) known[uid]++; }
    }
    if (f[1] == "reduce" && f.size() >= 4 && f[3] == "done") v.reducing++;   // ts reduce <n> done|no result rc=N
  }
  vector<std::pair<string, long>> d(known.begin(), known.end());
  std::sort(d.begin(), d.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (d.size() > 5) d.resize(5);
  v.dups = d;
  v.log = tail_of(v.dir + "/" + v.id + ".log", 400);   // enough for a tall terminal
  // the inbox items of this run
  v.inbox.clear();
  v.filed = 0;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(g_paths.human_queue, ec)) {
    string n = e.path().filename().string();
    if (!ends_with(n, ".report")) continue;
    string item = n.substr(0, n.size() - 7);
    if (!starts_with(item, v.id + "_")) continue;
    string title = report_field(report_header(read_file(e.path().string()), nullptr), "Title");
    bool filed = file_exists(g_paths.human_queue + "/" + item + ".filed");
    bool ok = file_exists(g_paths.human_queue + "/" + item + ".ok");
    if (filed) v.filed++;
    v.inbox.push_back({item + (filed ? " [filed]" : ok ? " [ok]" : ""), title});
  }
  v.candidates = (long)v.inbox.size();
}
// one line of the frame: the text with its colour codes, and how wide it really is
struct Line {
  string s;
  size_t vis = 0;
  void add(const char* colour, const string& text) {
    if (colour) s += colour;
    s += text;
    if (colour) s += C_RESET;
    vis += text.size();
  }
};
string human_count(unsigned long long n) {
  if (n < 10000) return std::to_string(n);
  if (n < 1000000) return fmt("%.1fk", n / 1000.0);
  return fmt("%.2fM", n / 1000000.0);
}
string frame_text(const View& v, int rows, int cols, int log_lines, const string& msg) {
  int W = std::clamp(cols - 1, 60, 240);
  int inner = W - 2;
  string frame = "\033[H";
  auto rule = [&](const char* lft, const char* rgt, const string& title) {
    string ln = lft;
    int used = 0;
    if (!title.empty()) { ln += string("\xe2\x94\x80 ") + C_HEAD + title + C_RESET + " "; used = (int)title.size() + 3; }
    for (int i = used; i < inner; i++) ln += "\xe2\x94\x80";
    ln += rgt;
    frame += ln + "\033[K\n";
  };
  auto push = [&](Line& L) {
    string ln = "\xe2\x94\x82";
    ln += L.s;
    for (size_t i = L.vis; i < (size_t)inner; i++) ln += ' ';
    ln += "\xe2\x94\x82\033[K\n";
    frame += ln;
  };
  auto text_line = [&](const char* colour, const string& text) {
    Line L;
    L.add(nullptr, " ");
    L.add(colour, clip(text, (size_t)inner - 1));
    push(L);
  };
  string state = kvget(v.kv, "state");
  if (!v.live) state = "finished";
  const char* sc = state == "running" ? C_OK : state == "paused" ? C_WARN : state == "finished" ? C_DIM : C_WARN;
  // header: what this is, which run, and the keys
  rule("\xe2\x94\x8c", "\xe2\x94\x90", "");
  {
    Line L;
    L.add(C_HEAD, string(" omnium ") + OMNIUM_VERSION);
    L.add(nullptr, "  ");
    L.add(nullptr, v.id);
    L.add(nullptr, "  ");
    L.add(sc, state);
    string right = "[q]uit [p]ause [r]esume [s]top [S]top-now [l/L]og [?]  ";
    string mid = "  up " + (v.started ? dur(now_s() - v.started) : string("-"));
    if (L.vis + mid.size() + right.size() + 2 < (size_t)inner) {
      L.add(C_DIM, mid);
      L.add(nullptr, string(inner - L.vis - right.size(), ' '));
      L.add(C_DIM, right);
    }
    push(L);
  }
  // the counters, in aligned columns
  {
    Line L;
    int cw = std::max(18, inner / 4);
    auto cell = [&](const char* label, const string& value, const char* vc) {
      string lab = string(" ") + label + " ";
      L.add(C_DIM, lab);
      L.add(vc, value);
      size_t want = L.vis + (cw - (lab.size() + value.size()) % cw) % cw;
      while (L.vis < want) L.add(nullptr, " ");
    };
    int ram = (int)to_long(kvget(v.kv, "ram_pct"), 0), shm = (int)to_long(kvget(v.kv, "shm_pct"), 0);
    cell("trials", kvget(v.kv, "finished") + "/" + kvget(v.kv, "launched"), nullptr);
    cell("statements", human_count((unsigned long long)to_long(kvget(v.kv, "performed"), 0)), nullptr);
    cell("RAM", fmt("%d%%/%d%%", ram, g_cfg.ram_cap_pct), ram >= g_cfg.ram_cap_pct ? C_BAD : ram >= g_cfg.ram_cap_pct - 10 ? C_WARN : C_OK);
    cell("shm", fmt("%d%%/%d%%", shm, g_cfg.shm_cap_pct), shm >= g_cfg.shm_cap_pct ? C_BAD : shm >= g_cfg.shm_cap_pct - 10 ? C_WARN : C_OK);
    push(L);
  }
  // The lists share what the screen has left once the fixed lines, the least the log gets and the
  // closing rule are taken off, so a small terminal is never overrun. A list that does not fit ends
  // in a "+N more" line. What the lists leave goes to the log, so the frame fills the screen.
  string jobs = kvget(v.kv, "jobs"), queue = kvget(v.kv, "queue");
  bool outcomes_any = false;
  for (auto& p : v.kv) if (starts_with(p.first, "outcome_")) outcomes_any = true;
  int log_n = std::max(1, log_lines);
  int fixed = 3 + 1 + 1 + (jobs.empty() ? 0 : 1) + 2 + (v.dups.empty() ? 0 : 1) + (outcomes_any ? 2 : 0) + 1 + 1;
  int room = rows - 1 - fixed - log_n;
  if (room < 2) { log_n = std::max(1, log_n - (2 - room)); room = std::max(0, rows - 1 - fixed - log_n); }   // the log gives way first
  auto take = [&](size_t have, int share, int least) { int cap = std::min<int>((int)have, std::max(least, share)); room -= cap; return (size_t)cap; };
  auto capped = [&](size_t have, size_t cap, const std::function<void(size_t)>& one) {
    if (cap == 0) return;
    size_t show = have > cap ? cap - 1 : have;
    for (size_t i = 0; i < show; i++) one(i);
    if (have > show) text_line(C_DIM, fmt("+%zu more", have - show));
  };
  size_t cap_builds = take(v.builds.empty() ? 1 : v.builds.size(), room / 4, 1);   // builds and slots always get a line
  size_t cap_slots = take(v.slots.empty() ? 1 : v.slots.size(), room / 2, 1);
  size_t cap_inbox = take(v.inbox.size(), room / 2, 0);
  size_t cap_dups = take(v.dups.size(), room, 0);
  log_n += std::max(0, room);   // the log takes the rows the lists left
  // builds
  rule("\xe2\x94\x9c", "\xe2\x94\xa4", "builds");
  if (v.builds.empty()) text_line(C_DIM, "none");
  else capped(v.builds.size(), cap_builds, [&](size_t i) { text_line(nullptr, v.builds[i]); });
  // slots
  rule("\xe2\x94\x9c", "\xe2\x94\xa4", "slots (" + kvget(v.kv, "slots") + " configured)");
  if (v.slots.empty()) text_line(C_DIM, "none busy");
  else capped(v.slots.size(), cap_slots, [&](size_t i) { text_line(nullptr, fmt("s%02zu ", i) + v.slots[i]); });
  if (!jobs.empty()) text_line(C_NEW, "pipeline: " + jobs + (queue.empty() ? "" : "   queued " + queue));
  // the pipeline and the inbox
  rule("\xe2\x94\x9c", "\xe2\x94\xa4", "pipeline");
  text_line(nullptr, fmt("saved %ld   reduced %ld   inbox %ld   filed %ld", v.saved, v.reducing, v.candidates, v.filed));
  capped(v.inbox.size(), cap_inbox, [&](size_t i) {
    auto& p = v.inbox[i];
    Line L;
    L.add(nullptr, " ");
    L.add(C_NEW, clip(p.first, 34));
    L.add(nullptr, string(std::max<size_t>(1, 36 - std::min<size_t>(34, p.first.size())), ' '));
    L.add(nullptr, clip(p.second, (size_t)std::max(10, inner - 38)));
    push(L);
  });
  if (!v.dups.empty()) {
    rule("\xe2\x94\x9c", "\xe2\x94\xa4", "known bugs eating the trials");
    capped(v.dups.size(), cap_dups, [&](size_t i) { auto& d = v.dups[i]; text_line(C_DIM, fmt("%4ld  ", d.second) + clip(d.first, (size_t)std::max(20, inner - 8))); });
  }
  vector<string> outcomes;
  for (auto& p : v.kv) if (starts_with(p.first, "outcome_")) outcomes.push_back(p.first.substr(8) + " " + p.second);
  if (!outcomes.empty()) {
    rule("\xe2\x94\x9c", "\xe2\x94\xa4", "outcomes");
    text_line(nullptr, join(outcomes, "   "));
  }
  // the log tail
  rule("\xe2\x94\x9c", "\xe2\x94\xa4", "log");
  size_t n = (size_t)log_n;
  size_t from = v.log.size() > n ? v.log.size() - n : 0;
  for (size_t i = from; i < v.log.size(); i++) {
    const string& l = v.log[i];
    const char* c = l.find("[WARN]") != string::npos ? C_WARN : l.find("saved-new") != string::npos ? C_NEW : C_DIM;
    text_line(c, l);
  }
  // blank rows keep the pane at its height
  for (size_t shown = v.log.size() - from; shown < n; shown++) text_line(nullptr, "");
  rule("\xe2\x94\x94", "\xe2\x94\x98", "");
  frame += "\033[J";
  if (!msg.empty()) frame += "\033[" + std::to_string(rows) + ";1H" + C_WARN + clip(msg, (size_t)cols) + C_RESET + "\033[K";
  return frame;
}
void draw(const View& v, int rows, int cols, int log_lines, const string& msg) {
  fputs(frame_text(v, rows, cols, log_lines, msg).c_str(), stdout);
  fflush(stdout);
}
void ctl(const View& v, const string& word) { write_file(v.dir + "/omnium.ctl", word + "\n"); }
}  // namespace

// the screen lines a frame of that shape takes, for the check that a small terminal is never overrun
int tui_frame_lines(int rows, int cols, int builds, int slots, int inbox, int dups, int log_lines) {
  View v;
  v.live = true;
  v.kv = {{"state", "running"}, {"slots", std::to_string(slots)}, {"jobs", "reduce 1"}, {"outcome_saved-new", "1"}};
  for (int i = 0; i < builds; i++) v.builds.push_back(fmt("build %d", i));
  for (int i = 0; i < slots; i++) v.slots.push_back(fmt("trial %d", i));
  for (int i = 0; i < inbox; i++) v.inbox.push_back({fmt("item %d", i), "a title"});
  for (int i = 0; i < dups; i++) v.dups.push_back({fmt("uid %d", i), (long)i});
  for (int i = 0; i < 100; i++) v.log.push_back(fmt("line %d", i));
  string f = frame_text(v, rows, cols, log_lines, "a message");
  return (int)std::count(f.begin(), f.end(), '\n');
}

// omnium tui [<run>] [--plain]
int cmd_tui(const Args& a) {
  string want;
  bool plain = false, one_frame = false;
  for (auto& s : a) {
    if (s == "--plain") plain = true;
    else if (s == "--frame") one_frame = true;
    else if (starts_with(s, "--")) { fprintf(stderr, "usage: omnium tui [<run>] [--plain] [--frame]\n"); return 2; }
    else want = s;
  }
  string dir = pick_run(want);
  if (dir.empty() || !dir_exists(dir)) { fprintf(stderr, "omnium tui: no run%s under %s\n", want.empty() ? "" : (" " + want).c_str(), g_cfg.data_dir.c_str()); return 1; }
  View v;
  v.dir = abs_path(dir);
  v.id = basename_of(v.dir);
  if (one_frame) {                                            // one picture of the run, then out
    int rows = 40, cols = 120;
    if (isatty(1)) term_size(rows, cols);
    collect(v);
    draw(v, rows, cols, 8, "");
    printf("\n");
    return 0;
  }
  const char* tui_env = getenv("TUI");
  bool tty = isatty(1) && isatty(0);
  if (plain || !tty || (tui_env && string(tui_env) == "0")) {
    // the log, followed; the same view a run in a screen writes
    printf("following %s (no terminal, or TUI=0)\n", (v.dir + "/" + v.id + ".log").c_str());
    size_t seen = 0;
    for (;;) {
      vector<string> lines = split_lines(read_file(v.dir + "/" + v.id + ".log"));
      for (size_t i = seen; i < lines.size(); i++) if (!lines[i].empty()) printf("%s\n", lines[i].c_str());
      seen = lines.size();
      fflush(stdout);
      collect(v);
      if (!v.live) return 0;
      sleep(2);
    }
  }
  Term t;
  t.enter();
  printf("\033[2J");
  int log_lines = 8;
  string msg;
  int64_t msg_until = 0;
  int rows, cols;
  for (;;) {
    term_size(rows, cols);
    collect(v);
    if (msg_until && now_s() > msg_until) { msg.clear(); msg_until = 0; }
    draw(v, rows, cols, log_lines, msg);
    // a second of keys, then the next frame
    for (int i = 0; i < 10; i++) {
      char c = 0;
      if (read(0, &c, 1) == 1) {
        auto say = [&](const string& m) { msg = m; msg_until = now_s() + 3; };
        if (c == 'q' || c == 3) { t.leave(); return 0; }
        else if (c == 'p') { ctl(v, "pause"); say("pause asked"); }
        else if (c == 'r' || c == 'P') { ctl(v, "resume"); say("resume asked"); }   // old key: P
        else if (c == 's') { ctl(v, "stop"); say("stop asked: the running trials finish first"); }
        else if (c == 'S') { ctl(v, "stop-now"); say("stop now asked"); }
        else if (c == 'l') { log_lines = std::min(40, log_lines + 4); }
        else if (c == 'L') { log_lines = std::max(2, log_lines - 4); }
        else if (c == 12) printf("\033[2J");   // Ctrl+L redraws
        else if (c == '?') say("q quit; p pause; r resume; s stop after the running trials; S stop now; l/L more or less log; ^L redraw");
        break;
      }
      usleep(100000);
    }
  }
}
