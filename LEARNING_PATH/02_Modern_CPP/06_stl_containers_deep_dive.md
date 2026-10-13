# 6. STL containers deep dive

**Time:** ~3 hours   **Prerequisites:** 04, 05

## 1. In plain words

The standard library gives you the data structures you kept re-writing in C: a growable array
(`vector`), a double-ended queue (`deque`), a linked list (`list`), a balanced tree (`map`, `set`),
and a hash table (`unordered_map`, `unordered_set`). Each one is a template, so it works for any
element type, and each one owns its elements (RAII), so you never free them by hand.

The real skill is not knowing that `map` exists. It is knowing what each container does to memory,
which operations invalidate your pointers and iterators, and why `vector` wins most benchmarks even
when the textbook says `list` should. This module is about the mechanics behind the interface.

Analogy: `vector` is a bookshelf (books side by side, fast to scan, painful to insert in the middle);
`list` is a treasure hunt (each clue points to the next, easy to insert a clue, slow to reach the
tenth); `map` is a library catalogue sorted by title; `unordered_map` is a coat check with numbered hooks.

## 2. The real thing

### 2.1 `std::vector<T>`: the default container

Contiguous array on the heap, three pointers in the object (`begin`, `end`, `capacity_end`).

- `size()` = elements, `capacity()` = allocated slots. `push_back` is amortised O(1): when
  `size == capacity`, it allocates a bigger block (libstdc++: doubles; MSVC: 1.5x), moves (or copies)
  all elements over, frees the old block.
- **Reallocation invalidates everything**: every pointer, reference and iterator into the old buffer.
- `reserve(n)` allocates up front; `shrink_to_fit()` is a request; `clear()` keeps capacity.
- `insert`/`erase` in the middle shift elements: O(n). Iterators at or after the point are invalidated
  (`erase` returns the next valid one).
- `emplace_back(args...)` constructs in place; `push_back(T&&)` moves in.
- `std::vector<bool>` is a bit-packed specialisation: `operator[]` returns a proxy, not `bool&`.
  Use `std::vector<char>` or `std::bitset` when you need real references.
- `data()` gives the raw `T*` for C APIs.

### 2.2 `std::array<T, N>`

A C array with a container interface. Size fixed at compile time, lives wherever you declare it (stack
or inside another object), no heap, zero overhead. Use it instead of `T arr[N]` to get `size()`,
bounds-checked `at()`, iterators, and copy semantics.

### 2.3 `std::deque<T>`

Blocks of fixed size (libstdc++: 512 bytes per block) indexed by a small array of block pointers.
O(1) `push_front` and `push_back`, O(1) indexing with two dereferences, and **pointers/references
stay valid on push/pop at either end** (iterators do not). Good as a FIFO queue; `std::queue` uses
it by default.

### 2.4 `std::list<T>` and `std::forward_list<T>`

Doubly (or singly) linked nodes, one heap allocation per element. O(1) insert/erase anywhere given an
iterator, and insert/erase **never invalidate other iterators**. The price: each node is
`sizeof(T) + 16` bytes plus allocator overhead, traversal is a pointer chase with a cache miss per
node, and there is no indexing. It is rarely the fastest choice; it is the right choice when
iterator stability matters (an intrusive LRU list, a registry where handles must stay valid).

### 2.5 `std::string` and `std::string_view`

`std::string` is `std::vector<char>` plus a null terminator and the small-string optimisation (SSO):
libstdc++ stores up to 15 characters inside the 32-byte object with no heap allocation. Beyond that,
heap. `c_str()` is always null-terminated.

`std::string_view` (C++17) is a `(const char*, size_t)` pair: a non-owning view of any character
range. Pass it instead of `const std::string&` so callers with a literal, a `char*`, or a substring
do not allocate. It does not own and is not null-terminated; never let it outlive the string it views
and never pass `.data()` to a C API expecting a C string (module 13).

### 2.6 `std::map<K, V>` and `std::set<K>`

Red-black trees: self-balancing binary search trees with O(log n) find/insert/erase, elements sorted
by `K`'s `operator<` (or a comparator). Each element is a separately allocated node (libstdc++: 32
bytes of header: colour, parent, left, right pointers; then the `pair<const K, V>`).

- **Iterators and references are never invalidated** by insert, and by erase only for the erased
  element.
- `operator[]` inserts a default-constructed value if the key is absent (and needs `V` to be
  default-constructible). Use `find`, `count`, `at`, or `try_emplace` when you do not want that.
- `lower_bound`/`upper_bound`/`equal_range` give O(log n) range queries: the reason to pick `map`
  over `unordered_map`.
- `multimap`/`multiset` allow duplicates.

### 2.7 `std::unordered_map<K, V>` and `std::unordered_set<K>`

Hash tables with separate chaining: an array of buckets, each a singly linked list of nodes.
Average O(1) find/insert/erase, worst case O(n) when many keys collide.

- `load_factor() = size() / bucket_count()`. When it would exceed `max_load_factor()` (default 1.0)
  on insert, the table **rehashes**: allocates more buckets (libstdc++ picks the next prime) and
  re-links every node.
- **Rehash invalidates all iterators but not pointers/references** to elements (the nodes themselves
  do not move).
- `reserve(n)` sizes the bucket array for `n` elements with no rehash on the way.
- Iteration order is unspecified and changes after rehash. Never depend on it.
- You need `std::hash<K>` and `operator==` for `K`. For your own key type, specialise `std::hash` or
  pass a hasher type.
- No heterogeneous lookup in C++17: `find` with a `string_view` key forces a `std::string`
  construction. (C++20 adds `is_transparent` support.)

### 2.8 Which container?

```
Need sorted order or range queries?           -> map / set
Need stable iterators across inserts?         -> list, or map/unordered_map (node-based)
Need O(1) at both ends?                       -> deque
Fixed size known at compile time?             -> array
Key lookup, no ordering needed?               -> unordered_map (reserve it!)
Everything else, especially < 10k elements    -> vector (sorted vector + binary search beats map
                                                 for read-mostly data)
```

Measure before deviating from `vector`. Linear scan over 100 contiguous ints is faster than a hash
lookup; a sorted `vector<pair<K,V>>` with `lower_bound` is usually faster than `map` for read-heavy
tables because of cache locality.

### 2.9 Iterator invalidation summary

| Container | insert / push | erase |
|---|---|---|
| vector | all, if reallocation; else at/after the point | at/after the point |
| deque | all iterators; pointers/refs survive if at the ends | all, except at the ends only the erased |
| list, forward_list | none | only the erased |
| map, set | none | only the erased |
| unordered_map/set | all iterators if rehash; pointers/refs never | only the erased |
| string | like vector | like vector |

## 3. Code

`code/m06_vector_growth.cpp` prints every reallocation and shows the safe erase/insert idioms:

```cpp
std::vector<int> v;
std::size_t last_cap = v.capacity();
for (int i = 0; i < 100; ++i) {
    v.push_back(i);
    if (v.capacity() != last_cap) {               // 1, 2, 4, 8, 16, 32, 64, 128 on libstdc++
        std::cout << "size=" << v.size() << " capacity " << last_cap << " -> " << v.capacity() << '\n';
        last_cap = v.capacity();
    }
}

for (auto it = nums.begin(); it != nums.end();) {   // erase while iterating: use the returned iterator
    if (*it < 0) it = nums.erase(it);
    else ++it;
}
```

`code/m06_map_vs_unordered.cpp` shows ordering, `operator[]` inserting, and rehash points:

```cpp
int z = ordered["zulu"];       // inserts "zulu" -> 0 silently; size grows
if (auto it = hashed.find("zulu"); it == hashed.end()) std::cout << "find() does not insert\n";

std::unordered_map<int, int> h;
for (int i = 0; i < 2000; ++i) {
    h[i] = i;
    if (h.bucket_count() != last_buckets) { /* print rehash: 1 -> 13 -> 29 -> 59 -> 127 -> 257 -> 541 -> 1109 -> 2357 */ }
}
std::unordered_map<int, int> pre;
pre.reserve(2000);              // bucket array sized once; no rehash during the inserts

auto lb = ordered.lower_bound("c");   // only ordered containers can do this
```

## 4. Under the hood (senior-level)

**Why vector beats list even for "list-friendly" workloads.** A modern core fetches 64-byte cache
lines and prefetches sequential ones. Walking a `vector<int>` touches one new line every 16 elements
and the prefetcher stays ahead; walking a `list<int>` touches a new, unpredictable line every
element: a cache miss (roughly 100 ns from DRAM, typical) per node. Inserting in the middle of a
vector of 10,000 ints memmoves on average 20 KB, which takes about the same time as a handful of
cache misses. Bjarne Stroustrup's often-cited benchmark shows vector winning insert/erase-in-the-middle
up to around 100k small elements. `m12_bench_basics.cpp` lets you measure it.

**Growth factor and amortisation.** Doubling gives amortised O(1) `push_back`: total copies for n
pushes are n + n/2 + n/4 + ... < 2n. A growth factor of 1.5 wastes less memory and lets the allocator
reuse freed blocks (1 + 1.5 < 1.5^2 so old blocks can be coalesced for the next one); 2.0 never can.
libstdc++ chose 2 for simplicity and speed.

**Exception safety during reallocation.** `vector` uses `std::move_if_noexcept`: if `T`'s move
constructor is `noexcept`, elements are moved; otherwise copied, so that if a copy throws the old
buffer is intact (module 04). For trivially copyable `T` it is a `memcpy`/`memmove`.

**Allocator and `operator new`.** By default every container allocates through `std::allocator<T>`,
which calls `::operator new`, which calls `malloc`. glibc `malloc` adds a 16-byte header per block
and rounds up to 16 bytes, so a `list<int>` node (24 bytes of data) costs 32 or 48 bytes. A million
`map` nodes is about 64 MB of allocator traffic and a million small `malloc` calls. Module 12 shows
how `std::pmr` and arenas remove that.

**Red-black tree invariants.** Every node is red or black; root is black; red nodes have black
children; every root-to-leaf path has the same number of black nodes. This bounds height at
2*log2(n+1), so `find` is at most about 40 comparisons for a million elements. Insert and erase do
O(1) rotations amortised. libstdc++ also keeps a header node with leftmost/rightmost cached, so
`begin()` and `rbegin()` are O(1).

**Hash table layout (libstdc++).** A single forward list of all nodes plus a bucket array of pointers
into that list ("before-begin" pointers). Iteration walks the single list, so iterating an
`unordered_map` is as fast as a `forward_list`, independent of bucket count. Each node stores the
cached hash when the hash is not fast to recompute (it does for `std::string` keys), making rehash
cheap: no re-hashing of strings, just re-linking. Bucket counts are primes to spread poor hashes.

**Hash quality and DoS.** `std::hash<int>` is the identity on libstdc++. Keys that are multiples of
the bucket count all collide. For attacker-controlled keys (HTTP headers, JSON fields) use a seeded
hash or a different container; a hash-flooding attack turns O(1) into O(n) per lookup.

**`std::string` SSO layout (libstdc++, new ABI).** 8-byte pointer, 8-byte size, 16-byte union of
either capacity or the inline buffer (15 chars + null). Moving a long string steals the pointer;
moving a short string copies the 16 bytes and the pointer is re-pointed into the destination. So a
"moved" short string is a full copy and the source stays valid and unchanged.

## 5. In production

- **Reserve when you know the size.** Reading 100k rows into a `vector` without `reserve` performs
  about 17 reallocations and moves 200k elements. `reserve(row_count)` makes it one allocation. The
  same applies to `unordered_map::reserve` and `std::string::reserve`.
- **The iterator-invalidation bug.** `for (auto& x : v) if (cond) v.push_back(...)` is UB on
  reallocation and may "work" for months. ASan catches it (`heap-use-after-free` on the old buffer).
  `-D_GLIBCXX_DEBUG` builds also abort with a clear message; use it in your test builds.
- **`map::operator[]` in a read path** silently fills the map with default entries and holds a write
  lock where a read lock would do. Use `find`.
- **Keys that outlive views.** A `std::unordered_map<std::string_view, V>` whose keys point into a
  request buffer that is reused is a dangling-view bug that shows as corrupted keys.
- **Erasing from `unordered_map` while iterating** is fine with `it = m.erase(it)`; erasing a
  different element invalidates only that element's iterator. Inserting while iterating may rehash
  and invalidate everything.
- **Memory never comes back.** `vector::clear()` keeps the capacity; a vector that once held a 100 MB
  batch holds 100 MB forever. Use `v = {}` or the swap-with-empty idiom (`std::vector<T>().swap(v)`)
  to actually release.
- **3 AM debugging.** A server grows to 8 GB RSS over a week. `pmap`/`heaptrack` shows the growth is
  in `std::_Rb_tree_node` allocations: a `std::map` used as a cache with no eviction. Or in
  `_Hashtable` buckets: an `unordered_map` with `max_load_factor` lowered to 0.1 "for speed" and 10x
  the memory. Know what the mangled container internals look like in a heap profile.

## 6. Try it

1. **(easy)** Build `m06_vector_growth.cpp`. Change the element type to a struct with a non-noexcept
   move constructor that prints, and observe that growth now copies. Done when you see COPY lines at
   every reallocation and can state the rule.
2. **(easy)** Write a program that inserts 1M `int`s into `std::set` and into a `std::vector` that
   is then sorted. Time both, then time 1M `find`/`binary_search` lookups. Done when you have numbers
   and an explanation involving cache lines.
3. **(medium)** Implement `struct AccountKey { std::string branch; int number; }` as a key for
   `std::unordered_map` (write `operator==` and a `std::hash` specialisation combining both fields) and
   for `std::map` (write `operator<`). Done when both containers compile and find the same entries.
4. **(medium)** Write an LRU cache with `std::list<std::pair<K,V>>` plus
   `std::unordered_map<K, list::iterator>`. Explain why the iterator stored in the map stays valid when
   you splice the node to the front. Done when `get` promotes and `put` evicts correctly in a test.
5. **(hard)** Build with `-D_GLIBCXX_DEBUG` and write three deliberate invalidation bugs (vector
   push during range-for, map iterator used after erase, unordered_map iterator used after rehash).
   Done when each one aborts with a diagnostic you can explain, and you have run the same binary
   under ASan to compare the reports.
6. **(hard)** Benchmark `std::map<int,int>` vs a sorted `std::vector<std::pair<int,int>>` with
   `lower_bound` for 1k, 100k and 10M elements, read-only lookups. Done when you can state the
   crossover point on your machine and explain it with node size and cache misses.

## 7. Interview questions

**Q: What happens inside `std::vector::push_back` when size equals capacity?**
It allocates a new buffer (twice the capacity on libstdc++), moves the existing elements if `T`'s
move constructor is `noexcept` (otherwise copies), constructs the new element, destroys the old
elements and frees the old buffer. All iterators, pointers and references into the old buffer are
invalidated.

**Q: Which containers keep pointers to elements valid across insertions?**
Node-based ones: `list`, `forward_list`, `map`, `set`, `unordered_map`, `unordered_set` (pointers
and references survive rehash; iterators do not). `deque` keeps pointers valid for push/pop at the ends
only. `vector` and `string` invalidate on reallocation.

**Q: Why is `std::vector` often faster than `std::list` even for insertions in the middle?**
Memory locality. The vector's elements share cache lines and the prefetcher streams them, so shifting
thousands of small elements costs less than the cache misses of walking the list to the insertion point.
`list` wins only with very large elements or when iterator stability is required.

**Q: What is the load factor and what happens on rehash?**
`size() / bucket_count()`. When an insert would push it over `max_load_factor()` (1.0 by default),
the table allocates a larger prime-sized bucket array and relinks every node. Iterators are invalidated;
element pointers and references are not, because the nodes do not move. `reserve(n)` avoids rehashes.

**Q: What is wrong with `if (map[key] == x)` in a read path?**
`operator[]` inserts a default-constructed value when the key is missing, so the lookup mutates the
map, needs a write lock under concurrency, and leaves junk entries behind. Use `find` or `count`.

**Q: `std::map` vs `std::unordered_map`: when do you pick the tree?**
When you need ordered iteration or range queries (`lower_bound`), when keys have no good hash,
when worst-case latency matters more than average (no rehash spikes, no hash flooding), or when you
need iterators that survive inserts. Otherwise the hash table is usually faster for point lookups.

**Q: What is the small-string optimisation?**
`std::string` keeps short contents (15 bytes on libstdc++) inside the object instead of the heap, so
short strings cost no allocation. Consequence: moving a short string copies bytes rather than stealing
a pointer, and `sizeof(std::string)` is 32 even when empty.

**Q: How do you erase elements while iterating a vector?**
Either `it = v.erase(it)` in a manual loop (never `++it` after erase), or the erase-remove idiom
`v.erase(std::remove_if(v.begin(), v.end(), pred), v.end())`, which is O(n) instead of O(n^2).

## 8. Recap

- `vector` is the default; it is contiguous, cache-friendly, and reallocation invalidates everything.
- `reserve` when the size is known; `clear` keeps capacity; `shrink_to_fit` is only a hint.
- Node containers (`list`, `map`, `unordered_map`) keep element addresses stable; `vector`/`deque` do not.
- `map` = red-black tree, O(log n), sorted, range queries; `unordered_map` = chained hash table,
  O(1) average, rehash on load factor, no ordering.
- `operator[]` inserts; use `find` for lookups.
- `string` has SSO (15 chars); `string_view` is non-owning and not null-terminated.
- Every container allocates through `operator new`/`malloc`; node containers pay per element.
- Know the invalidation table; `-D_GLIBCXX_DEBUG` and ASan catch violations.
- Measure: sorted `vector` + `lower_bound` frequently beats `map` for read-mostly data.
