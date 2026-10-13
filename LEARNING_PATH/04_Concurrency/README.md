# 04_Concurrency — Multithreading and concurrency, C++17 and Win32, to production depth

## What this course covers

You already run threads in production. Your application server creates Win32 threads, guards
shared tables with `CRITICAL_SECTION`, waits on Events and Mutex handles, and serves loan-origination
requests through a pooled request model. What you have not done yet is any of that in C++, and you
have never had to explain *why* it works: what a data race really is, what the compiler and the CPU
are allowed to reorder, what "acquire" and "release" mean, or how to prove a thread-pool shutdown
cannot lose a task.

This course takes you from "I use locks and it has not crashed" to "I can design, explain and debug
a concurrent server, in C++17 on Linux and in C on Win32, and I can defend every decision in a
senior interview."

Every Win32 primitive you use today is mapped to its C++17 standard-library and POSIX equivalent,
so you can describe your existing work in portable terms and write the same designs in modern C++.

## Why it is on the resume

Three resume items depend on it directly:

- "multithreaded application server handling transactional loan-origination requests"
- "thread synchronisation layer — mutexes, critical sections, wait objects and a pooled request model"
- tcpkv: "fixed thread pool ... store guarded by shared_mutex"

and two more depend on it indirectly: "in-memory caching" (a cache shared by many threads is a
concurrency problem first and a data-structure problem second) and "concurrency faults" (which you
cannot debug without a model of what the hardware is doing).

Interviewers for senior C++ roles ask about this topic in nearly every loop. The questions are
predictable: data races, mutex vs atomic, condition-variable predicate loops, memory orders,
deadlock diagnosis, thread-pool design, false sharing. This course covers every one of them to the
depth where you can derive the answer rather than recite it.

## Prerequisites

- C at a professional level (you have this).
- C++ basics: classes, RAII destructors, templates at the "I can read `std::vector<T>`" level,
  lambdas. If `[&](int x) { return x + n; }` looks foreign, do the first modules of
  02_Modern_CPP first and come back.
- WSL Ubuntu 24.04 with `g++ --version` reporting GCC 13. Every C++ file in `code/` compiles with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 file.cpp -o file
```

- For ThreadSanitizer (modules 02 and 12): add `-fsanitize=thread -g -O1`.
- Win32 `.c` examples are Windows-only. Build them with MSVC (`cl /W4 file.c`) or MinGW GCC
  (`gcc -Wall file.c`). They are reference material for module 13, not the main track.

## Modules

| #  | File | Topic | Hours |
|----|------|-------|-------|
| 01 | `01_processes_threads_and_the_os.md` | What a thread is, kernel view, pthread vs Win32 vs std::thread, create/join/detach, TLS, how many threads | 2.5 |
| 02 | `02_data_races_and_why_they_are_ub.md` | Data race definition, undefined behaviour, torn reads, compiler and CPU reordering, TSan intro | 2.5 |
| 03 | `03_mutexes_lock_guard_unique_lock.md` | std::mutex, lock_guard, unique_lock, scoped_lock, recursive and timed mutexes, CRITICAL_SECTION mapping, futex | 3 |
| 04 | `04_shared_mutex_and_readers_writers.md` | shared_mutex, shared_lock, reader-writer trade-offs, writer starvation, the tcpkv store | 2 |
| 05 | `05_condition_variables_and_wait_objects.md` | condition_variable, predicate loops, spurious and lost wakeups, Win32 Events, producer-consumer queue | 3 |
| 06 | `06_deadlocks_livelocks_starvation.md` | Four conditions, lock ordering, hierarchies, try_lock backoff, deadlocks in crash dumps, priority inversion | 2.5 |
| 07 | `07_atomics_and_the_memory_model.md` | std::atomic, CAS, memory orders, happens-before, fences, volatile, x86 vs ARM | 4 |
| 08 | `08_lock_free_basics.md` | Spinlock, SPSC ring buffer, Treiber stack and ABA, hazard pointers, when not to go lock-free | 3 |
| 09 | `09_thread_pools_and_task_queues.md` | Fixed thread pool from scratch, futures and promises, std::async pitfalls, shutdown, exceptions, backpressure | 4 |
| 10 | `10_cache_lines_false_sharing_performance.md` | MESI, false sharing, alignas(64), per-thread counters, Amdahl, steady-state benchmarking | 2.5 |
| 11 | `11_concurrency_patterns_for_servers.md` | Thread-per-connection vs pool vs event loop, actors, RCU snapshots, call_once, connection pools | 3 |
| 12 | `12_debugging_concurrency_bugs.md` | TSan, helgrind, thread-id logging, stress and sleep injection, all-thread stacks in gdb and WinDbg, bug catalogue | 3 |
| 13 | `13_win32_concurrency_reference.md` | CreateThread, CRITICAL_SECTION, SRWLOCK, CONDITION_VARIABLE, Events, Semaphores, WFMO, Interlocked, thread pool API, IOCP; side-by-side table | 2 |

Total: about 37 hours. At 4 to 5 hours a week alongside other courses, this is 8 weeks. Modules
01 to 05 are the core; do them first and in order. 07 is the hardest and the most asked about in
interviews; budget a second pass for it. 13 is a reference you can read in one sitting and return to.

## Supporting files

- `code/` — 20 complete programs. Each module names the files it uses.
- `EXERCISES.md` — every exercise from every module, with difficulty and a hint.
- `INTERVIEW_QA.md` — 36 questions from basic to senior, with model answers.

## Suggested order with the other courses

- Do 02_Modern_CPP modules on RAII and lambdas before module 03 here.
- Module 11 previews epoll and the event loop; the full treatment is in 08_Networking.
- Module 12's crash-dump reading pairs with the debugging course's WinDbg material.

## You are done when you can...

- [ ] State precisely what a data race is (two accesses, at least one write, not atomic, not ordered
      by happens-before) and explain why the standard makes it undefined behaviour rather than
      "unspecified value".
- [ ] Write a producer-consumer queue with `std::mutex` and `std::condition_variable`, and explain
      from first principles why the wait must be in a predicate loop.
- [ ] Explain acquire and release in the "message passing" model, and say what seq_cst adds that
      acquire/release does not.
- [ ] Write a fixed-size thread pool whose shutdown provably drains every submitted task, and whose
      tasks can throw without killing a worker.
- [ ] Look at the stacks of eight blocked threads in gdb or WinDbg and name the two that are
      deadlocked and the lock order that caused it.
- [ ] Explain why `std::shared_mutex` can be slower than `std::mutex` and when that happens.
- [ ] Explain false sharing with a drawing of two cache lines and say what `alignas(64)` fixes.
- [ ] Describe your Win32 synchronisation layer entirely in portable terms: CRITICAL_SECTION is a
      non-recursive-in-spirit mutex with a spin phase, SRWLOCK is a shared_mutex, an auto-reset
      Event is roughly a binary semaphore, WaitForMultipleObjects has no direct std equivalent.
- [ ] Design the locking for a master-table cache that is read by 200 threads and refreshed every
      five minutes, using an immutable snapshot and one atomic pointer swap.
- [ ] Explain the ABA problem and why a Treiber stack needs tagged pointers or hazard pointers.

## Resume lines this course backs up

- "multithreading & concurrency"
- "multithreaded application server handling transactional loan-origination requests"
- "thread synchronisation layer — mutexes, critical sections, wait objects and a pooled request model"
- "in-memory caching"
- tcpkv: "fixed thread pool ... store guarded by shared_mutex"
- "concurrency faults"
