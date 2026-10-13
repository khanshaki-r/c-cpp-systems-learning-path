# 08. Sorting

**Time:** ~4 hours   **Prerequisites:** 01, 02, 07

## 1. In plain words

Sorting puts things in order so that everything after it gets easier: binary search works,
duplicates sit next to each other, the smallest k are at the front, two sorted lists merge in
one pass. Your `orderedArray.cpp` kept the array sorted by inserting each new element in its
place — that is insertion sort, one element at a time, and it is O(n^2) for n elements because
each insertion may shift everything.

The O(n log n) sorts beat that by dividing the work. Merge sort splits the array in half,
sorts each half (recursively), and merges — always O(n log n), needs a scratch buffer, and is
stable (equal elements keep their order). Quicksort picks a pivot, moves smaller elements
left and larger right, and recurses on both sides — O(n log n) on average, in place, fastest
in practice, but O(n^2) if the pivots are bad. Heapsort uses the heap from module 12 to pull
out the minimum n times — O(n log n) always, in place, but slower because it jumps around
memory.

`std::sort` combines all three (introsort): quicksort for speed, heapsort when quicksort
starts degrading, insertion sort for the tiny pieces at the end. A senior engineer can explain
why each piece is there. And a database engineer knows one more: when the data does not fit
in memory, you sort runs that do fit and merge them from disk — external merge sort, which is
what `ORDER BY` and index builds do.

## 2. The real thing

### 2.1 The comparison sorts

**Insertion sort.** For i from 1 to n-1, take `v[i]` and shift it left past every larger
element. The prefix `v[0..i)` is always sorted. Worst case O(n^2) (reverse order), best case
O(n) (already sorted), adaptive (fast on nearly-sorted data), stable, in place. For n <= ~16
it is the fastest sort there is, which is why every library sort finishes with it.

**Selection sort.** For i from 0 to n-1, find the minimum of `v[i..n)` and swap it into `v[i]`.
Always O(n^2) comparisons, exactly n-1 swaps (minimum possible), not stable in the usual
swap form. Only useful when swaps are extremely expensive compared to comparisons.

**Merge sort.** Split, recurse, merge with a scratch buffer.
```
T(n) = 2 T(n/2) + O(n)  ->  O(n log n), always.
Space O(n) scratch. Stable (take from the left half on ties). Sequential access: cache-friendly.
```
Bottom-up variant: merge runs of 1, then 2, then 4, ... No recursion. Used for linked lists
(no random access needed; `std::list::sort` is a merge sort) and for external sorting.

**Quicksort.** Choose a pivot, partition so that elements < pivot are left and > pivot are
right, recurse on both sides.
```
Average: T(n) = 2 T(n/2) + O(n) -> O(n log n), about 1.39 n log2 n comparisons on average.
Worst: pivot is always min or max -> T(n) = T(n-1) + O(n) -> O(n^2). Happens on sorted or
       reverse-sorted input with first/last-element pivot, or on all-equal input with Lomuto.
Space: O(log n) stack if you recurse on the smaller side first and loop on the larger.
Not stable. In place.
```

Partition schemes:
- **Lomuto**: pivot at the end; `i` marks the boundary of the "< pivot" region; scan `j`,
  swap `v[j]` into position when `v[j] < pivot`; finally swap pivot into `v[i]`. Simple,
  ~3x more swaps than Hoare, degrades to O(n^2) on many equal keys (all go to one side).
- **Hoare**: two indices moving inward from both ends, swapping out-of-place pairs; stops when
  they cross. Fewer swaps, handles equal keys well (they get split between sides). Returns a
  split point, not the pivot's final index — the recursion is `[lo, p]` and `[p+1, hi]`.
- **Three-way (Dutch national flag)**: partitions into `< pivot`, `== pivot`, `> pivot` and
  recurses only on the outer two. Linear time on all-equal input. Used when many duplicates
  are expected.

Pivot choice:
- First or last element: O(n^2) on sorted input. Never do this.
- Random: expected O(n log n) on any input; an adversary cannot pick a bad input in advance.
- **Median of three** (first, middle, last): cheap, good on sorted/reverse data, what most
  libraries use. Median-of-medians gives a guaranteed O(n log n) but the constant is too high
  for practice.

**Heapsort.** Build a max-heap in O(n) (module 12), then repeatedly swap the root to the end
and sift down: n times O(log n). Always O(n log n), in place, O(1) extra space, not stable.
Slower than quicksort by about 2x in practice because the sift-down jumps across the array
(poor locality) and branches unpredictably. Its guarantee is its value.

### 2.2 `std::sort`: introsort

libstdc++'s `std::sort` (and MSVC's, and libc++'s) is **introspective sort** (Musser, 1997):

1. Quicksort with median-of-three pivot (libstdc++ uses the median of first+1, middle, last-1
   moved to first, then Hoare-style unguarded partition).
2. Track the recursion depth. If it exceeds `2 * floor(log2(n))`, the input is behaving
   adversarially: switch that sub-range to **heapsort**. This caps the worst case at
   O(n log n).
3. Stop recursing when a sub-range has <= 16 elements (`_S_threshold`). After the quicksort
   phase, run one **insertion sort** over the whole array: every element is within 16
   positions of its final place, so it is O(n) total.

Result: O(n log n) worst case guaranteed (since C++11 the standard requires it), quicksort's
speed on average, insertion sort's speed on the small pieces. It is not stable.

`std::stable_sort` is a merge sort (with an in-place fallback if the buffer allocation fails:
O(n log^2 n)). `std::partial_sort(first, middle, last)` is a heap-based selection of the
smallest `middle - first` elements in sorted order: O(n log k). `std::nth_element` is
quickselect: O(n) average, puts the k-th element in place with smaller before and larger after
— the right tool for "find the median" or "top k, order irrelevant".

### 2.3 Stability

A sort is **stable** if elements that compare equal keep their input order. It matters when you
sort by one key after another: sort transactions by time, then stable-sort by account, and
each account's transactions remain time-ordered. Stable: insertion, merge, counting, radix
(LSD), `std::stable_sort`. Not stable: selection (swap form), quicksort, heapsort,
`std::sort`. To make any sort stable, append the original index to the key as a tiebreaker.

### 2.4 Lower bound and non-comparison sorts

Any sort that only compares elements needs Omega(n log n) comparisons in the worst case: there
are n! orderings, each comparison halves the possibilities, log2(n!) ~ n log2 n. To beat it you
need to look at the keys themselves.

**Counting sort.** Keys are integers in [0, k). Count occurrences, prefix-sum the counts to get
starting positions, place each element. O(n + k) time and space, stable if you place in input
order from the back. Perfect for bytes, small enums, bucket indices.

**Radix sort (LSD).** Sort by the least significant digit (byte) with a stable counting sort,
then the next digit, ... For 32-bit keys with 8-bit digits: 4 passes of O(n + 256). O(n * d)
total with d = number of digits. Beats `std::sort` on large arrays of integers by 2 to 4x.
Not comparison-based, so it cannot take an arbitrary comparator; works for integers, fixed
floats (with a sign-bit flip), fixed-length strings.

**Bucket sort.** Distribute into buckets by range, sort each bucket, concatenate. O(n) when
the data is uniformly distributed; degenerates when it is not.

### 2.5 External merge sort (databases)

When n elements do not fit in memory of size M:

1. **Run generation**: read M elements, sort in memory (`std::sort`), write a sorted run to
   disk. Repeat: n/M runs. Replacement selection with a heap produces runs of ~2M on average
   from a buffer of M.
2. **Merge**: open k runs, keep the head of each in a min-heap (module 12), repeatedly pop the
   minimum and write it, refill from that run. k-way merge is O(n log k) comparisons and one
   sequential read of all runs. If k is too large for the available buffers, merge in multiple
   passes (each pass reads and writes everything once).

I/O cost: 2 x (number of passes) x n. With a 1 GB sort buffer and 100 GB of data, run
generation makes 100 runs; a 100-way merge finishes in one more pass. Total I/O: 4 x 100 GB.
Making the buffer bigger reduces the number of runs; making the fan-in bigger reduces the
passes. PostgreSQL's `work_mem` controls the buffer; when a sort exceeds it the planner's
`EXPLAIN ANALYZE` shows `Sort Method: external merge  Disk: NNN kB`. Index build (`CREATE
INDEX`) on a B-tree sorts the keys this way and then builds the tree bottom-up from the sorted
stream (module 11). `maintenance_work_mem` is the buffer for that.

### 2.6 Choosing

| Situation | Use |
|---|---|
| General, in memory, any type | `std::sort` |
| Need stability | `std::stable_sort` (or sort pairs with index) |
| Only the k smallest in order | `std::partial_sort` |
| k-th element or median, order irrelevant | `std::nth_element` |
| Integers, n large, speed critical | LSD radix sort |
| Keys in a small range | counting sort |
| Linked list | merge sort (`std::list::sort`) |
| Nearly sorted | insertion sort, or `std::sort` (handles it fine) |
| Does not fit in memory | external merge sort |
| Streaming, need sorted output continuously | heap (priority queue) |

## 3. Code

Files: `code/m08_sorting.cpp` (insertion, selection, merge top-down and bottom-up, quicksort
with Lomuto / Hoare / three-way partition and median-of-three, heapsort, counting sort, LSD
radix sort; all tested against `std::sort`; timing on 1M ints; a demonstration of quicksort's
worst case with a bad pivot), `code/m08_external_merge.cpp` (simulated external sort: splits a
large vector into runs of size M, sorts each, and k-way merges with a heap, counting "I/O"
operations).

Key parts:

```cpp
// Hoare partition: returns j such that [lo, j] <= pivot <= [j+1, hi]. Both sides non-empty.
int partition_hoare(std::vector<int>& v, int lo, int hi) {
    int pivot = v[lo + (hi - lo) / 2];
    int i = lo - 1, j = hi + 1;
    while (true) {
        do { ++i; } while (v[i] < pivot);     // stop at >= pivot: equal keys get split
        do { --j; } while (v[j] > pivot);
        if (i >= j) return j;
        std::swap(v[i], v[j]);
    }
}
void quicksort_hoare(std::vector<int>& v, int lo, int hi) {
    while (lo < hi) {
        int p = partition_hoare(v, lo, hi);
        // Recurse on the smaller half, loop on the larger: stack depth stays O(log n).
        if (p - lo < hi - p) { quicksort_hoare(v, lo, p); lo = p + 1; }
        else                 { quicksort_hoare(v, p + 1, hi); hi = p; }
    }
}
```

```cpp
// LSD radix sort on unsigned 32-bit keys, 4 passes of 8 bits, stable counting sort per pass.
void radix_sort(std::vector<std::uint32_t>& v) {
    std::vector<std::uint32_t> tmp(v.size());
    for (int shift = 0; shift < 32; shift += 8) {
        std::array<std::size_t, 257> count{};
        for (std::uint32_t x : v) ++count[((x >> shift) & 0xFF) + 1];
        for (int i = 0; i < 256; ++i) count[i + 1] += count[i];   // prefix sum = start positions
        for (std::uint32_t x : v) tmp[count[(x >> shift) & 0xFF]++] = x;   // stable placement
        v.swap(tmp);
    }
}
```

```cpp
// Heapsort: build max-heap bottom-up, then extract max n times.
void sift_down(std::vector<int>& v, int i, int n) {
    while (true) {
        int largest = i, l = 2 * i + 1, r = l + 1;
        if (l < n && v[l] > v[largest]) largest = l;
        if (r < n && v[r] > v[largest]) largest = r;
        if (largest == i) return;
        std::swap(v[i], v[largest]);
        i = largest;
    }
}
void heapsort(std::vector<int>& v) {
    int n = static_cast<int>(v.size());
    for (int i = n / 2 - 1; i >= 0; --i) sift_down(v, i, n);         // O(n) build
    for (int end = n - 1; end > 0; --end) { std::swap(v[0], v[end]); sift_down(v, 0, end); }
}
```

## 4. Under the hood (senior-level)

### 4.1 Why quicksort beats mergesort and heapsort in practice

All three are O(n log n). Quicksort's partition is a tight loop over contiguous memory with
one comparison per element and a predictable access pattern — the prefetcher keeps up, and
it needs no extra buffer, so it uses half the memory bandwidth of merge sort. Heapsort's
sift-down touches `2i+1`, `4i+3`, ... — stride doubling, a new cache line each step past the
first few levels. Mergesort writes every element to the scratch buffer and back at each of
log n levels: 2x the memory traffic. Measured on 1M random ints (typical): `std::sort` ~70 ms,
`std::stable_sort` ~90 ms, heapsort ~150 ms, radix sort ~20 ms. Run `m08_sorting.cpp` for your
numbers.

### 4.2 Branch misprediction in sorting

Each comparison in quicksort is a branch that is unpredictable on random data (50/50). That is
roughly 1.39 n log2 n mispredictions at 15 to 20 cycles each — a large fraction of the total
time. Tricks that remove branches: branchless partition (write both candidates, select with a
conditional move), block partitioning (BlockQuicksort), and pattern-defeating quicksort
(`pdqsort`, which is also what Rust's `sort_unstable` and recent libc++ `std::sort` use).
Radix sort has no data-dependent branches at all, which is a second reason it is fast.

### 4.3 The 16-element threshold

Below ~16 elements, the O(n^2) of insertion sort is maybe 100 comparisons, all on one or two
cache lines, with a loop that the branch predictor learns quickly. The recursive overhead of
quicksort (function calls, pivot selection, partition bookkeeping) costs more than that. The
exact threshold is tuned empirically; libstdc++ uses 16, libc++ uses 6 for trivially copyable
types and 30 otherwise.

### 4.4 Introsort's depth limit, exactly

libstdc++: `__introsort_loop(first, last, depth_limit = 2 * __lg(n))`. Each recursion level
decrements `depth_limit`; at 0 it calls `__partial_sort(first, last, last)` which is a
heapsort of that range. On random data the depth stays around log2(n) + a bit, so heapsort is
never triggered. On the "median-of-three killer" sequence (a crafted input) it would be
triggered and performance degrades to heapsort's constant — still O(n log n). Since C++11 the
standard guarantees `std::sort` is O(n log n) worst case; before, it allowed O(n^2).

### 4.5 Sorting in PostgreSQL

`tuplesort.c`: starts with an in-memory quicksort (actually a specialised introsort variant),
switches to external merge when `work_mem` is exceeded, using replacement selection
(historically) or just quicksorted runs (since PG 10, because larger runs from replacement
selection did not pay for the heap's cache misses) and a k-way merge with a heap of tape
heads. `EXPLAIN (ANALYZE, BUFFERS)` shows `Sort Method: quicksort Memory: NNNkB` or `external
merge Disk: NNNkB`. Berkeley DB does not sort; its B-tree keeps keys ordered on insert — the
bulk-load path in your migration engine benefited from pre-sorted input because sequential
inserts into a B-tree hit the same leaf page repeatedly (module 11).

## 5. In production

- **Sorting before bulk insert.** Loading 100M rows into a B-tree index with random keys does a
  random page write per row. Sorting first makes consecutive inserts hit the same leaf: 100x
  fewer page writes. Your Berkeley DB -> YugabyteDB migration almost certainly read in key
  order for this reason; if it did not, that is the first optimisation to mention.
- **`ORDER BY` with `LIMIT`.** PostgreSQL uses a bounded heap (top-k, module 12) instead of a
  full sort when `LIMIT k` is small: O(n log k) and O(k) memory. In your own code, `std::partial_sort`
  or `std::nth_element` is the equivalent — do not sort 10M rows to show the top 100.
- **Stability bugs.** A report sorted by amount shows transactions of equal amount in a
  different order every run because `std::sort` is unstable and the input order varies. If the
  display order matters, add a tiebreaker (timestamp, id) to the comparator. This is a
  frequent "flaky test" cause.
- **Comparator bugs.** A comparator that is not a strict weak ordering (`<=` instead of `<`,
  or inconsistent with equality) makes `std::sort` read out of bounds — a real crash, not just
  a wrong order, because the unguarded partition relies on the pivot stopping the scan. Rule:
  `comp(a, a)` must be false; `comp(a, b) && comp(b, c)` implies `comp(a, c)`.
- **Sorting pointers vs sorting objects.** Sorting a vector of 1 KB records moves 1 KB per
  swap. Sort a vector of indices or pointers instead, then permute once (or just read through
  the index). `std::sort` on 64-byte or smaller elements is fine to do directly.
- **3 AM story (typical).** A nightly batch took 6 hours instead of 1. `EXPLAIN ANALYZE` on the
  query showed `external merge Disk: 48 GB` where it used to say `quicksort Memory`. The data
  grew past `work_mem`. Fix: raise `work_mem` for that session, or add an index so the sort
  disappears. Knowing what "external merge" means is the difference between a 10-minute and a
  2-day investigation.

## 6. Try it

1. **(easy)** Rewrite your `orderedArray.cpp` insert as an insertion sort over a whole
   `std::vector<int>`, then count comparisons and shifts on sorted, reverse-sorted and random
   input of 1000 elements. Done when you can explain why the sorted case is O(n).
2. **(easy)** Implement merge sort for `std::vector<int>` with a single scratch buffer
   allocated once (not per recursion). Done when it matches `std::sort` on 1M random ints and
   you can state its time, space and stability.
3. **(medium)** Make quicksort go quadratic: use the first element as pivot and feed it a
   sorted array of 100 000 elements. Time it. Then switch to median-of-three and time again.
   Done when you have both numbers and the recursion did not overflow the stack (recurse on
   the smaller side).
4. **(medium)** Implement three-way partition quicksort and test on an array of 1M elements
   that are all the same value, then on one with 10 distinct values. Compare with Lomuto.
   Done when Lomuto is visibly quadratic on the all-equal input and three-way is linear.
5. **(hard)** Implement `nth_element` via quickselect and use it to find the median of 10M
   random ints; compare the time with a full `std::sort`. Done when quickselect is at least 3x
   faster and you can explain the expected O(n) via the geometric series of partition sizes.
6. **(hard)** Extend `m08_external_merge.cpp` to a two-pass merge when the number of runs
   exceeds the fan-in (e.g. 100 runs, fan-in 10). Count total "block reads" and "block writes".
   Done when the count equals 2 x n x (number of passes) and you can explain what
   `work_mem` controls in PostgreSQL.

## 7. Interview questions

**Q: How does `std::sort` work?**
Introsort: quicksort with median-of-three pivots; if recursion depth exceeds 2 log2 n, that
range switches to heapsort (guaranteeing O(n log n)); ranges of 16 or fewer are left for a
final insertion sort pass. Not stable. `std::stable_sort` is merge sort.

**Q: Quicksort's worst case — when and how to avoid it?**
O(n^2) when every pivot is an extreme element: sorted input with first/last pivot, or many
equal keys with Lomuto partition. Avoid with median-of-three or random pivots, Hoare or
three-way partition for duplicates, and a depth limit falling back to heapsort (introsort).

**Q: Merge sort vs quicksort?**
Merge: always O(n log n), stable, O(n) extra space, sequential access, good for lists and
external sorting. Quick: O(n log n) average, O(n^2) worst (mitigated), in place, not stable,
fastest in practice because of locality and no buffer traffic.

**Q: What is stability and when does it matter?**
Equal elements keep their input order. Matters for multi-key sorts (sort by time, then stable
by account) and for deterministic output. `std::sort` is not stable; add a tiebreaker or use
`std::stable_sort`.

**Q: Can you sort faster than O(n log n)?**
Not with comparisons alone (information-theoretic lower bound: log2(n!) comparisons). With
key structure, yes: counting sort O(n + k), radix sort O(n * digits), bucket sort O(n) on
uniform data.

**Q: How would you sort 100 GB of data with 1 GB of RAM?**
External merge sort: read 1 GB chunks, sort each in memory, write ~100 sorted runs; then
k-way merge the runs with a min-heap of run heads, streaming output to disk. Two passes over
the data; more if the fan-in is limited by buffer memory.

**Q: How do you find the median of a large array quickly?**
`std::nth_element` (quickselect): partition around a pivot, recurse only into the side
containing position n/2. Expected O(n), worst O(n^2) with bad pivots (median-of-medians fixes
it in theory). No full sort needed.

**Q: Why does a bad comparator crash `std::sort` instead of just mis-sorting?**
The partition loop is unguarded: it relies on the pivot element to stop the scan. A comparator
that returns true for equal elements (`<=`) lets the scan run past the pivot and off the end of
the array.

## 8. Recap

- Insertion sort: O(n^2) worst, O(n) nearly-sorted, stable; fastest for n <= 16.
- Merge sort: O(n log n) always, stable, O(n) buffer, sequential; lists and external sorting.
- Quicksort: O(n log n) average, O(n^2) worst; median-of-three or random pivot; Hoare or
  three-way partition for duplicates; recurse on the smaller side for O(log n) stack.
- Heapsort: O(n log n) always, in place, not stable, ~2x slower (locality).
- Introsort = quicksort + depth-limit fallback to heapsort + final insertion sort; `std::sort`.
- `std::stable_sort` is merge sort; `partial_sort` for top-k in order; `nth_element` for
  median/top-k unordered.
- Comparison sorts need Omega(n log n); counting and radix sort beat it using key structure.
- External merge sort: sorted runs of memory size, then k-way merge with a heap; passes x 2n I/O.
- Comparators must be strict weak orderings or `std::sort` reads out of bounds.
- Sort keys before bulk-loading a B-tree; use top-k instead of a full sort for `LIMIT`.
