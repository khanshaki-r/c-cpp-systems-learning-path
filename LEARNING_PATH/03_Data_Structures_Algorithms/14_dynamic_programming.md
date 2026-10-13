# 14. Dynamic programming

**Time:** ~5 hours   **Prerequisites:** 01, 07, 09

## 1. In plain words

Dynamic programming (DP) is recursion with a memory. You break a problem into smaller versions
of itself, exactly as in module 07, but you notice that the same smaller versions come up again
and again — so you solve each one once, write the answer down, and look it up next time.

An analogy: someone asks you "what is 1+1+1+1+1+1+1+1?" You count: 8. They add "+1" and ask
again. You do not recount from scratch; you say 9, because you remember the 8. That is all DP
is. The difficult part is not the memory; it is finding the right question to ask about
smaller inputs (the **state**) and how the answer for a big input is built from answers for
smaller ones (the **transition**, or recurrence).

Two names you will hear: **memoisation** (top-down) keeps the recursive function and adds a
cache in front of it. **Tabulation** (bottom-up) throws the recursion away and fills an array
in an order where every answer you need is already there. They compute the same thing. This
module teaches a fixed four-step ladder — recursion, memo, table, then shrink the table — and
applies it to the problems that interviews actually ask: climbing stairs, house robber, coin
change, knapsack, longest common subsequence, edit distance, longest increasing subsequence.

## 2. The real thing

### 2.1 When is it DP? The two properties

1. **Optimal substructure**: the best answer for the whole is built from best answers for parts.
   Shortest path A->C through B = shortest A->B + shortest B->C. (Longest *simple* path does
   *not* have this property, and it is not solvable by DP in general.)
2. **Overlapping subproblems**: the plain recursion solves the same subproblem many times.
   Fibonacci: `fib(5)` calls `fib(3)` twice and `fib(2)` three times. Merge sort has optimal
   substructure but its subproblems never overlap — that is divide and conquer, not DP.

### 2.2 How to recognise a DP problem in 60 seconds

Signals in the problem statement:

- It asks for a **count** ("how many ways"), a **min/max** ("fewest coins", "longest",
  "maximum profit"), or a **yes/no feasibility** ("can you reach", "can it be partitioned").
- The input is a sequence, a string, a pair of strings, a grid, or a budget/capacity.
- Choices are made **step by step** and each choice limits later choices (take or skip this
  item, match or skip this character, go right or go down).
- A greedy rule *almost* works but you can find a counterexample: coins `{1, 3, 4}`, amount 6 —
  greedy takes 4+1+1 (3 coins), the optimum is 3+3 (2 coins).
- Constraints: n up to a few thousand suggests O(n^2) DP; n * capacity up to ~10^7 suggests a
  2D table; n <= 20 suggests a bitmask (2^n) DP or backtracking.

Signals that it is **not** DP: you need to list all solutions (that is backtracking, module 07 —
the output itself is exponential), or a sort plus a single pass already gives the optimum
(greedy, e.g. interval scheduling by end time).

### 2.3 The ladder: recursion -> memo -> table -> rolling

Use this on every DP problem. Write each rung in words before code.

**Rung 1 — define the state in one sentence.** "`f(i)` = number of ways to climb to step `i`."
"`dp[i][c]` = best value using the first `i` items with capacity `c`." If you cannot say it in
one sentence, you do not have a state yet.

**Rung 2 — the recurrence and base cases.** "To reach step `i` the last move was 1 or 2 steps:
`f(i) = f(i-1) + f(i-2)`, `f(0) = f(1) = 1`." Write the plain recursion. It is correct and
exponential; it is also your brute-force reference for testing.

**Rung 3 — memoise.** Add a cache keyed by the state. Time = (number of states) x (work per
state). Climbing stairs: n states x O(1) = O(n).

**Rung 4 — tabulate.** Allocate `dp` over the state space and fill it in an order that respects
dependencies (if `dp[i]` reads `dp[i-1]`, fill `i` ascending). No recursion, no stack limit.

**Rung 5 — shrink.** If `dp[i]` only reads the previous one or two rows, keep only those.
Climbing stairs: two variables, O(1) space. Knapsack: one row. Edit distance: two rows.

```
fib(5) plain recursion                 table
             f5                        i:   0 1 2 3 4 5
          /      \                     dp:  1 1 2 3 5 8
        f4        f3                   each cell = left + left-left
       /  \      /  \
     f3   f2   f2   f1                 15 calls  vs  6 cells
    / \   / \  / \
  f2  f1 f1 f0 f1 f0  ...
```

**Top-down or bottom-up?** Top-down is easier to derive (it is the recursion) and only visits
reachable states — a win when most of the table is never needed. Bottom-up has no recursion
depth problem, no hash or `-1` checks, a predictable memory access pattern, and allows
rolling arrays. In interviews: derive top-down, convert to bottom-up if time allows, and say
which one you would ship and why.

### 2.4 1D DP

**Climbing stairs / Fibonacci**: `f(i) = f(i-1) + f(i-2)`.

**House robber** (max sum, no two adjacent): `dp[i] = max(dp[i-1], dp[i-2] + a[i])` — skip
house `i`, or rob it and the best up to `i-2`. O(n), O(1) space.

**Coin change, fewest coins** (unlimited coins): `dp[x] = 1 + min over coins c <= x of dp[x-c]`,
`dp[0] = 0`, unreachable = infinity. O(amount x coins).

**Coin change, number of ways** (combinations): `dp[0] = 1`; *outer* loop over coins, inner loop
over amounts ascending: `dp[x] += dp[x - c]`. Each combination is counted once, built in coin
order. If you swap the loops (amounts outer, coins inner), you count **permutations**
(1+2 and 2+1 separately) — this exact loop-order question is a favourite follow-up.

**Decode ways** (`"226"` -> 3): `dp[i] = dp[i-1]` if `s[i-1]` is 1-9, plus `dp[i-2]` if
`s[i-2..i)` is 10-26.

**Word break**: `dp[i]` = prefix of length `i` can be segmented = OR over `j < i` of
`dp[j] && s[j..i) in dict`. O(n^2) substring checks, or O(n x maxWordLen) if you only try
plausible word lengths.

**Longest increasing subsequence (LIS)**:
- O(n^2): `dp[i]` = length of the LIS *ending at* `i` = 1 + max `dp[j]` over `j < i` with
  `a[j] < a[i]`. Answer = max over all `i` (not `dp[n-1]` — a common bug).
- O(n log n) "patience sorting": keep `tails[k]` = the smallest tail of any increasing
  subsequence of length k+1. `tails` is sorted, so for each `x`, `lower_bound(tails, x)` finds
  the first tail >= x and replaces it (or appends). The length of `tails` is the answer. Use
  `lower_bound` for strictly increasing, `upper_bound` for non-decreasing. `tails` is *not* an
  actual subsequence; reconstruct with predecessor links (see the code).

```
a = 10 9 2 5 3 7 101 18
x=10  tails [10]
x=9   tails [9]            9 replaces 10: a length-1 sequence can end lower
x=2   tails [2]
x=5   tails [2 5]
x=3   tails [2 3]          3 replaces 5
x=7   tails [2 3 7]
x=101 tails [2 3 7 101]
x=18  tails [2 3 7 18]     length 4
```

### 2.5 2D DP on a grid

**Unique paths**: `dp[r][c] = dp[r-1][c] + dp[r][c-1]`, zero on obstacles. Roll to one row:
`dp[c] += dp[c-1]` sweeping left to right (`dp[c]` still holds the row above; `dp[c-1]` already
holds the current row). **Minimum path sum** is the same with `min` and `+ cost`.

### 2.6 Knapsack

**0/1 knapsack**: n items with weight `w[i]` and value `v[i]`, capacity `C`, each item used at
most once; maximise value.

```
dp[i][c] = best value from the first i items with capacity c
dp[i][c] = max( dp[i-1][c],                        skip item i-1
                dp[i-1][c - w[i-1]] + v[i-1] )     take it (if it fits)
dp[0][*] = 0
```

O(n C) time. It is **pseudo-polynomial**: polynomial in the *value* of C, exponential in the
number of bits of C. Knapsack is NP-hard; this DP only works when C is small enough to index
an array (say up to 10^6-10^7).

**One row**: `dp[c] = max(dp[c], dp[c - w] + v)` with `c` running **downward** from C to w.
Downward means `dp[c - w]` still holds the previous item's row, so the item is used at most
once. Run it **upward** and `dp[c - w]` may already include this item — that is the
**unbounded** knapsack (each item any number of times). Being able to explain this one-line
difference is the knapsack interview question.

**Reconstruction**: keep the full table; walk back from `dp[n][C]`: if `dp[i][c] != dp[i-1][c]`,
item `i-1` was taken, subtract its weight.

**Family**: partition equal subset sum (boolean knapsack to `sum/2`), target sum (+/- signs ->
subset sum to `(sum + target)/2`), coin change (unbounded knapsack with min/count), ones and
zeroes (two capacities).

### 2.7 Two-sequence DP: LCS and edit distance

**Longest common subsequence** of `a` (length n) and `b` (length m):

```
dp[i][j] = LCS length of a[0..i) and b[0..j)
a[i-1] == b[j-1]  ->  dp[i-1][j-1] + 1
otherwise         ->  max(dp[i-1][j], dp[i][j-1])
dp[0][*] = dp[*][0] = 0
```

```
        ""  a  c  e
    ""   0  0  0  0
    a    0  1  1  1
    b    0  1  1  1
    c    0  1  2  2
    d    0  1  2  2
    e    0  1  2  3       LCS("abcde", "ace") = 3
```

O(n m) time and space; O(min(n, m)) space for the length only. Reconstruct by walking back
from `dp[n][m]`: diagonal on a match, otherwise toward the larger neighbour.

**Edit distance (Levenshtein)**: minimum inserts, deletes and replacements turning `a` into `b`.

```
dp[i][j] = edits to turn a[0..i) into b[0..j)
dp[i][0] = i  (delete everything)     dp[0][j] = j  (insert everything)
a[i-1] == b[j-1]  ->  dp[i-1][j-1]
otherwise         ->  1 + min( dp[i-1][j]      delete a[i-1]
                               dp[i][j-1]      insert b[j-1]
                               dp[i-1][j-1] )  replace
```

`edit_distance("horse", "ros") = 3`: replace h->r, delete r, delete e. This is what `diff`,
spell checkers, and PostgreSQL's `fuzzystrmatch` `levenshtein()` compute. Two rows suffice for
the distance; the operations need the full table (or Hirschberg's divide-and-conquer trick,
which recovers the alignment in linear space).

**Relatives**: longest palindromic subsequence = LCS(s, reverse(s)); shortest common
supersequence length = n + m - LCS; "delete operations for two strings" = n + m - 2 LCS;
**longest common substring** (contiguous) uses `dp[i][j] = dp[i-1][j-1] + 1` on match and **0**
otherwise, answer = max cell.

### 2.8 Other DP shapes, by name

- **Interval DP**: `dp[i][j]` over a range, filled by increasing length (burst balloons, matrix
  chain multiplication, palindrome partitioning). O(n^3) typically.
- **State machine DP**: stock buy/sell with cooldown/fee: `hold[i]`, `sold[i]`, `rest[i]`.
- **Bitmask DP**: state is a subset as an integer (travelling salesman over <= ~16 cities,
  assignment). O(2^n x n^2). Module 15 has the code.
- **DP on trees**: post-order, each node returns a small tuple (house robber III: best with and
  without robbing this node).
- **Digit DP**, **probability DP**, **DP + binary search** (LIS), **DP + monotonic deque**
  (sliding window maximum over dp values) — know the names; they are rarer in interviews.

## 3. Code

Files:

- `code/m14_dp_classics.cpp` — climbing stairs at all four rungs (recursion, memo, table,
  O(1)); house robber; coin change fewest coins, number of combinations, number of
  permutations; LIS O(n^2) and O(n log n) with reconstruction; word break; decode ways; unique
  paths with obstacles in one row. Random differential tests against the brute-force recursions.
- `code/m14_knapsack_lcs_edit.cpp` — 0/1 knapsack full table with reconstruction and one-row
  version; unbounded knapsack; partition equal subset sum; LCS with reconstruction; edit
  distance full table with the list of operations and the two-row version; longest
  palindromic subsequence. Differential tests against brute force.

### 3.1 The ladder in code

```cpp
// Rung 2: recursion. O(phi^n).
std::int64_t stairs_rec(int n) {
    if (n <= 1) return 1;
    return stairs_rec(n - 1) + stairs_rec(n - 2);
}

// Rung 3: memo. O(n) time, O(n) memo + O(n) stack.
std::int64_t stairs_memo_go(int n, std::vector<std::int64_t>& memo) {
    if (n <= 1) return 1;
    if (memo[n] != -1) return memo[n];
    return memo[n] = stairs_memo_go(n - 1, memo) + stairs_memo_go(n - 2, memo);
}

// Rung 4: table. O(n) time and space, no recursion.
std::int64_t stairs_table(int n) {
    std::vector<std::int64_t> dp(std::max(n + 1, 2));
    dp[0] = dp[1] = 1;
    for (int i = 2; i <= n; ++i) dp[i] = dp[i - 1] + dp[i - 2];
    return dp[n];
}

// Rung 5: rolling. O(1) space.
std::int64_t stairs_o1(int n) {
    std::int64_t prev2 = 1, prev1 = 1;
    for (int i = 2; i <= n; ++i) { std::int64_t cur = prev1 + prev2; prev2 = prev1; prev1 = cur; }
    return prev1;
}
```

### 3.2 Coin change both ways

```cpp
int coin_change_min(const std::vector<int>& coins, int amount) {
    const int INF = std::numeric_limits<int>::max() / 2;          // INF + 1 must not overflow
    std::vector<int> dp(amount + 1, INF);
    dp[0] = 0;
    for (int x = 1; x <= amount; ++x)
        for (int c : coins)
            if (c <= x && dp[x - c] + 1 < dp[x]) dp[x] = dp[x - c] + 1;
    return dp[amount] >= INF ? -1 : dp[amount];
}

std::int64_t coin_change_ways(const std::vector<int>& coins, int amount) {
    std::vector<std::int64_t> dp(amount + 1, 0);
    dp[0] = 1;
    for (int c : coins)                                            // coins OUTER: combinations
        for (int x = c; x <= amount; ++x) dp[x] += dp[x - c];
    return dp[amount];
}
```

### 3.3 Knapsack, one row

```cpp
int knapsack_01(const std::vector<int>& w, const std::vector<int>& v, int cap) {
    std::vector<int> dp(cap + 1, 0);
    for (std::size_t i = 0; i < w.size(); ++i)
        for (int c = cap; c >= w[i]; --c)                          // DOWNWARD: each item once
            dp[c] = std::max(dp[c], dp[c - w[i]] + v[i]);
    return dp[cap];
}
```

### 3.4 LIS in O(n log n)

```cpp
int lis_length(const std::vector<int>& a) {
    std::vector<int> tails;
    for (int x : a) {
        auto it = std::lower_bound(tails.begin(), tails.end(), x);
        if (it == tails.end()) tails.push_back(x);
        else *it = x;                                              // a lower tail for that length
    }
    return static_cast<int>(tails.size());
}
```

### 3.5 Edit distance, two rows

```cpp
int edit_distance_2rows(std::string a, std::string b) {
    if (a.size() < b.size()) std::swap(a, b);                      // shorter one is the row
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1]
                                          : 1 + std::min({prev[j], cur[j - 1], prev[j - 1]});
        std::swap(prev, cur);                                      // O(1): swaps the buffers
    }
    return prev[b.size()];
}
```

`std::swap` on two vectors swaps three pointers, not the contents — the rolling trick costs
nothing.

## 4. Under the hood (senior-level)

### 4.1 Memo with a hash map vs an array

Memoising with `std::unordered_map<Key, Value>` is the quick way when the state is awkward
(a tuple, a string). Each lookup hashes the key, chases a bucket pointer and usually misses the
cache: on a typical x86 machine tens of nanoseconds versus about one for an array read in a
hot loop. For dense integer states always use a `std::vector`, flattened for 2D
(`dp[i * (m + 1) + j]`). Converting a hash-map memo to an array table often makes the same
algorithm 10-50x faster in practice; the complexity does not change, the constant does.

### 4.2 Table layout and loop order

`std::vector<std::vector<int>>` for an n x m table is n separate allocations; rows are not
adjacent in memory. A single `std::vector<int>(n * m)` is one block, and indexing is a
multiply-add. More important is the loop order: fill row by row so the inner loop walks
consecutive addresses. LCS's inner loop reads `dp[i-1][j-1]`, `dp[i-1][j]` and `dp[i][j-1]` —
two from the previous row, one from the current — all sequential, so the prefetcher keeps up.
Swap the loops (column-major on a row-major array) and each step jumps a whole row: for
m = 10 000 that is a cache miss per cell. Module 01's matrix experiment is exactly this.

### 4.3 Space: when the table does not fit

LCS of two 100 000-character strings needs 10^10 cells — 40 GB as `int`. The length alone
needs two rows (800 KB). The actual alignment in linear space needs **Hirschberg's algorithm**:
compute the middle row's split point with forward and backward two-row passes, then recurse
on the two halves; O(n m) time, O(n + m) space. Diff tools (GNU diff uses Myers' O(N D)
algorithm, where D is the number of differences) exploit that real files are mostly the same.

### 4.4 Recursion depth in top-down DP

Top-down memo on `n = 10^6` recurses a million deep — the same stack overflow as module 13's
DFS. Bottom-up has no such limit. If you must keep top-down (sparse state space), you can
"warm up" the memo bottom-up in chunks, or convert to an explicit stack.

### 4.5 Overflow and modular counting

Counting problems explode fast: the number of ways can exceed 2^63 for modest inputs.
Interviews often say "return the answer modulo 10^9 + 7". Apply the modulus at every addition
(`dp[x] = (dp[x] + dp[x - c]) % MOD`), keep values in `int64_t` so that one addition of two
values below MOD never overflows, and remember that `%` of a negative number is negative in C++.

### 4.6 DP in systems you know

- **Query optimisers**: PostgreSQL's standard join planner is a DP over subsets of relations
  (System R style): the best plan for joining `{A, B, C}` is built from the best plans for its
  subsets. That is bitmask DP, exponential in the number of tables, which is why PostgreSQL
  switches to the genetic optimiser (GEQO) at `geqo_threshold` (12 by default) relations.
- **diff and version control**: edit distance / LCS on lines (Myers' algorithm in git and GNU
  diff).
- **TCP and networking**: not DP, but the Viterbi algorithm in modems and Wi-Fi decoders is DP
  over a trellis (best path through states over time).
- **Fuzzy matching of customer names** in banking KYC and deduplication: Levenshtein (or
  Jaro-Winkler) distance between names, usually with a cut-off so the DP can stop early when
  the minimum in a row exceeds the threshold.

## 5. In production

- **Fuzzy name matching with a threshold.** "Is `RAJESH KUMAR` the same person as
  `RAJESH KUMMAR`?" Full Levenshtein over millions of pairs is O(n m) each. With a threshold k,
  only cells within k of the diagonal can matter (banded DP: O(k n)), and you stop as soon as
  every cell in a row exceeds k. Index first (trigram index, `pg_trgm` in PostgreSQL) to get
  candidates, then run the DP only on those.
- **Fee and limit optimisation.** "Split a payment across instruments to minimise fees under a
  daily limit" is a knapsack variant. Amounts in paise make the capacity huge; scale the
  amounts (e.g. to rupees) when the precision allows, or switch to a different formulation.
  Pseudo-polynomial complexity bites when the capacity is a currency amount.
- **Pitfall: the hidden O(n) in a transition.** `s.substr(j, i - j)` inside a word-break loop
  allocates and copies; the "O(n^2)" DP becomes O(n^3) with allocations. Use `std::string_view`
  for lookups (C++20 heterogeneous lookup in `unordered_set`, or a trie — module 15).
- **Pitfall: `-1` as "not computed" when -1 is a legal answer.** Use `std::optional`, a separate
  `bool computed[]`, or a sentinel outside the answer range.
- **Pitfall: answer is not `dp[n-1]`.** For "ending at i" states (LIS, maximum subarray), the
  answer is the max over all i.

## 6. Try it

1. **(easy)** Min cost climbing stairs: `cost[i]` to step on stair i, you may start at 0 or 1 and
   climb 1 or 2. Write all four rungs. Done when `[10,15,20] -> 15` and
   `[1,100,1,1,1,100,1,1,100,1] -> 6`, and all rungs agree on 1000 random inputs of length <= 20.
2. **(easy)** Maximum subarray (Kadane) restated as DP: `dp[i]` = best sum ending at `i`. Done
   when you can explain why Kadane is a rolled DP and the answer is a max over `i`.
3. **(medium)** Longest palindromic substring with the interval DP `dp[i][j]`, then with
   "expand around centre". Done when `"babad" -> "bab"` or `"aba"`, `"cbbd" -> "bb"`, and you can
   state O(n^2) time for both and O(n^2) vs O(1) space.
4. **(medium)** Target sum: assign + or - to each number to reach `target`; count the ways. Done
   when `[1,1,1,1,1], 3 -> 5` and you can show the reduction to subset sum
   `(sum + target) / 2`, including the parity and negative checks.
5. **(hard)** Edit distance with a threshold k: return the distance if <= k, otherwise -1, in
   O(k * n) time using a band around the diagonal and early exit. Done when it agrees with the
   full DP on 10 000 random pairs of length <= 50 for k = 3.
6. **(hard)** Russian doll envelopes (`[w, h]` pairs, nest strictly in both dimensions): reduce to
   LIS in O(n log n). Done when `[[5,4],[6,4],[6,7],[2,3]] -> 3` and you can explain why
   sorting by width ascending and height **descending** for equal widths is required.

## 7. Interview questions

**Q: How do you recognise that a problem is DP?**
It asks for a count, an optimum or feasibility; choices are made step by step and constrain later
choices; the plain recursion recomputes the same subproblems (overlapping subproblems); and the
best answer is built from best answers to smaller inputs (optimal substructure). A greedy
counterexample is a strong hint.

**Q: Memoisation vs tabulation?**
Memoisation keeps the recursion and caches results: easy to derive, computes only reachable
states, but has call overhead and a recursion-depth limit. Tabulation fills a table in
dependency order: no recursion, predictable memory access, and enables rolling arrays. Same time
complexity.

**Q: Why does the 0/1 knapsack one-row loop go downward?**
`dp[c - w]` must still hold the value from before this item was considered; iterating capacity
downward guarantees that, so each item is used at most once. Iterating upward reads values that
already include the current item, which gives the unbounded knapsack.

**Q: Coin change: why does loop order change "combinations" into "permutations"?**
With coins in the outer loop, each amount is built by adding coins in a fixed coin order, so
each multiset is counted once. With amounts in the outer loop, every coin can be the last one
added at every amount, so different orders are counted separately.

**Q: Give the LCS and edit distance recurrences and complexities.**
LCS: match -> `dp[i-1][j-1] + 1`, else `max(dp[i-1][j], dp[i][j-1])`. Edit distance: match ->
`dp[i-1][j-1]`, else `1 + min(delete, insert, replace)` with base cases i and j. Both O(n m)
time; O(min(n, m)) space for the value with two rows.

**Q: How does LIS in O(n log n) work?**
Maintain `tails[k]`, the smallest possible tail of an increasing subsequence of length k+1; it
stays sorted. For each element, binary-search the first tail >= it and replace it, or append.
The size of `tails` is the LIS length; reconstructing the sequence needs predecessor links.

**Q: Is the knapsack DP polynomial?**
No, pseudo-polynomial: O(n C) is polynomial in the numeric value of C but exponential in its bit
length. Knapsack is NP-hard; the DP is practical only when C is small enough to index an array.

## 8. Recap

- DP = recursion + remembering answers; needs optimal substructure and overlapping subproblems.
- Recognise it: count / min-max / feasible, step-by-step choices, greedy fails.
- Ladder: state in one sentence -> recurrence + base cases -> memo -> table -> roll.
- Time = number of states x work per state.
- Climbing stairs, house robber, decode ways: `dp[i]` from `dp[i-1]`, `dp[i-2]`.
- Coin change: min = `1 + min dp[x-c]`; ways = coins outer (combinations) vs amounts outer
  (permutations).
- 0/1 knapsack: one row, capacity **downward**; upward = unbounded. Pseudo-polynomial.
- LCS and edit distance: `dp[i][j]` on prefixes, O(n m), two rows for the value.
- LIS: O(n^2) "ending at i", or O(n log n) with `tails` + `lower_bound`.
- Array beats hash map for memo; fill in memory order; watch overflow and use `% MOD`.
