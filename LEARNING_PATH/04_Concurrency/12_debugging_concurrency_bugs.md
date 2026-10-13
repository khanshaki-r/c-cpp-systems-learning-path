# 12. Debugging concurrency bugs

**Time:** ~3 hours   **Prerequisites:** 02, 06, 07

## 1. In plain words

Concurrency bugs are the worst class of bug because they do not reproduce. The code is correct
almost all the time; it fails when two threads hit a two-instruction window at the same moment,
which happens once a day in production and never on your laptop. Standard debugging (set a
breakpoint, step through) makes the window disappear, because the debugger serialises the
threads.

So the approach is different. Instead of reproducing the failure, you look for the *condition*
that permits it: an unordered pair of accesses (ThreadSanitizer finds these mechanically), an
inconsistent lock order (helgrind finds these), a lock held across a blocking call (a dump shows
it). And when you must reproduce, you do not wait for luck; you widen the window on purpose by
injecting sleeps, you multiply the attempts with stress, and you log the thread id on every line
so the trace can be read.

Analogy: you cannot catch the two clerks writing the same ledger line by watching them, because
they behave when watched. You audit the rules instead: which lines are shared, who is allowed to
write them, and whether the lock they are supposed to use is actually used on every path. The
audit finds the bug the camera never would.

## 2. The real thing

### 2.1 ThreadSanitizer

```sh
g++ -std=c++17 -g -O1 -fsanitize=thread -pthread prog.cpp -o prog
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 history_size=4" ./prog
```

What it detects: data races (module 02), lock-order inversions (potential deadlocks, even if the
deadlock did not occur on this run), use of a destroyed mutex, unlock from a non-owning thread,
`pthread_cond_wait` without the mutex, signal-unsafe calls in handlers, and thread leaks.

A report:

```
WARNING: ThreadSanitizer: data race (pid=1234)
  Write of size 8 at 0x7b0400000010 by thread T2 (mutexes: write M1):
    #0 Cache::set(...) cache.cpp:42
    #1 worker(...) main.cpp:88
  Previous read of size 8 at 0x7b0400000010 by thread T1:
    #0 Cache::get(...) cache.cpp:31
    #1 worker(...) main.cpp:85
  Location is heap block of size 64 at 0x7b0400000000 allocated by main thread:
    #0 operator new ...
    #1 main main.cpp:70
  Mutex M1 (0x...) created at: cache.cpp:12
  Thread T2 (tid=1240, running) created by main thread at: main.cpp:77
  Thread T1 (tid=1239, running) created by main thread at: main.cpp:76
```

Read it as: *this* access (with the mutexes it held) conflicts with *that* earlier access (with
the mutexes it held); nothing orders them. The key line is "mutexes: write M1" vs no mutex: one
side took the lock and the other did not. That is the fix in most cases. When both sides hold a
mutex but different ones, two locks are guarding one field. When neither does, a lock is missing.

Limitations: 5x to 15x slower and 5x to 10x memory (typical, per LLVM docs); it needs the racing
pair to actually execute in the run (it finds unordered accesses that *happened*, not ones in code
paths not taken, so coverage matters); it understands pthread, `std::atomic`, `std::mutex` but not
your hand-rolled spinlock unless you annotate it (`__tsan_acquire` / `__tsan_release` or build the
spinlock on `std::atomic` with proper orders, which TSan does understand). It cannot be combined
with ASan in one binary. Linux and macOS only; for Windows code, run the portable parts on WSL.

Lock-order report: `WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)` with the
two acquisition orders and stacks. It fires even if the second thread never ran at the same time.
This is the hierarchical mutex of module 06 for free.

### 2.2 helgrind and DRD

```sh
valgrind --tool=helgrind ./prog
valgrind --tool=drd ./prog
```

Valgrind's thread checkers. Slower than TSan (20x to 50x), no recompilation needed, work on any
binary with debug info. helgrind reports lock-order inversions explicitly ("lock order violated:
observed A then B, now B then A") with both stacks, and data races with happens-before analysis.
DRD is similar with different trade-offs (better on condition variables, more false positives
with lock-free code). Use them when you cannot rebuild with TSan or want a second opinion. Both
struggle with `std::atomic`-based code (they may report races on relaxed atomics that are not
races in the C++ sense).

### 2.3 Logging that can be read after the fact

A multithreaded log without thread ids is useless. Minimum fields per line: monotonic timestamp
(microseconds), thread id or name, component, message. Example line format:

```
1696348800.123456 [worker-07] cache: refresh begin version=42
```

Rules:

- Use `std::this_thread::get_id()` or, better, a `thread_local` name set at thread start
  (`pthread_setname_np`, `SetThreadDescription`) and read in the logger.
- Log lock acquisitions and releases for the suspect locks only, at debug level, with the lock
  name and the time waited. "Lock cache_mtx waited 230 ms" in the log is the deadlock's shadow.
- Never log under a lock you are diagnosing: logging takes its own lock and changes timing.
  Write to a per-thread ring buffer (module 08's SPSC) and drain from a logger thread.
- Keep a ring of the last N events per thread in memory and dump it in the crash handler. When
  the server dies, the last 100 things each thread did are in the core file.
- Log the *decision*, not just the action: "pop: queue empty, closed=1, exiting" tells you why a
  worker exited; "worker exiting" does not.

### 2.4 Reproducing on purpose

**Stress.** Run the operation in a loop from many threads for minutes. A race with a
one-in-a-million window shows in seconds at a million operations per second. Make a `stress_test`
target that runs every concurrent component with 2x cores threads for 60 seconds and asserts
invariants (sums match, no duplicates, no missing). Run it in CI under TSan.

**Sleep injection.** You suspect a window between `check` and `act`. Insert
`std::this_thread::sleep_for(1ms)` between them in a debug build (behind a macro,
`RACE_POINT("cache-fill")`) and run two threads. The window is now 1 ms wide; the race happens on
the first try. Then remove the sleep and write the fix. A table of named race points you can
enable from the environment is a serious tool; several large codebases have one.

**Yield injection.** `std::this_thread::yield()` at race points, randomly, with a seed. Less
heavy than sleeps; shuffles schedules.

**Pin to one core.** `taskset -c 0 ./prog`. Forces time-slicing, which exposes races that
multi-core parallelism hides (and hides some that it exposes). Run both ways.

**Reduce the data.** A hash map with 2 buckets collides on every insert; a queue of capacity 1
blocks on every push. Small sizes maximise contention and shrink the search space.

**Deterministic replay.** `rr record ./prog` then `rr replay` gives you a debugger session over an
exact recording, including every thread switch; you can step backwards. rr serialises threads onto
one core (so it changes what races happen) but once a run exhibits the bug you can examine it
forever. Linux only, needs hardware performance counters (not under WSL; use a native Linux box or
a VM with PMU passthrough).

### 2.5 Reading all thread stacks

The central skill. In **gdb** (live or on a core):

```
(gdb) info threads                      # list: id, LWP (kernel tid), current frame
(gdb) thread apply all bt               # every stack
(gdb) thread apply all bt 8             # top 8 frames each (long dumps)
(gdb) thread 5                          # switch
(gdb) frame 3                           # go to the frame of interest
(gdb) info locals
(gdb) p *this
(gdb) p *(pthread_mutex_t*)&m_          # see __owner
(gdb) p $_siginfo                       # on a crash: faulting address
```

To get a core from a hung process without killing it: `gcore PID` (needs `ulimit -c unlimited`
only for crash cores, not for gcore). From a crashed process: ensure `ulimit -c unlimited` and
check `/proc/sys/kernel/core_pattern` (on Ubuntu it may route to apport; set it to `core` or a
path for development).

In **WinDbg** on a dump (`.dmp` from Task Manager, procdump, or WER):

```
0:000> .symfix; .reload            ; symbols
0:000> ~                            ; list threads
0:000> ~*k                          ; every stack (~*kv for args, ~*kP for params expanded)
0:000> ~5s                          ; switch to thread 5
0:000> !locks                       ; owned CRITICAL_SECTIONs and their owners
0:000> !cs -l -o                    ; locked CSs with owner stacks
0:000> dt ntdll!_RTL_CRITICAL_SECTION ADDR
0:000> !analyze -v -hang            ; automated hang analysis: often names the deadlock
0:000> .ecxr; k                     ; on a crash: switch to the exception context
```

Patterns to recognise in stacks:

| Top frames | Meaning |
|---|---|
| `futex_wait` / `__lll_lock_wait` / `pthread_mutex_lock` | blocked on a mutex; find the owner |
| `futex_wait` / `pthread_cond_wait` | waiting on a condition variable; is a notify missing? |
| `NtWaitForSingleObject` / `RtlpWaitOnCriticalSection` | Win32 blocked on CS; `!cs` for owner |
| `NtWaitForAlertByThreadId` under `RtlAcquireSRWLock*` | blocked on SRWLOCK |
| `NtWaitForMultipleObjects` | WFMO; which handle? look at the args |
| `recv` / `read` / `WSARecv` under a lock holder | lock held across I/O |
| `sched_yield` or a tight loop with no syscall | spinning; livelock or spinlock |
| the same user function in every worker | the shared bottleneck |
| `__GI_raise` / `abort` / `__assert_fail` | the crashing thread; look at *its* frames and also what every other thread was doing at that instant |

A crash in thread 7 is often caused by thread 3. Always read all stacks, not just the faulting one.

### 2.6 Classic bug catalogue

**Use-after-free across threads.** Thread A deletes a session; thread B is still handling its last
request. Symptoms: crash in B inside session code with garbage fields, or ASan
`heap-use-after-free` naming the deleting thread's stack. Fix: shared ownership (`shared_ptr`) or
a drain protocol (mark closing, wait for in-flight count to reach zero, then delete).

**Iterator invalidation under concurrent write.** Thread A iterates a `std::map` under a shared
lock (or no lock); thread B inserts under the exclusive lock *after A released it* but A kept the
iterator. Or: A holds no lock at all and B rehashes an `unordered_map`. Symptoms: random crashes
in `_Rb_tree_increment` or `_M_find_before_node`. Fix: copy out under the lock, or hold the lock
for the whole iteration, or snapshot (module 11).

**Signal handler plus lock.** A signal arrives while the thread holds `malloc`'s lock or your
logger's mutex; the handler calls `printf` or `malloc` or your logger; self-deadlock. Symptoms:
hang with the handler at the top of one stack and a lock wait beneath it. Fix: handlers only set
a `volatile sig_atomic_t` or `std::atomic` flag and write to a pipe (`self-pipe trick`); the
real work happens in a normal thread. Same on Windows with console control handlers (they run on
a new thread, so the problem is a different one: racing with shutdown).

**Static initialisation order.** Two globals in different translation units; one's constructor
uses the other, which is not constructed yet. Not strictly a concurrency bug but shows up with
threads started from a global constructor using another global. Symptoms: crash before `main`.
Fix: function-local statics (module 11), never start threads before `main`.

**Destroyed while waiting.** A `condition_variable` or mutex member of an object destroyed while a
thread is still inside `wait`. UB; typically a crash in `pthread_cond_wait` returning into freed
memory. Fix: join or signal-and-wait-for-exit before destruction. Every object that owns a thread
must join it in its destructor.

**Detached thread at exit.** Static destructors run while a detached thread still uses a global.
Crash at shutdown only. Fix: never detach in a server (module 01).

**Lock held across callback.** The callback takes another lock or re-enters. Deadlock or recursion
on a non-recursive mutex. Fix: copy what the callback needs, release the lock, call.

**Double-checked init with a plain pointer.** Module 11.

**Non-atomic flag loop.** `while (!done) {}` hoisted. Module 02.

**Lost wakeup.** Flag set outside the mutex. Module 05.

**Missing `notify` on a rare path.** Queue grows on one code path that forgot to notify. Symptoms:
items in the queue, consumers asleep. Module 05 exercise 4.

**ABA in a lock-free free list.** Module 08.

**False sharing.** Not a correctness bug; a performance one that looks like "slower with more
threads". Module 10.

### 2.7 A diagnostic procedure for a hang

1. Do not restart. Capture: `gcore PID` / `procdump -ma PID`. Also `top -H -p PID` or Process
   Explorer's thread tab to see whether any thread is burning CPU (livelock/spin) or all are at 0%
   (deadlock/blocked I/O).
2. `thread apply all bt` / `~*k`. Save to a file.
3. Classify each thread by its top frames using the table above.
4. For each blocked-on-lock thread, find the lock and its owner. Build the wait-for graph.
5. Cycle: deadlock; fix the order. No cycle, owner in I/O: lock across blocking call. No cycle, all
   in `cond_wait`, queue non-empty: lost notify. All in `cond_wait`, queue empty, no clients: not a
   bug (idle). Threads spinning: livelock or a spinlock on a descheduled holder.
6. Write the finding as: "T7 holds M_cache (acquired at cache.cpp:120) and waits for M_conn
   (conn.cpp:88). T12 holds M_conn (conn.cpp:40) and waits for M_cache (cache.cpp:95). Lock order
   document says cache(40) before conn(25) is forbidden; conn.cpp:88 is the violation."

## 3. Code

File: `code/m12_tsan_demo.cpp` — a small program with four seeded bugs (an unlocked counter, a
lock-order inversion, a condition variable flag set outside the mutex, and a use-after-free across
threads), each selectable by command-line argument so you can run TSan or ASan against one at a
time and read a clean report.

```sh
g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m12_tsan_demo.cpp -o tsan_demo
./tsan_demo race        # data race report
./tsan_demo order       # lock-order-inversion report (no actual deadlock occurs)
./tsan_demo wakeup      # race on the flag written outside the mutex
g++ -std=c++17 -g -O1 -fsanitize=address -pthread m12_tsan_demo.cpp -o asan_demo
./asan_demo uaf         # heap-use-after-free across threads
```

The race-point macro pattern used in the file:

```cpp
#ifdef RACE_POINTS
#define RACE_POINT(name) do { if (race_point_enabled(name)) std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (0)
#else
#define RACE_POINT(name) do {} while (0)
#endif
```

with `race_point_enabled` reading a comma-separated list from the `RACE_POINTS` environment
variable, so `RACE_POINTS=cache-fill ./prog` widens exactly one window.

## 4. Under the hood (senior-level)

### How TSan instruments

Every load and store in instrumented code becomes a call into the runtime that updates the
shadow for that address (4 shadow cells per 8 bytes, each 8 bytes: thread id, epoch, access
size/offset, write bit). Every synchronisation (mutex lock/unlock, atomic with ordering, thread
create/join) updates vector clocks: a thread's clock is a vector of the latest epoch it has
observed from each other thread. On unlock, the thread's clock is stored into the mutex's clock
(release); on lock, the mutex's clock is merged into the thread's (acquire). An access races with a
shadow cell if they overlap, at least one is a write, they are from different threads, and the
cell's epoch is greater than the current thread's clock entry for that thread (so the earlier
access is not known to have happened-before). Memory overhead is the shadow (8 cells x 8 bytes per 8
bytes of application memory in the default configuration, i.e. the 5x to 10x figure). The runtime
also intercepts `malloc` so it can report where the racing memory was allocated.

### Why the debugger hides races

A breakpoint stops *all* threads (gdb's default all-stop mode). Stepping runs one thread a few
instructions while the others are frozen. No two threads execute concurrently, so no race. gdb's
`set scheduler-locking off` and non-stop mode (`set non-stop on`) let the others run, which
restores concurrency but makes the stepping thread's view inconsistent. Logging with microsecond
timestamps to a per-thread buffer is the only observation tool that does not materially change the
schedule.

### Core dumps and threads

A Linux core contains every thread's registers and the full memory image. gdb reconstructs
`info threads` from the `NT_PRSTATUS` notes, one per thread. `__owner` fields in mutexes, the
contents of your queues, and `thread_local` variables (reachable through each thread's TLS base)
are all there. A Windows minidump with `MiniDumpWithFullMemory` is equivalent; the default Task
Manager dump is full memory. A minidump without full memory has stacks but not heap, so `!cs` may
fail to resolve owners; always ask for `-ma` with procdump.

### `!analyze -v -hang`

WinDbg's hang analysis walks every thread's wait, resolves the object being waited on (CS owner,
mutex owner, event), and looks for cycles. It frequently produces a one-line
"DEADLOCK: thread 7 waits on CS owned by thread 12, which waits on CS owned by thread 7". It does
not understand SRWLOCK ownership as well (SRWLOCK does not record its owner), which is one practical
cost of SRWLOCK vs CRITICAL_SECTION. Record the owner yourself in debug builds if you use SRWLOCK
heavily.

### The limits of sanitizers

TSan sees only what executes. A race on a code path exercised only by a Tuesday-night batch job
is invisible on Monday. Coverage from tests under TSan is the real metric. TSan does not model
weak memory (it treats the program as sequentially consistent after accounting for the memory
orders you wrote), so it reports relaxed-vs-acquire mistakes as races (good) but cannot show you a
specific weak-memory reordering. For that, there are model checkers (CDSChecker, GenMC,
herd7 litmus tests) and real ARM hardware.

## 5. In production

- **The dump nobody took.** The hang happened, someone restarted the service, the incident was
  closed as "unknown, resolved by restart". It will happen again. Establish the rule: a hang gets
  a dump before a restart, always, and the on-call runbook has the exact `procdump` or `gcore`
  command.
- **Thread names pay for themselves the first time.** `~*k` with 64 threads named `worker-00` to
  `worker-63`, `acceptor`, `refresher`, `logger` reads in two minutes; the same with
  `0x1a3c` ids reads in an hour.
- **TSan in CI finds the bugs you were going to ship.** Build the test suite with
  `-fsanitize=thread`, run nightly, treat any report as a build failure. The first run will find
  three races in code that has "worked for years". Each is a story for the interview: "we found a
  race on the statistics counters that had been silently undercounting; the fix was a relaxed
  atomic."
- **Sleep injection as a review tool.** When a reviewer says "I think there is a window between
  these two lines", the fastest way to settle it is to put a `RACE_POINT` there and run the test.
  Thirty seconds, no argument.
- **Watchdog plus dump.** A thread that notices "no request completed in 30 s while queue depth >
  0" and calls `abort()` (Linux, to get a core) or `MiniDumpWriteDump` then `ExitProcess` (Windows)
  converts a 2-hour outage into a 30-second restart with full evidence.
- **The 3 AM procedure, condensed:** is CPU 0% or 100%? Dump. All stacks. Classify. Owners.
  Graph. Write it down in the incident ticket in the "T7 holds X waits Y" form. Fix the order or
  the lock-across-I/O. Add a TSan test that would have caught it.

## 6. Try it

1. **(easy)** Build `m12_tsan_demo.cpp` with TSan and run each of `race`, `order`, `wakeup`. For
   each, identify in the report the two accesses and what synchronisation each held. Done when you
   have a one-line fix for each written down.
2. **(easy)** Build with ASan and run `uaf`. Identify the freeing thread's stack and the using
   thread's stack. Done when you can state the ownership fix.
3. **(medium)** Run `m06_deadlock_demo hang` from module 06 under `valgrind --tool=helgrind`.
   helgrind reports the lock order inversion *before* the deadlock happens. Compare with TSan's
   `lock-order-inversion`. Done when you have both reports and have noted which is clearer.
4. **(medium)** Add `RACE_POINT` support to `m05_producer_consumer.cpp` at the gap between the
   predicate check and the wait (you will need to inline the wait loop to place it). Enable it and
   remove the lock around `closed_ = true`. Produce the lost wakeup deterministically. Done when the
   program hangs every run with the race point on and never with the lock restored.
5. **(hard)** Take a core from `m06_deadlock_demo hang` with `gcore`. In gdb, without running the
   program, identify both deadlocked threads, print each mutex's `__owner`, map to LWP ids, and
   write the wait-for graph. Then do the same for a three-lock cycle you build yourself. Done when
   you can produce the "T holds X waits Y" paragraph from the core alone in under ten minutes.
6. **(hard)** Build a 50-line per-thread ring-buffer event log (`thread_local` array of 256
   `{timestamp, const char* event, long arg}`) and a crash handler (`SIGSEGV` / `SIGABRT` via
   `sigaction`) that dumps every thread's ring to stderr (register each thread's ring in a global
   list at thread start). Trigger a crash from one worker and show the last events of every thread.
   Done when the output lets you reconstruct what each thread was doing when the crash happened.

## 7. Interview questions

**Q: How do you find a data race that you cannot reproduce?**
Build with ThreadSanitizer and run the test suite and a stress test; TSan reports unordered
conflicting accesses based on happens-before tracking, so it finds races that did not produce a
wrong result on that run. Supplement with code audit of which mutex guards which field, and with
sleep or yield injection at suspected windows to widen them deliberately.

**Q: Walk me through diagnosing a hung multithreaded server.**
Capture a dump before restarting. Get every thread's stack. Classify each by its top frames: mutex
wait, condition wait, I/O, spinning. For mutex waiters find the owner (mutex owner field or
`!locks`). Build the wait-for graph; a cycle is a deadlock and the application frames show the
inconsistent order. Owner in I/O means a lock held across a blocking call. Condition waiters with a
non-empty queue mean a missing notify.

**Q: Why does adding a breakpoint or a print make a race disappear?**
A breakpoint stops all threads and stepping runs one at a time, removing concurrency. A print takes
the stdio lock and spends tens of microseconds in I/O, serialising the threads and shifting their
timing so the narrow window no longer aligns. Non-intrusive per-thread in-memory logging is the
observation tool that preserves the schedule.

**Q: What is a lock-order inversion and how do tools detect it before a deadlock happens?**
Thread A acquired X then Y at some point; thread B acquired Y then X at another point, perhaps
hours apart. The cycle is possible even if the two never overlapped. TSan and helgrind record the
set of locks held at each acquisition and report the first time they see an acquisition order that
contradicts a previously observed one, regardless of timing.

**Q: Name three classic multithreading bugs and their fixes.**
Use-after-free across threads: one thread deletes an object another is using; fix with shared
ownership or a drain-before-delete protocol. Iterator invalidation: iterating a container while
another thread mutates it; fix by copying under the lock or using snapshots. Lost wakeup: flag set
outside the condition variable's mutex; fix by taking the mutex around the write.

**Q: Why should signal handlers not take locks or call `malloc`?**
The signal may arrive while the interrupted thread already holds that lock (including the
allocator's internal lock), so the handler blocks forever on itself. Handlers must only do
async-signal-safe operations: set an atomic flag, write to a pipe, and let a normal thread do the
work.

**Q: What does TSan not catch?**
Races on code paths that did not execute during the run, so coverage matters. Specific weak-memory
reorderings (it checks ordering rules, not hardware behaviour). Synchronisation it does not
understand, such as a custom spinlock not built on `std::atomic`, unless annotated. And anything in
a Windows-only build, since it runs on Linux and macOS.

## 8. Recap

- TSan: `-fsanitize=thread`; finds unordered conflicting accesses and lock-order inversions by
  happens-before, not by outcome. Run the test suite under it in CI.
- helgrind/DRD: no rebuild needed, slower, good second opinion on lock order.
- Log with thread names and microsecond timestamps; never log under the lock you are debugging.
- Reproduce by widening the window: stress, sleep injection at named race points, small sizes,
  single-core pinning, rr.
- Read *all* stacks: `thread apply all bt`, `~*k`. Classify by top frames. Find owners. Build the
  wait-for graph. Cycle = deadlock; owner in I/O = lock across blocking call; empty waiters with a
  full queue = lost notify.
- `!analyze -v -hang` and `!locks` in WinDbg; `__owner` in glibc mutexes in gdb.
- Catalogue: cross-thread use-after-free, iterator invalidation, signal plus lock, static init
  order, destroyed-while-waiting, detached at exit, lock across callback.
- Capture a dump before restarting. Every time.
- The debugger hides races; in-memory per-thread rings do not.
