# 11. Concurrency patterns for servers

**Time:** ~3 hours   **Prerequisites:** 04, 05, 07, 09

## 1. In plain words

The primitives from modules 03 to 09 are bricks. This module is about the buildings: the handful of
shapes that every production server takes, and how to choose between them. How do you map
connections to threads? How do you let two hundred threads read a master table that changes every
five minutes without a lock on every read? How do you initialise a shared object exactly once when
ten threads ask for it at the same moment? How do you hand out database connections to workers
without exhausting the database?

Analogy: a bank branch. Thread-per-connection is one teller per customer, hired when the customer
walks in and fired when they leave. A thread pool is a fixed number of tellers and a queue. An
event loop is one very fast teller who never waits: when a customer needs a form fetched from the
back office, the teller takes the next customer and comes back when the form arrives. Each shape
is right for a different branch.

Your server is the pool shape. Your master-table cache is the immutable-snapshot shape. Your
database connections are the pool-of-resources shape. This module names them, shows their
portable C++17 form, and tells you what to say when asked why you chose each.

## 2. The real thing

### 2.1 Mapping connections to threads

**Thread per connection.** Accept, spawn a thread, that thread does blocking reads and writes for
the life of the connection. Simple code: the handler reads like a single-threaded program.
Cost: one thread (stack, kernel structures, context switches) per connection, unbounded, and
mostly asleep. Fine to around a few hundred connections. Falls over at thousands.

**Thread pool with blocking I/O** (your server). Acceptor thread accepts and reads a request,
pushes it to the pool; a worker processes it (including the database call) and writes the reply.
Bounded threads, good for request/response protocols where each request is short and connections
are many but mostly idle. Weakness: a worker blocked on a slow database call is a worker not
serving anyone; sizing is a guess (module 01). Still the right default for a transactional
backend with a few hundred to a few thousand connections.

**Event loop (reactor).** One thread (or one per core) calls `epoll_wait` (Linux), `kqueue` (BSD),
or an I/O completion port (Windows) on thousands of sockets at once. When a socket is readable it
reads without blocking, parses, and either replies immediately or hands CPU-heavy or blocking work
to a pool. No thread ever blocks on a socket. Handles 100,000 connections on one box. Cost: the
handler is a state machine, not a straight-line program; any blocking call in the loop stalls
every connection. nginx, Redis, Node.js and every modern proxy are this shape. 08_Networking
builds one.

**Hybrid (the modern default).** N event-loop threads (one per core) for all I/O plus a separate
pool for blocking work (database calls, disk). Requests move between them via queues. This is what
you would build if you rewrote your server today, and it is the answer to "how would you scale it
to 50,000 connections".

Decision table:

| Connections | Request type | Shape |
|---|---|---|
| < 500, long-lived | anything | thread per connection is acceptable |
| 500 to 5,000 | short request/response, blocking DB | pool (your server) |
| > 5,000 or mostly idle | anything | event loop + pool for blocking work |
| any | CPU-bound compute per request | pool sized to cores |

### 2.2 Actors and message passing

An actor is a thread (or a task on a pool) that owns some state exclusively and communicates only
through a queue of messages. Nobody else touches its state, so it needs no locks. The queue is a
`BoundedQueue<Message>` from module 05 and the thread is `while (auto m = inbox.pop()) handle(*m);`.
This is what a "logger thread", a "stats aggregator thread", a "connection manager thread" each
are. Benefits: no shared-state races, no lock ordering because there are no locks, each actor is
testable alone. Costs: every interaction is a copy or a move into a queue, latency of one queue
hop per interaction, and a request that needs state from three actors becomes three messages and
a correlation id.

Rule of thumb: state that is *owned* by one logical component and mutated frequently is a good
actor. State that many components read and few write is a snapshot (next section). State that many
components mutate is a lock, sharded if hot.

### 2.3 Immutable snapshots and read-copy-update

Your master-table cache: product codes, branch tables, rate matrices. Read on every request by
every worker. Refreshed every five minutes by one thread. The reads vastly outnumber the writes
and the data is small enough to copy.

Design: the cache holds a single `std::shared_ptr<const Table>`. Readers atomically load the
pointer, use the table, and drop the pointer. The refresher builds a brand-new `Table` off to the
side (unlocked, as slow as it likes), then atomically swaps the pointer. Readers who already hold
the old pointer keep using the old table; when the last of them drops it, the `shared_ptr`
deletes it. New readers get the new table. No reader ever blocks, no reader ever sees a
half-updated table, and the writer's critical section is one pointer store.

```cpp
class SnapshotCache {
    std::shared_ptr<const Table> current_;   // accessed only via the atomic free functions
public:
    std::shared_ptr<const Table> get() const {
        return std::atomic_load_explicit(&current_, std::memory_order_acquire);   // C++17 form
    }
    void publish(std::shared_ptr<const Table> next) {
        std::atomic_store_explicit(&current_, std::move(next), std::memory_order_release);
    }
};
```

C++17 provides `std::atomic_load` / `std::atomic_store` free functions overloaded for
`std::shared_ptr`. C++20 replaces them with `std::atomic<std::shared_ptr<T>>`. The acquire on load
and release on store are the module 07 publish pattern: the reader who sees the new pointer sees
the fully built table behind it.

Why not `std::shared_mutex`? Because even a shared lock is two atomic RMWs on one line per read
(module 04); with 200 readers that line is hot. The snapshot read is one atomic RMW (the
`shared_ptr` copy increments the control block's count) plus one at release. Still a shared line,
but with no writer ever excluding readers, no starvation, and a refresh that never blocks a
request. To eliminate even that count traffic, use RCU proper.

**RCU (read-copy-update)**, the Linux kernel's version: readers mark "I am reading" with a
per-CPU, cache-local operation (no shared line at all) and the writer, after swapping the pointer,
waits for a "grace period": a point at which every CPU has passed through a quiescent state since
the swap, guaranteeing no reader can still hold the old pointer. Then it frees the old copy. In
user space, `liburcu` implements it; a simpler approach that captures most of the benefit is
**epoch-based reclamation**: a global epoch counter, each reader records the current epoch on
entry in its own padded slot, the writer retires old tables tagged with the epoch and frees them
when every reader's recorded epoch is past it. `shared_ptr` swapping is "RCU with reference
counting as the grace period" and is the pragmatic choice unless you are at millions of reads per
second.

Costs and constraints of snapshots: the whole table is copied on every refresh (fine for
megabytes every five minutes, wrong for a gigabyte every second); there is a window where two
copies live; a reader that holds a snapshot for a long time holds the old memory. For a cache with
frequent small updates (a session table), this is the wrong shape and a sharded `shared_mutex`
map is right.

### 2.4 Double-checked locking and `std::call_once`

The problem: initialise a shared object lazily, once, on first use, from any thread, cheaply after
the first time. The naive double-checked pattern:

```cpp
Table* instance = nullptr;   // plain pointer: BROKEN
std::mutex m;
Table* get() {
    if (!instance) {                       // unsynchronised read: data race with the write below
        std::lock_guard<std::mutex> lk(m);
        if (!instance) instance = new Table();   // the compiler/CPU may publish the pointer
    }                                             // before the Table's constructor completes
    return instance;
}
```

Two bugs: the first read is a data race (UB), and even if it were atomic, a relaxed read could see
the pointer before the pointed-to object's fields. Correct versions:

```cpp
// 1. Atomic pointer with acquire/release.
std::atomic<Table*> instance{nullptr};
Table* get() {
    Table* p = instance.load(std::memory_order_acquire);
    if (!p) {
        std::lock_guard<std::mutex> lk(m);
        p = instance.load(std::memory_order_relaxed);   // under the lock: ordered by the mutex
        if (!p) { p = new Table(); instance.store(p, std::memory_order_release); }
    }
    return p;
}

// 2. std::call_once: the standard does the above for you.
std::once_flag flag;
Table* instance2 = nullptr;
Table* get2() {
    std::call_once(flag, [] { instance2 = new Table(); });   // exactly once; others block until done
    return instance2;                                        // call_once provides the happens-before
}

// 3. Function-local static (C++11 "magic static"): the compiler emits the call_once for you.
Table& get3() {
    static Table t;    // thread-safe initialisation guaranteed by [stmt.dcl]; one guard check after
    return t;
}
```

Use 3 when the object can be a static with a fixed lifetime (most singletons). Use 2 when the
initialisation needs arguments or the storage is a member. Use 1 only when you need to replace the
pointer later as well (then it is the snapshot pattern). If the initialiser throws inside
`call_once`, the flag stays unset and the next caller retries; a thrown exception from a magic
static's constructor does the same.

Win32 equivalent: `InitOnceExecuteOnce` with an `INIT_ONCE` structure (Vista+), which is exactly
`call_once`.

### 2.5 Connection pools

A database allows a limited number of connections, and opening one costs a TCP handshake, TLS,
and authentication (milliseconds). A pool of pre-opened connections handed out to workers is a
resource pool: a bounded set, a mutex, a condition variable to wait when all are out, and an RAII
handle that returns the connection on destruction.

```cpp
class ConnectionPool {
    std::mutex m_;
    std::condition_variable available_;
    std::vector<std::unique_ptr<Conn>> idle_;   // GUARDED_BY(m_)
public:
    class Lease {   // RAII: returns the connection to the pool in its destructor
        ConnectionPool* pool_; std::unique_ptr<Conn> c_;
    public:
        Lease(ConnectionPool* p, std::unique_ptr<Conn> c) : pool_(p), c_(std::move(c)) {}
        ~Lease() { if (c_) pool_->give_back(std::move(c_)); }
        Lease(Lease&&) = default;
        Lease(const Lease&) = delete;
        Conn& operator*() { return *c_; }
    };
    Lease acquire(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        if (!available_.wait_for(lk, timeout, [&] { return !idle_.empty(); }))
            throw std::runtime_error("connection pool exhausted");   // WHY throw: fail fast, do not hang
        auto c = std::move(idle_.back()); idle_.pop_back();
        return Lease(this, std::move(c));
    }
private:
    void give_back(std::unique_ptr<Conn> c) {
        { std::lock_guard<std::mutex> lk(m_); idle_.push_back(std::move(c)); }
        available_.notify_one();
    }
};
```

Production additions: health check on give-back (discard a connection whose last query failed
with a connection error), maximum age (recycle connections older than an hour to pick up DNS and
failover changes), a timeout on acquire that is shorter than the client's request timeout, and
metrics for in-use count and wait time. Pool size: at most the database's limit divided by the
number of server instances, and often fewer; a database is rarely faster with 200 connections than
with 32.

Rule interaction with module 06: never hold a process mutex while holding a connection (lock order
puts the DB pool below everything but leaf locks), and never hold a connection while waiting for a
pool lease (two leases per worker with a pool smaller than 2 x workers deadlocks).

### 2.6 Choosing

| Problem | Shape |
|---|---|
| many connections, short requests, blocking DB | acceptor + pool (module 09) |
| 10,000+ connections | event loop + pool |
| state owned by one component, mutated often | actor with a queue |
| read-mostly data, periodic refresh | immutable snapshot, atomic `shared_ptr` swap |
| read-mostly, frequent small updates | sharded `shared_mutex` map |
| lazy one-time initialisation | magic static or `call_once` |
| limited external resources | RAII-leased pool with timeout |
| hot counter | striped per-thread atomics (module 10) |

## 3. Code

File: `code/m11_call_once_singleton.cpp` — the three correct forms and a stress test that proves
exactly one construction under 16 threads.

File: `code/m11_rcu_snapshot_cache.cpp` — the snapshot cache with 8 readers and 1 refresher,
verifying every reader always sees a consistent table (a checksum stored in the table matches its
contents), while the refresher's publish never blocks.

Key part of the snapshot cache:

```cpp
struct Table {
    std::vector<long> rates;
    long checksum;                                  // consistency witness: sum of rates
    explicit Table(long version) : rates(1024), checksum(0) {
        for (std::size_t i = 0; i < rates.size(); ++i) { rates[i] = version * 1000 + static_cast<long>(i); checksum += rates[i]; }
    }
};

class SnapshotCache {
    std::shared_ptr<const Table> cur_;
public:
    explicit SnapshotCache(std::shared_ptr<const Table> t) : cur_(std::move(t)) {}
    std::shared_ptr<const Table> get() const {
        return std::atomic_load_explicit(&cur_, std::memory_order_acquire);   // WHY acquire: see the table's contents
    }
    void publish(std::shared_ptr<const Table> next) {
        std::atomic_store_explicit(&cur_, std::move(next), std::memory_order_release);
    }
};

// reader
auto t = cache.get();              // one atomic refcount increment; no lock
long s = 0; for (long r : t->rates) s += r;
if (s != t->checksum) ++inconsistent;   // never happens
// t goes out of scope: refcount decrement; last holder frees the old table

// refresher
auto next = std::make_shared<const Table>(version++);   // slow build, unlocked
cache.publish(std::move(next));                          // one pointer swap: sub-microsecond median
```

## 4. Under the hood (senior-level)

### `std::atomic_load(&shared_ptr)` is usually not lock-free

libstdc++ implements the `shared_ptr` atomic free functions with a small table of spinlocks keyed
by the address of the `shared_ptr` object (`_Sp_locker`). The load takes the spinlock, copies the
two pointers (object and control block), increments the control block's count, releases. So a
"lock-free" snapshot read is actually a spinlock hold of a few nanoseconds plus one `lock xadd`.
This is still far better than `shared_mutex` under heavy reads (the spinlock is held for ~4
instructions, never across anything slow) but it is not zero shared-line traffic. Two shared lines
are written per read: the spinlock word and the control block count. Epoch-based RCU brings that to
zero. C++20's `std::atomic<std::shared_ptr>` is implemented similarly in libstdc++; some
implementations use a 128-bit CAS.

### Why the release/acquire pair matters even with `shared_ptr`

`make_shared<const Table>(v)` runs the constructor, then the `publish` stores the pointer. Without
release on the store the compiler may reorder the table's field writes after the pointer store (it
cannot see that another thread will read through the pointer). Without acquire on the load, the
reader on ARM may see the pointer and stale table contents. The `shared_ptr` control block's own
atomics are about the count, not about your data. You need the explicit orders (or seq_cst).

### Magic static implementation

```cpp
static Table t;
```

compiles to a 64-bit guard variable, an inline check of its first byte (acquire load; "already
initialised" fast path, one instruction), and on the slow path a call to `__cxa_guard_acquire`,
which takes a mutex (glibc uses a futex-based lock per guard), re-checks, runs the constructor,
calls `__cxa_guard_release` (sets the byte with release semantics, wakes waiters). If the
constructor throws, `__cxa_guard_abort` resets the guard so the next caller tries again. Recursive
initialisation (the constructor calls `get3()`) deadlocks or throws; the standard says it is UB.

### `std::call_once` implementation

libstdc++'s `call_once` historically wrapped `pthread_once`, which has a well-known limitation:
an exception thrown from the initialiser could leave the once-control in a bad state on some
versions. Since GCC 11 libstdc++ uses its own futex-based implementation to handle exceptions
correctly per the standard. If you see `std::call_once` in a crash on an old toolchain, that is the
history. For new code on GCC 13, both magic statics and `call_once` are correct.

### Grace periods in the kernel's RCU

Classic RCU readers do `rcu_read_lock()`, which merely disables preemption (so no shared memory is
written at all). The writer does `synchronize_rcu()`, which waits until every CPU has context
switched at least once (a quiescent state); since readers cannot be preempted inside a read-side
section, a context switch on every CPU proves no reader from before the swap is still running.
Read cost: zero. Write cost: milliseconds of waiting. User-space `liburcu` approximates this with
per-thread counters and a signal-based or periodic quiescent-state announcement. This is the end
state of "readers must not write shared memory"; the `shared_ptr` snapshot is the 90% solution at
1% of the complexity.

### Why actors avoid the lock-order problem

An actor never holds its own state's "lock" (it has none) while waiting for another actor; it
sends a message and continues or returns to its queue. Hold-and-wait (module 06) cannot occur. The
failure mode instead is a full mailbox: actor A's queue is bounded, A is blocked sending to B, B is
blocked sending to A. That is a distributed deadlock across queues, and the fix is the same as for
locks: an ordering (A never blocks sending to B if B may block sending to A) or unbounded mailboxes
with a drop policy.

## 5. In production

- **Describing your server.** "Acceptor thread, fixed worker pool, bounded request queue, blocking
  database calls in the worker. Master tables in an immutable snapshot cache refreshed on a timer
  thread. Database connections leased from a pool with a timeout. Statistics in per-worker
  striped counters." Every noun in that sentence is a module in this course, and that is the
  description that gets you to the system-design round.
- **The refresh that locked the world.** A master-table refresh under the table's `CRITICAL_
  SECTION` took 400 ms (it re-queried the database inside the lock). Every request during those
  400 ms waited. Snapshot swap: build the new table from the query result first, then swap. The
  exclusive section shrinks from 400 ms to one pointer swap: under a microsecond typically,
  a few hundred microseconds to milliseconds in the worst case when more threads than cores
  fight over the library's internal spinlock (see exercise 2).
- **The cache that returned references.** `const Rate& lookup(code)` into a map that the refresh
  replaced. Dangling reference once per refresh under load. A snapshot `shared_ptr` held by the
  request for its lifetime makes this structurally impossible: the request's table cannot be freed
  while the request holds the pointer.
- **Lazy init that double-constructed.** A connection-string parser called from two threads at
  startup, both saw `nullptr`, both constructed, one leaked and the other's construction raced with
  the first's use. Magic static.
- **Pool exhaustion as a hang.** A worker that leaked a lease (forgot to return the connection on
  an error path) eventually drained the pool; every subsequent request blocked forever on acquire.
  RAII lease plus an acquire timeout turns a silent hang into a logged "pool exhausted" error
  with a stack.
- **3 AM symptom:** latency spike every five minutes. Timer-driven refresh under a lock. Snapshot
  swap fixes it, and the graph flattening is a satisfying before/after for your portfolio.

## 6. Try it

1. **(easy)** Build and run `m11_call_once_singleton.cpp`. Confirm "constructed: 1" for each of
   the three forms under 16 threads. Then write the broken double-checked version with a plain
   pointer and run it under TSan. Done when TSan reports the race.
2. **(easy)** Build and run `m11_rcu_snapshot_cache.cpp` twice: `./m11_rcu_snapshot_cache 3`
   and `./m11_rcu_snapshot_cache 8`. Confirm zero inconsistent reads both times. Compare the
   publish median and p99. On a 4-core laptop (measured on WSL, GCC 13) the median stays below a
   microsecond, but with 8 readers p99 jumps to milliseconds because readers outnumber cores and
   contend on libstdc++'s internal spinlock. Done when you can explain why the tail grows and the
   median does not.
3. **(medium)** Replace the snapshot cache with a `std::shared_mutex`-guarded table that the
   refresher rebuilds *in place* under the exclusive lock (the slow way). Measure reader p99
   latency with and without the refresher running. Compare to the snapshot version. Done when
   you have the two p99 numbers and the ratio.
4. **(medium)** Implement the `ConnectionPool` from 2.5 with a fake `Conn` that sleeps 1 ms per
   "query". 16 workers, pool of 4, acquire timeout 50 ms. Count successful queries and timeouts
   over 2 seconds. Then leak one lease on purpose and watch the pool degrade. Done when the leak
   is visible in the in-use metric.
5. **(hard)** Build a two-actor system: a `Parser` actor that receives raw strings, parses them to
   `Order` structs, and sends them to an `Accountant` actor that keeps per-account balances and
   answers `Query` messages by replying through a `std::promise`. Both have bounded mailboxes.
   Drive it with 4 producer threads. Then make the mailboxes small (2) and construct a
   mailbox-deadlock between the actors; explain it in terms of hold-and-wait. Done when both the
   working system and the deliberate deadlock are demonstrated.

## 7. Interview questions

**Q: Thread per connection, thread pool, or event loop?**
Thread per connection is simplest and fine below a few hundred connections. A pool bounds threads
and suits short request/response with blocking back-end calls at thousands of connections. An event
loop handles tens of thousands of mostly idle connections on a few threads but requires non-blocking
handlers; the modern default is event loops for I/O plus a pool for blocking work.

**Q: How do you share a read-mostly table with hundreds of threads without a lock on every read?**
Hold it behind an atomically swapped `shared_ptr<const Table>`. Readers acquire-load the pointer
and use that immutable snapshot; the refresher builds a new table off to the side and release-stores
the pointer. Readers never block or see partial state; the old table is freed when the last reader
drops it. Real RCU goes further by making the read side write no shared memory at all.

**Q: Why is classic double-checked locking broken, and what is the fix?**
The unlocked first read of a plain pointer is a data race, and even if atomic, without acquire it
may observe the pointer before the object's fields on weakly ordered hardware (or because the
compiler reordered the constructor's stores). Fix: `std::atomic` with acquire on the fast-path
load and release on the store, or `std::call_once`, or a function-local static, which the compiler
implements with exactly that protocol.

**Q: How does a function-local static achieve thread-safe initialisation?**
The compiler emits a guard variable, an inline acquire check on the fast path, and on the slow path
`__cxa_guard_acquire`, which takes a lock, re-checks, runs the constructor, and releases with a
release store. Exactly one thread constructs; others block until it is done; a throwing constructor
resets the guard so the next call retries.

**Q: Design a database connection pool.**
A bounded set of pre-opened connections behind a mutex and a condition variable. `acquire` waits
with a timeout and throws on exhaustion rather than hanging; it returns an RAII lease that gives
the connection back on destruction so error paths cannot leak. Add health checks on return, a
maximum connection age, in-use and wait-time metrics, and size it below the database's limit.

**Q: What is an actor and when would you use one?**
A component that owns its state exclusively and interacts only through a message queue, so it
needs no locks and cannot participate in a lock-order deadlock. Use it for state owned by one
logical component and mutated frequently, such as a logger, a stats aggregator, or a connection
manager. The cost is a queue hop per interaction and bounded mailboxes that can deadlock each other
if two actors block sending to one another.

**Q: Is `std::atomic_load` on a `shared_ptr` lock-free?**
Usually not; libstdc++ uses a small table of spinlocks keyed by the `shared_ptr`'s address, so the
load is a few nanoseconds of spinlock plus an atomic count increment. It is still far cheaper under
heavy reads than a `shared_mutex` and never blocks readers for the duration of a refresh.

## 8. Recap

- Connection shapes: thread-per-connection (small), pool (your server), event loop + pool (large).
- Actors own state and communicate by queue; no locks, no lock order, but mailbox deadlocks exist.
- Read-mostly, periodically refreshed data: immutable snapshot behind an atomically swapped
  `shared_ptr<const T>` with release/acquire. Refresh becomes a pointer store.
- RCU is the same idea with a grace period instead of reference counting; `liburcu` or epochs
  when reference counts are too hot.
- Lazy init: magic static first, `call_once` second, atomic pointer with acquire/release only
  when you also need replacement.
- Connection pools: bounded, RAII lease, acquire timeout, health check, max age, metrics.
- Never hold a process lock across a leased connection; never take two leases per worker.
- `shared_ptr` atomic functions are spinlock-backed in libstdc++; still much better than a
  reader-writer lock under heavy reads.
- Describe your server in these nouns: acceptor, pool, bounded queue, snapshot cache, connection
  pool, striped counters.
