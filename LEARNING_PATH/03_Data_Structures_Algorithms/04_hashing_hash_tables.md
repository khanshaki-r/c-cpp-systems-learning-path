# 04. Hashing and hash tables

**Time:** ~4 hours   **Prerequisites:** 01, 02, 03

## 1. In plain words

Your `array.cpp` finds the number 7 by looking at every box until it sees a 7. Your
`orderedArray.cpp` does better by keeping the boxes sorted and halving. A hash table does
something that sounds like cheating: it computes, from the value itself, *which box to look
in*. "7? That goes in box 7 mod 10 = box 7." One arithmetic step, one memory read. O(1).

The catch is collisions: 7 and 17 both want box 7. Every hash table design is an answer to
"what do we do when two keys want the same box". Chaining hangs a short linked list off the
box. Open addressing walks to the next free box. Both work; they have very different cache
behaviour, and knowing why is a senior-level distinction.

Think of a coat check. The attendant hashes your ticket number to a hook. If two coats land
on one hook, chaining means they hang together on that hook; open addressing means the second
coat goes on the next empty hook and the attendant knows to keep looking. Either way, you get
your coat in roughly constant time as long as there are enough hooks for the coats — that
ratio is the load factor, and keeping it low is what keeps the "roughly constant" honest.

## 2. The real thing

### 2.1 Vocabulary

- **Hash function** h(key) -> integer. Deterministic. Should spread keys uniformly.
- **Bucket** (slot): one position in the table array of size m. `index = h(key) % m`.
- **Collision**: two different keys map to the same index.
- **Load factor** alpha = n / m: number of stored elements per bucket on average.
- **Rehash**: allocate a bigger table (typically 2x), reinsert every element. O(n), but
  amortised O(1) per insert by the same argument as vector growth.

### 2.2 Chaining (separate chaining)

Each bucket is the head of a linked list of entries (key, value, next). Lookup: hash, walk the
chain comparing keys. Expected chain length is alpha. With alpha <= 1 that is O(1) expected.
Worst case: all keys in one bucket, O(n).

```
buckets[0] -> (16,"p") -> (32,"q") -> null
buckets[1] -> null
buckets[2] -> (18,"r") -> null
buckets[3] -> (3,"s") -> (11,"t") -> (19,"u") -> null    <- hash % 8
...
```

Pros: simple; deletion is trivial; tolerates alpha > 1; pointers/iterators to elements stay
valid across rehash (the node moves between buckets, it is not copied). Cons: one allocation
per element; each chain hop is a cache miss; memory overhead of a `next` pointer per entry.
`std::unordered_map` is specified such that only chaining satisfies its guarantees (stable
references, `bucket_count()`, `max_load_factor()` default 1.0).

### 2.3 Open addressing

All entries live in the table array itself. On collision, probe other slots in a fixed order
until a free one is found.

- **Linear probing**: slots i, i+1, i+2, ... Best cache behaviour (probes stay in one or two
  cache lines). Suffers from **primary clustering**: runs of occupied slots grow and merge.
- **Quadratic probing**: i, i+1, i+4, i+9, ... Less clustering, slightly worse locality.
- **Double hashing**: i, i+h2(k), i+2*h2(k), ... Best distribution, worst locality.

Deletion cannot simply empty a slot: a later key that probed past it would become
unreachable. Use a **tombstone** marker ("deleted, keep probing"), and count tombstones toward
the load factor so a table full of tombstones still rehashes. Alternatively, **backward-shift
deletion** (move later entries in the cluster back) avoids tombstones at the cost of a more
complex delete; Robin Hood hashing uses this.

Load factor must stay well below 1 (typically rehash at 0.5 to 0.875). Expected probes for a
successful search under linear probing ~ (1 + 1/(1-alpha)) / 2: at alpha = 0.5 about 1.5, at
alpha = 0.9 about 5.5. Beyond that it degrades sharply.

Pros: no per-element allocation, one contiguous array, excellent cache behaviour, 2 to 5x
faster than chaining in practice for small keys. Cons: rehash invalidates all references;
needs tombstones or backward shift; sensitive to a bad hash function (clustering).

Modern high-performance tables (Google's `absl::flat_hash_map`, Facebook's `F14`) are open
addressing with SIMD-accelerated probing over groups of 16 control bytes. If an interviewer
asks "how would you make `unordered_map` faster", that is the answer.

### 2.4 Hash functions for the table

For integers: `std::hash<int>` on libstdc++ is the **identity**. That is fine when the table
size is a prime (libstdc++ uses a prime bucket count) and keys are arbitrary; it is terrible
with power-of-two table sizes and keys that share low bits (e.g. all multiples of 8 — pointers,
aligned addresses). For your own open-addressing table with power-of-two size, mix the bits
first: a multiply by a large odd constant and a shift (Fibonacci hashing: `(k * 11400714819323198485ULL) >> (64 - log2(m))`), or splitmix64.

For strings: `std::hash<std::string>` (MurmurHash2 on libstdc++), FNV-1a (tiny to write), or
the polynomial hash from module 03. For composite keys (pair, struct): combine member hashes
with `seed ^= h(member) + 0x9e3779b9 + (seed << 6) + (seed >> 2)` (Boost's `hash_combine`) and
specialise `std::hash<YourType>` or pass a hasher as a template argument.

### 2.5 `std::unordered_map` and `std::unordered_set` pitfalls

```cpp
std::unordered_map<std::string, int> m;
m["key"];              // INSERTS a default-constructed 0 if absent. Use find() to test membership.
m.count("key");        // 0 or 1; cheap membership test
auto it = m.find("k"); // end() if absent; no insert
m.try_emplace("k", 5); // insert only if absent; does not construct value if present (C++17)
m.insert_or_assign("k", 5);
m.erase("k");
m.reserve(n);          // pre-size buckets for n elements: avoids rehashes
m.load_factor(); m.bucket_count(); m.max_load_factor(0.7f);
for (auto& [k, v] : m) ...   // iteration order is unspecified and changes after rehash
```

Pitfalls to know:
1. `operator[]` inserts. In a loop that checks membership this silently grows the map.
2. Iteration order is unspecified. Code that depends on it breaks on rehash or a different
   library. Tests that print a map and compare output are flaky by construction.
3. Iterating is slow: it walks bucket by bucket through a node list; each node is a cache
   miss. `std::unordered_map` with 1M entries iterates ~10x slower than a vector of pairs.
4. Erasing during iteration: `it = m.erase(it)` is fine; anything else is UB.
5. Rehash invalidates **iterators** but not pointers/references to elements (chaining).
6. No `std::hash` for `std::pair` or your structs: provide one.
7. The worst case is O(n). If the keys come from an attacker, see module 03 hash flooding.
8. `max_load_factor` default is 1.0; libstdc++ rehashes when size exceeds bucket_count.
   Lowering it trades memory for shorter chains.

When to use `std::map` instead: you need sorted iteration, range queries, `lower_bound`,
or predictable worst-case O(log n). When to use a sorted `std::vector` instead: the data is
built once and queried many times (binary search, best cache behaviour, smallest memory).

### 2.6 Designing with hashes: the three questions

1. **What is the key?** Exactly the thing you want O(1) lookup on. Normalise it (sorted
   string for anagrams, `prefix % k` for divisibility, `(x, y)` packed into one `int64`).
2. **What is the value?** Count, first index, last index, list of indices, a node pointer
   (module 05 LRU).
3. **What do I look up?** `target - x`, `prefix - k`, `x - 1` and `x + 1`, `pattern ->
   canonical form`.

Most hash-map interview problems are solved by answering those three questions out loud.

## 3. Code

Files: `code/m04_hash_map_chaining.cpp` (a complete chained hash map with resize, templated
on key/value with a hasher), `code/m04_hash_map_open_addressing.cpp` (linear probing with
tombstones and Fibonacci hashing), `code/m04_hash_problems.cpp` (the six problems).

### Your own chained hash map (key parts)

```cpp
template <typename K, typename V, typename Hash = std::hash<K>>
class HashMap {
    struct Node { K key; V value; Node* next; };
    std::vector<Node*> buckets_;
    std::size_t size_ = 0;
    Hash hash_;

    std::size_t index_of(const K& k) const { return hash_(k) % buckets_.size(); }

    void rehash(std::size_t new_bucket_count) {
        std::vector<Node*> nb(new_bucket_count, nullptr);
        for (Node* head : buckets_)            // move every node; no reallocation of nodes
            while (head) {
                Node* next = head->next;
                std::size_t i = hash_(head->key) % new_bucket_count;
                head->next = nb[i]; nb[i] = head;   // push front into the new bucket
                head = next;
            }
        buckets_.swap(nb);
    }
public:
    V* find(const K& k) {
        for (Node* p = buckets_[index_of(k)]; p; p = p->next)
            if (p->key == k) return &p->value;
        return nullptr;
    }
    bool insert(const K& k, const V& v) {      // returns false if key existed (value updated)
        if (V* existing = find(k)) { *existing = v; return false; }
        if (size_ + 1 > buckets_.size() * MAX_LOAD) rehash(buckets_.size() * 2);
        std::size_t i = index_of(k);
        buckets_[i] = new Node{k, v, buckets_[i]};
        ++size_;
        return true;
    }
    bool erase(const K& k) {
        Node** pp = &buckets_[index_of(k)];     // pointer-to-pointer: no special case for head
        while (*pp) {
            if ((*pp)->key == k) { Node* dead = *pp; *pp = dead->next; delete dead; --size_; return true; }
            pp = &(*pp)->next;
        }
        return false;
    }
};
```

The `Node**` trick in `erase` is the same one you use in C to unlink from a singly linked list
without a "previous" variable. The full file adds a destructor, move semantics, `operator[]`
and iteration.

### Open addressing with linear probing (key parts)

```cpp
enum class State : std::uint8_t { Empty, Full, Tombstone };
struct Slot { K key; V value; State state = State::Empty; };
std::vector<Slot> slots_;              // size is a power of two
std::size_t mask_;                     // slots_.size() - 1

std::size_t probe_start(const K& k) const {
    std::uint64_t h = hash_(k) * 0x9E3779B97F4A7C15ULL;   // Fibonacci mixing
    return static_cast<std::size_t>(h >> (64 - log2_size_)); // take the HIGH bits
}
V* find(const K& k) {
    for (std::size_t i = probe_start(k);; i = (i + 1) & mask_) {
        if (slots_[i].state == State::Empty) return nullptr;       // hit a hole: not here
        if (slots_[i].state == State::Full && slots_[i].key == k) return &slots_[i].value;
        // Tombstone or other key: keep probing
    }
}
```

### The six problems (`m04_hash_problems.cpp`)

1. **Two Sum (unsorted)** — map value -> index; for each x look up `target - x` *before*
   inserting x (handles `x + x == target` with distinct indices). O(n), O(n).
2. **Longest consecutive sequence** — put all values in a set; for each x that has no `x - 1`
   (it is a run start), count upward. Each value is visited O(1) times amortised. O(n).
3. **Group anagrams** — key = 26-count encoded as a string (avoids sorting). O(total chars).
4. **First unique character** — count array of 256, then second pass. O(n), O(1).
5. **Top-k frequent elements** — count map, then bucket by frequency (array of vectors indexed
   by count) and read from the top. O(n). The heap version (module 12) is O(n log k).
6. **LRU cache** is in module 05 (it needs a list); here instead: **contains duplicate within
   k** — sliding window set of the last k values. O(n), O(k).

## 4. Under the hood (senior-level)

### 4.1 libstdc++ `unordered_map` layout

A singly linked list of **all** nodes, plus a bucket array of pointers into that list. Each
bucket pointer points to the node *before* the first node of that bucket (so insertion into a
bucket can splice without a second pointer). Each node holds the key-value pair, a `next`
pointer, and (for keys whose hash is not "fast", e.g. strings) a cached hash code so rehash
does not recompute it. Node size for `<int,int>`: 16 bytes payload + 8 next = 24, rounded up
by malloc to 32. Bucket count is the next prime above `size / max_load_factor`. Iteration
walks the single node list — so it is O(n) regardless of bucket count, but every step is a
pointer chase.

Rehash: new bucket array of the next prime size, walk the node list, relink. Nodes are not
reallocated, which is why references stay valid.

### 4.2 The cost of a lookup, in cache misses

Chained `unordered_map<int,int>`: hash (free for int) -> read bucket pointer (miss 1) -> read
node (miss 2) -> compare -> done. Two dependent cache misses ~ 200 ns on a cold cache.

Linear-probing `flat_hash_map<int,int>`: hash with mixing (~2 ns) -> read slot (miss 1) ->
compare; a second probe is usually in the same cache line. One miss ~ 100 ns.

A sorted `vector<int>` of 1M entries: binary search touches ~20 cache lines, but the first ~10
are the same lines every time and stay in L1/L2. Measured on typical hardware the three are
within 2 to 3x of each other for point lookups at 1M elements; the flat table wins, the
chained table loses. For 1000 elements the sorted vector often wins outright.

### 4.3 Why prime bucket counts

With `index = h % m`, if m is a power of two the index is just the low bits of h. Keys that
differ only in high bits (multiples of 1024, pointers to 64-byte-aligned objects) all collide.
A prime m makes every bit of h matter. The cost is an integer division (~20 to 40 cycles) on
every operation. Power-of-two tables avoid the division (`h & (m-1)`) but must mix the hash
first — that is the Fibonacci multiply in the open-addressing file.

### 4.4 Amortised resize and latency spikes

Doubling on load factor gives amortised O(1) but a single insert can take O(n) when it
triggers the rehash. For a 10M-entry map that is tens of milliseconds — a latency spike your
monitoring will see. Mitigations: `reserve` to the expected size at startup; incremental
rehash (keep old and new tables, move a few buckets per operation — Redis does this); or
accept the spike and do it off the hot path.

### 4.5 Database hashing

PostgreSQL hash indexes (reliable since version 10) bucket keys by `hash(key) % nbuckets` and
grow by **linear hashing**: buckets split one at a time rather than all at once, so there is no
O(n) rehash. Hash joins build an in-memory hash table on the smaller relation and probe it
with the larger one; when it does not fit in `work_mem` it partitions both sides to disk
("grace hash join"). Berkeley DB's `DB_HASH` access method uses extended linear hashing for the
same reason. Your `unordered_map` and these are the same idea at different scales; the
database ones care about not stalling for a full rehash.

## 5. In production

- **Session lookup in the RPC server.** Session id -> `Session*`. `unordered_map` is right.
  Reserve to the max connection count at startup so no rehash happens under load. Guard it
  with a mutex or shard it by `id % nshards` with one mutex per shard (see 04_Concurrency);
  the shard index is itself a hash.
- **Pointer keys.** `unordered_map<Connection*, ...>`: the identity hash on an 8- or 16-byte
  aligned pointer with a prime bucket count is fine; with a power-of-two custom table it is a
  disaster (every pointer ends in 000 or 0000). Mix the bits.
- **Account numbers.** Store as `std::uint64_t`, not `std::string`. 8 bytes vs 32, one
  multiply vs 16 byte reads, and no SSO edge cases.
- **Memory.** Each `unordered_map<int,int>` entry costs ~32 bytes of node plus ~8 bytes of
  bucket pointer: 40 bytes for 8 bytes of data. 10M entries = 400 MB. A flat table at 0.5 load
  costs 16 bytes per entry. A sorted vector costs 8. Choose consciously.
- **Debugging a slow map.** Symptoms: lookups take microseconds, CPU in `_M_find_before_node`.
  Check `load_factor()` and the chain lengths (`bucket_size(i)` for a sample of buckets). If a
  few buckets are huge, the hash is bad for your key distribution — e.g. hashing only the
  first 8 bytes of a key that always starts with the same prefix.
- **War story (typical).** A cache keyed by `std::string` request path was iterated every
  second to expire entries; with 2M entries the sweep took 300 ms and stalled request
  handling. Fix: keep a separate time-ordered structure (a list, module 05) for expiry so the
  sweep touches only what is expiring, and never iterate the hash map on the hot path.

## 6. Try it

1. **(easy)** Using `std::unordered_map`, count word frequencies in a text file and print the
   10 most frequent. Done when it runs on a 10 MB file in under a second and never uses
   `operator[]` for lookups that should not insert.
2. **(easy)** Write `struct Point { int x, y; }` with a `std::hash<Point>` specialisation using
   `hash_combine`, and store 1M random points in an `unordered_set<Point>`. Done when
   `bucket_count()` and `load_factor()` printed after insertion look healthy (load <= 1).
3. **(medium)** Add `operator[]`, a destructor, move constructor and `size()` to the chained
   `HashMap` in `m04_hash_map_chaining.cpp` if you cover them yourself without reading the
   file, then compare with the file. Done when the file's test harness passes with your version.
4. **(medium)** Implement "subarray sums divisible by k" using prefix sums mod k and a count
   map. Done when `([4,5,0,-2,-3,1], 5) -> 7`. Handle negative remainders.
5. **(hard)** Add backward-shift deletion to the linear-probing table (no tombstones). Done
   when 1M random insert/erase operations produce the same contents as a `std::unordered_map`
   mirror, and `find` never reads a tombstone state because there are none.
6. **(hard)** Benchmark point lookups at n = 1 000, 100 000, 10 000 000 for: `std::unordered_map`,
   your open-addressing map, and `std::lower_bound` on a sorted vector. Done when you have a
   table of ns/lookup and can explain each crossover in terms of cache lines touched.

## 7. Interview questions

**Q: How does a hash table achieve O(1) lookup?**
Compute an index from the key with a hash function and go straight to that bucket. With a
good hash and bounded load factor, the expected number of keys examined is constant.
Worst case is O(n) when many keys collide.

**Q: Chaining vs open addressing — trade-offs?**
Chaining: one node per element, simple deletion, tolerates load > 1, stable references, poor
cache locality. Open addressing: everything in one array, excellent locality, no per-element
allocation, needs tombstones or backward shift for delete, must keep load below ~0.8, rehash
invalidates everything. Flat tables are faster for small keys; `std::unordered_map` is chained
because the standard requires reference stability.

**Q: What is load factor and what happens when it grows?**
Elements per bucket, n/m. For chaining, expected chain length is the load factor. For open
addressing, expected probes grow as 1/(1-alpha) and explode near 1. Tables rehash to a bigger
array when a threshold is crossed; that is O(n) once and amortised O(1).

**Q: Why does `m[key]` in a membership check cause bugs?**
`operator[]` inserts a default-constructed value when the key is absent. The map grows,
memory leaks logically, and later `count()`/iteration see phantom entries. Use `find` or
`count`.

**Q: Why is iterating an `unordered_map` slow?**
Elements are individually allocated nodes linked in a list; each step is a pointer chase to
an unpredictable address, i.e. a cache miss. Order is also unspecified. If you iterate a lot,
keep a vector alongside or use a flat table.

**Q: How would you hash a struct with two ints?**
Specialise `std::hash` (or pass a hasher functor): combine the member hashes with
`seed ^= h(x) + 0x9e3779b9 + (seed << 6) + (seed >> 2)`, or pack both into a 64-bit value and
mix it. Never just XOR them — `(a, b)` and `(b, a)` would collide.

**Q: An attacker controls the keys. What can go wrong?**
Hash flooding: chosen collisions push every operation to O(n). Mitigate with a keyed/seeded
hash per process, by bounding input, or by using a structure with guaranteed bounds
(`std::map`) for untrusted keys.

**Q: How does PostgreSQL avoid an O(n) rehash on its hash index?**
Linear hashing: it splits one bucket at a time as the index grows, keeping two active
hash-range sizes, so growth is spread across inserts instead of stalling on one.

## 8. Recap

- A hash table turns the key into a bucket index; collisions are the whole design problem.
- Chaining: list per bucket, stable references, cache-unfriendly; this is `std::unordered_map`.
- Open addressing: probe sequence in one array, cache-friendly, needs tombstones or backward
  shift, keep load factor below ~0.8; this is `absl::flat_hash_map`.
- Load factor alpha = n/m; rehash doubles the table; amortised O(1) but a single insert can
  stall — `reserve` at startup.
- Prime bucket counts tolerate a weak hash; power-of-two tables need bit mixing first.
- `operator[]` inserts; iteration order is unspecified and iteration is slow; provide
  `std::hash` for your own key types with `hash_combine`.
- Three design questions: what is the key, what is the value, what do I look up.
- Two cache misses per chained lookup versus one for a flat table; sorted vector can win at
  small n.
- Databases use linear hashing to grow without a stop-the-world rehash.
- Untrusted keys mean hash flooding is possible; know the mitigation.
