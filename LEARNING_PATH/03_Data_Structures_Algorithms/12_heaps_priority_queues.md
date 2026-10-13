# 12. Heaps and priority queues

**Time:** ~3 hours   **Prerequisites:** 01, 02, 10

## 1. In plain words

A priority queue hands you the most important item first, whatever order things arrived in.
A hospital triage desk is one: patients arrive in any order, the most urgent is seen next. A
timer system is one: many timers are pending, the one that fires soonest is the one you care
about. A merge of many sorted files is one: the next output is the smallest of the heads.

A **heap** is the standard way to build a priority queue. It is a binary tree with two rules —
every parent is at least as large as its children (max-heap), and the tree is filled level by
level, left to right — stored in a plain array with no pointers at all: the children of index
`i` are at `2i+1` and `2i+2`. Finding the maximum is O(1) (it is at index 0); inserting or
removing is O(log n) because you only fix the one path from the changed spot to the root or to
a leaf. It is the simplest structure in this course that gives log-time ordered access, and the
only one with zero allocation per operation.

You have met it twice already: the k-way merge in external sort (module 08) and heapsort. This
module makes it a tool you can write in ten minutes, then applies it to the "top k", "merge k",
and "scheduler / timer wheel" problems that systems interviews ask.

## 2. The real thing

### 2.1 Array layout

```
index:   0   1   2   3   4   5   6   7   8
value:  90  70  80  30  60  50  75  10  20

              90
           /      \
         70        80
        /  \      /  \
      30   60   50   75
     /  \
   10   20

parent(i) = (i - 1) / 2       left(i) = 2i + 1       right(i) = 2i + 2
```

Complete tree: all levels full except the last, which fills left to right. That is exactly what
makes the array layout work with no gaps: the n elements occupy indices 0..n-1. Height is
floor(log2 n).

Heap property (max-heap): `a[parent(i)] >= a[i]` for every i > 0. The maximum is `a[0]`. There
is no ordering between siblings or across subtrees — a heap is **not** sorted, and in-order
traversal of it means nothing.

### 2.2 Operations

**push(x)**: append at index n, then **sift up** (swim): while the element is larger than its
parent, swap with the parent. O(log n) swaps.

**pop()**: save `a[0]`, move `a[n-1]` to index 0, shrink, then **sift down** (sink): while the
element is smaller than its larger child, swap with that child. O(log n).

**top()**: `a[0]`. O(1).

**build from n elements**: call sift down on indices n/2 - 1 down to 0. This is **O(n)**, not
O(n log n): half the nodes are leaves (0 work), a quarter move at most 1 level, an eighth at
most 2, ... Sum n/2^(k+1) * k = O(n). Pushing one at a time would be O(n log n).

**decrease-key / increase-key**: change `a[i]` then sift up or down. Needs to know `i`, which
the basic heap does not track; an **indexed heap** keeps a map from item id to position and
updates it on every swap. Dijkstra (module 13) needs decrease-key or the "lazy deletion"
alternative: push a new entry and skip stale ones when popped.

**delete(i)**: swap with the last, shrink, sift the moved element up *or* down (either may be
needed).

### 2.3 `std::priority_queue`

```cpp
#include <queue>
std::priority_queue<int> maxq;                                     // max-heap by default
std::priority_queue<int, std::vector<int>, std::greater<int>> minq; // min-heap
maxq.push(5); maxq.top(); maxq.pop(); maxq.empty(); maxq.size();

// Custom ordering: a comparator type or a lambda (C++17 needs the type and the object).
auto cmp = [](const Task& a, const Task& b) { return a.deadline > b.deadline; };  // earliest first
std::priority_queue<Task, std::vector<Task>, decltype(cmp)> q(cmp);
```

The comparator means "a has *lower* priority than b"; with `std::less` the largest is on top.
Read `std::greater` as "smallest on top" and move on; everyone mixes this up once.

Under the hood it is `std::vector` plus `std::push_heap`, `std::pop_heap`, `std::make_heap`
from `<algorithm>`, which you can use directly on any vector when you need to see or modify the
elements (`priority_queue` hides them). `std::make_heap` is the O(n) build. `std::sort_heap`
is heapsort.

No decrease-key, no iteration, no erase of arbitrary elements. For those use `std::set` /
`std::multiset` (ordered, O(log n) for everything, but allocation per element and cache
misses), or write an indexed heap, or use lazy deletion.

### 2.4 The classic problems

**Top-k largest of n** (k << n): keep a **min**-heap of size k. For each element, if it is
larger than the heap's minimum, pop the minimum and push it. O(n log k) time, O(k) space.
Streaming-friendly: you never need all n in memory. The alternative `std::nth_element` is
O(n) but needs everything in memory and gives the k in arbitrary order; `std::partial_sort`
is O(n log k) and gives them sorted.

**k-th largest**: same min-heap of size k; the answer is its top. Or `nth_element`.

**Merge k sorted lists** (total n elements): min-heap of (head value, list index). Pop the
smallest, push the next from that list. O(n log k). This is the external sort merge, the
merge step of a LSM-tree compaction, and the "merge sorted streams from k shards" problem.

**k closest points / k most frequent**: top-k with a different key.

**Median of a stream**: two heaps. A max-heap for the lower half and a min-heap for the upper
half, kept within one element of each other in size. The median is the top of the bigger heap
or the average of the two tops. O(log n) per insert, O(1) median.

**Task scheduling with cooldown, meeting rooms, CPU with priorities**: a heap keyed by "next
available time" plus simulation.

**Reorganise string / rearrange so no two adjacent are equal**: max-heap by frequency, pop two
at a time.

### 2.5 Scheduling and timers

A server with 10 000 connections, each with an idle timeout: which one expires first? A
**min-heap keyed by expiry time** answers in O(1) and updates in O(log n). The event loop's
`poll`/`epoll_wait` timeout is `heap.top().expiry - now`. When it wakes, pop everything with
`expiry <= now` and run those callbacks. Cancelling a timer: either indexed heap with
delete(i), or lazy — mark it cancelled and skip it when it pops.

The alternative at very large scale is the **timer wheel** (hashed or hierarchical): an array
of buckets indexed by `expiry / tick`, O(1) insert and cancel, timers fire at tick
granularity. Linux's kernel timers are a hierarchical wheel; `epoll`/`timerfd` give user space
a heap-friendly interface. Rule: heap for thousands of timers with arbitrary precision; wheel
for millions with coarse ticks.

CPU schedulers: Linux CFS uses a red-black tree keyed by virtual runtime (needs ordered
iteration and arbitrary removal, which a heap lacks). Thread pools with priorities use a heap
or a fixed set of per-priority FIFO queues (which is O(1) and avoids starvation issues).

## 3. Code

Files: `code/m12_heap.cpp` (a templated binary heap with comparator: push, pop, top, O(n)
build, `sift_up`/`sift_down`, an indexed variant with `decrease_key` and `erase`, invariant
check, differential test against `std::priority_queue`; top-k, k-th largest, merge k sorted,
median of a stream, k most frequent), `code/m12_scheduler.cpp` (a timer queue: schedule,
cancel with lazy deletion and with an indexed heap, `run_until(now)`; and a simulated
multi-worker job scheduler that assigns each job to the earliest-free worker, as a load
balancer would).

### Binary heap (key parts)

```cpp
template <typename T, typename Compare = std::less<T>>   // std::less -> max-heap, like std::priority_queue
class BinaryHeap {
    std::vector<T> a_;
    Compare less_;
    void sift_up(std::size_t i) {
        while (i > 0) {
            std::size_t p = (i - 1) / 2;
            if (!less_(a_[p], a_[i])) break;             // parent already >= child
            std::swap(a_[p], a_[i]);
            i = p;
        }
    }
    void sift_down(std::size_t i) {
        std::size_t n = a_.size();
        while (true) {
            std::size_t l = 2 * i + 1, r = l + 1, best = i;
            if (l < n && less_(a_[best], a_[l])) best = l;
            if (r < n && less_(a_[best], a_[r])) best = r;
            if (best == i) return;
            std::swap(a_[i], a_[best]);
            i = best;
        }
    }
public:
    void push(T x) { a_.push_back(std::move(x)); sift_up(a_.size() - 1); }
    T pop() {
        T top = std::move(a_[0]);
        a_[0] = std::move(a_.back()); a_.pop_back();
        if (!a_.empty()) sift_down(0);
        return top;
    }
    explicit BinaryHeap(std::vector<T> v, Compare c = {}) : a_(std::move(v)), less_(c) {
        for (std::size_t i = a_.size() / 2; i-- > 0;) sift_down(i);   // O(n) heapify
    }
};
```

### Top-k and merge-k (key parts)

```cpp
std::vector<int> top_k(const std::vector<int>& v, std::size_t k) {
    std::priority_queue<int, std::vector<int>, std::greater<int>> minq;   // smallest of the k on top
    for (int x : v) {
        if (minq.size() < k) minq.push(x);
        else if (x > minq.top()) { minq.pop(); minq.push(x); }           // x beats the weakest member
    }
    ...drain...
}

std::vector<int> merge_k(const std::vector<std::vector<int>>& lists) {
    using Item = std::tuple<int, std::size_t, std::size_t>;              // value, list, index
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (std::size_t i = 0; i < lists.size(); ++i) if (!lists[i].empty()) q.emplace(lists[i][0], i, 0);
    std::vector<int> out;
    while (!q.empty()) {
        auto [val, li, idx] = q.top(); q.pop();
        out.push_back(val);
        if (idx + 1 < lists[li].size()) q.emplace(lists[li][idx + 1], li, idx + 1);
    }
    return out;
}
```

## 4. Under the hood (senior-level)

### 4.1 Memory access pattern

A sift-down from the root touches indices 0, 1-2, 3-6, 7-14, ...: the first ~4 levels (15
elements, 60 bytes of `int`) sit in one or two cache lines; after that every level is a new
line, and the two children compared are adjacent so they share a line. So a pop on a 1M-heap
costs ~16 cache misses, versus ~20 for a `std::set` of the same size, with no allocation.
Push is cheaper: sift-up usually stops within a few levels (a random new element is unlikely
to be near the top).

**d-ary heaps** (each node has d children, e.g. 4 or 8) make the tree shallower — log_d n
levels — and put all d children in one cache line; pops do more comparisons per level but
fewer misses. Dijkstra implementations in routing engines use 4-ary heaps for this reason.

### 4.2 Why `build` is O(n)

Node at height h (leaves are 0) sifts down at most h levels. There are at most n / 2^(h+1)
nodes at height h. Total work <= sum over h of n/2^(h+1) * h = n * sum h/2^(h+1) = n * 1 = O(n).
The intuition: most nodes are near the bottom and move very little. This is the argument you
give when asked "is `make_heap` really linear".

### 4.3 `std::priority_queue` codegen and the hidden copy

`push` does `c.push_back(x); std::push_heap(c.begin(), c.end(), comp);`. `pop` does
`std::pop_heap(...)` (moves the top to the back, sifts the former last element down) then
`c.pop_back()`. `top()` returns a `const T&`. There is no `pop()` that returns the value — so
the idiom is `T x = std::move(const_cast<T&>(q.top())); q.pop();` if the element is expensive
to copy (ugly but standard practice), or just `T x = q.top(); q.pop();` for small `T`.
libstdc++'s `__adjust_heap` uses a "hole" technique: it moves the hole down to a leaf first,
then sifts the saved value up from there — fewer comparisons on average than the textbook
sift-down.

### 4.4 Lazy deletion vs indexed heap

Dijkstra and timer cancellation both need "change or remove an element that is not at the
top". Two approaches:

- **Indexed heap**: an `id -> position` array updated on every swap. `decrease_key(id)` sifts
  up from `pos[id]`. Exact, O(log n), more code, every swap costs two extra writes.
- **Lazy**: push a new entry with the new key; mark the old one invalid (a version counter or
  a `cancelled` flag in a side table). When popping, skip entries whose version is stale. The
  heap can grow to O(number of updates) rather than O(n), but each operation is a plain push
  and the code is tiny. This is what most production Dijkstra and timer implementations do.

### 4.5 Heaps in the systems you know

PostgreSQL: `tuplesort` uses a heap for the k-way merge of external sort runs and for
bounded (`LIMIT`) sorts — "top-N heapsort" in `EXPLAIN`. The autovacuum launcher picks the
next database to vacuum from a heap by priority. Linux: `hrtimers` use a red-black tree per
CPU (needs removal and ordered scan); older timers used wheels; `epoll` has no heap but libuv,
libevent and every event loop built on it keep a timer heap. Berkeley DB's lock manager uses
no heap, but its memory pool's buffer priority uses a priority queue. Go's runtime timers are a
4-ary heap per P. Your RPC server's "which connection times out next" is the same question.

## 5. In production

- **Per-connection timeouts.** 50 000 connections, each with its own deadline. A heap keyed by
  deadline makes the event loop's wait time `top.deadline - now` and the expiry sweep
  O(k log n) for k expired. A sweep over all connections every second is O(n) and shows up as
  a CPU sawtooth — that is the bug to look for when idle CPU is mysteriously high.
- **Top-N reports.** "Top 100 accounts by transaction volume today" over 50 million rows: a
  min-heap of 100 in one pass, O(n log 100), with 100 rows of memory. Sorting the 50 million is
  O(n log n) and needs them all in memory. The database does this for you with `ORDER BY ...
  LIMIT 100` (top-N heapsort); do the same in your own code.
- **Merging shards.** Results from 16 shards each sorted by timestamp: k-way merge with a heap
  of 16 is O(n log 16) and streams — you can start returning rows before all shards finish.
- **Rate-limited retry queues.** Failed jobs scheduled for retry at `now + backoff`: a heap by
  retry time, popped by the worker loop. Cancel by lazy marking.
- **Pitfall: heap of pointers to mutable objects.** If a scheduled task's priority field is
  modified after it is pushed, the heap invariant is silently broken and `top()` returns the
  wrong task. Priorities must be immutable while in the heap; to change one, pop-modify-push
  or use an indexed heap's `update`.
- **Pitfall: comparator direction.** `std::priority_queue<Task, vector<Task>, Cmp>` with
  `Cmp = a.deadline < b.deadline` gives you the *latest* deadline first. Test with two
  elements before trusting a new comparator.

## 6. Try it

1. **(easy)** Implement push/pop/top on a max-heap of ints in a `std::vector`, then run 100 000
   random pushes and pops in parallel with `std::priority_queue` and assert equality at every
   pop. Done when they agree and your `build` from a random vector is visibly faster than
   n pushes for n = 10^7.
2. **(easy)** k-th largest element with a min-heap of size k, and again with
   `std::nth_element`. Done when both give 5 for `[3,2,3,1,2,4,5,5,6], k=4` and you can state
   both complexities.
3. **(medium)** Median of a data stream with two heaps. Done when adding `5, 15, 1, 3` yields
   medians `5, 10, 5, 4` and 1M random inserts take under a second.
4. **(medium)** k closest points to the origin, O(n log k). Done when
   `[[1,3],[-2,2]], k=1 -> [[-2,2]]` and you can explain why a max-heap by distance is used.
5. **(hard)** Indexed heap with `decrease_key(id)` and `erase(id)`, tested against a
   `std::set<std::pair<key,id>>` mirror over 1M random operations. Done when the mirror and
   the heap agree on every `top()` and the heap never exceeds n entries.
6. **(hard)** Build a timer queue for a simulated event loop: 100 000 connections with random
   idle timeouts, random activity that resets a connection's deadline (cancel + reschedule),
   and a loop that pops expired timers. Implement it twice: lazy deletion and indexed heap.
   Done when both fire exactly the same set of timeouts in the same order and you can report
   the maximum heap size each one reached.

## 7. Interview questions

**Q: What is a heap and what are its complexities?**
A complete binary tree with the heap property (parent >= children for a max-heap), stored in an
array with children at 2i+1 and 2i+2. top O(1); push and pop O(log n); build O(n). Not sorted:
only the root is guaranteed extreme.

**Q: Why is building a heap O(n) and not O(n log n)?**
Sift-down from the bottom up: a node at height h moves at most h levels, and there are n/2^(h+1)
such nodes; the sum n * sum(h / 2^(h+1)) converges to 2n. Most nodes are near the leaves and
barely move.

**Q: Find the k largest of n elements in a stream.**
Min-heap of size k: for each element, if larger than the heap's minimum, replace it. O(n log k)
time, O(k) memory, single pass. If everything is in memory and order among the k does not
matter, `std::nth_element` is O(n).

**Q: Merge k sorted lists.**
Min-heap of (head value, list id). Pop the minimum, emit it, push the next element of that
list. O(N log k) for N total elements. Same as external merge sort's merge phase.

**Q: How do you get the median of a stream in O(log n) per element?**
A max-heap for the lower half and a min-heap for the upper half, balanced to within one
element. The median is the top of the larger heap, or the mean of both tops when equal.

**Q: `std::priority_queue` has no decrease-key. How do you handle Dijkstra or timer
cancellation?**
Lazy deletion: push a new entry and skip stale ones when popped (check against the current
best distance or a cancelled flag). Or an indexed heap that tracks each id's position so it can
sift from there. Or `std::set` with erase/insert.

**Q: Heap vs balanced BST (`std::set`) for a priority queue?**
Heap: array, no allocation, O(1) top, better cache behaviour, O(n) build; no ordered iteration
or arbitrary erase. `std::set`: O(log n) everything including erase and ordered scans, but a
node allocation per element and a cache miss per level. Use the heap unless you need those
extras.

**Q: How would you implement connection timeouts in an event loop?**
Min-heap keyed by deadline. The loop waits `top.deadline - now`; on wake, pop and handle every
expired entry. Activity on a connection reschedules by lazy cancel + new push, or by
`decrease_key` in an indexed heap. For millions of coarse timers, a timer wheel.

## 8. Recap

- Heap = complete binary tree in an array; `parent = (i-1)/2`, children `2i+1`, `2i+2`.
- Max-heap property: parent >= children; only the root is extreme; it is not sorted.
- push: append + sift up; pop: move last to root + sift down; both O(log n); top O(1).
- Build with bottom-up sift-down is O(n).
- `std::priority_queue<T>` is a max-heap; `std::greater<T>` makes it a min-heap; it wraps
  `push_heap`/`pop_heap` on a vector.
- Top-k largest: min-heap of size k, O(n log k). k-th largest: its top.
- Merge k sorted: heap of heads, O(N log k). Median of stream: two heaps.
- No decrease-key: use lazy deletion or an indexed heap.
- Timers: heap by deadline, loop waits `top - now`; timer wheel for millions at coarse ticks.
- Never mutate a priority while the element is in the heap; test comparator direction on two
  elements.
