# Interview questions — 03. Data Structures and Algorithms

40 questions with model answers, ordered from basic to senior. Say each answer out loud in
under a minute. The module in brackets has the full explanation.

---

## Basics: complexity and arrays

**1. What does O(n) actually mean?** [01]
An upper bound on how the cost grows with input size, ignoring constant factors and lower-order
terms: there is a constant c such that the cost is at most c * n for large n. It says nothing
about the constant, so an O(n) algorithm with bad memory access can lose to an O(n log n) one at
realistic sizes. Big-Theta is the tight bound; in interviews "O" is used loosely to mean the tight one.

**2. What is amortised complexity? Use `std::vector::push_back` as the example.** [01, 02]
Amortised cost is the average cost per operation over a worst-case *sequence* of operations, not
a probability. `push_back` is usually O(1) but occasionally reallocates and copies all n
elements. With geometric growth (factor 2 in libstdc++, 1.5 in MSVC), the total copies over n
pushes are n/2 + n/4 + ... < n, so the total is O(n) and each push is amortised O(1). A fixed
increment (+1000) would make the total O(n^2).

**3. Why do 1.5x and 2x growth factors both work, and why might 1.5x be preferred?** [01]
Any factor > 1 gives a geometric series, hence amortised O(1); a bigger factor means fewer copies
but more wasted capacity. With factor < ~1.618 (the golden ratio), the sum of previously freed
blocks can eventually be large enough to hold the next allocation, so an allocator can reuse that
memory; with 2x it never can. That is the usual argument for 1.5x.

**4. When does `std::vector` invalidate iterators, pointers and references?** [02]
Any reallocation (a `push_back`/`insert`/`reserve` that exceeds capacity) invalidates all of
them. Without reallocation, `insert` and `erase` invalidate everything at or after the point of
change. `reserve` up front and indices instead of pointers avoid the bug.

**5. Two pointers vs sliding window — what is the difference?** [02]
Two pointers usually start at opposite ends of a sorted array and move toward each other based on
a comparison (pair sum, palindrome). A sliding window has both pointers moving in the same
direction, maintaining a contiguous range that satisfies a property, and works when the property
is monotone (shrinking the window can only fix a violation). Both are O(n) because each pointer
moves at most n times.

**6. What is small string optimisation?** [03]
`std::string` stores short strings (up to 15 chars in libstdc++, 22 in libc++) inside the object
itself instead of on the heap, so creating and copying them does not allocate. Longer strings
allocate. It makes `std::string` 32 bytes in libstdc++ and means moving a short string is a copy.

## Hashing

**7. How does `std::unordered_map` work internally?** [04]
An array of buckets; each element is a heap-allocated node in a singly linked list. libstdc++
links all nodes in one list and each bucket points to the node *before* its first element, which
makes iteration O(n) and erase O(1). Lookup hashes the key, takes it modulo the bucket count (a
prime in libstdc++), and walks that bucket's chain comparing keys. When size / bucket_count
exceeds `max_load_factor` (1.0 by default) it rehashes into about twice as many buckets.

**8. Chaining vs open addressing?** [04]
Chaining stores colliding elements in per-bucket lists: simple, tolerates load factor > 1, stable
element addresses, but a pointer chase and an allocation per element. Open addressing stores
elements in the array itself and probes (linear, quadratic, Robin Hood) on collision: no
per-element allocation and far better cache behaviour, but needs load factor well below 1,
tombstones or backward-shift for delete, and moves elements on rehash. Fast modern hash maps
(Abseil's `flat_hash_map`, Folly's F14) are open addressing.

**9. What is the worst case of a hash table and how can it be triggered?** [04]
O(n) per operation, when many keys land in the same bucket. It happens with a weak hash or an
adversary who knows the hash function (hash-flooding attacks on web servers). Defences: a seeded,
strong hash (SipHash), or capping chain length and switching to a tree (Java 8 `HashMap` converts
long buckets to red-black trees).

**10. Why does `std::unordered_map` iteration and lookup lose to a sorted vector at some sizes?** [04, 09]
Each node is a separate allocation, so lookup is hash + bucket array read + at least one
pointer chase to a random address, and iteration follows pointers all over the heap. A sorted
vector is contiguous; binary search touches log n cache lines but the first few levels stay
cached, and iteration is sequential. For small or read-mostly data the vector often wins; for
large data with frequent inserts the hash map wins.

## Linked lists, stacks, queues

**11. When would you use a linked list over a vector?** [05]
When you need O(1) insertion and removal in the middle *given a pointer to the node* and stable
element addresses — an LRU cache's recency list, intrusive lists in kernels, free lists in
allocators. For almost everything else a vector wins because traversal is sequential and the
prefetcher hides memory latency; a list costs a cache miss per node.

**12. How do you detect a cycle in a linked list and find its start?** [05]
Floyd's tortoise and hare: slow moves one step, fast two; they meet inside the cycle if there is
one. Then reset one pointer to the head and move both one step at a time; they meet at the cycle
start, because the distance from the head to the start equals the distance from the meeting point
to the start modulo the cycle length. O(n) time, O(1) space.

**13. Design an LRU cache with O(1) get and put.** [05]
A doubly linked list ordered by recency plus a hash map from key to list node. `get` looks up the
node, moves it to the front, returns the value. `put` updates and moves an existing node, or
inserts at the front and, if over capacity, evicts the tail and erases its key from the map. In
C++, `std::list` plus `unordered_map<Key, list::iterator>` with `splice` for the move.

**14. What is a ring buffer and where is it used?** [06]
A fixed-size array with head and tail indices that wrap around, giving an O(1) FIFO with no
allocation. Using a power-of-two capacity lets you wrap with a mask instead of a modulo. Used for
socket send/receive buffers, logging, audio, and lock-free single-producer single-consumer queues
between threads.

**15. What is a monotonic stack and what does it solve?** [06]
A stack kept in increasing or decreasing order: before pushing, pop everything that violates the
order. Each element is pushed and popped once, so it is O(n) total. It answers "next greater or
smaller element" for every position in one pass: daily temperatures, largest rectangle in a
histogram, trapping rain water.

## Sorting and searching

**16. What does `std::sort` actually do?** [08]
Introsort: quicksort with median-of-three pivots; if the recursion depth exceeds about 2 log2 n
it switches that range to heapsort, guaranteeing O(n log n) worst case; and ranges below 16
elements are left unsorted and finished with one insertion-sort pass at the end. It is not
stable; use `std::stable_sort` (merge sort, O(n) extra memory, O(n log^2 n) if it cannot allocate)
when equal elements must keep their order.

**17. Why is quicksort usually faster than merge sort and heapsort in practice?** [08]
Partitioning scans memory sequentially in place, so it is cache-friendly and has a small inner
loop; merge sort needs an extra buffer and copies; heapsort jumps around the array (children at 2i)
and has poor locality. The cost is a worst case of O(n^2) for bad pivots, which introsort removes.

**18. What is a stable sort and when does it matter?** [08]
Stable means equal keys keep their original relative order. It matters when you sort by multiple
keys in passes (sort by time, then stable-sort by account, and each account's entries stay in
time order) and when the displayed order must be deterministic.

**19. How do you sort data that does not fit in memory?** [08]
External merge sort: read chunks that fit in memory, sort each and write it as a run, then merge
the runs with a k-way merge using a min-heap of run heads, streaming input and output in large
blocks. Cost is about 2 * N * (number of passes) block I/Os; with enough fan-in it is two passes.
This is what PostgreSQL does when a sort exceeds `work_mem`.

**20. Write `lower_bound` without off-by-one errors — what is the invariant?** [09]
Use a half-open range `[lo, hi)` with the invariant "everything before lo is < target, everything
at or after hi is >= target". While `lo < hi`, take `mid = lo + (hi - lo) / 2`; if `a[mid] < t`
set `lo = mid + 1`, else `hi = mid`. Return `lo`. `upper_bound` is the same with `<=`.

**21. What is "binary search on the answer"?** [09]
When the question is "the minimum x such that condition(x) is true" and the condition is
monotone (false, false, ..., true, true), binary-search x over its range and evaluate the
condition at each mid. Examples: minimum eating speed, minimum ship capacity, integer square root.
Cost: O(log range * cost of checking).

## Trees

**22. Why does an unbalanced BST degrade to O(n), and what do balanced trees do about it?** [10, 11]
A BST's operations cost O(height); inserting sorted keys makes every new node the right child of
the previous one, so height is n and the tree is a linked list. Balanced trees (AVL, red-black)
detect imbalance after insert or delete and fix it with O(1) rotations per level, keeping height
O(log n).

**23. AVL vs red-black tree?** [11]
AVL keeps subtree heights within 1, so it is more strictly balanced (height <= 1.44 log2 n),
giving slightly faster lookups but more rotations on updates. Red-black allows height up to
2 log2(n + 1) but needs at most two rotations per insert and three per delete, so updates are
cheaper. `std::map` and `std::set` use red-black trees; so does the Linux CFS scheduler.

**24. B+tree vs binary search tree — why do databases use B+trees?** [11]
A B+tree node is a whole page (8 KB in PostgreSQL) holding hundreds of keys, so the fan-out is in
the hundreds and a billion keys need only 3-4 levels — 3-4 page reads instead of ~30 for a binary
tree, which matters when each level is a disk read or cache miss. All values live in leaves, and
leaves are linked, so a range scan is one descent plus a sequential walk. Splits and merges keep
it balanced with all leaves at the same depth.

**25. B-tree vs B+tree?** [11]
A B-tree stores values in internal nodes too, so some lookups end early, but internal nodes hold
fewer keys (lower fan-out) and range scans must move up and down the tree. A B+tree keeps only
separator keys in internal nodes (higher fan-out, shallower) and all records in a linked leaf
level, which makes range scans and cursors simple. Databases and file systems almost always use
B+trees.

**26. What does "B-tree cursor traversal" mean in practice?** [11]
A cursor is a position in the leaf level: it remembers the current leaf page and slot. Positioning
does one root-to-leaf descent (binary search inside each page); `next` moves to the next slot and
follows the right-sibling link at the end of a page, without going back up the tree. Berkeley DB's
`DBC->get(DB_SET_RANGE)` then `DB_NEXT`, and PostgreSQL's index scans, work this way; concurrent
splits are handled with latch coupling or B-link right-links.

**27. Why are B+trees good even in RAM?** [11]
Memory latency to DRAM is about 100 ns, so a tree's cost is dominated by cache misses per level. A
binary tree of 10^7 nodes has ~23 levels, each a likely miss; a B+tree with 64-256-byte nodes has a
handful of levels, each node read as a few adjacent cache lines that the prefetcher fetches
together. That is why in-memory databases and `absl::btree_map` beat `std::map`.

## Heaps

**28. What is a binary heap and what are its complexities?** [12]
A complete binary tree stored in an array (children of i at 2i+1 and 2i+2) where each parent is
>= its children (max-heap). top is O(1); push sifts up and pop sifts down, both O(log n); building
from n elements bottom-up is O(n). It is not sorted — only the root is guaranteed extreme.

**29. Why is building a heap O(n)?** [12]
Sift down from the last internal node to the root. A node at height h moves at most h levels, and
there are about n / 2^(h+1) nodes at height h; summing h * n / 2^(h+1) over all h converges to n.
Most nodes are near the bottom and barely move.

**30. How do you find the top k of a large stream?** [12]
Keep a min-heap of size k: for each element, if it is larger than the heap's minimum, replace the
minimum. O(n log k) time, O(k) memory, single pass. If all data is in memory and order does not
matter, `std::nth_element` gives O(n) average.

**31. How would you implement timers for 50 000 connections in an event loop?** [12]
A min-heap keyed by deadline: the loop's poll timeout is `top.deadline - now`, and on wake it pops
every expired timer. Resetting a timer on activity is done with lazy deletion (push a new entry,
skip stale ones by version) or an indexed heap with decrease-key. For millions of coarse timers, a
hierarchical timer wheel gives O(1) insert and cancel; the Linux kernel uses wheels.

## Graphs

**32. BFS vs DFS — properties and uses?** [13]
Both visit every vertex and edge once: O(V + E). BFS uses a queue, explores by distance, and finds
shortest paths in unweighted graphs; mark vertices when enqueued. DFS uses recursion or a stack and
is the tool for reachability, cycle detection, topological sort and connected components; deep
graphs need an explicit stack to avoid stack overflow.

**33. How do you detect a cycle in a directed graph, and how is that used in databases?** [13]
DFS with three colours: white unvisited, grey on the current path, black finished; an edge to a
grey vertex is a back edge, so there is a cycle. Alternatively, Kahn's topological sort fails to
output every vertex. Databases detect deadlocks this way: PostgreSQL and Berkeley DB build a
waits-for graph of transactions and search it for a cycle, then abort a victim.

**34. Explain Dijkstra's algorithm and its limits.** [13]
Repeatedly take the unfinished vertex with the smallest tentative distance from a min-heap,
finalise it, and relax its outgoing edges. With a binary heap it is O((V + E) log V); with lazy
deletion the heap holds up to E entries. It requires non-negative weights, because it assumes a
finalised distance cannot be improved later; with negative edges use Bellman-Ford (O(V E)), which
also detects negative cycles.

**35. What is union-find and why is it "almost constant time"?** [13]
A forest stored as a parent array: `find` follows parents to the root, `unite` links one root
under another. Union by size keeps trees O(log n) tall, and path compression points every visited
node at the root; together m operations cost O(m alpha(n)), where the inverse Ackermann function
alpha is at most 4 for any realistic n. It is used for Kruskal's MST, dynamic connectivity, and
grouping records that share identifiers.

## Dynamic programming, tries, bits

**36. How do you recognise a DP problem, and how do you build the solution?** [14]
Signals: it asks for a count, a minimum/maximum or feasibility; choices are made step by step and
restrict later choices; a greedy rule has a counterexample; and the brute-force recursion repeats
subproblems. Build it as a ladder: define the state in one sentence, write the recurrence and base
cases as a recursion, memoise it, convert to a table filled in dependency order, then keep only
the rows the recurrence reads.

**37. Explain 0/1 knapsack and why the one-row version iterates capacity downward.** [14]
`dp[c]` = best value with capacity c; for each item, `dp[c] = max(dp[c], dp[c - w] + v)`. Going
downward means `dp[c - w]` still holds the value without the current item, so each item is used at
most once; going upward lets the item be reused, which is the unbounded knapsack. It is O(n C):
pseudo-polynomial, because C is a number, not an input size.

**38. Give the edit distance recurrence and one production use.** [14]
`dp[i][j]` = edits to turn the first i chars of a into the first j of b; on a match take
`dp[i-1][j-1]`, otherwise 1 + the min of delete `dp[i-1][j]`, insert `dp[i][j-1]` and replace
`dp[i-1][j-1]`; base cases are i and j. O(n m) time, two rows of space for the value. It is used
for fuzzy matching of customer names in deduplication and KYC, usually banded to a threshold k so
it costs O(k n).

**39. When is a trie better than a hash table, and what does it cost?** [15]
When you need prefix operations — autocomplete, count words with a prefix, shortest matching root
— or many patterns matched in one pass over text. Lookups are O(L) in the key length regardless of
the number of keys. The cost is memory (an array-of-26 node is over 200 bytes) and a cache miss per
character; radix trees, index-based arenas or a sorted array mitigate it.

## Senior: cache effects and putting it together

**40. Two O(n) loops — one over a `std::vector<int>`, one over a `std::list<int>` of the same
size — differ by 10x or more. Explain.** [01, 02, 05]
The vector is contiguous: one 64-byte cache line holds 16 ints, and the hardware prefetcher
detects the sequential pattern and loads lines before they are needed, so the loop runs at memory
bandwidth. Each list node is a separate heap allocation, often scattered, and the next address is
only known after loading the current node, so every step can be a full DRAM latency (~100 ns,
typical) that the prefetcher cannot hide. Big-O counts operations, not cache misses; data layout
— contiguous arrays, structure of arrays, B+trees over BSTs, open addressing over chaining, CSR
over adjacency lists — is how you win the constant factor in systems code.
