# 16. Interview coding playbook

**Time:** ~2 hours to read, then ~6 hours a week of practice for 10 weeks   **Prerequisites:** 01-15 (the schedule tells you which module each week depends on)

## 1. In plain words

A coding interview is not a test of whether you can solve the problem. It is a 45-minute sample
of what working with you is like. The interviewer watches how you understand a vague request,
how you choose an approach, how you write code someone else can read, and how you check your
own work. A candidate who silently produces a perfect answer in 20 minutes often scores *lower*
than one who talks through a clean, tested solution in 35, because the interviewer had nothing
to write in the feedback form.

So you need two things: a **method** you run every time, the same way a pilot runs a checklist,
and **pattern recognition**, so that within two minutes of reading a problem you know which of
about fifteen standard shapes it is. This module gives you the method (six steps), the script
for talking while you code, a cheat sheet that maps problem signals to patterns, a 10-week
practice schedule from 2026-10-06 to 2026-12-14 aligned with modules 02-15, a fixed 45-minute
practice block, and a procedure for when you are stuck.

You already have an advantage most candidates lack: production C. You think about memory,
ownership, edge cases and failure. The playbook makes sure the interviewer sees that.

## 2. The real thing

### 2.1 The six-step method

Budget for a 45-minute round with one medium problem (the clock starts after introductions,
so you usually have ~40 minutes):

| Step | Minutes | What you do | What you say |
|------|---------|-------------|--------------|
| 1. Clarify | 0-4 | Restate the problem. Ask about input size, value ranges, duplicates, empty input, sortedness, what to return when there is no answer, whether input may be modified. | "Let me restate to be sure... Can n be zero? Can values be negative? Is the array sorted?" |
| 2. Examples | 4-7 | Write one normal example and one or two edge cases in a comment block. Work the normal one by hand. | "For `[2,7,11,15]`, target 9, the answer is indices 0 and 1. Edge case: no pair, I will return an empty vector." |
| 3. Brute force | 7-10 | State the obvious solution and its complexity. Do not code it unless asked or stuck. | "Brute force: every pair, O(n^2) time, O(1) space. With n up to 10^5 that is 10^10 — too slow." |
| 4. Optimise | 10-15 | Find the bottleneck in the brute force and name the pattern that removes it. State the final complexity. **Get agreement before coding.** | "The inner loop searches for `target - x`. A hash map makes that O(1), so O(n) time, O(n) space. Shall I code that?" |
| 5. Code | 15-32 | Write clean, compilable C++. Narrate intent, not syntax. Helper functions with good names. | "I iterate once; for each value I check whether its complement was seen, then record the value." |
| 6. Test | 32-40 | Trace your example through the code line by line, with variable values. Then the edge cases. Fix bugs calmly. State final complexity. | "i=0: x=2, need 7, not in map, store 2->0. i=1: x=7, need 2, found at 0, return {0,1}." |

The last 5 minutes are for the interviewer's follow-ups ("what if the data does not fit in
memory?", "what if it is a stream?") and your questions.

Rules that make the method work:

- **Never start coding before step 4 is agreed.** Coding the wrong approach for 15 minutes is
  the most common way to fail a round you could have passed.
- **Size the input before choosing the algorithm.** n <= 20: exponential is fine (2^n
  backtracking, bitmask). n <= 10^3: O(n^2) fine. n <= 10^5-10^6: you need O(n log n) or O(n).
  n up to 10^9 or more: O(log n) or math. A modern CPU does roughly 10^8-10^9 simple operations
  per second; that is the arithmetic behind these limits.
- **Write the test cases before the code** (step 2). They catch misunderstandings early and you
  reuse them in step 6.
- **Trace, do not reread.** Reading your code you see what you meant; tracing with concrete
  values you see what you wrote.

### 2.2 Talking while coding

Silence is the enemy, but so is narrating syntax ("now I write for int i equals zero...").
Narrate **decisions and invariants**:

- *Why this structure:* "I use `unordered_map<int,int>` from value to index because I need O(1)
  lookup by value."
- *The invariant:* "Everything left of `slow` is already deduplicated." "The window
  `[left, right]` always has at most k distinct characters."
- *Edge cases as you pass them:* "If the list is empty, `head` is null and the loop does not run,
  so we return null — fine."
- *Trade-offs you are skipping:* "I take the string by `const&`; a `string_view` would also work."
- *Explicit TODOs instead of silent gaps:* "I will assume valid input here and come back to
  validation if we have time."

When you need to think, say so: "Let me think for thirty seconds about the boundary condition."
Then think. Interviewers accept silence that has been announced.

When the interviewer gives a hint, take it, say thanks, and say what it changes: "Right, if it is
sorted I can use two pointers instead of the map, which brings space to O(1)."

C++-specific habits that read as senior:

- `const&` for input containers, return by value (move/NRVO make it free).
- `std::size_t` or explicit casts for sizes; mention signed/unsigned when comparing.
- `auto [a, b] = ...` structured bindings, range-for, `std::vector::reserve` when the size is known.
- Say the complexity of the STL calls you use: `std::sort` O(n log n), `unordered_map` average
  O(1) worst O(n), `std::map` O(log n), `std::priority_queue` push/pop O(log n), `lower_bound` on
  a vector O(log n) — but on a `std::set` use the member `set.lower_bound`, not the free function
  (which is O(n) on non-random-access iterators).
- No `using namespace std;` and no `#include <bits/stdc++.h>` in an interview for a C++ role;
  some interviewers read it as competitive-programming habits.
- Overflow: mention `int` vs `int64_t` when sums or products can be large; `mid = lo + (hi - lo) / 2`.

### 2.3 Pattern cheat sheet

Read the problem, find the signal, pick the pattern. Module numbers point at the full treatment.

| Signal in the problem | Pattern | Typical complexity | Module |
|-----------------------|---------|--------------------|--------|
| Sorted array, find pair/triplet with a sum; remove duplicates in place; palindrome | Two pointers | O(n) | 02 |
| Longest/shortest contiguous subarray or substring with a property | Sliding window (variable) | O(n) | 02, 03 |
| Sum of a range, many range queries, subarray sum equals k | Prefix sums (+ hash map of prefix counts) | O(n) | 02, 04 |
| "Have I seen this", count frequencies, group by key, pair with complement | Hash map / set | O(n) | 04 |
| Linked list: middle, cycle, k-th from end | Fast/slow pointers | O(n), O(1) | 05 |
| Reverse a list or part of it, merge lists | Pointer surgery + dummy head | O(n) | 05 |
| Next greater/smaller element, histogram, temperatures | Monotonic stack | O(n) | 06 |
| Sliding window maximum | Monotonic deque | O(n) | 06 |
| Matching brackets, evaluate expression, undo | Stack | O(n) | 06 |
| All permutations / subsets / combinations, N-queens, sudoku | Backtracking | exponential | 07 |
| Intervals: merge, insert, meeting rooms | Sort by start (or end) + sweep | O(n log n) | 08 |
| Sorted input, or "minimum x such that condition(x) holds" with monotone condition | Binary search / search on answer | O(log n) or O(n log range) | 09 |
| Tree: depth, path sum, balanced, diameter, LCA | DFS recursion returning a value | O(n) | 10 |
| Tree level by level, right side view | BFS with layer loop | O(n) | 10, 13 |
| BST: validate, k-th smallest, range | In-order traversal / bounds | O(h) to O(n) | 10 |
| Top k, k-th largest, merge k sorted, median of stream, scheduling | Heap | O(n log k) | 12 |
| Grid or network, shortest steps, spread over time | BFS (multi-source if many starts) | O(V + E) | 13 |
| Islands, regions, connected groups | DFS/BFS flood fill or union-find | O(V + E) | 13 |
| Dependencies, prerequisites, ordering | Topological sort | O(V + E) | 13 |
| Weighted shortest path | Dijkstra | O((V+E) log V) | 13 |
| Dynamic grouping, "are these connected", redundant edge | Union-find | ~O(1) per op | 13 |
| Count ways / min cost / max value / feasible with sequential choices | DP | states x transition | 14 |
| Two strings: common subsequence, edit, interleave | 2D DP on prefixes | O(n m) | 14 |
| Choose items under a capacity | Knapsack DP | O(n C) | 14 |
| Prefix lookups, autocomplete, word search in grid | Trie | O(L) | 15 |
| "Appears once / twice", subsets of small n, power of two, flags | Bit manipulation | O(n) | 15 |
| Design: LRU cache, rate limiter, hit counter | Hash map + list / deque / ring buffer | O(1) | 05, 06 |

If two patterns fit, say both and pick one with a reason: "This could be DP over positions, but
the window property is monotone, so a sliding window does it in O(n) and O(1) space."

### 2.4 The 10-week problem schedule

Weeks run Tuesday to Monday starting **Tuesday 2026-10-06**, the same weeks as `../00_START_HERE.md`. Each week has a pattern focus aligned with
the course modules you study in parallel, and 10-12 problems: the first half warm-up level
(easy or easy-medium), the rest medium, one stretch problem. Names are the standard LeetCode
titles so you can find them. Re-solve every problem you fail on day +3 and day +10 (see 2.5).

**Week 1 — 2026-10-06 to 10-12. Arrays, two pointers, sliding window, prefix sums (modules 01, 02).**
Two Sum; Best Time to Buy and Sell Stock; Remove Duplicates from Sorted Array; Squares of a
Sorted Array; Two Sum II (sorted input); Container With Most Water; 3Sum; Maximum Subarray;
Product of Array Except Self; Longest Substring Without Repeating Characters; Minimum Size
Subarray Sum. Stretch: Trapping Rain Water.

**Week 2 — 10-13 to 10-19. Hashing and strings (modules 03, 04).**
Valid Anagram; Contains Duplicate; Valid Palindrome; Group Anagrams; Top K Frequent Elements;
Longest Consecutive Sequence; Subarray Sum Equals K; Longest Repeating Character Replacement;
Permutation in String; String to Integer (atoi); Encode and Decode Strings. Stretch: Minimum
Window Substring.

**Week 3 — 10-20 to 10-26. Linked lists, stacks, queues (modules 05, 06).**
Reverse Linked List; Merge Two Sorted Lists; Linked List Cycle; Middle of the Linked List;
Remove Nth Node From End of List; Reorder List; Valid Parentheses; Min Stack; Implement Queue
using Stacks; Daily Temperatures; Evaluate Reverse Polish Notation; LRU Cache. Stretch:
Largest Rectangle in Histogram.

**Week 4 — 10-27 to 11-02. Recursion, backtracking, sorting, intervals (modules 07, 08).**
Subsets; Permutations; Combination Sum; Generate Parentheses; Letter Combinations of a Phone
Number; Word Search; Merge Intervals; Insert Interval; Non-overlapping Intervals; Sort Colors;
Kth Largest Element in an Array (quickselect). Stretch: N-Queens.

**Week 5 — 11-03 to 11-09. Binary search and heaps (modules 09, 12).**
Binary Search; Search Insert Position; Find First and Last Position of Element in Sorted Array;
Search in Rotated Sorted Array; Find Minimum in Rotated Sorted Array; Koko Eating Bananas;
Time Based Key-Value Store; K Closest Points to Origin; Last Stone Weight; Merge k Sorted Lists;
Task Scheduler. Stretch: Find Median from Data Stream.

**Week 6 — 11-10 to 11-16. Trees and BSTs (modules 10, 11).**
Maximum Depth of Binary Tree; Invert Binary Tree; Same Tree; Subtree of Another Tree; Binary
Tree Level Order Traversal; Binary Tree Right Side View; Validate Binary Search Tree; Kth
Smallest Element in a BST; Lowest Common Ancestor of a BST; Diameter of Binary Tree; Construct
Binary Tree from Preorder and Inorder Traversal. Stretch: Serialize and Deserialize Binary Tree.

**Week 7 — 11-17 to 11-23. Graphs (module 13).**
Flood Fill; Number of Islands; Max Area of Island; Rotting Oranges; Clone Graph; Course
Schedule; Course Schedule II; Pacific Atlantic Water Flow; Number of Provinces; Redundant
Connection; Network Delay Time. Stretch: Word Ladder.

**Week 8 — 11-24 to 11-30. Dynamic programming I: 1D (module 14).**
Climbing Stairs; Min Cost Climbing Stairs; House Robber; House Robber II; Coin Change; Coin
Change II; Decode Ways; Word Break; Longest Increasing Subsequence; Maximum Product Subarray;
Partition Equal Subset Sum. Stretch: Russian Doll Envelopes.

**Week 9 — 12-01 to 12-07. DP II (2D) plus tries and bits (modules 14, 15).**
Unique Paths; Minimum Path Sum; Longest Common Subsequence; Edit Distance; Longest Palindromic
Substring; Target Sum; Implement Trie (Prefix Tree); Design Add and Search Words Data Structure;
Single Number; Counting Bits; Missing Number. Stretch: Word Search II.

**Week 10 — 12-08 to 12-14. Mixed timed mocks.**
No new pattern. Four full mock rounds of two problems each (one medium, one medium-hard) under
a 45-minute timer, chosen at random from weeks 1-9 problems you have not re-solved recently,
plus these new mixed ones: Meeting Rooms II; Cheapest Flights Within K Stops; Accounts Merge;
Design Hit Counter; Find K Pairs with Smallest Sums; Longest Increasing Path in a Matrix;
Snapshot Array; Alien Dictionary. If possible do at least two mocks with another person
(a friend, a peer-interview site) — talking to a real listener is a different skill from
talking to yourself.

Weekly rhythm (about 6 hours): five weekday 45-minute blocks (one new problem each plus review),
and one weekend session of three blocks (two new problems and one re-solve). Read the matching
module on the weekend before the week starts.

### 2.5 The 45-minute practice block

Practise exactly like the real thing, or the practice does not transfer.

```
00:00  Timer on. Read the problem once. Write the clarifying questions you would ask
       as comments, and answer them from the problem statement / constraints.
00:04  Examples: one normal, two edge cases, in a comment.
00:07  Brute force + complexity, one line in a comment.
00:10  Optimised approach + complexity in two lines. Name the pattern.
00:15  Code. Out loud (yes, alone at your desk). No autocomplete, no compiling yet.
00:32  Trace your example by hand. Then compile and run the tests.
00:40  Fix. Submit.
00:42  Log it (below). Stop at 00:45 whatever the state.
```

If you are not done at 00:45, read a good solution, close it, and write it from memory the same
day. Then it goes into the re-solve queue.

The log is a plain file, one line per attempt:

```
date        problem                       pattern          result   time  note
2026-10-07  Longest Substring Without...  sliding window   solved   28m   forgot to shrink with while
2026-10-08  Product of Array Except Self  prefix products  hint     45m   missed the two-pass idea
```

Re-solve queue: anything marked `hint` or `failed` comes back on day +3 and day +10. A problem
is "owned" when you solve it cold in under 25 minutes, twice. By week 10 the log tells you
exactly which patterns are weak; spend the mock week on those.

Write each solution in a local file with a tiny test harness (section 3) and compile it with
the course flags. Platform judges hide your compiler warnings; your interviewer will not.

### 2.6 When you are stuck

Use this ladder in order. Say each step out loud in a real interview.

1. **Re-read the constraints.** n tells you the target complexity (2.1). "Sorted", "distinct",
   "positive", "at most k" are hints you skipped.
2. **Do a small example by hand, slowly**, and watch what *you* do. Your hands often perform the
   algorithm before your head names it.
3. **Write the brute force** and ask: what is it recomputing? What is it searching for in the
   inner loop? Repeated work -> memo/DP or prefix sums; searching -> hash map, binary search or
   heap; scanning a window -> sliding window.
4. **Walk the cheat sheet** (2.3) top to bottom and ask "could this be X?" for each row.
5. **Change the problem**: solve it for sorted input, for k = 1, for a tree that is a list, for
   n = 2. Then generalise.
6. **Think backwards** from the answer: "if I knew the answer for the first n-1 elements, how
   would I get it for n?" — that is a DP or an inductive invariant.
7. **Ask for a hint** after ~5 minutes of no progress, framed with what you have: "I have an
   O(n^2) approach using X; I suspect the bottleneck is Y; is the direction of using a heap
   reasonable?" A specific question costs you very little; ten minutes of silence costs you the
   round.
8. **If time is short, code the brute force cleanly** and test it. A correct, clean O(n^2) with a
   clear explanation of how to improve it beats a broken O(n).

When you have a bug in step 6 and cannot see it: re-trace with the smallest failing input,
print (or say) the variables at each loop iteration, and check the usual suspects — off-by-one
in loop bounds, the empty input, `int` overflow, a reference to a vector element across a
`push_back`, a missing `break`, modifying a container while iterating it.

## 3. Code

No new program file for this module: every pattern's reference code is in `code/m02_*` to
`code/m15_*`. What you need is a practice template. Save this as `practice.cpp` (outside the
course folder), copy it for every problem, and compile with the course flags. The `check`
helper prints the failing case instead of only aborting, which saves time in a 45-minute block.

```cpp
// practice.cpp — copy per problem.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined practice.cpp -o p && ./p

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Clarify:   n in [2, 1e5], values may be negative, exactly one answer?  -> return {} if none
// Examples:  [2,7,11,15], 9 -> {0,1}     [3,3], 6 -> {0,1}     [1,2], 7 -> {}
// Brute:     all pairs, O(n^2) time, O(1) space
// Optimised: hash map value -> index, O(n) time, O(n) space

std::vector<int> solve(const std::vector<int>& a, int target) {
    std::unordered_map<int, int> seen;                   // value -> index
    seen.reserve(a.size());
    for (int i = 0; i < static_cast<int>(a.size()); ++i) {
        auto it = seen.find(target - a[i]);
        if (it != seen.end()) return {it->second, i};
        seen.emplace(a[i], i);                           // emplace keeps the FIRST index of a duplicate
    }
    return {};
}

std::vector<int> brute(const std::vector<int>& a, int target) {
    for (int i = 0; i < static_cast<int>(a.size()); ++i)
        for (int j = i + 1; j < static_cast<int>(a.size()); ++j)
            if (a[i] + a[j] == target) return {i, j};
    return {};
}

template <typename T>
void check(const T& got, const T& want, const char* what) {
    if (got == want) return;
    std::cerr << "FAIL: " << what << '\n';
    std::abort();
}

int main() {
    check(solve({2, 7, 11, 15}, 9), std::vector<int>{0, 1}, "example 1");
    check(solve({3, 3}, 6), std::vector<int>{0, 1}, "duplicates");
    check(solve({1, 2}, 7), std::vector<int>{}, "no answer");

    // Differential test against the brute force on small random inputs: the fastest way to
    // find the bug you cannot see. Compare only "found / not found" plus validity when the
    // problem allows several correct answers.
    std::mt19937 rng(1);
    for (int t = 0; t < 10000; ++t) {
        std::vector<int> a(2 + rng() % 8);
        for (int& x : a) x = static_cast<int>(rng() % 21) - 10;
        int target = static_cast<int>(rng() % 21) - 10;
        auto s = solve(a, target), b = brute(a, target);
        check(s.empty(), b.empty(), "found vs not found");
        if (!s.empty()) check(a[s[0]] + a[s[1]], target, "valid pair");
    }
    std::cout << "ok\n";
}
```

Two habits this template builds: writing the clarify/examples/brute/optimised notes *before*
code (steps 1-4 of the method), and keeping a brute-force oracle for differential testing,
exactly as the course's code files do.

## 4. Under the hood (senior-level)

### 4.1 What the interviewer writes down

Most companies use a rubric with four or five dimensions, scored separately. The names vary;
the content is stable:

- **Problem solving**: did you clarify, consider alternatives, reach an efficient approach with
  little help, and justify complexity?
- **Coding**: is the code correct, readable, idiomatic for the language, with sensible names and
  structure? Did you handle edge cases?
- **Verification**: did you test without being asked, find your own bugs, and reason about
  correctness?
- **Communication**: could the interviewer follow your thinking throughout? Did you respond well
  to hints?
- (for senior and systems roles) **Depth**: can you discuss memory layout, concurrency, scale,
  and production trade-offs when the follow-up comes?

The six steps map one-to-one onto these. Steps 1-4 are problem solving, step 5 is coding,
step 6 is verification, and talking throughout is communication. The "Under the hood" sections
of modules 01-15 are your material for depth.

### 4.2 Follow-ups for C++ systems roles

After the algorithm works, expect one of these. Prepare a two-sentence answer for each:

- *"What if the input does not fit in memory?"* External sort and k-way merge (modules 08, 12),
  streaming with bounded state (top-k heap), partition by hash and process each part (04).
- *"What if it is a stream?"* Sliding windows, heaps, reservoir sampling, count-min sketch.
- *"Make it thread-safe."* Mutex around the structure, then sharding by key (module 05, LRU),
  then read-mostly structures; name the contention point. See 04_Concurrency.
- *"What is the cache behaviour?"* Contiguous vs node-based (vector vs list, open addressing vs
  chaining, B+tree vs BST, CSR vs adjacency lists).
- *"What happens with 10^9 elements?"* `int` overflow, `size_t` indices, memory footprint per
  element, recursion depth.
- *"How would you test this in production code?"* Unit tests on edge cases, differential tests
  against a brute-force oracle, sanitizers, fuzzing.

### 4.3 Why practising out loud matters

Explaining while you code uses working memory; so does holding the algorithm in your head. The
first few times you do both, your coding speed drops noticeably and you make bugs you would not
make silently. That cost goes away with practice — but only with practice *out loud*. This is
the main reason silent LeetCode grinding transfers poorly to interviews, and why the practice
block insists on narration even when alone.

## 5. In production

The playbook's habits are the same habits that make a good production engineer, which is the
point you want the interviewer to notice.

- **Clarify = requirements.** "Can the account number be empty? Can amounts be negative
  (reversals)?" is the same question you ask before writing a migration step. Mention a real
  example when it fits: "In our core-banking server, a reversal is a negative amount, so I would
  not assume non-negative input without asking."
- **Brute force first = a correct baseline.** In the BDB -> YugabyteDB migration, the first
  version that is obviously correct (row by row) is the oracle for the fast batched version.
  Same as the differential test in the practice template.
- **Test = reconciliation.** Row counts and checksums after a migration are step 6 at scale.
- **Stories for the behavioural part.** Prepare three, each two minutes, in situation -> action ->
  result form, each with a data-structure or algorithm angle: a performance bug you fixed (an
  O(n^2) loop, a lock held too long, a missing index), a crash you debugged from a dump (stack
  overflow from recursion is a good one), and a design decision with a trade-off (hash vs
  B-tree lookup, ring buffer for socket I/O).
- **Pitfall: over-engineering in the interview.** Templates, custom allocators and class
  hierarchies for a 30-line function cost time and read as poor judgement. Write a function.
  Mention the production version in one sentence.

## 6. Try it

1. **(easy)** Run the practice template as given. Then break `solve` on purpose (use
   `operator[]` instead of `emplace`, which overwrites the first index of a duplicate) and see
   whether the tests catch it. Done when you understand which test would catch it and add one
   if none does.
2. **(easy)** Write the pattern cheat sheet (2.3) from memory on one page. Done when you can fill
   in at least 20 of the 27 rows with signal, pattern and complexity in 15 minutes.
3. **(medium)** Do three 45-minute blocks on week 1 problems out loud while recording audio on
   your phone. Listen back. Done when you have written down three things you said that were
   noise ("so, um, now I...") and three decisions you did not say aloud but should have.
4. **(medium)** Take one problem you solved and answer the systems follow-ups in 4.2 for it in
   writing, two sentences each. Done when each answer names a concrete technique and a module.
5. **(hard)** Week 10 mock: two problems, 45 minutes, with another person as interviewer using
   the rubric in 4.1. Done when you have their scores on each dimension and one concrete thing
   to change for the next mock.
6. **(hard)** Keep the practice log for all ten weeks. Done when on 2026-12-14 the log has at least
   110 entries, every `hint`/`failed` entry has two re-solve entries, and you can name your two
   weakest patterns with evidence from the log.

## 7. Interview questions

These are meta-questions interviewers actually ask, plus the ones you should be ready to answer
about your own process.

**Q: How do you approach a problem you have never seen?**
Clarify inputs and edge cases, work examples by hand, state a brute force and its complexity,
find the bottleneck and the pattern that removes it, agree on the approach, code it while
explaining, then trace tests. I size the input first because it tells me the complexity I need.

**Q: Your solution is O(n^2). Can you do better?**
Name what the inner loop does — searching, recomputing, or scanning a window — and replace it
with a hash map, prefix sums/DP, binary search or heap, or a sliding window. State the new time
and space, and the trade-off (usually memory for time).

**Q: How do you test your code?**
A normal case traced by hand, then edge cases: empty, single element, all equal, negatives,
maximum size (overflow, recursion depth), no answer. Off the whiteboard: differential tests
against a brute-force oracle and sanitizers.

**Q: What would you change for production?**
Input validation and error handling, `int64_t` where values can grow, iterative instead of
recursive for deep inputs, reserve and avoid needless copies, thread-safety if shared, and
tests. Name the one that matters most for this problem.

**Q: What do you do when you are stuck?**
Re-read constraints, do a tiny example by hand, write the brute force and ask what it repeats,
try simpler versions of the problem, and ask a specific question if I am still stuck after a few
minutes, explaining what I have.

**Q: Why C++ for this interview?**
It is the language of the role and my strongest after C; it gives me the standard containers and
algorithms with known complexities, and I can reason about memory layout and cost precisely —
which is the part of the follow-up discussion I enjoy most.

## 8. Recap

- Six steps: clarify, examples, brute force, optimise (agree before coding), code, test.
- Budget: ~15 minutes to an agreed approach, ~17 to code, ~8 to test.
- Input size picks the complexity: 20 -> exponential, 10^3 -> n^2, 10^5-10^6 -> n log n or n.
- Narrate decisions and invariants, not syntax; announce thinking pauses.
- Pattern cheat sheet: signal -> pattern -> complexity -> module.
- Schedule: 10 weeks from 2026-10-06, one pattern family per week, mocks in week 10.
- Practice in 45-minute blocks, out loud, logged; re-solve misses on day +3 and +10.
- Stuck ladder: constraints, small example, brute force bottleneck, cheat sheet, simplify,
  backwards, specific hint request, clean brute force.
- Keep a brute-force oracle and differential tests, as in production.
