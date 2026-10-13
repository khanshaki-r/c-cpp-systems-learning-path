# 15. Tries and bit manipulation

**Time:** ~3 hours   **Prerequisites:** 03, 04, 14

## 1. In plain words

A **trie** (say "try", from re*trie*val) is a tree for strings where each edge is one
character. To store "app" and "apple" you walk a, p, p from the root, mark the node "a word
ends here", then continue l, e and mark again. Words that share a prefix share the path. It is
the structure behind the autocomplete box on your phone: type "ban", walk three edges, and
every word below that node is a suggestion. A hash set can tell you "is `bank` a word?" just as
fast, but it cannot answer "which words start with `ban`?" without scanning everything. A trie
answers both in time proportional to the length of the word, no matter how many words are
stored.

**Bit manipulation** is using the individual bits of an integer as data. A 64-bit integer is 64
yes/no flags you can test, set and combine in a single CPU instruction. You already do this in
C with flag masks (`O_RDONLY | O_CREAT`) and permission bits. Interviews ask a small, fixed set
of tricks: is this a power of two, count the set bits, find the number that appears once, list
all subsets. Systems interviews go one step further: bitsets for "which of 10 million ids have
I seen", bitmaps in allocators and databases, and why `x & (x - 1)` is everywhere.

The two topics meet in the **binary trie**: a trie over the bits of integers, which finds the
maximum XOR of two numbers and is how IP routing tables do longest-prefix match.

## 2. The real thing

### 2.1 Trie structure

```
insert: app, apple, ape, bank

            (root)
           /      \
          a        b
          |        |
          p        a
         / \       |
        e*  p*     n
            |      |
            l      k*
            |
            e*          * = a word ends here
```

Each node has:

- children: one slot per possible next character. Options: a fixed array of 26 pointers
  (fastest, 208 bytes of pointers per node on 64-bit), a sorted small vector of `(char, child)`
  pairs, or a hash map (sparse, slow constant).
- an end marker: `bool is_end`, or a counter `end` (how many copies of the word end here — needed
  for multiset semantics and for deletion).
- optionally `pass`: how many words pass through this node — gives "count words with this
  prefix" in O(prefix length).

Operations, L = length of the word or prefix:

| Operation | Time | Notes |
|-----------|------|-------|
| insert(w) | O(L) | creates at most L nodes |
| search(w) | O(L) | walk, then check the end marker |
| starts_with(p) | O(L) | walk only |
| count_prefix(p) | O(L) | needs `pass` counts |
| erase(w) | O(L) | decrement counts; free branches whose `pass` hits 0 |
| autocomplete(p, k) | O(L + size of the output subtree visited) | DFS below the prefix node, children in order gives lexicographic output |

Memory: O(total characters x alphabet size) for array children in the worst case. 100 000
English words average ~9 letters; with no sharing that is 900 000 nodes x ~216 bytes = ~190 MB.
Real sharing helps a lot, but array-of-26 tries are memory-hungry. That is the main trade-off
to state in an interview.

### 2.2 Trie vs hash set vs sorted array

| Need | Hash set | Sorted vector + binary search | Trie |
|------|----------|-------------------------------|------|
| Exact lookup | O(L) average | O(L log n) | O(L) |
| All words with prefix p | O(n L) scan | O(L log n) to find the range, then output | O(L) to find, then output |
| Count with prefix | O(n L) | two binary searches: O(L log n) | O(L) with `pass` |
| Memory | low | lowest | high |
| Ordered iteration | no | yes | yes (DFS) |

The sorted vector is underrated: `lower_bound(p)` and `lower_bound(p + '\x7f')` (or a
"prefix successor") bound the range of words with prefix `p`, and the data is a contiguous
array. If the dictionary is static, a sorted array often beats a pointer trie on real hardware.
The trie wins when the set changes, when you need per-node data (counts, "best suggestion
below here"), or when you walk the text character by character against many patterns at once
(word search II, replace words, Aho-Corasick).

### 2.3 Prefix problems that want a trie

- **Implement trie / prefix tree**: insert, search, starts_with.
- **Replace words**: replace each word by its shortest root in a dictionary — walk the word in
  the trie and stop at the first end marker.
- **Word search II**: find all dictionary words in a letter grid. Put the dictionary in a trie
  and DFS the grid walking the trie in parallel; prune when the current path is not a prefix.
  Searching each word separately is far slower.
- **Autocomplete / search suggestions**: prefix walk, then DFS for the first k words. Store the
  top suggestions at each node if queries dominate.
- **Longest common prefix**: walk from the root while the node has exactly one child and is not
  an end. (Without a trie: sort, compare only the first and last words.)
- **Maximum XOR of two numbers**: binary trie (2.7).

### 2.4 Bit operations you must know cold

On unsigned integers (always do bit work on unsigned types — see 4.1):

| Expression | Meaning | Example (x = 0b101100) |
|------------|---------|------------------------|
| `(x >> i) & 1` | test bit i | bit 2 -> 1 |
| `x \| (1u << i)` | set bit i | |
| `x & ~(1u << i)` | clear bit i | |
| `x ^ (1u << i)` | toggle bit i | |
| `x & (x - 1)` | clear the lowest set bit | 0b101000 |
| `x & -x` (or `x & (~x + 1)`) | isolate the lowest set bit | 0b000100 |
| `x != 0 && (x & (x - 1)) == 0` | x is a power of two | |
| `x \| (x + 1)` | set the lowest clear bit | 0b101101 |
| `(x & mask) == mask` | all bits of mask set | |

XOR facts: `a ^ a = 0`, `a ^ 0 = a`, XOR is commutative and associative. So XOR-ing a list in
which every value appears twice except one leaves that one ("single number"). XOR-ing `0..n`
with the array leaves the missing number.

Counting set bits (popcount):

- loop over 32 bits: O(32);
- **Kernighan**: `for (; x; x &= x - 1) ++c;` — O(number of set bits);
- `__builtin_popcount` / `__builtin_popcountll` (GCC, Clang), `std::bitset<N>::count()`, or
  `std::popcount` in C++20 — a single `POPCNT` instruction when compiled with `-mpopcnt` or
  `-march=native`, a table/bit-twiddling sequence otherwise.

Related builtins: `__builtin_ctz` (count trailing zeros = index of the lowest set bit),
`__builtin_clz` (count leading zeros; `31 - clz(x)` = floor(log2 x)). Both are undefined for
x == 0 — check first. C++20 gives `std::countr_zero`, `std::countl_zero`, `std::bit_width`,
`std::has_single_bit`, which are defined for 0.

### 2.5 Subsets as masks

n items, `mask` from 0 to 2^n - 1: bit i set means item i is in the subset. This replaces the
backtracking subset generator of module 07 for n <= ~20.

```cpp
for (std::uint32_t mask = 0; mask < (1u << n); ++mask)
    for (int i = 0; i < n; ++i)
        if (mask & (1u << i)) { /* item i is in this subset */ }
```

All submasks of `m`: `for (s = m; ; s = (s - 1) & m) { ...; if (s == 0) break; }`. Iterating
submasks of every mask costs 3^n in total, not 4^n — a known bound worth quoting.

**Bitmask DP**: state = (set of items used so far, last item). Travelling salesman over n
cities: `dp[mask][v]` = cheapest path from city 0 visiting exactly `mask`, ending at `v`;
O(2^n n^2). n = 16 is about 16 million states x 16 — fine; n = 30 is hopeless.

**Gray code**: `g(i) = i ^ (i >> 1)`; consecutive values differ in one bit. Used in rotary
encoders and to enumerate subsets by toggling one element at a time.

### 2.6 Bitsets

`std::bitset<N>` is N bits packed into machine words with a fixed compile-time N: `set`, `reset`,
`test`, `count`, `&`, `|`, `^`, `<<`, `>>`, `any`, `none`. Operations on whole bitsets work a word
(64 bits) at a time, so intersecting two sets of 1 million flags is ~15 600 word ANDs.

For a runtime size, `std::vector<bool>` is a specialised, bit-packed vector — but its
`operator[]` returns a proxy object, not a `bool&`, so `auto& b = v[i]` does not compile and
it is not a real container. Many codebases avoid it; use `std::vector<char>` when you want
bytes, or a small hand-written dynamic bitset (in `m15_bit_tricks.cpp`) when you want bits and
word-level operations. `boost::dynamic_bitset` exists too.

Classic uses: Sieve of Eratosthenes (one bit per number), "seen" sets over dense integer ids,
Bloom filters (k hash functions setting k bits; false positives, no false negatives),
allocation bitmaps (which pages/blocks/slots are free — find the first zero bit with
`ctz(~word)`), and subset DP where the DP row itself is a bitset (`dp |= dp << w` solves subset
sum for all sums at once, 64 sums per instruction).

### 2.7 Binary trie: maximum XOR of two numbers

Insert every number's bits from bit 31 down to bit 0 into a trie with two children per node.
For a query `x`, walk from the top bit and at each level try to go to the child with the
*opposite* bit (that bit of the XOR becomes 1); if it does not exist, take the same bit. A 1 in
bit k outweighs all lower bits combined (2^k > 2^k - 1), so greedy from the top is optimal.
O(32 n) total. The same structure, keyed by IP address bits, is a routing table's
longest-prefix match (production routers use compressed variants — Patricia/radix tries).

## 3. Code

Files:

- `code/m15_trie.cpp` — a 26-way pointer trie with `insert`, `search`, `starts_with`,
  `count_prefix`, `erase` (frees unused branches) and lexicographic `autocomplete`; an
  index-based trie with all nodes in one `std::vector`; longest common prefix; replace words;
  maximum XOR with a binary trie; a 20 000-operation differential test against
  `std::multiset<std::string>`.
- `code/m15_bit_tricks.cpp` — test/set/clear/toggle, lowest set bit, power of two, three
  popcounts, counting bits 0..n, single number I and II, missing number, reverse bits,
  subset masks, submask enumeration, Gray code, a `std::bitset` sieve, a dynamic bitset with
  `count`, `&=` and `next`, and bitmask-DP TSP.

### 3.1 Trie with counts (key parts)

```cpp
class Trie {
    struct Node {
        std::array<std::unique_ptr<Node>, 26> next{};
        int pass = 0;                     // words passing through this node
        int end = 0;                      // words ending here
    };
    Node root_;
public:
    void insert(std::string_view w) {
        Node* n = &root_;
        ++n->pass;
        for (char ch : w) {
            auto& child = n->next[ch - 'a'];
            if (!child) child = std::make_unique<Node>();
            n = child.get();
            ++n->pass;
        }
        ++n->end;
    }
    bool erase(std::string_view w) {
        if (!search(w)) return false;
        Node* n = &root_;
        --n->pass;
        for (char ch : w) {
            auto& child = n->next[ch - 'a'];
            if (--child->pass == 0) { child.reset(); return true; }   // frees the whole branch
            n = child.get();
        }
        --n->end;
        return true;
    }
    // search / starts_with / count_prefix all use the same walk(prefix) helper.
};
```

### 3.2 Index trie: one allocation, 4-byte links

```cpp
class IndexTrie {
    struct Node { std::array<std::int32_t, 26> next{}; bool end = false; };
    std::vector<Node> nodes_{1};                   // node 0 = root; 0 also means "no child"
public:
    void insert(std::string_view w) {
        std::int32_t n = 0;
        for (char ch : w) {
            std::int32_t c = nodes_[n].next[ch - 'a'];
            if (c == 0) {
                c = static_cast<std::int32_t>(nodes_.size());
                nodes_.emplace_back();             // may reallocate: do not hold a Node& across this
                nodes_[n].next[ch - 'a'] = c;
            }
            n = c;
        }
        nodes_[n].end = true;
    }
};
```

The comment on `emplace_back` is the bug everyone writes once: `Node& cur = nodes_[n];` then
`nodes_.emplace_back()` reallocates and `cur` dangles. Indices survive reallocation; references
and pointers do not.

### 3.3 Bit tricks (key parts)

```cpp
constexpr std::uint32_t clear_lowest(std::uint32_t x) { return x & (x - 1); }
constexpr bool is_pow2(std::uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

int popcount_kernighan(std::uint32_t x) { int c = 0; for (; x; x &= x - 1) ++c; return c; }

std::vector<int> count_bits(int n) {               // a one-line DP
    std::vector<int> b(n + 1, 0);
    for (int i = 1; i <= n; ++i) b[i] = b[i >> 1] + (i & 1);
    return b;
}

int single_number(const std::vector<int>& a) { int x = 0; for (int v : a) x ^= v; return x; }

int single_number_ii(const std::vector<int>& a) {  // others appear three times
    std::uint32_t result = 0;
    for (int b = 0; b < 32; ++b) {
        int cnt = 0;
        for (int v : a) cnt += static_cast<int>((static_cast<std::uint32_t>(v) >> b) & 1u);
        if (cnt % 3) result |= (1u << b);
    }
    return static_cast<int>(result);
}
```

### 3.4 Maximum XOR (key parts)

```cpp
std::uint32_t best = 0;
for (each x after the first) {
    std::int32_t n = 0; std::uint32_t cur = 0;
    for (int b = 31; b >= 0; --b) {
        int want = static_cast<int>(((x >> b) & 1u) ^ 1u);       // the opposite bit
        if (t[n][want] != 0) { cur |= (1u << b); n = t[n][want]; }
        else n = t[n][want ^ 1];
    }
    best = std::max(best, cur);
    add(x);
}
```

## 4. Under the hood (senior-level)

### 4.1 Signed shifts and undefined behaviour

In C++17: left-shifting a negative value, or shifting a 1 into or past the sign bit of a
signed type, is undefined behaviour; right-shifting a negative value is implementation-defined
(GCC does an arithmetic shift). Shifting by >= the width of the type is undefined for any type:
`1u << 32` is UB, and on x86 the hardware masks the count to 5 bits, so it often yields `1`, not
`0` — a classic silent bug. `1 << 31` with an `int` is UB in C++17. Rules: use unsigned types
(`std::uint32_t`, `std::uint64_t`), write `1u` or `std::uint64_t{1}`, and never shift by a
count that might equal the width. C++20 defines signed integers as two's complement and makes
more of this well-defined, but the shift-count rule remains.

Your C background helps here: these are the same rules as C. `-fsanitize=undefined` catches
bad shifts at run time.

### 4.2 What the compiler does with bit idioms

GCC and Clang recognise idioms: `x & (x - 1)` compiles to `BLSR` with BMI1
(`-mbmi`/`-march=haswell` or newer), `x & -x` to `BLSI`, `__builtin_ctz` to `TZCNT`/`BSF`,
popcount to `POPCNT`. Without `-march`, the default x86-64 baseline has no `POPCNT`, so
`__builtin_popcount` becomes a library call or a ~12-instruction bit-twiddling sequence.
For bit-heavy hot loops, compile with `-march=native` (or a known minimum like `x86-64-v2`,
which includes POPCNT) and check the assembly on Compiler Explorer.

### 4.3 Trie memory and cache behaviour

A pointer trie touches one new node per character, and each node is a separate allocation
somewhere on the heap: one likely cache miss per character on a cold trie. Lookup of a
10-character word in a large trie is ~10 misses — often slower than hashing the 10 bytes and
taking one or two misses in a hash table. Ways production systems fix this:

- **Index trie / arena**: nodes in one vector, 4-byte indices instead of 8-byte pointers
  (`IndexTrie` above). Nodes created together sit together.
- **Radix (Patricia) tree**: collapse chains of single-child nodes into one edge labelled with a
  string. The Linux kernel uses radix trees (now the XArray) for the page cache index;
  routing tables use compressed tries for longest-prefix match.
- **Adaptive radix tree (ART)**: nodes of size 4, 16, 48 or 256 chosen by fan-out; used in
  main-memory databases (HyPer, DuckDB's indexes).
- **Double-array tries and FSTs** (finite state transducers): read-only, extremely compact —
  Lucene stores its term dictionary as an FST.
- **Sorted array + binary search** for static dictionaries (2.2).

### 4.4 Bitmaps in the systems you know

- **PostgreSQL**: the *visibility map* and *free space map* are per-page bitmaps/maps; a *bitmap
  heap scan* (`EXPLAIN`: `Bitmap Index Scan` -> `Bitmap Heap Scan`) builds a bitmap of matching
  pages from one or more indexes, ANDs/ORs them (`BitmapAnd`, `BitmapOr`), and then reads the
  heap in physical order. The null bitmap in each tuple header has one bit per nullable column.
- **File systems**: ext4 tracks free blocks and inodes with bitmaps; finding a free block is
  "find first zero bit".
- **Linux**: `cpumask_t` is a bitset of CPUs; `find_first_bit` / `find_next_bit` use `ctz` per
  word — exactly `DynBitset::next` in the code.
- **Bloom filters**: LSM-tree databases (RocksDB, Cassandra, YugabyteDB's DocDB which is built
  on RocksDB) keep a Bloom filter per SSTable so a point lookup skips files that certainly do
  not contain the key. Your YugabyteDB target uses this on every read.

## 5. In production

- **Dedup of ids in a batch.** "Which of the 10 million account numbers appeared in today's
  file?" If ids are dense (or can be mapped to dense ranks), a bitset of 10 million bits is
  1.25 MB and fits in L2/L3 cache; `std::unordered_set<int>` of the same is hundreds of MB and
  orders of magnitude slower. Counting the overlap of two days is `popcount(a & b)`.
- **Flags in protocol headers.** Your RPC protocol probably has a flags byte. Define the bits as
  `constexpr std::uint8_t kCompressed = 1u << 0;` etc., test with `&`, never with `==` (other
  bits may be set), and document reserved bits as must-be-zero so future versions can use them.
- **Endianness.** Bit tricks are on values, not on bytes, so they are endian-independent; the
  moment you `memcpy` an integer into a network buffer, convert with `htonl`/`htobe64`. Mixing
  the two is a classic interop bug between a Windows client and a Linux server — both little
  endian, so it hides until something big-endian appears or someone byte-swaps twice.
- **Autocomplete for account search.** A teller types the first digits of an account number or
  letters of a name: a trie or a sorted array of keys with `lower_bound` gives the candidates.
  For a database-backed version, a B+tree index (module 11) already supports prefix queries
  via `LIKE 'abc%'` with the `text_pattern_ops` operator class in PostgreSQL.
- **Pitfall: `std::bitset<10000000>` on the stack.** 1.25 MB on a 1 MB Windows thread stack
  crashes. Make it `static`, a member of a heap-allocated object, or use a vector-backed bitset.

## 6. Try it

1. **(easy)** Implement `is_pow2`, `popcount_kernighan`, `lowest_set` and `clear_lowest` and
   check them against `std::bitset<32>::count()` on 10^6 random values. Done when all agree and
   you can say what `x & (x - 1)` does in one sentence.
2. **(easy)** Implement the 26-way trie with `insert`, `search`, `starts_with` without looking at
   the code. Done when `insert("apple"); search("app") == false; starts_with("app") == true;
   insert("app"); search("app") == true`.
3. **(medium)** Search suggestions: given a product list and a search word, return after each
   typed character up to three lexicographically smallest products with that prefix. Solve it
   with a trie and again with a sorted vector and `lower_bound`. Done when both agree on the
   LeetCode example (`mobile, mouse, moneypot, monitor, mousepad`, query `mouse`).
4. **(medium)** Single number III: two numbers appear once, every other twice; find both in O(n)
   time and O(1) space. Done when `[1,2,1,3,2,5] -> {3,5}` and you can explain why
   `diff & -diff` splits the array into two groups.
5. **(hard)** Word search II: find all dictionary words in a letter grid using a trie-guided
   DFS, removing words from the trie as they are found and pruning empty branches. Done when a
   12 x 12 grid with a 10 000-word dictionary runs in under 100 ms and the result equals
   searching each word separately.
6. **(hard)** Subset sum with a bitset: `std::bitset<100001> dp; dp[0] = 1; for x: dp |= dp << x;`.
   Done when it agrees with the boolean knapsack from module 14 on random inputs and you can
   explain the 64x speed-up.

## 7. Interview questions

**Q: When would you use a trie instead of a hash set?**
When you need prefix queries (autocomplete, count with prefix, shortest root), ordered iteration,
or to match many patterns while walking text once. A hash set is better for exact lookups only:
less memory and fewer cache misses. For a static dictionary, a sorted array with binary search
also handles prefix ranges well.

**Q: What are the time and space costs of a trie?**
Insert, search and prefix walk are O(L) in the word length, independent of how many words are
stored. Space is O(total characters x alphabet) with array children, which is large; maps or
sorted child lists, radix compression, or index-based arenas reduce it.

**Q: What does `x & (x - 1)` do, and where is it used?**
It clears the lowest set bit, because subtracting 1 flips that bit and all zeros below it. Uses:
power-of-two test (`x && !(x & (x - 1))`), Kernighan's popcount, and iterating set bits of a
mask.

**Q: Find the number that appears once when every other appears twice.**
XOR all of them: pairs cancel because `a ^ a = 0`, and XOR is associative and commutative.
O(n) time, O(1) space. For "every other appears three times", count each bit position mod 3.

**Q: Why should bit manipulation use unsigned types in C++?**
Shifting into or out of the sign bit of a signed integer is undefined or implementation-defined
in C++17, and `>>` on negative values is arithmetic in practice. Unsigned arithmetic is fully
defined (modulo 2^N). Shifting by the type width or more is undefined for both.

**Q: How do you find the maximum XOR of any two numbers in an array?**
Insert numbers bit by bit from the top into a binary trie; for each number walk greedily toward
the opposite bit at every level. A high bit outweighs all lower bits, so greedy is optimal.
O(32 n) time.

**Q: What is a bitset good for and what does `std::vector<bool>` get wrong?**
Dense sets of small integers: one bit per element and set operations a word at a time — sieves,
seen-sets, allocation bitmaps, Bloom filters, subset-sum DP. `std::vector<bool>` is bit-packed
but returns a proxy instead of `bool&`, so it does not behave like a normal container; prefer
`std::bitset`, `std::vector<char>`, or a dedicated dynamic bitset.

## 8. Recap

- Trie: one edge per character, end marker per node; insert/search/prefix in O(L).
- `pass` counts give count-with-prefix; `end` counts give multiset semantics and safe erase.
- Trie vs hash vs sorted array: prefix queries vs memory vs cache behaviour.
- Index-based tries (one vector, int32 links) beat pointer tries; never hold a reference across
  `emplace_back`.
- `x & (x - 1)` clears the lowest bit; `x & -x` isolates it; power of two = one bit set.
- Popcount: Kernighan, `__builtin_popcount`, `std::bitset::count`; compile with `-march` for POPCNT.
- XOR cancels pairs: single number, missing number.
- Masks enumerate subsets (n <= 20); bitmask DP is O(2^n x n^2).
- Unsigned types for bit work; never shift by >= the width.
- Bitsets: 64 flags per word; PostgreSQL bitmap scans, ext4 free maps, Bloom filters.
