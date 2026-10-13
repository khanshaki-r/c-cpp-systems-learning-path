# 06_Linux_Toolchain_DevOps: interview questions and model answers

38 questions, ordered from screening level to senior level. Answer out loud, then compare. A good
answer is 2 to 6 sentences, uses the precise term, and ends with a practical consequence. Where a
module covers the topic in depth, it is named in brackets.

---

## Part 1. Building C and C++ on Linux

**1. What are the stages of building a C++ program, and what does each produce?** [02]
Preprocessing (`cpp`, part of `g++ -E`) expands `#include` and macros into a translation unit.
Compilation (`cc1plus`) turns it into assembly; assembly (`as`) turns that into a relocatable
object file (`.o`, ELF) with a symbol table and relocations. Linking (`ld`, driven by `g++`)
combines objects and libraries, resolves every undefined symbol to a definition, applies
relocations and writes an executable or shared library. `-E`, `-S` and `-c` stop after each of
the first three stages.

**2. What does the linker actually do?** [02]
It reads the symbol tables of all inputs, matches every undefined reference (`U` in `nm`) with
exactly one definition, lays out the sections into segments, assigns final addresses, and patches
each relocation with the resolved address. For static libraries it pulls in only the archive
members that resolve a currently undefined symbol, scanning left to right, which is why library
order on the command line matters with GNU ld. For shared libraries it records a `NEEDED` entry and
leaves the final binding to the dynamic loader at run time.

**3. Static library vs shared library: differences and trade-offs?** [02]
A static library (`.a`) is an archive of object files; the needed members are copied into the
executable at link time, so the binary is self-contained and has no run-time lookup cost, but every
binary carries its own copy and a security fix requires relinking everything. A shared library
(`.so`) is loaded once by `ld.so` and mapped into every process that uses it: smaller binaries,
shared memory pages, updates without relinking, at the cost of run-time resolution, version
compatibility (ABI, sonames) and "cannot open shared object file" failures in deployment.

**4. You get "undefined reference to `foo`". What are the possible causes?** [02]
Three families: the definition is not in the link at all (a missing `.cpp`, a missing `-lfoo`, or
a library listed before the object that needs it); the names do not match (C vs C++ linkage, a
missing `extern "C"`, a different signature or namespace, so the mangled names differ); or the
symbol exists but is not visible (hidden visibility, a template never instantiated, an `inline`
or `static` function defined only in another file). `nm -C` on the objects and libraries tells
you which: absent, present under another mangled name, or present but local (`t` instead of `T`).

**5. What is the One Definition Rule, and why can violations go undetected?** [02]
The ODR says every non-inline function and variable has exactly one definition in the program, and
every class, inline function or template defined in several translation units must be defined
identically (token for token, with the same meaning). Duplicate strong symbols give "multiple
definition" link errors, but differing class definitions with the same name (two `struct Config`
in two `.cpp` files, or a header compiled with different macros) produce no error, because the
linker only sees symbol names, not layouts. The program then reads the wrong offsets at run time;
`-flto -Wodr` and ASan's ODR checks can catch some cases.

**6. What is `-fPIC` and why do shared libraries need it?** [02]
Position-independent code addresses its global data and external functions through the GOT and PLT
(and PC-relative addressing) instead of absolute addresses, so the same code pages work wherever the
loader maps the library and can be shared between processes. Without it, the loader would have to
patch text relocations in the code itself, which makes the pages private per process and is
refused on x86-64 for most relocation types. Executables are PIE (position-independent) by default
on Ubuntu for ASLR.

**7. How does the dynamic loader find a shared library at run time?** [02]
`ld.so` reads the executable's `NEEDED` entries and searches, in order: `DT_RPATH` (if no
`RUNPATH`), `LD_LIBRARY_PATH`, `DT_RUNPATH`, the `/etc/ld.so.cache` built by `ldconfig`, then the
default directories `/lib` and `/usr/lib` (and their multiarch subdirectories). `ldd` shows the
result and `LD_DEBUG=libs` traces the search. In production, prefer installing to a standard path
with `ldconfig` or an `$ORIGIN`-relative rpath over `LD_LIBRARY_PATH`.

**8. What do `-O0`, `-O1`, `-O2`, `-O3`, `-Os` and `-Og` mean? Which do you ship?** [03]
`-O0` does no optimisation (fast compile, debuggable, slow); `-O1` basic optimisations; `-O2` nearly
all optimisations that do not trade size for speed, the usual release level; `-O3` adds aggressive
inlining, loop transformations and more vectorisation, which may or may not be faster; `-Os`
optimises for size; `-Og` optimises without hurting debugging. Ship `-O2 -g` (CMake's
RelWithDebInfo) with symbols archived, and adopt `-O3` or LTO only when a benchmark shows a gain.

**9. Why build with `-g` in release builds?** [03, 07]
Debug info does not change the generated code; it only adds `.debug_*` sections that are never
loaded at run time. Shipping a stripped binary and archiving the unstripped one (or a split
`.debug` file matched by build-id) lets you symbolise production cores and `perf` profiles with
exact file and line numbers. Without it, a release-build crash gives you raw addresses.

**10. What are sanitizers, and which ones would you run in CI?** [03, 05]
Compiler instrumentation that detects bugs at run time: AddressSanitizer (out-of-bounds,
use-after-free, double free, leaks via LeakSanitizer; about 2x slowdown, typical), UndefinedBehaviorSanitizer
(signed overflow, invalid shifts, misaligned access, null dereference), ThreadSanitizer (data races,
lock-order inversions; 5 to 15x slowdown, typical) and MemorySanitizer (uninitialised reads,
Clang only). ASan and TSan cannot be combined, so CI runs an ASan+UBSan job and a separate TSan
job over the whole test suite, with `halt_on_error` so findings fail the build.

**11. What does `-Wall -Wextra -Werror` give you, and what is the risk of `-Werror`?** [03]
`-Wall` and `-Wextra` enable the warnings that usually indicate bugs (unused variables, sign
comparisons, missing returns, implicit fallthrough); `-Werror` makes the build fail on any of them,
so warnings cannot accumulate. The risk is that a new compiler version adds warnings and breaks the
build for packagers and users; the standard answer is `-Werror` on in CI and for developers, and
an option to turn it off (tcpkv's `TCPKV_WERROR`, the `no-werror` preset).

## Part 2. CMake and testing

**12. What do PUBLIC, PRIVATE and INTERFACE mean in `target_link_libraries` and friends?** [04]
They define which usage requirements (include directories, compile definitions, options, linked
libraries) apply to the target itself and which propagate to targets that link to it. PRIVATE:
used to build this target only. INTERFACE: not used for this target, only for consumers (header-only
libraries). PUBLIC: both. The rule: if a dependency appears in your public headers it is PUBLIC; if
only in your `.cpp` files it is PRIVATE. Wrong visibility either leaks dependencies to every
consumer or breaks their compilation with missing headers.

**13. Why "modern CMake" with targets instead of setting global variables like `CMAKE_CXX_FLAGS` and `include_directories`?** [04]
Global settings apply to every target in the directory and below, cannot express which consumer
needs what, and do not travel with an installed package. Target-based commands attach requirements
to the target and propagate them through the dependency graph, so `target_link_libraries(app
PRIVATE tcpkv::core)` brings the right include paths, definitions, C++ standard and `-pthread`
automatically, both in the build tree and through `find_package` after installation.

**14. What are CMake presets and why use them?** [04, 09]
`CMakePresets.json` names complete configurations (generator, build directory, build type, cache
variables such as sanitizers) and matching build and test presets, checked into the repository.
`cmake --preset asan && cmake --build --preset asan && ctest --preset asan` is then the same command
for every developer, every IDE and CI. Personal tweaks go in the gitignored `CMakeUserPresets.json`.

**15. How do you structure tests for a networked server so most of them are fast and deterministic?** [05]
Keep the logic in pure, I/O-free units (the protocol parser, the store) and test them with
GoogleTest directly: fixtures for setup, parameterised tests for input tables, death tests for
aborts, gMock for dependencies behind interfaces. Run them under sanitizers. Keep a smaller set of
integration tests that start the real binary on a free port and drive it through a client,
labelled separately (`ctest -L integration`) so the unit suite stays under a few seconds.

## Part 3. Git

**16. Merge vs rebase: what is the difference and when do you use each?** [06]
Merge creates a new commit with two parents that joins the histories, preserving exactly what
happened. Rebase replays your commits one by one on top of another base, creating new commits with
new hashes and a linear history. Rebase your own unpublished feature branch onto `main` to keep it
current and clean; never rebase commits others have based work on; integrate into `main` by merge
or squash-merge according to the team's convention.

**17. You ran `git reset --hard` and lost commits. How do you get them back?** [06]
`git reflog` lists every position `HEAD` (and each branch) has had, including commits no branch
points to any more. Find the entry before the reset, then `git reset --hard HEAD@{n}` or `git branch
rescue <sha>`. Unreachable commits survive until garbage collection prunes them (reflog entries
expire after 90 days by default, 30 for unreachable ones), so recovery is almost always possible
for recent mistakes.

**18. How does `git bisect` work, and how do you automate it?** [06]
It binary-searches the history between a known good and a known bad commit: Git checks out the
midpoint, you mark it good or bad, and it halves the range, finding the first bad commit in about
log2(N) steps. `git bisect run ./script.sh` automates it: exit 0 means good, 1 to 124 or 126 to 127
bad, 125 skip (does not build). The script should build in a separate directory and use `timeout`
so a hang counts as bad.

**19. What is a Git commit, physically?** [06]
An object in `.git/objects` addressed by the hash of its content: it records a tree (a snapshot of
the directory, itself made of trees and blobs), zero or more parent commit hashes, author,
committer and message. Branches and tags are just references to commit hashes. Because the hash
covers the parents, changing any old commit changes every hash after it, which is why rebasing
rewrites history.

## Part 4. Debugging, profiling and scripting

**20. What do `strace` and `perf` each tell you, and when do you reach for which?** [07]
`strace` intercepts system calls with ptrace, showing every syscall, its arguments, result and
(with `-T`) time spent: the tool for "what is it doing with files, sockets and processes" and
"why does it fail with EACCES or hang in `futex`". It slows the target heavily. `perf` samples
the CPU (stack traces at a frequency) with low overhead and shows where CPU time goes, per
function, as a report or flame graph. Hang or error: strace first. Slow while busy: perf. Slow while
idle (waiting): strace `-c`, off-CPU analysis, or gdb stacks.

**21. A process is at 100 % CPU in production. What do you do?** [07]
Find the thread: `top -H -p PID` (or `ps -L`). Sample it: `perf top -p PID` or `perf record -g -p
PID -- sleep 30` then `perf report`, which names the hot functions. If it is a spin or an infinite
loop, `gdb -p PID` (or a batch stack dump) on that thread shows where. Capture evidence before
restarting, compare with a known-good build, and reproduce under the same load with a profiler
attached.

**22. What does `set -euo pipefail` do in a bash script?** [10]
`-e` exits on the first failing command, `-u` makes expanding an unset variable an error, and
`-o pipefail` makes a pipeline fail if any stage fails instead of only the last. Together they turn
silent failures into immediate exits, for example `ctest | tee log` passing after failed tests, or
`rm -r "$DIR/"` running with an empty variable. It has holes (commands in `if`, `||`, `&&`, `local
x=$(cmd)`), so critical steps still need explicit checks, and a `trap cleanup EXIT` handles cleanup.

## Part 5. Linux systems programming

**23. What is the difference between a system call and a library call?** [11]
A system call switches the CPU to kernel mode to request a service only the kernel can provide
(I/O, memory mapping, process creation); a library call runs in user space and may make zero or
several syscalls. `printf` buffers and calls `write` occasionally; `malloc` serves most requests
from its arenas and calls `brk` or `mmap` only to grow. Failures are reported through a return
value plus `errno`, which must be read immediately after the failing call.

**24. What happens when you send SIGTERM to a process, and how should a server handle it?** [11, 08]
The kernel marks the signal pending; on the next return to user mode it runs the installed handler
or applies the default action (terminate). A server should install a handler with `sigaction` that
only sets a `volatile sig_atomic_t` flag (or writes to a self-pipe, or use `signalfd`), let the
main loop notice it, stop accepting, drain in-flight requests within a bounded time and exit 0.
systemd and Docker send SIGTERM and follow with SIGKILL after a timeout (`TimeoutStopSec`, 10 s
for `docker stop`), so the drain must finish within that window.

**25. Why can you not call `printf` or `malloc` in a signal handler?** [11]
The handler can interrupt the program in the middle of any function, including `malloc` or
`printf` while they hold internal locks or have data structures half-updated. Calling them again
from the handler can deadlock on the same lock or corrupt the heap. Only async-signal-safe
functions listed in `signal-safety(7)` (such as `write`, `_exit`, `sigaction`) are allowed, and the
handler must save and restore `errno`.

**26. Explain `fork` and `exec` and the danger of `fork` in a multithreaded program.** [11]
`fork` creates a child that is a copy of the parent with copy-on-write memory and duplicated file
descriptors; `exec` replaces the process image with a new program. The split lets the child set up
redirections and privileges before `exec`. In a multithreaded parent, only the forking thread
exists in the child, and locks held by other threads at that moment stay locked forever, so the
child may deadlock in `malloc` or a logger; between `fork` and `exec` it must call only
async-signal-safe functions, or the program should use `posix_spawn`.

**27. Does `write()` guarantee the data is on disk? How do you make a write durable?** [11]
No. `write` returns once the data is in the kernel's page cache; writeback happens later. Durability
requires `fsync` (or `fdatasync`) on the file, plus `fsync` on the directory after creating or
renaming a file. For atomic replacement: write a temp file, `fsync` it, `rename` over the target,
`fsync` the directory. Treat an `fsync` failure as fatal for that data: after a writeback error the
kernel may have discarded the dirty pages, so retrying can report success falsely.

**28. Why is epoll more scalable than `select`/`poll` for many connections?** [11]
`select` and `poll` pass the full list of fds on every call and the kernel scans all of them, so
each wait costs O(n). epoll registers interest once in a kernel object; readiness callbacks put fds
on a ready list, and `epoll_wait` returns only those, so the cost is proportional to activity. It
needs non-blocking fds; level-triggered mode re-reports while data remains, edge-triggered mode
reports transitions and requires reading until `EAGAIN`.

**29. A service keeps disappearing with exit code 137 and nothing in its logs. What is happening?** [08, 11]
137 is 128 + 9: SIGKILL, which cannot be caught, so the program never logs. The usual sender is the
OOM killer, system-wide or because the service's cgroup hit its memory limit (`MemoryMax=`, Docker
`--memory`), or a stop timeout escalating from SIGTERM. Confirm with `journalctl -k | grep -i oom`,
`docker inspect` (`OOMKilled: true`) or `systemctl status`; then find the memory growth with a heap
profiler instead of raising the limit.

**30. How do you run a C++ server as a production service on Linux with systemd?** [11]
Write a unit with `ExecStart` running the binary in the foreground (`Type=exec` or `notify`), a
dedicated unprivileged `User=`, `Restart=on-failure` with `RestartSec` and a start-limit burst,
`TimeoutStopSec` matched to the graceful drain, `LimitNOFILE` sized for connections, `MemoryMax`
for a cgroup cap, and sandboxing (`NoNewPrivileges`, `ProtectSystem=strict`, `PrivateTmp`). Logs go
to stdout/stderr and are read with `journalctl -u`. `systemctl daemon-reload` after edits,
`enable --now` to start it and at boot.

## Part 6. Docker and CI/CD

**31. What is a Docker image layer, and how does layer caching affect your Dockerfile?** [08]
Each filesystem-changing instruction (`RUN`, `COPY`, `ADD`) produces a read-only layer, a tarball
of the changes, stacked with an overlay filesystem; the container adds one writable layer on top.
The builder reuses a cached layer as long as the instruction and its inputs (for `COPY`, the file
checksums) are unchanged, and rebuilds every layer after the first change. So order instructions
from least to most frequently changing: system packages, then build files and dependency fetches,
then sources. Files deleted in a later layer still occupy space in the earlier one.

**32. What is a multi-stage build and why use it for C++?** [08]
A Dockerfile with several `FROM` stages, where later stages copy only selected artefacts from
earlier ones with `COPY --from=`. For C++, the build stage has the compiler, CMake and headers and
runs the tests; the runtime stage starts from a small base, installs only runtime libraries and
copies the stripped binary. The result is an image without compilers or sources: smaller, faster
to pull, smaller attack surface, and the tests ran in the exact environment of the shipped binary.

**33. Why does PID 1 matter inside a container?** [08, 11]
The process with PID 1 in a namespace gets no default signal actions from the kernel: a SIGTERM
without an installed handler is ignored, so `docker stop` waits 10 seconds and sends SIGKILL. PID 1
also inherits orphaned processes and must reap them, or zombies accumulate. Either the server
installs SIGTERM handling and never forks, or a tiny init such as `tini` (or `docker run --init`)
runs as PID 1, forwards signals and reaps children.

**34. How would you design a CI pipeline for a C++ service?** [09]
Trigger on every PR and on `main` and tags; cancel superseded runs. A matrix of compilers (GCC,
Clang) and build types with `-Werror`, running the full test suite with JUnit output; separate
sanitizer jobs (ASan+UBSan, TSan) using the same CMake presets developers use; formatting and
clang-tidy checks; ccache and dependency caches keyed by content hash. Then build the Docker image,
smoke-test that exact image, and push it tagged with the commit SHA only from `main` or tags.
Archive debug symbols per build, require all checks via branch protection, keep PR feedback under
about ten minutes and move slow suites (Valgrind, long fuzzing) to nightly jobs.

**35. How do you handle secrets in CI and in containers?** [08, 09]
Store them in the CI system's encrypted secrets (repository, organisation, or a vault such as Key
Vault), expose them only to the steps that need them, and rely on them being masked and unavailable
to fork PRs. Prefer short-lived credentials: the per-run `GITHUB_TOKEN`, OIDC federation to cloud
providers. Never put secrets in the repository, in Dockerfile `ARG`/`ENV` (they persist in image
history) or in compose files; inject them at run time through environment files, Docker secrets or
the orchestrator.

## Part 7. Senior-level and cross-platform

**36. Compare the Windows and Linux toolchains for C++ in two minutes.** [12]
MSVC `cl`/`link` with MSBuild versus GCC or Clang with Make or Ninja, both drivable by CMake.
`/W4 /WX` maps to `-Wall -Wextra -Werror`; `/O2 /Zi` to `-O2 -g`. Windows has PE images, `.lib` and
`.dll` with import libraries and nothing exported by default, separate PDBs matched by GUID, and a
per-module CRT choice (`/MD` vs `/MT`); Linux has ELF, `.a` and `.so` with everything exported by
default, DWARF matched by build-id, and one libc per process. Debugging is WinDbg or Visual Studio
on dumps versus GDB on cores; tracing is ETW/Process Monitor versus perf/strace; services are SCM
versus systemd; Windows commits memory up front while Linux overcommits and has an OOM killer.

**37. A release build crashes in production but the debug build never does. How do you approach it?** [03, 05, 07]
Optimisation exposes undefined behaviour the debug build hides (uninitialised variables, signed
overflow, out-of-bounds, data races, strict-aliasing violations) or changes timing. Get the core
dump and the matching unstripped binary, read the stack and registers in GDB; rebuild the release
configuration with `-g` and run the tests and a load test under ASan+UBSan and TSan; compile with
more warnings (`-Wall -Wextra -Wconversion`) and with both GCC and Clang. If it reproduces only in
one place, `git bisect` with the release preset narrows it to a commit.

**38. Where do the "it works on my machine" failures come from, and how does your setup prevent them?** [04, 08, 09]
From differences in compiler version and flags, library versions, environment variables, file
system behaviour (case, line endings), and undeclared dependencies installed on the developer
machine. The prevention is to define everything in the repository: CMake presets for flags,
pinned dependencies (FetchContent tags, vcpkg baselines), `.gitattributes` for line endings, a
Docker build stage that is the reference environment, and CI that builds from a clean checkout on
every commit and tests the exact image that ships.
