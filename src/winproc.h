// Created by Roel Van de Paar, MariaDB
// winproc.h - the native Windows process behind a Cygwin pid
#pragma once
#include <cstdint>
#include <sys/types.h>

// A handle to the native process that stands behind a Cygwin pid, a server started by fork and exec,
// kept open so that its exit status can still be read after it has ended. 0 when there is none: not
// Windows, no such process, or a process whose image name does not contain `image` (the Cygwin
// stub that execs a native program has the pid until the program is up, and it must not be taken for
// the program).
uintptr_t winproc_open(pid_t cygpid, const char* image);

// The exit status of that process: for one that died of an exception it is the NTSTATUS (0xC00000FD a
// stack overflow, 0xC0000409 a failed stack cookie check or __fastfail, 0xC0000005 an access
// violation), which Cygwin turns into signal 11 or exit 127 and so loses. false while it still runs,
// and for a handle of 0.
bool winproc_exit_status(uintptr_t handle, uint32_t* status);

void winproc_close(uintptr_t handle);
