# 05. Memory, crash dumps, debuggers and root cause analysis

## What this course covers

This course is about what happens when a C or C++ program goes wrong at runtime and how you
find out why. It starts with how a process is laid out in memory, moves through the stack,
the heap and the catalogue of memory bugs, then teaches the tools: gdb and core dumps on Linux,
WinDbg and crash dumps on Windows, Valgrind and the sanitizers. It ends with the human side of
the job: triaging a production incident at 3 AM and writing a root cause analysis (RCA) that a
bank's engineering leadership will accept.

You already debug Windows crash dumps with WinDbg at a working level. This course takes that to
expert level (heap internals, PageHeap, Application Verifier, reading a dump without `!analyze`)
and adds the Linux side from zero: gdb, core dumps, Valgrind, AddressSanitizer, ThreadSanitizer.
Every Linux example targets `g++ -std=c++17` on WSL Ubuntu 24.04 (GCC 13).

## Why it is on the resume

Core-banking software runs for months without restart, handles money, and is supported under
SLAs. A crash is a client incident with a clock on it. The engineer who can take a dump, find the
faulting instruction, trace it back to the line that corrupted the heap three hours earlier, fix
it, and write a document that explains all of that to a non-engineer is the person a vendor keeps.
Interviewers for senior C, C++ roles probe this hard: "What does `free(): invalid pointer` mean?",
"How do you debug a crash you cannot reproduce?", "Walk me through a core dump."

## Prerequisites

- Solid C (pointers, malloc/free, structs). You have this.
- Beginner C++ (classes, `new`/`delete`, `std::vector`, `std::string`). Enough to read the examples.
- Module 10 assumes you have done at least the mutex and condition variable modules of
  `04_Concurrency`.
- WSL Ubuntu 24.04 with: `sudo apt install build-essential gdb valgrind heaptrack binutils elfutils`.
  Optional: `clang` (for libFuzzer in module 13), `systemd-coredump`.
- Windows: WinDbg (from the Microsoft Store, "WinDbg" / formerly WinDbg Preview), Sysinternals
  ProcDump and VMMap, Windows SDK Debugging Tools for `gflags.exe`, `umdh.exe`, `appverif.exe`.

## Modules

| #  | File | Topic | Hours |
|----|------|-------|-------|
| 01 | `01_process_memory_layout.md` | Virtual memory, sections, heap, stack, mmap, ASLR, `/proc/<pid>/maps`, VMMap | 3 |
| 02 | `02_stack_frames_and_calling_conventions.md` | Frames, return addresses, SysV and Win64 ABI, unwinding, DWARF and PDB | 3 |
| 03 | `03_the_heap_malloc_internals.md` | glibc ptmalloc chunks, bins, arenas; Windows NT heap and LFH; why corruption shows up late | 4 |
| 04 | `04_the_catalogue_of_memory_bugs.md` | Every memory bug class with a minimal reproduction and production symptom | 3 |
| 05 | `05_gdb_from_zero_to_fluent.md` | gdb: breakpoints, stepping, backtraces, watchpoints, threads, TUI, attach, pretty printers | 4 |
| 06 | `06_core_dumps_on_linux.md` | ulimit, core_pattern, coredumpctl, loading a core, reading a crash cold, full worked example | 4 |
| 07 | `07_windbg_and_windows_crash_dumps.md` | Dump capture, symbols, `!analyze -v`, stacks, `!heap`, `!locks`, exception codes, PageHeap, AppVerifier, gdb mapping | 4 |
| 08 | `08_valgrind_and_sanitizers.md` | Memcheck, Helgrind, Cachegrind; ASan, LSan, UBSan, TSan: build, read, choose | 4 |
| 09 | `09_leaks_and_memory_growth.md` | Leak vs growth vs fragmentation, heaptrack, massif, UMDH, `!heap -s`, hunting leaks in a long-running server | 3 |
| 10 | `10_concurrency_crashes_and_heisenbugs.md` | Race crash signatures, deadlock dumps, lying stack traces, stress reproduction, TSan, low-impact diagnostics | 3 |
| 11 | `11_production_incident_triage.md` | The first 15 minutes, severity, client communication, logging by design, safe restarts, postmortems | 3 |
| 12 | `12_writing_a_formal_rca.md` | RCA structure, trigger vs root cause, 5-whys, tone for a bank, two complete example RCAs | 3 |
| 13 | `13_defensive_design_to_prevent_the_bugs.md` | RAII, bounds checks, assertions, fuzzing, static analysis, hardening flags, review checklist | 3 |

Total: about 44 hours of study plus exercises. Done in parallel with other courses this is 4 to 5
weeks at 10 hours a week.

Supporting files:

- `code/` - deliberately broken programs (null deref, stack overflow, heap overflow,
  use-after-free, double free, uninitialised read, leak, data race, deadlock) plus the worked
  core-dump example, a crash handler, and fixed versions. `code/build_debug.sh` builds every
  variant (debug, optimised, ASan, TSan, UBSan) and runs Valgrind. `code/gdb_cheatsheet.md` is the
  one-page gdb reference with WinDbg equivalents. `code/gdbinit.sample` is a starter `.gdbinit`.
- `EXERCISES.md` - every exercise in one place with difficulty and hints.
- `INTERVIEW_QA.md` - 36 interview questions with model answers, basic to senior.

## Suggested order

Do 01 to 04 first; they are theory and you need them to interpret anything a debugger shows you.
Then 05 and 06 (gdb and cores) on WSL with the programs in `code/`. Then 07 to consolidate your
WinDbg knowledge against the gdb vocabulary you just learned. Then 08 and 09 (tools). Then 10
after you have done the concurrency course modules on mutexes and condition variables. Finish
with 11 to 13, which are about process and prevention and are best read with a real incident from
your own work in mind.

## You are done when you can...

- [ ] Explain what a pointer value means in terms of page tables, and why dereferencing `0x10`
      faults but dereferencing a freed heap block usually does not.
- [ ] Draw an x86-64 stack frame from memory and explain how gdb or WinDbg walks it, with and
      without frame pointers.
- [ ] Explain the glibc chunk header, tcache, fastbins and why `free(): invalid next size` means
      "somebody wrote past the end of the previous allocation, possibly hours ago".
- [ ] Take a core dump you have never seen, with the matching binary, and within 15 minutes state
      the signal, the faulting address, the faulting instruction, the source line, and whether it
      is a null dereference, a wild pointer, a stack overflow or heap corruption.
- [ ] Do the same with a Windows minidump in WinDbg without relying on `!analyze -v` alone.
- [ ] Choose between Valgrind, ASan, TSan and UBSan for a given situation and explain the cost of
      each.
- [ ] Distinguish a leak from legitimate growth from fragmentation using RSS data and a heap
      profiler, on Linux and Windows.
- [ ] Read a deadlock dump: identify every waiting thread, the lock it waits for and who holds it.
- [ ] Run the first 15 minutes of a Sev-1 incident on a banking server without losing forensic
      evidence or the client's confidence.
- [ ] Write an RCA that separates trigger from root cause, includes evidence, and proposes
      verifiable prevention.
- [ ] Set up a build with warnings-as-errors, hardening flags and sanitizer CI jobs, and explain
      what each flag protects against.

## Resume lines this course backs up

> "Crash dump & core dump analysis, memory corruption & leak investigation, production incident
> triage, root cause analysis (RCA)"

> "Debug production crash dumps and live incidents - tracing memory corruption, leaks and
> concurrency faults to root cause and delivering formal RCA documentation to banking clients
> under support SLAs"

> Tools: "WinDbg, GDB, Valgrind"

Modules 06 and 07 back up "crash dump & core dump analysis". Modules 03, 04, 08 and 09 back up
"memory corruption & leak investigation". Module 10 backs up "concurrency faults". Modules 11 and
12 back up "production incident triage" and "formal RCA documentation ... under support SLAs".
Modules 05 to 08 back up the tool names.
