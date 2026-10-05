// Stand-in for <sys/auxv.h> on MSYS2, which has no auxiliary vector. build.sh puts src/msys2 on the
// include path of the embedded generator, revgen and reducer on Windows only. Their seeding of the
// random generator is all they take from Linux that MSYS2 lacks: they ask getauxval(AT_RANDOM) for
// the 16 random bytes the kernel hands a new process and treat 0 as "none", and they call gettid().
// The other sources of the seed (getrandom, the clocks, the cycle counter, the pid) stay.
#ifndef OMNIUM_MSYS2_SYS_AUXV_H
#define OMNIUM_MSYS2_SYS_AUXV_H

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#define AT_RANDOM 25

static inline unsigned long getauxval(unsigned long) { return 0; }

// a macro, not a function, so a later MSYS2 that declares gettid in <unistd.h> (included above)
// is no clash; the thread id only has to tell the threads of one process apart
#define gettid() ((pid_t)(uintptr_t)pthread_self())

#endif
