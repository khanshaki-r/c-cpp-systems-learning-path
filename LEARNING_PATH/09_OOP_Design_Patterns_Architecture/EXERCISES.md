# Exercises: 09 OOP, Design Patterns and Architecture

Every exercise from every module, in one place. The full statement and the "done when"
condition are in section 6 of each module; this list gives the short form, the difficulty and
a hint. Build everything with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -o ex file.cpp && ./ex
```

Use `-fsanitize=thread` (instead of address) for anything with threads.

Suggested path if time is short: do every **easy** exercise in modules 01, 03, 06, 08, 09 and
11, then the **medium** ones in 09 and 11, then exercise 11.6 (timed practice) until you can
finish each LLD problem in 45 minutes.

## Module 01: OOP for a C programmer

| # | Level | Exercise | Hint |
|---|---|---|---|
| 1.1 | easy | Add `size` to the C function-pointer store and to the virtual `Store`; compare failure modes | A forgotten C slot is a null call at run time; a missing pure virtual override is a compile error when you instantiate. |
| 1.2 | easy | Turn one of your C modules into a class with private state | Find the invariant first (e.g. `head < capacity`), then make every mutation a member function that keeps it. |
| 1.3 | medium | `Shape` as a hierarchy and as `std::variant`; compare `sizeof` and allocations | Hierarchy: one heap block per shape plus a vptr each; variant: size of the largest alternative plus a tag, stored inline. |
| 1.4 | medium | Fix the dangling pointer from `find()` two ways | `std::map` nodes never move; or look up by id inside `transfer` after any call that can reallocate. |
| 1.5 | hard | Benchmark virtual (mixed and uniform types), variant, template | Uniform types make the indirect branch perfectly predicted; the remaining gap is lost inlining and pointer chasing. |

## Module 02: SOLID in plain words

| # | Level | Exercise | Hint |
|---|---|---|---|
| 2.1 | easy | Add `DEL` without touching `KvService` | Open/closed: register a new operation object in the table the service reads. |
| 2.2 | easy | `ReadOnlyStore` that throws on `put`; fix with `ReadableStore` | This is Liskov plus interface segregation: split the interface so the read-only type never promises `put`. |
| 2.3 | medium | `Clock` interface and `TtlStore` decorator tested with `FixedClock` | The decorator stores expiry per key and asks the clock on every `get`; the test advances the fake clock. |
| 2.4 | medium | Split one real C function into parse / rule / storage on paper | Write the storage interface from the rule's point of view: only the queries the rule needs. |
| 2.5 | hard | Generic `contract(make_store)` test template | Call it with lambdas returning `unique_ptr<Store>`; use `assert` with a message string via `&& "msg"`. |

## Module 03: Ownership and object lifetime design

| # | Level | Exercise | Hint |
|---|---|---|---|
| 3.1 | easy | Ownership diagram of your work RPC server | Every dashed arrow to something not owned by an ancestor is a potential use-after-free; list each one. |
| 3.2 | easy | Swap member order of `metrics_` and `conns_` and predict shutdown | Members are destroyed in reverse declaration order; a connection's destructor that touches metrics must die first. |
| 3.3 | medium | Generation counters for `BufferPool` handles | Store a generation per slot, bump it on release, compare in `get()`. |
| 3.4 | medium | Arena that runs destructors on `reset()` | Keep a vector of `(ptr, dtor)` pairs; a captureless lambda converts to `void(*)(void*)`. |
| 3.5 | hard | Two-thread server: I/O thread owns connections, worker gets ids | Workers never hold `Connection*`; replies carry a `ConnId` the I/O thread looks up and drops if gone. |

## Module 04: Creational patterns

| # | Level | Exercise | Hint |
|---|---|---|---|
| 4.1 | easy | Add a `"cached"` kind to `make_store` | The factory composes: construct the memory store, then wrap it. |
| 4.2 | easy | Cross-field validation in `ServerConfigBuilder` | Validate in `build()`, not in the setters, because the order of setter calls is free. |
| 4.3 | medium | Replace a `MetricsRegistry` singleton with DI and show state leaking between tests | Run two scenarios in one process; the singleton keeps counts from the first. |
| 4.4 | medium | Health-checked `ConnectionPool::acquire()` | Loop: pop, check `alive()`, discard dead ones, construct a new one if the pool runs dry. |
| 4.5 | hard | Self-registering factory and the static-library trap | An unreferenced object file in a `.a` is never pulled in; fix with `--whole-archive` or explicit registration calls. |

## Module 05: Structural patterns

| # | Level | Exercise | Hint |
|---|---|---|---|
| 5.1 | easy | `erase()` through the C API adapter | Map the C return code to `bool`; only throw for real failures, not "not found". |
| 5.2 | easy | `TimeoutStore` decorator in two stack positions | Outside the retry it times the whole retried call; inside it times each attempt. |
| 5.3 | medium | Negative caching in `CachingStoreProxy` | Cache `std::nullopt` with a short TTL and invalidate on `put` of that key. |
| 5.4 | medium | Age rule as a leaf and as a composite, plus `Not` | Composites win when rules are assembled from configuration; leaves win for clarity of one fixed rule. |
| 5.5 | hard | Split pimpl into three files and observe rebuilds and the destructor error | `~unique_ptr<Impl>` needs the complete `Impl`; define the destructor in the `.cpp` where `Impl` is complete. |

## Module 06: Behavioural patterns

| # | Level | Exercise | Hint |
|---|---|---|---|
| 6.1 | easy | `TieredFee` strategy from a threshold table | Capture the vector by value in the lambda (`[tiers = std::move(t)]`). |
| 6.2 | easy | `AuditLog` unsubscribes inside its own callback | `notify()` iterates a snapshot of ids and re-checks each id still exists before calling. |
| 6.3 | medium | Connection state machine as enum + `switch` | Remove a `case` and compile with `-Wall` to see `-Wswitch` name the missing enumerator. |
| 6.4 | medium | Add `Refund` to the variant and to the classic visitor | Variant: every `visit` without a generic fallback fails to compile. Classic: only visitors missing the new `visit` overload fail. |
| 6.5 | hard | `timeout` middleware with a worker thread | Capture the request by value (or `shared_ptr`) in the worker; the caller's stack frame is gone after the timeout. |
| 6.6 | hard | Bidirectional `RingBuffer::iterator` | Store a logical position (0..size) instead of a raw index; wrap only when dereferencing. |

## Module 07: Concurrency patterns

| # | Level | Exercise | Hint |
|---|---|---|---|
| 7.1 | easy | Bound of 1 vs 1000 in producer-consumer | Small bound: more blocking and context switches; large bound: more memory and latency hidden in the queue. |
| 7.2 | easy | Non-blocking `try_push` | Same lock, check `size() == cap`, return `false` instead of waiting. |
| 7.3 | medium | Reproduce and fix thread-pool self-deadlock | Record worker thread ids at start; if the caller is a worker, run the child task inline. |
| 7.4 | medium | `shared_mutex` config vs RCU snapshot benchmark | Every `shared_lock` writes the mutex's reader count: one cache line bouncing between cores. |
| 7.5 | hard | Supervisor actor that restarts a crashed account actor | Wrap the actor loop in `try`; on exception send `Died{name}` to the supervisor with the last state. |
| 7.6 | hard | Minimal epoll reactor plus thread pool and `eventfd` | Replies go through a queue; the worker writes to the `eventfd`; the loop drains replies and looks up `ConnId`, dropping unknown ones. |

## Module 08: Modern C++ alternatives to classic patterns

| # | Level | Exercise | Hint |
|---|---|---|---|
| 8.1 | easy | Third store type in `vector<AnyStore>` | It only needs `put` and `get` members with the right signatures; no base class. |
| 8.2 | easy | `CountingLog` policy | A stateless policy (static counter) keeps the empty base optimisation, so `sizeof` does not change. |
| 8.3 | medium | Small-buffer optimisation for `AnyStore` | `alignas(std::max_align_t) unsigned char buf[32]`, placement new, and a flag or function pointer to know which storage is in use. |
| 8.4 | medium | `Escape` event and `Paused` state in the variant framer | Without the fallback overload, the compiler lists each missing (state, event) pair as an error. |
| 8.5 | hard | Virtual decorator stack vs policy store benchmark | Use `-O2`, a volatile sink for results, and `perf stat` to see instructions per call; also compare `size` of both binaries. |
| 8.6 | hard | Find the pipeline loop in assembly | `-O2 -S -fverbose-asm`; search for the loop label; with `std::function` look for `call *%rax` or similar. |

## Module 09: Designing a server codebase

| # | Level | Exercise | Hint |
|---|---|---|---|
| 9.1 | easy | Add `EXISTS` to `m09_layered_server.cpp` | Touch protocol (new variant alternative), `serve` (new lambda) and one service function; storage already has `get`. |
| 9.2 | easy | `RequestIdLogger` decorator | Wrap `common::Logger`, hold the id, prefix each message; create one per request inside `serve`. |
| 9.3 | medium | `FlakyStorage` that fails as `Unavailable`, never as "not found" | The port must be able to say "failed": return `Result<optional<string>>` from `get`, and map `Unavailable` to `-ERR try again`. |
| 9.4 | medium | CMake target graph that forbids service -> storage includes | Give each target only its own `src/<layer>` as an include directory; link only along the allowed arrows. |
| 9.5 | hard | Characterization tests, then split a legacy handler | Capture outputs for at least 15 inputs first, including malformed ones; refactor only while all pass. |
| 9.6 | hard | One-page architecture for the loan-origination server | Use the Module 12 template; the outbox and unit-of-work decisions each need a rejected alternative. |

## Module 10: API design and code quality

| # | Level | Exercise | Hint |
|---|---|---|---|
| 10.1 | easy | `BranchId` strong type and a swapped call that fails to compile | Distinct tag types make `Strong<A, int>` and `Strong<B, int>` unrelated types. |
| 10.2 | easy | Observe `[[nodiscard]]` turning into an error | `-Werror` promotes `-Wunused-result`; restore the attribute afterwards. |
| 10.3 | medium | `ChaosLedger` fake that fails `post` on demand | A public `bool fail_next` flag in the fake is fine; tests own their doubles. |
| 10.4 | medium | `std::hash` for `Strong` | Specialise `template <class Tag, class Rep> struct std::hash<Strong<Tag, Rep>>` and hash `get()` inside it. |
| 10.5 | hard | Asynchronous shadow route with a timeout | Run the new handler via `std::async` or a small pool, never wait for it on the request path; compare results in the background. |
| 10.6 | hard | Review a real function with the checklist | Write findings as "problem, risk, suggested change"; at least one per group (design, correctness, quality). |

## Module 11: Low-level design interview walkthroughs

| # | Level | Exercise | Hint |
|---|---|---|---|
| 11.1 | easy | `ElectricCar` needing a charger spot | Model chargers as a separate size class or a per-spot flag; only the fit rule and allocator change. |
| 11.2 | easy | `NUMEQUALTO` alias and `END` command in the KV interpreter | Keep the store unchanged; this is protocol-layer work only. |
| 11.3 | medium | Byte-based capacity in the LRU | Track `bytes_used_`; evict in a loop until the new entry fits; reject entries larger than capacity. |
| 11.4 | medium | Sliding window counter rate limiter | `estimate = prev_count * (1 - elapsed_fraction) + curr_count`; refuse if `estimate >= limit`. |
| 11.5 | hard | SLA escalation in the workflow engine driven by `SchedulerCore` | Schedule an escalation job in the `StartReview` action; cancel it in the actions of `Approve` and `Reject`. |
| 11.6 | hard | Timed practice: all seven problems in 45 minutes each | Write the clarifying questions first, every time; compare your diagram with the one in the code file header. |

## Module 12: UML and communicating design

| # | Level | Exercise | Hint |
|---|---|---|---|
| 12.1 | easy | ASCII class diagram of `m11_parking_lot.cpp` | `vector<Level>` is composition with `*`; the allocator and fee policy are owned `std::function`s, so also composition. |
| 12.2 | easy | PlantUML for the rate limiter diagram | `KeyedRateLimiter *-- "*" Algorithm`, `Algorithm <|-- TokenBucket`, and so on. |
| 12.3 | medium | Sequence diagram of tcpkv shutdown | Signal handler and worker threads are separate lifelines; use `->>` for the wake-pipe write and condition variable notify. |
| 12.4 | medium | Trade-off table: where eligibility rules live | Criteria: round trips, testability, consistency across callers, deploy process, scaling tier, who can change rules. |
| 12.5 | hard | Two-page design doc for a master-data cache, presented in 10 minutes | Record yourself; check each row of the presentation table is covered and that the decision appears in the first minute. |
