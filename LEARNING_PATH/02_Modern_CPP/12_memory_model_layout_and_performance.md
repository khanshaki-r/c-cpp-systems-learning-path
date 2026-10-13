# 12. Memory layout, allocation and performance

**Time:** ~3.5 hours   **Prerequisites:** 05, 06

## 1. In plain words

A program's speed is mostly decided by how it touches memory, not by how many instructions it
executes. A core can do several additions per nanosecond but waits around 100 ns for a value that is
not in cache. So the questions that matter are: how are my objects laid out (padding, alignment,
size), where do they live (stack vs heap), how often do I allocate, and do my hot loops walk memory
in a cache-friendly order. You already know most of this from C; this module shows how the C++
features map onto it and adds the parts C programmers often skip: cache lines, false sharing, small
buffer optimisation, PMR allocators, benchmarking honestly, and undefined behaviour as a performance
contract.

Analogy: a warehouse. Items on the shelf next to the one you are holding are free to grab (cache
line). Walking to another aisle costs seconds (cache miss). Ordering from the supplier costs days
(heap allocation plus page fault). Layout and allocation policy decide which of these your loop does.

## 2. The real thing

### 2.1 Object layout, alignment, padding

Every type has `sizeof` and `alignof`. A member must sit at an offset that is a multiple of its
alignment, so the compiler inserts **padding**; the struct's size is rounded up to a multiple of its
own alignment (the largest member's) so arrays stay aligned.

```cpp
struct Bad  { char tag; double amount; int id; char flag; };   // 1+7pad, 8, 4, 1+3pad = 24 bytes
struct Good { double amount; int id; char tag; char flag; };   // 8, 4, 1, 1, 2pad     = 16 bytes
```

Sort members by alignment descending to remove padding. `alignas(64)` over-aligns (one object per
cache line); `#pragma pack(1)` removes padding for wire formats at the cost of misaligned access (use
`memcpy` in and out). `offsetof` and `static_assert` pin the layout. `std::is_standard_layout_v<T>`
tells you the layout is C-compatible; a vptr (module 09) adds 8 bytes at the front.

### 2.2 Cache lines and false sharing

Memory moves between cores and caches in 64-byte **lines**. Reading one `int` pulls its whole line
into L1; the next 15 ints are then free. Two variables in the same line that are written by two
different cores force the line to bounce between the cores' caches on every write: **false sharing**.
It looks like a lock without a lock. Fix: `alignas(64)` the per-thread fields so each gets its own
line. Full treatment in 04_Concurrency; `m12_false_sharing_bench.cpp` shows the effect.

### 2.3 Stack vs heap

Stack allocation is a subtraction from `rsp`: free, cache-hot, and freed automatically. Heap
allocation (`new`, `malloc`) costs tens to hundreds of nanoseconds (glibc `malloc` fast path with a
thread cache is around 20 ns, typical; a fresh page from the kernel is far more), takes a lock or a
per-thread arena, scatters your data, and must be freed. Modern C++ hides heap use inside `vector`,
`string`, `unique_ptr`, `std::function`, so you must know which operations allocate:

| Allocates | Does not |
|---|---|
| `std::vector` growth, `std::string` > 15 chars, `std::map`/`unordered_map` node per element, `make_unique`/`make_shared`, `std::function` with a large callable, `std::any`, `std::stringstream` | `std::array`, `std::optional`, `std::variant`, `std::string_view`, `std::pair`/`tuple`, small `std::string` (SSO), lambdas, `std::function` with a small callable, `std::span` (C++20) |

### 2.4 Allocation patterns

- **Reserve once.** `v.reserve(n)`, `m.reserve(n)`, `s.reserve(n)`.
- **Reuse buffers.** Keep a `std::vector<char>` member and `clear()` it per request instead of
  allocating a new one; capacity is retained.
- **Arena / bump allocators.** One large block; allocation is a pointer increment; "free" is resetting
  the pointer at the end of a request. Your C arena with ownership contracts. In C++17 the standard
  version is `std::pmr::monotonic_buffer_resource` plus `std::pmr::vector`, `std::pmr::string`.
- **Pools.** Fixed-size blocks in a free list for objects of one type (connections, messages):
  `std::pmr::unsynchronized_pool_resource` / `synchronized_pool_resource`.
- **Small buffer optimisation (SBO).** Store small payloads inline, spill to the heap only when large.
  `std::string` does it (15 bytes), `std::function` (16 bytes), many `small_vector` implementations.

### 2.5 Benchmarking basics

1. Use `std::chrono::steady_clock`, never `system_clock` (it jumps).
2. Build with `-O2` (or whatever production uses). `-O0` numbers are meaningless.
3. Warm up, repeat, take the minimum or median, not the mean.
4. Make the result observable (print it, write to a `volatile`, or use a benchmark library's
   `DoNotOptimize`) or the optimiser deletes the work.
5. Benchmark the realistic size and access pattern: a 1k-element test fits in L1 and says nothing
   about a 100 MB table.
6. Use `perf stat -e cache-misses,instructions,cycles ./bench` to see *why*, not just how long.
   Google Benchmark is the standard library for serious micro-benchmarks.

### 2.6 What `-O2` does

Inlining (the enabler for everything else), constant folding and propagation, dead code elimination,
loop-invariant code motion, strength reduction, common subexpression elimination, register
allocation, instruction scheduling, auto-vectorisation (SSE/AVX for simple loops), devirtualisation
when provable, and tail-call elimination. `-O3` adds more aggressive inlining and loop transforms;
`-Os` optimises for size; `-march=native` enables the CPU's full instruction set (but the binary
then runs only on that CPU class); `-flto` optimises across translation units. The optimiser assumes
your program has no undefined behaviour; that assumption is what makes the next section matter.

### 2.7 Undefined behaviour and why it matters

**Undefined behaviour (UB)**: the standard imposes no requirements. The compiler may assume it never
happens and optimise accordingly, so the program may do anything, including "work" until the next
compiler upgrade. Categories you will hit:

| UB | Example | What the optimiser does with it |
|---|---|---|
| Signed integer overflow | `int x = INT_MAX; x + 1` | assumes `x + 1 > x` always; removes overflow checks like `if (x + 1 < x)` |
| Out-of-bounds access | `arr[n]` | assumes the index is in range; may remove a later bounds check |
| Dereferencing null / dangling | `*p` after `delete p`, returning `int&` to a local | assumes `p` non-null; reorders or deletes checks |
| Strict aliasing violation | `*(float*)&int_value` | assumes an `int*` and `float*` never alias; may reorder loads/stores across them |
| Data race | two threads write a non-atomic `int` | assumes single-threaded view; hoists loads out of loops (spin loop never exits) |
| Uninitialised read | `int x; if (x) ...` | may treat `x` as any value, both branches "taken" |
| Shift by >= width or negative | `1 << 32` on 32-bit int | arbitrary |
| Modifying a `const` object, violating `noexcept`-adjacent rules, misaligned access through pointer | | |

Tools: `-fsanitize=undefined` (UBSan) reports most of these at run time, `-fsanitize=address`
(ASan) catches memory errors, `-fsanitize=thread` (TSan) catches data races, `-Wall -Wextra` plus
`-Wstrict-aliasing -Wshadow -Wconversion` catch some at compile time. Use `std::memcpy` for type
punning (the compiler turns it into a register move), `unsigned` or `__builtin_add_overflow` for
wrap-around arithmetic, `std::launder` and `std::bit_cast` (C++20) where needed.

## 3. Code

`code/m12_padding_alignment.cpp` prints sizes, alignments and offsets:

```cpp
struct Bad  { char tag; double amount; int id; char flag; };   // sizeof 24
struct Good { double amount; int id; char tag; char flag; };   // sizeof 16
struct alignas(64) CacheLineBox { int counter; };              // sizeof 64, alignof 64
#pragma pack(push, 1)
struct WireHeader { std::uint8_t type; std::uint32_t length; std::uint16_t flags; };   // sizeof 7 (12 unpacked)
#pragma pack(pop)
std::cout << offsetof(Bad, amount) << ' ' << offsetof(Bad, flag) << '\n';   // 8 20
static_assert(sizeof(Good) % alignof(Good) == 0);
```

`code/m12_arena_pmr.cpp` is your C arena in C++, then the `std::pmr` version:

```cpp
class Arena {
    void* allocate(std::size_t n, std::size_t align) {
        std::size_t aligned = (used_ + align - 1) & ~(align - 1);   // round up
        if (aligned + n > cap_) throw std::bad_alloc();
        used_ = aligned + n;
        return buf_ + aligned;                                      // no header, no free list, no lock
    }
    template <class T, class... Args> T* make(Args&&... args) {
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);   // placement new
    }
    void reset() noexcept { used_ = 0; }                            // O(1) free-all (trivially destructible T only)
};

std::byte stack_buf[2048];
std::pmr::monotonic_buffer_resource pool(stack_buf, sizeof stack_buf, &upstream);
std::pmr::vector<std::pmr::string> names(&pool);   // every allocation (vector + each string) comes from the pool
```

The `CountingResource` in the file shows the number of heap calls: with the pool, zero until the 2 KB
buffer is exhausted; without it, one per vector growth plus one per long string.

`code/m12_false_sharing_bench.cpp` (build with `-O2`) and `code/m12_bench_basics.cpp` (vector vs
list, linear vs binary search, best-of-N timing) give you numbers from your own machine:

```cpp
struct Adjacent { std::atomic<std::uint64_t> a{0}; std::atomic<std::uint64_t> b{0}; };            // one line
struct Padded { alignas(64) std::atomic<std::uint64_t> a{0}; alignas(64) std::atomic<std::uint64_t> b{0}; };  // two lines
// two threads, each hammering its own counter: expect Adjacent to be several times slower (typical: 3x to 10x)

template <class F> double time_ms(F&& f, int reps = 5) {   // best-of-N
    double best = 1e300;
    for (int r = 0; r < reps; ++r) { auto t0 = steady_clock::now(); f(); best = std::min(best, elapsed_ms(t0)); }
    return best;
}
std::uint64_t sink = 0;   // every result feeds 'sink', which is printed: the optimiser cannot delete the work
```

## 4. Under the hood (senior-level)

**Why alignment exists.** Loads of a naturally aligned 8-byte value are one memory transaction. A
misaligned 8-byte load that crosses a cache line is two transactions and, on x86, is legal but
slower; on ARM it may fault or be emulated. Atomic operations *require* alignment: a misaligned
`std::atomic<uint64_t>` is not atomic. SIMD loads (`movaps`) fault on misalignment. The ABI's
alignment rules exist so the compiler can emit the fast instructions without checks.

**The cache hierarchy (typical x86-64 server, 2020s).** L1d 32 to 48 KB per core, around 4 cycles;
L2 1 to 2 MB per core, around 14 cycles; L3 tens of MB shared, around 40 to 70 cycles; DRAM 70 to
100+ ns. A pointer chase that misses to DRAM costs as much as roughly 300 simple instructions. The
hardware prefetcher detects sequential and strided access and hides latency; it cannot follow a
linked list. This is the whole argument for `vector` over `list` and for structure-of-arrays layouts
in hot loops.

**False sharing in the coherence protocol.** Caches keep lines in states (MESI: Modified, Exclusive,
Shared, Invalid). A write requires the line in Modified state on that core; any other core's copy is
invalidated. Two cores writing different bytes of one line ping-pong the line through the interconnect
on every write (tens of ns each). Padding to 64 bytes makes them independent. Reads alone do not
cause it (Shared state is fine); one writer plus many readers of the same line does.

**What `malloc` does (glibc ptmalloc).** Per-thread cache (`tcache`) of recently freed small blocks
(fast path: pop from a singly linked list, no lock); then per-arena bins with a mutex; then
`brk`/`mmap` from the kernel for new memory (page faults on first touch, zeroing by the kernel). Each
block has a 16-byte header and is rounded up to 16 bytes; a `new int` costs 32 bytes. Freed memory is
coalesced but rarely returned to the OS (fragmentation keeps RSS high). Many small allocations from
many threads contend on arenas; that is where arenas, pools and `jemalloc`/`tcmalloc` (drop-in via
`LD_PRELOAD`) win.

**`std::pmr` design.** `std::pmr::polymorphic_allocator<T>` holds a `memory_resource*` and forwards
`allocate`/`deallocate` through two virtual calls. The container type does not change with the
resource (unlike classic allocators, where `std::vector<T, MyAlloc>` is a different type), so a
function can accept `std::pmr::vector<int>&` regardless of where its memory comes from.
`monotonic_buffer_resource` never frees individually (`deallocate` is a no-op), grows by asking its
upstream for geometrically larger chunks, and releases everything in `release()`/destructor.
The pool resources keep free lists per size class. All of them chain to an upstream (default
`new_delete_resource()`), so a pool can sit on top of an arena on top of `malloc`.

**Small buffer optimisation mechanics.** A union of an inline buffer and a `(pointer, capacity)`
pair, plus a discriminator (often folded into the capacity or size field). Access checks the
discriminator; moves of the inline case copy bytes; moves of the heap case steal the pointer.
`std::string`'s 15-byte buffer and `std::function`'s 16-byte buffer are both "the object's own
storage reinterpreted". `sizeof` goes up, allocations go down. Right for types that are usually small.

**How UB enables optimisation.** `for (int i = 0; i < n; ++i) a[i] = b[i] + 1;` is vectorised only
because the compiler may assume `a` and `b` do not alias when their types differ (strict aliasing), the
loop counter does not overflow (signed overflow UB lets it use 64-bit induction variables), and the
indices are in bounds. Replace `int i` with `unsigned i` and some loops get slower because wrap-around
is defined and must be honoured. UB is the contract that lets the compiler stop proving things.

**The two aliasing escape hatches.** `char`, `unsigned char` and `std::byte` may alias anything, so
byte-wise access is always legal. `std::memcpy` between objects of different types is the sanctioned
type pun; GCC compiles an 8-byte memcpy to a single `mov`. The `union` trick is legal in C, UB in C++
(though GCC documents it as supported).

**Stack size.** The main thread's stack is typically 8 MB (`ulimit -s`); other threads default to
8 MB on glibc but are configurable. A 1 MB `std::array` on the stack in a deep recursion is a segfault
with no diagnostic other than `SIGSEGV` at a `sub rsp` or a `push`. Large buffers belong in a `vector`
(heap) or a static.

## 5. In production

- **Measure RSS, not just speed.** A `std::map<int, int>` with 10M entries is about 640 MB (64-byte
  nodes); a sorted `std::vector<std::pair<int,int>>` is 80 MB. In a banking server where the cache of
  account summaries dominates memory, layout *is* capacity planning.
- **Per-request arenas.** Allocate everything for one RPC from a `monotonic_buffer_resource` created
  at request start and released at the end: one allocation per request instead of hundreds, no
  fragmentation, no per-object frees, and a natural leak guard. This is the design you already use in C.
- **`-O2 -g` always.** Debug builds (`-O0`) are 5 to 20x slower and behave differently (no UB
  "surprises"). Ship `-O2` with debug symbols (`-g`), strip them into a separate file
  (`objcopy --only-keep-debug`), and symbolise core dumps with it.
- **Sanitiser builds in CI.** ASan + UBSan on the test suite catches the bugs that only show in
  production under `-O2`. TSan on the concurrency tests. The cost (2 to 3x slower tests) is nothing
  next to a corrupted ledger.
- **`-Wall -Wextra -Werror` plus `-Wconversion -Wshadow`** at the start of a project, not later. Adding
  them to a 200k-line codebase afterwards produces thousands of warnings nobody fixes.
- **Beware "optimisations" without a profile.** `perf record -g ./server` then `perf report` shows
  where time goes. The usual winners: an unexpected allocation in a hot loop (`std::string` temporaries
  from `+`), a `std::map` where a `vector` would do, a `shared_ptr` copy per call, false sharing on a
  counter, and `std::endl` flushing a log on every line.
- **3 AM debugging.** A function "returns garbage only in release builds": UB. Classic causes: reading
  an uninitialised member (constructor forgot it), signed overflow in a hash or a timer calculation,
  returning a reference to a local, strict-aliasing punning of a network buffer. Rebuild with
  `-fsanitize=undefined,address -O1 -g`, re-run the failing input, read the first report.

## 6. Try it

1. **(easy)** Build `m12_padding_alignment.cpp`. Reorder `Bad` to minimise size by hand before running,
   predict the offsets, run. Done when predictions match and you have added a `static_assert` on `sizeof(Good)`.
2. **(easy)** Build and run `m12_false_sharing_bench.cpp` with `-O2`. Change the padding to 32 bytes and
   observe. Done when you can state your CPU's line size from the experiment and from `getconf LEVEL1_DCACHE_LINESIZE`.
3. **(medium)** Extend `Arena` with a `destroy_all()` that runs destructors for non-trivial objects
   (keep a small vector of `(void*, void(*)(void*))` finalisers). Done when a `std::string` allocated in
   the arena is destroyed correctly under Valgrind.
4. **(medium)** Run `m12_bench_basics.cpp` at `-O0`, `-O2`, `-O3 -march=native`, and with `perf stat -e cache-misses`.
   Done when you have a table of times and cache misses and can explain the vector/list ratio with them.
5. **(medium)** Write a `SmallVector<T, N>` with inline storage for `N` elements and heap beyond it
   (SBO). Done when `sizeof(SmallVector<int, 8>)` is as expected, moves of the inline case copy and of
   the heap case steal, and a `Loud`-style element type shows the right number of moves.
6. **(hard)** Write three functions containing (a) a signed overflow check `if (x + 1 < x)`,
   (b) a null check after a dereference, (c) a `float` read through an `int*`. Compile at `-O2` with
   `-S` and show how the optimiser removed or changed each. Then run under UBSan. Done when you can
   explain, for each, which assumption the compiler made.
7. **(hard)** Replace the allocator of a `std::unordered_map<int, std::string>` with
   `std::pmr::unsynchronized_pool_resource` and benchmark 1M inserts and erases against the default.
   Done when you have numbers for time and peak RSS (`/usr/bin/time -v`) and a recommendation.

## 7. Interview questions

**Q: Why does `struct { char; double; int; char; }` occupy 24 bytes?**
Each member must be aligned to its own alignment (8 for `double`, 4 for `int`), so the compiler
inserts 7 bytes after the first `char` and 3 at the end so that arrays of the struct keep every
`double` 8-byte aligned. Reordering members by descending alignment gives 16 bytes.

**Q: What is false sharing and how do you fix it?**
Two cores writing different variables that happen to share a 64-byte cache line force the line to
bounce between their caches on every write, serialising them as if they held a lock. Fix by placing
the variables in different lines with `alignas(64)` or padding, or by giving each thread its own
counter and summing at the end.

**Q: Which standard library operations allocate?**
`vector`/`string` growth beyond capacity (strings over 15 chars), every node of `map`/`set`/
`unordered_map`/`list`, `make_unique`/`make_shared`, `std::function` holding a callable larger than
16 bytes, `std::any`, `stringstream`. `array`, `optional`, `variant`, `string_view`, `pair`, `tuple`
and lambdas do not.

**Q: What does an arena allocator buy you and what does it cost?**
Allocation becomes a pointer bump (no lock, no header, no free list), objects are contiguous and
cache-friendly, and freeing is O(1) for the whole arena. The cost: no individual frees (memory is held
until reset), destructors must be run manually for non-trivial types, and sizing must be chosen. The
C++17 standard version is `std::pmr::monotonic_buffer_resource`.

**Q: Why is undefined behaviour a performance feature and a danger at once?**
The optimiser assumes UB never happens, which lets it drop bounds and overflow checks, keep values in
registers, reorder memory accesses, and vectorise loops. The same assumption means that if UB does
happen the program's behaviour has no relation to the source: checks vanish, branches invert, and the
bug appears only at `-O2`. Sanitisers exist to find UB before the optimiser exploits it.

**Q: How do you legally reinterpret the bytes of a `float` as an `int`?**
`std::memcpy(&i, &f, sizeof i)`, which GCC compiles to a single register move. Casting the pointer
(`*(int*)&f`) violates strict aliasing and is UB. C++20 adds `std::bit_cast`.

**Q: What is the small buffer optimisation?**
Storing small payloads inside the object itself and using the heap only when the payload exceeds a
threshold: `std::string` keeps up to 15 characters inline, `std::function` keeps callables up to 16
bytes. It trades a larger `sizeof` and a branch per access for zero allocations in the common case.

**Q: How do you benchmark a function honestly?**
Build with production flags (`-O2`), use `steady_clock`, warm up, repeat and take the minimum or median,
make the result observable so the optimiser cannot remove the work, use realistic data sizes, and
check `perf stat` counters (cache misses, instructions) to understand the result rather than just
report it.

## 8. Recap

- Members are padded to their alignment; order by descending alignment; `alignas` and `#pragma pack`
  for lines and wire formats; `static_assert` the layout.
- 64-byte cache lines; sequential access is prefetched, pointer chasing is not; false sharing is
  two writers in one line.
- Stack is free; heap costs tens of ns plus scattering; know which standard types allocate.
- Reserve, reuse, arena (`std::pmr::monotonic_buffer_resource`), pool, SBO.
- `-O2` assumes no UB; UB categories: signed overflow, out of bounds, dangling/null, strict
  aliasing, data races, uninitialised reads.
- `memcpy` for type punning; `char`/`std::byte` alias anything.
- ASan, UBSan, TSan in CI; `-O2 -g` in production with separate debug symbols.
- Benchmark: `-O2`, `steady_clock`, best-of-N, observable results, realistic sizes, `perf stat`.
- Layout decides memory footprint as much as speed: node containers cost 32 to 64 bytes per element.
