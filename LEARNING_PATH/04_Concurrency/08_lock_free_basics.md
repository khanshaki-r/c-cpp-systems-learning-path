# 08. Lock-free basics

**Time:** ~3 hours   **Prerequisites:** 07

## 1. In plain words

A lock-free data structure is one where threads coordinate using atomic instructions instead of
mutexes, and where at least one thread is always guaranteed to make progress no matter what the
others are doing. If a thread holding a mutex gets descheduled, everyone waiting on that mutex
stalls. If a thread in the middle of a lock-free operation gets descheduled, the others simply
retry and succeed. That is the appeal.

Analogy: a whiteboard where anyone may write, but the rule is "copy the current number, compute the
new number, and write it back only if the board still shows the number you copied; otherwise start
over". Nobody holds a pen exclusively, so nobody can walk away with it. But if two people keep
colliding, both keep restarting, and if someone erases a number and writes the *same* number back
while you were computing, your check passes even though the world changed under you. That last
problem is called ABA and it is why lock-free code is hard.

The honest senior position: lock-free structures are rarely needed in a request server, they are
easy to get subtly wrong, and a well-designed mutex-based structure with short critical sections
is usually as fast. Learn the three classics (spinlock, SPSC ring, Treiber stack) so you can
recognise them, reason about them, and explain why you chose *not* to use them.

## 2. The real thing

### 2.1 Progress guarantees

- **Blocking**: a thread may wait indefinitely for another (mutexes).
- **Obstruction-free**: a thread completes if it runs alone long enough.
- **Lock-free**: at least one thread completes in a bounded number of steps system-wide. Some
  thread always makes progress; an individual thread may starve.
- **Wait-free**: every thread completes in a bounded number of steps. The strongest; rare and
  usually expensive.

A CAS loop is lock-free: if your CAS fails, it is because someone else's succeeded. A spinlock is
*not* lock-free (the holder can be descheduled and everyone spins); it is a blocking lock that
happens to wait by spinning.

### 2.2 Spinlock

```cpp
class Spinlock {
    std::atomic<bool> locked_{false};
public:
    void lock() {
        for (;;) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;   // got it
            while (locked_.load(std::memory_order_relaxed)) { /* pause */ }     // spin on a load
        }
    }
    void unlock() { locked_.store(false, std::memory_order_release); }
};
```

Memory orders: `acquire` on the successful exchange (so the critical section cannot start before
we own the lock, and sees the previous owner's writes), `release` on unlock (so our writes are
visible to the next acquirer). The inner loop spins on a *load*, not on `exchange`: an exchange
is a write that steals the cache line from the holder every iteration and slows the unlock;
a load lets the line sit in Shared state on every spinner until the unlock invalidates it.
That is the test-and-test-and-set pattern.

The pause: `_mm_pause()` on x86 (`#include <immintrin.h>`) or `__builtin_ia32_pause()`, which
tells the CPU to back off the spin loop for a few tens of cycles and avoids a memory-order
violation pipeline flush on exit. On ARM use `__asm__ volatile("yield")`. After some number of
spins (hundreds), fall back to `std::this_thread::yield()` or a real futex wait: a spinner on a
single-core VM holding the CPU while the lock owner waits for the CPU is a livelock.

When a spinlock is right: critical sections of a few dozen instructions, known-low contention,
and more cores than threads (so the holder is never descheduled mid-section). Kernel code, and
very little else. The glibc adaptive mutex spins briefly then sleeps and is almost always the
better choice in user space.

### 2.3 SPSC ring buffer

Single producer, single consumer. Because each index is written by exactly one thread, no CAS is
needed; two atomic indices and release/acquire pairs are enough. This is the one lock-free
structure worth memorising because it is genuinely simple and genuinely useful (log pipelines,
audio, network packet hand-off between an I/O thread and a worker).

```cpp
template <class T, std::size_t N>   // N power of two
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    alignas(64) std::atomic<std::size_t> head_{0};   // written by consumer, read by producer
    alignas(64) std::atomic<std::size_t> tail_{0};   // written by producer, read by consumer
    alignas(64) T buf_[N];
public:
    bool push(const T& v) {
        std::size_t t = tail_.load(std::memory_order_relaxed);        // own index: relaxed
        std::size_t h = head_.load(std::memory_order_acquire);        // see consumer's pops
        if (t - h == N) return false;                                 // full
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);                // publish the slot
        return true;
    }
    bool pop(T& out) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        std::size_t t = tail_.load(std::memory_order_acquire);        // see producer's pushes
        if (h == t) return false;                                     // empty
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);                // free the slot
        return true;
    }
};
```

Why it is correct: the producer writes `buf_[t]` *before* the release store of `tail_ = t+1`. The
consumer's acquire load of `tail_` that sees `t+1` therefore sees `buf_[t]`. Symmetrically the
consumer's release of `head_` after reading the slot tells the producer the slot is free to
overwrite. Indices are monotonic `size_t` that wrap naturally; `t - h` is the occupancy even
across wraparound because unsigned subtraction is modular.

The `alignas(64)` puts each index on its own cache line so the producer's writes to `tail_` do not
invalidate the consumer's `head_` line (false sharing, module 10). A common refinement is to cache
the other side's index locally and only re-load it when the cached value says full or empty,
cutting the cross-core traffic further.

### 2.4 Treiber stack and the ABA problem

A lock-free stack: push and pop by CAS on the head pointer.

```cpp
template <class T>
class TreiberStack {
    struct Node { T value; Node* next; };
    std::atomic<Node*> head_{nullptr};
public:
    void push(T v) {
        Node* n = new Node{std::move(v), nullptr};
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(n->next, n,
                   std::memory_order_release, std::memory_order_relaxed)) {}
    }
    bool pop(T& out) {
        Node* old = head_.load(std::memory_order_acquire);
        while (old && !head_.compare_exchange_weak(old, old->next,          // <-- reads old->next
                   std::memory_order_acquire, std::memory_order_acquire)) {}
        if (!old) return false;
        out = std::move(old->value);
        delete old;                                                          // <-- the problem
        return true;
    }
};
```

Two bugs hide in `pop`, and they are the whole lesson.

**Bug 1: use-after-free.** Thread A loads `old = head` (node X) and is about to read
`old->next`. Thread B pops X and `delete`s it. A now reads freed memory. A mutex-based stack cannot
have this problem because B could not pop while A held the lock. Lock-free structures have no
critical section, so a node may be freed while another thread still holds a pointer to it. This is
the **memory reclamation problem** and it is the real difficulty of lock-free programming.

**Bug 2: ABA.** Thread A loads `old = X`, reads `old->next = Y`. Thread B pops X, pops Y, pushes a
new node that happens to be allocated at X's old address (allocators reuse hot addresses eagerly).
Head is X again (same address, different node) and its next is now Z, not Y. A's CAS compares head
to X, succeeds, and sets head to Y, which is freed. The stack is corrupt. The CAS checked the
*value* of the pointer; it could not know the *identity* behind it changed and came back.

Fixes:

- **Tagged pointers / version counters.** Pack a 16-bit counter into the unused high bits of the
  pointer (x86-64 user-space addresses use 47 bits) or use a 128-bit CAS (`cmpxchg16b`) on a
  `{pointer, counter}` pair. Every successful CAS increments the counter, so X-then-X-again has
  different tags. Fixes ABA; does not fix use-after-free.
- **Never free.** Keep nodes in a free list owned by the stack; recycled, never returned to the
  allocator. Memory never becomes invalid, so the dereference in bug 1 reads stale-but-valid
  memory, and the tag fixes ABA. Common in fixed-size pools.
- **Hazard pointers.** Each thread publishes "I am currently looking at node P" in a per-thread
  slot before dereferencing it, and re-checks that head is still P after publishing. A thread
  that wants to free a node first scans all hazard slots; if any thread has it marked, the free is
  deferred to a retire list and retried later. Bounded memory, lock-free, and the standard answer
  in interviews. C++26 is adding `std::hazard_pointer`.
- **Epoch-based reclamation / RCU.** Threads announce when they enter and leave a "read epoch";
  freed nodes are queued and only released when every thread has passed through a quiescent
  state. Cheaper per access than hazard pointers; higher memory latency. Module 11 uses the idea
  with `shared_ptr`.
- **`shared_ptr` with atomic operations.** `std::atomic<std::shared_ptr<T>>` (C++20) or the
  C++17 free functions `std::atomic_load(&sp)` give you reference counting instead of manual
  reclamation. Not lock-free in most implementations (a hidden spinlock) but correct and simple.

The code file includes the broken version (for TSan/ASan to flag) and a tagged, never-free version
that is correct for a fixed pool.

### 2.5 MPMC queues: a warning

A multi-producer, multi-consumer lock-free queue is where most engineers should stop. The
well-known designs (Michael-Scott queue, Vyukov's bounded MPMC, LMAX Disruptor) each run to
hundreds of lines with subtle invariants and all need a reclamation scheme or a fixed ring. In a
request server, a `std::mutex` plus `std::deque` plus `std::condition_variable` queue handles
hundreds of thousands of items per second and blocks consumers properly when idle. Lock-free
queues spin or sleep-poll when empty, which is wrong for a thread pool. Use a library
(`moodycamel::ConcurrentQueue`, `boost::lockfree`, folly) if you truly need one; do not write it.

### 2.6 When lock-free is not worth it

- Your critical section is more than a few dozen instructions. A CAS loop over a large update
  re-does the work on every collision.
- You need to block when empty (every work queue). Lock-free gives you "return false"; you still
  need a condition variable or a futex to sleep, at which point the lock-free part bought little.
- You cannot afford to get it wrong, and you cannot test on ARM. Lock-free bugs are the hardest
  class to reproduce.
- Contention is low. An uncontended mutex is two atomics; a CAS loop is at least one. The gain
  is nil.
- The structure needs `delete`. See bug 1.

When it is worth it: a single hot counter or flag (that is just `std::atomic`), SPSC hand-off
between two specific threads, a spinlock in a tight kernel-style section, and a published
immutable snapshot (module 11). Those four cover almost every legitimate use in a server.

## 3. Code

File: `code/m08_spinlock.cpp`

```cpp
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <mutex>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_relax() { _mm_pause(); }
#else
static inline void cpu_relax() { std::this_thread::yield(); }
#endif

class Spinlock {
    std::atomic<bool> locked_{false};
public:
    void lock() {
        for (int spins = 0;; ++spins) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;
            // Test-and-test-and-set: spin on a read so the line stays Shared across spinners.
            while (locked_.load(std::memory_order_relaxed)) {
                if (spins < 100) cpu_relax();                 // WHY: short back-off, holder is likely active
                else std::this_thread::yield();               // WHY: let a descheduled holder run
            }
        }
    }
    void unlock() { locked_.store(false, std::memory_order_release); }
    bool try_lock() { return !locked_.exchange(true, std::memory_order_acquire); }
};

template <class Lock>
double bench(const char* name, int threads, int iters) {
    Lock lk;
    long shared = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < iters; ++i) { std::lock_guard<Lock> g(lk); ++shared; }
        });
    for (auto& t : ts) t.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << name << " threads=" << threads << " total=" << shared
              << " (expected " << static_cast<long>(threads) * iters << ") " << ms << " ms\n";
    return ms;
}

int main() {
    for (int t : {1, 2, 4, 8}) {
        bench<Spinlock>("spinlock  ", t, 1'000'000);
        bench<std::mutex>("std::mutex", t, 1'000'000);
    }
    return 0;
}
```

File: `code/m08_spsc_ring.cpp` — the ring buffer above with a throughput test and a checksum.

File: `code/m08_treiber_stack.cpp` — the naive stack (compile with `-DBROKEN` to see ASan/TSan
catch it) and a tagged-pointer, pooled-node version that is correct.

Key part of the tagged version:

```cpp
struct TaggedPtr { std::uint32_t index; std::uint32_t tag; };   // index into a fixed node pool
std::atomic<std::uint64_t> head_;   // packed TaggedPtr; 8 bytes, always lock-free

bool pop(T& out) {
    std::uint64_t old = head_.load(std::memory_order_acquire);
    for (;;) {
        TaggedPtr h = unpack(old);
        if (h.index == NIL) return false;
        std::uint32_t next = pool_[h.index].next;            // WHY safe: pool memory is never freed
        TaggedPtr nh{next, h.tag + 1};                       // WHY tag+1: defeats ABA
        if (head_.compare_exchange_weak(old, pack(nh), std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            out = std::move(pool_[h.index].value);
            free_node(h.index);                              // onto a second Treiber free-list
            return true;
        }
    }
}
```

## 4. Under the hood (senior-level)

### Why test-and-test-and-set matters

With N spinners doing `exchange` in a loop, every iteration on every core requests the line in
Exclusive state. The line ping-pongs N ways; the holder's `unlock` store has to fight for it. With
spin-on-load, after the first miss every spinner has the line in Shared state and spins entirely
in its own L1. The holder's unlock invalidates all copies once; spinners miss once, reload, see
`false`, and one wins the next `exchange`. Coherence traffic drops from O(N) per iteration to O(N)
per unlock.

### Why `_mm_pause`

Tight spin loops on Intel trigger a pipeline flush when the loop exits because of speculative
loads past the branch; `pause` hints the CPU to delay (tens to ~140 cycles depending on
microarchitecture, per Intel's optimisation manual) and reduces power and the flush penalty. It
also gives the sibling hyperthread the core. It is a hint; without it the spinlock is still
correct.

### CAS loop fairness

`compare_exchange` has no queue. Under contention the core that most recently touched the line
tends to win the next CAS (the line is in its cache), so one thread can starve while others
proceed. Lock-free is not fair. Ticket locks and MCS/CLH queue locks fix fairness for spinlocks;
they are the standard answer to "how would you make a fair spinlock" and are worth reading about
after this module.

### 128-bit CAS and `is_lock_free`

`std::atomic<TaggedPtr>` where `TaggedPtr` is 16 bytes is lock-free on x86-64 only if compiled
with `-mcx16` (and GCC still may route it through libatomic for ABI reasons). That is why the code
packs index and tag into 64 bits: a 32-bit index into a pool plus a 32-bit tag fits in one
always-lock-free word. On ARMv8.1 there is `casp` for 128 bits; on older ARM you use `ldxp/stxp`.

### Hazard pointers, concretely

Each thread owns K slots (usually 1 or 2) in a global array of `std::atomic<void*>`. To
dereference node P: `slot.store(P, seq_cst); if (head.load() != P) retry;`. The re-check closes
the window where P was freed between the load and the publish. To free node Q: push it onto the
thread's private retire list; when the list exceeds a threshold R, scan all slots (O(threads * K))
and free the retired nodes not present in any slot. Amortised cost per retire is O(threads*K / R).
Memory overhead is bounded by threads * K * R unreclaimed nodes. The seq_cst store on every read is
the hot-path cost that RCU avoids.

### Why a mutex often wins

An uncontended `std::mutex` lock+unlock is two atomic RMWs on one line, roughly 40 ns total
(typical). A CAS loop for a stack push is one CAS when uncontended, roughly 20 ns. Under
contention the mutex serialises (threads sleep, no wasted work) while the CAS loop wastes the
losers' work and burns cache bandwidth. For critical sections longer than roughly 100 ns the mutex
pulls ahead under contention. Lock-free wins only when the operation is tiny *and* you cannot
tolerate a descheduled holder blocking others (real-time, signal handlers, kernel paths).

## 5. In production

- **Spinlocks in user space are a bug until proven otherwise.** The classic failure: a container
  limited to 1 CPU, a spinlock holder descheduled, 7 spinners burning the CPU the holder needs.
  Throughput near zero, CPU at 100%. If you inherit one, add a spin limit and yield, or replace
  with `std::mutex`.
- **SPSC for logging.** A worker pushes log records into a per-worker SPSC ring; a single logger
  thread drains all rings and writes. Workers never block on disk, the logger never takes a lock.
  This is the right use of lock-free in a server, and `moodycamel::ReaderWriterQueue` is a
  battle-tested implementation if you do not want to own the code.
- **The "lock-free for performance" rewrite that got slower.** Someone replaces a `std::mutex`
  protected `std::vector<Order>` with a hand-rolled lock-free list. It is faster in the
  single-thread microbenchmark and slower under 16-thread load because the CAS loop collides and
  the structure's traversal is cache-hostile. Always benchmark under realistic contention before
  and after.
- **ABA at 3 AM.** A corrupted free list in a custom allocator after days of uptime. The signature
  is a crash in the allocator with a node pointing to itself or to a freed block, with no
  reproducer. If the allocator uses a CAS'd head pointer without a tag, you have found it.
- **Memory reclamation is the question.** When an interviewer asks about a lock-free stack, the
  senior answer leads with "the push is easy; the pop is the problem because of reclamation and
  ABA, and here is how hazard pointers solve it". The junior answer writes the CAS loop and stops.

## 6. Try it

1. **(easy)** Build and run `m08_spinlock.cpp`. Compare spinlock vs mutex at 1, 2, 4, 8 threads.
   Then run with `taskset -c 0` (one core). Done when you have seen the spinlock's single-core
   collapse and can explain it.
2. **(easy)** Build `m08_spsc_ring.cpp`, run, confirm the checksum. Change `tail_.store(...,
   release)` to `relaxed` and run under TSan. Done when TSan reports the race on `buf_`.
3. **(medium)** Build `m08_treiber_stack.cpp` with `-DBROKEN -fsanitize=address` and run with 4
   threads pushing and popping. Done when ASan reports heap-use-after-free in `pop`, and you have
   written the interleaving (which thread deleted what while which thread held it).
4. **(medium)** Add local caching of the other side's index to `SpscRing` (producer caches `head_`
   and only reloads when it believes the ring is full; consumer symmetric). Measure throughput
   before and after with `perf stat -e cache-misses` if available. Done when the number of
   acquire loads per operation drops and you can explain why it is still correct.
5. **(hard)** Implement hazard pointers for the Treiber stack with real `new`/`delete`: one slot
   per thread, a per-thread retire list, scan at 2x thread count retired. Verify with ASan and TSan
   under 8 threads for 10 million operations. Done when both sanitizers are silent and you can
   state the memory bound.

## 7. Interview questions

**Q: What does lock-free mean, precisely?**
At least one thread completes its operation in a bounded number of steps regardless of what other
threads do, including being descheduled. Wait-free is stronger: every thread completes in bounded
steps. A spinlock is not lock-free; a descheduled holder blocks everyone.

**Q: Explain the ABA problem.**
A CAS checks that a location still holds the value you read, but a value can be changed and
changed back. In a lock-free stack, a thread reads head X and X's next Y; another thread pops X
and Y and pushes a new node at X's recycled address; the first thread's CAS sees X, succeeds, and
installs Y, which is freed. Fixes: a version tag alongside the pointer so X-again has a new tag,
or reclamation schemes that prevent the address from being reused while anyone holds it.

**Q: Why is memory reclamation the hard part of lock-free structures?**
There is no critical section, so a thread may hold a pointer to a node while another thread frees
it. The freeing thread must know nobody is looking. Hazard pointers have readers publish what they
hold and freers defer nodes that are published; epoch/RCU schemes defer until every thread passes
a quiescent point; or you never free and recycle from a pool.

**Q: How does an SPSC ring buffer work without a CAS?**
Each index has exactly one writer. The producer writes the slot then release-stores the tail; the
consumer acquire-loads the tail, so it sees the slot. The consumer reads the slot then
release-stores the head; the producer acquire-loads the head, so it knows the slot is free.
Monotonic unsigned indices with power-of-two masking handle wraparound, and each index lives on
its own cache line.

**Q: Why spin on a load rather than on an exchange?**
An exchange is a write and pulls the cache line into Exclusive state on the spinning core every
iteration, stealing it from other spinners and from the holder trying to unlock. Spinning on a load
leaves the line Shared in every spinner's cache until the unlock invalidates it once. This is
test-and-test-and-set.

**Q: When would you choose a mutex over a lock-free structure?**
Almost always in a server: when the critical section is more than a few dozen instructions, when
consumers need to block on empty, when the structure needs to free memory, when contention is low
so the mutex is uncontended anyway, or when you cannot test on weakly ordered hardware. Lock-free
is for tiny operations where a descheduled holder is unacceptable.

**Q: What is a hazard pointer?**
A per-thread published pointer saying "I am reading this node". Before dereferencing, a reader
stores the pointer in its slot and re-validates that the node is still reachable. A thread
retiring a node checks all slots and defers freeing any node that is published. It bounds
unreclaimed memory to threads times slots times the scan threshold and is lock-free.

## 8. Recap

- Lock-free: some thread always progresses; wait-free: every thread does. Spinlocks are neither.
- Spinlock: exchange to acquire, spin on a load, pause, yield after a limit, release to unlock.
  Rarely right in user space.
- SPSC ring: two single-writer indices, release on publish, acquire on observe, indices on
  separate cache lines. The one lock-free structure worth owning.
- Treiber stack: CAS on head. Pop has use-after-free and ABA. Both come from the absence of a
  critical section.
- ABA fixes: tags or never-free pools. Reclamation fixes: hazard pointers, epochs/RCU,
  reference counting.
- 64-bit packed index+tag is always lock-free; 128-bit CAS needs `-mcx16` and may not be.
- MPMC lock-free queues: use a library or a mutex, do not write one.
- Under contention a CAS loop wastes losers' work; a mutex serialises cleanly. Benchmark under
  real contention.
- In interviews, lead with reclamation and ABA, not with the CAS loop.
