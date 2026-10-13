# 04. shared_mutex and readers-writers

**Time:** ~2 hours   **Prerequisites:** 03

## 1. In plain words

Most shared data in a server is read far more often than it is written. A master table of branch
codes is read on every request and refreshed once an hour. With a plain mutex, two threads that
only want to *read* the table still take turns, even though neither would disturb the other. A
reader-writer lock fixes that: any number of readers may hold it at once, but a writer needs it
alone. `std::shared_mutex` is the C++17 name; `SRWLOCK` is the Win32 name you have seen;
`pthread_rwlock_t` is the POSIX one.

Analogy: a museum room. Any number of visitors can be inside looking at the paintings. When the
curator wants to rehang a painting, everyone must leave and nobody may enter until the curator is
done. The guard at the door counts visitors in and out and holds newcomers back when the curator is
waiting.

The catch, which this module spends half its time on, is that the guard's counting is itself a
shared write. For very short reads, the cost of counting readers in and out can exceed the cost
of a plain mutex, and a plain mutex wins. "Use shared_mutex for read-mostly data" is the beginner
rule. "Measure, because the reader count is contended too" is the senior one.

## 2. The real thing

### 2.1 The API

```cpp
#include <shared_mutex>

std::shared_mutex m;

// writer: exclusive
{ std::unique_lock<std::shared_mutex> lk(m);  /* or std::lock_guard, or std::scoped_lock */ }

// reader: shared
{ std::shared_lock<std::shared_mutex> lk(m); }
```

Member functions: `lock / try_lock / unlock` (exclusive) and `lock_shared / try_lock_shared /
unlock_shared` (shared). `std::shared_lock` is the RAII wrapper for the shared side; it has the
same interface as `unique_lock` (defer, try, move, unlock early). `std::shared_timed_mutex`
(C++14) adds `try_lock_for` and `try_lock_shared_for`; `std::shared_mutex` (C++17) drops the timed
operations and is usually a thinner wrapper over the platform primitive.

Rules:

- A thread holding a shared lock must not request the exclusive lock (UB; deadlock in practice).
  There is no standard "upgrade" operation. If you need read-then-maybe-write, release the shared
  lock, take the exclusive lock, and re-check the condition, because another writer may have run
  in between.
- A thread holding the exclusive lock may not take the shared lock either.
- Same single-owner rules as `std::mutex`: unlock from the locking thread, no recursion.

### 2.2 Mapping

| Win32 | POSIX | C++17 |
|---|---|---|
| `SRWLOCK srw = SRWLOCK_INIT;` | `pthread_rwlock_t` | `std::shared_mutex m;` |
| `AcquireSRWLockExclusive(&srw)` | `pthread_rwlock_wrlock` | `m.lock()` / `unique_lock` |
| `ReleaseSRWLockExclusive(&srw)` | `pthread_rwlock_unlock` | `m.unlock()` |
| `AcquireSRWLockShared(&srw)` | `pthread_rwlock_rdlock` | `m.lock_shared()` / `shared_lock` |
| `ReleaseSRWLockShared(&srw)` | `pthread_rwlock_unlock` | `m.unlock_shared()` |
| `TryAcquireSRWLockExclusive` | `pthread_rwlock_trywrlock` | `m.try_lock()` |

`SRWLOCK` is pointer-sized, non-recursive, cannot be upgraded, and is faster than
`CRITICAL_SECTION` even in exclusive-only use on modern Windows. If you have a `CRITICAL_SECTION`
protecting read-mostly data, `SRWLOCK` is the drop-in improvement, and `std::shared_mutex` is how
you describe it portably.

### 2.3 Reader-writer trade-offs

When shared_mutex helps:

- Read sections are **long** relative to the lock overhead (say, a `std::map` lookup with string
  compares, or walking a few hundred entries), **and**
- readers greatly outnumber writers, **and**
- there are enough cores that readers actually run in parallel.

When a plain `std::mutex` is as fast or faster:

- Read sections are **tiny** (a single integer load, an unordered_map lookup on a small key). The
  shared lock still does an atomic increment on acquire and an atomic decrement on release; both
  hit the same cache line from every reading core. That cache line bounces between cores exactly
  as a mutex's lock word would. You pay the contention cost without the exclusivity benefit.
- Low core count. Two readers on two cores gain little; the bookkeeping dominates.
- Writers are frequent. Every writer must wait for all readers to drain, and readers arriving
  during that drain must wait too, so under write pressure a rwlock behaves like a mutex with
  extra steps.

Measured on typical x86-64 Linux hardware, the crossover is on the order of a few hundred
nanoseconds of read work: below that a mutex wins, above it shared_mutex wins (this is the kind of
number you must re-measure on your own machine; see exercise 4). For a cache of immutable
snapshots (module 11) you can eliminate the reader lock entirely, which beats both.

### 2.4 Writer starvation and fairness

If readers arrive continuously, a naive rwlock that admits any reader whenever no writer *holds*
the lock will never let a writer in: the reader count never reaches zero. That is **writer
starvation**. Implementations handle it in one of two ways:

- **Writer preference:** once a writer is waiting, new readers block even though current readers
  are still inside. The writer gets in as soon as current readers drain. glibc's
  `pthread_rwlock` can be configured for this (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`);
  its default prefers readers. libstdc++'s `std::shared_mutex` on Linux wraps `pthread_rwlock_t`
  with default attributes, so by default it is **reader-preferring and can starve writers**.
  `SRWLOCK` on Windows does not document a policy but behaves roughly fair in practice.
- **Fair / FIFO:** readers and writers are queued in arrival order; a batch of adjacent readers
  runs together. Fair, no starvation, slightly lower reader throughput.

Practical consequence: in a server with a steady stream of reads, a writer on a default libstdc++
`shared_mutex` may wait seconds. If the writer is your five-minute master-table refresh, the data
goes stale. Know your implementation's policy or design around it (snapshot swap, module 11).

### 2.5 The tcpkv store design

Your tcpkv project: a TCP key-value server, fixed thread pool, store guarded by `shared_mutex`.
The portable, defensible design:

```cpp
class Store {
    mutable std::shared_mutex m_;
    std::unordered_map<std::string, std::string> map_;   // GUARDED_BY(m_)
public:
    std::optional<std::string> get(const std::string& k) const {
        std::shared_lock lk(m_);              // many GETs in parallel
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;                    // copy out while holding the lock
    }
    void set(std::string k, std::string v) {
        std::unique_lock lk(m_);              // SET excludes everyone
        map_[std::move(k)] = std::move(v);
    }
    bool del(const std::string& k) {
        std::unique_lock lk(m_);
        return map_.erase(k) > 0;
    }
};
```

Why `get` returns a copy and not a reference or pointer: the moment the shared lock is released,
a `set` may rehash the map and invalidate every reference into it. Returning `const std::string&`
from a function that releases the lock on return is a use-after-free that only shows under load.
Returning `std::optional<std::string>` (a copy) is correct and the copy is cheap compared to the
TCP write that follows.

Why `std::optional` and not an empty string: an empty string is a valid value. The protocol must
distinguish "key absent" from "key present with empty value".

What to say when asked "why shared_mutex here?": GETs dominate in a KV workload, the read section
is a hash lookup plus a string copy (hundreds of ns), and the pool has 8 threads on 8 cores, so
parallel reads are real. What to say when asked "what would you do next?": shard the map into 16
or 64 buckets each with its own `shared_mutex` keyed by `hash(key) % N`, which cuts contention on
both reads and writes, and consider making the read path lock-free with an atomic snapshot if
writes are rare.

## 3. Code

File: `code/m04_shared_mutex_map.cpp`

```cpp
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <string>
#include <optional>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>

class Store {
    mutable std::shared_mutex m_;
    std::unordered_map<std::string, std::string> map_;   // GUARDED_BY(m_)
public:
    std::optional<std::string> get(const std::string& k) const {
        std::shared_lock<std::shared_mutex> lk(m_);
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;                 // WHY copy: references die when lk is released
    }
    void set(std::string k, std::string v) {
        std::unique_lock<std::shared_mutex> lk(m_);
        map_[std::move(k)] = std::move(v);
    }
    bool del(const std::string& k) {
        std::unique_lock<std::shared_mutex> lk(m_);
        return map_.erase(k) > 0;
    }
    std::size_t size() const {
        std::shared_lock<std::shared_mutex> lk(m_);
        return map_.size();
    }
};

// A sharded variant: N independent stores, chosen by hash. Same interface.
template <std::size_t N>
class ShardedStore {
    Store shards_[N];
    static std::size_t idx(const std::string& k) { return std::hash<std::string>{}(k) % N; }
public:
    std::optional<std::string> get(const std::string& k) const { return shards_[idx(k)].get(k); }
    void set(std::string k, std::string v) {
        std::size_t i = idx(k);                 // compute before moving k
        shards_[i].set(std::move(k), std::move(v));
    }
    bool del(const std::string& k) { return shards_[idx(k)].del(k); }
};

template <class S>
double run_benchmark(S& store, int readers, int writers, std::chrono::milliseconds dur) {
    std::atomic<bool> stop{false};
    std::atomic<long> ops{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 1000; ++i) store.set("key" + std::to_string(i), "value");

    for (int r = 0; r < readers; ++r)
        ts.emplace_back([&] {
            long local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 1000; ++i) { (void)store.get("key" + std::to_string(i % 1000)); ++local; }
            }
            ops.fetch_add(local, std::memory_order_relaxed);   // WHY: one atomic add per thread, not per op
        });
    for (int w = 0; w < writers; ++w)
        ts.emplace_back([&] {
            long local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                store.set("key" + std::to_string(local % 1000), "v2"); ++local;
            }
            ops.fetch_add(local, std::memory_order_relaxed);
        });

    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    return static_cast<double>(ops.load()) / dur.count() / 1000.0;   // million ops per second
}

int main() {
    using namespace std::chrono_literals;
    Store s;
    ShardedStore<16> sh;
    std::cout << "shared_mutex, 8 readers 0 writers: " << run_benchmark(s, 8, 0, 500ms) << " Mops/s\n";
    std::cout << "shared_mutex, 8 readers 2 writers: " << run_benchmark(s, 8, 2, 500ms) << " Mops/s\n";
    std::cout << "sharded x16,  8 readers 2 writers: " << run_benchmark(sh, 8, 2, 500ms) << " Mops/s\n";

    // Correctness smoke test
    Store c;
    c.set("a", "");
    std::cout << "present-empty: " << (c.get("a").has_value() ? "yes" : "no")
              << ", absent: " << (c.get("zz").has_value() ? "yes" : "no") << '\n';
    return 0;
}
```

Note the benchmark constructs a `std::string` key per lookup; that allocation is part of the "read
work" and makes the read section long enough for `shared_mutex` to pay off. Exercise 4 asks you to
remove it and watch the result flip.

## 4. Under the hood (senior-level)

### glibc `pthread_rwlock_t` layout and the shared fast path

The lock holds a 32-bit `__readers` word with the reader count in the upper bits and two flag bits:
"writer present" and "writers waiting / phase". `rdlock` does a CAS loop: read the word, if no
writer flag, CAS in `count + 1`. `unlock` (shared) does an atomic `fetch_sub`. Every reader thus
performs two atomic read-modify-writes on one cache line. On a 2-socket machine the line travels
across the interconnect for each; that is 100 ns or more per operation (typical). A `std::mutex`
lock/unlock is also two atomics on one line, which is why for tiny read sections they cost the
same and the mutex, with less logic, often wins.

The writer: sets the writer flag with a CAS when the count is zero, otherwise futex-waits on the
word. Readers that see the writer flag futex-wait on a separate word. The reader/writer-preference
policy decides whether a reader arriving while a writer *waits* (not holds) increments the count
or blocks.

### Why `SRWLOCK` is fast

It is a single pointer-sized word. The low bits are flags (locked, waiting, multiple shared
holders) and the rest is either the shared count or a pointer to a wait block chain on the
waiters' stacks. Acquire is one `InterlockedCompareExchange`; release likewise. Waiters block via
keyed events (a kernel object addressed by a key, the Windows futex ancestor). No allocation, no
kernel object creation, which is why it can be statically initialised (`SRWLOCK_INIT`) and why
Windows's own `CONDITION_VARIABLE` is designed to pair with it.

### Reader scalability ceiling

Even with no writers, N readers on N cores do not get N times throughput from a single
`shared_mutex`, because the reader count line is contended. The ceiling is set by the cache
coherence latency of that one line. To scale reads linearly you must stop writing shared memory
on the read path: sharding (each core mostly touches its own shard's lock), per-core reader counts
(the "big reader lock" design from the Linux kernel, `brlock`), or no lock at all with immutable
snapshots and RCU (module 11).

### Upgrade and the lost-update race

Because there is no upgrade, this pattern is wrong:

```cpp
{ std::shared_lock lk(m); if (map.count(k)) return map[k]; }
{ std::unique_lock lk(m); map[k] = compute(); return map[k]; }   // another thread may have inserted
```

It is not a data race (every access is locked) but it is a logic race: two threads miss, both
compute, both insert. For a cache that is acceptable. For "insert if absent, else fail" it is a
bug: use `try_emplace` under the exclusive lock and check the returned bool. Boost offers
`upgrade_lock` which reserves the right to upgrade; the standard does not, because supporting
upgrade cheaply constrains the implementation.

## 5. In production

- **Master tables.** Your core-banking server's product codes, branch lists, interest matrices:
  read on every request, refreshed by a scheduled job. This is the textbook `SRWLOCK` /
  `shared_mutex` use. If you still have them under `CRITICAL_SECTION`, switching to `SRWLOCK` is
  a one-day change with a measurable p99 gain under concurrent load. The next step beyond that is
  the snapshot swap in module 11, which removes the reader lock entirely.
- **Writer starvation in the wild.** A refresh thread that logs "refresh took 14 s" while the
  table is 100 entries: it was waiting for a gap in readers that never came. Fix: snapshot swap
  (writer builds the new table unlocked, then takes the exclusive lock for a single pointer
  assignment) so the exclusive section is nanoseconds and readers barely notice.
- **Holding a shared lock across a call-out.** A reader that takes the shared lock and then calls
  into code that eventually tries the exclusive lock on the same mutex (a lazy-fill in a getter)
  deadlocks itself, and because it is a reader, every writer behind it is stuck too. The
  `_locked` naming convention from module 03 is how you spot this in review.
- **Returning references.** A `const T& get(key)` on a `shared_mutex`-guarded map is the most
  common serious bug in KV stores written by people who have just learned `shared_lock`. It is a
  use-after-free that passes every single-threaded test.
- **3 AM symptom:** throughput drops to near zero, all worker stacks show `pthread_rwlock_rdlock`
  or `AcquireSRWLockShared`. A writer holds the lock and is doing something slow inside it (the
  refresh job calling the database under the exclusive lock). Find the one thread in `wrlock`'s
  critical section and look at what *it* is waiting on.

## 6. Try it

1. **(easy)** Build and run `m04_shared_mutex_map.cpp`. Note the three throughput numbers. Done
   when the sharded store beats the single store with writers present.
2. **(easy)** Change `get` to return `const std::string*` pointing into the map. Run with
   `-fsanitize=address` and 8 readers plus 2 writers that `del` and re-`set`. Done when ASan
   reports a heap-use-after-free and you can explain the lifetime problem in one sentence.
3. **(medium)** Replace `std::shared_mutex` with `std::mutex` (and `shared_lock` with
   `lock_guard`) in `Store`. Re-run the 8-reader / 0-writer benchmark. Done when you have both
   numbers and a sentence on which won and why.
4. **(medium)** Make the read section tiny: pre-build the 1000 key strings in a vector and look
   them up by index so there is no per-lookup allocation. Re-run shared_mutex vs mutex. Done when
   you have found the point where the plain mutex is as fast or faster, or can explain why on your
   machine it never is.
5. **(hard)** Demonstrate writer starvation on libstdc++: 8 readers in a tight loop of
   `shared_lock`, one writer that measures how long `unique_lock` takes to acquire. Report max
   wait over 10 seconds. Then implement a small fair rwlock (mutex + condition_variable + counts +
   "writer waiting" flag that blocks new readers) and show the writer's max wait drops. Done when
   both numbers are recorded and your rwlock passes TSan.

## 7. Interview questions

**Q: When is `shared_mutex` faster than `mutex`, and when is it not?**
Faster when read sections are long enough (hundreds of nanoseconds or more) that running them in
parallel outweighs the cost of the atomic reader-count increment and decrement, readers dominate,
and there are enough cores. Not faster, often slower, when reads are a few instructions, because
the reader count is a contended cache line just like a mutex's lock word, or when writes are
frequent and keep draining readers.

**Q: What is writer starvation and how do implementations avoid it?**
With continuous readers, a reader-preferring rwlock never reaches a zero reader count and a writer
waits indefinitely. Writer-preferring locks block new readers once a writer is waiting; fair locks
queue in arrival order. glibc defaults to reader preference, so libstdc++ `shared_mutex` can
starve writers unless the writer's exclusive section is kept to a pointer swap.

**Q: Can you upgrade a shared lock to exclusive?**
Not with `std::shared_mutex`; attempting to lock exclusively while holding shared is UB. Release
the shared lock, acquire exclusive, and re-check the condition because another writer may have
intervened. Boost's `upgrade_lock` provides a reservation mechanism; the standard omitted it.

**Q: Why must `get` on a `shared_mutex`-guarded map return a copy?**
A reference or pointer into the map is valid only while the lock is held. After `get` returns and
releases the shared lock, a writer may erase or rehash and the caller holds a dangling reference.
Copying under the lock (or returning a `shared_ptr` to an immutable value) is the only safe
interface.

**Q: Describe the tcpkv store and what you would do to scale it.**
An `unordered_map` behind a `shared_mutex`; GET takes a shared lock and copies the value out, SET
and DEL take exclusive. To scale: shard by key hash into N independent maps each with its own
lock, so both readers and writers of different shards do not contend; beyond that, make reads
lock-free with immutable snapshots if writes are rare.

**Q: What is `SRWLOCK` and how does it relate to `shared_mutex`?**
Windows's slim reader-writer lock: a pointer-sized, non-recursive, non-upgradable lock with
shared and exclusive modes, acquired with one interlocked operation and waited on via keyed
events. It is the direct equivalent of `std::shared_mutex` and faster than `CRITICAL_SECTION` even
in exclusive-only use.

## 8. Recap

- `shared_mutex`: many readers or one writer. `shared_lock` for readers, `unique_lock` or
  `scoped_lock` for writers.
- Direct equivalents: `SRWLOCK` on Windows, `pthread_rwlock_t` on POSIX.
- The reader count is a contended atomic; tiny read sections are no faster than with a mutex.
- Writer starvation is real under reader preference, which is libstdc++'s default on Linux.
- No upgrade; release, re-acquire exclusive, re-check.
- Never return references into locked data; copy under the lock.
- Shard to scale both reads and writes; snapshot swap to make reads lock-free.
- Keep the exclusive section to a swap; build the new state outside the lock.
