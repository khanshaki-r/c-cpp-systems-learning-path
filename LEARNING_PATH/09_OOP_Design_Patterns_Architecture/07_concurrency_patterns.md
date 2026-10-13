# 07. Concurrency Patterns

**Time:** ~3 hours   **Prerequisites:** 03, 06; 04_Concurrency (mutex, condition_variable, threads, futures)

## 1. In plain words

Every concurrency pattern is an answer to one question: *which thread is allowed to touch
this state, and how do the others ask it to?* There are only a few good answers. Lock the
state and let any thread touch it under the lock (monitor). Give the state its own thread
and send it requests (active object, actor). Make the state immutable and share it freely
(snapshots). Or do not share at all and pass values through queues (producer-consumer).

You maintain a multithreaded RPC server, so you have already met all of these, possibly
without names: the connection table under a mutex is a monitor; the worker threads pulling
from a request queue are a thread pool over a producer-consumer queue; the config you reload
by swapping a pointer is a snapshot. This module names them, shows the idiomatic C++17
spelling of each, and, more importantly, tells you which one to pick for which shape of
problem. The mechanics of mutexes and condition variables are in `04_Concurrency`; here we
talk about structure.

## 2. The real thing

### 2.1 Monitor object

**What:** an object whose every public method acquires the object's own mutex. Callers never
lock; the invariant is protected at the class boundary. Condition variables inside let
methods wait for a state ("not empty") without the caller knowing.

**Shape:**

```cpp
class BoundedQueue {
    mutable std::mutex mu_;  std::condition_variable not_empty_, not_full_;  std::deque<T> q_;
public:
    bool push(T);                 // waits while full
    std::optional<T> pop();       // waits while empty; nullopt when closed and drained
    void close();                 // wakes everyone
};
```

**Rules:**

- Wait with a predicate: `cv.wait(lk, [&]{ return closed_ || !q_.empty(); })`. This handles
  spurious wakeups and the "notify before wait" race.
- Unlock before notify when you can; the woken thread can then take the lock immediately
  instead of blocking on it.
- Never call out to unknown code (callbacks, virtual functions on foreign objects) while
  holding the monitor lock. That is how you get deadlocks between two monitors (A's method
  calls B under A's lock while B's method calls A under B's lock).
- `close()` for shutdown: a closed flag plus `notify_all`, producers get `false`, consumers
  drain and get `nullopt`.

**Cost:** every call serialises on the mutex. Fine for a queue (operations are tiny). Bad for
a hot read-mostly structure (see snapshots).

**C equivalent:** `pthread_mutex_t` and `pthread_cond_t` in the struct, every function
does lock/unlock by hand. Same design; the C++ version makes the unlock unforgettable.

### 2.2 Producer-consumer and bounded queues

**What:** producers push work, consumers pop it, a queue decouples their rates. The queue is
the monitor above.

**The bound is the design decision.** An unbounded queue lets a burst of requests consume all
memory while consumers fall behind; the process dies of OOM an hour later, far from the
cause. A bounded queue makes producers block (back-pressure) or fail fast ("server busy").
In a server, *fail fast at the edge* is almost always right: reject new connections or return
a "try later" error while the queue is full, rather than accepting work you cannot finish.

**Sizing:** bound = consumers times the acceptable queueing delay in units of per-item
service time. Eight workers at 1 ms per item and a 50 ms latency budget suggests a bound
around 400. Make it configurable; measure.

### 2.3 Thread pool

**What:** N worker threads pulling from one task queue; tasks return futures. Fixed N means
no thread-per-request explosion and no thread creation cost per request.

**Shape:** `std::future<R> submit(F&& f, Args&&... args)` packages the call in a
`std::packaged_task`, pushes a type-erased `std::function<void()>` onto the queue, returns
the future. Exceptions inside the task are stored in the future and rethrown at `get()`.

**Rules:**

- **Never block a pool thread on work from the same pool.** Four workers each `submit` a
  child and `get()` it: all four block waiting for children that can never run. Deadlock.
  Fixes: separate pools per dependency level, or continuation-style (`then`) instead of
  blocking, or ensure children are run inline when called from a pool thread.
- **Capture by value.** A task may run long after the submitting scope has ended. `[&local]`
  is the most common thread-pool crash.
- **Bound the queue** (Section 2.2), and decide what `submit` does when full: throw, block,
  or return a failed future.
- **Sizing:** CPU-bound work: number of cores. I/O-bound work that blocks (database calls):
  more, bounded by the downstream's capacity (your database's connection limit), and ideally
  a separate pool so a slow database does not starve CPU work.
- **Shutdown:** stop flag under the lock, `notify_all`, join all. Decide whether queued tasks
  are drained or dropped and document it.

**C equivalent:** `pthread_create` N times, a mutex/cond queue of `{void (*fn)(void*),
void* arg}`. Results via out-pointers and a separate completion signal; no futures.

### 2.4 Active object

**What:** an object with its *own* thread and a queue of method calls. Callers enqueue
closures and receive futures. The object's state is touched by exactly one thread, so it
needs no mutex; the queue is the only synchronised structure.

**When:** state that must be mutated strictly in order (a ledger, a log writer, a device
driver), or that would otherwise need a complex lock protocol. Trades lock complexity for
one context switch per call and one thread per object.

**Monitor vs active object in one line:** a monitor runs methods on the caller's thread
under a lock; an active object runs them on its own thread from a queue.

**Cost:** a thread per object (so do not make a thousand of them), latency of a queue hop
(microseconds), and the result is asynchronous (a future) even when the caller wanted a
plain return value.

### 2.5 Reactor vs proactor

These describe how an I/O-driven server is organised.

**Reactor:** one (or a few) event-loop threads block in `epoll_wait` (Linux) or `select`/
`WaitForMultipleObjects` (Windows). When a socket is *readable*, the loop calls the
connection's handler, which performs the non-blocking `read` itself and parses. Readiness is
reported; the application does the I/O. libevent, Node.js, Redis, Nginx are reactors.

**Proactor:** the application *initiates* an asynchronous read ("read up to 4 KB into this
buffer and tell me when done"); the OS performs the I/O; a completion event is delivered with
the data already in the buffer. Completion is reported; the OS did the I/O. Windows IOCP is
the canonical proactor; Linux `io_uring` is one; Boost.Asio presents a proactor interface over
both (emulating it on epoll).

```
Reactor:   epoll_wait -> "fd 7 readable" -> handler calls read(7, buf) -> parse
Proactor:  ReadAsync(fd 7, buf) ... later ... completion(fd 7, 1024 bytes in buf) -> parse
```

**Trade-offs:** proactor needs buffers to be committed up front for every pending read (memory
proportional to connections); reactor allocates a buffer only when data is ready. Proactor
maps better to Windows and to true async disk I/O. Reactor is simpler to reason about and is
what Linux servers mostly use. Your Win32 server is very likely IOCP (proactor) or
select-based (reactor); know which and why.

**Pairing with workers:** either model usually hands parsed requests to a thread pool and
gets replies back through a queue the loop drains (`eventfd`/pipe/`PostQueuedCompletionStatus`
to wake the loop). The ownership rule from Module 03 applies: workers hold `ConnId`, not
`Connection*`.

### 2.6 Read-copy-update snapshots and immutable messages

**Problem:** configuration, routing tables, fee schedules: read on every request by every
thread, updated rarely. A mutex around reads serialises the hot path for the sake of a
once-a-minute write. A reader-writer lock helps but still has shared cache-line traffic.

**Solution:** readers take a `std::shared_ptr<const Config>` snapshot (atomic load plus a
refcount increment), use it lock-free, drop it. Writers copy the current config, modify the
copy, and atomically swap the pointer. Readers holding the old snapshot see a consistent
version until they finish; the old object is freed when the last reference drops.

```cpp
std::shared_ptr<const Config> snapshot() const { return std::atomic_load(&current_); }
void update(F mutate) { lock_guard lk(writer_mu_); Config next = *atomic_load(&current_);
                        mutate(next); atomic_store(&current_, make_shared<const Config>(move(next))); }
```

**Why `const`:** immutability is what makes the sharing safe. A published snapshot is never
written again, so any number of threads may read it with no synchronisation. This is also the
rule for **immutable messages** between threads: build the message, then hand it over; never
mutate it after sending. Passing by value (move) achieves this trivially; passing
`shared_ptr<const T>` achieves it for large objects.

**Relation to kernel RCU:** same idea. Readers never block; writers pay a copy; old versions
linger until all readers are done (the kernel detects quiescence; `shared_ptr` counts
references).

**C++17 detail:** `std::atomic_load`/`atomic_store` on `shared_ptr` are free functions
(deprecated in C++20 in favour of `std::atomic<std::shared_ptr<T>>`). They are typically
implemented with a small internal spinlock table, so they are not strictly lock-free, but
they are uncontended and fast.

### 2.7 Actor model

**What:** every actor owns its state, processes one message at a time from its mailbox, and
communicates only by sending immutable messages to other actors by *address* (a name or
handle), never by pointer. No shared mutable state means no data races by construction.

Active object is an actor with a typed method interface; the actor model generalises it to
untyped messages, dynamic addressing, and supervision (an actor that restarts failed
children). Erlang/OTP and Akka are the reference systems.

**When in C++:** systems with many independent stateful entities and complex interaction
protocols: per-account state, per-session state, sagas (multi-step transactions with
compensation), simulation. The code file runs a transfer as a saga across two account
actors with a coordinator.

**Costs:** every interaction is asynchronous (a message, not a return value), which makes
simple request-reply logic verbose; one thread per actor does not scale past hundreds, so
real actor systems schedule actors onto a pool; message ordering is guaranteed only per
sender-receiver pair.

### 2.8 Choosing

| State shape | Pattern |
|---|---|
| Small shared structure, short operations, any thread | Monitor object |
| Work handoff between stages | Bounded producer-consumer queue |
| Many independent CPU or I/O tasks | Thread pool (bounded queue, by-value captures) |
| State that must be mutated in strict order | Active object |
| Read-mostly shared data | Immutable snapshots via `shared_ptr<const T>` |
| Many stateful entities with protocols between them | Actors |
| Network I/O multiplexing | Reactor (epoll) or proactor (IOCP, io_uring) feeding a pool |

The first question in a design review: "which thread owns this state?" If the answer is "any
thread, under this lock" you have a monitor and need to check every call path for lock
ordering. If the answer is "this one thread" you have an active object or actor and need to
check that nothing else touches it. If the answer is "nobody writes it after publication" you
have a snapshot and need nothing else.

## 3. Code

- `code/m07_active_object.cpp`: `ActiveLedger` with a private worker thread, `enqueue()`
  returning futures via `packaged_task`, strict in-order postings from four client threads,
  drain-on-destroy.
- `code/m07_monitor_producer_consumer.cpp`: `BoundedQueue<T>` with `push`, `pop`,
  `pop_for(timeout)`, `close()`; three producers, two consumers, bound of 8 so back-pressure
  occurs.
- `code/m07_thread_pool.cpp`: `ThreadPool` with `submit(f, args...)`, by-value argument
  capture through `std::apply`, bounded queue with rejection, exceptions through futures,
  the deadlock rule in a comment.
- `code/m07_rcu_snapshot.cpp`: `ConfigHolder` with `snapshot()` (atomic load) and `update()`
  (copy, mutate, atomic store); four readers verify every snapshot is internally consistent
  while a writer swaps 20 versions.
- `code/m07_actor.cpp`: `Mailbox`, `ActorSystem` (mailboxes by name), `account_actor`,
  `coordinator_actor` running a debit-then-credit saga with compensation, messages as a
  `std::variant`.

The bounded queue's `pop` is the template for every condition-variable wait you will write:

```cpp
std::optional<T> pop() {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [this] { return closed_ || !q_.empty(); });   // predicate: re-checked on every wake
    if (q_.empty()) return std::nullopt;                               // closed and drained
    T item = std::move(q_.front());  q_.pop_front();
    lk.unlock();                                                       // release before notify
    not_full_.notify_one();
    return item;
}
```

## 4. Under the hood (senior-level)

**What `condition_variable::wait` does.** Atomically releases the mutex and sleeps on a
futex (Linux) keyed on the CV's internal word; on wake it re-acquires the mutex before
returning. Spurious wakeups are real: the futex can return without a matching notify (signal
delivery, timeouts, implementation details), which is why the predicate loop is mandatory.
`notify_one` is a `FUTEX_WAKE` syscall only if there is a waiter; libstdc++ tracks this to
avoid the syscall when nobody is waiting.

**Why unlock before notify.** If you notify while holding the lock, the woken thread
immediately blocks on the mutex you still hold, costing an extra context switch (the
"hurry up and wait" problem). Unlocking first lets it proceed. Some argue for notify-under-
lock for strict ordering guarantees; for a queue it is unnecessary.

**`packaged_task` and `future` internals.** A shared state object on the heap holds the
result or exception, a ready flag and a futex-like waiter. `get()` blocks on it. The
`packaged_task` is move-only (it owns the promise side); that is why the pool wraps it in a
`shared_ptr` to stuff it into a copyable `std::function`. Each `submit` therefore allocates:
the shared state, the packaged_task, possibly the `std::function` target. For a
microsecond-level hot path you would use a lock-free queue of fixed-size task structs; for
request handling it is irrelevant.

**`shared_ptr` snapshot cost.** `atomic_load(&sp)` is an atomic pointer read plus an atomic
increment on the control block (`lock xadd`). With four readers on four cores all
incrementing the *same* control block, that cache line bounces between cores: tens of
nanoseconds per snapshot rather than one. Still far cheaper than a mutex, and readers take
one snapshot per *request*, not per field access. For the very hottest paths, thread-local
cached snapshots refreshed by generation counter remove even that.

**epoll vs IOCP, mechanically.** `epoll_wait` returns a list of ready fds; the kernel kept a
ready-list that network interrupts append to. The application then calls `read`, copying
from the socket buffer into user memory. IOCP: `WSARecv` posts a user buffer to the kernel;
when data arrives the kernel copies directly into it and queues a completion packet;
`GetQueuedCompletionStatus` dequeues it. IOCP also limits the number of concurrently *running*
threads to the number you specify, waking another only when one blocks, which is a built-in
thread-pool scheduler. `io_uring` brings the posted-buffer completion model to Linux with
shared submission/completion rings and no syscall per operation in the fast path.

**Actor mailboxes and ordering.** A mailbox is a monitor queue. Message order is FIFO per
mailbox, so messages from A to B arrive in the order A sent them, but messages from A and C
to B interleave arbitrarily. Protocol design must not depend on cross-sender order. This is
the same constraint as TCP (ordered per connection) and the same mistake people make.

## 5. In production

**Your server's worker pool at 3 AM.** Requests time out; CPU is idle; the queue is full.
All workers are blocked in database calls because the database is slow. Diagnosis: `gdb -p`
or a dump shows every worker in `PQexec`. Fixes: a separate pool for database-bound work
with a size matching the database's capacity; a timeout on the database call; shedding load
at the edge (bounded queue, "busy" reply) so clients retry instead of piling up. The
underlying design error was one pool for two different resource constraints.

**The config reload that crashed.** Old design: `Config g_config;` behind a mutex; reload
*mutated it in place* while a request thread was halfway through reading three related
fields and saw a mix of old and new. Fix: snapshots. The reload builds a new `Config` and
swaps the pointer; every request sees exactly one version. This is a one-afternoon change
that removes a class of bugs that look like "random" misbehaviour once a day.

**The thread-pool capture bug.** A handler submits `[&request] { ... }` and returns; the
request lives on the connection's stack frame which is reused; the task reads garbage or
writes into the next request. AddressSanitizer catches it in test if the test actually
exercises the async path. Rule in code review: every lambda submitted to a pool is checked
for `&` captures and `this`.

**Shutdown ordering.** Pool destructor joins workers; workers may be mid-task calling into
the store; store must still exist. Declaration order in the owning class (Module 03): store
before pool. Also decide: drain or drop queued tasks on shutdown? For a banking server,
drain (a posting that was accepted must complete), with a timeout and a log of anything
dropped.

**Windows note.** On your Win32 server the equivalents are `CRITICAL_SECTION` /
`SRWLOCK` + `CONDITION_VARIABLE`, IOCP for the reactor/proactor role, and the Windows thread
pool API. The patterns are identical; `std::thread` and friends map onto them in MSVC's
standard library.

## 6. Try it

1. **(easy)** In `m07_monitor_producer_consumer.cpp`, set the bound to 1 and to 1000.
   Measure total runtime and maximum observed `size()`. Done when you can explain what the
   bound trades.
2. **(easy)** Add `try_push(T)` to `BoundedQueue` that returns `false` immediately when full
   instead of blocking. Use it in a producer that counts rejections. Done when you can state
   when a server should block producers and when it should reject.
3. **(medium)** Reproduce the thread-pool self-deadlock: a 2-thread pool, two tasks that each
   submit a child task and `get()` it. Observe the hang. Then fix it without changing pool
   size (hint: run the child inline when `std::this_thread::get_id()` belongs to the pool).
   Done when the fixed version completes and you can explain why the detection is needed.
4. **(medium)** Convert `ConfigHolder` to use a `std::shared_mutex` with readers taking
   `shared_lock`. Benchmark 4 readers at `-O2` against the snapshot version for one second
   each. Done when you have numbers and an explanation involving cache lines.
5. **(hard)** Extend `m07_actor.cpp` with a supervisor actor: if an account actor receives a
   `Crash{}` message it throws; the supervisor catches the thread's failure (hint: run the
   actor body in a `try`, send a `Died{name}` message), restarts it with the last known
   balance, and re-sends any messages that were in flight. Done when a transfer survives a
   crash of one account actor.
6. **(hard)** Write a minimal reactor on Linux: `epoll` on a listening socket plus
   connections, non-blocking reads into per-connection buffers, echo back. Then add a
   `ThreadPool` for "processing" with `(ConnId, std::string)` messages in both directions
   and an `eventfd` to wake the loop when replies are ready. Done when a client disconnecting
   mid-request causes the reply to be dropped, not a crash.

## 7. Interview questions

**Q: What is the difference between a monitor object and an active object?**
A: A monitor runs methods on the caller's thread under the object's mutex; many threads enter
one at a time. An active object runs methods on its own thread from a queue of requests;
callers get futures. Monitors are simpler and synchronous; active objects remove locking
from the object and guarantee ordering at the cost of a thread and asynchronous results.

**Q: Why must a condition variable wait use a predicate loop?**
A: Because wakeups can be spurious, because `notify` may happen before `wait` (the notification
is lost unless the predicate is checked first), and because another thread may consume the
state between the notify and your re-acquiring the lock. The predicate re-check handles all
three.

**Q: Why should a work queue be bounded?**
A: An unbounded queue turns overload into unbounded memory growth and an OOM far from the
cause. A bound gives back-pressure (producers block) or fast failure (reject at the edge),
keeping latency and memory predictable. Rejecting early lets clients retry against a server
that is still healthy.

**Q: Explain reactor versus proactor.**
A: Reactor: the event loop waits for readiness (epoll) and the application performs the
non-blocking I/O itself. Proactor: the application posts an asynchronous operation with a
buffer and is notified on completion with the data already there (IOCP, io_uring). Proactor
commits buffers up front; reactor is simpler on Linux. Both typically hand parsed work to a
pool.

**Q: How do you share a frequently read, rarely updated configuration across threads?**
A: Publish immutable snapshots: readers take a `shared_ptr<const Config>` with an atomic load
and use it lock-free; writers copy, modify, and atomically swap the pointer. Old versions are
freed when the last reader drops them. Readers never block and always see one consistent
version.

**Q: What is the classic thread-pool deadlock and how do you avoid it?**
A: A pool task blocks waiting on another task in the same pool; with all workers blocked,
the awaited tasks never run. Avoid by never blocking a pool thread on same-pool work: use
continuations, separate pools per dependency level, or run child tasks inline when called
from a pool thread.

**Q: What makes the actor model free of data races?**
A: Each actor's state is touched only by its own processing loop, and actors communicate
only through immutable messages sent to mailboxes by address. There is no shared mutable
state to race on; the only synchronised structure is the mailbox queue.

## 8. Recap

- Ask "which thread owns this state?" before choosing a pattern.
- Monitor: mutex inside the object, predicate waits, no callbacks under the lock.
- Bounded producer-consumer queues give back-pressure; the bound is a design decision.
- Thread pool: by-value captures, bounded queue, never block on same-pool work, size by
  resource.
- Active object: own thread plus queue; strict ordering, no internal locks, async results.
- Reactor reports readiness (epoll); proactor reports completion (IOCP, io_uring).
- Snapshots: `shared_ptr<const T>`, atomic load/store, copy-on-write; immutability makes
  sharing safe.
- Actors: state per actor, messages by address, FIFO per mailbox only.
- Shutdown is declaration order: pools declared last, resources they use declared first.
