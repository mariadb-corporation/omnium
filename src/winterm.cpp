// Created by Roel Van de Paar, MariaDB
// winterm.cpp - what of the terminal window the user can see. A window can be dragged taller than
// the screen, and its terminal still tells the program all of its rows, so a view that fills them
// draws under the taskbar. On Windows the window can be found and measured: the rows below the work
// area of its monitor are left empty. Anywhere else every row is taken as visible.
#include "winterm.h"

#include <algorithm>

int rows_below_work_area(long client_top, long client_h, long work_bottom, int rows) {
  if (rows <= 0 || client_h <= 0) return 0;
  long below = client_top + client_h - work_bottom;
  if (below <= 0) return 0;
  long hidden = (below * rows + client_h - 1) / client_h;     // rounded up: half a row is no row
  return (int)std::min<long>(hidden, rows);
}

#if defined(__MSYS__) || defined(__CYGWIN__)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unistd.h>

#include <cstdio>

namespace {
// a number out of /proc/<pid>/<file> (ppid, winpid), 0 when there is none
long proc_number(long pid, const char* file) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%ld/%s", pid, file);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  long n = 0;
  if (fscanf(f, "%ld", &n) != 1) n = 0;
  fclose(f);
  return n;
}

struct Pick {
  DWORD pid;
  HWND best;
  long area;
};
// the biggest visible top-level window without an owner that the process has
BOOL CALLBACK pick_window(HWND h, LPARAM lp) {
  Pick* p = (Pick*)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid != p->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  RECT c;
  if (!GetClientRect(h, &c)) return TRUE;
  long area = (long)(c.right - c.left) * (c.bottom - c.top);
  if (area > p->area) { p->area = area; p->best = h; }
  return TRUE;
}
HWND window_of_pid(DWORD pid) {
  Pick p = {pid, nullptr, 0};
  EnumWindows(pick_window, (LPARAM)&p);
  return p.best;
}

// The window of the terminal this process runs in: the console window, or else the window of the
// nearest ancestor that has one. A mintty window is no console, so its children have none.
HWND find_window() {
  HWND con = GetConsoleWindow();
  if (con) {
    HWND owner = GetWindow(con, GW_OWNER);          // Windows Terminal: its pseudo console window
    if (owner) con = owner;                         // is owned by the real one
    if (IsWindowVisible(con)) return con;
  }
  long pid = (long)getppid();
  for (int up = 0; pid > 1 && up < 8; up++) {
    long winpid = proc_number(pid, "winpid");
    if (winpid > 0) {
      HWND w = window_of_pid((DWORD)winpid);
      if (w) return w;
    }
    pid = proc_number(pid, "ppid");
  }
  return nullptr;
}

int hidden_in(HWND win, int rows) {
  if (!win || !IsWindow(win) || IsIconic(win)) return 0;
  RECT c;
  POINT top = {0, 0};
  if (!GetClientRect(win, &c) || !ClientToScreen(win, &top)) return 0;
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  HMONITOR mon = MonitorFromWindow(win, MONITOR_DEFAULTTONEAREST);
  if (!mon || !GetMonitorInfoA(mon, &mi)) return 0;
  long client_h = c.bottom - c.top;
  POINT foot = {top.x + (c.right - c.left) / 2, top.y + (LONG)client_h - 1};
  HMONITOR under = MonitorFromPoint(foot, MONITOR_DEFAULTTONULL);
  if (under && under != mon) return 0;           // the bottom is on another screen: it is seen
  return rows_below_work_area(top.y, client_h, mi.rcWork.bottom, rows);
}
}  // namespace

int term_rows_hidden(int rows) {
  static HWND win = nullptr;
  static bool looked = false;
  if (!looked) { looked = true; win = find_window(); }
  return hidden_in(win, rows);
}
#else
int term_rows_hidden(int) { return 0; }
#endif
