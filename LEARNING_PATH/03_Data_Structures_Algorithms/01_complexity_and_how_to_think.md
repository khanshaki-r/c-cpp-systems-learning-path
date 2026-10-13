# 01. Complexity and how to think

**Time:** ~2 hours   **Prerequisites:** none

## 1. In plain words

When you wrote `array.cpp`, deleting the number 9 meant finding it (a loop) and then shifting
everything after it one cell left (another loop). If the array had 10 elements that was fast.
If it had 10 million, the shift alone would be 10 million copies. "Complexity" is the habit of
asking, before you run anything: *how does the amount of work grow when the input grows?*

Big-O notation is the shorthand for the answer. O(n) means "work grows in proportion to the
input size n". O(1) means "work does not grow at all". O(n^2) means "double the input, four times
the work". It deliberately ignores constants: whether a loop does 3 or 30 operations per
element, both are O(n), because for large n the shape of the curve matters more than its
slope. Think of it like describing a journey: "it is a flight" versus "it is a drive" tells you
more about a 3000 km trip than the exact speed of the car.

The senior twist is that constants *do* matter once you are in production. Two O(n) algorithms
can differ by 50x because one walks memory in order and the other jumps around. Big-O picks the
algorithm; cache behaviour picks the implementation. This module teaches both habits.

## 2. The real thing

### 2.1 Definitions

- **n** — the size of the input: array length, string length, number of nodes, number of bits
  in a number (for arithmetic algorithms).
- **T(n)** — the number of "basic operations" (comparisons, assignments, memory reads) the
  algorithm performs on an input of size n, in the worst case unless stated otherwise.
- **O(f(n))** — "T(n) is eventually at most c * f(n) for some constant c". An upper bound.
  Formally: T(n) is O(f(n)) if there exist c > 0 and n0 such that T(n) <= c * f(n) for all n >= n0.
- **Omega(f(n))** — a lower bound. **Theta(f(n))** — both; the tight bound. Interviewers say
  "Big-O" when they mean Theta almost always. Use the same convention.
- **Worst / average / best case** — three different functions. Quicksort is O(n log n) average,
  O(n^2) worst. Say which one you mean.
- **Space complexity** — extra memory beyond the input. Recursion depth counts, because each
  frame lives on the stack.

### 2.2 Rules for counting

1. **Sequential statements add.** `O(n) + O(n) = O(n)`; `O(n) + O(n^2) = O(n^2)`. Keep the
   dominant term.
2. **Nested loops multiply.** A loop of n inside a loop of n is O(n^2). A loop of n inside a
   loop of m is O(n * m) — do not collapse to n^2 unless n == m.
3. **Drop constants and lower-order terms.** `3n^2 + 20n + 7` is O(n^2).
4. **Logarithms appear when you halve.** A loop where `i *= 2` or `n /= 2` each step runs
   log2(n) times. Binary search is the canonical example: each step throws away half.
5. **Base of the logarithm does not matter.** log2(n) and log10(n) differ by a constant factor.
6. **Recursion: write the recurrence, then solve it.**
   - `T(n) = T(n/2) + O(1)` -> O(log n) (binary search)
   - `T(n) = 2T(n/2) + O(n)` -> O(n log n) (merge sort)
   - `T(n) = T(n-1) + O(1)` -> O(n) (linear recursion)
   - `T(n) = T(n-1) + O(n)` -> O(n^2) (e.g. naive selection sort written recursively)
   - `T(n) = 2T(n-1) + O(1)` -> O(2^n) (naive Fibonacci)
   The Master Theorem covers `T(n) = aT(n/b) + O(n^d)`: compare d with log_b(a). If
   d < log_b(a) the answer is O(n^(log_b a)); if equal, O(n^d log n); if greater, O(n^d).
   You rarely need it in interviews beyond the three common cases above.

### 2.3 Worked examples

```cpp
// Example A: O(n). One pass.
int sum = 0;
for (std::size_t i = 0; i < v.size(); ++i) sum += v[i];

// Example B: O(n^2). Inner loop runs n, n-1, n-2, ... 1 times. Sum = n(n-1)/2.
for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = i + 1; j < n; ++j)
        if (v[i] == v[j]) ++dups;

// Example C: O(n log n). Outer loop n times, inner loop log n times.
for (std::size_t i = 0; i < n; ++i)
    for (std::size_t k = 1; k < n; k *= 2) work();

// Example D: O(n). Looks nested but the inner pointer never goes backwards.
// Total moves of j across the whole run <= n. This is the two-pointer pattern (module 02).
std::size_t j = 0;
for (std::size_t i = 0; i < n; ++i) {
    while (j < n && v[j] < v[i] + k) ++j;
}

// Example E: O(sqrt(n)). Loop until i*i exceeds n.
for (long i = 2; i * i <= n; ++i) if (n % i == 0) return false;
```

Example D is the one beginners get wrong. The question to ask for any nested loop is not
"how many loops are there" but "how many times does the *inner body* execute in total".

### 2.4 Amortised cost

Amortised analysis answers: "if I do a sequence of m operations, what is the average cost per
operation, even if some individual operations are expensive?" This is not the same as average
case (which is about random inputs). Amortised is about the worst-case sequence.

The classic example is `std::vector::push_back`. Most pushes write one element: O(1). When
capacity is full, the vector allocates a bigger block, copies all n elements, frees the old
block: O(n). If capacity doubles each time, how much copying happens across n pushes?

```
push #       1  2  3  4  5  6  7  8  9 ... 16  17 ...
capacity     1  2  4  4  8  8  8  8 16 ... 16  32
copies on    0  1  2  0  4  0  0  0  8 ...  0  16
   growth
```

Total copies for n pushes = 1 + 2 + 4 + ... + n/2 < n. So n pushes cost at most n + n = 2n
element writes. Divide by n: **2 per push, amortised O(1)**. The general formula with growth
factor g: the geometric series 1 + 1/g + 1/g^2 + ... sums to g/(g-1). For g = 2 that is 2;
for g = 1.5 it is 3. So 1.5x growth copies more in total (3n vs 2n) but wastes less memory
(at most 50% slack vs 100%). libstdc++ uses 2x; MSVC uses 1.5x. Both are amortised O(1).

What breaks it: growing by a constant (`capacity += 100`). Then growth happens every 100
pushes and copies n elements each time: total ~ n^2 / 200, amortised O(n) per push. That is
why you never write a "grow by fixed step" dynamic array.

The accounting view: charge every push 3 "coins". It spends 1 to write itself and banks 2.
When a growth of size n happens, the n/2 elements pushed since the last growth have banked n
coins between them — enough to pay for copying all n elements. The bank never goes negative,
so the amortised cost is 3 = O(1).

### 2.5 Space

Count what you allocate beyond the input:

- A few scalars: O(1).
- An array of size n (prefix sums, visited flags, a hash set of all elements): O(n).
- A 2D table n by m (edit distance): O(n * m).
- Recursion depth d: O(d) stack frames. Depth-first search on a path graph of n nodes recurses
  n deep: O(n) stack. Merge sort recurses log n deep: O(log n) stack plus O(n) temp buffer.

In interviews, "in-place" means O(1) extra space, and the input may be modified.

### 2.6 Common complexity table

| Complexity | Name | Example | n = 10^6 ops (typical) |
|---|---|---|---|
| O(1) | constant | array index, hash lookup (average), stack push | 1 |
| O(log n) | logarithmic | binary search, balanced BST op, heap push/pop | ~20 |
| O(n) | linear | single pass, linear search, BFS on a path | 10^6 |
| O(n log n) | linearithmic | merge sort, heapsort, sort then sweep | ~2 * 10^7 |
| O(n^2) | quadratic | nested loops, bubble/insertion/selection sort, naive pair search | 10^12 (too slow) |
| O(n^3) | cubic | Floyd-Warshall, naive matrix multiply | only for n <= ~500 |
| O(2^n) | exponential | subsets, naive recursion with two branches | only for n <= ~25 |
| O(n!) | factorial | permutations | only for n <= ~11 |

Rule of thumb for a coding round (one CPU, ~1 second, typical judge): about 10^8 simple
operations. So n = 10^5 rules out O(n^2) (10^10) and demands O(n log n) or better. n = 10^3
allows O(n^2). n = 20 allows O(2^n). Read the constraints in the problem statement first; they
tell you the target complexity before you think about the algorithm.

## 3. Code

`code/m01_vector_growth.cpp` prints the capacity after each push so you can see the doubling,
counts the total number of element copies, and compares three ways to build an array of n
integers: `push_back` with no reserve, `push_back` after `reserve(n)`, and grow-by-constant
(a hand-written array that grows by 1024 each time). It also times a row-major versus a
column-major sweep of a 2D array to show the cache effect.

Key part:

```cpp
// Watch capacity change. Each change is a reallocation: allocate, copy, free.
std::vector<int> v;
std::size_t last_cap = 0;
long long copies = 0;
for (int i = 0; i < 1000; ++i) {
    v.push_back(i);
    if (v.capacity() != last_cap) {
        copies += static_cast<long long>(v.size() - 1);   // elements moved on this growth
        std::printf("size=%4zu capacity=%4zu\n", v.size(), v.capacity());
        last_cap = v.capacity();
    }
}
std::printf("total element copies: %lld (n=1000, bound 2n=2000)\n", copies);
```

On libstdc++ you will see 1, 2, 4, 8, ..., 1024: exact doubling. Total copies = 1023 < 2n.

Why the course never writes `using namespace std;`: in a 50-line homework file it is harmless.
In a header, or any file that is included or grown over years, it pulls every name from
`std` into the global scope. `std::size`, `std::data`, `std::count`, `std::distance`,
`std::begin` — all collide with names you are likely to have in a server codebase. Writing
`std::` costs five characters and tells the reader exactly where the name comes from. All code
files in this course use the `std::` prefix.

## 4. Under the hood (senior-level)

### 4.1 Why Big-O lies: the memory hierarchy

A modern CPU core executes a simple instruction in well under a nanosecond. Reading a value
from main memory (DRAM) takes roughly 60 to 100 ns (typical for a desktop; see Intel/AMD
optimisation manuals for exact figures). That is a factor of 100 or more. To hide it, the CPU
has caches: L1 (about 32 to 48 KB per core, ~4 cycles), L2 (hundreds of KB to a few MB, ~12
cycles), L3 (tens of MB shared, ~40 cycles). Data moves between memory and cache in **cache
lines** of 64 bytes.

Consequences:

- When you read `v[i]`, the CPU fetches 64 bytes, i.e. 16 `int`s. Reading `v[i+1]` next is
  almost free. Sequential access is cheap; the hardware **prefetcher** also notices the
  pattern and fetches ahead.
- A linked list node lives wherever `new` put it. Following `next` is a fresh cache miss each
  time. Walking a list of n nodes is O(n) in Big-O and O(n) cache misses in practice; walking a
  vector of n ints is O(n) in Big-O and O(n/16) cache misses.
- This is why "vector beats list for almost everything, including inserting in the middle for
  sizes up to a few thousand" is a true statement that surprises people who only know Big-O.
  Shifting 1000 ints (4 KB, 64 cache lines, sequential) is faster than one pointer chase that
  misses L3.

The 2D sweep in `m01_vector_growth.cpp` demonstrates it: row-major (inner loop walks
contiguous memory) is typically 5 to 10x faster than column-major (inner loop jumps by a full
row each step) on a 4096 x 4096 `int` matrix, with the same O(n^2) count.

### 4.2 Branch prediction

The CPU guesses which way an `if` will go and starts executing early. A wrong guess costs ~15
to 20 cycles (pipeline flush). A loop over random data with `if (v[i] > 128) sum += v[i]` is
much slower than the same loop over sorted data, because sorted data makes the branch
predictable. Same Big-O, different wall-clock. Interviewers for systems roles like to ask
"why would sorting the input first make this loop faster?" — this is the answer.

### 4.3 Allocation cost

`new` and `malloc` are not O(1) in the way a comparison is. They take a lock or hit a
thread-local cache, search a free list, possibly call `mmap`/`brk`. A data structure that
allocates per element (list, `std::map`, `std::unordered_map` nodes) pays this per insert.
A vector pays it log n times total. When you say "O(1) insert" for a list, add "plus one
allocation" in your head.

### 4.4 Big-O of library calls

Memorise these, because you will build on them:

| Call | Complexity |
|---|---|
| `std::vector::push_back`, `pop_back`, `operator[]` | O(1) amortised / O(1) / O(1) |
| `std::vector::insert` or `erase` at position p | O(n - p) |
| `std::sort` | O(n log n) worst case (introsort, module 08) |
| `std::lower_bound` on random-access range | O(log n) |
| `std::unordered_map` find/insert/erase | O(1) average, O(n) worst (all keys in one bucket) |
| `std::map` find/insert/erase | O(log n) |
| `std::priority_queue` push/pop/top | O(log n) / O(log n) / O(1) |
| `std::string::operator+=` | O(1) amortised per char |
| `std::list::splice` | O(1) |
| `std::deque::push_front/back` | O(1) amortised |

## 5. In production

- **The 3 AM version of O(n^2):** a request handler that, for each of n open sessions, scans
  the list of n sessions to find duplicates. Works fine in testing with 50 sessions. In
  production with 20 000 sessions it is 400 million comparisons per request. CPU goes to
  100%, latency climbs, the pager goes off. The fix is a hash set (module 04) and the symptom
  to recognise is "it got slower as the system got busier, non-linearly".
- **Hidden O(n) inside a loop:** `std::vector::erase(v.begin())` inside a `for` loop over the
  vector is O(n^2). `std::string s; for (...) s = s + c;` is O(n^2) because `s + c` builds a
  new string each time (use `+=`). `std::find` inside a loop is O(n^2). These are the three
  most common accidental quadratics in C++ code review.
- **Reserve when you know the size.** If you are reading 1 million rows from a database cursor
  into a vector, `v.reserve(count)` avoids ~20 reallocations and the copying that goes with
  them. Not asymptotically important; measurably important.
- **Your Berkeley DB migration engine:** reading a B-tree sequentially via cursor is O(n) with
  excellent locality (leaf pages are contiguous and linked, module 11). Doing a point lookup per
  key instead would be O(n log n) with a random page read each time. Same output, 100x slower
  on a cold cache. Knowing the access pattern of the structure is what tells you which to use.
- **Measure before you believe.** Big-O gets you to the right algorithm. After that, a profiler
  (`perf` on Linux, see the Linux systems course) tells you where the time actually goes, and
  it is usually cache misses or allocations, not instruction count.

## 6. Try it

1. **(easy)** For each snippet below, write the Big-O time. Done when you have an answer and a
   one-line justification for each.
   ```cpp
   for (i = 0; i < n; ++i) for (j = 0; j < 10; ++j) f();            // (a)
   for (i = 1; i < n; i *= 3) f();                                   // (b)
   for (i = 0; i < n; ++i) for (j = i; j < n; ++j) f();             // (c)
   for (i = 0; i < n; ++i) for (j = 1; j < i; j *= 2) f();          // (d)
   void r(int n) { if (n <= 1) return; r(n/2); r(n/2); f(); }       // (e)
   ```
2. **(easy)** Compile and run `code/m01_vector_growth.cpp`. Note the capacity sequence. Change
   the growth of the hand-written array from +1024 to x2 and observe the copy count. Done when
   your copy count for n = 1 000 000 is below 2n.
3. **(medium)** Prove that growth factor 1.5 gives amortised copy cost at most 3 per push.
   Write the geometric series and sum it. Done when you can do it on paper in 5 minutes.
4. **(medium)** In `m01_vector_growth.cpp`, change the matrix size to 1024 x 1024 and then to
   8192 x 8192. Record row-major vs column-major times. Explain why the ratio changes with
   size (hint: at which size does one row stop fitting in L1? in L2?). Done when you can
   explain the ratio in terms of cache sizes on your machine (`lscpu` prints them).
5. **(hard)** Write a function that takes a sorted `std::vector<int>` and a target and counts
   pairs (i < j) with `v[i] + v[j] == target`, in O(n). Then write the O(n^2) version. Time
   both at n = 100 000. Done when the O(n) version finishes in under 1 ms and you can state
   why the inner pointer in your O(n) version never moves backwards.

## 7. Interview questions

**Q: What is the difference between O, Theta and Omega?**
O is an upper bound, Omega a lower bound, Theta is both. Interviewers usually say O and mean
Theta. If an algorithm is O(n^2) worst case but O(n) best case, say "O(n^2) worst, O(n) best".

**Q: `push_back` is O(1) amortised. Explain.**
Most pushes write one slot. On a full buffer the vector doubles capacity and copies n
elements. Total copies across n pushes form a geometric series 1 + 2 + 4 + ... < 2n, so the
average per push is bounded by a constant. Growing by a fixed amount instead would make the
total quadratic.

**Q: Two algorithms are both O(n). Can one be 50x faster?**
Yes. Big-O hides constants. Sequential memory access (vector) hits cache; pointer chasing
(linked list) misses cache on every node. Branch predictability and allocation count also
matter. Big-O chooses the algorithm family; the memory access pattern chooses the speed.

**Q: What is the complexity of this nested loop?** (shows two-pointer code)
O(n). The inner loop's index never resets; across the entire outer loop it moves at most n
times in total. Count total executions of the inner body, not the nesting depth.

**Q: How do you analyse a recursive function?**
Write the recurrence: how many sub-calls, on what size, plus how much work outside the
calls. Solve it with the standard cases: halving with constant work is O(log n), two halves
with linear merge is O(n log n), two branches reducing by one is O(2^n).

**Q: What is the space complexity of recursive DFS?**
O(depth of recursion) for the stack plus whatever you allocate (visited set: O(V)). On a path
graph the depth is O(V), which can overflow a default 8 MB stack at around 100 000 frames.

**Q: n = 10^5 in the problem constraints. What complexity do you aim for?**
O(n log n) or O(n). O(n^2) is 10^10 operations, too slow for a one-second limit.

## 8. Recap

- Big-O describes how work grows with input size; it drops constants and lower-order terms.
- Sequential code adds, nested loops multiply, halving gives log, two-branch recursion gives 2^n.
- For nested loops count total executions of the inner body; two-pointer loops are O(n).
- Amortised cost is the average over a worst-case sequence; vector doubling gives amortised O(1)
  push with at most 2n copies; constant-step growth gives O(n) per push.
- Space counts extra memory plus recursion depth.
- n = 10^5 means O(n log n); n = 10^3 allows O(n^2); n = 20 allows O(2^n).
- Cache lines are 64 bytes; sequential access is ~100x cheaper than pointer chasing; this is
  why vector beats list in practice at small and medium sizes.
- Branch misprediction and allocation are the other two hidden constants.
- The three accidental quadratics: erase in a loop, `s = s + c`, `find` in a loop.
- Big-O picks the algorithm; profiling picks the implementation.
