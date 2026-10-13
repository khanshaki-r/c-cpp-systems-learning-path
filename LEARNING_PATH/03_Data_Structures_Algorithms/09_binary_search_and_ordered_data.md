# 09. Binary search and ordered data

**Time:** ~3 hours   **Prerequisites:** 01, 02, 08

## 1. In plain words

You wrote binary search in `orderedArray.cpp`: look at the middle, throw away the half that
cannot contain the key, repeat. Twenty steps find anything in a million sorted items. This
module is about everything that idea can do beyond "is the key here": find the *first* place
a key could go, the *last*, count how many equal keys exist, find the smallest value that
satisfies a condition even when there is no array at all ("search on the answer"), and — the
connection to your resume — find the right child pointer inside a B-tree page.

The reason binary search deserves a module is that most people get it subtly wrong. Off-by-one
on the bounds, infinite loops when `mid` rounds the wrong way, overflow in `(lo + hi) / 2`.
Knuth noted that the first bug-free binary search appeared 16 years after the first published
one. The fix is not more care; it is a single template you memorise, with one invariant you can
state in a sentence, and never deviate from.

## 2. The real thing

### 2.1 The one template: `lower_bound`

Find the first index `i` in a sorted range such that `v[i] >= target` (or `n` if none). This is
`std::lower_bound`. Use half-open bounds `[lo, hi)`:

```cpp
std::size_t lower_bound(const std::vector<int>& v, int target) {
    std::size_t lo = 0, hi = v.size();          // answer is in [lo, hi]; hi == n means "not found"
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;   // no overflow; rounds down so mid < hi
        if (v[mid] < target) lo = mid + 1;      // mid is too small: answer is strictly after it
        else hi = mid;                          // v[mid] >= target: mid might be the answer
    }
    return lo;
}
```

Invariant to say aloud: "everything before `lo` is `< target`; everything from `hi` on is `>=
target`." The loop ends when `lo == hi`, the boundary. It cannot loop forever because `mid <
hi` always (rounding down) so `hi = mid` shrinks, and `lo = mid + 1 > mid >= lo` grows.

Every other variant is this template with a different predicate or a +1:

| Want | Code |
|---|---|
| first index with `v[i] >= t` | `lower_bound(t)` |
| first index with `v[i] > t` (`upper_bound`) | same loop, condition `v[mid] <= t` -> `lo = mid + 1` |
| does `t` exist | `i = lower_bound(t); i < n && v[i] == t` |
| first occurrence of `t` | `lower_bound(t)` (check it equals `t`) |
| last occurrence of `t` | `upper_bound(t) - 1` (check) |
| count of `t` | `upper_bound(t) - lower_bound(t)` |
| insert position keeping sorted | `lower_bound(t)` (before equals) or `upper_bound(t)` (after) |
| largest `v[i] <= t` | `upper_bound(t) - 1` |
| smallest `v[i] >= t` | `lower_bound(t)` |

`std::lower_bound(v.begin(), v.end(), t)`, `std::upper_bound`, `std::equal_range` (both at
once), `std::binary_search` (bool) do all of this. Know them, use them in production, and be
able to write the loop by hand in an interview.

### 2.2 Search on a predicate (the general form)

Binary search does not need an array. It needs a **monotone predicate** `P(x)` over an ordered
domain: false, false, ..., false, true, true, ..., true. It finds the first `x` where `P(x)` is
true. The array version is `P(i) = (v[i] >= target)`. Other domains:

- **Search on the answer.** "Minimum capacity so that the tasks fit in d days": `P(cap) = can
  we finish in d days with capacity cap?` is monotone (more capacity never hurts). Binary
  search `cap` in `[max(task), sum(tasks)]`, each check O(n). Total O(n log(sum)). Same shape:
  Koko eating bananas, split array largest sum, minimum days to make bouquets, allocate
  minimum pages, `sqrt(x)` to integer precision.
- **Bisection on reals.** Root of a continuous function `f` with `f(lo) < 0 < f(hi)`: halve
  the interval 50 to 60 times for double precision. Used for solving equations without a
  closed form (interest rate from payments — an actual banking calculation).
- **Lower bound of the first "bad" version** (`git bisect`): `P(version) = is broken`.

The template is unchanged; only `P` and the domain differ:

```cpp
// first x in [lo, hi) with P(x) true, or hi if none; P must be monotone false->true
long long first_true(long long lo, long long hi, const std::function<bool(long long)>& P) {
    while (lo < hi) {
        long long mid = lo + (hi - lo) / 2;
        if (P(mid)) hi = mid; else lo = mid + 1;
    }
    return lo;
}
```

### 2.3 Rotated sorted array

`[4,5,6,7,0,1,2]` is sorted then rotated. At any `mid`, at least one of `[lo, mid]` and `[mid,
hi]` is properly sorted (the one that does not contain the rotation point). Check whether the
target lies inside the sorted half (plain comparison with its ends); if yes go there, else go to
the other half. O(log n). Finding the minimum (the rotation point): `P(i) = v[i] <= v[last]` is
monotone — binary search for its first true. With duplicates the problem degrades to O(n) in
the worst case because `v[lo] == v[mid] == v[hi]` gives no information.

### 2.4 2D and other shapes

- **Row-sorted matrix where each row starts after the previous ends**: treat it as one array of
  `rows * cols`, index `(mid / cols, mid % cols)`.
- **Matrix sorted by rows and by columns independently**: start top-right, go left if too big,
  down if too small — O(rows + cols), a two-pointer "staircase" not a binary search.
- **Peak element**: `P(i) = v[i] > v[i+1]` is not globally monotone, but binary search still
  finds *a* peak because moving toward the larger neighbour always has a peak ahead.
- **Median of two sorted arrays** in O(log(min(m, n))): binary search the partition point in
  the smaller array. The classic hard one; know the idea (partition both so left halves have
  the right size and `maxLeft <= minRight`), do not expect to derive it under pressure.

### 2.5 Binary search inside a B-tree node

A B-tree page (module 11) holds a sorted array of up to a few hundred keys and the child
pointers between them. Descending the tree means: in this page, find the child whose key range
contains the search key. That is exactly `upper_bound(keys, target)`: the index of the first
key `> target` is the index of the child to follow (child `i` holds keys in `[key[i-1],
key[i])`). For a leaf, `lower_bound` tells you whether the key exists and where a cursor
should start a range scan. PostgreSQL's `_bt_binsrch` is this loop with a twist: it remembers
the result of the search on the previous page to skip comparisons when inserting sorted
data. With 300 keys per page, each page costs 8 to 9 comparisons; a 4-level tree costs ~35
comparisons total — and 4 page reads, which dominate.

Why not a linear scan inside the page? For keys that fit a cache line (integers), a linear
scan of 16 keys is about as fast as 4 branchy comparisons and vectorises; for 300 keys or for
string keys, binary search wins. Some engines (e.g. in-memory tries like ART, or "Eytzinger
layout" arrays) reorder keys so that binary search's memory accesses become sequential.

### 2.6 `std::map`, `std::set` and sorted vectors

`std::map<K, V>` and `std::set<K>` are red-black trees (module 11): O(log n) insert/find/erase,
sorted iteration, `lower_bound`/`upper_bound` members, stable iterators. Per element: a node
of 32 bytes + payload, one allocation, a cache miss per level (~log2 n misses per lookup).

A **sorted `std::vector`** with `std::lower_bound` has the same O(log n) lookup with far better
constants (contiguous, half the cache misses, 8x less memory) but O(n) insert. Use it when the
data is built once or rarely changes (configuration tables, static lookup tables, a snapshot
sorted once per batch). Boost's `flat_map` and C++23's `std::flat_map` are this pattern
packaged. Rule: `std::map` for heavy interleaved insert and lookup; sorted vector for
build-then-query; `unordered_map` when order and range queries are not needed.

## 3. Code

File: `code/m09_binary_search.cpp` — hand-written `lower_bound`/`upper_bound`, first/last
occurrence and count, `first_true` on a predicate, integer square root, search on answer (ship
capacity / Koko bananas), rotated array search and minimum, peak element, 2D matrix search,
bisection on a real function (loan interest rate), and a benchmark of `std::map::find` versus
`std::lower_bound` on a sorted vector at 1M entries. All with asserts.

Key parts beyond the template above:

```cpp
// Search on answer: minimum ship capacity to send all packages within `days` days.
int ship_within_days(const std::vector<int>& w, int days) {
    auto feasible = [&](long long cap) {
        int used = 1; long long load = 0;
        for (int x : w) {
            if (load + x > cap) { ++used; load = 0; }
            load += x;
        }
        return used <= days;
    };
    long long lo = *std::max_element(w.begin(), w.end());        // must fit the heaviest item
    long long hi = std::accumulate(w.begin(), w.end(), 0LL);     // one day for everything
    while (lo < hi) {
        long long mid = lo + (hi - lo) / 2;
        if (feasible(mid)) hi = mid; else lo = mid + 1;
    }
    return static_cast<int>(lo);
}
```

```cpp
// Rotated array search: one half is always sorted; decide if target is inside that half.
int search_rotated(const std::vector<int>& v, int t) {
    int lo = 0, hi = static_cast<int>(v.size()) - 1;
    while (lo <= hi) {                                   // closed bounds here: we test v[mid] == t
        int mid = lo + (hi - lo) / 2;
        if (v[mid] == t) return mid;
        if (v[lo] <= v[mid]) {                           // left half sorted
            if (v[lo] <= t && t < v[mid]) hi = mid - 1; else lo = mid + 1;
        } else {                                         // right half sorted
            if (v[mid] < t && t <= v[hi]) lo = mid + 1; else hi = mid - 1;
        }
    }
    return -1;
}
```

Note the closed-interval form here (`lo <= hi`, `hi = mid - 1`): it is natural when you test
for equality at `mid` and can discard `mid` from both sides. The half-open form is natural
when you are looking for a boundary. Pick the one that matches the question, and know why.

## 4. Under the hood (senior-level)

### 4.1 Overflow and the famous bug

`int mid = (lo + hi) / 2` overflows when `lo + hi > INT_MAX`, i.e. arrays over ~1 billion
elements or when `hi` is `INT_MAX` in a search-on-answer. Java's `Arrays.binarySearch` had
this bug for nine years. `lo + (hi - lo) / 2` cannot overflow when `lo <= hi`. With
`std::size_t` the sum cannot be negative but can still wrap past 2^64 in theory; the subtraction
form is correct everywhere. Make it a habit.

### 4.2 Branches, cache lines and the cost of a comparison

Each iteration of binary search is a data-dependent branch that is unpredictable (50/50 on
random queries): ~15 cycles of misprediction per step. For 20 steps that is ~300 cycles,
already more than the comparisons. `std::lower_bound` in libstdc++ is written to let the
compiler produce a conditional move (`cmov`) for the `lo`/`hi` update instead of a branch — a
"branchless binary search" — and gains 2x on large arrays. The first few probes (middle,
quarters) are the same for every query and stay in L1; the last ~log2(n) - 10 probes are
scattered and each costs a cache miss (~100 ns) on arrays larger than L2. So a binary search
over 16M ints costs ~10 cache misses ~ 1 us, dominated by memory, not by the 24 comparisons.
Prefetching both possible next probes (`__builtin_prefetch`) hides some of it.

### 4.3 Why B-trees exist (a preview)

If a binary search over 16M keys costs 10 cache misses, and a disk page read costs 100 us or
more, you want each memory access (or each disk read) to eliminate as much of the search space
as possible. A B-tree node with 300 keys eliminates 299/300 of the remaining keys per page read,
versus 1/2 per probe for binary search. That turns 24 probes into 3 or 4 page reads — and
within each page you binary search anyway. Binary search is the inner loop of every ordered
index.

### 4.4 Interpolation and exponential search

If keys are uniformly distributed, **interpolation search** guesses `mid` proportionally
(`lo + (t - v[lo]) * (hi - lo) / (v[hi] - v[lo])`) and takes O(log log n) probes on average,
O(n) worst case. **Exponential (galloping) search** probes 1, 2, 4, 8, ... from the start to
bracket the target, then binary searches within the bracket: O(log i) where i is the answer's
position — the right tool when the answer is likely near the beginning, and what merge
algorithms (Timsort) use when one run is much longer than the other.

## 5. In production

- **Time-series lookup.** Log records sorted by timestamp in a vector: "first record after T"
  is `lower_bound`; "records between T1 and T2" is `[lower_bound(T1), lower_bound(T2))`. This is
  the pattern for any append-only, time-ordered buffer, including WAL segment indexes.
- **Rate tables and tiers.** Interest rate tiers by balance, fee schedules by amount: a sorted
  vector of thresholds and `upper_bound(amount) - 1` gives the tier in O(log n). Do not write
  the if-else ladder.
- **Capacity planning via search on answer.** "Smallest thread pool that keeps p99 under 50 ms"
  is a monotone predicate over pool size, evaluated by load test. Binary search needs 5 or 6
  runs instead of 40.
- **Version bisection.** When a regression appears between release 1.40 and 1.72, `git bisect`
  finds the commit in ~5 builds. Same with configuration changes in a deployment history.
- **Debug story (typical).** A lookup table of fee thresholds returned the wrong tier for
  amounts exactly equal to a threshold. The code used a hand-written search with `v[mid] <
  target` in one branch and `<=` in another — a hybrid of `lower_bound` and `upper_bound`.
  Equal values fell on the wrong side. The fix was to delete the hand-written loop and call
  `std::upper_bound`, and the lesson was to decide "where do equal keys go" before writing
  anything.
- **Sorted vector vs map in the hot path.** A `std::map<int, Handler*>` with 200 entries was
  looked up per message. Replacing it with a sorted `std::vector<std::pair<int, Handler*>>`
  and `lower_bound` removed ~8 cache misses per lookup — measurable at 200k messages per
  second. Build-once tables should be vectors.

## 6. Try it

1. **(easy)** Implement `lower_bound` and `upper_bound` by hand and test against the standard
   library on 1000 random arrays with duplicates. Done when every result matches, including
   targets below the minimum and above the maximum.
2. **(easy)** Count occurrences of `t` in a sorted array in O(log n). Done when
   `[1,2,2,2,3], 2 -> 3` and `[1,3], 2 -> 0`.
3. **(medium)** Integer square root: largest `r` with `r*r <= x` for `x` up to 10^18. Done when
   `isqrt(10^18) == 10^9` and `isqrt(2^63 - 1)` returns the right value without overflow (use
   `mid <= x / mid` rather than `mid * mid <= x`).
4. **(medium)** Koko eating bananas: minimum eating speed to finish all piles within `h`
   hours. Done when `[3,6,7,11], 8 -> 4` and `[30,11,23,4,20], 5 -> 30`.
5. **(hard)** Find minimum in a rotated sorted array **with duplicates**. Done when
   `[2,2,2,0,1] -> 0`, `[1,1,1] -> 1`, and you can explain why the worst case is O(n).
6. **(hard)** Implement a B-tree-style node search: given a node with sorted keys
   `k[0..m)` and children `c[0..m]`, return the child index to descend into for target `t`
   (child `i` holds keys in `[k[i-1], k[i])`). Then write the leaf version that returns the
   position for a cursor to start a `>= t` range scan. Done when you reuse the same
   `lower_bound`/`upper_bound` function for both and can say which one and why.

## 7. Interview questions

**Q: Write binary search without bugs.**
Half-open `[lo, hi)`, `mid = lo + (hi - lo) / 2`, `if (v[mid] < t) lo = mid + 1; else hi =
mid;`, loop while `lo < hi`, return `lo`. State the invariant: before `lo` everything is less
than `t`, from `hi` on everything is at least `t`. Check `v[lo] == t` for existence.

**Q: `lower_bound` vs `upper_bound`?**
`lower_bound` is the first element `>= t`; `upper_bound` is the first element `> t`. Between
them lie all elements equal to `t`; their difference is the count. Insert at `upper_bound` to
keep stability.

**Q: What is "binary search on the answer"?**
When the answer is a number and "is x feasible" is monotone (feasible for x implies feasible
for all larger x), binary search x over its range, evaluating feasibility with a linear check.
O(n log(range)). Examples: minimum ship capacity, Koko bananas, split array largest sum.

**Q: How do you search a rotated sorted array in O(log n)?**
At `mid`, one of the two halves is sorted (compare `v[lo]` and `v[mid]`). If the target lies
within that sorted half's range, search it; otherwise search the other half. With duplicates
the worst case degrades to O(n).

**Q: Why `lo + (hi - lo) / 2` instead of `(lo + hi) / 2`?**
The sum can overflow when the indices are large (or when searching on a value range up to
`INT_MAX`). The difference form never exceeds `hi`.

**Q: How is binary search used inside a B-tree?**
Each node holds sorted separator keys; `upper_bound` of the target among them gives the child
index to descend; in a leaf, `lower_bound` gives the slot where the key is or where a range
scan starts. Node fan-out of hundreds means a few page reads replace dozens of probes.

**Q: Sorted vector with `lower_bound` versus `std::map` — when each?**
Sorted vector: build once, query many; smaller memory, fewer cache misses, O(n) insert.
`std::map`: interleaved inserts and lookups, stable iterators, O(log n) everything with
a cache miss per level.

**Q: Why is `std::lower_bound` faster than a textbook loop on large arrays?**
It is written so the compiler emits conditional moves rather than branches, avoiding ~15
cycles of misprediction per step, and its access pattern keeps the first probes in L1.

## 8. Recap

- One template: half-open `[lo, hi)`, `mid = lo + (hi - lo) / 2`, move `lo` past "too small",
  move `hi` onto "could be the answer"; return `lo`.
- `lower_bound`: first `>= t`. `upper_bound`: first `> t`. Count = difference. Last occurrence
  = `upper_bound - 1`.
- Any monotone predicate over an ordered domain is binary-searchable: search on the answer,
  bisection on reals, version bisection.
- Rotated arrays: one half is sorted; check whether the target lies inside it.
- Peak finding works by always moving toward the larger neighbour.
- Closed bounds (`lo <= hi`) when testing equality at `mid`; half-open when finding a boundary.
- Each late probe on a big array is a cache miss; branchless search and B-tree fan-out exist to
  cut those.
- B-tree descent is `upper_bound` on the node's keys; leaf positioning is `lower_bound`.
- Sorted vector beats `std::map` for build-once tables; `std::map` for interleaved updates.
- Decide where equal keys go before writing the loop.
