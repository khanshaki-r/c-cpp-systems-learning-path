# 10. Concurrency crashes and Heisenbugs

**Time:** ~3 hours   **Prerequisites:** 04, 06, 08; `04_Concurrency` modules on mutexes,
condition variables and module 12 of that course (debugging concurrent code)

## 1. In plain words

A Heisenbug is a bug that changes or disappears when you look at it. Almost all of them are
concurrency bugs: two threads touch the same data without agreeing on an order, and whether the
program breaks depends on which thread got there first by a few nanoseconds. Attach a debugger,
add a `printf`, run on a different machine, and the timing changes and the bug hides.

Concurrency bugs do not have their own crash signature. They *cause* the ordinary memory bugs of
module 04 - a use-after-free because thread B deleted what thread A was reading, a corrupted
container because two threads inserted at once, a lost update because both read-modify-wrote -
or they cause a hang when threads wait for each other forever. So the debugging skill is: look at
an ordinary-looking crash or hang, recognise the fingerprints that say "another thread did this",
and then switch tools from "find the bad line" to "find the missing ordering". This module is
the fingerprints, the dump-reading procedure for deadlocks, and how to make the bug reproduce on
purpose.

## 2. The real thing

### 2.1 Crash signatures of data races

A race rarely crashes at the racing instruction. It corrupts something, and the crash comes later,
in the thread that trusted the corrupted thing. The fingerprints:

| What the dump shows | What probably happened |
|---------------------|------------------------|
| Crash inside a container's internals (`std::_Rb_tree_insert_and_rebalance`, `_Hashtable::_M_insert`, `std::vector::_M_realloc_insert`, `std::deque` iterator math) with a garbage pointer | two threads mutated the container without a lock (or one read while another mutated: `find` racing `insert` is enough for `unordered_map` rehash) |
| UAF-shaped crash (tcache pointer / `0xDDDDDDDD` / `0xFEEEFEEE` in the object) in a thread that did not free anything | another thread freed the object: lifetime race; shared object with unclear ownership, or a callback after `shutdown` |
| Crash in a virtual call with a bad vtable pointer (`call [rax+8]` with garbage rax) | object destroyed by another thread while this one was about to call a method on it |
| Counter/total wrong but no crash; checksum mismatch; "impossible" state assertion | lost update: unsynchronised read-modify-write (`++`, `+=`, `if (!x) x = ...`) |
| Crash only under load / only with N cores / only on the client's 32-core box / never under gdb or Valgrind | timing-dependent -> race; Valgrind serialises threads so races vanish under it |
| Same corruption, different crash site every time | a race that corrupts a shared structure; which thread trips over it varies |
| Crash in a thread's stack frame whose locals look "half-updated" (one field new, one old) | torn read/write of a multi-word value (a 16-byte struct, a `std::string`, a double on 32-bit) |
| `thread apply all bt` shows two threads inside the same non-reentrant method of the same object | that is the race, caught in the act; compare the `this` pointers |
| Crash in a signal handler, or `malloc` deadlock with one thread in the handler | async-signal-unsafe call in a handler (module 11); technically a race between the handler and the interrupted code |

The decisive move in a core: **`thread apply all bt` and look at every thread, not just the one
that crashed.** Then for the crashing object, ask "who else could have a pointer to this?" and look
for that thread. In a race, the other thread is often *in the same function* or in a destructor /
`erase` / `close` for the same object at the moment of the dump. `p $_siginfo` and the registers
tell you *what* broke; the other threads tell you *who*.

### 2.2 Deadlock dumps

A deadlock is two or more threads each waiting for a lock the other holds. The dump is static -
nothing is moving - which makes it the one concurrency bug that a core proves outright.

**Linux procedure (gdb on a `gcore` of the hung process):**

```
(gdb) thread apply all bt 12
```

Every blocked thread shows `__futex_abstimed_wait_common` / `___pthread_mutex_lock` /
`__lll_lock_wait` (mutex), or `pthread_cond_wait` (condition variable), or `__GI___poll` /
`read` (I/O), or `pthread_rwlock_rdlock/wrlock`. Collect the ones waiting on **mutexes**.

For each, go to the frame that owns the `std::lock_guard` / calls `lock()` and print the mutex:

```
(gdb) thread 3
(gdb) frame 4                        # the frame in transfer_a_to_b
(gdb) p g_connections_mtx
$1 = {<std::__mutex_base> = {_M_mutex = {__data = {__lock = 2, __count = 0, __owner = 4243,
      __nusers = 1, __kind = 0, __spins = 0, __elision = 0, __list = {__prev = 0x0, __next = 0x0}}, ...
```

`__lock = 2` means locked **with waiters**, `__owner = 4243` is the **kernel thread id (LWP)** of
the holder. (`__lock = 1` = locked, no waiters; `__lock = 0` = free.) Now:

```
(gdb) info threads
  Id   Target Id                                     Frame
  1    Thread 0x7f... (LWP 4241) "m10_deadlock"      ... pthread_join
  2    Thread 0x7f... (LWP 4242) "m10_deadlock"      __futex_abstimed_wait_common ...   <- waits for connections_mtx
* 3    Thread 0x7f... (LWP 4243) "m10_deadlock"      __futex_abstimed_wait_common ...   <- waits for sessions_mtx
(gdb) thread 2 ; frame 4 ; p g_sessions_mtx.__data.__owner      ->  4242? no: 4243... 
```

Build the table:

| Thread (LWP) | Holds | Waits for | Held by |
|--------------|-------|-----------|---------|
| 4242 | `g_sessions_mtx` | `g_connections_mtx` | 4243 |
| 4243 | `g_connections_mtx` | `g_sessions_mtx` | 4242 |

A cycle. That is the deadlock, proven. The fix is a lock order (always sessions before
connections) or `std::scoped_lock(m1, m2)`; the RCA says *which code path acquired in the wrong
order and since which change*.

Variations:

- **Self-deadlock**: `__owner` equals the waiting thread's own LWP. A non-recursive mutex locked
  twice on one thread (a callback re-entering, or a method calling another method of the same
  object that also locks). One-thread cycle.
- **Lock held by a dead thread**: `__owner` is an LWP not in `info threads`. A thread exited (or
  was cancelled) while holding the mutex; or the lock is in shared memory and another process
  holds it. `PTHREAD_MUTEX_ROBUST` exists for the cross-process case.
- **Condition variable "deadlock"**: everyone in `pthread_cond_wait`, nobody holds the mutex. Not
  a lock cycle: a **lost wakeup** (signal before wait with no predicate re-check) or a
  predicate that can never become true. Check the predicate variables in the frame.
- **Starvation / livelock**: threads not blocked but never progressing (spinning on a CAS,
  retrying). `top -H` shows CPU; `gcore` twice a minute apart and compare the frames.
- **Blocked on I/O that will never complete**: `read` on a socket whose peer is gone with no
  timeout; `connect` to a dead DB. Looks like a hang; not a lock bug; fix with timeouts.

**Windows procedure:** `~*k` (or `!uniqstack` to collapse identical waiters), waiters sit in
`ntdll!NtWaitForAlertByThreadId` (SRW locks / `std::mutex`), `RtlpWaitOnCriticalSection`
(critical sections), `NtWaitForSingleObject` (events, mutex objects). For critical sections,
`!locks` lists `OwningThread` per held section; `~~[tid]s` jumps to it; `k` shows what it is
waiting on; follow the cycle. For SRW locks no owner is stored: the holder is the thread that is
*inside* the protected code, not inside a wait; the frame with the `lock_guard` tells you which
lock. `!runaway` to spot a spinner.

### 2.3 Stack traces that lie

Concurrency dumps punish literal reading:

- **The crashing thread is the victim.** It dereferenced what another thread freed. Its stack is
  correct and innocent. The culprit's stack is in another thread, and only *if* that thread is
  still near the scene; often it has moved on and its stack shows the next request. Then you need
  the object's history (ASan's free stack, or your own trace buffer), not the dump.
- **Inlined and optimised frames** (module 02) make "two threads in the same function" look like
  "two threads in the caller". `info frame` and `disassemble` to confirm what was executing.
- **`<signal handler called>`** frames: the code below the marker was interrupted
  asynchronously; its locals may be mid-update.
- **Stale frames after a corrupted stack** (`??`, "following frames may be wrong"): if the race
  overflowed a stack buffer, the trace past that frame is fiction.
- **Thread names and ids**: `info threads` ids are gdb's numbering (1..N by discovery), not the
  LWP; logs and `__owner` use the LWP. Mixing them up is a classic wrong-thread conclusion.
- **Under TSan/ASan the stack is right but the timing is different**: the race TSan reports may
  not be the one that crashed in production; it is still a real race. Fix all of them.
- **The thread that "holds" a lock per `__owner` may have been pre-empted inside a 3-instruction
  critical section**: holding a lock in a dump is not evidence of a long hold, just a snapshot.
  Two `gcore`s show whether anything moved.

### 2.4 Reproducing on purpose

A race needs a specific interleaving. Make interleavings more likely and more varied:

| Technique | How | Effect |
|-----------|-----|--------|
| **More cores than threads** | run on a big box; `taskset -c 0-15` | true parallelism of the racing threads |
| **Fewer cores than threads** | `taskset -c 0 ./prog` | forces pre-emption at arbitrary points; exposes lost-wakeup and ordering assumptions |
| **Background load** | `stress-ng --cpu 8 --io 2 --vm 2 --timeout 10m` alongside | random pre-emption and cache pressure |
| **Loop the test** | `for i in $(seq 1000); do ./test || break; done` | rare interleavings eventually occur |
| **Sleep/yield injection** | `std::this_thread::yield()` or `usleep(rand()%100)` at suspected points (behind a `#ifdef RACE_TEST`) | widens the window of the suspected race |
| **Randomised scheduling** | `rr record --chaos` (native Linux), or `setarch -R` + taskset combos | explores schedules deliberately |
| **TSan** | `-fsanitize=thread` on the test suite | finds the race from any run that executes both accesses; no manifestation needed |
| **Barrier-start** | start all workers on a `std::latch`/barrier so they race from the first instruction | removes thread start-up skew that hides races |
| **Hostile allocator** | `MALLOC_PERTURB_=165`, ASan quarantine | turns UAF-by-race into a loud failure |

Order of attempts in practice: TSan first (cheapest, most reliable), then stress + loop with the
test that resembles the production traffic, then sleep injection at the suspected window to confirm
the mechanism, then the fix verified by the same TSan run and stress loop.

### 2.5 Diagnostics that do not change the timing too much

Adding `printf` to a race often cures it: `printf` takes the stdio lock, does a syscall, and
serialises the threads. The rules for observing without disturbing:

- **Per-thread in-memory ring buffers** (a "flight recorder"): each thread appends fixed-size
  records (timestamp, event id, two integers) to its own array with a relaxed atomic index. No
  locks, no syscalls, ~20 ns per event. On crash, the buffers are **in the core dump**, and you
  read them with `p trace_buf[thread]` or a Python script. This is how production databases and
  trading systems debug races.
- **Timestamps**: `rdtsc` (`__rdtsc()`), or `std::chrono::steady_clock` (vDSO, no syscall).
  Never `gettimeofday` through a lock or `time()` with formatting in the hot path.
- **Atomic counters** (`std::atomic<uint64_t>` with `fetch_add(1, memory_order_relaxed)`) for
  "how many times did we enter path X" - cheap, no ordering, no timing impact worth mentioning.
- **Log after the fact**: record in the ring, dump the ring from the crash handler (module 11)
  using `write(2)` only.
- **Kernel tracing**: `perf trace`, `strace -f -tt` (slows a lot), `ltrace`, `bpftrace` uprobes on
  your functions (`bpftrace -e 'uprobe:/path/server:close_session { printf("%d %s\n", tid, ustack) }'`)
  - observes without recompiling, overhead per probe in the microsecond range.
- **Windows**: ETW (`TraceLoggingWrite`, `xperf`/WPA) is the ring-buffer approach built into the
  OS; `OutputDebugString` is a lock+syscall and perturbs like `printf`.
- **Assertions on invariants** (`assert(owner_tid == gettid())` inside a critical section;
  `assert(refcount > 0)` before use) crash *at the race* instead of later; cheap enough to leave
  in release builds as `ALWAYS_ASSERT` for the critical ones (module 13).

What *does* change timing and should be avoided while hunting: `printf`/`std::cout`, mutex-
protected log calls in the hot path, `strace`, running under Valgrind (serialises threads), and
breakpoints.

### 2.6 Fixing versus papering over

A race fix is one of: add the missing synchronisation (mutex, atomic with the right ordering),
remove the sharing (per-thread data, message passing, immutable snapshots), or fix the lifetime
(owner-controlled destruction, `shared_ptr`/`weak_ptr`, "stop then join then destroy"). A deadlock
fix is one of: a global lock order (documented and asserted), `std::scoped_lock` for multi-lock
acquisition, `try_lock` with back-off, or lock splitting/removal. "Added a sleep", "retry on
failure", "made the mutex recursive" are not fixes; they change the odds. An RCA must say which
of the real categories the fix is (module 12). The design side is in `04_Concurrency`.

## 3. Code

`code/m10_data_race.cpp`: two threads increment a global and mutate a `std::map` without a
lock.

```bash
./build_debug.sh debug tsan
./build/m10_data_race_debug          # "expected 2000000 got 1737422 (map size 97)" - or a crash in _Rb_tree
./build/m10_data_race_tsan 2>&1 | head -40
# WARNING: ThreadSanitizer: data race ... Write of size 8 ... by thread T2 ... Previous write ... by thread T1
#   Location is global 'g_total' of size 8
# then a second report for the map node pointers
```

Watch the plain binary crash occasionally in the red-black tree code: that is signature #1 of
2.1. Run `taskset -c 0 ./build/m10_data_race_debug` and note how the lost-update count changes
(fewer cores -> different interleavings).

`code/m10_deadlock.cpp`: lock-order inversion.

```bash
./build/m10_deadlock_debug &  PID=$!
sleep 2; gcore -o /tmp/hang $PID; kill $PID
gdb -q ./build/m10_deadlock_debug /tmp/hang.$PID
(gdb) thread apply all bt 8
(gdb) thread 2 ; frame 5 ; p g_connections_mtx.__data.__owner ; p g_sessions_mtx.__data.__owner
(gdb) info threads
```

Fill in the Holds / Waits-for / Held-by table from section 2.2 with your real LWPs. Then:

```bash
TSAN_OPTIONS=detect_deadlocks=1:second_deadlock_stack=1 timeout 10 ./build/m10_deadlock_tsan 2>&1 | head -50
# WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)
#   Cycle in lock order graph: M1 (0x...) => M2 (0x...) => M1
#   Mutex M2 acquired here while holding mutex M1 in thread T1: ... transfer_a_to_b
#   Mutex M1 acquired here while holding mutex M2 in thread T2: ... transfer_b_to_a
```

TSan finds the inversion even on a run where the deadlock did not happen. That is why it belongs
in CI.

Diagnostic ring buffer sketch (to put in your own server; not a separate file):

```cpp
struct TraceRec { uint64_t tsc; uint32_t ev; uint32_t a; uint64_t b; };
struct ThreadTrace { TraceRec rec[4096]; std::atomic<uint32_t> idx{0}; };
inline thread_local ThreadTrace *t_trace = nullptr;          // assigned from a global table at thread start
inline void trace(uint32_t ev, uint32_t a, uint64_t b) {
    if (!t_trace) return;
    uint32_t i = t_trace->idx.fetch_add(1, std::memory_order_relaxed) & 4095;
    t_trace->rec[i] = { __rdtsc(), ev, a, b };                // ~20 ns, no lock, no syscall
}
// in gdb on the core:  p g_traces[3].rec[(g_traces[3].idx-1)&4095]   or a 20-line Python dump sorted by tsc
```

## 4. Under the hood (senior-level)

**Why races corrupt containers.** `std::map::insert` rebalances a red-black tree through several
pointer writes; `unordered_map::insert` may rehash, allocating a new bucket array and relinking
every node; `vector::push_back` may reallocate and move. A second thread reading or writing midway
sees a tree with a node linked from two parents, a bucket array half-populated, or a dangling
pointer into the old vector buffer. The standard's guarantee is: concurrent *const* operations on
the same container are fine; any non-const operation requires exclusive access. `find` concurrent
with `insert` is a data race and UB, not "mostly safe".

**Why a lost update happens even on x86.** `g_total += 1` compiles to `mov rax,[g]; add rax,1;
mov [g],rax` (or `add [g],1`, which is still not atomic across cores without `lock`). Two cores
can both load 5, both store 6. The cache coherence protocol keeps *each* store coherent; it does
not make the read-modify-write sequence atomic. `lock add` / `std::atomic::fetch_add` does. The
C++ memory model says the plain version is UB, so the compiler may also keep `g_total` in a
register for the whole loop (it does at `-O2`: look at the asm), making the lost-update count
enormous.

**futex and `__owner`.** glibc mutexes are a 32-bit word in user space: uncontended lock is a
single `cmpxchg` (0 -> 1); on contention the word becomes 2 and the thread sleeps in the kernel
with `futex(FUTEX_WAIT)`; unlock stores 0 and, if the word was 2, calls `futex(FUTEX_WAKE)`.
`__owner` is written after acquiring, for debugging and for error-checking/recursive mutex types.
That is why a core shows `__lock = 2, __owner = <tid>`, and why a mutex "held by a dead thread"
stays locked forever: nothing in the kernel knows about user-space mutex ownership (unlike
Windows mutex *objects*, which are abandoned on thread exit and report `WAIT_ABANDONED`).

**Why TSan does not need the race to manifest.** It computes happens-before from the
synchronisation it observes; two unordered accesses are reported regardless of whether, in that
run, they actually overlapped in time. This is strictly more powerful than stress testing for
the paths the test executes. Its blind spot is paths not executed and synchronisation it cannot
see (custom spinlocks without atomics - which are also UB).

**Signal handlers as a concurrency problem.** A handler runs on the interrupted thread between
any two instructions. Calling `malloc`, `printf`, or taking a mutex inside it can deadlock (the
interrupted code may hold that same lock) or corrupt (the allocator was mid-operation). Only
async-signal-safe functions (`write`, `_exit`, `sigaction`, ...) are allowed; `code/m11_crash_handler.cpp`
follows those rules and module 11 explains them.

**Memory ordering and "it works on x86".** x86 is strongly ordered (stores are not reordered
with other stores, loads not with loads), so many ordering bugs are invisible on x86 and appear
on ARM (phones, Graviton, Apple silicon). A bank migrating to ARM servers can surface years-old
races. The C++ model (`memory_order_*`) is the only portable contract - `04_Concurrency`.

## 5. In production

- **Dumps for hangs are routine, not exceptional.** A `gcore`/ProcDump of a hung server before
  restarting it is non-negotiable (module 11); a deadlock is *proven* by a dump and *guessed*
  without one. Train the operations team to take it.
- **Two dumps a minute apart** distinguish deadlock (nothing moved), livelock/starvation (same
  threads, different frames), and slow progress (everything moved).
- **Lock order as documentation and as a check.** Write the global lock order down (module 13's
  review checklist). In debug builds, assert it: a thread-local stack of held lock ranks and an
  assert that a new lock's rank is higher. Cheap, finds inversions before TSan does.
- **Lifetime races in RPC servers**: a connection is closed by the reader thread (peer went away)
  while a worker is still writing a response to it; a session is expired by the sweeper thread
  while a request on it is mid-flight. The dump: worker crashed in `send`/`write` on a freed
  connection object, and the sweeper/reader thread's stack shows `close`/`erase` for that id a
  moment ago. The fix is ownership: refcount the connection (`shared_ptr`), or "mark closed, let
  the last user destroy".
- **Thread pools and callbacks after shutdown**: a timer fires into an object already destroyed
  during shutdown. Dump: crash during exit, with `__run_exit_handlers` on one thread and a worker
  in a callback on another. Fix: stop timers and join workers *before* destroying what they use.
- **Windows specifics**: `std::mutex` on MSVC is an SRW lock - no owner in the dump, infer from
  stacks; `CRITICAL_SECTION` has `!locks`; the Windows kernel mutex object reports
  `WAIT_ABANDONED` if the owner dies, pthreads do not. Also `WaitForMultipleObjects` deadlocks
  through the loader lock (`DllMain`) are a Windows-only classic: a thread inside `DllMain`
  waiting for a thread that needs the loader lock. `!locks` shows `ntdll!LdrpLoaderLock`.
- **Never close a concurrency RCA with "could not reproduce, added a retry"**. Either TSan/stress
  reproduces it and the fix is one of the real categories, or the RCA states the evidence,
  the most likely mechanism, the mitigation and the detection added to catch the next occurrence
  with more data (ring buffer, asserts) - and stays open.

## 6. Try it

1. **Easy - see the race.** Run `m10_data_race_debug` ten times and record the totals; then under
   `taskset -c 0`; then under TSan. Done when you can explain the three sets of results.
2. **Easy - the deadlock table.** Do the `gcore` walkthrough in section 3 and produce the
   Holds / Waits-for / Held-by table with real LWPs. Done when `__owner` values match
   `info threads`.
3. **Medium - victim and culprit.** Write a program where thread A `delete`s a shared object
   after 1 ms and thread B calls a virtual method on it in a loop. Crash it, take the core, and
   from `thread apply all bt` identify both threads and the evidence that A freed it (A's frame,
   the tcache pointer where the vtable was). Done when the evidence is listed without using ASan;
   then confirm with ASan.
4. **Medium - sleep injection.** Make `m10_data_race` race only rarely (e.g. increment every
   1000th iteration), confirm it is hard to see, then inject `std::this_thread::yield()` between
   the load and the store (split `g_total += 1` into two statements with a yield) and show it
   fails every run. Done when you can state why the yield widens the window.
5. **Hard - ring buffer in the core.** Add the trace ring from section 3 to `m10_data_race`,
   record `(ev=1, a=tid, b=i)` around the map insert, crash it (or `abort()` on a wrong total),
   and in gdb print the last 10 records of each thread sorted by `tsc`. Done when the interleaving
   around the failure is visible from the core.
6. **Hard - lock-rank assertion.** Implement a debug-only lock rank checker (thread-local vector
   of ranks, assert on acquisition out of order) and wrap the two mutexes of `m10_deadlock`.
   Done when the program aborts with a clear message at the first inverted acquisition, before
   it has a chance to deadlock.

## 7. Interview questions

**Q: How do you recognise that a crash was caused by a data race rather than a plain memory
bug?**
A: The crash is in a shared structure (container internals, a shared object with a garbage vtable
or a freed-pattern), it depends on load or core count, vanishes under Valgrind or a debugger, and
`thread apply all bt` shows another thread in a mutating operation on the same object (erase,
close, destructor) at the time of the dump. The crashing thread is the victim; the other thread
is the suspect; TSan on the same test confirms.

**Q: Walk me through diagnosing a deadlock from a core dump.**
A: `thread apply all bt` (or `~*k`) to find threads blocked in mutex waits; for each, select the
frame holding the lock guard and print the mutex: `__owner` is the LWP of the holder and
`__lock = 2` means contended; map LWPs with `info threads`; build a holds/waits-for table and find
the cycle. On Windows, `!locks` gives critical section owners; for SRW locks infer the holder from
the thread executing inside the protected region.

**Q: Why does adding a `printf` make a race disappear, and what do you do instead?**
A: `printf` takes the stdio lock and performs a syscall, serialising the threads and widening or
closing the race window. Use per-thread lock-free ring buffers with `rdtsc` timestamps and relaxed
atomic indices, read them from the core after the crash, or use kernel tracing (`bpftrace`, ETW)
which observes without recompiling and with microsecond overhead.

**Q: What does ThreadSanitizer report that stress testing cannot?**
A: TSan reports any pair of unordered conflicting accesses that executed, whether or not they
overlapped in time in that run - it derives ordering from the synchronisation it observes
(happens-before via vector clocks). Stress testing needs the bad interleaving to actually occur
and to produce a visible failure. TSan also reports lock-order inversions as potential deadlocks
from a run that never hung.

**Q: How do you reproduce a race that only happens at the client?**
A: Run the TSan build of the test closest to the client's traffic; run the plain build in a loop
under `stress-ng` with more and with fewer cores than threads (`taskset`); inject yields at the
suspected window to confirm the mechanism; use a hostile allocator (`MALLOC_PERTURB_`, ASan) so a
lifetime race fails loudly. If none reproduces, add a ring buffer and invariant asserts to the
production build so the next occurrence captures the interleaving.

**Q: A `pthread_mutex_t` in a core shows `__owner` equal to a thread id that no longer exists.
What happened?**
A: A thread exited (or was cancelled) while holding the mutex, so it is locked forever - pthread
mutexes have no kernel-side ownership and are not released on thread death (unlike Windows mutex
objects, which report `WAIT_ABANDONED`). Every later locker blocks. The fix is RAII locking so the
unlock cannot be skipped, and not letting threads die with locks held; `PTHREAD_MUTEX_ROBUST` for
the cross-process case.

**Q: What are legitimate fixes for a race and a deadlock, and what are not?**
A: Race: add correct synchronisation (mutex, atomic with proper ordering), eliminate the sharing
(thread-local, message passing, immutable snapshots), or fix the lifetime (owner-controlled
destruction, `shared_ptr`/`weak_ptr`). Deadlock: a documented and asserted global lock order,
`std::scoped_lock` for multiple locks, `try_lock` with back-off, or removing a lock. Not fixes:
sleeps, retries, making the mutex recursive, or "could not reproduce".

## 8. Recap

- Races cause ordinary memory bugs (UAF, corrupted containers, lost updates); recognise them by
  the container-internals crash, the freed-pattern object, load dependence, and another thread
  mid-mutation in `thread apply all bt`.
- The crashing thread is the victim; look at every thread; match `this` pointers.
- Deadlock from a core: find mutex waiters, print the mutex, `__owner` = holder's LWP,
  `__lock = 2` = contended, build holds/waits-for, find the cycle. Windows: `!locks` for critical
  sections, stack inference for SRW/`std::mutex`.
- Variants: self-deadlock (`__owner` == self), dead owner, lost wakeup (all in `cond_wait`),
  livelock (take two dumps), I/O without timeout.
- Stacks lie: inlining, signal frames, corrupted stacks, gdb ids vs LWPs, culprit moved on.
- Reproduce: TSan first; then loop + `stress-ng` + `taskset`; yield injection to confirm; hostile
  allocator for lifetime races.
- Observe without perturbing: per-thread lock-free ring buffers with `rdtsc`, atomic counters,
  bpftrace/ETW, invariant asserts; never `printf` in the window.
- Real fixes: synchronisation, no sharing, or ownership; lock order, `scoped_lock`, `try_lock`.
  Sleeps and retries are not fixes.
