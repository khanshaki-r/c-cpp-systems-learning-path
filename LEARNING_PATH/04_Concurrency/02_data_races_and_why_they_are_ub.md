# 02. Data races and why they are undefined behaviour

**Time:** ~2.5 hours   **Prerequisites:** 01

## 1. In plain words

A data race is two threads touching the same memory at the same time, where at least one of them
is writing, and nothing forces one to happen before the other. In C and C++ that is not "you get
one of the two values". It is undefined behaviour: the compiler is allowed to assume it never
happens, and it generates code on that assumption. Programs with data races can print impossible
values, loop forever, skip code, or work perfectly for three years and fail the week after a
compiler upgrade.

Analogy: two clerks updating the same paper ledger line without talking. One reads 100, the other
reads 100, both add 10 and both write 110. You lost an update. That is the easy case. The hard
case is that the clerks are allowed to use shorthand, write digits in any order they like, and
copy the ledger to a scratch pad and never look at the original again, all because the ledger
rules promise that nobody else will touch the line while they hold it. When that promise is broken
you do not get a slightly wrong number; you get a line that nobody wrote.

You have avoided most races at work by wrapping shared state in `CRITICAL_SECTION`. This module
explains what you were protecting against, so that you can spot the ones that are not wrapped.

## 2. The real thing

### 2.1 The precise definition

The C++ standard (intro.races) says two expression evaluations **conflict** if one modifies a
memory location and the other reads or modifies the same location. A program has a **data race**
if it contains two conflicting actions in different threads, at least one of which is not atomic,
and neither **happens before** the other. Any such execution is undefined behaviour.

Three parts to remember:

1. **Conflict**: same memory location, at least one write. Two reads never race.
2. **Not both atomic**: operations on `std::atomic<T>` are excluded. Two atomic writes to the
   same variable from two threads are a race in the English sense but not in the standard's
   sense; the result is one of the two values, well defined.
3. **No happens-before**: if something orders them (a mutex unlock then lock, a thread join,
   an atomic release then acquire), there is no race. Module 07 defines happens-before properly.
   For now: a mutex makes the unlock happen before the next lock, so everything inside the
   critical section is ordered.

"Memory location" means a scalar object or a maximal sequence of adjacent bit-fields. Two
different `int` members of the same struct are different memory locations: writing `s.a` in one
thread and `s.b` in another is not a race (it may be slow; see module 10 on false sharing). Two
bit-fields in the same word usually *are* the same memory location.

### 2.2 Why undefined and not "unspecified value"

Because every optimising compiler already assumes single-threaded semantics for non-atomic
variables, and the committee chose to legalise what compilers do rather than forbid optimisation.
Concretely the compiler may:

- **Keep a variable in a register** across a loop. `while (!done) {}` with a plain `bool done`
  becomes `if (!done) for(;;);` because nothing in *this* thread writes `done`. GCC does this at
  `-O2`. The thread spins forever even after another thread sets `done = true`.
- **Reorder** loads and stores to different addresses when the single-threaded result is the
  same. `x = 1; flag = true;` can be emitted as `flag = true; x = 1;`.
- **Invent loads and stores.** A compiler may read a variable twice where you wrote one read, or
  speculatively write a value and write it back if a branch is not taken. Each of these is
  harmless single-threaded and catastrophic if another thread is watching.
- **Assume a race-free program is deadlock-free** when hoisting a lock... and so on. The list is
  long and grows with every compiler release.

Making races UB means the compiler never has to reason about them. Making them "unspecified value"
would forbid all of the above and cost measurable performance on every program, including the
single-threaded ones. Java made the other choice (races produce some observed value, with strict
rules); the cost shows up in what the JIT may not do.

### 2.3 Torn reads and writes

Even without the compiler doing anything clever, the hardware may not write your value in one
piece. A **torn** access is one where a reader sees half of an old value and half of a new one.

- A naturally aligned 1, 2, 4 or 8 byte access on x86-64 is single-copy atomic in hardware. You
  will not see a torn `int`.
- An 8-byte access that crosses a cache-line boundary (misaligned) can tear on x86. It will tear
  on ARM.
- A 16-byte struct `{ int64_t balance; int64_t version; }` written with two 8-byte stores can
  always be seen half-written by another core. A reader may see new balance with old version.
- A `long double`, a `__int128`, a `std::string` (pointer + size + capacity): all torn.

Hardware atomicity is not language atomicity. The compiler can still split an 8-byte store into
two 4-byte stores if it wants to, and it sometimes does for constants (two `mov dword` with
immediates instead of one `mov qword` through a register). Only `std::atomic` guarantees
"all or nothing".

### 2.4 Compiler reordering

```cpp
int data = 0;
bool ready = false;

// thread A                 // thread B
data = 42;                  while (!ready) {}
ready = true;               std::cout << data;   // may print 0
```

Three separate reasons B can print 0:

1. Compiler keeps `ready` in a register in B: infinite loop (not 0, but also not 42).
2. Compiler swaps the two stores in A because they are to different addresses and single-threaded
   semantics do not care.
3. Even if the compiler emits them in order, the CPU can make them visible in another order
   (next section).

The fix is one of: a mutex around both sides, or `std::atomic<bool> ready` with release/acquire
(module 07). `volatile` does not fix it; it stops reason 1 and sometimes reason 2 but never
reason 3, and it does not create happens-before so the program is still UB.

### 2.5 CPU reordering

A modern core executes out of order and buffers stores. On x86-64 the rule (the TSO model, Total
Store Order) is: stores can be delayed in a store buffer and become visible to other cores later
than subsequent loads from this core execute. Loads are not reordered with other loads, stores are
not reordered with other stores. So on x86 the example above is actually safe at the hardware
level once the compiler is prevented from reordering; `ready` cannot become visible before `data`.

On ARM and POWER, stores to different addresses may become visible in any order and loads may be
satisfied out of order. The example genuinely prints 0 on a phone. Code that "works" on your x86
laptop because of TSO is still UB and will fail the day it is built for an ARM server (AWS
Graviton, Apple silicon, Ampere).

The classic demonstration is **store buffering**:

```
x = y = 0 initially
thread A:  x = 1;  r1 = y;
thread B:  y = 1;  r2 = x;
```

Can `r1 == 0 && r2 == 0`? In a sequentially consistent world, no: one of the stores happened
first and the other thread's load must see it. On real x86 hardware, yes, routinely: both stores
sit in their store buffers while both loads read the old 0 from cache. Module 07 shows the same
test with atomics and memory orders.

### 2.6 "It worked on my machine"

Races are timing-dependent. Things that hide a race:

- Debug builds (`-O0`): the compiler does not keep values in registers, so reason 1 above
  disappears. The race is still there.
- Single-core VM or a heavily loaded machine: threads rarely run truly simultaneously.
- Adding `printf` for debugging: the I/O takes a lock and tens of microseconds, which serialises
  the threads. The race disappears while you are looking at it and returns in production.
- x86 TSO hiding the CPU-reordering class of bugs.
- Small test data: a race on a hash table with 10 entries hits the same bucket rarely.

Things that expose it: `-O2`, many cores, ARM, running for hours, and ThreadSanitizer.

### 2.7 ThreadSanitizer in one paragraph

TSan (`-fsanitize=thread`) instruments every memory access and every synchronisation call. It
keeps, per 8-byte memory word, a small history of recent accesses tagged with thread id and a
vector clock. When a new access conflicts with a recorded one and the vector clocks say neither
happened before the other, it reports a race with both stack traces. It finds races that did not
actually produce a wrong result on this run, because it checks the ordering rules, not the
outcome. Cost: 5x to 15x slowdown and 5x to 10x memory (typical, from the LLVM documentation).
Module 12 goes deep; for now just run it.

```sh
g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m02_race_demo.cpp -o race && ./race
```

## 3. Code

File: `code/m02_race_demo.cpp`

```cpp
#include <thread>
#include <iostream>
#include <vector>
#include <atomic>
#include <cstdint>

// Demo 1: lost updates. Two threads increment a plain int. Expected 2,000,000; you will get less.
int plain_counter = 0;

void bump_plain(int n) {
    for (int i = 0; i < n; ++i) ++plain_counter;   // load, add, store: three steps, interleavable
}

// Demo 2: tearing. A 16-byte record written as two 8-byte halves. Reader checks invariant a == b.
struct Record { std::int64_t a; std::int64_t b; };
Record rec{0, 0};
std::atomic<bool> stop{false};

void writer_tear() {
    std::int64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        ++v;
        rec.a = v;        // WHY two separate stores: this is what any struct assignment compiles to
        rec.b = v;        // reader may observe a == v, b == v-1
    }
}

long reader_tear() {
    long torn = 0;
    for (int i = 0; i < 5'000'000; ++i) {
        Record snapshot = rec;         // plain copy: itself racy; this is the demo
        if (snapshot.a != snapshot.b) ++torn;
    }
    return torn;
}

int main() {
    // Demo 1
    std::thread t1(bump_plain, 1'000'000);
    std::thread t2(bump_plain, 1'000'000);
    t1.join(); t2.join();
    std::cout << "plain counter: " << plain_counter << " (expected 2000000)\n";

    // Demo 2
    std::thread w(writer_tear);
    long torn = reader_tear();
    stop.store(true, std::memory_order_relaxed);
    w.join();
    std::cout << "torn reads observed: " << torn << '\n';

    // Both demos are undefined behaviour on purpose. Compile with -fsanitize=thread and it will
    // report both races with the two stacks involved. That is the point of this file.
    return 0;
}
```

What you will see at `-O2` on a multi-core WSL: the counter is well short of 2,000,000 and the torn
count is non-zero. At `-O0` the counter is still short. With `-fsanitize=thread`, TSan prints
`WARNING: ThreadSanitizer: data race` for both, naming `bump_plain` and `writer_tear`/`reader_tear`.

Note the use of `std::atomic<bool> stop`. That one *is* correct: the loop termination flag must
be atomic or the compiler may hoist the load out of the loop (reason 1 in 2.4). The racy parts
are only the ones the comments mark.

## 4. Under the hood (senior-level)

### What `++plain_counter` compiles to

At `-O2`, GCC emits for the loop body something like:

```
    add  DWORD PTR plain_counter[rip], 1
```

A single instruction. It is still not atomic: the CPU performs it as load, add, store internally,
and another core can store between the load and the store. x86 provides the `lock` prefix to make
it atomic (`lock add`), which is what `std::atomic<int>::fetch_add` compiles to. The `lock` prefix
asserts exclusive ownership of the cache line for the duration of the read-modify-write. Without
it, two cores do `read 100 / read 100 / write 101 / write 101`.

Often GCC does better than that for the loop: it notices nothing else in the function touches
`plain_counter`, so it keeps the running total in a register and stores once at the end:

```
    mov  eax, DWORD PTR plain_counter[rip]
    add  eax, 1000000
    mov  DWORD PTR plain_counter[rip], eax
```

Now the "race window" is the whole function. One thread's million increments can be entirely
overwritten by the other's. This is why the measured counter can be exactly 1,000,000. The
compiler is allowed to do this precisely because the race is UB.

### Store buffers and cache coherence, briefly

Each core has a store buffer: a FIFO of stores that have executed but not yet reached L1 cache.
A store drains when the core gets the cache line in exclusive state (MESI, module 10). Loads from
the same core check the store buffer first (store-to-load forwarding), so the core sees its own
stores immediately, but other cores see them only after the drain. That gap is the CPU reordering
of 2.5. An `mfence` instruction (or any `lock`-prefixed instruction) waits for the store buffer to
drain; that is what `seq_cst` stores cost on x86 and what `release` stores do not.

### Why `volatile` is not the answer

`volatile` tells the compiler every access is an observable side effect: do not elide, do not
reorder *with other volatile accesses*, do not cache in a register. It was designed for memory
mapped hardware registers. It says nothing about the CPU (no fences), nothing about atomicity
(a `volatile` 16-byte struct still tears), and nothing about ordering with non-volatile accesses
(the `data = 42` store can still move after `ready = true`). MSVC on x86 historically added
acquire/release semantics to `volatile` as an extension (`/volatile:ms`), which is why Win32 code
you have seen gets away with it. Portable C++ does not. Use `std::atomic`.

### What TSan actually tracks

For every 8-byte memory word TSan keeps a shadow of 4 slots (configurable), each recording
`(thread id, epoch, address offset and size, is_write)`. Each thread has a vector clock: its own
epoch and the latest epoch it has synchronised with from every other thread. `mutex.unlock`
publishes the thread's clock into the mutex; `mutex.lock` merges the mutex's clock into the
locker. An access races with a shadow slot if it conflicts, is from a different thread, and the
current thread's clock for the other thread is older than the recorded epoch. This is why TSan
has no false positives when all synchronisation uses primitives it knows (pthread, std::atomic,
std::mutex) and why it reports races that happened to produce the right value this time.

## 5. In production

- **The counter you never locked.** Statistics counters (`requests_served++`) in your server are
  the most common unprotected shared write in C servers. They look harmless. With the compiler
  optimisation above, a worker can overwrite another's hundred increments. Fix: `std::atomic<long>`
  with `memory_order_relaxed`, or per-thread counters summed on read (module 10).
- **The flag you never made atomic.** `volatile BOOL g_shutdown` polled by workers. On MSVC x86
  it works because of the `/volatile:ms` extension. Port it to GCC on ARM and the workers never
  stop. Portable fix: `std::atomic<bool>`.
- **The struct copied under no lock.** Reading a config struct that another thread updates
  "rarely" is a torn read waiting for the day the update lands mid-copy. In a loan server,
  interest rate 7.25 with the old rounding mode is the kind of fault that reaches the customer.
  Fix: immutable snapshot swapped through an atomic pointer (module 11).
- **Heisenbugs.** If adding logging makes a bug disappear, assume a race until proven otherwise.
  Do not ship the logging as the fix.
- **Make TSan a CI job.** Build the test suite with `-fsanitize=thread` and run it nightly. It is
  the cheapest way to find these before customers do. If your production build is Windows-only,
  the test suite still runs on WSL; races in portable code are the same on both.

## 6. Try it

1. **(easy)** Build `m02_race_demo.cpp` at `-O0` and `-O2` and run each five times. Record the
   counter values. Done when you can explain why `-O2` sometimes gives exactly 1,000,000.
2. **(easy)** Build with `-fsanitize=thread` and read the report. Identify the two stack traces,
   the "previous write" and the "current read", and the thread creation sites. Done when you can
   point to the line number of both accesses.
3. **(medium)** Write the `while (!ready) {}` spin with a plain `bool ready` and a second thread
   that sleeps 100 ms then sets it. Build at `-O2`. Observe it hangs. Look at the assembly
   (`g++ -O2 -S`) and find the loop that never reloads `ready`. Then change to `std::atomic<bool>`
   and confirm it terminates. Done when you have both assembly snippets side by side.
4. **(medium)** Write the store-buffering litmus test from 2.5 with two threads, a start barrier
   (spin on an atomic counter so both start together), and run it 1,000,000 times counting how
   often `r1 == 0 && r2 == 0`. Done when you have seen a non-zero count on x86 with plain ints or
   relaxed atomics, and a zero count with `seq_cst` atomics (come back after module 07 if needed).
5. **(hard)** Take a piece of C from your server (or write a 100-line stand-in) that has a
   statistics struct updated by workers under no lock. List every memory location, classify each
   access as read or write, and mark each pair that conflicts with no happens-before. Rewrite with
   the minimum synchronisation needed. Done when TSan is quiet on the rewrite.

## 7. Interview questions

**Q: Define a data race.**
Two accesses to the same memory location from different threads, at least one a write, at least
one non-atomic, with no happens-before relationship between them. Any program containing one has
undefined behaviour. Two reads are not a race, and two atomic operations are not a race.

**Q: Why is a data race undefined behaviour rather than just an unpredictable value?**
Because optimising compilers assume single-threaded semantics for non-atomic variables: they
hoist loads out of loops, keep variables in registers, reorder independent stores, and invent
temporary stores. Defining a race result would forbid those optimisations everywhere. UB lets the
compiler ignore the possibility, and puts the burden on the programmer to use atomics or locks.

**Q: Can `int x` tear on x86-64?**
An aligned 4-byte `int` does not tear at the hardware level. But a struct of two ints, a misaligned
int, a 16-byte value, or a value the compiler chooses to write in two halves all can. Hardware
atomicity for a particular size is not a language guarantee; only `std::atomic` is.

**Q: Why doesn't `volatile` fix a race?**
It prevents the compiler from eliding or caching the access and from reordering it with other
volatile accesses. It adds no CPU fence, no atomicity for multi-word types, and no ordering with
non-volatile accesses, and the standard still calls the program UB. MSVC's `/volatile:ms` adds
acquire/release on x86 as an extension, which is why Win32 code sometimes gets away with it.

**Q: Explain store buffering and why `r1 == r2 == 0` can happen.**
Each core buffers its stores before they reach cache. Thread A stores `x = 1` into its buffer and
loads `y`, which is still 0 in cache. Thread B does the mirror image. Both loads complete before
either store drains. Sequential consistency forbids this; real x86 permits it. A full fence or
`seq_cst` atomics prevents it.

**Q: How does ThreadSanitizer find a race that did not produce a wrong answer?**
It tracks happens-before with vector clocks and keeps a per-word history of recent accesses. It
reports when two accesses conflict and the clocks prove neither is ordered before the other,
regardless of what values resulted. So it finds the bug in the ordering, not the symptom.

## 8. Recap

- Data race = conflicting accesses, different threads, at least one non-atomic, no
  happens-before. Result: undefined behaviour.
- Compilers assume no races and will hoist, register-cache, reorder and invent accesses.
- CPUs reorder too: x86 delays stores behind loads (TSO); ARM reorders almost everything.
- Torn reads happen for anything the hardware cannot write in one piece, including every
  multi-word struct.
- `volatile` is for hardware registers, not for threads.
- "Works on my machine" usually means x86 TSO plus luck plus `-O0`.
- `std::atomic<bool>` for flags, mutex or atomics for everything shared, immutable snapshots for
  rarely-changing config.
- Run the test suite under `-fsanitize=thread`; it reports ordering violations, not just wrong
  values.
