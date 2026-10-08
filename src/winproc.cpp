// Created by Roel Van de Paar, MariaDB
// winproc.cpp - the native Windows process behind a Cygwin pid. A Windows server dies of an
// exception with an NTSTATUS as its exit status, and the Cygwin runtime reports that to the parent as
// signal 11 (an access violation, a stack overflow) or as exit status 127 (everything it has no signal
// for, such as the 0xC0000409 of a failed /GS stack cookie check). The status itself is only to be
// had from the Windows process, through a handle opened while it runs. Anywhere else this does nothing.
#include "winproc.h"

#if defined(__MSYS__) || defined(__CYGWIN__)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>

uintptr_t winproc_open(pid_t cygpid, const char* image) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%ld/winpid", (long)cygpid);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  long winpid = 0;
  if (fscanf(f, "%ld", &winpid) != 1) winpid = 0;
  fclose(f);
  if (winpid <= 0) return 0;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)winpid);
  if (!h) return 0;
  char name[MAX_PATH];
  DWORD len = sizeof(name);
  if (!QueryFullProcessImageNameA(h, 0, name, &len)) { CloseHandle(h); return 0; }
  std::string have(name, len), want = image ? image : "";
  for (char& c : have) c = (char)tolower((unsigned char)c);
  for (char& c : want) c = (char)tolower((unsigned char)c);
  if (!want.empty() && have.find(want) == std::string::npos) { CloseHandle(h); return 0; }
  return (uintptr_t)h;
}

bool winproc_exit_status(uintptr_t handle, uint32_t* status) {
  if (!handle) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess((HANDLE)handle, &code) || code == STILL_ACTIVE) return false;
  if (status) *status = (uint32_t)code;
  return true;
}

void winproc_close(uintptr_t handle) {
  if (handle) CloseHandle((HANDLE)handle);
}
#else
uintptr_t winproc_open(pid_t, const char*) { return 0; }
bool winproc_exit_status(uintptr_t, uint32_t*) { return false; }
void winproc_close(uintptr_t) {}
#endif
