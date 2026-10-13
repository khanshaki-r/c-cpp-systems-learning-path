# 06. Stacks, queues and deques

**Time:** ~3 hours   **Prerequisites:** 01, 02, 05

## 1. In plain words

A stack is a pile of plates: you add to the top and take from the top. Last in, first out
(LIFO). A queue is a line at a bank counter: join at the back, served from the front. First
in, first out (FIFO). A deque ("deck", double-ended queue) lets you add and remove at both
ends. All three are **restricted** containers: they deliberately offer fewer operations than an
array so that every operation is O(1) and the usage pattern is obvious.

You already use a stack without naming it: every function call pushes a frame, every return
pops one. Your RPC server uses a queue: requests arrive at the back, worker threads take from
the front. The receive buffer on a socket is a ring buffer — a queue of bytes in a fixed array
that wraps around.

The interview value is in two patterns built on top: the **monotonic stack** (a stack kept
sorted so that "next greater element" questions become O(n)) and the **monotonic deque**
(sliding window maximum in O(n)). Both look like magic the first time and are mechanical once
you see the invariant.

## 2. The real thing

### 2.1 Stack

Operations: `push`, `pop`, `top`, `empty`, `size`. All O(1). Implement on a `std::vector`:
`push_back`, `pop_back`, `back()`. `std::stack<T>` is an adapter over `std::deque<T>` by
default; `std::stack<T, std::vector<T>>` uses a vector. The adapter exists to *hide* random
access so the code states its intent.

Where stacks appear: function calls, expression evaluation (operators and operands), undo
history, bracket matching, DFS (iterative), backtracking, parsing (shift-reduce), the
"recently opened" set in a monotonic-stack problem.

### 2.2 Queue

Operations: `push` (back), `pop` (front), `front`, `back`, `empty`, `size`. On a vector,
`pop_front` is O(n) — wrong container. Options:

- `std::deque<T>` (what `std::queue<T>` uses): O(1) both ends.
- **Two stacks**: push onto `in`; to pop, if `out` is empty move everything from `in` to `out`
  (reversing order), then pop `out`. Each element is moved at most once, so amortised O(1).
  This is the classic "implement a queue with stacks" question, and it is also how
  functional languages implement persistent queues.
- **Ring buffer** (circular buffer): a fixed array with `head` and `tail` indices that wrap
  modulo capacity. O(1) both ends, zero allocation after construction, cache-friendly. The
  structure behind socket buffers, audio buffers, log rings, lock-free SPSC queues.

### 2.3 Deque

`std::deque<T>`: O(1) push/pop at both ends, O(1) random access, iterators invalidated by
insertion at either end (but references are **not** invalidated by push at the ends — a quirk
worth knowing). Implemented as a vector of fixed-size blocks (512 bytes per block on libstdc++)
plus a map array of block pointers. Random access is two indirections. Growing at either end
allocates a new block; no element is copied. Use it when you need a queue with indexing, or a
stack that must not reallocate (block addresses are stable).

### 2.4 Ring buffer in detail

```
capacity 8, head=2 (read), tail=6 (write), size=4
index:  0 1 2 3 4 5 6 7
        . . A B C D . .
            ^head   ^tail
push(E): buf[6]=E; tail=(6+1)%8=7
push(F): buf[7]=F; tail=0          <- wrapped
pop(): return buf[2]=A; head=3
```

Design decisions:
1. **Full vs empty**: with only `head` and `tail`, `head == tail` is ambiguous. Either keep a
   `count`, or sacrifice one slot (full when `(tail+1)%cap == head`), or use monotonically
   increasing 64-bit `head`/`tail` counters and index with `% cap` (then `size = tail - head`,
   never ambiguous, and the counters serve as sequence numbers — this is what high-performance
   queues do).
2. **Power-of-two capacity** turns `% cap` into `& (cap-1)`: a division becomes an AND. Worth
   it in a hot path; irrelevant otherwise.
3. **Overwrite or reject when full?** A log ring overwrites the oldest (you want recent
   history). A network buffer rejects (backpressure). Make it an explicit policy.
4. **Contiguous reads**: `readv`/`writev` or two `memcpy` calls when the data wraps. Some
   implementations mirror the buffer with `mmap` so the wrap is invisible ("magic ring buffer").

### 2.5 Monotonic stack

A stack whose elements are kept in sorted order (increasing or decreasing from bottom to top)
by popping anything that would violate the order before pushing. Each element is pushed once
and popped at most once: O(n) total.

Use it for: next greater/smaller element to the left or right, daily temperatures ("days until
a warmer day"), largest rectangle in a histogram, trapping rain water, stock span, removing k
digits to make the smallest number, and "previous smaller" boundaries generally.

Template (next greater element to the right):

```
for i in 0..n-1:
    while stack not empty and v[stack.top] < v[i]:
        answer[stack.top] = i        # v[i] is the first greater element for stack.top
        pop
    push i
remaining indices on the stack have no greater element to the right
```

The stack holds indices of elements still waiting for their answer, in decreasing value order
from bottom to top. When a larger value arrives it resolves everyone smaller.

### 2.6 Monotonic deque: sliding window maximum

Maintain a deque of indices with decreasing values. For each new `i`: pop from the back while
`v[back] <= v[i]` (those can never be the max while `i` is in the window); push `i`; pop from
the front if it has left the window; `v[front]` is the current maximum. O(n). The brute force
is O(nk); a heap gives O(n log k). This is the structure for "max over the last k samples" in
monitoring.

## 3. Code

Files: `code/m06_stack_queue.cpp` (vector-backed stack, two-stack queue, min-stack, valid
parentheses, evaluate RPN), `code/m06_ring_buffer.cpp` (templated ring buffer with
counters, power-of-two mask, overwrite/reject policies, test), `code/m06_monotonic_stack.cpp`
(next greater element, daily temperatures, largest rectangle, sliding window maximum).

### Problem 1 — Valid parentheses

Push openers; on a closer, the top must be the matching opener. Empty at the end.

```cpp
bool valid_parens(std::string_view s) {
    std::vector<char> st;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') { st.push_back(c); continue; }
        if (st.empty()) return false;
        char open = st.back(); st.pop_back();
        if ((c == ')' && open != '(') || (c == ']' && open != '[') || (c == '}' && open != '{')) return false;
    }
    return st.empty();
}
```
O(n), O(n).

### Problem 2 — Min stack: `push`, `pop`, `top`, `get_min` all O(1)

Keep a second stack of "minimum so far". Push `min(x, mins.back())` with every push; pop both.

```cpp
void push(int x) { data.push_back(x); mins.push_back(mins.empty() ? x : std::min(x, mins.back())); }
void pop() { data.pop_back(); mins.pop_back(); }
int get_min() const { return mins.back(); }
```
Why it works: the minimum of the stack after popping is exactly the minimum of the elements
below, which `mins` recorded at the time they were pushed.

### Problem 3 — Evaluate Reverse Polish Notation

Push numbers; on an operator pop two (right operand first), apply, push the result.

```cpp
int eval_rpn(const std::vector<std::string>& tokens) {
    std::vector<long long> st;
    for (const std::string& t : tokens) {
        if (t == "+" || t == "-" || t == "*" || t == "/") {
            long long b = st.back(); st.pop_back();
            long long a = st.back(); st.pop_back();
            st.push_back(t == "+" ? a + b : t == "-" ? a - b : t == "*" ? a * b : a / b);
        } else st.push_back(std::stoll(t));
    }
    return static_cast<int>(st.back());
}
```
This is how a stack machine (the JVM, Forth, PostScript, the bytecode of many interpreters)
executes. Infix to RPN conversion is the shunting-yard algorithm, another stack.

### Problem 4 — Daily temperatures

`answer[i]` = number of days until a warmer temperature, or 0. Monotonic decreasing stack of
indices.

```cpp
std::vector<int> daily_temperatures(const std::vector<int>& t) {
    std::vector<int> ans(t.size(), 0), st;
    for (int i = 0; i < static_cast<int>(t.size()); ++i) {
        while (!st.empty() && t[st.back()] < t[i]) { ans[st.back()] = i - st.back(); st.pop_back(); }
        st.push_back(i);
    }
    return ans;
}
```
O(n): each index is pushed once and popped at most once.

### Problem 5 — Largest rectangle in a histogram

For each bar, the widest rectangle with that bar's height extends left to the previous
shorter bar and right to the next shorter bar. A monotonic increasing stack finds both
boundaries in one pass: when a shorter bar arrives, every taller bar on the stack has found
its right boundary (the current index) and its left boundary (the new stack top).

```cpp
long long largest_rectangle(const std::vector<int>& h) {
    std::vector<int> st;        // indices with increasing heights
    long long best = 0;
    int n = static_cast<int>(h.size());
    for (int i = 0; i <= n; ++i) {
        int cur = (i == n) ? 0 : h[i];        // sentinel 0 flushes the stack at the end
        while (!st.empty() && h[st.back()] >= cur) {
            int height = h[st.back()]; st.pop_back();
            int left = st.empty() ? -1 : st.back();
            best = std::max(best, static_cast<long long>(height) * (i - left - 1));
        }
        st.push_back(i);
    }
    return best;
}
```
O(n). The sentinel at `i == n` is the trick that avoids a second loop. This is the hardest
stack problem commonly asked; "maximal rectangle in a binary matrix" reduces to it row by row.

### Problem 6 — Sliding window maximum

```cpp
std::vector<int> max_sliding_window(const std::vector<int>& v, int k) {
    std::deque<int> dq;                     // indices, values decreasing front to back
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(v.size()); ++i) {
        while (!dq.empty() && v[dq.back()] <= v[i]) dq.pop_back();   // dominated: never the max again
        dq.push_back(i);
        if (dq.front() <= i - k) dq.pop_front();                      // left the window
        if (i >= k - 1) out.push_back(v[dq.front()]);
    }
    return out;
}
```
O(n), O(k).

## 4. Under the hood (senior-level)

### 4.1 The call stack is a stack

Each call pushes a frame: return address, saved registers, locals. `rsp` is the stack pointer
and moves down on x86-64. The default thread stack is 8 MB on Linux (`ulimit -s`), 1 MB on
Windows. A recursive DFS on a 1M-node path, or the recursive list destructor from module 05,
exceeds it: SIGSEGV with a stack address, which in a core dump looks like a crash in a tiny
function with an enormous backtrace. Converting recursion to an explicit `std::vector` stack
moves the frames to the heap, where the limit is memory. Module 07 does this conversion.

### 4.2 `std::deque` layout

libstdc++: a "map" (array of pointers) to fixed-size chunks of 512 bytes (so 128 `int`s or 64
pointers per chunk), plus start/finish iterators that each hold (current, first, last, node).
`push_back` with room in the last chunk: one store and an increment. Without room: allocate a
chunk (no copying of elements), possibly grow the map (copying pointers only). Random access:
compute chunk index and offset — two loads. Iteration crosses chunk boundaries with a branch
per element, which is why `std::sort` on a deque is slower than on a vector. Rule: deque for
queues, vector for everything else.

### 4.3 Ring buffer and the hardware

A ring buffer of 4096 bytes is 64 cache lines. A producer writing at `tail` and a consumer
reading at `head` touch different lines most of the time — until they get close, when they
share a line and **false sharing** makes both slow (each write invalidates the other core's
copy). Lock-free single-producer single-consumer queues pad `head` and `tail` to separate
cache lines (`alignas(64)`) for this reason, and read the other side's index only when
necessary (cache the last-seen value). The Linux kernel's `kfifo`, DPDK's `rte_ring`, and
the LMAX Disruptor are all ring buffers with these tricks. See 04_Concurrency for the memory
ordering needed to make it correct without a mutex.

### 4.4 Why monotonic stack is O(n) — amortised, again

The inner `while` looks nested, but each index enters the stack once and leaves once. Sum of
all inner-loop iterations <= n. Same argument as module 01's two-pointer and vector growth:
count total work, not loop depth. If you are asked for the complexity and say O(n^2), you lose
the problem even with correct code.

### 4.5 `std::stack` and `std::queue` adapters

They hold a container member and forward: `push` -> `push_back`, `pop` -> `pop_front` (queue)
or `pop_back` (stack). They have no iterators, deliberately. `pop()` returns void because
returning the value by copy and then popping would not be exception-safe if the copy threw
after the pop (the "Sutter pop problem"); so you write `x = q.front(); q.pop();`. Under
optimisation the adapters cost nothing.

## 5. In production

- **Request queue between acceptor and workers.** A `std::deque<Request>` guarded by a mutex
  and a condition variable is the simplest correct design (04_Concurrency). When it is the
  bottleneck, a bounded ring buffer per worker with lock-free SPSC semantics replaces it. The
  bound is important: an unbounded queue hides overload until memory runs out.
- **Socket receive buffer.** Bytes arrive in arbitrary chunks; the parser consumes whole
  messages. A ring buffer lets `recv` write at `tail` and the parser read at `head` without
  ever shifting data. Your server's "partial message" handling is exactly this problem.
- **Undo/redo and transaction logs.** Undo is a stack; redo is a second stack; a new action
  clears redo. A database's undo log for a transaction is a stack of before-images popped on
  rollback.
- **Expression evaluation in PL/pgSQL or rules engines.** Shunting-yard to RPN once at parse
  time, then a stack machine per evaluation.
- **Flow-control debugging.** A queue whose depth grows without bound is the first signal of a
  consumer that fell behind. Export queue depth as a metric; alert on it. A ring buffer that
  overwrites silently is a data-loss bug waiting to be found in an audit.
- **3 AM story (typical).** Latency spikes every few minutes; CPU normal. Queue depth metric
  shows a sawtooth: the consumer thread blocks on a slow downstream call while holding the
  queue mutex, producers pile up, then it drains. Fix: never block on I/O while holding the
  queue lock; take the item out, release, then call.

## 6. Try it

1. **(easy)** Implement `MinStack` and test with the sequence push 3, push 5, get_min, push 2,
   push 1, get_min, pop, get_min, pop, get_min. Done when you get 3, 1, 2, 3.
2. **(easy)** Implement a queue with two stacks and prove to yourself (write it down) that n
   operations cost O(n) total. Done when 1M mixed push/pop operations run in well under a
   second.
3. **(medium)** Extend `RingBuffer<T>` in `m06_ring_buffer.cpp` with `peek(i)` (i-th element
   from the front) and `read_contiguous()` returning up to two `(pointer, length)` spans
   suitable for `writev`. Done when a wrapped buffer returns two spans whose lengths sum to
   `size()`.
4. **(medium)** Trapping rain water with a monotonic stack. Done when
   `[0,1,0,2,1,0,1,3,2,1,2,1] -> 6`; then re-solve it with two pointers and compare.
5. **(hard)** Maximal rectangle of 1s in a binary matrix via "largest rectangle in histogram"
   per row. Done when the 4x5 LeetCode example gives 6 and you can state the O(rows * cols).
6. **(hard)** Build a single-producer single-consumer lock-free ring buffer with
   `std::atomic<std::size_t>` head and tail and `alignas(64)` padding. Done when a producer
   and a consumer thread exchange 100M integers without loss or duplication under
   `-fsanitize=thread` (04_Concurrency explains acquire/release ordering).

## 7. Interview questions

**Q: Implement a queue using two stacks and give the complexity.**
Push onto `in`. To pop or peek, if `out` is empty, move all of `in` onto `out` (reverses
order), then use `out.top()`. Each element is moved from `in` to `out` once, so n operations
cost O(n): amortised O(1) per operation, O(n) worst case for a single pop.

**Q: What is a ring buffer and where is it used?**
A fixed-size array used as a queue with head/tail indices that wrap modulo the capacity.
O(1) push/pop, no allocation, cache-friendly. Used for socket and audio buffers, log rings,
and lock-free SPSC queues. Full-vs-empty is disambiguated with a count, a spare slot, or
monotonically increasing counters.

**Q: Explain the monotonic stack and its complexity.**
A stack kept sorted by popping violating elements before each push. Each element is pushed
and popped at most once, so a pass is O(n) despite the nested loop. Solves next-greater,
daily temperatures, histogram rectangle, rain water.

**Q: How do you get the sliding window maximum in O(n)?**
A deque of indices with decreasing values. For each element, pop smaller-or-equal values from
the back (they can never be the max again), push the index, drop the front if it left the
window; the front is the maximum.

**Q: Why does `std::stack::pop()` return void?**
Returning the popped value by copy could throw after the element was removed, losing it. The
standard separates `top()` and `pop()` so each is exception-safe.

**Q: How does `std::deque` give O(1) at both ends and O(1) indexing?**
It is an array of pointers to fixed-size blocks. Pushing at either end fills a block or
allocates a new one without moving elements; indexing computes block and offset with two
loads. References stay valid on end pushes; iterators do not.

**Q: When does recursion need to be converted to an explicit stack?**
When depth can reach tens of thousands (deep DFS, long lists, degenerate trees): the thread
stack (8 MB default on Linux) overflows. An explicit `std::vector` of frames lives on the heap.

## 8. Recap

- Stack: LIFO, `std::vector` with `push_back`/`pop_back`/`back`. Queue: FIFO, `std::deque` or ring buffer.
- Two-stack queue: amortised O(1), each element moved once.
- Ring buffer: fixed array, head/tail wrap; disambiguate full/empty with a count or 64-bit
  counters; power-of-two capacity for `& mask`; decide overwrite vs reject.
- `std::deque` is blocks plus a pointer map; O(1) ends, O(1) index via two loads.
- Monotonic stack: pop violators, push; each index in and out once; O(n). Next greater, daily
  temperatures, histogram rectangle (with a sentinel), rain water.
- Monotonic deque: sliding window maximum in O(n).
- Min stack: parallel stack of running minimums.
- RPN evaluation is a stack machine; shunting-yard converts infix to RPN.
- The call stack is a stack with an 8 MB limit; deep recursion becomes an explicit stack.
- Pad producer and consumer indices to separate cache lines in a concurrent ring buffer.
