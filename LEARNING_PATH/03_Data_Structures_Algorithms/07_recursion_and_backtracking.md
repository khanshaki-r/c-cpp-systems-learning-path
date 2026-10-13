# 07. Recursion and backtracking

**Time:** ~3 hours   **Prerequisites:** 01, 06

## 1. In plain words

Recursion is a function that solves a problem by calling itself on a smaller version of the
same problem, and stopping when the problem is small enough to answer directly. You know the
mechanics from C: each call gets its own frame on the stack with its own locals; `return`
pops it. What trips people up is not the mechanics but the *trust*: you have to write the
function assuming the recursive call already works, and only make sure of two things — the
base case is right, and every call moves toward it.

Backtracking is recursion used as a search: build a candidate solution one choice at a time,
and when a choice cannot lead to a valid solution, undo it ("back track") and try the next
one. Think of walking a maze with a piece of chalk: at every fork you mark a path, walk it,
and if it dead-ends you walk back to the fork and take the next one. Permutations, subsets,
N-queens, Sudoku and "all combinations that sum to k" are all backtracking.

Memoisation is the trick that turns exponential recursion into polynomial time when subproblems
repeat: write the answer down the first time, look it up afterwards. It is the bridge to
dynamic programming (module 14).

## 2. The real thing

### 2.1 Anatomy of a recursive function

```cpp
R solve(State s) {
    if (base_case(s)) return direct_answer(s);        // 1. stop
    R sub = solve(smaller(s));                        // 2. trust the smaller call
    return combine(s, sub);                           // 3. build the answer from it
}
```

Rules that prevent 90% of bugs:
- Write the base case first. Test it alone.
- Every recursive call must strictly reduce something (size, index, remaining budget).
- Decide what the function *returns* versus what it *accumulates* in a parameter passed by
  reference. Mixing the two in one function is the usual source of confusion.
- Think about the depth: it is the space complexity and, past ~100k frames, a crash.

### 2.2 Call stack mechanics

On x86-64 Linux, a call pushes the return address (8 bytes), the callee saves `rbp` and any
callee-saved registers it uses, then subtracts space for locals. A frame for `int f(int n)`
with one local is around 32 to 48 bytes; with a `std::string` local or an array it is far more.
The default main-thread stack is 8 MB (`ulimit -s`), secondary threads get 8 MB by default on
glibc (2 MB on some systems, 1 MB on Windows). So a plain recursive function overflows at
roughly 100 000 to 200 000 frames, less if frames are big. Tail-call optimisation (compiler
turns `return f(n-1)` into a jump) exists at `-O2` but is not guaranteed in C++ — do not
depend on it. When depth can be large, convert to an explicit stack (section 2.6).

### 2.3 Classic recursions and their shapes

| Problem | Recurrence | Time | Depth |
|---|---|---|---|
| factorial, sum of list | T(n) = T(n-1) + O(1) | O(n) | n |
| binary search | T(n) = T(n/2) + O(1) | O(log n) | log n |
| merge sort | T(n) = 2T(n/2) + O(n) | O(n log n) | log n |
| naive Fibonacci | T(n) = T(n-1) + T(n-2) + O(1) | O(phi^n) ~ O(1.618^n) | n |
| subsets of n items | 2^n leaves, O(n) each to copy | O(n 2^n) | n |
| permutations of n | n! leaves | O(n n!) | n |
| tower of Hanoi | T(n) = 2T(n-1) + 1 | O(2^n) | n |

The Fibonacci row is why memoisation exists: the recursion tree has 2^n nodes but only n
distinct arguments.

### 2.4 Backtracking template

```cpp
void backtrack(State& state, Path& path, Results& out) {
    if (is_solution(state)) { out.push_back(path); return; }
    for (Choice c : candidates(state)) {
        if (!valid(c, state)) continue;      // prune: do not even recurse
        apply(c, state, path);               // choose
        backtrack(state, path, out);         // explore
        undo(c, state, path);                // un-choose: restore EXACTLY the previous state
    }
}
```

Three things make or break it:
1. **Choose / explore / un-choose** must be symmetric. If `apply` pushes and sets a flag, `undo`
   pops and clears the flag. The state after `undo` must be bit-for-bit what it was before
   `apply`.
2. **Pruning** (`valid`) is where the speed is. N-queens without pruning tries n^n placements;
   with column/diagonal checks it visits a tiny fraction.
3. **Avoid duplicates by construction**, not by filtering at the end: for subsets, only consider
   candidates with index > the last chosen index; for combinations with repeated values, sort
   first and skip `v[i] == v[i-1]` when `i > start`.

Complexity is "number of nodes in the search tree times work per node". Say it that way.

### 2.5 The three canonical problems

**Subsets (power set).** At each index decide include/exclude. 2^n leaves.

```cpp
void subsets(const std::vector<int>& v, std::size_t i, std::vector<int>& cur,
             std::vector<std::vector<int>>& out) {
    if (i == v.size()) { out.push_back(cur); return; }
    subsets(v, i + 1, cur, out);                 // exclude v[i]
    cur.push_back(v[i]);                         // include v[i]
    subsets(v, i + 1, cur, out);
    cur.pop_back();                              // undo
}
```

Alternative "for-loop" form, which is what you use for combinations and k-subsets:

```cpp
void subsets2(const std::vector<int>& v, std::size_t start, std::vector<int>& cur, Out& out) {
    out.push_back(cur);                          // every prefix is a subset
    for (std::size_t i = start; i < v.size(); ++i) {
        cur.push_back(v[i]);
        subsets2(v, i + 1, cur, out);            // i + 1: only later elements, no duplicates
        cur.pop_back();
    }
}
```

**Permutations.** Choose which unused element goes in position `pos`. Swap-based in-place
version: swap `v[pos]` with each `v[i]`, `i >= pos`, recurse on `pos + 1`, swap back. n!
leaves, O(n) to copy each: O(n * n!).

```cpp
void permute(std::vector<int>& v, std::size_t pos, Out& out) {
    if (pos == v.size()) { out.push_back(v); return; }
    for (std::size_t i = pos; i < v.size(); ++i) {
        std::swap(v[pos], v[i]);                 // choose v[i] for position pos
        permute(v, pos + 1, out);
        std::swap(v[pos], v[i]);                 // un-choose
    }
}
```

**N-queens.** Place one queen per row. Track used columns and the two diagonal families:
`row - col` (constant on "\" diagonals) and `row + col` (constant on "/" diagonals). A
placement is valid if none of the three is taken. Three boolean arrays make the check O(1).

```cpp
void queens(int row, int n, std::vector<int>& col_of, std::vector<char>& cols,
            std::vector<char>& d1, std::vector<char>& d2, int& count) {
    if (row == n) { ++count; return; }
    for (int c = 0; c < n; ++c) {
        int a = row - c + n - 1, b = row + c;              // shift a to be non-negative
        if (cols[c] || d1[a] || d2[b]) continue;           // prune
        cols[c] = d1[a] = d2[b] = 1; col_of[row] = c;
        queens(row + 1, n, col_of, cols, d1, d2, count);
        cols[c] = d1[a] = d2[b] = 0;                       // undo
    }
}
```

n = 8 gives 92 solutions; n = 12 gives 14 200. Without the diagonal arrays you would scan all
previous rows for each placement — O(n) per check instead of O(1).

### 2.6 Recursion to explicit stack

Any recursion can be made iterative by pushing what the frame holds onto a `std::vector`.
The mechanical translation: a struct with the parameters and a "where was I" marker; a loop
that pops, does the work, pushes continuations. In practice you rarely need the general
form; you need the two common cases:

- **Tree/graph DFS**: push the root; loop: pop node, process, push children. (Module 10, 13.)
- **Tail recursion** (`return f(smaller)`): becomes a `while` loop directly.

Do this when depth may exceed ~10^4 to 10^5, or in production code where a stack overflow is
an unacceptable failure mode.

### 2.7 Memoisation

Cache results keyed by the arguments. For integer arguments a `std::vector<long long>` with a
sentinel; for composite arguments a `std::unordered_map` or a 2D vector.

```cpp
long long fib(int n, std::vector<long long>& memo) {
    if (n < 2) return n;
    if (memo[n] != -1) return memo[n];
    return memo[n] = fib(n - 1, memo) + fib(n - 2, memo);
}
```

O(n) instead of O(1.6^n). When to memoise: the recursion has **overlapping subproblems**
(the same arguments recur) and **optimal substructure** (the answer for the whole is built
from answers for parts). When the subproblems are all distinct (permutations, subsets),
memoisation buys nothing — the search tree really is that big. Module 14 turns memoised
recursion into bottom-up tables.

## 3. Code

Files: `code/m07_backtracking.cpp` (subsets both forms, permutations with and without
duplicates, combination sum, N-queens count and boards, letter combinations of a phone
number, word search in a grid), `code/m07_memo_fib.cpp` (naive vs memoised vs iterative
Fibonacci with timing and call counts, climbing stairs, grid paths with memo, and the
recursion-to-explicit-stack conversion of a tree DFS).

Highlights from `m07_backtracking.cpp`:

```cpp
// Combination sum: unlimited reuse of candidates; pass `start` = i (not i+1) to allow reuse,
// and sort so that we can break (not continue) when a candidate exceeds the remaining target.
void combination_sum(const std::vector<int>& c, int start, int remaining,
                     std::vector<int>& cur, std::vector<std::vector<int>>& out) {
    if (remaining == 0) { out.push_back(cur); return; }
    for (int i = start; i < static_cast<int>(c.size()); ++i) {
        if (c[i] > remaining) break;              // sorted: everything after is bigger too
        cur.push_back(c[i]);
        combination_sum(c, i, remaining - c[i], cur, out);
        cur.pop_back();
    }
}
```

```cpp
// Permutations with duplicate values: sort, and at each position only use the FIRST unused
// copy of a value. The `used[i-1]` test is the standard duplicate guard.
if (used[i] || (i > 0 && v[i] == v[i-1] && !used[i-1])) continue;
```

Highlights from `m07_memo_fib.cpp`:

```cpp
// Naive: counts calls so you can see the explosion. fib(30) makes 2.7 million calls.
long long fib_naive(int n, long long& calls) {
    ++calls;
    return n < 2 ? n : fib_naive(n - 1, calls) + fib_naive(n - 2, calls);
}
// Memoised: 2n-1 calls.
// Iterative: two variables, O(1) space, the bottom-up version from module 14.
```

## 4. Under the hood (senior-level)

### 4.1 What a frame costs

`g++ -O2` inlines small leaf functions and converts simple tail recursion to loops, so
`fib_iterative` and a tail-recursive sum compile to the same code. But any function that
makes two recursive calls (Fibonacci, merge sort, tree traversal) cannot be a loop without an
explicit stack, and the compiler will not invent one. For those, the cost per call is: `call`
(push return address, ~1 cycle but a branch), prologue (save registers, adjust `rsp`), the
work, epilogue, `ret` (a predicted indirect branch; return-address prediction is good unless
recursion is deep). Roughly 5 to 15 cycles overhead per call. A backtracking search that makes
10^8 calls spends ~1 second in call overhead alone. Iterative versions of hot inner recursions
pay off; for depth-20 searches it does not matter.

### 4.2 Stack overflow in a crash dump

The symptom is `SIGSEGV` with a faulting address just below the stack's guard page, and a
backtrace with thousands of identical frames. `gdb`: `bt -10` shows the bottom. In WinDbg
`!analyze -v` reports `STACK_OVERFLOW` and `kb` shows the repetition. Causes in production
code: unbounded recursion on malicious input (a JSON parser given 100 000 nested `[`), a
recursive destructor on a long list, mutual recursion between two callbacks. Defence: depth
limits on recursive parsers, iterative destructors, explicit stacks for DFS.

### 4.3 Why memoisation changes the complexity class

The recursion tree for `fib(n)` has ~1.6^n nodes but only n+1 distinct labels. Memoisation
makes each distinct label compute once, so the total work is (number of distinct states) x
(work per state excluding recursive calls). This "states x transitions" formula is how you
compute DP complexity in module 14, and it is the same number for memoised top-down and
tabulated bottom-up.

### 4.4 Backtracking and pruning in real engines

A SQL query planner choosing a join order for 10 tables faces 10! orderings; PostgreSQL's
standard planner does dynamic programming over subsets (2^10 states) with pruning, and
switches to a genetic algorithm (GEQO) above 12 tables. SAT solvers are backtracking with
clever pruning (unit propagation, clause learning). Regular expression engines that
backtrack (PCRE, Java's) can hit exponential time on pathological patterns — the reason
RE2 and Rust's regex use automata instead. If you ever see a regex take seconds, that is
backtracking without memoisation.

## 5. In production

- **Recursive descent parsers** for message formats and config files: each grammar rule is a
  function that calls the functions for its sub-rules. Clean, but put a depth counter in and
  fail at, say, 1000 to defend against nested-bracket attacks.
- **Directory walking, tree serialisation, dependency resolution** are recursions over trees
  and DAGs. For DAGs, memoise ("visited") or you re-walk shared subtrees exponentially.
- **Retry with backtracking** in distributed transactions: try a plan, on conflict undo and
  try the next. Same choose/explore/undo shape; the undo is the hard part to get right there
  too.
- **Debugging a backtracking bug.** The state after `undo` is not identical to the state before
  `apply`: a counter not decremented, a flag not cleared, a vector not popped because an early
  `return` skipped the cleanup line. Put the cleanup in an RAII guard (a small struct whose
  destructor undoes) or structure the code so `undo` cannot be skipped. In an interview, walk
  through one `apply`/`undo` pair out loud.
- **Memoisation as caching** in a server: a function `price(account, date)` called a million
  times a day with 10 000 distinct arguments is a memoisation opportunity, with the extra
  questions production adds: invalidation, bounded size (LRU, module 05) and thread-safety.

## 6. Try it

1. **(easy)** Write recursive `sum`, `max`, and `reverse` on a `std::vector<int>` by index. Then
   write the loop versions. Done when you can state the recurrence and depth for each.
2. **(easy)** Generate all binary strings of length n. Done when n = 3 prints 8 strings in
   lexicographic order and you can say why the order comes out that way.
3. **(medium)** Combinations of k out of n (`C(n,k)` results) using the for-loop subset
   template with `start`. Add the prune "if remaining slots > remaining candidates, return".
   Done when `C(20,10)` runs instantly and the count is 184 756.
4. **(medium)** Generate all valid parentheses strings for n pairs. Hint: track `open` and
   `close` counts; you may add `(` if `open < n`, `)` if `close < open`. Done when n = 3 gives 5
   strings.
5. **(hard)** Sudoku solver with backtracking and the "fewest candidates first" heuristic for
   cell order. Done when it solves a hard puzzle from any newspaper in under 100 ms and you
   can report how many placements it tried with and without the heuristic.
6. **(hard)** Convert the recursive `subsets` function to an iterative one with an explicit
   stack of `(index, cur_size)` frames. Done when both produce identical output for n = 15 and
   the iterative version handles n = 10 000 with a single subset request without overflowing
   (generate lazily — do not store 2^10000 results).

## 7. Interview questions

**Q: How do you approach writing a recursive function?**
Define what it returns for a given input, write the base case, make the recursive call on a
strictly smaller input, trust its result and combine. Check the depth for stack safety.

**Q: Permutations vs subsets — what is the structure of each search tree?**
Subsets: depth n, two branches per level (include/exclude), 2^n leaves. Permutations: depth n,
n-i branches at level i, n! leaves. Both are O(n) work per leaf to copy out the answer, so
O(n 2^n) and O(n n!).

**Q: How does N-queens prune?**
One queen per row by construction; before placing in column c, O(1) checks on three boolean
arrays: column c, diagonal row-c, anti-diagonal row+c. Any conflict skips the recursive call.
This cuts the search from n^n to a tiny fraction.

**Q: How do you avoid duplicate results in combination problems with repeated values?**
Sort the input and, within one level of the loop, skip a value equal to the previous value
(`i > start && v[i] == v[i-1]`). For permutations use `!used[i-1]` so only the first unused
copy is chosen at each position.

**Q: When does memoisation help and when does it not?**
It helps when the same subproblem arguments recur (Fibonacci, grid paths, edit distance):
exponential becomes polynomial. It does not help when every call has distinct arguments
(enumerating all permutations): the output itself is exponential.

**Q: What is the space complexity of recursive backtracking?**
O(depth) frames plus the current path; for subsets and permutations that is O(n), not
counting the output.

**Q: Your recursive function crashes with SIGSEGV on large input. Why and what do you do?**
Stack overflow: each frame uses stack space and the default stack is 8 MB. Convert to an
iterative version with an explicit heap-allocated stack, or bound the depth, or (rarely)
enlarge the thread stack when creating the thread.

## 8. Recap

- Recursion: base case first, strictly smaller call, trust and combine; depth = space.
- Default stack is 8 MB; ~10^5 frames overflow it; convert deep recursion to an explicit stack.
- Backtracking = choose, explore, un-choose; undo must restore the exact prior state.
- Pruning is where speed comes from; duplicates are avoided by construction (start index,
  sorted skip, `used[i-1]`).
- Subsets: include/exclude or for-loop with `start`; 2^n leaves.
- Permutations: swap in place; n! leaves. Combination sum: pass `i` to reuse, `i+1` otherwise.
- N-queens: one per row; column and two diagonal arrays give O(1) validity checks.
- Memoise when subproblems overlap; complexity becomes (distinct states) x (work per state).
- Naive Fibonacci is O(1.6^n); memoised is O(n); iterative is O(n) time, O(1) space.
- Depth-limit recursive parsers; backtracking regex engines are exponential on bad input.
