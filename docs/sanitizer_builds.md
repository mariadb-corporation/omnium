# Sanitizer builds

`./build.sh` builds omnium under each sanitizer. The binaries coexist, so a run can be repeated
under a different one without a rebuild.

| Command | Binary | Covers |
|---|---|---|
| `./build.sh ubasan` | `./omnium_ubasan` | UndefinedBehaviorSanitizer plus AddressSanitizer |
| `./build.sh tsan` | `./omnium_tsan` | ThreadSanitizer |
| `./build.sh msan` | `./omnium_msan` | MemorySanitizer, needs the instrumented libc++ in `/MSAN_libs` |

Each binary takes the same verbs as `./omnium`, so `./omnium_tsan selftest` is the whole check list
under ThreadSanitizer.

## What each one is good for

UBSAN+ASAN and TSAN cover omnium end to end and are worth running on any change. A report stops the
run, so the exit code is the gate.

MSAN is different on this box. It reports every read of memory it did not watch being written, and
omnium links against the system OpenSSL, curl, GnuTLS, pcre2 and the MariaDB client library, none of
which is instrumented. A full selftest under MSAN raises over 300,000 reports, all of them inside
`libcrypto`, `libcurl` or `libssl`. MSAN is therefore a triage tool here, not a gate: build it with
the recover flag it already carries, run with `MSAN_OPTIONS=halt_on_error=0:log_path=<file>`, and
read the reports whose top frame names a file under `src/`. Instrumented builds of those libraries
would remove the noise; that is a box-wide change, not a repo one.

## Where the sanitizer sees a library boundary

`src/detect.cpp` calls pcre2 and reads what pcre2 wrote, and `src/mail.cpp` reads the DNS answer
the resolver wrote. MSAN cannot see those writes, so each value omnium takes out of such a call is
marked written at the boundary with `OMNIUM_LIB_WROTE`. The macro compiles to nothing without
MemorySanitizer. It marks only the bytes the library reports it filled, so a value omnium itself
leaves unset is still caught.

## The embedded objects

The generator, revgen and the reducer are compiled into the binary from the mariadb-qa checkout, and
a sanitizer mode instruments them too. Afterwards all their symbols but the entry point are made
local, so nothing clashes with omnium's own. The entry point keeps its C++ name (`nm` shows it as
`T`). The sanitizer runtime's symbols stay global through that step as well: making them local
leaves a common symbol that lld will not link. That needs `llvm-objcopy`, so a sanitizer build stops
with a clear message when only GNU `objcopy` is on the box.

The generator is one 22 MB translation unit and its instrumented compile can take over an hour.
`NO_GENERATOR=1` leaves it out, which gives a sanitizer binary covering every file under `src/` in
about two minutes.

On a box that runs the mariadb-qa memory watchdog, the watchdog kills a process after 20 minutes
when its command line holds `mariadb` and its name is not on the list in `process_filter.source`.
The shell that ninja runs this compile in is such a process, so the build ends with `FAILED` and no
message. `NO_GENERATOR=1` avoids that, and so does a pause of the watchdog for the build.

## The selftest keeps stderr under a sanitizer

The checks park a verb's output while it runs, so the check list stays readable. A sanitizer writes
its report to stderr, so a sanitizer build parks only stdout; otherwise a report inside a verb call
would be lost and the run would end with no output at all.
