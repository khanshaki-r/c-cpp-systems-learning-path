# 07. Atomics and the memory model

**Time:** ~4 hours   **Prerequisites:** 02, 03

## 1. In plain words

An atomic variable is one that every thread sees as a single, indivisible value: no torn reads, no
lost updates on increment, and, critically, a way to say "when you see this value, you are also
guaranteed to see everything I wrote before I stored it". That last part is the memory model, and
it is the part most engineers get wrong.

The mental model that works is **message passing**. Think of a `store` with release semantics as
sealing an envelope: everything the sender wrote before sealing goes into the envelope. A `load`
with acquire semantics that reads that value is opening the envelope: everything in it is now
visible to the reader. Nothing is promised about writes that happened after the seal, and nothing
is promised to a reader who did not open the envelope. Relaxed atomics are postcards with no
envelope: the value arrives intact, but nothing else is promised about what else you can see.
Sequentially consistent atomics are envelopes that are also stamped by a single global post office
so everyone agrees on the order all envelopes were sent.

You have used `InterlockedIncrement` and `InterlockedCompareExchange`. On Windows x86 those are
full barriers (seq_cst). This module explains what you were getting for free, what it costs on ARM,
and how to ask for exactly as much ordering as you need.

## 2. The real thing

### 2.1 `std::atomic<T>`

```cpp
#include <atomic>
std::atomic<int>   counter{0};
std::atomic<bool>  ready{false};
std::atomic<Node*> head{nullptr};
```

`T` must be trivially copyable. Operations:

| Operation | Meaning |
|---|---|
| `load(order)` / `operator T` | read |
| `store(v, order)` / `operator=` | write |
| `exchange(v, order)` | write, return old |
| `compare_exchange_weak(expected&, desired, ...)` | CAS, may fail spuriously |
| `compare_exchange_strong(expected&, desired, ...)` | CAS, fails only if value differs |
| `fetch_add / fetch_sub / fetch_and / fetch_or / fetch_xor` | read-modify-write, return old (integral and pointer types) |
| `operator++ / -- / += ...` | same, seq_cst |
| `is_lock_free()` / `is_always_lock_free` | does this use a hardware instruction or a hidden lock? |

`is_lock_free()` is true for 1, 2, 4, 8 byte types on x86-64 and usually for 16-byte with
`-mcx16` (cmpxchg16b). For a 24-byte struct it is false: libatomic implements it with a hidden
mutex (a hash of spinlocks keyed by address). A non-lock-free atomic is still correct, just not
usable in signal handlers or where you were promised no blocking. `static_assert(std::atomic<T>::
is_always_lock_free)` when you depend on it.

### 2.2 Compare-and-swap

```cpp
bool compare_exchange_strong(T& expected, T desired,
                             memory_order success, memory_order failure);
```

Atomically: if the current value equals `expected`, write `desired` and return true. Otherwise
write the current value into `expected` and return false. This is the universal building block;
every lock-free structure is a CAS loop:

```cpp
int old = counter.load();
while (!counter.compare_exchange_weak(old, old * 2)) {
    // old has been updated to the current value; loop recomputes desired
}
```

`compare_exchange_weak` may return false even when the value equals `expected` (on LL/SC
architectures like ARM, the store-conditional can fail because of an unrelated cache event). Use
`weak` inside a loop (it is the natural fit and cheaper on ARM), `strong` when a spurious failure
would make you take a wrong branch and you are not looping.

On x86 both compile to `lock cmpxchg`. On ARMv8 `weak` is `ldaxr` / `stlxr` once; `strong` is the
same in a retry loop.

### 2.3 The six memory orders

```cpp
enum class memory_order {
    relaxed, consume, acquire, release, acq_rel, seq_cst
};
```

Forget `consume`; no compiler implements it as specified (they promote it to `acquire`) and it is
being redesigned. The five that matter:

| Order | Used on | Promise |
|---|---|---|
| `relaxed` | any | atomicity only. No ordering with other memory accesses. |
| `release` | store, RMW | every write before this store (in this thread) is visible to a thread that acquire-loads the stored value. "Seal the envelope." |
| `acquire` | load, RMW | every read or write after this load (in this thread) sees what the releasing thread wrote before its store. "Open the envelope." |
| `acq_rel` | RMW | both: the RMW is an acquire for the load half and a release for the store half. For passing the envelope along a chain. |
| `seq_cst` | any | acquire and/or release, plus a single total order of all seq_cst operations that every thread agrees on. Default. |

The precise rule, which you should be able to state in an interview:

> If thread A performs a **release** store to atomic `x` and thread B performs an **acquire** load
> of `x` that reads the value A stored (or a later value in `x`'s modification order written by a
> release sequence), then A's release **synchronizes-with** B's acquire, and everything
> **sequenced-before** the release in A **happens-before** everything sequenced-after the acquire
> in B.

"Sequenced before" is the within-thread program order. "Happens before" is the cross-thread
guarantee. A write that happens-before a read is visible to that read. That is the whole memory
model in one sentence; the rest is consequences.

### 2.4 The canonical example: publish a result

```cpp
int data = 0;                         // plain, non-atomic
std::atomic<bool> ready{false};

// producer                                   // consumer
data = 42;                                    while (!ready.load(std::memory_order_acquire)) {}
ready.store(true, std::memory_order_release); assert(data == 42);   // guaranteed
```

Why it is correct: `data = 42` is sequenced-before the release store. The acquire load reads
`true`, which the release stored, so the release synchronizes-with the acquire. Therefore
`data = 42` happens-before `assert(data == 42)`. The compiler may not move `data = 42` below the
release store, nor the `data` read above the acquire load, and it must emit whatever CPU
instructions make the hardware honour that.

With `relaxed` on both, there is no synchronizes-with. The assert can fail: the compiler may
reorder, or the CPU may (on ARM). The program is not a data race on `ready` (it is atomic) but it
*is* a data race on `data`, because the write and read of `data` are unordered. Relaxed atomics
do not create happens-before.

### 2.5 Relaxed: when it is enough

Relaxed guarantees: the operation is atomic, and all threads agree on the modification order of
*that one variable* (you never see a counter go 5, 7, 6). That is exactly what a statistics
counter needs:

```cpp
requests_total.fetch_add(1, std::memory_order_relaxed);   // nobody reads other data based on this
```

Also enough for: a "stop" flag where the consumer only needs to eventually see it and does not
read other data based on it; a sequence number generator; a reference count *increment* (you
already hold a reference, so you are not publishing anything).

Not enough for: any flag that says "the data is ready", any pointer that is published for others
to dereference, a reference count *decrement* that leads to delete (the deleter must see every
write from every other previous owner: `fetch_sub(1, acq_rel)` or `release` plus an `acquire`
fence before the delete; this is what `shared_ptr` does).

### 2.6 What seq_cst adds

Acquire/release gives you pairwise agreement: A and B agree about A's envelope. It does not give
you a global order of *unrelated* envelopes. The store-buffering test from module 02:

```cpp
std::atomic<int> x{0}, y{0};
// thread 1                              // thread 2
x.store(1, release);                     y.store(1, release);
int r1 = y.load(acquire);                int r2 = x.load(acquire);
// can r1 == 0 && r2 == 0?  With acquire/release: YES.  With seq_cst: NO.
```

With acquire/release each thread's store and load are to different variables and nothing
synchronizes (no load read the other's store), so no happens-before edge exists and both can read
0. With `seq_cst`, all four operations are in one total order; whichever store is first in it is
visible to the other thread's load. Dekker's and Peterson's mutual exclusion algorithms depend on
this, which is why they fail with acquire/release.

Rule: use `seq_cst` (the default) unless you have measured a cost and can write down the
acquire/release argument for why the weaker order is correct. On x86 the only extra cost of
seq_cst is on *stores* (an `mfence` or `xchg` instead of a plain `mov`); loads are free either way.
On ARM both loads and stores pay.

### 2.7 Fences

`std::atomic_thread_fence(order)` is a standalone barrier. A release fence followed (later in
program order) by a relaxed store makes that store act as a release; a relaxed load followed by
an acquire fence makes that load act as an acquire. Use case: you have many relaxed operations and
want one barrier instead of one per operation:

```cpp
// producer: fill 1000 slots relaxed, then one fence, then publish
for (...) slot[i].store(v, relaxed);
std::atomic_thread_fence(std::memory_order_release);
published.store(true, std::memory_order_relaxed);

// consumer
while (!published.load(relaxed)) {}
std::atomic_thread_fence(std::memory_order_acquire);
// all slot[i] visible
```

`std::atomic_signal_fence` is the compiler-only barrier (no CPU instruction) for ordering with a
signal handler on the same thread. The old `asm volatile("" ::: "memory")` is the GCC equivalent.

### 2.8 Why `volatile` is not atomic

Covered in module 02; the one-line version: `volatile` constrains the compiler's treatment of that
variable's accesses and nothing else. No atomicity for multi-word types, no CPU fence, no ordering
with other variables, no happens-before. MSVC's `/volatile:ms` extension gives acquire/release on
x86 and ARM, which is why old Win32 code with `volatile LONG` flags works on MSVC and breaks on
GCC. Replace with `std::atomic`.

### 2.9 x86 vs ARM

| | x86-64 (TSO) | ARMv8 (weakly ordered) |
|---|---|---|
| relaxed load / store | `mov` | `ldr` / `str` |
| acquire load | `mov` (free) | `ldar` |
| release store | `mov` (free) | `stlr` |
| seq_cst load | `mov` (free) | `ldar` |
| seq_cst store | `xchg` or `mov; mfence` (expensive) | `stlr` (+ `dmb` on some compilers) |
| fetch_add any order | `lock xadd` (full barrier always) | `ldxr/stxr` loop or `ldadd` (ARMv8.1) with ordering flags |
| CAS any order | `lock cmpxchg` (full barrier) | `ldaxr/stlxr` loop or `cas` |
| full fence | `mfence` | `dmb ish` |

Consequences: on x86 every RMW is already a full barrier so memory orders on `fetch_add` change
nothing in the generated code; the only place you see a difference is seq_cst stores. On ARM every
order choice changes the code and the cost. Code that is subtly wrong (relaxed where acquire was
needed) passes on x86 and fails on ARM. Test on ARM if you can (a Raspberry Pi, an AWS Graviton
instance, Apple silicon).

### 2.10 Mapping to Win32 Interlocked

| Win32 | C++ |
|---|---|
| `InterlockedIncrement(&v)` | `v.fetch_add(1) + 1` (seq_cst) |
| `InterlockedExchange(&v, x)` | `v.exchange(x)` |
| `InterlockedCompareExchange(&v, desired, expected)` | `v.compare_exchange_strong(expected, desired)` (note argument order differs; Win32 returns the old value, C++ returns bool and updates `expected`) |
| `InterlockedExchangeAdd` | `fetch_add` |
| `InterlockedOr / And` | `fetch_or / fetch_and` |
| `InterlockedIncrementAcquire / Release` | `fetch_add(1, acquire / release)` |
| `InterlockedIncrementNoFence` | `fetch_add(1, relaxed)` |
| `MemoryBarrier()` | `atomic_thread_fence(seq_cst)` |
| `_ReadWriteBarrier()` | `atomic_signal_fence(seq_cst)` (compiler only) |
| `volatile` with `/volatile:ms` | acquire load / release store |

All un-suffixed Interlocked functions are full barriers. The `Acquire`, `Release` and `NoFence`
suffixed variants exist for ARM Windows where the difference matters.

## 3. Code

File: `code/m07_atomic_counter_orders.cpp` — counter correctness and the cost of each order.

```cpp
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>

template <class F>
double time_ms(F f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::atomic<long> counter{0};

void bump(std::memory_order mo, int n) {
    for (int i = 0; i < n; ++i) counter.fetch_add(1, mo);   // WHY: on x86 all orders emit lock xadd
}

std::atomic<long> seq_store_target{0};
void seq_stores(std::memory_order mo, int n) {
    for (int i = 0; i < n; ++i) seq_store_target.store(i, mo);   // WHY: here x86 differs: mov vs xchg
}

int main() {
    const int threads = 4, per = 2'000'000;
    for (auto mo : {std::memory_order_relaxed, std::memory_order_acq_rel, std::memory_order_seq_cst}) {
        counter.store(0);
        double ms = time_ms([&] {
            std::vector<std::thread> ts;
            for (int t = 0; t < threads; ++t) ts.emplace_back(bump, mo, per);
            for (auto& t : ts) t.join();
        });
        std::cout << "fetch_add order " << static_cast<int>(mo) << ": counter=" << counter.load()
                  << " (expected " << static_cast<long>(threads) * per << ") " << ms << " ms\n";
    }
    for (auto mo : {std::memory_order_relaxed, std::memory_order_release, std::memory_order_seq_cst}) {
        double ms = time_ms([&] { seq_stores(mo, 50'000'000); });
        std::cout << "single-thread store order " << static_cast<int>(mo) << ": " << ms << " ms\n";
    }
    static_assert(std::atomic<long>::is_always_lock_free, "need lock-free long");
    struct Big { long a, b, c; };
    std::atomic<Big> big{};
    std::cout << "atomic<24-byte struct> lock free: " << (big.is_lock_free() ? "yes" : "no") << '\n';
    return 0;
}
```

File: `code/m07_message_passing.cpp` — the publish pattern, and the store-buffering litmus test
showing acquire/release vs seq_cst.

```cpp
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>
#include <cassert>

// Part 1: publish non-atomic data through an atomic flag with release/acquire.
struct Payload { int a; int b; long c; };
Payload payload{};                       // plain data, written by producer before publish
std::atomic<bool> ready{false};

void producer() {
    payload = Payload{1, 2, 3};                           // sequenced-before the release store
    ready.store(true, std::memory_order_release);        // seal the envelope
}
void consumer(long& out) {
    while (!ready.load(std::memory_order_acquire)) {}    // open the envelope
    out = payload.a + payload.b + payload.c;             // guaranteed 6; no data race
}

// Part 2: store buffering. Count how often both threads read 0.
template <std::memory_order StoreMO, std::memory_order LoadMO>
long store_buffering(int iterations) {
    long both_zero = 0;
    for (int it = 0; it < iterations; ++it) {
        std::atomic<int> x{0}, y{0};
        std::atomic<int> start{0};
        int r1 = -1, r2 = -1;
        std::thread t1([&] {
            start.fetch_add(1); while (start.load() < 2) {}     // WHY barrier: both start together
            x.store(1, StoreMO);
            r1 = y.load(LoadMO);
        });
        std::thread t2([&] {
            start.fetch_add(1); while (start.load() < 2) {}
            y.store(1, StoreMO);
            r2 = x.load(LoadMO);
        });
        t1.join(); t2.join();
        if (r1 == 0 && r2 == 0) ++both_zero;
    }
    return both_zero;
}

int main() {
    long sum = 0;
    std::thread c(consumer, std::ref(sum));
    std::thread p(producer);
    p.join(); c.join();
    std::cout << "published sum: " << sum << '\n';
    assert(sum == 6);

    const int iters = 20000;   // thread creation dominates; keep modest
    std::cout << "store-buffering (release/acquire): both-zero " 
              << store_buffering<std::memory_order_release, std::memory_order_acquire>(iters)
              << " / " << iters << "  (non-zero is legal and happens on x86)\n";
    std::cout << "store-buffering (seq_cst):         both-zero "
              << store_buffering<std::memory_order_seq_cst, std::memory_order_seq_cst>(iters)
              << " / " << iters << "  (must be 0)\n";
    return 0;
}
```

The first number is often small but non-zero on a multi-core x86 (you need the two stores to be
in flight at the same moment). Thread creation per iteration makes the window narrow; exercise 3
asks you to improve the harness. The second number is always 0.

## 4. Under the hood (senior-level)

### What the compiler emits (x86-64, GCC 13, -O2)

```cpp
ready.store(true, std::memory_order_release);  // mov BYTE PTR ready[rip], 1
ready.store(true, std::memory_order_seq_cst);  // mov eax,1 ; xchg al, BYTE PTR ready[rip]
ready.load(std::memory_order_acquire);         // movzx eax, BYTE PTR ready[rip]
counter.fetch_add(1, std::memory_order_relaxed);   // lock add QWORD PTR counter[rip], 1
```

The release store is a plain `mov` because x86 TSO already forbids a store from passing an earlier
store or load. The compiler's job on x86 is mostly to *not reorder* across the atomic; the hardware
does the rest. The seq_cst store needs `xchg` (which is implicitly locked and therefore a full
fence) to prevent a later load from being satisfied before this store leaves the store buffer:
the store-buffering case.

On ARMv8 the same four lines are `stlr`, `stlr`, `ldar`, and an `ldaxr`/`add`/`stlxr` loop (or
`ldadd` on v8.1). Every ordering decision becomes an instruction choice.

### Cache coherence makes atomics possible

All modern multi-core CPUs are cache coherent: at any instant, a given cache line has one agreed
value across all cores (MESI protocol, module 10). Coherence is why a relaxed store is eventually
seen by every other core and why all cores agree on the modification order of a single variable.
What coherence does **not** give you is ordering *between* different cache lines. Core A may see
line X's new value before line Y's new value while core B sees the reverse. Memory orders are about
cross-line ordering; coherence alone handles single-variable atomicity.

### The store buffer and the invalidation queue

Two hardware structures cause reordering. The **store buffer** holds stores waiting for cache-line
ownership; later loads from the same core can complete before earlier stores drain (store-load
reordering, allowed on x86). The **invalidation queue** on each core holds "your copy of line L is
stale" messages that the core acknowledges before processing; a load can hit a stale line still in
cache (allowed on ARM/POWER, not x86). A release store on ARM (`stlr`) waits for prior stores to
drain; an acquire load (`ldar`) processes the invalidation queue before loading. On x86 the
hardware does both implicitly for ordinary loads and stores, which is why only the store-load case
needs `mfence`/`xchg`.

### `lock` prefix cost

A `lock`-prefixed instruction must obtain the line in Exclusive state, perform the RMW with the line
pinned, and (because it is a full fence) drain the store buffer. Uncontended, on a line already in
L1: roughly 20 cycles (typical). Contended across cores: the line bounces, 100+ ns per operation
(typical), and throughput *falls* as you add cores. That is why `fetch_add` on one shared counter
from 16 threads is slower than from 1 thread, and why module 10 uses per-thread counters.

### Modification order and release sequences

Each atomic variable has a single modification order all threads agree on. If A release-stores
value 1, and then B does a relaxed `fetch_add` producing 2, and C acquire-loads 2, C still
synchronizes with A's release: the RMW extends A's release sequence. This is what makes a
reference count work: the thread that decrements to zero acquires the releases of *all* previous
decrementers, not just the last one.

### Why the standard, not the hardware, is the contract

Writing to the hardware model ("x86 is TSO, so relaxed is fine for this flag") is wrong even if the
generated code would be right, because the *compiler* is bound only by the standard. A relaxed
store gives the compiler permission to move `data = 42` below it. GCC does this. The hardware
never gets a chance to save you.

## 5. In production

- **Statistics counters.** Replace `g_requests++` with `std::atomic<long>` and
  `fetch_add(1, relaxed)`. Correct, portable, and cheap when read rarely. If 32 workers hammer it,
  it becomes a contended line; module 10 fixes that with per-thread counters.
- **Shutdown flag.** `std::atomic<bool> stop`. Workers `load(relaxed)` in their loop condition is
  fine *if* they only exit on it. If they also read "final config" or "drain target" that the
  stopper wrote just before setting the flag, it must be `release`/`acquire`. When unsure, use the
  default seq_cst; a shutdown flag is read once per loop and the cost is irrelevant.
- **Published pointers.** A config or cache snapshot swapped by `ptr.store(new_table, release)`
  and read by `ptr.load(acquire)`. The acquire is not optional: without it the reader may see the
  pointer but not the table's contents (on ARM) or the compiler may hoist reads of the table above
  the load. Module 11 builds this.
- **Double-checked locking.** The textbook broken pattern becomes correct with an acquire load on
  the fast path and a release store after construction, or you use `std::call_once` which does it
  for you (module 11).
- **Porting `volatile LONG` Win32 flags.** Each one is a bug on GCC. Audit, replace with
  `std::atomic`, choose the order by asking "does anyone read other data based on this value?"
  If yes, release/acquire. If no, relaxed.
- **3 AM symptom:** a flag-based handshake works on the x86 build and the consumer reads garbage
  on the new ARM box. Nine times out of ten a relaxed store that should have been release. TSan
  (which understands memory orders) reports this on x86 even though x86 hides it at runtime.

## 6. Try it

1. **(easy)** Build and run `m07_atomic_counter_orders.cpp`. Confirm all three orders give the
   correct count. Compare the store timings: relaxed and release should be similar, seq_cst
   slower. Dump the assembly (`-S`) and find the `xchg`. Done when you can point to it.
2. **(easy)** In `m07_message_passing.cpp`, change the producer's store to `relaxed` and the
   consumer's load to `relaxed`. Build with `-fsanitize=thread`. TSan reports a data race on
   `payload`. Restore release/acquire; TSan is silent. Done when you can explain why TSan
   considers the relaxed version a race even though x86 would run it correctly.
3. **(medium)** Improve the store-buffering harness: two long-lived threads with a shared atomic
   "round" counter, each iteration both threads spin until the round increments, then do their
   store/load, then signal completion. Run 10 million rounds. Report the both-zero count for
   relaxed, release/acquire, and seq_cst. Done when relaxed and acq/rel show non-zero and seq_cst
   shows exactly zero.
4. **(medium)** Implement a reference-counted `struct Shared { std::atomic<int> refs; Payload p; }`
   with `acquire_ref()` (relaxed increment) and `release_ref()` (decrement; if it hits zero, delete).
   Choose the memory order for the decrement and write a comment deriving why `acq_rel` (or
   `release` plus an acquire fence) is necessary and relaxed is not. Done when the comment
   mentions "the deleter must see every other thread's writes to `p`".
5. **(hard)** Write Peterson's mutual-exclusion algorithm for two threads with atomics. Use
   seq_cst and verify two threads incrementing a plain shared counter 1 million times each give
   exactly 2 million. Then change to release/acquire and observe (on x86 you may need many runs,
   on ARM it fails quickly) that mutual exclusion breaks. Explain in terms of the store-buffering
   test which reorder breaks it. Done when the explanation names the two stores and two loads
   involved.

## 7. Interview questions

**Q: Explain acquire and release.**
A release store seals everything the storing thread wrote before it into the stored value; an
acquire load that reads that value unseals it, so everything the storer did before the release is
visible to the loader after the acquire. Formally the release synchronizes-with the acquire and
sequenced-before plus synchronizes-with gives happens-before. Nothing is promised to threads that
do not load the value, and relaxed operations do not create this edge.

**Q: What does seq_cst add over acquire/release?**
A single total order over all seq_cst operations that every thread agrees on. Acquire/release only
orders the pair that actually communicated; two threads each storing to their own flag and loading
the other's can both read the old value. Under seq_cst one store is first in the global order and
the other thread's load must see it. Dekker's and Peterson's algorithms need this.

**Q: When is `memory_order_relaxed` correct?**
When you need atomicity and nothing else: a statistics counter nobody makes decisions from, a
sequence generator, a reference count increment where you already own a reference, a stop flag
checked in a loop where no other data is read based on it. The moment another thread reads other
memory because it saw the value, you need release on the writer and acquire on the reader.

**Q: Why does `std::shared_ptr` use `acq_rel` (or release plus acquire fence) on the decrement?**
The thread whose decrement reaches zero deletes the object and must see every write every other
owner made to it before releasing their reference. Each decrement releases that thread's writes;
the final decrement must acquire all of them. Relaxed would allow the deleter to free memory that
another core's writes have not yet reached.

**Q: What is the difference between `compare_exchange_weak` and `strong`?**
Weak may fail spuriously, returning false even when the value matched, because on LL/SC
architectures the store-conditional can fail for reasons unrelated to the value. Use weak in a
retry loop (natural fit, cheaper on ARM), strong when a false failure would send you down a wrong
path and you are not looping. On x86 both are `lock cmpxchg`.

**Q: Why is `volatile` not a substitute for `std::atomic`?**
It only constrains the compiler's handling of that variable's accesses: no elision, no caching in
registers, no reordering with other volatiles. It gives no atomicity for multi-word types, emits
no CPU fences, imposes no order relative to non-volatile accesses, and creates no happens-before,
so the program is still UB under the standard. MSVC's `/volatile:ms` adds acquire/release as an
extension, which is why some Win32 code gets away with it.

**Q: Why does the same memory-order bug pass on x86 and fail on ARM?**
x86 is TSO: hardware never reorders stores with stores or loads with loads, so acquire loads and
release stores are plain `mov`s and relaxed code often behaves as if it were acquire/release.
ARM reorders almost everything unless told not to, so a missing `ldar`/`stlr` lets the hardware
expose the reorder. The compiler can reorder on both, so even on x86 the relaxed version is a real
bug that happens to be hidden most of the time.

**Q: What does `lock xadd` cost and why does a shared counter get slower with more threads?**
The lock prefix requires the core to hold the cache line in Exclusive state for the duration and
acts as a full fence. Uncontended on a local line it is tens of cycles; contended, the line
migrates between cores on every increment at 100 ns or more each, so total throughput drops as
cores are added. Per-thread counters that are summed on read avoid the shared line.

## 8. Recap

- `std::atomic<T>`: indivisible load, store, exchange, CAS, fetch-ops; `is_lock_free` tells you
  whether it is a hardware instruction or a hidden lock.
- Release store + acquire load that reads it = synchronizes-with; sequenced-before plus that =
  happens-before; happens-before means visible.
- Relaxed is atomicity only, no ordering; fine for counters and simple flags, wrong for
  publishing data.
- seq_cst adds a single global order; needed when two threads each write their own flag and
  read the other's. It is the safe default.
- `acq_rel` for read-modify-write that passes ownership along a chain; `shared_ptr`'s decrement.
- Fences let one barrier cover many relaxed operations.
- `volatile` is not atomic; MSVC's extension is why Win32 code sometimes survives.
- x86: acquire/release are free, seq_cst stores cost an `xchg`, RMWs are always full barriers.
  ARM: every order choice is a different instruction.
- Interlocked* are seq_cst; `Acquire`/`Release`/`NoFence` suffixes map to the C++ orders.
- The compiler is bound by the standard, not by your CPU; write to the standard.
