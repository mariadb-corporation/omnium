// Created by Roel Van de Paar, MariaDB
// cli.cpp - `omnium cli`: a bash with an rcfile omnium writes. Every shortcut is a function that
// calls this binary, so there is one implementation of each action. The prompt says where you are
// (a basedir, a workdir, a trial), and h prints the shortcut list as a box.
#include "verbs.h"
#include <sys/ioctl.h>

namespace {
// brief is what the box shows, help is what h <name> answers with
struct Short { const char* group; const char* name; const char* body; const char* brief; const char* help; };
const Short SHORTS[] = {
  {"navigate", "d",    "omnium_goto dbg \"$@\"",                     "newest debug build",     "newest debug build; d 12.3, d es 11.4, d MDEV-1234"},
  {"navigate", "o",    "omnium_goto opt \"$@\"",                     "newest optimised build", "newest optimised build"},
  {"navigate", "ds",   "omnium_goto uba-dbg \"$@\"",                 "newest UBSAN+ASAN dbg",  "newest UBSAN+ASAN debug build"},
  {"navigate", "os",   "omnium_goto uba-opt \"$@\"",                 "newest UBSAN+ASAN opt",  "newest UBSAN+ASAN optimised build"},
  {"navigate", "dm",   "omnium_goto msan-dbg \"$@\"",                "newest MSAN debug",      "newest MSAN debug build"},
  {"navigate", "om",   "omnium_goto msan-opt \"$@\"",                "newest MSAN optimised",  "newest MSAN optimised build"},
  {"navigate", "ct",   "cd /test",                                    "the builds",             "the builds"},
  {"navigate", "da",   "cd \"$(\"$OMNIUM\" config DATA_DIR | cut -d= -f2)\"", "the workdirs",  "the workdirs"},
  {"navigate", "j",    "omnium_jump \"$@\"",                          "a workdir, or a trial",  "j <workdir> or j <workdir> <trial>"},
  {"basedir",  "anc",  "\"$OMNIUM\" fresh \"$@\"",                    "fresh server, no client","fresh server here, no client"},
  {"basedir",  "a",    "\"$OMNIUM\" fresh --cl \"$@\"",               "fresh server + client",  "fresh server, then a client"},
  {"basedir",  "cl",   "\"$OMNIUM\" cl \"$@\"",                       "a client on it",         "client on the server here"},
  {"basedir",  "rp",   "\"$OMNIUM\" replay in.sql \"$@\"",            "run in.sql",             "run in.sql, output to mysql.out (the framework's ./test)"},
  {"basedir",  "ins",  "${EDITOR:-vi} in.sql",                        "edit in.sql",            "edit in.sql (bash keeps the name in for itself)"},
  {"basedir",  "m",    "\"$OMNIUM\" myver \"$@\"",                    "version banner",         "version banner"},
  {"basedir",  "tl",   "tail -100 log/master.err",                    "tail the error log",     "tail the error log"},
  {"basedir",  "vl",   "${EDITOR:-vi} log/master.err",                "open the error log",     "open the error log"},
  {"bug",      "t",    "\"$OMNIUM\" t \"$@\"",                        "the UniqueID here",      "the UniqueID here"},
  {"bug",      "tt",   "\"$OMNIUM\" tt \"$@\"",                       "UniqueID + is it known", "the UniqueID plus the known-bug verdict and Jira links"},
  {"bug",      "stack","\"$OMNIUM\" stack \"$@\"",                    "the stack or SAN block", "the stack or sanitizer block"},
  {"bug",      "sts",  "\"$OMNIUM\" sts \"$@\"",                      "the sanitizer UniqueID", "the sanitizer UniqueID"},
  {"bug",      "els",  "\"$OMNIUM\" els \"$@\"",                      "scan an error log",      "error_log_scan"},
  {"workdir",  "pr",   "omnium_pr \"$@\"",                            "this run's trials",      "this run: trials, outcomes, saved"},
  {"workdir",  "i",    "\"$OMNIUM\" trial \"$@\"",                    "one trial in detail",    "one trial in detail"},
  {"workdir",  "vt",   "omnium_vt \"$@\"",                            "a trial's error log",    "open a trial's error log"},
  {"workdir",  "sr",   "\"$OMNIUM\" reduce \"$@\"",                   "reduce a trial",         "reduce a trial"},
  {"workdir",  "rplan","\"$OMNIUM\" reduce --plan \"$@\"",            "what a reduction uses",  "what a reduction would use"},
  {"workdir",  "mx",   "\"$OMNIUM\" matrix \"$@\"",                   "the detection matrix",   "the Bug Detection Matrix of a testcase"},
  {"workdir",  "rep",  "\"$OMNIUM\" report \"$@\"",                   "the bug report",         "the bug report of a reduced trial"},
  {"workdir",  "mtr",  "\"$OMNIUM\" mtr \"$@\"",                      "the MTR testcase",       "the MTR form of a testcase"},
  {"workdir",  "dt",   "omnium_dt \"$@\"",                            "delete a trial",         "delete a trial (asks when it holds a core)"},
  {"knowledge","kb",   "\"$OMNIUM\" kb \"$@\"",                       "known bugs",             "known_bugs.strings"},
  {"knowledge","kba",  "\"$OMNIUM\" kba \"$@\"",                      "known bugs, sanitizer",  "known_bugs.strings.SAN"},
  {"knowledge","kbs",  "\"$OMNIUM\" kbs \"$@\"",                      "search known bugs",      "search the known bugs"},
  {"knowledge","kbsa", "\"$OMNIUM\" kbsa \"$@\"",                     "search, sanitizer",      "search the sanitizer known bugs"},
  {"knowledge","eb",   "\"$OMNIUM\" eb \"$@\"",                       "the SQL of a filed bug", "the SQL of a filed bug"},
  {"omnium",   "run",  "\"$OMNIUM\" run \"$@\"",                      "start trials",           "start trials"},
  {"omnium",   "build","\"$OMNIUM\" build \"$@\"",                    "build a version",        "build a version or branch"},
  {"omnium",   "st",   "\"$OMNIUM\" status \"$@\"",                   "every run on the box",   "every run on the box"},
  {"omnium",   "tui",  "\"$OMNIUM\" tui \"$@\"",                      "the live view",          "the live view of a run"},
  {"omnium",   "ib",   "\"$OMNIUM\" inbox \"$@\"",                    "the human queue",        "the human queue"},
  {"omnium",   "ok",   "omnium_ok \"$@\"",                            "approve an inbox item",  "approve an inbox item (touch <item>.ok)"},
  {"omnium",   "builds","\"$OMNIUM\" builds \"$@\"",                  "the build registry",     "the build registry"},
  {"screen",   "s",    "omnium_screen \"$@\"",                        "attach, or list them",   "attach a screen, or list them"},
  {"screen",   "sn",   "screen -S \"${1:-$(basename $PWD)}\"",        "a new screen",           "new screen"},
  {"screen",   "sc",   "echo \"${STY:-not in a screen}\"",            "which screen am I in",   "which screen am I in"},
};
string clip(const string& s, size_t w) {
  if (s.size() <= w) return s;
  return w > 1 ? s.substr(0, w - 1) + "~" : s.substr(0, w);
}
// how wide the box may be: what the caller said, else the terminal, else a sane default
int box_width(int want) {
  if (want > 0) return want;
  struct winsize w{};
  if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
  return 100;
}
// the shortcut list in the frame the tui draws: the group is named once, in the left gutter, and
// its shortcuts fill the columns beside it. Two columns at 80 characters, three from about 120.
string shortcuts_box(int want) {
  const int GUT = 11, NAME = 7;
  int inner = std::clamp(box_width(want) - 2, 56, 138);        // so the whole box is 58 to 140 wide
  int cols = std::clamp((inner - GUT) / 33, 1, 3);
  int cell = (inner - GUT) / cols;
  string s;
  auto rule = [&](const char* lft, const char* rgt, const string& title) {
    string ln = lft;
    int used = 0;
    string tt = clip(title, (size_t)std::max(0, inner - 4));
    if (!tt.empty()) { ln += "\xe2\x94\x80 " + tt + " "; used = (int)tt.size() + 3; }
    for (int i = used; i < inner; i++) ln += "\xe2\x94\x80";
    s += ln + rgt + "\n";
  };
  auto row = [&](const string& text) {
    string t = clip(text, (size_t)inner);
    t += string(inner - t.size(), ' ');
    s += "\xe2\x94\x82" + t + "\xe2\x94\x82\n";
  };
  rule("\xe2\x94\x8c", "\xe2\x94\x90", string("omnium ") + OMNIUM_VERSION + " cli shortcuts");
  string group, line;
  int n = 0;
  bool group_open = false;
  auto flush = [&]() { if (n) { row(line); line.clear(); n = 0; } };
  for (auto& sh : SHORTS) {
    if (group != sh.group) { flush(); group = sh.group; group_open = true; }
    if (n == 0) {
      line = group_open ? " " + group : string();
      line = clip(line, (size_t)GUT - 1);
      line += string(GUT - line.size(), ' ');
      group_open = false;
    }
    string c = clip(sh.name, (size_t)NAME - 1);
    c += string(NAME - c.size(), ' ');
    c += clip(sh.brief, (size_t)(cell - NAME));
    if ((int)c.size() < cell) c += string(cell - c.size(), ' ');
    line += c;
    if (++n == cols) flush();
  }
  flush();
  rule("\xe2\x94\x9c", "\xe2\x94\xa4", "");
  row(" h <name> for one in full. Any omnium verb works too.");
  row(" The framework aliases are off in here. Ctrl+D leaves.");
  rule("\xe2\x94\x94", "\xe2\x94\x98", "");
  return s;
}
// h <name>: that one in full, else every shortcut whose name or text holds the string
int print_shortcut(const string& q) {
  auto one = [](const Short& sh) {
    printf("  %-7s %s\n", sh.name, sh.help);
    printf("  %-7s runs: %s\n", "", replace_all(sh.body, "\"$OMNIUM\"", "omnium").c_str());
  };
  for (auto& sh : SHORTS) if (q == sh.name) { one(sh); return 0; }
  bool any = false;
  for (auto& sh : SHORTS)
    if (lower(string(sh.name) + " " + sh.group + " " + sh.brief + " " + sh.help).find(lower(q)) != string::npos) { one(sh); any = true; }
  if (!any) { printf("no shortcut %s; h lists them all\n", q.c_str()); return 1; }
  return 0;
}
string rcfile_text(const string& exe) {
  string s;
  s += "# omnium cli: written by omnium cli, sourced by the bash it starts\n";
  s += "[ -r /etc/bash.bashrc ] && . /etc/bash.bashrc\n[ -r ~/.bashrc ] && . ~/.bashrc\n";
  // the shell keeps the environment of the bashrc; the aliases go, or a framework alias of the same
  // name would win over the shortcut of the same name (and break its definition)
  s += "unalias -a\n";
  s += "OMNIUM=" + sh_quote(exe) + "\n";
  s += "export HISTFILE=\"$HOME/.omnium_history\"\nexport HISTSIZE=10000\nexport HISTFILESIZE=20000\nshopt -s histappend\n";
  // context prompt: basedir, workdir, trial
  s += R"BASH(omnium_ctx() {
  local d="$PWD"
  if [ -x "$d/bin/mariadbd" ] || [ -x "$d/bin/mysqld" ]; then "$OMNIUM" myver --short 2>/dev/null || basename "$d"; return; fi
  if [ -f "$d/MYBUG" ]; then echo "$(basename $(dirname $d)) tr $(basename $d)"; return; fi
  if [ -f "$d/status.txt" ] || [ -f "$d/pquery-run.log" ]; then basename "$d"; return; fi
  echo omnium
}
PS1='\[\033[38;5;39m\][$(omnium_ctx)]\[\033[0m\]\$ '
omnium_goto() {
  local d
  d="$("$OMNIUM" builds newest "$@" 2>/dev/null)" || { echo "no build for: $*"; return 1; }
  [ -n "$d" ] || { echo "no build for: $*"; return 1; }
  cd "$d" || return 1
  "$OMNIUM" myver --short 2>/dev/null
}
omnium_jump() {
  local data w
  data="$("$OMNIUM" config DATA_DIR | cut -d= -f2)"
  w="$1"; [ -d "$data/$w" ] || w="O$1"
  [ -d "$data/$w" ] || { echo "no workdir $1"; return 1; }
  cd "$data/$w" || return 1
  [ -n "$2" ] && cd "$2"
  return 0
}
omnium_vt() { ${EDITOR:-vi} "${1:-.}/log/master.err"; }
omnium_pr() {
  local d="$PWD"
  [ $# -gt 0 ] && { "$OMNIUM" status "$@"; return; }
  [ -f "$d/MYBUG" ] && d="$(dirname "$d")"
  "$OMNIUM" status "$d"
}
omnium_dt() {
  local t
  for t in "$@"; do
    if [ -s "$t/core" ] || ls "$t"/*core* >/dev/null 2>&1; then
      read -r -p "trial $t holds a core; delete it? [y/N] " a
      [ "$a" = y ] || continue
    fi
    rm -rf -- "$t" && echo "deleted $t"
  done
}
omnium_ok() {
  local q i
  q="$("$OMNIUM" config TEST_DIR | cut -d= -f2)/omnium/HUMAN-queue"
  for i in "$@"; do i="${i%.report}"; touch "$q/$i.ok" && echo "approved $i (omnium inbox --process files it)"; done
}
omnium_screen() { if [ $# -eq 0 ]; then screen -ls; else screen -x "$1" || screen -r "$1"; fi; }
)BASH";
  // the shortcuts
  for (auto& sh : SHORTS) {
    s += string(sh.name) + "() { " + sh.body + "; }\n";
  }
  // h: the box, or one shortcut. The binary draws it, so the width follows the terminal.
  s += R"BASH(h() {
  if [ $# -eq 0 ]; then "$OMNIUM" cli --shortcuts --width "${COLUMNS:-0}"; else "$OMNIUM" cli --shortcut "$1"; fi
}
_omnium_complete() {
  local cur prev
  cur="${COMP_WORDS[COMP_CWORD]}"
  COMPREPLY=($(compgen -W "$("$OMNIUM" help 2>/dev/null | awk '/^  [a-z]/ {print $1}' | tr '\n' ' ')" -- "$cur"))
}
complete -F _omnium_complete omnium
h
)BASH";
  return s;
}
}  // namespace

// omnium cli [--rcfile-only] [--shortcuts [--width N]] [--shortcut NAME]
int cmd_cli(const Args& a) {
  bool dump = false, box = false;
  int width = 0;
  string one;
  for (size_t i = 0; i < a.size(); i++) {
    const string& s = a[i];
    auto val = [&](const string& name) -> string { if (i + 1 >= a.size()) { fprintf(stderr, "%s needs a value\n", name.c_str()); exit(2); } return a[++i]; };
    if (s == "--rcfile-only") dump = true;
    else if (s == "--shortcuts") box = true;
    else if (s == "--width") width = (int)to_long(val(s), 0);
    else if (s == "--shortcut") one = val(s);
    else { fprintf(stderr, "usage: omnium cli [--rcfile-only] [--shortcuts [--width N]] [--shortcut NAME]\n"); return 2; }
  }
  if (!one.empty()) return print_shortcut(one);
  if (box) { fputs(shortcuts_box(width).c_str(), stdout); return 0; }
  string exe = self_exe();
  if (exe.empty()) exe = "omnium";
  string rc = rcfile_text(exe);
  if (dump) { fputs(rc.c_str(), stdout); return 0; }
  string path = home_dir() + "/.omnium_cli_rc";
  if (!write_file(path, rc)) { fprintf(stderr, "omnium cli: cannot write %s\n", path.c_str()); return 1; }
  vector<char*> argv;
  string bash = "/bin/bash";
  argv.push_back((char*)bash.c_str());
  string f1 = "--rcfile", f2 = path, f3 = "-i";
  argv.push_back((char*)f1.c_str());
  argv.push_back((char*)f2.c_str());
  argv.push_back((char*)f3.c_str());
  argv.push_back(nullptr);
  setenv("OMNIUM_CLI", "1", 1);
  execv(bash.c_str(), argv.data());
  fprintf(stderr, "omnium cli: cannot start %s\n", bash.c_str());
  return 1;
}
