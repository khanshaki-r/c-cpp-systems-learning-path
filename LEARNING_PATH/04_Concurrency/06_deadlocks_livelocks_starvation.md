# 06. Deadlocks, livelocks and starvation

**Time:** ~2.5 hours   **Prerequisites:** 03, 05

## 1. In plain words

A deadlock is two or more threads each waiting for something another one of them holds, forever.
Thread A holds lock 1 and wants lock 2; thread B holds lock 2 and wants lock 1. Neither will ever
release what it holds because neither will ever finish. The server stops accepting requests, CPU
drops to zero, and nothing in the log says why.

Analogy: two cars meet on a one-lane bridge from opposite ends. Each waits for the other to back
up. Both are being perfectly reasonable and nobody moves. A livelock is when both back up, both
try again at the same moment, meet again, back up again, forever: lots of motion, no progress.
Starvation is when the bridge has a priority rule and one car is always told to wait because a
higher-priority car keeps arriving.

You have almost certainly seen a deadlock in your server: the one where every worker is "stuck"
and the only fix was a restart. This module gives you the theory to prevent them, the discipline
(a lock order document) to keep them prevented as the code grows, and the technique to find the
culprit in a crash dump in minutes rather than hours.

## 2. The real thing

### 2.1 The four necessary conditions (Coffman, 1971)

A deadlock can occur only if all four hold at once:

1. **Mutual exclusion**: a resource can be held by one thread at a time. (Every mutex.)
2. **Hold and wait**: a thread holds one resource while waiting for another.
3. **No preemption**: a resource cannot be forcibly taken from its holder.
4. **Circular wait**: a cycle of threads each waiting for a resource held by the next.

Break any one and deadlock is impossible. In practice:

- (1) cannot be broken for a mutex; it is the point. You can reduce it with lock-free structures
  (module 08) or immutable data.
- (2) is broken by acquiring all locks at once (`std::scoped_lock`, `std::lock`) or by never
  holding a lock while blocking on another (`try_lock` and back off).
- (3) is broken by timeouts: `try_lock_for` gives up and releases what it holds. The lock is not
  preempted but the thread voluntarily releases.
- (4) is broken by **lock ordering**: if every thread acquires locks in the same global order,
  a cycle cannot form. This is the standard production answer.

### 2.2 Lock ordering

Assign every mutex in the program a rank. A thread may acquire a mutex only if its rank is
greater than every mutex the thread currently holds. Then a thread holding rank 3 can never wait
for rank 1, and a thread holding rank 1 that wants rank 3 may wait for the first thread, but the
first thread is never waiting for the second. No cycle.

Two variants:

- **Static order**: a document (section 2.7) listing mutexes by rank. Enforced by code review and
  by a debug-build checker (section 2.3).
- **Dynamic order by address**: when two locks are of the same kind and chosen at runtime (two
  `Account` objects), order by `std::less<void*>` on their addresses, or by a stable id. Or just
  use `std::scoped_lock`, which does not need an order.

### 2.3 Lock hierarchies with a runtime checker

A hierarchical mutex records, in a `thread_local` variable, the rank of the most recently acquired
lock. On `lock()`, if the new mutex's rank is not greater than the current thread-local value,
throw or abort. On `unlock()`, restore the previous value. In debug builds this turns a potential
deadlock into an immediate, deterministic failure at the first wrong-order acquisition, even if
the second thread needed for the actual deadlock never runs.

```cpp
class hierarchical_mutex {
    std::mutex m_;
    const unsigned rank_;
    unsigned prev_rank_ = 0;
    static thread_local unsigned this_thread_rank;   // 0 = holding nothing; higher = outer
    void check() const {
        if (this_thread_rank != 0 && rank_ >= this_thread_rank)
            throw std::logic_error("lock hierarchy violated");
    }
    void update() { prev_rank_ = this_thread_rank; this_thread_rank = rank_; }
public:
    explicit hierarchical_mutex(unsigned rank) : rank_(rank) {}
    void lock()   { check(); m_.lock(); update(); }
    void unlock() { this_thread_rank = prev_rank_; m_.unlock(); }
    bool try_lock() { check(); if (!m_.try_lock()) return false; update(); return true; }
};
thread_local unsigned hierarchical_mutex::this_thread_rank = 0;   // but see the file for the
                                                                 // "ULONG_MAX means nothing held" variant
```

(Here lower rank is acquired *later*: hold 1000, may take 500, may not take 2000. The code file
uses the convention "may only take a lower rank than you hold"; either is fine if documented.)

Clang's `-Wthread-safety` with `ACQUIRED_BEFORE` / `ACQUIRED_AFTER` attributes checks a static
order at compile time. Valgrind's helgrind detects inconsistent lock orders at runtime without
any code changes (module 12).

### 2.4 try_lock and back-off

When you cannot impose an order (two subsystems that each think they own the top rank), the
fallback is:

```cpp
for (;;) {
    std::unique_lock<std::mutex> la(a);
    if (b.try_lock()) { /* have both */ ... b.unlock(); return; }
    la.unlock();
    std::this_thread::yield();   // or sleep_for a random few microseconds
}
```

This breaks hold-and-wait: the thread never blocks while holding. It risks **livelock**: two
threads each take their first lock, each fail `try_lock` on the second, each release, each retry
at the same instant, forever. Randomised back-off (sleep for a random 1 to 50 µs) makes the
collision probability drop each round. `std::lock` does this internally, which is why
`scoped_lock` is the better tool whenever both mutexes are known at the same call site.

### 2.5 Livelock and starvation

**Livelock**: threads are running, not blocked, but make no progress because they keep undoing
each other. Symptoms: 100% CPU, zero throughput. Causes: try-lock loops without back-off, two
threads each retrying a CAS that the other keeps invalidating, two protocol peers each waiting for
the other to speak first with identical timeouts.

**Starvation**: a thread that is runnable and would make progress never gets the resource. Causes:
a reader-preferring rwlock with continuous readers (module 04), an unfair mutex where a busy
thread keeps re-acquiring before woken waiters get scheduled (lock convoy, module 03), a
scheduler priority gap where a high-priority spinner never yields.

### 2.6 Priority inversion

A low-priority thread L holds a mutex. A high-priority thread H blocks on it. A medium-priority
thread M, which needs no lock, becomes runnable and preempts L. Now H waits on L, L waits for CPU
behind M. H is effectively running at M's priority: inverted. The Mars Pathfinder lander (1997)
rebooted repeatedly from exactly this until a priority-inheritance flag was turned on remotely.

Fixes: **priority inheritance** (the mutex temporarily raises L to H's priority while H waits;
`PTHREAD_PRIO_INHERIT` on Linux, automatic for Win32 kernel Mutex objects through the balance set
manager's boosts, not available for `CRITICAL_SECTION`), **priority ceiling** (every lock has a
ceiling priority; the holder runs at it), or simply **do not use thread priorities** in a server.
Most request servers run every worker at the same priority precisely to avoid this class of
problem. If you have a real-time thread (audio, trading), give it a lock-free path.

### 2.7 A lock order document for a server

Every multithreaded server of any size should have one. It fits on a page:

```
LOCK ORDER (acquire top to bottom; never acquire a higher line while holding a lower one)

 10  g_shutdown_mtx          process lifecycle flag; held for microseconds
 20  ConnectionTable::mtx    accept thread inserts; workers look up by fd
 30  Session::mtx            per-session; one at a time; never two sessions at once
 40  Cache::mtx (shared)     master-table cache; readers shared, refresh exclusive
 50  Metrics::mtx            counters; leaf lock; never call out while holding
 60  Logger::mtx             leaf lock; held only for the write(2); never call out

RULES
 - A "leaf" lock is one under which no other lock is taken and no callback is invoked.
 - Locks 50 and 60 are leaf locks. Anything that logs while holding 20..40 is fine because 60 > 40.
 - Never take any lock while holding a DB connection's lock (DB pool lock is 25) except 50, 60.
 - Two Session locks are never held together. Cross-session work goes through a message.
 - Any new mutex must be added here with a rank before merge.
 - Debug builds use hierarchical_mutex and will abort on violation.
```

The things that make it work: numbers with gaps (insert later), the leaf-lock concept (so logging
and metrics can be called from anywhere), an explicit "never" for same-rank pairs, and a merge
gate. Interviewers asking "how do you prevent deadlocks in a large codebase" want to hear this,
not "I am careful".

### 2.8 Detecting a deadlock in a crash dump or a live process

Deadlocked processes do not crash; they hang. Capture a dump (Windows: Task Manager "Create dump
file", or `procdump -ma pid`; Linux: `gcore pid`, or attach with `gdb -p pid`). Then:

**gdb:**

```
(gdb) thread apply all bt
```

**WinDbg:**

```
0:000> ~*k           ; stack of every thread
0:000> !locks        ; CRITICAL_SECTIONs that are currently owned, with owner thread id
0:000> !cs -l        ; locked critical sections with details
```

Procedure:

1. Find every thread whose top frames are a wait: `futex_wait`, `__lll_lock_wait`,
   `pthread_mutex_lock`, `pthread_cond_wait`; on Windows `NtWaitForSingleObject`,
   `RtlpWaitOnCriticalSection`, `RtlEnterCriticalSection`, `NtWaitForAlertByThreadId` (SRWLOCK).
2. For each, note **which mutex** it is waiting on. In gdb, go up to the frame that called
   `lock()` and print the mutex's address (`frame 3`, `p &m_` or `info locals`). In WinDbg, the
   argument to `RtlEnterCriticalSection` is the CRITICAL_SECTION address, and `!cs ADDR` prints
   its owner thread.
3. For each mutex, find **who holds it**. glibc `pthread_mutex_t` stores the owner's TID in
   `__data.__owner` (`p *(pthread_mutex_t*)ADDR`). WinDbg's `!locks` lists owners directly.
4. Draw the graph: thread -> waiting on mutex -> held by thread -> waiting on mutex -> ...
   A cycle is your deadlock. Two threads is common; three or four happens.
5. Read down each deadlocked thread's stack to find the *application* frames that took the first
   lock and are now trying the second. Those two code paths acquire in opposite orders. That is
   the bug and the fix is to make them agree with the lock order document.

If every waiting thread's mutex is held by a thread that is *not* waiting on a mutex but is in a
`recv`, `read`, or database call: not a deadlock, but a lock held across I/O (module 03 rule).
Same symptom, different fix.

## 3. Code

File: `code/m06_deadlock_demo.cpp`

```cpp
#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <stdexcept>

struct Account {
    std::mutex m;
    long balance;
    std::string name;
    Account(std::string n, long b) : balance(b), name(std::move(n)) {}
};

// BROKEN: acquires in argument order. transfer(a,b) and transfer(b,a) in parallel will deadlock.
void transfer_broken(Account& from, Account& to, long amt) {
    std::lock_guard<std::mutex> l1(from.m);
    std::this_thread::sleep_for(std::chrono::microseconds(10));   // WHY: widen the window so it reliably hangs
    std::lock_guard<std::mutex> l2(to.m);
    from.balance -= amt; to.balance += amt;
}

// FIX 1: scoped_lock locks both with the std::lock algorithm; argument order is irrelevant.
void transfer_scoped(Account& from, Account& to, long amt) {
    std::scoped_lock lk(from.m, to.m);
    from.balance -= amt; to.balance += amt;
}

// FIX 2: a global order by address. Equivalent guarantee, shows the principle explicitly.
void transfer_ordered(Account& from, Account& to, long amt) {
    Account* first = &from; Account* second = &to;
    if (std::less<Account*>{}(second, first)) std::swap(first, second);   // WHY std::less: defined for unrelated pointers
    std::lock_guard<std::mutex> l1(first->m);
    std::lock_guard<std::mutex> l2(second->m);
    from.balance -= amt; to.balance += amt;
}

// FIX 3: try_lock with back-off. Breaks hold-and-wait; needs randomised sleep to avoid livelock.
void transfer_trylock(Account& from, Account& to, long amt) {
    for (int attempt = 0;; ++attempt) {
        std::unique_lock<std::mutex> l1(from.m);
        if (to.m.try_lock()) {
            std::lock_guard<std::mutex> l2(to.m, std::adopt_lock);
            from.balance -= amt; to.balance += amt;
            return;
        }
        l1.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(1 + (attempt * 7) % 13));   // crude jitter
    }
}

// A hierarchical mutex: debug-time enforcement of a lock order. Convention here: you may only
// acquire a mutex with a rank LOWER than the lowest you currently hold.
class hierarchical_mutex {
    std::mutex m_;
    const unsigned long rank_;
    unsigned long prev_ = 0;
    static thread_local unsigned long this_thread_lowest;   // ULONG_MAX = holding nothing
    void check() const {
        if (rank_ >= this_thread_lowest)
            throw std::logic_error("lock order violation: rank " + std::to_string(rank_) +
                                   " while holding " + std::to_string(this_thread_lowest));
    }
public:
    explicit hierarchical_mutex(unsigned long rank) : rank_(rank) {}
    void lock()   { check(); m_.lock(); prev_ = this_thread_lowest; this_thread_lowest = rank_; }
    void unlock() { this_thread_lowest = prev_; m_.unlock(); }
    bool try_lock() { check(); if (!m_.try_lock()) return false; prev_ = this_thread_lowest; this_thread_lowest = rank_; return true; }
};
thread_local unsigned long hierarchical_mutex::this_thread_lowest = ~0UL;

hierarchical_mutex high(10000), low(5000);

void correct_order()  { std::lock_guard<hierarchical_mutex> a(high); std::lock_guard<hierarchical_mutex> b(low); }
void wrong_order()    { std::lock_guard<hierarchical_mutex> b(low);  std::lock_guard<hierarchical_mutex> a(high); }

template <class F>
void run_pair(const char* label, F transfer, bool expect_hang) {
    Account a("A", 1000), b("B", 1000);
    std::atomic<bool> done{false};
    std::thread t1([&] { for (int i = 0; i < 2000; ++i) transfer(a, b, 1); });
    std::thread t2([&] { for (int i = 0; i < 2000; ++i) transfer(b, a, 1); });
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done.load()) {
            std::cout << label << ": DEADLOCKED (watchdog fired after 3 s). Attach gdb now, or ctrl-C.\n";
            if (expect_hang) std::_Exit(0);   // WHY _Exit: joining the deadlocked threads is impossible
        }
    });
    t1.join(); t2.join();
    done.store(true);
    watchdog.join();
    std::cout << label << ": ok, sum=" << a.balance + b.balance << '\n';
}

int main(int argc, char**) {
    run_pair("scoped_lock ", transfer_scoped, false);
    run_pair("ordered     ", transfer_ordered, false);
    run_pair("try_lock    ", transfer_trylock, false);

    correct_order();
    try { wrong_order(); } catch (const std::logic_error& e) { std::cout << "hierarchy caught: " << e.what() << '\n'; }

    if (argc > 1) {   // ./m06 hang  -> demonstrate the real deadlock for gdb practice
        std::cout << "running the broken version; this will hang...\n";
        run_pair("broken      ", transfer_broken, true);
    }
    return 0;
}
```

Run `./m06` for the fixes and `./m06 hang` to produce a real deadlock. While it hangs, from another
WSL terminal: `gdb -p $(pgrep m06)` then `thread apply all bt`. You will see two threads inside
`__lll_lock_wait` under `transfer_broken`, each having passed the first `lock_guard`. That is the
exercise in section 6.

## 4. Under the hood (senior-level)

### What a blocked thread looks like

In Linux, a thread in `futex_wait` is in state `S` (interruptible sleep) in `/proc/PID/task/TID/
stat`, consuming no CPU. `cat /proc/PID/task/TID/wchan` shows `futex_wait_queue`. The scheduler
has removed it from the run queue and placed it on the futex hash bucket's wait list. It will
never be considered for scheduling until a `FUTEX_WAKE` on that address or a signal. That is why a
deadlocked process shows 0% CPU and why `kill -SEGV` (to force a core dump) still works: signals
interrupt the futex wait.

### Reading the owner out of a glibc mutex

```
(gdb) frame 4                   # the frame inside your code that called lock()
(gdb) p *(pthread_mutex_t*)&m_
$1 = {__data = {__lock = 2, __count = 0, __owner = 12345, ...}}
```

`__lock = 2` means locked with waiters, `__owner` is the kernel TID (what `info threads` shows as
`LWP 12345`). This is only populated for default and error-checking mutexes, which is what
`std::mutex` uses on glibc. Match `__owner` to `info threads` and you have the holder.

### Why deadlock detection is not in the mutex

`PTHREAD_MUTEX_ERRORCHECK` detects self-deadlock (locking what you hold) and returns `EDEADLK`.
General cycle detection across threads would require every lock to record its waiters in a shared
graph and run cycle detection on each blocking acquire: a global lock on every lock operation.
Databases do this (PostgreSQL's deadlock detector runs when a lock wait exceeds `deadlock_timeout`,
default 1 s, and aborts one transaction) because transaction locks are already expensive and
rollback is possible. Mutexes cannot roll back. So detection is left to debug tools (helgrind, the
hierarchical mutex) and to the human with a dump.

### `std::lock`'s algorithm, precisely

libstdc++ locks `m1`, then tries `m2 ... mn` in order with `try_lock`. On failure at `mk` it
unlocks everything, then rotates so `mk` is the first to be fully `lock()`ed next round. Because
the thread blocks on at most one mutex at a time while holding nothing else, hold-and-wait is
never satisfied. The rotation reduces livelock: the mutex that was contended becomes the one we
wait on, which is also the one most likely to be released soon.

### Watchdogs

A production server should detect its own hang. A watchdog thread increments a heartbeat counter;
each worker touches a per-worker "last progress" timestamp when it finishes a request; the
watchdog checks that at least one worker progressed in the last N seconds while the queue is
non-empty. If not: log every thread's state, dump the lock-owner table (if you record owners),
write a core (`abort()` on Linux produces one with `ulimit -c unlimited`; `MiniDumpWriteDump` on
Windows), and exit so the supervisor restarts. Thirty minutes of downtime become thirty seconds,
and you get a dump to analyse in the morning.

## 5. In production

- **The accept-thread / worker deadlock.** Accept thread holds `ConnectionTable::mtx` to insert
  a new session and calls `Session::init()`, which takes `Session::mtx`. A worker holds
  `Session::mtx` to finish a request and calls `ConnectionTable::remove()` on close, which takes
  `ConnectionTable::mtx`. Order: 20 then 30 vs 30 then 20. The fix after the lock order document
  exists is obvious: `remove()` must be called after `Session::mtx` is released. Before the
  document exists, it is a week of "it hangs sometimes".
- **The logger that locks.** Logger has a mutex. Someone adds a log line inside the cache's
  exclusive section. Someone else adds a cache lookup inside the logger (to print the current
  user). Deadlock across two unrelated subsystems. Rule: leaf locks never call out.
- **Database lock plus process mutex.** Worker A holds a process mutex and waits for a database
  row lock. Worker B holds that row lock (in its own transaction) and waits for the process
  mutex. The database's deadlock detector does not see the process mutex, so it never fires.
  Rule: never hold a process lock across a database call (module 03).
- **The deadlock that is not one.** All workers blocked on a mutex held by a thread that is
  blocked in `recv()` from a client that went silent. No cycle, but the same symptom. Fix:
  socket timeouts, and never hold the lock across the `recv`.
- **Timeouts as a last line.** If a lock acquisition in a request path takes a timed lock and
  logs the lock name and holder on timeout, the first deadlock in production produces a log line
  that names both sides. Cheap insurance.
- **3 AM procedure.** Capture a dump before restarting. Always. The restart fixes the symptom;
  the dump is the only evidence. `procdump -ma` or `gcore`, then restart, then analyse in the
  morning with section 2.8.

## 6. Try it

1. **(easy)** Build `m06_deadlock_demo.cpp`, run `./m06`, then `./m06 hang`. Attach gdb and run
   `thread apply all bt`. Identify the two threads in `__lll_lock_wait`, the frame in
   `transfer_broken` for each, and which `Account` each holds. Done when you have written the
   cycle: "T1 holds A waits B; T2 holds B waits A".
2. **(easy)** In gdb, print the owner field of the mutex each deadlocked thread is waiting on and
   match it to `info threads`. Done when the TIDs match your cycle.
3. **(medium)** Write a three-thread deadlock: T1 takes A then B, T2 takes B then C, T3 takes C
   then A. Produce the hang, dump the stacks, and draw the three-node cycle. Then fix it with the
   lock order document approach by assigning ranks. Done when the fixed version runs 10,000
   iterations without hanging.
4. **(medium)** Remove the jitter sleep from `transfer_trylock` and replace it with a bare `yield`.
   Run with two threads on two cores pinned (`taskset -c 0,1 ./m06`). Measure throughput versus
   the jittered version. Done when you can show the livelock-prone version is slower or stalls,
   and explain why randomisation helps.
5. **(hard)** Write the lock order document for a server you know (your real one, anonymised, or
   tcpkv extended with a connection table, a metrics block and a logger). Assign ranks, mark leaf
   locks, list every "never" pair. Then implement `hierarchical_mutex` for all of them in a
   debug build and write a test that violates the order and is caught. Done when the document is
   one page and the test fails deterministically.

## 7. Interview questions

**Q: What are the four conditions for deadlock and which do you break in practice?**
Mutual exclusion, hold-and-wait, no preemption, and circular wait. In practice you break circular
wait with a global lock order, or hold-and-wait with `std::scoped_lock` / `std::lock` or try-lock
with back-off, or no-preemption with timed locks that give up and release.

**Q: How do you prevent deadlocks in a large codebase?**
A lock order document: every mutex has a rank, locks are taken in increasing rank only, leaf locks
never call out, and adding a mutex requires adding it to the document. Enforce in debug builds
with a hierarchical mutex that aborts on the first out-of-order acquisition, and run helgrind in
CI. Also never hold a lock across I/O or a callback.

**Q: How do you diagnose a deadlock from a crash dump?**
Get every thread's stack (`thread apply all bt`, `~*k`). Find threads blocked in a lock wait and
which mutex each is waiting on. Find each mutex's owner (`__owner` in glibc's `pthread_mutex_t`,
`!locks` in WinDbg). Draw thread -> mutex -> owner -> mutex; a cycle is the deadlock. The
application frames above the waits show the two code paths that acquire in opposite orders.

**Q: What is the difference between deadlock, livelock and starvation?**
Deadlock: threads blocked forever in a cycle, zero CPU. Livelock: threads running but undoing
each other's progress, full CPU, zero throughput, typically try-lock loops without back-off.
Starvation: a runnable thread never gets the resource because the policy always favours others,
e.g. reader-preferring rwlocks or unfair mutexes.

**Q: What is priority inversion and how is it solved?**
A high-priority thread waits on a lock held by a low-priority thread that is preempted by a
medium-priority thread, so the high-priority thread effectively runs at medium priority. Solutions:
priority inheritance (holder temporarily inherits the waiter's priority; `PTHREAD_PRIO_INHERIT`),
priority ceiling, or avoiding thread priorities in servers altogether.

**Q: Why does `std::scoped_lock` with two mutexes not deadlock, and when would you still use an
explicit lock order instead?**
It uses `std::lock`: lock one, try the rest, release all and retry on failure, so it never blocks
while holding another of the set. You still use an explicit order when the two locks are acquired
at different call sites or in different functions, where no single `scoped_lock` can see both.

**Q: A hung server shows all workers waiting on one mutex, and the owner is in a `recv` call. Is
that a deadlock?**
No; there is no cycle. It is a lock held across blocking I/O. The owner will proceed when the
peer sends or the socket times out. The fix is to never hold the lock across the call and to put
a timeout on the socket. The diagnosis technique is the same: find the owner and see what it is
waiting for.

## 8. Recap

- Deadlock needs mutual exclusion, hold-and-wait, no preemption and circular wait, all at once.
- Lock ordering breaks circular wait and is the standard production prevention.
- `std::scoped_lock` / `std::lock` break hold-and-wait for locks acquired at one site.
- try-lock with randomised back-off breaks hold-and-wait at the cost of livelock risk.
- Timed locks break no-preemption and turn hangs into logged failures.
- A hierarchical mutex turns order violations into deterministic debug-build failures.
- Write and maintain a one-page lock order document with ranks and leaf locks.
- Diagnose from a dump: all stacks, who waits on what, who owns what, find the cycle.
- Priority inversion: avoid thread priorities in servers or use priority inheritance.
- Always capture a dump before restarting a hung server.
