# 03. Mutexes: lock_guard, unique_lock, scoped_lock

**Time:** ~3 hours   **Prerequisites:** 01, 02

## 1. In plain words

A mutex (mutual exclusion) is a token that only one thread can hold at a time. A thread that wants
to touch shared data takes the token, does its work, and gives the token back. A thread that finds
the token taken waits. That is all a mutex does. Everything else in this module is about how to
take and give back the token without ever forgetting, how to hold two tokens without two threads
each waiting for the other's, and what the operating system does while a thread waits.

You already do this with `EnterCriticalSection` / `LeaveCriticalSection`. The C++ difference is
that you almost never call lock and unlock by hand. You create a small object whose constructor
locks and whose destructor unlocks, so that leaving the scope by return, break, or exception
always releases the mutex. This is RAII (Resource Acquisition Is Initialisation) applied to locks,
and it eliminates the single most common bug in C lock code: the early `return` that skips the
`LeaveCriticalSection`.

## 2. The real thing

### 2.1 The mutex types in `<mutex>` and `<shared_mutex>`

| Type | Recursive? | Timed? | Shared? | Win32 nearest |
|---|---|---|---|---|
| `std::mutex` | no | no | no | `CRITICAL_SECTION` (but CS is recursive), `SRWLOCK` exclusive |
| `std::timed_mutex` | no | yes | no | `WaitForSingleObject(hMutex, ms)` |
| `std::recursive_mutex` | yes | no | no | `CRITICAL_SECTION`, Win32 `Mutex` handle |
| `std::recursive_timed_mutex` | yes | yes | no | Win32 `Mutex` with timeout |
| `std::shared_mutex` (C++17) | no | no | yes | `SRWLOCK` |
| `std::shared_timed_mutex` (C++14) | no | yes | yes | none |

Operations on all of them: `lock()`, `try_lock()`, `unlock()`. Timed ones add
`try_lock_for(duration)` and `try_lock_until(time_point)`. Shared ones add `lock_shared()` etc.
(module 04).

Rules the standard imposes:

- `unlock()` must be called by the thread that locked. Unlocking from another thread is UB.
- Locking a non-recursive mutex you already hold is UB (in practice: deadlock on Linux, or
  an exception / assertion in debug builds).
- Destroying a locked mutex is UB.
- `std::mutex` is neither copyable nor movable. A class containing one is not copyable unless you
  write the copy yourself (and lock both sides while copying).

### 2.2 The lock wrappers

**`std::lock_guard<M>`** — the default. Locks in the constructor, unlocks in the destructor,
nothing else. No member functions. Cannot be moved. Use it for 90% of critical sections.

```cpp
{
    std::lock_guard<std::mutex> lk(m);   // locked here
    shared_value += 1;
}                                        // unlocked here, even if += threw
```

**`std::unique_lock<M>`** — a movable lock that can be unlocked and re-locked, can defer locking,
can adopt an already-held lock, and can try or time out. It is slightly larger (holds a `bool
owns` flag) and slightly slower. You need it when:

- a `std::condition_variable` must unlock and re-lock around the wait (module 05);
- you need to release the lock early in the middle of a function;
- you want to transfer lock ownership out of a function (return it);
- you want `try_lock` or a timeout.

```cpp
std::unique_lock<std::mutex> lk(m, std::defer_lock);   // not locked yet
if (lk.try_lock_for(std::chrono::milliseconds(50))) {  // only for timed_mutex
    ...
}
lk.unlock();          // early release
...
lk.lock();            // re-acquire
```

**`std::scoped_lock<M...>`** (C++17) — locks one or more mutexes at once with a deadlock-avoiding
algorithm, unlocks all in the destructor. With one mutex it is identical to `lock_guard`. With
two or more it replaces the C++11 idiom `std::lock(m1, m2); lock_guard a(m1, adopt_lock); ...`.

```cpp
void transfer(Account& from, Account& to, long amount) {
    std::scoped_lock lk(from.m, to.m);   // any order of arguments: never deadlocks against
    from.balance -= amount;              // another transfer(to, from, ...)
    to.balance   += amount;
}
```

With C++17 class template argument deduction you write `std::scoped_lock lk(a, b);` without the
template arguments. Prefer `scoped_lock` over `lock_guard` in new code; it does everything
`lock_guard` does and also handles the multi-lock case.

### 2.3 How `std::lock` avoids deadlock

`std::lock(m1, m2, ...)` (used by `scoped_lock`) locks the first mutex, then `try_lock`s the
rest. If any `try_lock` fails, it unlocks everything it holds, yields, and starts again with the
one that failed as the first to `lock()`. This is "lock one, try the rest, back off". It cannot
deadlock because at no point does a thread hold one mutex while blocking on another. It can
livelock in theory under extreme contention; libstdc++ rotates the starting mutex to avoid that in
practice. For two mutexes a fixed global order (module 06) is cheaper; `scoped_lock` is for when
you cannot know the order statically (two `Account` objects chosen at runtime).

### 2.4 `recursive_mutex` and why to avoid it

A recursive mutex lets the same thread lock it again while already holding it; it counts depth and
only the matching number of unlocks release it. `CRITICAL_SECTION` and the Win32 `Mutex` handle
are both recursive, which is why you may never have thought about this.

Why it is a design smell:

1. It hides the fact that a function is called both with and without the lock held. The
   invariant "this data is consistent when the mutex is unlocked" becomes "consistent when the
   depth is zero", and a function at depth 2 may see state that a function at depth 1 left
   half-updated.
2. It makes it impossible to use with `condition_variable` correctly: the wait releases one level,
   and the outer levels still hold the lock, so the notifier can never acquire it.
3. It costs a thread-id compare on every lock.

The fix is almost always two functions: a public `foo()` that takes the lock and calls a private
`foo_locked()` that assumes it. Convention: suffix `_locked` or `_unlocked` and a comment saying
which mutex must be held. This also documents the lock discipline for the next reader.

### 2.5 Timed mutexes

`try_lock_for` is for "I would rather fail this request than block the thread forever". In a
request server that is a legitimate design: a worker that cannot get the cache lock within 50 ms
returns a "retry later" error instead of becoming one of the 64 stuck threads at 3 AM. Combine with
a counter of how often it times out; a rising count is your early warning of a lock convoy.

### 2.6 Granularity

Coarse-grained: one mutex for the whole store. Simple, correct, serialises everything.
Fine-grained: one mutex per bucket, per account, per table. More parallelism, more mutexes, more
chances of deadlock, more memory (a `std::mutex` is 40 bytes on x86-64 Linux glibc; a `SRWLOCK`
is pointer-sized; a `CRITICAL_SECTION` is 40 bytes on 64-bit Windows).

Rules of thumb:

- Start coarse. Measure. Split only the mutex that shows contention.
- Hold a lock for as short a time as possible: compute outside, lock, copy or swap, unlock.
- **Never hold a lock across a blocking call** you do not control: disk I/O, a database round
  trip, a network send, `malloc` of large size, or a call into code that might take another lock.
  This is the rule that your 3 AM deadlock will have broken.
- Never hold a lock while calling a user-supplied callback.

### 2.7 Mapping to what you already use

| Win32 | C++17 | Note |
|---|---|---|
| `InitializeCriticalSection(&cs)` | `std::mutex m;` | constructor does it |
| `InitializeCriticalSectionAndSpinCount(&cs, 4000)` | `std::mutex` | glibc mutex does not spin by default; `PTHREAD_MUTEX_ADAPTIVE_NP` does |
| `EnterCriticalSection(&cs)` | `m.lock()` or `lock_guard` | CS is recursive, `std::mutex` is not |
| `TryEnterCriticalSection(&cs)` | `m.try_lock()` | |
| `LeaveCriticalSection(&cs)` | `m.unlock()` or scope exit | |
| `DeleteCriticalSection(&cs)` | destructor | |
| `CreateMutex(NULL, FALSE, NULL)` | `std::recursive_timed_mutex` roughly | Win32 Mutex is a kernel object: cross-process, waitable with WFMO, 1 µs per lock |
| `WaitForSingleObject(hMutex, 100)` | `try_lock_for(100ms)` | |
| `ReleaseMutex(hMutex)` | `unlock()` | |
| abandoned mutex (`WAIT_ABANDONED`) | no equivalent | a thread dying while holding a `std::mutex` is UB; the kernel Mutex detects it |

The important conceptual shift: `CRITICAL_SECTION` is a user-mode spin-then-block lock inside one
process; the Win32 `Mutex` *handle* is a kernel object that costs a syscall every time. You have
probably been told "use CRITICAL_SECTION unless you need cross-process". `std::mutex` is the
`CRITICAL_SECTION` of C++, and it is implemented the same way: user-mode fast path, kernel wait on
contention. The kernel wait mechanism on Linux is the futex.

## 3. Code

File: `code/m03_mutex_counter.cpp`

```cpp
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <map>
#include <string>

// A thread-safe counter map: the smallest realistic "store guarded by a mutex".
class CounterMap {
    mutable std::mutex m_;              // mutable: const readers must still lock
    std::map<std::string, long> data_;  // GUARDED_BY(m_)
public:
    void increment(const std::string& key, long by = 1) {
        std::lock_guard<std::mutex> lk(m_);      // RAII: unlocked on every exit path
        data_[key] += by;
    }
    long get(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = data_.find(key);
        return it == data_.end() ? 0 : it->second;
    }
    // Snapshot copy: hold the lock only for the copy, never while the caller iterates.
    std::map<std::string, long> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return data_;
    }
};

// Two accounts, two mutexes, lock both without deadlock regardless of argument order.
struct Account {
    std::mutex m;
    long balance = 0;
    explicit Account(long b) : balance(b) {}
};

void transfer(Account& from, Account& to, long amount) {
    std::scoped_lock lk(from.m, to.m);   // WHY: std::lock algorithm; transfer(a,b) and
    from.balance -= amount;              // transfer(b,a) in parallel cannot deadlock
    to.balance   += amount;
}

// unique_lock: release early so the slow part runs unlocked.
std::mutex log_mtx;
std::vector<std::string> log_lines;

void log_and_do_slow_work(const std::string& msg) {
    std::unique_lock<std::mutex> lk(log_mtx);
    log_lines.push_back(msg);
    lk.unlock();                                   // WHY: the sleep below must not hold the lock
    std::this_thread::sleep_for(std::chrono::microseconds(50));
}

int main() {
    CounterMap counters;
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&counters, t] {
            for (int i = 0; i < 100000; ++i) counters.increment(t % 2 ? "odd" : "even");
        });
    }
    for (auto& t : ts) t.join();
    std::cout << "odd=" << counters.get("odd") << " even=" << counters.get("even") << '\n';
    // 400000 and 400000, every time.

    Account a(1000), b(1000);
    std::thread t1([&] { for (int i = 0; i < 10000; ++i) transfer(a, b, 1); });
    std::thread t2([&] { for (int i = 0; i < 10000; ++i) transfer(b, a, 1); });
    t1.join(); t2.join();
    std::cout << "a=" << a.balance << " b=" << b.balance << " sum=" << a.balance + b.balance << '\n';

    std::vector<std::thread> loggers;
    for (int i = 0; i < 4; ++i)
        loggers.emplace_back([i] { for (int k = 0; k < 100; ++k) log_and_do_slow_work("t" + std::to_string(i)); });
    for (auto& t : loggers) t.join();
    std::cout << "log lines: " << log_lines.size() << '\n';   // 400
    return 0;
}
```

The `// GUARDED_BY(m_)` comment is a convention worth adopting: it tells the reader which mutex
protects which field. Clang's thread-safety analysis can check it mechanically if you use the real
attributes (`__attribute__((guarded_by(m_)))`), which is worth knowing exists even if you build
with GCC.

## 4. Under the hood (senior-level)

### What `std::mutex::lock()` does on Linux (glibc, x86-64)

`std::mutex` wraps a `pthread_mutex_t`. The default (`PTHREAD_MUTEX_NORMAL`) lock path:

1. **Fast path, user mode.** Atomic compare-and-swap on the 32-bit lock word: 0 (unlocked) to 1
   (locked, no waiters). One `lock cmpxchg` instruction, roughly 20 ns uncontended (typical,
   depends on whether the cache line is local). No syscall. This is why uncontended mutexes are
   cheap and why "I replaced the mutex with an atomic for speed" rarely helps.
2. **Contended path.** CAS failed. Set the word to 2 (locked, waiters present) with an atomic
   exchange, then call the `futex(addr, FUTEX_WAIT, 2)` syscall. The kernel checks that `*addr`
   is still 2 (if not, the lock was released in between and it returns immediately: this check is
   what prevents a lost wakeup) and then puts the thread to sleep on a hash bucket keyed by the
   address.
3. **Unlock.** Atomic exchange the word to 0. If the old value was 2 (there were waiters), call
   `futex(addr, FUTEX_WAKE, 1)` to wake one. If the old value was 1, nobody is waiting, no
   syscall. So an uncontended lock/unlock pair is two atomic instructions and zero syscalls.

A **futex** ("fast userspace mutex") is just that: a kernel wait queue addressed by a user-space
memory address, with an atomic "sleep only if the value is still X" check. It is the primitive
under every glibc lock, condition variable, semaphore and `pthread_join`. Windows has the same
thing since Windows 8: `WaitOnAddress` / `WakeByAddressSingle`, and `SRWLOCK` and
`CRITICAL_SECTION` (on recent Windows) are built on it.

### What `CRITICAL_SECTION` does

The same shape: an `InterlockedCompareExchange` on `LockCount` for the fast path. On contention,
optionally spin `SpinCount` times re-trying the CAS (useful on multi-core because the owner is
probably about to release), then wait on an auto-reset Event (older Windows) or via a keyed event /
`WaitOnAddress` (newer). `RecursionCount` and `OwningThread` implement the recursion. The
`SpinCount` is why `InitializeCriticalSectionAndSpinCount` exists; glibc's default mutex does not
spin, `PTHREAD_MUTEX_ADAPTIVE_NP` does, and `std::mutex` gives you no knob (one of its gaps).

### Why `lock_guard` has no runtime cost

The constructor and destructor are inline; after optimisation the only instructions left are the
`lock()` and `unlock()` calls. The "cost" of RAII is zero; what you gain is that the unlock is
emitted on every return path and in the exception-unwinding landing pad, which the compiler
generates for you.

### The cost of contention is not the lock, it is the sleep

An uncontended lock is tens of nanoseconds. A contended lock that has to futex-wait costs two
context switches (sleep, wake) plus a cold cache on return: tens of microseconds. A lock that is
contended 1% of the time at 1 µs each and uncontended 99% at 20 ns spends over 80% of its total
lock time in the 1%. This is why reducing hold time matters more than reducing lock count.

### Lock convoys

If many threads block on a mutex and the owner releases it, the kernel wakes one waiter, but a
*running* thread may grab the mutex before the woken one is scheduled (glibc mutexes are not
fair). The woken thread goes back to sleep; another context switch wasted. Under heavy contention
this cascades into a convoy where throughput falls below single-threaded. Symptom: many threads
in `futex_wait`, CPU not saturated, throughput poor. Fix: shorter critical sections, or sharding
the lock, or a different structure (module 10 and 11).

## 5. In production

- **Your `LeaveCriticalSection` bugs are gone.** Every C critical section that has an early
  `return` or a `goto cleanup` is a place where a forgotten unlock hung the server. RAII removes
  the class of bug. When you describe your synchronisation layer in interviews, say that moving it
  to C++ would make the unlock structural rather than disciplinary.
- **The mutex must have the same lifetime as the data.** A `std::mutex` that is a member of the
  class it protects is destroyed with the class. If another thread is still inside `lock()`, that
  is UB. The usual cause: a connection object deleted by the acceptor thread while a worker is
  still finishing its request. Fix: `shared_ptr` ownership or an explicit "drain before destroy"
  protocol (module 09 shutdown).
- **Lock across the database call.** The classic mistake: lock the cache mutex, miss, query
  YugabyteDB (20 ms), fill the cache, unlock. Every other request on the server now waits 20 ms
  behind the miss. Fix: unlock, query, re-lock, insert (and accept that two threads might both
  miss and both insert; last write wins and that is fine for a cache).
- **`recursive_mutex` as a porting crutch.** When porting `CRITICAL_SECTION` code, the temptation
  is `std::recursive_mutex` everywhere because CS was recursive. Resist; find the re-entrant
  calls (they are the ones that would deadlock with `std::mutex`) and split them into
  `foo()` / `foo_locked()`. Each one you find is a place where the old code's invariants were
  weaker than anyone thought.
- **Timed locks as a circuit breaker.** A worker that times out on a lock, logs the lock name,
  the thread id and the owner (if you record it), and fails the request, turns a silent hang into
  a diagnosable event.
- **3 AM symptom:** all 64 workers show `futex_wait` (Linux) or `WaitForSingleObject` inside
  `RtlEnterCriticalSection` (Windows) in their stacks. One of them holds the lock and is blocked
  on something else. Module 06 and 12 show how to find that one.

## 6. Try it

1. **(easy)** Build and run `m03_mutex_counter.cpp`. Remove the `lock_guard` from `increment` and
   confirm the totals are wrong. Put it back. Done when totals are 400000 both ways.
2. **(easy)** Replace `std::scoped_lock lk(from.m, to.m)` with two sequential `lock_guard`s in
   argument order. Run the `transfer` test. It will probably deadlock (hang). Attach gdb
   (`gdb -p PID`, `thread apply all bt`) and find both threads inside `lock`. Done when you have
   the two stacks and can say which thread holds which mutex.
3. **(medium)** Write a `foo()` that locks `m` and calls `bar()`, which also locks `m`. With
   `std::mutex` this hangs. Fix it the right way: `bar_locked()` that assumes the lock. Then
   write one paragraph on why `std::recursive_mutex` would have been the wrong fix. Done when the
   paragraph mentions invariants at intermediate depth.
4. **(medium)** Measure uncontended lock cost: 10 million `lock_guard` acquisitions on one thread,
   timed with `steady_clock`. Then measure contended: two threads, each doing 10 million, same
   mutex. Report ns per lock in both cases. Done when you can explain the ratio in terms of futex
   calls and context switches.
5. **(hard)** Implement `class ConfigStore` with `get(key)` and `reload(path)` where `reload`
   reads a file (slow). Version A holds the mutex during the file read. Version B reads the file
   into a local map first, then swaps under the lock. Run 8 reader threads and 1 reloader; measure
   reader p99 latency (keep a vector of per-call durations, sort, take index 0.99 * n). Done when
   version B's p99 is at least an order of magnitude lower and you can explain why.

## 7. Interview questions

**Q: What is the difference between `lock_guard`, `unique_lock` and `scoped_lock`?**
`lock_guard` locks in the constructor and unlocks in the destructor, nothing else, zero overhead.
`unique_lock` adds deferred locking, manual unlock and relock, try and timed lock, move, and is
required by `condition_variable::wait`. `scoped_lock` (C++17) locks any number of mutexes with the
deadlock-avoiding `std::lock` algorithm and should be the default for new code.

**Q: How does `std::lock` or `scoped_lock` avoid deadlock with multiple mutexes?**
It locks the first, then `try_lock`s the rest. If any fails it releases everything, yields, and
retries starting with the one that failed. A thread never blocks while holding another of the set,
so the hold-and-wait condition for deadlock is never met.

**Q: Why avoid `recursive_mutex`?**
Recursion hides that a function is reached both with and without the lock, so the invariant
"consistent when unlocked" weakens to "consistent at depth zero" and inner calls may see partial
state. It also cannot be used correctly with condition variables and costs an owner check per
lock. Split into `f()` and `f_locked()` instead.

**Q: What is a futex?**
A Linux syscall providing a kernel wait queue keyed by a user-space address, with an atomic check
"sleep only if `*addr` still equals this value". It lets a mutex take and release the lock in
user space with a CAS and only enter the kernel when a thread must actually sleep or be woken.
Windows has `WaitOnAddress` as the equivalent.

**Q: How does `std::mutex` compare with `CRITICAL_SECTION` and with a Win32 Mutex handle?**
`std::mutex` and `CRITICAL_SECTION` are both intra-process locks with a user-mode atomic fast path
and a kernel wait on contention; the differences are that `CRITICAL_SECTION` is recursive and can
spin. A Win32 Mutex handle is a kernel object: every acquire is a syscall, it works across
processes, it can be waited on with `WaitForMultipleObjects`, and it reports abandonment.

**Q: What is a lock convoy?**
A pattern where a mutex is released and a waiter is woken, but a running thread grabs the mutex
first, so the woken thread sleeps again. Each handoff wastes context switches; throughput can fall
below single-threaded. It appears as many threads blocked in the lock with low CPU use. Shorter
critical sections and sharding are the fixes.

**Q: Should a mutex ever be held across a database call?**
No. The hold time becomes the database latency, and every other thread that needs the mutex waits
that long. Fetch outside the lock, then lock briefly to insert or swap. Accept duplicate work on a
miss rather than serialising all requests behind one.

## 8. Recap

- A mutex gives one thread at a time exclusive access; `std::mutex` is the C++ `CRITICAL_SECTION`
  minus recursion.
- Always lock through an RAII wrapper; the unlock then runs on every return and on exceptions.
- `lock_guard` for simple scopes, `unique_lock` for condition variables and early unlock,
  `scoped_lock` for multiple mutexes and as the default in C++17.
- `std::lock` avoids deadlock by lock-one, try-the-rest, back off.
- Avoid `recursive_mutex`; split functions into locked and unlocked versions.
- Timed locks turn hangs into diagnosable failures.
- Uncontended lock: one CAS, ~20 ns. Contended: futex syscall, context switch, cold cache,
  microseconds. Hold time is what matters.
- Never hold a lock across I/O, a database call, or a callback.
- Futex: kernel wait queue by address with an atomic value check; `WaitOnAddress` on Windows.
- Document which mutex guards which field; `GUARDED_BY` comments at minimum.
