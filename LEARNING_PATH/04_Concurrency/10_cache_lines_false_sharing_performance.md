# 10. Cache lines, false sharing and performance

**Time:** ~2.5 hours   **Prerequisites:** 07

## 1. In plain words

Your CPU does not read memory one byte or one integer at a time. It reads 64-byte blocks called
cache lines, and it keeps copies of recently used lines in small, fast caches next to each core.
When two cores both have a copy of the same line and one of them writes to it, the other core's
copy is thrown away and must be fetched again. That is correct behaviour (both cores always agree
on the value) but it is slow: a fetch from another core's cache costs tens of nanoseconds, a
hundred times more than a hit in your own L1.

False sharing is when two threads write to *different* variables that happen to sit in the *same*
64-byte line. Neither thread cares about the other's variable, but the hardware does not know
that; it bounces the line between the cores on every write. Two per-thread counters declared next
to each other can run ten times slower than they would with 64 bytes of padding between them.

Analogy: two clerks each keep their own tally on the same sheet of paper. Only one can hold the
sheet at a time. They spend all day passing the sheet back and forth even though they never read
each other's numbers. Give each clerk their own sheet and the problem vanishes. This module teaches
you to recognise the shared sheet, measure it, and separate the sheets with `alignas(64)`.

## 2. The real thing

### 2.1 Cache hierarchy in numbers

Typical figures for a modern x86-64 server core (the shape matters more than the exact values;
measure your own machine):

| Level | Size | Latency | Shared by |
|---|---|---|---|
| L1 data | 32 to 48 KB | ~4 cycles (~1 ns) | one core |
| L2 | 1 to 2 MB | ~14 cycles (~4 ns) | one core |
| L3 | 30+ MB | ~40 to 70 cycles (~15 to 25 ns) | all cores on the socket |
| another core's L1/L2 (coherence miss) | | ~40 to 100 ns | |
| DRAM | | ~80 to 120 ns | |
| another socket's memory (NUMA) | | ~150 to 250 ns | |

The unit of transfer between every level is the 64-byte line. `std::hardware_destructive_
interference_size` (C++17, `<new>`) is the standard's name for "how far apart to put things to
avoid false sharing"; on GCC x86-64 it is 64. Some recent Intel cores prefetch pairs of lines
(128-byte "spatial prefetcher"), so 128-byte separation is sometimes used for the hottest data.

### 2.2 MESI in plain words

Every cache line in every core's cache is in one of four states:

- **M**odified: this core has the only copy and it is dirty (newer than memory).
- **E**xclusive: this core has the only copy and it matches memory.
- **S**hared: this core has a copy, other cores may too, all match memory. Read-only.
- **I**nvalid: this core's copy is stale; must re-fetch before use.

Rules: to **write**, a core needs the line in M or E. If it is in S, it must broadcast an
"invalidate" to every other core holding it (they go to I) and wait for acknowledgements, then it
moves to M. If it is in I, it must first fetch the line ("read for ownership"). To **read**, S is
enough; if the line is in I, fetch it from whoever has it (another core's cache or memory) and
enter S.

That is the whole protocol at the level you need. The consequence: a line that is **written by
one core and read or written by another** goes S -> I -> (fetch) -> S -> I -> ... on every
alternation. Each transition is a coherence message across the on-chip network. A line that is
only read stays S in every cache forever at L1 speed. A line that is written by one core and never
touched by another stays M in that core's cache at L1 speed. The expensive case is exactly one:
**shared and written**.

### 2.3 False sharing

```cpp
struct Counters {
    long thread0_count;   // offset 0
    long thread1_count;   // offset 8   <- same 64-byte line as thread0_count
    long thread2_count;   // offset 16
    long thread3_count;   // offset 24
};
```

Four threads each incrementing their own counter: no data race (different memory locations), yet
every increment invalidates the other three cores' copies. Measured slowdown versus padded
counters is commonly 5x to 20x (typical; the benchmark in section 3 gives your number).

Fix:

```cpp
struct alignas(64) PaddedCounter { long count; };   // sizeof == 64; each in its own line
PaddedCounter counters[4];
```

`alignas(64)` on the struct makes its size a multiple of 64 and its address 64-aligned, so
consecutive array elements are on different lines. Alternatively pad explicitly:
`long count; char pad[64 - sizeof(long)];`. C++17 also offers
`std::hardware_destructive_interference_size`, but GCC 12 and later warn when you use it
(`-Winterference-size`) because its value can change with `-march`/`-mtune` flags and silently
give different struct layouts in different translation units. Under `-Werror` that warning is a
build failure, so most production code simply writes 64.

Where false sharing hides:

- Per-thread counters or flags in an array or a struct (the classic).
- A mutex next to the data it protects *when other threads read that data without the lock*: not
  usual with a mutex, but an `std::atomic<bool> stop` flag sitting next to a hot counter is.
- The head and tail of a queue (module 08's SPSC ring aligns them for this reason).
- `std::vector<std::atomic<int>>` indexed by thread id.
- Struct members where one is hot-written by one thread and another is hot-read by many.
- Heap objects allocated back to back by different threads (allocators like jemalloc/tcmalloc use
  per-thread arenas partly for this reason).

### 2.4 True sharing

One counter incremented by all threads is *true* sharing: everyone writes the same variable. Padding
cannot help. The fix is structural: per-thread (or per-core) counters, each on its own line,
summed when read. Reads become O(threads) but writes stop contending. This is the "striped counter"
or `LongAdder` pattern, and it is how every serious metrics library works.

```cpp
class StripedCounter {
    struct alignas(64) Slot { std::atomic<long> v{0}; };
    std::vector<Slot> slots_;
public:
    explicit StripedCounter(std::size_t n) : slots_(n) {}
    void add(std::size_t tid, long d) { slots_[tid % slots_.size()].v.fetch_add(d, std::memory_order_relaxed); }
    long read() const { long s = 0; for (auto& x : slots_) s += x.v.load(std::memory_order_relaxed); return s; }
};
```

The read is not a consistent snapshot (slots may change while summing); for a metric that is fine.
For an exact total you need a lock or a quiescent point.

### 2.5 Amdahl's law

If fraction `s` of a job is inherently serial and `1 - s` is perfectly parallel, the speedup on
`N` processors is

```
speedup(N) = 1 / (s + (1 - s) / N)
```

With `s = 0.05` (5% serial), 8 cores give 5.9x, 64 cores give 14.5x, infinite cores give 20x. The
serial fraction caps you. In a server, the serial fraction is whatever runs under one global lock:
the queue mutex, the logger, the connection table. Every nanosecond under a global lock is in `s`.
This is why "the lock is only held for a microsecond" matters when there are a million requests
per second: that is one full second of serial time per second, `s = 1`, speedup 1.

Gustafson's counterpoint: as you add cores you usually also add work (more clients), so the
parallel part grows and the serial part becomes a smaller fraction. True for throughput, not for
the latency of a single request.

### 2.6 How to benchmark concurrency honestly

- **Warm up.** The first iterations pay for page faults, cold caches, thread creation, CPU
  frequency ramp. Run for a second before timing.
- **Steady state.** Time a fixed duration (not a fixed count) and count operations, so slow
  configurations do not take forever and the measurement window is the same.
- **Pin threads** (`taskset`, `pthread_setaffinity_np`, `SetThreadAffinityMask`) when measuring
  cache effects, so the OS does not migrate threads mid-run and smear the result. Report whether
  you pinned.
- **Repeat and report the distribution** (min, median, p99), not one number. Variance between
  runs is itself a finding.
- **Control the machine.** No browser, no indexer. On WSL, remember the host is also running.
- **Measure the right thing.** Throughput (ops/s) and latency (time per op, percentiles) are
  different and often move in opposite directions. A server cares about p99 latency under a target
  throughput.
- **Use counters when available.** `perf stat -e cache-misses,L1-dcache-load-misses` on Linux
  (not always available under WSL). On Windows, VTune or `xperf`.
- **Beware the compiler.** A loop whose result is unused is deleted. Accumulate into a `volatile`
  sink or return the result from `main`.

## 3. Code

File: `code/m10_false_sharing.cpp`

```cpp
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <new>
#include <cstddef>

// The standard name would be std::hardware_destructive_interference_size, but GCC 12+ warns
// (-Winterference-size) that its value can differ between compiler flags, which breaks -Werror
// and can silently change struct layout across translation units. Production code pins 64,
// the cache line size on every current x86-64 and most ARM server cores. See code/m10_false_sharing.cpp.
constexpr std::size_t kLine = 64;

struct Packed  { std::atomic<long> v{0}; };                       // 8 bytes: 8 per line
struct alignas(kLine) Padded { std::atomic<long> v{0}; };         // one per line

template <class Slot>
double run(const char* label, int threads, std::chrono::milliseconds dur) {
    std::vector<Slot> slots(threads);
    std::atomic<bool> stop{false};
    std::vector<long> local(threads);
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            long n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                slots[t].v.fetch_add(1, std::memory_order_relaxed);   // WHY relaxed: pure counter
                ++n;
            }
            local[t] = n;
        });
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    long total = 0; for (long n : local) total += n;
    double mops = static_cast<double>(total) / dur.count() / 1000.0;
    std::cout << label << " threads=" << threads << " sizeof(slot)=" << sizeof(Slot)
              << " : " << mops << " M increments/s\n";
    return mops;
}

// True sharing: all threads hit one counter.
double run_shared(int threads, std::chrono::milliseconds dur) {
    alignas(kLine) std::atomic<long> one{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] { while (!stop.load(std::memory_order_relaxed)) one.fetch_add(1, std::memory_order_relaxed); });
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    double mops = static_cast<double>(one.load()) / dur.count() / 1000.0;
    std::cout << "true-shared threads=" << threads << " : " << mops << " M increments/s\n";
    return mops;
}

int main() {
    using namespace std::chrono_literals;
    std::cout << "cache line assumed: " << kLine << " bytes\n";
    for (int t : {1, 2, 4, 8}) {
        run<Packed>("packed (false sharing)", t, 500ms);
        run<Padded>("padded (no sharing)   ", t, 500ms);
        run_shared(t, 500ms);
        std::cout << '\n';
    }
    return 0;
}
```

Expected shape (your numbers will differ): padded scales nearly linearly with threads; packed is
flat or *falls* as threads are added; true-shared falls hardest. The `stop` flag is read by every
thread but written once, so it stays in S state everywhere and does not interfere.

## 4. Under the hood (senior-level)

### Coherence traffic for one false-shared increment

Core 0 does `lock add [line+0], 1`; core 1 does `lock add [line+8], 1`. Sequence on a MESI
machine with both lines initially S:

1. Core 0 sends invalidate for the line; core 1 acknowledges, marks I. Core 0 -> M, performs add.
2. Core 1 needs the line: sends read-for-ownership. Core 0 must write back (or forward) its M copy
   and go to I. Core 1 -> M, performs add.
3. Repeat.

Each step is a round trip on the ring/mesh interconnect: ~40 to 100 ns. An L1-resident increment
is ~1 ns. Hence the order-of-magnitude penalty.

Real Intel cores use MESIF (F = Forward, to designate which sharer answers a request) and AMD uses
MOESI (O = Owned, dirty but shared). The extra states reduce memory traffic; the S/I bouncing is
the same.

### Why `lock`-prefixed RMW is worse than a plain store under sharing

A plain store can retire into the store buffer and the core moves on; the coherence miss is paid
asynchronously (until the buffer fills). A `lock add` must hold the line and complete before
subsequent instructions retire (it is a full fence), so the core stalls for the entire round trip.
Relaxed `fetch_add` on x86 is still `lock xadd`; the memory order does not help here. Only
restructuring (per-thread slots) does.

### Store-buffer and prefetcher interactions

Adjacent-line prefetching on Intel can pull in the *neighbouring* 64-byte line, so two padded
variables on adjacent lines can still interfere slightly. That is the origin of 128-byte padding
advice. Measure before paying double the memory.

### NUMA

On a two-socket machine, memory is attached to one socket or the other. A thread on socket 1
writing to a line homed on socket 0 pays an extra hop. False sharing across sockets is roughly
twice as bad as within a socket. `numactl --hardware` shows the topology; `numactl --cpunodebind
--membind` pins. For a single-socket developer box this does not apply; for a production
server it may, and "first touch" allocation (memory is homed where the thread that first writes
it runs) is the rule to know.

### Measuring with hardware counters

`perf stat -e L1-dcache-load-misses,LLC-load-misses,instructions,cycles ./m10` shows miss counts;
the false-shared run has orders of magnitude more LLC misses for the same instruction count.
`perf c2c` (cache-to-cache) identifies false-shared lines by address in a real program. Intel
VTune's "memory access" analysis does the same on Windows. If your WSL kernel lacks perf, run the
binary on a native Linux box or reason from the timing.

## 5. In production

- **Per-worker stats in an array.** `struct WorkerStats { long requests; long errors; } stats[64];`
  updated by each worker: 16 bytes per worker, four workers per line, false sharing on every
  request. Pad to 64 bytes. This is a ten-minute change with a measurable throughput gain at
  high request rates, and a good story for "a performance fix you made".
- **The global request counter.** `InterlockedIncrement(&g_requests)` on every request from 64
  workers is true sharing. At 10,000 requests per second nobody notices. At 1,000,000 the counter
  is a bottleneck. Striped counter.
- **The mutex and its data.** A `std::mutex` (40 bytes) followed by the data it protects shares a
  line with it. That is fine and even good: the lock acquirer will need the data next. Padding
  between a lock and its data is a common mis-optimisation.
- **Measuring on the wrong machine.** A 4-core laptop shows a 2x false-sharing penalty; the
  32-core production box shows 15x. Scaling bugs appear with cores. Test on the biggest box you
  can get before concluding "it's fine".
- **Amdahl in a loan server.** If every request takes a global "audit sequence number" lock for
  2 µs, at 10,000 req/s the serial fraction is 2%; at 100,000 it is 20% and the box cannot scale
  past 5x no matter how many cores. Replace with per-thread sequence ranges or an atomic
  `fetch_add` (which is still true sharing but ~50 ns instead of a mutex's microsecond).
- **3 AM symptom:** throughput falls when you *add* workers. Almost always true or false sharing
  on a hot line, or a lock convoy. `perf c2c` or VTune finds the line; the fix is padding or
  striping.

## 6. Try it

1. **(easy)** Build and run `m10_false_sharing.cpp`. Record the three series. Done when you have
   the ratio padded/packed at your maximum thread count.
2. **(easy)** Change `alignas(kLine)` to `alignas(32)` and `alignas(128)`. Done when you can
   explain the 32 result (two slots per line) and whether 128 differs from 64 on your CPU.
3. **(medium)** Implement `StripedCounter` from 2.4 and compare `add` throughput against a single
   `std::atomic<long>` at 1, 4, 8 threads. Then measure `read()` cost. Done when you can state the
   trade-off in numbers.
4. **(medium)** Pin threads with `pthread_setaffinity_np` (one per core) and re-run the benchmark.
   Compare variance across 5 runs pinned vs unpinned. Done when you have both variances.
5. **(hard)** Take the `ThreadPool` from module 09 and add per-worker statistics (tasks run, total
   task time). Implement them packed and padded, drive the pool with 1,000,000 ten-nanosecond
   tasks, and measure throughput both ways. Then compute the serial fraction implied by the queue
   mutex using Amdahl: measure single-thread throughput and 8-thread throughput, solve for `s`.
   Done when you have `s` and a sentence on what dominates it.

## 7. Interview questions

**Q: What is false sharing?**
Two threads writing to different variables that share a 64-byte cache line. The coherence
protocol invalidates the line in the other core on every write even though the data is logically
independent, so each access becomes a cross-core transfer of tens of nanoseconds instead of an L1
hit. Fix by placing hot per-thread data on separate lines with `alignas(64)` or padding.

**Q: Explain MESI briefly.**
Each cached line is Modified (sole dirty copy), Exclusive (sole clean copy), Shared (clean,
possibly in other caches), or Invalid. Writing requires M or E, so a writer invalidates all other
copies first. Reading requires at least S. Lines that are read-only or written by a single core are
fast; lines written by one core and touched by another bounce and are slow.

**Q: How do you fix a hot shared counter?**
Stripe it: one slot per thread (or per core), each on its own cache line, updated with a relaxed
atomic add; sum the slots on read. Writes stop contending; reads become O(threads) and are not an
instantaneous snapshot, which is acceptable for metrics.

**Q: What does Amdahl's law say about a global lock?**
Speedup is bounded by 1 over the serial fraction; time spent under a global lock is serial. A lock
held for 2 µs per request is 2% serial at 10,000 req/s and 20% at 100,000, capping speedup at 5x.
Shrink or shard anything held under a global lock on the hot path.

**Q: Why does `memory_order_relaxed` not help a contended `fetch_add`?**
On x86 every atomic RMW is a `lock`-prefixed instruction, which is a full fence and must own the
cache line regardless of the C++ order. The cost is the line transfer, not the ordering. Only
restructuring to avoid the shared line helps.

**Q: How would you benchmark a concurrent data structure honestly?**
Warm up, run for a fixed duration at steady state and count operations, pin threads, repeat and
report a distribution, measure both throughput and latency percentiles, prevent the compiler from
deleting the work, use hardware counters to confirm the mechanism, and test on a machine with as
many cores as production.

**Q: Is it a problem that a mutex shares a cache line with the data it protects?**
Usually not; the thread that acquires the lock will immediately need the data, so having both in
one line saves a miss. Padding between a lock and its data is a common mistake. The problem is a
hot-written variable sharing a line with data that *other* threads read without the lock.

## 8. Recap

- Memory moves in 64-byte lines; L1 hit ~1 ns, cross-core transfer ~40 to 100 ns.
- MESI: writing needs exclusive ownership and invalidates other copies; shared-and-written lines
  bounce.
- False sharing: independent variables on one line. Fix with `alignas(64)` / padding.
- True sharing: one variable, many writers. Fix with striped per-thread slots summed on read.
- `std::hardware_destructive_interference_size` is the standard's name for the padding size, but GCC warns on it; pin 64 in practice.
- `lock`-prefixed RMWs stall on the transfer; relaxed ordering does not help.
- Amdahl: serial fraction caps speedup; global locks are serial fraction.
- Benchmark at steady state, pinned, repeated, with percentiles, on a big machine.
- Throughput falling as threads are added means sharing or a convoy; `perf c2c` finds the line.
