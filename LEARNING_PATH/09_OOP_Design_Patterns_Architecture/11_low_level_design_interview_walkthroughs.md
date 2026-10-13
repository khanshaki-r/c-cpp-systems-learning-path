# 11. Low-Level Design Interview Walkthroughs

**Time:** ~5 hours   **Prerequisites:** 01 to 08 (06, 07 and 08 especially)

## 1. In plain words

The low-level design (LLD) round, sometimes called "machine coding" or "object-oriented
design", gives you a small system ("design a parking lot", "design a rate limiter") and 45 to
60 minutes. You are expected to ask questions, pick classes and their responsibilities, draw
a class diagram, write the core of the code, and discuss how it changes when requirements
change: more threads, more types, more scale.

It is not a test of whether you know the "right" answer. There is no right answer. It is a
test of whether you work like a senior engineer: you clarify before building, you keep the
design as simple as the requirements allow, you name trade-offs out loud, and the code you
write compiles and handles the edge cases.

This module gives you a repeatable method and seven complete walkthroughs. Each has a code
file in `code/` that compiles and checks itself. Practise each one against a 45-minute timer
until you can do it without looking.

## 2. The real thing

### 2.1 The method (use it every time)

```
 0-5 min    1. CLARIFY       functional requirements, scale, concurrency, what is out of scope
 5-10 min   2. ENTITIES      nouns -> classes / value types; verbs -> methods
            3. VARIATION     what will change? -> interface, std::function, or template
 10-15 min  4. DIAGRAM       boxes, ownership (<>-- composition), uses (-->), multiplicity
 15-40 min  5. CODE          core data structures and the 2-3 central methods, compiling
 40-45 min  6. TEST + EXTEND walk one example through; edge cases; threading; scale; trade-offs
```

**1. Clarify.** Ask, then write the answers down where the interviewer can see them:
- What operations? What inputs and outputs? What errors?
- How many items, requests per second? (Decides data structures.)
- Single-threaded or concurrent? (Decides locking.)
- What happens on failure, on full, on duplicate?
- What is explicitly out of scope (persistence, distribution, UI)?

**2. Entities.** Underline the nouns. Each becomes a class *if it has behaviour or
invariants*, otherwise a plain struct. Value types (`Ticket`, `Record`) are cheap and
copyable; entities with identity (`Level`, `ParkingLot`) are usually owned in one place.

**3. Variation.** The interviewer's follow-ups ("now add a new vehicle type", "now a
different pricing") target the parts that vary. Put them behind a strategy *now*, but only
those: a `FeePolicy`, a `SpotAllocator`, a rate-limit `Algorithm`. One operation:
`std::function`. Several operations: an interface. Fixed at compile time and hot: template.

**4. Diagram.** ASCII on the whiteboard or the shared editor:

```
 +-----------+ 1     * +-------+         <>----->   composition (owns, lifetime bound)
 |   Owner   |<>------>| Part  |         ------->   uses / depends on
 +-----------+         +-------+         ---|>      implements / inherits
 | method()  |
 +-----------+
```

**5. Code.** Start with the data structures and the central method. Use the standard
library. Show ownership (`unique_ptr`, references), `const`, error returns. Leave
boilerplate (getters, printing) for the end or skip it with a comment.

**6. Test and extend.** Walk one example through the code out loud. Then discuss: thread
safety (one mutex first, then striping or sharding), scale (what breaks at 10x), testability
(injected clock), and the trade-offs of the choices you made.

**Things that lose points:** coding before clarifying; a class per noun with no behaviour
("Manager" classes that hold one map); inheritance hierarchies three deep for a
two-variant problem; global singletons; not handling the full/empty/duplicate case;
silence (the interviewer cannot grade what you do not say).

### 2.2 Walkthrough 1: parking lot

**Clarify.** Levels with Small/Medium/Large spots. Motorcycle fits any spot, Car fits Medium
or Large, Truck only Large. Park returns a ticket or "full"; unpark returns a fee. Fee per
hour by vehicle type, first 15 minutes free. Several entry gates (concurrent). Out of scope:
payments, reservations, displays.

**Entities and variation.** `Vehicle` (value), `Spot` (identified by level and index),
`Level` (free spots per size), `Ticket` (value), `ParkingLot` (facade). Varies: how a spot is
chosen (`SpotAllocator`), how fees are computed (`FeePolicy`).

```
 +--------------------+ 1     * +----------------------+
 | ParkingLot         |<>------>| Level                |
 |  park(v, t)        |         |  take(size)          |
 |  unpark(id, t)     |         |  give_back(size, i)  |
 |  free_spots(size)  |         |  free_: set<int>[3]  |
 +--------------------+         +----------------------+
   |  uses        | uses            * active
   v              v                 v
 SpotAllocator  FeePolicy      +-----------+
 (std::function)(std::function)| Ticket    | id, plate, type, spot, size, entry
                               +-----------+
```

**Key code** (`code/m11_parking_lot.cpp`):

```cpp
// Smallest fitting size first, lowest level first within that size.
std::optional<std::pair<SpotId, SpotSize>>
smallest_fit_lowest_level(std::vector<Level>& levels, VehicleType t) {
    for (auto s = static_cast<std::size_t>(smallest_fit(t)); s < kSizes; ++s)
        for (auto& lvl : levels)
            if (auto spot = lvl.take(static_cast<SpotSize>(s)))
                return std::make_pair(*spot, static_cast<SpotSize>(s));
    return std::nullopt;
}
```

Each `Level` keeps a `std::set<int>` of free spot indices per size, so "nearest free spot" is
`*begin()` in O(log n). The lot keeps `active_` tickets by id and a `plates_` map so the same
car cannot park twice.

**Talking points.**
- Why not `class Car : public Vehicle`? Vehicles have no behaviour here; the only difference
  is size class. An enum plus one `smallest_fit()` function is simpler. Use inheritance when
  types *behave* differently.
- Concurrency: one mutex around `park`/`unpark`. The critical section is microseconds;
  gates are a handful of threads. Per-level locks add deadlock risk (a car spilling to the
  next level needs two) for no measurable gain.
- Extensions: electric spots with chargers (a new `SpotSize` or a feature flag on spots and
  a different allocator); reservations (a `Reserved` state per spot with expiry); display
  boards (observer on free-count changes).

### 2.3 Walkthrough 2: LRU cache with TTL

**Clarify.** `get`, `put(key, value, ttl)`, `erase`. Capacity in entries; evict least
recently used when full. Entries expire after their TTL. O(1) operations. Thread-safe.
Testable without sleeping.

**Design.** A doubly linked list in recency order plus a hash map from key to list
iterator. Each list node stores key, value and expiry time. Expiry is checked lazily on
`get` and by an O(n) `purge_expired()` sweep called from a timer.

```
 +---------------------------+ uses  +-------------+
 | LruTtlCache<K,V>          |------>| Clock       |<|--- SteadyClock, FakeClock
 |  get(k) -> optional<V>    |       +-------------+
 |  put(k, v, ttl?)          |
 |  erase(k), purge_expired()|        index_: unordered_map<K, list<Entry>::iterator>
 +---------------------------+        lru_:   list<Entry>   (front = most recent)
            ^ wraps                   Entry { K key; V value; TimePoint expires; }
 +---------------------------+
 | SyncLruTtlCache (mutex)   |---> ShardedLruTtlCache<K,V,N>: N shards by hash(key)
 +---------------------------+
```

**Key code** (`code/m11_lru_ttl.cpp`):

```cpp
std::optional<V> get(const K& key) {
    auto it = index_.find(key);
    if (it == index_.end()) { ++stats_.misses; return std::nullopt; }
    auto node = it->second;
    if (is_expired(*node)) { remove(it); ++stats_.expirations; ++stats_.misses; return std::nullopt; }
    lru_.splice(lru_.begin(), lru_, node);   // touch: O(1), no allocation, iterators stay valid
    ++stats_.hits;
    return node->value;                      // a copy: a reference would dangle after eviction
}
```

**Talking points.**
- `splice` is the key trick: it relinks a node without copying or reallocating, and other
  iterators in the map stay valid.
- `get` mutates (moves the node, updates stats), so a `shared_mutex` does not help: every
  call is a writer. To scale, shard by key hash. Sharding makes LRU approximate globally.
- Lazy expiry alone leaks memory for keys never read again; hence the periodic purge. For
  millions of entries, keep a min-heap or a timing wheel of expiry times so purging is
  O(expired) instead of O(n).
- Time comes from an injected `Clock` and uses `steady_clock`, never wall time, which jumps.
- Alternatives worth naming: CLOCK (second-chance) approximates LRU with a bit per entry and
  no list reordering, which is cheaper under concurrency; LFU when frequency matters more
  than recency.

### 2.4 Walkthrough 3: in-memory KV store with transactions

**Clarify.** `SET`, `GET`, `UNSET`, `COUNT value`, `BEGIN`, `ROLLBACK`, `COMMIT`.
Transactions nest; `ROLLBACK` undoes the innermost; `COMMIT` makes everything permanent.
`GET` and `COUNT` O(1). Single client (ask!).

**Design choice, say it out loud:**

| | Undo log (chosen) | Overlay layers |
|---|---|---|
| Write | apply to live map, record before-image once per key per tx | write into the top layer only |
| Read | O(1), live map | O(depth): search layers top-down |
| Rollback | replay before-images | drop the top layer |
| Commit | discard logs (or merge into parent) | merge layers down |
| Isolation for other clients | none: they see uncommitted writes | natural: base untouched until commit |

```
 +-----------------------------+
 | TxKvStore                   |      data_:   unordered_map<string,string>
 |  set/get/unset/count        |      counts_: unordered_map<string,size_t>  (value -> #keys)
 |  begin/rollback/commit_*    |      txs_:    vector<Tx>   (stack)
 +-----------------------------+      Tx { unordered_map<string, optional<string>> undo; }
          ^ used by
 execute(db, "SET a 10")  -- the protocol layer, kept out of the store
```

**Key code** (`code/m11_kv_transactions.cpp`):

```cpp
void write(const std::string& k, std::optional<std::string> v) {
    if (!txs_.empty()) txs_.back().undo.emplace(k, get(k));   // first write wins: keeps the ORIGINAL
    raw_write(k, std::move(v));                               // the only function that touches data_
}

[[nodiscard]] bool rollback() {
    if (txs_.empty()) return false;
    Tx tx = std::move(txs_.back());
    txs_.pop_back();
    for (auto& [key, before] : tx.undo) raw_write(key, before);
    return true;
}
```

**Talking points.**
- `emplace` does not overwrite, so a key written twice in one transaction keeps its value
  from *before the transaction*, which is what rollback must restore.
- `COUNT` stays O(1) because `raw_write` maintains `counts_`; one choke point for the
  invariant.
- Nested commit (`commit_innermost`) merges the child's undo entries into the parent, keeping
  the parent's own before-image when both touched a key.
- Multi-client follow-up: overlay layers per transaction give read-committed isolation;
  conflict detection at commit gives optimistic concurrency; this is how MVCC engines think
  (see `07_Databases_Internals`).

### 2.5 Walkthrough 4: logger library

**Clarify.** Levels, runtime minimum level, several sinks, file and line, thread id,
timestamps. Asynchronous: request threads must never block on disk. What on overflow: drop
and count, or block? (Ask; for a server the answer is usually drop.) Flush on demand and on
shutdown.

```
 LOG(lg, Level::Info) << "loan " << id;      macro: if disabled, nothing to the right is evaluated
        |
        v
 +-----------+  submit(Record)  +--------------------------+  write(Record) +-------------+
 | LogLine   |----------------->| Logger                   |--------------->| Sink        |<|-- Console
 | (RAII:    |                  |  level_ (atomic)         |                |  write()    |<|-- Memory
 | submits in|                  |  queue_ (bounded deque)  |                |  flush()    |<|-- File
 | dtor)     |                  |  worker_ thread          |                +-------------+
 +-----------+                  +--------------------------+
```

**Key code** (`code/m11_logger.cpp`):

```cpp
#define LOG(lg, lvl) \
    if (!(lg).enabled(lvl)) {} else LogLine((lg), (lvl), __FILE__, __LINE__).stream()

void run() {                                    // the background thread
    std::deque<Record> batch;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) return;
        batch.swap(queue_);                     // one lock per batch, not per record
        in_flight_ = batch.size();
        lk.unlock();                            // write OUTSIDE the lock: sinks may be slow
        for (auto& r : batch) for (auto& s : sinks_) s->write(r);
        batch.clear();
        lk.lock();
        written_ += in_flight_; in_flight_ = 0;
        drained_cv_.notify_all();               // wakes flush()
    }
}
```

**Talking points.**
- The macro's `if (...) {} else` shape means a disabled `LOG(...) << expensive()` costs one
  relaxed atomic load and a branch; `expensive()` is never called. It also nests safely
  inside an unbraced `if`/`else`.
- A `Record` owns its message (`std::string`): it crosses threads, so a pointer into the
  caller's stack would dangle.
- The worker thread is the last member: constructed after everything it touches, and the
  destructor stops and joins it before members are destroyed.
- Bounded queue with drop counting: logging must never take the server down. Expose the
  drop counter as a metric.
- Follow-ups: `fmt`-style formatting done on the background thread (pass arguments, not
  strings); a lock-free MPSC queue; file rotation; rate-limited logging for repeated errors.

### 2.6 Walkthrough 5: rate limiter

**Clarify.** `allow(key)` per customer or API key. Which algorithm? Burst allowed? Exact or
approximate? Memory per key? Distributed across servers (usually: "first single node, then
discuss")?

| Algorithm | State per key | Bursts | Exactness | Notes |
|---|---|---|---|---|
| Token bucket | tokens, last refill time | up to capacity | exact long-run rate | default choice |
| Fixed window | window index, count | 2N at a boundary | approximate | cheapest |
| Sliding window log | timestamps of accepted requests | none | exact | memory O(N) per key |
| Sliding window counter | two window counts | small | approximate (weighted) | good compromise |

```
 +----------------------------+  owns *  +----------------------------+
 | KeyedRateLimiter           |<>------->| Algorithm                  |<|-- TokenBucket
 |  allow(key)                | per key  |  try_acquire(now) -> bool  |<|-- FixedWindow
 |  evict_idle()              |          +----------------------------+<|-- SlidingWindowLog
 |  stripes_[16]: mutex + map |
 |  factory_, now_ (injected) |
 +----------------------------+
```

**Key code** (`code/m11_rate_limiter.cpp`):

```cpp
bool TokenBucket::try_acquire(TimePoint now) {
    if (now > last_) {                       // lazy refill: no timer thread per key
        const double secs = std::chrono::duration<double>(now - last_).count();
        tokens_ = std::min(capacity_, tokens_ + secs * rate_);
        last_ = now;
    }
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
}
```

**Talking points.**
- Algorithms are single-key, single-threaded and take `now` as a parameter: pure functions
  of their state, trivial to test. The keyed wrapper adds lookup, locking and eviction.
- Lock striping (16 mutexes by key hash) means two different customers rarely contend.
- Idle keys are evicted, or memory grows with every client ever seen.
- Distributed: a shared store (Redis with an atomic script) for exact global limits at the
  cost of a network hop per request, or local limits of `global / servers` with periodic
  rebalancing, which is approximate but free.
- Where it sits in a server: as middleware before the service (Module 06, chain of
  responsibility), returning a "try later" error with a retry-after hint.

### 2.7 Walkthrough 6: loan-application workflow engine

This one is your home ground. Interviewers at fintech companies ask it in many forms
("design an order workflow", "design a KYC pipeline").

**Clarify.** States: Draft, Submitted, UnderReview, Approved, Rejected, Disbursed,
Cancelled. Events: Submit, StartReview, Approve, Reject, Disburse, Cancel. Who may trigger
what (roles)? Guard rules per transition (KYC done, age range, product limit, FOIR, maker
cannot approve own application). Return all failed rules, not just the first. Side effects
after success (letters, posting to core banking). Full audit trail.

```
 Draft --Submit--> Submitted --StartReview--> UnderReview --Approve--> Approved --Disburse--> Disbursed
   |                   |                          |    \--Reject--> Rejected
   +------Cancel-------+-----------Cancel---------+-----------Cancel----+--> Cancelled

 +-------------------------+ owns  +----------------------------------------------+
 | WorkflowEngine          |<>---->| map<(State, Event), Transition>               |
 |  add(from, ev, t)       |       | Transition { to; set<Role>; vector<Rule>;     |
 |  fire(app, ev, actor)   |       |              vector<Action> }                 |
 |  available_events(s)    |       +----------------------------------------------+
 +-------------------------+       Rule   { name; function<optional<string>(app, actor)> }
            | mutates              Action function<void(app&, actor)>
            v
 +-------------------------+ 1  *  +------------+
 | Application             |<>---->| AuditEntry | from, to, event, user, accepted, reasons
 +-------------------------+       +------------+
```

**Key code** (`code/m11_workflow_engine.cpp`):

```cpp
Outcome fire(Application& app, Event ev, const Actor& actor) const {
    const State from = app.state;
    std::vector<std::string> reasons;
    auto it = table_.find({from, ev});
    if (it == table_.end()) {
        reasons.push_back(std::string("event not allowed in state ") + name(from));
    } else {
        const Transition& t = it->second;
        if (!t.allowed_roles.count(actor.role)) reasons.push_back("role not permitted");
        for (const Rule& r : t.rules)                          // run ALL rules
            if (auto why = r.check(app, actor)) reasons.push_back(r.name + ": " + *why);
    }
    const bool ok = reasons.empty();
    const State to = ok ? it->second.to : from;
    app.audit.push_back(AuditEntry{from, to, ev, actor.user, ok, reasons});
    if (!ok) return Outcome{false, from, std::move(reasons)};
    app.state = to;
    for (const Action& a : it->second.actions) a(app, actor);
    return Outcome{true, to, {}};
}
```

**Talking points.**
- The engine is generic; the loan knowledge is in the table and the rules. A gold-loan
  product is a different table, not a different engine. The table can be loaded from a
  database configuration table, which is how lending platforms let operations change a
  workflow without a release.
- Rules are small named functions with captured parameters (`foir_at_most(5000)`), each
  unit-testable on its own. Money is integer paise; ratios are integer basis points.
- Why not the GoF state pattern with a class per state? Behaviour per state here is "which
  transitions exist and which rules guard them", which is data. Classes would scatter the
  table across seven files.
- Persistence: state change, audit row and outbox rows for actions commit in one database
  transaction. Concurrency: two officers acting on one application at once is prevented with
  optimistic locking (a version column checked in the `UPDATE ... WHERE version = ?`).
- Extensions: SLA timers (escalate if UnderReview over 48 hours: the job scheduler below);
  parallel approvals (a sub-state counting approvals); deviation workflows (a rule failure
  routes to a higher approver instead of refusing).

### 2.8 Walkthrough 7: job scheduler

**Clarify.** Run once at a time; run every period; cancel. Run jobs on a worker pool so a
slow job does not delay others. Retry failures with back-off? Missed runs after a pause: run
all, run once, or skip? Persistence across restarts (usually out of scope; mention a jobs
table)?

**Design, the key idea:** split pure logic from threads.

```
 +---------------------------+ owns +-------------------------------+
 | Scheduler (threaded)      |<>--->| SchedulerCore (pure)          |
 |  schedule_after/every     |      |  schedule_at(when, fn, retry) |
 |  cancel(id), stop()       |      |  run_due(now) -> vector<Due>  |
 |  dispatcher_ thread       |      |  report(id, ok, now, backoff) |
 |  workers_ threads         |      |  heap_: min-heap(due,seq,id)  |
 |  ready_ deque             |      |  jobs_, in_flight_: id -> Job |
 +---------------------------+      +-------------------------------+
```

**Key code** (`code/m11_job_scheduler.cpp`):

```cpp
void dispatch() {                                   // dispatcher thread
    std::unique_lock<std::mutex> lk(m_);
    while (!stopped_) {
        auto next = core_.next_due();
        if (!next) { cv_.wait(lk); continue; }
        if (cv_.wait_until(lk, *next) == std::cv_status::no_timeout) continue;  // woken: re-check
        for (auto& d : core_.run_due(Clock::now())) ready_.push_back(std::move(d));
        work_cv_.notify_all();
    }
}
```

**Talking points.**
- The core takes `now` as an argument, so every ordering, cancellation, catch-up and retry
  rule is tested deterministically without threads or sleeps. The threaded shell is thin.
- Min-heap keyed by (due time, sequence number): the sequence number gives FIFO order for
  jobs due at the same instant.
- Cancellation is lazy: erase from the job map; the heap entry is skipped when it surfaces.
  Removing from the middle of a binary heap would be O(n).
- `wait_until` is woken early by a new job that is due sooner than the current head; the loop
  re-reads the head after any wake-up (and spurious wake-ups are handled by the same path).
- Fixed rate (next = previous due + period) does not drift; fixed delay (next = finish +
  period) never overlaps itself. After a long pause this core runs a missed job once, not
  once per missed period. Say which you chose.
- Jobs run outside the lock, and exceptions are caught in the worker so one bad job cannot
  kill a worker thread.
- Scale: millions of timers use a hierarchical timing wheel (O(1) insert, as in the Linux
  kernel's timer wheel and Kafka's purgatory) instead of a heap.

## 3. Code

One file per walkthrough, each with a `main()` of `assert`-based checks:

| File | Contents |
|---|---|
| `code/m11_parking_lot.cpp` | `Level` with per-size free sets, allocator and fee strategies, `ParkingLot` facade with a mutex, concurrent-gate test |
| `code/m11_lru_ttl.cpp` | `LruTtlCache` (list + map + splice), lazy and swept expiry, stats, `SyncLruTtlCache`, `ShardedLruTtlCache`, `FakeClock` |
| `code/m11_kv_transactions.cpp` | `TxKvStore` with undo log, O(1) `COUNT`, nested rollback, flat and nested commit, text command interpreter |
| `code/m11_logger.cpp` | `Logger` (sync/async, bounded dropping queue, flush), `Sink` interface, `LogLine` RAII, `LOG` macro |
| `code/m11_rate_limiter.cpp` | token bucket, fixed window, sliding log, `KeyedRateLimiter` with lock striping and idle eviction |
| `code/m11_workflow_engine.cpp` | table-driven `WorkflowEngine`, rules (KYC, age, amount, FOIR, maker-checker), actions to an outbox, audit trail |
| `code/m11_job_scheduler.cpp` | pure `SchedulerCore` (heap, lazy cancel, catch-up, retry with back-off) and a threaded `Scheduler` |

Build any of them:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -o lld code/m11_lru_ttl.cpp && ./lld
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=thread -o lld code/m11_job_scheduler.cpp && ./lld
```

## 4. Under the hood (senior-level)

**`std::list` node cost.** Each node is a separate heap allocation holding two pointers plus
the element: for `Entry<std::string, std::string>` on 64-bit libstdc++ that is 16 bytes of
links plus two 32-byte strings plus the time point, before malloc overhead. Lookups chase a
pointer from the hash bucket to the map node, then to the list node: two likely cache misses
per `get`. High-performance caches use an intrusive list (links inside a slab-allocated
entry) or an array-based CLOCK to cut this to one miss.

**`std::unordered_map` in libstdc++** is a vector of buckets, each a pointer into one singly
linked list of nodes. `reserve(n)` sizes the bucket array once, so the hot path never
rehashes; a rehash is O(n) and invalidates iterators (but not references or pointers to
elements). That is why the LRU stores *list* iterators in the map and never map iterators
anywhere.

**`std::priority_queue`** is a binary heap in a `std::vector`: push and pop are O(log n) with
good locality, and there is no way to remove an arbitrary element, which is why the scheduler
cancels lazily. A timing wheel is an array of buckets indexed by `due / tick % size`;
insertion is O(1) and the cost moves to the tick.

**Condition variables and `wait_until`.** On Linux, `std::condition_variable::wait_until`
with a `steady_clock` time point uses `pthread_cond_clockwait` with `CLOCK_MONOTONIC` (glibc
2.30 and later; libstdc++ uses it when available). Older combinations converted to the
system clock, so a wall-clock jump could delay or hasten wake-ups. This is one more reason
to keep every deadline on `steady_clock`.

**Floating point in a token bucket.** `double` tokens accumulate rounding error over billions
of refills; it does not matter for a rate limiter, but in billing it would. An integer
variant stores tokens in micro-tokens or computes refill from integer nanoseconds.

**Mutex cost in these designs.** An uncontended `std::mutex` lock/unlock on Linux is a pair of
atomic operations in user space (futex fast path), on the order of tens of nanoseconds
(typical). Contended, it becomes a `futex` syscall and a context switch, microseconds. This
is why one mutex is fine for a parking lot and striping matters for a rate limiter that sees
every request.

## 5. In production

**The LRU that ate the heap.** A cache keyed by request-supplied strings with no capacity
limit, or with capacity counted in entries while values were megabyte-sized documents.
Capacity should be in bytes when values vary in size, with the size of each entry computed
at `put`.

**Rate limiting the wrong key.** Limiting by client IP behind a load balancer that does not
pass the original address limits everyone at once. In a branch-banking network, many users
share one NAT address; limit by authenticated user or branch id instead.

**Logger shutdown order.** A global logger destroyed before a global object whose destructor
logs: use-after-destruction at exit, visible as a crash in `atexit` handlers. Owning the
logger in `main` and passing it down (Module 09) makes the order explicit.

**Workflow tables changed live.** Operations removed a transition from the configuration
table while 200 applications were in that state, stranding them. Validate a new table
against current data (every non-terminal state with live applications must still have an
exit) before activating it.

**Scheduler and clock changes.** A daily job scheduled with `system_clock` ran twice on the
day the server's clock was corrected backwards by NTP. Use `steady_clock` for intervals and
compute "next civil time" (02:00 IST) explicitly from a time zone each time, never by adding
86,400 seconds.

**In the interview room.** If the interviewer pushes toward a distributed version, say what
changes: state moves to a shared store, operations become idempotent with request ids,
locks become leases with expiry, and you now need to discuss what happens during a network
partition. Then return to the single-node design they asked for.

## 6. Try it

1. **(easy)** Parking lot: add an `ElectricCar` that must take a spot with a charger. Done
   when only the allocator and fit rule changed and a test proves it does not take a plain
   Medium spot.
2. **(easy)** KV transactions: add `NUMEQUALTO` as an alias of `COUNT` and an `END` command
   that stops a script. Done when the script test in `main` covers both.
3. **(medium)** LRU: make capacity count bytes (`key.size() + value.size()`) instead of
   entries. Done when one large `put` evicts several small entries and a value larger than
   capacity is rejected.
4. **(medium)** Rate limiter: implement the sliding window counter (two fixed windows,
   weighted by overlap) and show it refuses the boundary burst that `FixedWindow` allows.
   Done when its test sits next to the fixed-window test.
5. **(hard)** Workflow engine: add an SLA. If an application stays `UnderReview` longer than
   48 hours, an `Escalate` event moves it to a new `Escalated` state. Use
   `SchedulerCore` from `m11_job_scheduler.cpp` with a fake time to drive it. Done when the
   test advances time and sees the escalation, and a decision before the deadline cancels
   the timer.
6. **(hard)** Timed practice: for each of the seven problems, start a 45-minute timer, open
   an empty file, and produce clarifications, a diagram and compiling code. Done when you
   finish all seven within time, then compare with the code files.

## 7. Interview questions

**Q: Walk me through how you approach an LLD problem.**
A: Clarify requirements, scale, concurrency and scope first and write them down. Identify
entities and the parts likely to vary, which become strategies. Draw a class diagram with
ownership. Code the core data structures and two or three central methods so they compile,
then walk an example, cover edge cases, and discuss threading, scale and trade-offs.

**Q: Why a list plus a hash map for LRU, and why splice?**
A: The map gives O(1) lookup of the node, the list keeps recency order with O(1) unlink and
relink, and `splice` moves a node to the front without allocation while keeping all other
iterators valid. Storing list iterators in the map is safe because list iterators are never
invalidated by operations on other nodes.

**Q: How would you make your LRU cache thread-safe and scalable?**
A: Start with one mutex around the operations; note that `get` mutates recency so a
reader-writer lock does not help. To scale, shard by key hash with an independent cache and
lock per shard, accepting approximate global LRU, or move to a CLOCK-style design where hits
only set a bit atomically.

**Q: Token bucket vs sliding window?**
A: Token bucket allows controlled bursts up to its capacity and enforces a long-run rate
with two numbers of state; it is the usual default. A sliding window log enforces an exact
count in any trailing window but stores a timestamp per accepted request; the sliding
window counter approximates it with two counters.

**Q: Undo log or overlay for transactions in a KV store?**
A: Undo log keeps reads O(1) and makes rollback replay the before-images, but other clients
see uncommitted writes. Overlay layers keep the base untouched until commit, giving
isolation at the cost of O(depth) reads. The choice depends on whether multiple clients
read concurrently.

**Q: How do you design a state machine for a business workflow?**
A: A table of (state, event) to transition, each with allowed roles, guard rules and
actions; a generic engine that runs all guards and reports every failure, records an audit
entry for every attempt, changes state, then runs actions. Persist state, audit and outbox
in one transaction and use optimistic locking against concurrent edits.

**Q: How do you test a scheduler without sleeping?**
A: Separate a pure core that takes `now` as a parameter from the thread that waits. All
ordering, cancellation and retry logic is tested by calling the core with chosen times; the
threaded shell gets a couple of short real-time smoke tests.

**Q: In an async logger, what happens when the queue is full?**
A: Choose explicitly: drop and count (protects request latency; the right default for a
server), block (never loses logs, can stall the service), or drop low-severity records first.
Expose the drop count as a metric so data loss is visible.

## 8. Recap

- Method: clarify, entities, variation, diagram, code, test and extend. Talk throughout.
- Make only the varying parts pluggable; `std::function` for one operation, interfaces for several.
- Parking lot: enum plus fit rule over a vehicle hierarchy; sets of free indices; one mutex.
- LRU with TTL: list plus map plus `splice`; lazy and swept expiry; inject the clock; shard to scale.
- KV transactions: undo log with first-write-wins before-images; one choke point for counts.
- Logger: macro that skips disabled work, RAII line, owned records, bounded dropping queue, write outside the lock.
- Rate limiter: pure single-key algorithms taking `now`; striped keyed wrapper; idle eviction.
- Workflow engine: data-driven table, all guards reported, audit always, actions via outbox.
- Scheduler: pure core plus thin threaded shell; heap with sequence tie-break; lazy cancel.

