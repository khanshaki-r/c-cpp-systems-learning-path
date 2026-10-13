# 02. Arrays, vectors and two pointers

**Time:** ~4 hours   **Prerequisites:** 01

## 1. In plain words

An array is a row of boxes in memory, all the same size, side by side. Box i is at
`start + i * sizeof(element)`. That one multiplication is why indexing is O(1) and why arrays
are the fastest structure a CPU knows: the next element is literally the next address.

`std::vector` is the array you should use in C++. It owns a heap block, remembers how many
boxes are in use (`size()`) and how many are allocated (`capacity()`), and grows itself. Your
`LowArray` class in `lowArray.cpp` wrapped a vector and then re-did by hand what the vector
already does (resize, shift on delete). From this module on, you use `std::vector` directly and
spend your effort on the algorithms.

The algorithms in this module share one idea: **walk the array with more than one index at a
time, and never walk backwards.** Two pointers, sliding window and prefix sums all turn an
O(n^2) "for every i, for every j" into an O(n) single pass. These three patterns alone solve
roughly a quarter of all array interview questions.

## 2. The real thing

### 2.1 `std::vector` essentials

```cpp
std::vector<int> v;            // empty, size 0, capacity 0
v.reserve(100);                // capacity >= 100, size still 0. No elements constructed.
v.resize(10);                  // size 10, elements value-initialised (0 for int)
v.push_back(5);                // size 11, amortised O(1)
v.emplace_back(5);             // same, constructs in place (matters for non-trivial types)
v[3] = 7;                      // no bounds check. UB if 3 >= size().
v.at(3) = 7;                   // bounds-checked, throws std::out_of_range
v.front(); v.back();           // first / last element (UB if empty)
v.pop_back();                  // O(1), removes last
v.insert(v.begin() + i, x);    // O(n - i): shifts tail right
v.erase(v.begin() + i);        // O(n - i): shifts tail left. Same as your delete loop.
v.erase(v.begin() + i, v.begin() + j);  // erase range [i, j)
v.clear();                     // size 0, capacity unchanged
v.shrink_to_fit();             // request capacity == size (non-binding)
v.data();                      // int* to the contiguous buffer. Pass to C APIs.
v.empty(); v.size();           // size() returns std::size_t (unsigned!)
```

Half-open ranges: `[begin, end)` everywhere in the standard library. `end()` points one past
the last element. `erase(first, last)` removes `last - first` elements. Get used to this; it
makes "empty range" and "length = end - begin" fall out naturally and is the convention you
will use for your own binary search bounds in module 09.

**Iterator invalidation.** After `push_back` that reallocates, or any `insert`/`erase`, every
iterator, pointer and reference into the vector may be dangling. The classic bug:

```cpp
for (auto it = v.begin(); it != v.end(); ++it)
    if (*it % 2 == 0) v.erase(it);          // UB: it is invalid after erase
```

Correct forms:

```cpp
for (auto it = v.begin(); it != v.end(); )
    if (*it % 2 == 0) it = v.erase(it);     // erase returns the next valid iterator
    else ++it;
// Better, O(n) instead of O(n^2):
v.erase(std::remove_if(v.begin(), v.end(), [](int x) { return x % 2 == 0; }), v.end());
```

The erase-remove idiom (second form) is the two-pointer pattern in disguise: `remove_if` keeps
a write index that only advances for kept elements, then `erase` chops the tail once.

**Signed/unsigned.** `v.size()` is `std::size_t`. `v.size() - 1` when size is 0 wraps to
18446744073709551615. `for (int i = 0; i < v.size(); ++i)` warns under `-Wextra` (sign
compare) and fails under `-Werror`. Either loop with `std::size_t`, or take
`int n = static_cast<int>(v.size());` once at the top and use `int` throughout. In problem
code this course does the latter, because many loops need to go down to -1 or compare with
`i - 1`.

### 2.2 Pattern 1: two pointers

Two indices move through the array, each only in one direction. Total movement <= 2n, so O(n).

- **Opposite ends** (`l = 0, r = n-1`, move towards each other): sorted two-sum, container
  with most water, reverse in place, palindrome check, 3-sum inner loop.
- **Same direction, slow/fast** (`write` trails `read`): remove duplicates, remove element,
  move zeroes, partition. The write index marks the boundary of the "good" prefix.
- **Two arrays** (`i` in A, `j` in B): merge two sorted arrays, intersection of sorted arrays.

The invariant you state out loud in an interview: "everything before `write` is the finished
output; everything between `write` and `read` is garbage I will overwrite; everything from
`read` on is unprocessed."

### 2.3 Pattern 2: sliding window

A contiguous subarray `[l, r]` grows at the right and shrinks at the left. Each index moves at
most n times: O(n). Use it when the question says "contiguous subarray/substring" and
"longest / shortest / count with property P", where P is **monotone**: if a window violates P,
every larger window containing it also violates P (or the reverse). Sum >= k with non-negative
numbers is monotone. "At most k distinct" is monotone. "Sum equals exactly k with negatives"
is **not** — use prefix sums + hash for that.

Fixed window (size k): add `v[r]`, remove `v[r-k]`, one pass.
Variable window: `for r: add v[r]; while (window invalid) remove v[l++]; record answer`.

### 2.4 Pattern 3: prefix sums

`P[0] = 0, P[i] = v[0] + ... + v[i-1]`. Then `sum(v[i..j)) = P[j] - P[i]` in O(1) after O(n)
build. Use for: many range-sum queries, "count subarrays with sum == k" (store seen prefix
sums in a hash map), "subarray sum divisible by k" (prefix sums mod k), 2D range sums
(2D prefix table). Works for any invertible operation: sum, XOR, product without zeros.

### 2.5 Pattern 4: in-place deletion

Your `array.cpp` shifted elements one cell left after finding the key. For deleting **many**
elements that is O(n^2). The slow/fast two-pointer does all deletions in one O(n) pass:

```
read ->  [3, 0, 1, 0, 0, 5]
write    keep if != 0: write 3, write 1, write 5  -> [3,1,5,_,_,_], new size = 3
```

If order does not matter, swap the deleted element with the last one and `pop_back`: O(1).

### 2.6 Pattern 5: Kadane's algorithm

Maximum sum of a contiguous subarray. The idea: at each index decide "extend the best
subarray ending at i-1, or start fresh at i". `cur = max(v[i], cur + v[i])`, `best = max(best,
cur)`. It is a one-dimensional dynamic programme (module 14) with O(1) state. Generalises to
maximum product (track min and max because negatives flip), circular arrays (total - minimum
subarray), and "best time to buy and sell stock" (differences).

## 3. Code

Files: `code/m02_vector_basics.cpp` (vector API, invalidation, erase-remove),
`code/m02_two_pointers.cpp` (problems 1 to 4), `code/m02_sliding_window.cpp` (problems 5, 6),
`code/m02_prefix_sums_kadane.cpp` (problems 7, 8). Each has a small `check()` harness so you
can add your own cases. Below, every problem: statement, reasoning, solution, complexity.

### Problem 1 — Two Sum on a sorted array

*Given a sorted array and a target, return indices of two numbers that add to target.*

Reasoning: brute force is O(n^2). Sorted order means if `v[l] + v[r] < target`, the only way
to increase the sum is to move `l` right (moving `r` left can only decrease it). Symmetric
for `> target`. Each step eliminates one index for good.

```cpp
std::pair<int, int> two_sum_sorted(const std::vector<int>& v, int target) {
    int l = 0, r = static_cast<int>(v.size()) - 1;
    while (l < r) {
        long long s = static_cast<long long>(v[l]) + v[r];  // avoid int overflow
        if (s == target) return {l, r};
        if (s < target) ++l; else --r;
    }
    return {-1, -1};
}
```
O(n) time, O(1) space. Unsorted input: use a hash map (module 04) — O(n) time, O(n) space.

### Problem 2 — Remove duplicates from a sorted array in place

*Return the new length; the first k elements must be the unique values in order.*

Reasoning: because it is sorted, duplicates are adjacent. Keep `write` as the end of the unique
prefix. For each `read`, if `v[read] != v[write-1]`, copy it in.

```cpp
int remove_duplicates(std::vector<int>& v) {
    if (v.empty()) return 0;
    int write = 1;
    for (int read = 1; read < static_cast<int>(v.size()); ++read)
        if (v[read] != v[write - 1]) v[write++] = v[read];
    return write;
}
```
O(n), O(1). The invariant: `v[0..write)` is strictly increasing and equals the unique values
seen so far.

### Problem 3 — Move zeroes to the end, preserving order of non-zeroes

Reasoning: same slow/fast pattern. Write non-zeroes forward, then fill the tail with zeroes.
Or swap-in-place so the zeroes bubble right (fewer writes when few zeroes).

```cpp
void move_zeroes(std::vector<int>& v) {
    int write = 0;
    for (int read = 0; read < static_cast<int>(v.size()); ++read)
        if (v[read] != 0) std::swap(v[write++], v[read]);  // swap is a no-op when write == read
}
```
O(n), O(1). Note that the swap version does at most n swaps and keeps relative order.

### Problem 4 — Container with most water

*`h[i]` is a wall height. Pick two walls so that the water between them (width x min height)
is maximal.*

Reasoning: start with the widest container `l = 0, r = n-1`. The area is limited by the
shorter wall. Moving the taller wall inward can never help (width shrinks, height is still
capped by the shorter one). So move the shorter one. Every step discards one wall that cannot
be part of a better answer.

```cpp
long long max_area(const std::vector<int>& h) {
    int l = 0, r = static_cast<int>(h.size()) - 1;
    long long best = 0;
    while (l < r) {
        long long area = static_cast<long long>(r - l) * std::min(h[l], h[r]);
        best = std::max(best, area);
        if (h[l] < h[r]) ++l; else --r;
    }
    return best;
}
```
O(n), O(1). The proof-by-elimination argument is what the interviewer wants to hear.

### Problem 5 — Minimum size subarray with sum >= target (positive integers)

Reasoning: positives make sum monotone in window size. Grow `r` until sum >= target, then
shrink `l` while it stays >= target, recording the length.

```cpp
int min_subarray_len(int target, const std::vector<int>& v) {
    int n = static_cast<int>(v.size()), l = 0, best = n + 1;
    long long sum = 0;
    for (int r = 0; r < n; ++r) {
        sum += v[r];
        while (sum >= target) {          // shrink while still valid
            best = std::min(best, r - l + 1);
            sum -= v[l++];
        }
    }
    return best == n + 1 ? 0 : best;
}
```
O(n): `l` and `r` each move at most n times. O(1) space. If the array had negatives this
breaks (shrinking could make the sum larger) — then you need prefix sums + a monotonic deque
(module 06).

### Problem 6 — Longest subarray of 1s after flipping at most k zeroes

Reasoning: a window is valid if it contains <= k zeroes. That is monotone. Track zero count.

```cpp
int longest_ones(const std::vector<int>& v, int k) {
    int l = 0, zeros = 0, best = 0;
    for (int r = 0; r < static_cast<int>(v.size()); ++r) {
        if (v[r] == 0) ++zeros;
        while (zeros > k) if (v[l++] == 0) --zeros;
        best = std::max(best, r - l + 1);
    }
    return best;
}
```
O(n), O(1). The template "add right, while invalid drop left, record" is worth memorising
verbatim; it solves "longest substring with at most k distinct", "fruit into baskets",
"longest repeating character replacement" with only the validity test changed.

### Problem 7 — Count subarrays with sum exactly k (integers may be negative)

Reasoning: sliding window fails with negatives. `sum(i..j) = P[j+1] - P[i] = k` means for each
`j` we need the number of earlier prefix sums equal to `P[j+1] - k`. A hash map from prefix
sum to count gives that in O(1).

```cpp
int subarray_sum_equals_k(const std::vector<int>& v, int k) {
    std::unordered_map<long long, int> seen;   // prefix sum -> how many times seen
    seen[0] = 1;                                // empty prefix
    long long prefix = 0;
    int count = 0;
    for (int x : v) {
        prefix += x;
        auto it = seen.find(prefix - k);
        if (it != seen.end()) count += it->second;
        ++seen[prefix];
    }
    return count;
}
```
O(n) time, O(n) space. Same skeleton solves "longest subarray with sum k" (store first index
instead of count) and "subarray sums divisible by k" (store `prefix % k`, normalised to
non-negative).

### Problem 8 — Maximum subarray sum (Kadane)

```cpp
long long max_subarray(const std::vector<int>& v) {
    long long cur = v[0], best = v[0];
    for (std::size_t i = 1; i < v.size(); ++i) {
        cur = std::max<long long>(v[i], cur + v[i]);  // extend or restart
        best = std::max(best, cur);
    }
    return best;
}
```
O(n), O(1). Why it works: the best subarray ending at i either is just `v[i]` or extends the
best one ending at i-1. If `cur` ever goes negative, it is worth dropping, because any future
subarray would be better off without it. To return the indices, record `start` when you
restart and `(start, i)` when `best` updates.

### Also in the code files

- Merge two sorted arrays into the first one, which has room at the end: fill from the back
  so nothing is overwritten (`m02_two_pointers.cpp`).
- Rotate array by k using three reversals (`m02_two_pointers.cpp`).
- Range-sum queries with a prefix array (`m02_prefix_sums_kadane.cpp`).
- Best time to buy and sell stock as Kadane on differences (`m02_prefix_sums_kadane.cpp`).

## 4. Under the hood (senior-level)

### 4.1 What `std::vector<int>` actually is

Three pointers: `begin`, `end` (one past last used), `end_of_storage` (one past allocated).
`sizeof(std::vector<int>)` is 24 bytes on 64-bit libstdc++. `size()` is `end - begin`,
`capacity()` is `end_of_storage - begin`. `push_back` with spare capacity is: construct at
`*end`, increment `end`. That inlines to roughly three instructions plus the store — which is
why `push_back` into a reserved vector is as fast as writing to a C array.

Reallocation calls `operator new` for the new block, **move**-constructs (or copies, if the
type's move constructor is not `noexcept`) each element into it, destroys the old elements,
frees the old block. For trivially copyable types like `int` the move loop becomes a `memcpy`.
For `std::string` it moves 32-byte objects (three pointers plus SSO buffer, module 03). For a
type with a throwing move constructor the vector falls back to copying to keep the strong
exception guarantee — mark your move constructors `noexcept`.

### 4.2 Why `v[i]` has no bounds check

`operator[]` compiles to one load with a computed address. `at()` adds a compare and a branch.
In a hot loop that branch is nearly free (predicted), but the standard chose to leave the
choice to you. Under `-D_GLIBCXX_ASSERTIONS` libstdc++ adds a check to `operator[]` as well;
turn it on in debug builds. Out-of-range `operator[]` is undefined behaviour: in practice it
reads the next heap object, which is how you get the "works in debug, corrupt in release" bug.

### 4.3 Why two pointers is fast beyond Big-O

Both indices move forward. Every memory access is to a cache line already fetched or the next
one. The hardware prefetcher sees two sequential streams and keeps both fed. Branch prediction
works well because the "move l or move r" decision is often locally consistent. A two-pointer
pass over 10^7 ints finishes in a few milliseconds; the equivalent hash-map solution, also
O(n), is typically 10 to 30x slower because each lookup is a random access.

### 4.4 `std::array`, C arrays, and spans

`std::array<int, N>` is a C array with a size and iterators, no heap. Use it for fixed small
buffers (a 256-entry character count, a 4-entry direction table). A raw `int arr[100]` like in
your `array.cpp` decays to `int*` when passed to a function and loses its length; `std::array`
does not. C++20 has `std::span` for "pointer plus length" views; in C++17 pass `(const int*
data, std::size_t n)` or a `const std::vector<int>&`.

### 4.5 Growth factor, memory and the allocator

With 2x growth the peak memory during reallocation is old + new = 3n elements. With 1.5x it is
2.5n and freed blocks can be coalesced and reused by the allocator more often (because the sum
of previous sizes exceeds the next size: 1 + 1.5 > 2.25). That is Folly's and MSVC's argument
for 1.5. libstdc++ chose 2 for fewer reallocations. For your server, if a vector holds
hundreds of MB, `reserve` once from a known upper bound and the growth factor stops mattering.

## 5. In production

- **Request buffers.** The receive buffer in a TCP server is a vector of bytes that grows as
  partial reads arrive. Without `reserve` to the expected message size, a 64 KB message
  arriving in 1 KB chunks causes about 6 reallocations and copies — on every request. Reserve.
- **Compacting a list of active connections.** Removing closed connections from a vector one at
  a time with `erase` is O(n^2) per sweep. Use `remove_if` + `erase` (one pass), or swap with
  back and `pop_back` if order does not matter.
- **Sliding window in monitoring.** "Requests in the last 60 seconds" is a sliding window over a
  time-ordered array of timestamps. Keep `l` at the oldest timestamp still inside the window.
  This is also how rate limiters are implemented.
- **Prefix sums in reporting.** "Sum of transactions between day i and day j" over a year of
  daily totals: build the prefix array once, answer any range in O(1). Databases do the same
  thing with materialised running totals when queries are heavy and writes are light.
- **Kadane in anomaly detection.** The maximum-sum window of (latency - baseline) finds the
  worst sustained degradation period in a metrics series.
- **Debugging story.** A vector of `Session*` was iterated while a callback inside the loop
  called `push_back` on the same vector. Reallocation moved the buffer; the loop's iterator
  pointed into freed memory. Symptom: random crash under load, never in test. Fix: iterate by
  index with the size snapshotted, or collect new sessions into a separate vector and append
  after the loop. Iterator invalidation is the vector's one sharp edge; respect it.

## 6. Try it

1. **(easy)** Rewrite your `array.cpp` using `std::vector<int>`: build from the arrivals list,
   search with `std::find`, delete with `erase`. Done when it compiles under `-Wall -Wextra
   -Werror` with no `using namespace std`.
2. **(easy)** Squares of a sorted array (may contain negatives) in sorted order, O(n). Hint:
   two pointers from both ends, fill the output from the back. Done when
   `[-4,-1,0,3,10] -> [0,1,9,16,100]`.
3. **(medium)** Longest subarray with at most two distinct values ("fruit into baskets").
   Use the variable-window template with a count map of size <= 3. Done when
   `[1,2,1,2,3] -> 4` and `[1,2,3,2,2] -> 4`.
4. **(medium)** Product of array except self, O(n), no division. Hint: prefix products from the
   left into the output, then a running suffix product from the right. Done when
   `[1,2,3,4] -> [24,12,8,6]` and `[0,1] -> [1,0]`.
5. **(medium)** Three Sum: all unique triplets summing to zero. Sort, fix `i`, two-pointer the
   rest, skip duplicates. O(n^2). Done when `[-1,0,1,2,-1,-4] -> {[-1,-1,2],[-1,0,1]}` with no
   duplicate triplets.
6. **(hard)** Maximum sum circular subarray. Hint: answer is max(Kadane(v), total - min
   subarray), with a special case when all elements are negative. Done when `[5,-3,5] -> 10`
   and `[-3,-2,-3] -> -2`.

## 7. Interview questions

**Q: Why is `std::vector` preferred over `std::list` even for frequent middle insertion?**
Contiguous memory gives cache locality and prefetching; shifting a few thousand elements with
`memmove` is faster than one cache-missing pointer chase. Lists win only when elements are
huge and non-movable, or when you hold iterators that must survive insertions elsewhere.

**Q: What does `reserve` do and when do you call it?**
Allocates capacity without constructing elements, so subsequent `push_back`s do not
reallocate. Call it when you know the final size or a good upper bound, e.g. reading n rows
from a query. It does not change `size()`.

**Q: What invalidates vector iterators?**
Any reallocation (push_back past capacity, insert, reserve to a larger value) invalidates all.
`erase`/`insert` invalidate iterators at and after the position even without reallocation.
`pop_back` invalidates only the end iterator and the erased element's.

**Q: How do you turn an O(n^2) pair search into O(n)?**
If sorted: two pointers from both ends, moving the one that must move. If unsorted: one pass
with a hash set of seen values. If it is about contiguous ranges: sliding window (monotone
property) or prefix sums with a hash map (exact sums, negatives).

**Q: When does sliding window fail and what replaces it?**
When the property is not monotone in window size, e.g. "sum == k" with negative numbers.
Replace with prefix sums and a hash map keyed by prefix value.

**Q: Explain Kadane's algorithm and why it is correct.**
Maintain the best sum of a subarray ending at i: `cur = max(v[i], cur + v[i])`. If the running
sum turns negative, any later subarray does better without it, so restarting is safe. The
answer is the max over all i. O(n), O(1).

**Q: `for (int i = 0; i < v.size(); ++i)` fails to compile with `-Werror`. Why?**
`size()` returns unsigned `size_t`; comparing with signed `int` triggers `-Wsign-compare`.
Either use `size_t` for `i` or cast `size()` to `int` once.

## 8. Recap

- `std::vector` is three pointers; `push_back` is amortised O(1); `insert`/`erase` at i is O(n-i).
- Ranges are half-open `[begin, end)`; `size()` is unsigned — cast once or loop with `size_t`.
- Reallocation invalidates all iterators; `erase` returns the next valid one; prefer erase-remove.
- Two pointers: opposite ends on sorted data, slow/fast for in-place filtering, one per array for merging.
- Sliding window: "add right, while invalid drop left, record"; needs a monotone property.
- Prefix sums: `sum(i..j) = P[j+1] - P[i]`; with a hash map handles exact sums and negatives.
- In-place deletion of many elements is one slow/fast pass, not repeated shifting.
- Kadane: extend or restart; O(n) max subarray; generalises to products and circular arrays.
- Sequential access patterns are 10 to 30x faster than hash lookups at the same Big-O.
- Reserve receive buffers; never `push_back` to a vector you are iterating.
