# Exercises — 03. Data Structures and Algorithms

Every exercise from modules 01-16 in one place. The full "done when" criteria are in each
module's section 6; this list gives the difficulty and a hint you can read *after* you have
been stuck for 10 minutes. Build everything with
`g++ -std=c++17 -Wall -Wextra -Werror -pthread` on WSL, and add
`-fsanitize=address,undefined` while developing.

Suggested use: do every easy exercise, at least four of the six per module overall, and the
hard ones for modules 04, 05, 11, 12 and 13 — those back up resume lines directly.

---

## 01. Complexity and how to think

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 1.1 | Big-O of five snippets (a)-(e) | easy | (a) the inner 10 is a constant; (b) count how many times you can multiply by 3 before reaching n; (c) sum of n + (n-1) + ... ; (d) inner loop runs log i times; (e) write T(n) = 2T(n/2) + 1 and expand the tree. |
| 1.2 | Run `m01_vector_growth.cpp`, switch +1024 growth to x2, count copies | easy | With doubling, the copies form the series n/2 + n/4 + ... < n. With a fixed increment, each regrowth copies everything so far: quadratic. |
| 1.3 | Prove growth factor 1.5 gives amortised copy cost <= 3 per push | medium | At the last growth you copy at most n elements; the earlier growths copy n/1.5, n/1.5^2, ...; sum the geometric series 1 / (1 - 1/1.5) = 3. |
| 1.4 | Row-major vs column-major at 1024^2 and 8192^2 | medium | A 1024-int row is 4 KB; an 8192-int row is 32 KB. Compare those with your L1d (often 32-48 KB) and L2 from `lscpu`; column-major touches one new cache line per element once rows stop fitting. |
| 1.5 | Count pairs summing to target in a sorted vector, O(n) and O(n^2) | hard | Two pointers from both ends; when the sum is too big move right left, too small move left right. With duplicates, count runs of equal values and multiply (or use n*(n-1)/2 when both pointers hit the same value). |

## 02. Arrays, vectors, two pointers

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 2.1 | Rewrite `array.cpp` with `std::vector<int>` | easy | `std::find` returns an iterator; compare with `end()`. `erase(it)` shifts for you. |
| 2.2 | Squares of a sorted array, O(n) | easy | The largest square is at one of the two ends. Fill the output from the back. |
| 2.3 | Longest subarray with at most two distinct values | medium | Variable window: grow right, update a count map; while the map has more than 2 keys, shrink left and erase keys whose count hits 0. |
| 2.4 | Product of array except self, no division | medium | Pass 1: `out[i]` = product of everything left of i. Pass 2 from the right with a running suffix product multiplied in. |
| 2.5 | Three Sum, unique triplets | medium | Sort; for each i skip `a[i] == a[i-1]`; two pointers on the rest; after a hit, skip equal values on both sides. |
| 2.6 | Maximum sum circular subarray | hard | Either the best subarray does not wrap (Kadane) or it wraps (total minus the minimum subarray). If all are negative, the second formula gives 0 (empty) — return the Kadane answer. |

## 03. Strings

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 3.1 | `is_palindrome(std::string_view)` ignoring non-alphanumerics and case | easy | Two pointers; skip with `std::isalnum(static_cast<unsigned char>(c))`; compare with `std::tolower`. The cast matters for chars above 127. |
| 3.2 | `split` into `string_view`s, skipping empty tokens | easy | Use `find(delim, start)`; `substr` on a `string_view` returns a `string_view` (no allocation). |
| 3.3 | `my_atoi` with clamping | medium | Skip spaces, read sign, then digits; before `r = r*10 + d` check `r > (INT_MAX - d) / 10`. |
| 3.4 | Longest repeating character replacement | medium | Window valid while `len - maxFreq <= k`. `maxFreq` only needs to grow: a stale max can only keep the window the same size, never report a wrong larger answer. |
| 3.5 | Rabin-Karp with two moduli, compare to `std::string::find` | hard | Rolling hash: remove `s[i] * base^(m-1)`, multiply by base, add the new char; keep everything in `uint64_t` mod a prime < 2^31 to avoid overflow. Verify each hit with `memcmp`. |
| 3.6 | KMP failure table and search | hard | `fail[i]` = length of the longest proper prefix of `p[0..i]` that is also a suffix. For `"abcabd"`: 0 0 0 1 2 0. On mismatch, fall back to `fail[j-1]`, never move the text pointer back. |

## 04. Hashing and hash tables

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 4.1 | Word frequencies, top 10, 10 MB file | easy | `++counts[word]` is fine for counting; use `find` for read-only lookups. Top 10 with `std::partial_sort` or a heap of size 10 (module 12). |
| 4.2 | `std::hash<Point>` with `hash_combine`, 1M points | easy | `seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2)`. Specialise `std::hash` in namespace `std` and provide `operator==`. |
| 4.3 | Add `operator[]`, destructor, move ctor, `size()` to the chained map | medium | `operator[]` = find or insert a default value and return a reference. The move constructor steals the bucket vector and leaves the source empty but valid. |
| 4.4 | Subarray sums divisible by k | medium | Count prefix sums by `((sum % k) + k) % k`; each earlier prefix with the same remainder forms a valid subarray. Start with `count[0] = 1`. |
| 4.5 | Backward-shift deletion in linear probing | hard | After emptying slot i, scan forward; move an element at j back into i if its home slot is not cyclically in (i, j]. Stop at the first empty slot. |
| 4.6 | Benchmark lookups: `unordered_map`, open addressing, sorted vector | hard | Use random keys and random query order; prevent the optimiser from deleting work by summing results. Explain crossovers with cache lines touched per lookup vs data size vs L2/L3 size. |

## 05. Linked lists

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 5.1 | Reverse a list, raw pointers and `unique_ptr` | easy | Three variables: `prev`, `cur`, `next`. With `unique_ptr`, move `cur->next` out first, then move `prev` into `cur->next`. |
| 5.2 | Remove all nodes with a value, one pass, dummy head | easy | `Node dummy{0, head}; Node* p = &dummy; while (p->next) if (p->next->val == v) unlink else p = p->next;` — do not advance after unlinking. |
| 5.3 | Cycle start index | medium | Floyd: after slow and fast meet, restart one pointer at head; move both one step; they meet at the cycle start (a = kL - b). |
| 5.4 | Merge k sorted lists pairwise and with a heap | medium | Pairwise in rounds (1+2, 3+4, ...) is O(n log k) too; merging one-by-one into an accumulator is O(nk). The heap holds (value, list index). |
| 5.5 | Hand-made LRU with your own doubly linked nodes | hard | Sentinel head and tail nodes remove all null checks. `get` = unlink + push front; `put` on a full cache evicts `tail->prev` and erases it from the map. |
| 5.6 | Sharded thread-safe LRU | hard | `std::array<Shard, N>`, each with its own `std::mutex`, list and map; pick the shard by `hash(key) % N`; never hold two shard locks at once. |

## 06. Stacks, queues, deques

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 6.1 | `MinStack` | easy | Push `(value, min so far)` pairs, or keep a second stack of minimums. |
| 6.2 | Queue with two stacks, prove O(n) total | easy | Pop from `out`; when `out` is empty, move everything from `in`. Each element is moved at most once: amortised O(1). |
| 6.3 | `RingBuffer::peek(i)` and `read_contiguous()` | medium | Index `(head + i) & mask` (or `% cap`). Two spans: `[head, min(head + size, cap))` and the wrapped part from 0. |
| 6.4 | Trapping rain water, stack then two pointers | medium | Stack of indices with decreasing heights; on a taller bar pop the bottom and add `(min(left, right) - bottom) * width`. Two pointers: move the side with the smaller max. |
| 6.5 | Maximal rectangle of 1s | hard | Build a histogram per row (`h[c] = row[c] ? h[c] + 1 : 0`), then run largest-rectangle-in-histogram on each. |
| 6.6 | Lock-free SPSC ring buffer | hard | Producer writes the slot then stores `tail` with release; consumer loads `tail` with acquire before reading. Put head and tail on separate 64-byte cache lines. |

## 07. Recursion and backtracking

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 7.1 | Recursive sum, max, reverse by index | easy | `sum(i) = v[i] + sum(i + 1)`; reverse swaps `v[i]` and `v[n-1-i]` and recurses on `i + 1` until the middle. Depth n or n/2. |
| 7.2 | All binary strings of length n | easy | Choose '0' first, then '1', recursing on the next position; the order is lexicographic because '0' < '1' at every level. |
| 7.3 | Combinations C(n, k) with pruning | medium | Loop `i` from `start` to `n - (k - cur.size())` — the upper bound is the prune. |
| 7.4 | Valid parentheses for n pairs | medium | Add `(` if `open < n`; add `)` if `close < open`. Base case when the length is 2n. |
| 7.5 | Sudoku with fewest-candidates-first | hard | Keep row/column/box bitmasks; candidates = `~(row \| col \| box) & 0x1FF`; pick the empty cell with the smallest popcount (module 15). |
| 7.6 | Iterative subsets with an explicit stack, lazily | hard | A subset of n items is an n-bit decision path; generate lazily with an iterator-like object holding the stack of `(index, included?)` frames rather than storing results. |

## 08. Sorting

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 8.1 | Insertion sort, count comparisons and shifts | easy | On sorted input the inner loop stops after one comparison: n - 1 comparisons, 0 shifts. |
| 8.2 | Merge sort with one scratch buffer | easy | Allocate the buffer once in a wrapper and pass it down; merge into the buffer, copy back (or alternate roles per level). Use `<=` when taking from the left to stay stable. |
| 8.3 | Make quicksort quadratic, then fix it | medium | First-element pivot on sorted input splits n into 0 and n - 1. Recurse on the smaller side and loop on the larger to keep stack depth O(log n). |
| 8.4 | Three-way partition quicksort | medium | Dutch national flag: `lt`, `i`, `gt`; elements equal to the pivot stay in the middle and are never recursed on. |
| 8.5 | Quickselect median of 10M ints | hard | Recurse (or loop) only into the side containing k; random pivot; expected work n + n/2 + n/4 + ... = 2n. |
| 8.6 | Two-pass external merge with fan-in limit | hard | Merge runs in groups of `fan_in` into longer runs, then merge those. Every pass reads and writes every block once. |

## 09. Binary search and ordered data

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 9.1 | Hand-written `lower_bound` / `upper_bound` | easy | Half-open `[lo, hi)`; `lower_bound` moves `lo = mid + 1` when `a[mid] < t`, `upper_bound` when `a[mid] <= t`. Return `lo`. |
| 9.2 | Count occurrences in O(log n) | easy | `upper_bound - lower_bound`. |
| 9.3 | Integer square root up to 10^18 | medium | Search on answer in `[0, min(x, 2^32)]`; test `mid <= x / mid` to avoid overflow. |
| 9.4 | Koko eating bananas | medium | Search the speed in `[1, max pile]`; hours at speed s = sum of `(p + s - 1) / s`; find the first s with hours <= h. |
| 9.5 | Minimum in rotated sorted array with duplicates | hard | Compare `a[mid]` with `a[hi]`: greater -> go right, smaller -> go left (keep mid), equal -> `--hi` (this is the O(n) worst case). |
| 9.6 | B-tree node child index and leaf cursor start | hard | Child to descend = `upper_bound(keys, t)` index (keys equal to a separator live in the right child under the stated convention); leaf start for `>= t` = `lower_bound`. |

## 10. Trees and BST

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 10.1 | Pre-, in-, post-order on a sample tree | easy | Only the position of "visit" changes relative to the two recursive calls. |
| 10.2 | Max depth, node count, leaf count, sum | easy | Each is `combine(f(left), f(right))` with a base case for null that returns 0. |
| 10.3 | BST delete, all three cases, validate | medium | Two children: copy the in-order successor's key (leftmost of the right subtree) into the node, then delete the successor from the right subtree. |
| 10.4 | Serialise / deserialise in pre-order with `#` | medium | Deserialise with a shared index into the token list: read a token, `#` -> null, else make the node and recurse left then right. |
| 10.5 | k-th smallest with subtree sizes | hard | `size(left) == k - 1` -> this node; smaller -> go right with `k - size(left) - 1`; larger -> go left. Update sizes on the way down only if the insert/delete will succeed. |
| 10.6 | Sorted vs shuffled insert, BST vs `std::map` | hard | Sorted inserts make your BST a list: height n, so each search is O(n). Recursive insert will overflow the stack at 100 000 — use an iterative insert. |

## 11. Balanced trees, B-trees and disk

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 11.1 | Draw a B+tree (max 3 keys) after inserting 1..10 and 10..1 | easy | Leaf split: copy the first key of the new right leaf up; internal split: move the middle key up. Sequential inserts leave half-full leaves behind. |
| 11.2 | Fan-out, height and size for a UUID index | easy | Index tuple = 8-byte header + 16-byte key, plus a 4-byte line pointer, rounded to 8-byte alignment; divide usable page space by that. Height = ceil(log_fanout(leaf pages)) + 1. |
| 11.3 | AVL height and rotation count for sorted inserts | medium | Compare height with 1.44 log2(n + 2). Sorted inserts always unbalance the right spine in the same way; count single rotations. |
| 11.4 | `prev()` on the B+tree cursor | medium | Maintain `prev` in `split_leaf` (new right leaf's prev = old leaf; old right neighbour's prev = new leaf) and in merges. |
| 11.5 | Bulk loading `from_sorted` | hard | Pack leaves left to right to the fill factor, chain them, then build each internal level from the first key of each child until one node remains. |
| 11.6 | Simulated B-link search | hard | Split: create the right node with the high key, link it, then update the parent. A search that finds `key > high_key` follows the right link instead of failing. |

## 12. Heaps and priority queues

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 12.1 | Max-heap push/pop/top vs `std::priority_queue`; build vs n pushes | easy | Build = sift down from `n/2 - 1` to 0. Compare at every pop with the reference. |
| 12.2 | k-th largest with a heap and with `nth_element` | easy | Min-heap of size k; answer = top. `nth_element(v.begin(), v.begin() + k - 1, v.end(), std::greater<>())`. |
| 12.3 | Median of a data stream | medium | Max-heap low half, min-heap high half; push into low, move low's top to high, rebalance so low has the extra element. |
| 12.4 | k closest points to the origin | medium | Max-heap of size k by squared distance (no `sqrt`); pop when it exceeds k, so the farthest of the current best k leaves. |
| 12.5 | Indexed heap with `decrease_key` and `erase` | hard | Maintain `pos[id]` on every swap — write one `swap_nodes(i, j)` helper that updates both. `erase(i)`: swap with last, pop, then sift the moved element up *or* down. |
| 12.6 | Timer queue: lazy deletion vs indexed heap | hard | Lazy: store `(deadline, conn, version)`; skip pops whose version is not the connection's current version. Track the max heap size to see the lazy version's growth. |

## 13. Graphs: BFS, DFS and friends

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 13.1 | Adjacency list and CSR, BFS on both | easy | CSR: count degrees into `offsets[u+1]`, prefix-sum them, then fill `targets` using a copy of `offsets` as write cursors. |
| 13.2 | Flood fill, recursive and iterative; crash the recursive one | easy | If the new colour equals the old, return immediately (otherwise infinite loop). Iterative: `std::vector<std::pair<int,int>>` as a stack, recolour on push. |
| 13.3 | Minimum number of semesters (parallel course schedule) | medium | Kahn's algorithm processed layer by layer: each layer is one semester. If not every course is output, return -1. |
| 13.4 | Cheapest flights within k stops | medium | Bellman-Ford for k + 1 rounds; each round relaxes from a *copy* of the previous round's array so a path cannot use more than one extra edge per round. |
| 13.5 | Strongly connected components for deadlock cycles | hard | Kosaraju: DFS finish order on the graph, then DFS on the reversed graph in reverse finish order; each tree is an SCC. Tarjan: one DFS with `index`, `lowlink` and an on-stack flag. |
| 13.6 | Bidirectional word ladder | hard | Keep two frontier sets; always expand the smaller one; stop when a generated word is in the other frontier. |

## 14. Dynamic programming

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 14.1 | Min cost climbing stairs, all four rungs | easy | `dp[i] = cost[i] + min(dp[i-1], dp[i-2])`; answer `min(dp[n-1], dp[n-2])`. Or define `dp[i]` = cost to *reach* step i (top is step n). |
| 14.2 | Kadane as DP | easy | `dp[i] = max(a[i], dp[i-1] + a[i])` — either start fresh at i or extend. Rolling it into one variable gives Kadane. |
| 14.3 | Longest palindromic substring, DP and expand-around-centre | medium | `dp[i][j] = s[i] == s[j] && (j - i < 2 \|\| dp[i+1][j-1])`, filled by increasing length. Centres: 2n - 1 (each char and each gap). |
| 14.4 | Target sum (count +/- assignments) | medium | P - N = target and P + N = sum, so P = (sum + target) / 2. If that is odd or negative, the answer is 0. Count subsets summing to P with a downward 1D loop. |
| 14.5 | Edit distance with threshold k in O(k n) | hard | Only cells with `abs(i - j) <= k` can hold a value <= k; compute that band per row and stop when the row minimum exceeds k. If `abs(n - m) > k`, return -1 immediately. |
| 14.6 | Russian doll envelopes via LIS | hard | Sort by width ascending, and by height descending for equal widths, so two envelopes with the same width cannot both be in the increasing sequence; then LIS on heights with `lower_bound`. |

## 15. Tries and bit manipulation

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 15.1 | `is_pow2`, Kernighan popcount, lowest set bit, clear lowest | easy | Subtracting 1 flips the lowest 1 and every 0 below it; AND with the original keeps everything above. |
| 15.2 | 26-way trie from memory | easy | Node = `std::array<std::unique_ptr<Node>, 26>` + `bool end`. `search` and `starts_with` share one `walk` helper and differ only in checking `end`. |
| 15.3 | Search suggestions, trie and sorted vector | medium | Sorted vector: for each prefix, `lower_bound(prefix)` and take up to three entries that still start with the prefix. The start position only moves right as the prefix grows. |
| 15.4 | Single number III | medium | XOR everything to get `a ^ b`; its lowest set bit differs between a and b; partition the numbers by that bit and XOR each group. |
| 15.5 | Word search II with trie pruning | hard | DFS from each cell carrying the trie node; when you find a word, record it and clear its end flag; on the way back, delete child nodes that became empty so later searches skip them. |
| 15.6 | Subset sum with `std::bitset` shifts | hard | `dp \|= dp << x` adds x to every reachable sum at once, 64 sums per machine word. Answer: `dp[target]`. |

## 16. Interview coding playbook

| # | Exercise | Level | Hint |
|---|----------|-------|------|
| 16.1 | Run the practice template; break it on purpose | easy | `seen[a[i]] = i` overwrites the first index of a duplicate; the `[3,3]` test still passes because the complement is found before the overwrite — think about which input would expose it, or decide it is harmless and say why. |
| 16.2 | Pattern cheat sheet from memory | easy | Group the rows by data structure (array, hash, list, stack, tree, heap, graph, DP, trie/bits) and rebuild each group. |
| 16.3 | Three recorded out-loud blocks | medium | Listen for silences longer than 20 seconds and for syntax narration; replace both with "the invariant is..." or "I am choosing X because...". |
| 16.4 | Systems follow-ups for one problem | medium | Use section 4.2's list: memory, stream, threads, cache, scale, testing. Each answer: technique + module reference. |
| 16.5 | Mock with a real interviewer and the rubric | hard | Give the interviewer the rubric from 4.1 before you start; ask for one score per dimension, not an overall impression. |
| 16.6 | Ten-week practice log | hard | Keep it in one text file you open every day; the re-solve dates (+3, +10) are written when you log the miss, so the schedule is never in your head. |
