# 03. Data Structures and Algorithms — DSA in C++17, from arrays to interview-ready

## What this course covers

You already wrote `array.cpp`, `lowArray.cpp` and `orderedArray.cpp`: a fixed-size array with
linear search, delete-by-shifting, and a binary search on a sorted array. That is the floor.
This course starts one step above it and ends with you able to:

- analyse any loop or recursion for time and space cost, and explain why the constant
  factors (cache misses, branch misprediction) sometimes matter more than the Big-O;
- implement from memory, without looking anything up: a hash map, a singly and doubly linked
  list, an LRU cache, a ring buffer, a binary search tree, an AVL rotation, a B+tree with
  range scan, a binary heap, a trie, union-find, BFS/DFS/Dijkstra, and the standard
  dynamic-programming tables;
- pass a 45-minute LeetCode-medium coding round for a C++ systems role: talk while you code,
  pick the right pattern in two minutes, write it bug-free in fifteen, test it in five;
- connect each structure to the systems you have worked on: B-trees to Berkeley DB and
  PostgreSQL indexes, hash tables to connection lookups in your RPC server, ring buffers to
  socket I/O, heaps to timer wheels and schedulers, external merge sort to `ORDER BY`.

Everything is C++17 on WSL Ubuntu 24.04 (`g++ -std=c++17 -Wall -Wextra -Werror -pthread`).
The code files never use `using namespace std;`. Module 01 explains why.

## Why this is on the resume

The resume says "Data structures & algorithms", "B-tree cursor traversal", "indexing", and
"hashing". Those are not decorative. A Berkeley DB -> YugabyteDB migration engine walks B-tree
pages with a cursor; a core-banking application server hashes session ids and account numbers
to find state in O(1); PostgreSQL indexes are B+trees. An interviewer who reads those lines will
ask you to implement or explain them. This course makes sure you can.

The second reason is the coding round. Every C++ systems role you will apply to in the next
12 weeks screens with one or two 45-minute problem-solving rounds. They are LeetCode-medium
style: arrays, hashing, linked lists, trees, graphs, DP. Module 16 turns the first 15 modules
into a 10-week practice schedule.

## Prerequisites

- C at a professional level (pointers, structs, malloc/free, function pointers). You have this.
- C++ basics: classes, constructors, references, `std::vector`, `std::string`. You have most of
  this. Where a module uses something newer (`std::unique_ptr`, `std::optional`, structured
  bindings, lambdas, `std::string_view`), it explains it in one paragraph at the point of use.
- Ability to compile and run a C++ file on WSL:
  `g++ -std=c++17 -Wall -Wextra -Werror -pthread file.cpp -o file && ./file`.

## Module list

| # | Module | Hours | What you get |
|---|--------|-------|--------------|
| 01 | [Complexity and how to think](01_complexity_and_how_to_think.md) | 2 | Big-O, amortised cost, counting loops, the cache caveat |
| 02 | [Arrays, vectors, two pointers](02_arrays_vectors_two_pointers.md) | 4 | `std::vector` internals, two-pointer, sliding window, prefix sums, Kadane, 8 problems |
| 03 | [Strings](03_strings.md) | 3 | SSO, `string_view`, parsing, string hashing, KMP |
| 04 | [Hashing and hash tables](04_hashing_hash_tables.md) | 4 | Buckets, chaining vs open addressing, load factor, your own hash map, 6 problems |
| 05 | [Linked lists](05_linked_lists.md) | 4 | `unique_ptr` vs raw, dummy head, reverse, cycle, merge, LRU cache |
| 06 | [Stacks, queues, deques](06_stacks_queues_deques.md) | 3 | Monotonic stack, two-stack queue, ring buffer, 6 problems |
| 07 | [Recursion and backtracking](07_recursion_and_backtracking.md) | 3 | Call stack, permutations, subsets, N-queens, memoisation |
| 08 | [Sorting](08_sorting.md) | 4 | Merge, quick, heap, introsort, stability, radix, external merge sort |
| 09 | [Binary search and ordered data](09_binary_search_and_ordered_data.md) | 3 | lower/upper bound, first/last, search on answer, rotated arrays, B-tree node search |
| 10 | [Trees and BST](10_trees_bst.md) | 4 | Traversals, insert/delete, validate, LCA, serialise, why unbalanced is bad |
| 11 | [Balanced trees, B-tree and disk](11_balanced_trees_btree_and_disk.md) | 6 | AVL, red-black, B-tree, B+tree in depth, cursor traversal, in-memory B+tree |
| 12 | [Heaps and priority queues](12_heaps_priority_queues.md) | 3 | Array layout, sift up/down, top-k, merge k, timers and schedulers |
| 13 | [Graphs: BFS, DFS and friends](13_graphs_bfs_dfs.md) | 5 | Representations, topo sort, cycles, components, Dijkstra, union-find, 8 problems |
| 14 | [Dynamic programming](14_dynamic_programming.md) | 5 | Recursion -> memo -> table, knapsack, LCS, edit distance, LIS, coin change |
| 15 | [Tries and bit manipulation](15_tries_and_bit_manipulation.md) | 3 | Trie, prefix problems, popcount, XOR tricks, bitsets |
| 16 | [Interview coding playbook](16_interview_coding_playbook.md) | 2 + practice | 6-step method, talking while coding, pattern cheat sheet, 10-week schedule |

Total: about 58 hours of reading and coding, plus the practice schedule in module 16
(roughly 6 hours a week for 10 weeks).

Other files:

- `code/` — 37 compilable C++17 programs, one per data structure or problem set, named
  `mNN_<name>.cpp` where `NN` is the module number.
- `EXERCISES.md` — every exercise in the course with difficulty and a hint.
- `INTERVIEW_QA.md` — 40 interview questions with model answers, basic to senior.

## Suggested order

Study the modules in order; each one assumes the previous ones. If you are short on time, the
minimum path for the coding round is 01, 02, 04, 05, 06, 09, 10, 12, 13, 14, 16. The minimum
path for the "B-tree cursor traversal / indexing" resume line is 01, 09, 10, 11.

Study this course in parallel with the Modern C++ course. Module 05 here uses `std::unique_ptr`;
if that is unfamiliar, read the smart-pointer module there first.

## You are done when you can...

- [ ] Say the time and space complexity of any code you write, and justify it by counting.
- [ ] Derive why `std::vector::push_back` is amortised O(1) and what happens when growth is 1.5x vs 2x.
- [ ] Explain why iterating a `std::vector` is often 10x faster than iterating a `std::list` of the
      same size even though both are O(n).
- [ ] Implement a chained hash map with resize in 20 minutes; explain load factor and why
      `std::unordered_map` iteration is slow.
- [ ] Implement an LRU cache (doubly linked list + hash map) in 15 minutes with O(1) get and put.
- [ ] Reverse a linked list iteratively and recursively; detect a cycle with Floyd's algorithm
      and explain why it terminates.
- [ ] Write a ring buffer with wrap-around and explain when it is used in a TCP server.
- [ ] Explain quicksort partition schemes, the worst case, and how introsort avoids it.
- [ ] Write `lower_bound` by hand without an off-by-one, and use it for "first occurrence",
      "search on answer" and "find key in B-tree node".
- [ ] Insert and delete in a BST; explain why an unbalanced BST degrades to O(n).
- [ ] Explain AVL rotations and red-black invariants at the level of "what and why", not line-by-line.
- [ ] Draw a B+tree page, explain fan-out from page size and key size, walk a cursor through a
      range scan, and implement an in-memory B+tree with split.
- [ ] Implement a binary heap; solve top-k and merge-k-sorted; explain a timer queue.
- [ ] BFS, DFS, topological sort, Dijkstra, union-find — from memory.
- [ ] Recognise a DP problem in 60 seconds and go from recursion to memo to table.
- [ ] Solve a LeetCode-medium in 35 minutes while talking, using the 6-step method.

## Resume lines this course backs up

- "Data structures & algorithms" — all modules.
- "B-tree cursor traversal" — module 11 (cursor, range scan, leaf chain), module 09 (node search).
- "indexing" — module 11 (why databases use B+trees, page layout, PostgreSQL btree), module 08
  (external merge sort for index build), module 04 (hash indexes).
- "hashing" — module 03 (string hashing), module 04 (hash tables), module 05 (LRU cache).
- Interview coding rounds for C++ systems roles — modules 02 to 15 problems, module 16 playbook.
