# 11. Balanced trees, B-trees and disk

**Time:** ~6 hours   **Prerequisites:** 09, 10

## 1. In plain words

Module 10 ended with the problem: a BST is only fast if it is balanced, and real key streams
(sequence numbers, timestamps, sorted batch loads) make it a linked list. Self-balancing trees
fix this by doing a little extra work on every insert and delete — a **rotation**, which is a
local re-wiring of three nodes that lowers one side and raises the other — so the height stays
within a constant factor of log2(n). AVL trees keep the two subtrees of every node within one
level of each other. Red-black trees use a looser rule that needs fewer rotations; that is what
`std::map` is.

The B-tree answers a different question: what if each step down the tree costs not a
nanosecond but a disk read of 100 microseconds, or at least a cache miss of 100 nanoseconds?
Then you want each step to eliminate as much as possible. A B-tree node is a whole disk page
holding hundreds of keys; one page read narrows the search by a factor of hundreds instead of
two. A billion keys fit in a tree of height 3 or 4. Every relational database index you have
ever used — PostgreSQL's `CREATE INDEX`, Berkeley DB's `DB_BTREE`, SQLite, MySQL InnoDB — is a
B-tree variant called a B+tree, where all the data sits in the leaves and the leaves are chained
together so that a **cursor** can walk a range without going back up.

Your resume says "B-tree cursor traversal" and "indexing". The migration engine you worked on
opened a Berkeley DB cursor and walked it with `DB_NEXT`: that is a leaf-chain traversal. This
module gives you the vocabulary and the mechanics to explain exactly what happened underneath,
and an in-memory B+tree you can write from memory.

## 2. The real thing

### 2.1 Rotations

A rotation changes the shape of a BST without changing its in-order sequence.

```
Right rotation at y            Left rotation at x
      y                x              x                    y
     / \              / \            / \                  / \
    x   C    ->      A   y          A   y      ->        x   C
   / \                  / \            / \              / \
  A   B                B   C          B   C            A   B
```

In-order is `A x B y C` before and after. Right rotation makes `x` the parent of `y`; `x`'s old
right subtree `B` becomes `y`'s left. Three pointer writes (plus parent pointers if you keep
them). O(1). With `unique_ptr` the rotation is a sequence of `std::move`s on the owning links:

```cpp
// right rotation: `link` owns y; afterwards it owns x.
std::unique_ptr<Node> y = std::move(link);
std::unique_ptr<Node> x = std::move(y->left);
y->left = std::move(x->right);     // B moves under y
x->right = std::move(y);           // y goes under x
link = std::move(x);
```

### 2.2 AVL trees

Invariant: for every node, `|height(left) - height(right)| <= 1`. Store the height in each
node. Guarantees height < 1.4405 log2(n + 2): a 1M-node AVL tree has height at most 28.

Insert: BST insert, then walk back up updating heights. At the first node whose balance factor
(`height(left) - height(right)`) becomes +2 or -2, fix with one or two rotations. Four cases:

| Case | Shape | Fix |
|---|---|---|
| Left-Left | left child is left-heavy | right rotation at the node |
| Right-Right | right child is right-heavy | left rotation at the node |
| Left-Right | left child is right-heavy | left rotation at the left child, then right rotation at the node |
| Right-Left | right child is left-heavy | right rotation at the right child, then left rotation at the node |

After the fix the subtree height is what it was before the insert, so no ancestor needs
further rebalancing: insert does at most 2 rotations. Delete may need a rotation at every
level on the way up: O(log n) rotations worst case, still O(log n) total.

AVL is stricter than red-black: shorter trees (faster lookups), more rotations on update. Use
when lookups dominate.

### 2.3 Red-black trees

Each node is red or black. Invariants:
1. Root is black.
2. Red nodes have black children (no two reds in a row).
3. Every path from a node to its null leaves has the same number of black nodes
   (the **black height**).

Consequence: the longest path (alternating red and black) is at most twice the shortest (all
black), so height <= 2 log2(n + 1). A 1M-node red-black tree has height at most 40 (typically
~20 to 22).

Insert: colour the new node red (never violates 3). If the parent is red, violation of 2; fix
with recolouring (if the uncle is red: recolour parent, uncle, grandparent and move up) or one
or two rotations (if the uncle is black). Amortised O(1) recolourings and at most 2 rotations.
Delete has more cases (6 in the textbook) but also at most 3 rotations.

Why `std::map`, Java's `TreeMap`, the Linux CFS scheduler and epoll's timer tree use red-black
and not AVL: fewer rotations per update (each rotation is pointer writes to several cache
lines, which matters under concurrent modification), and insert/delete with at most a constant
number of rotations. A red-black tree is isomorphic to a **2-3-4 tree** (a B-tree with 2 to 4
children per node): a black node with its red children is one 2-3-4 node. That is the
conceptual bridge to the B-tree.

Know the invariants and the height bound, and be able to describe the insert fix-up in words.
Nobody expects you to write red-black delete from memory; everyone expects you to know what
`std::map` is and why it is O(log n).

### 2.4 B-trees

A B-tree of **order** m (or "minimum degree" t, with m = 2t) is a search tree where:
- every node has at most m - 1 keys and m children;
- every non-root node has at least ceil(m/2) - 1 keys (nodes are at least half full);
- the root has at least 1 key (unless the tree is empty);
- all leaves are at the same depth (the tree is perfectly height-balanced);
- within a node, keys are sorted, and child i holds keys strictly between key[i-1] and key[i].

Height h satisfies n >= 2 (m/2)^h - 1, so h <= log_(m/2)((n+1)/2). With m = 500 and n = 10^9:
h <= log_250(5 x 10^8) ~ 3.6. Four levels. With the root and the next level cached (they are
hot), a point lookup costs 2 disk reads.

Search: in each node, binary search the keys (module 09): `upper_bound` gives the child
index. Descend. O(log_m n) nodes, O(log2 m) comparisons each: O(log2 n) comparisons total,
same as a BST, but O(log_m n) **page reads**.

Insert: descend to the leaf, insert in sorted position. If the leaf now has m keys (overflow),
**split**: the median key moves up into the parent, the keys left of it stay, the keys right of
it form a new sibling. The parent gains one key and one child; if it overflows, split it too,
possibly up to the root. A root split is the only way the tree grows taller — and it grows at
the top, which is why all leaves stay at the same depth.

Delete: remove the key from the leaf. If the leaf falls below the minimum (underflow), either
**borrow** a key from a sibling through the parent (rotation at the B-tree level), or if the
sibling is also at minimum, **merge** with it and pull the separator key down from the parent.
Parent underflow propagates up. If the root ends with zero keys, its single child becomes the
root: the tree shrinks at the top.

In the original B-tree (Bayer & McCreight, 1972) values are stored in every node alongside
keys, including internal nodes.

### 2.5 B+trees

The variant every database uses. Differences from the B-tree:

1. **Internal nodes hold only separator keys and child pointers.** No values. So an internal
   node of 8 KB holds many more separators -> higher fan-out -> shorter tree.
2. **All key/value pairs live in the leaves.** Every key appears in a leaf; a separator in an
   internal node is a copy (or a shortened form — see suffix truncation) that only guides the
   search.
3. **Leaves are linked** left-to-right (and usually right-to-left). A range scan descends once
   to the first leaf and then walks siblings. No parent revisits.

Leaf split in a B+tree: the new right leaf gets the upper half; the **first key of the right
leaf is copied up** as the separator (the key itself stays in the leaf). Internal split: the
median separator **moves up** (it is not needed in the children, since it only guides).

```
Internal:            [ 20 | 40 ]
                    /     |     \
Leaves:   [5 10 15] -> [20 25 30] -> [40 45 50 55]  -> null
          values      values         values
```

Search for 25: 25 >= 20 and < 40 -> middle child -> binary search in leaf -> found.
Range scan [12, 47): descend for 12 -> first leaf, `lower_bound` = position of 15 -> emit 15;
follow `next` -> emit 20 25 30 -> follow `next` -> emit 40 45; stop at 50 >= 47.

### 2.6 Page size and fan-out arithmetic

Pages are 8 KB in PostgreSQL (`BLCKSZ`), 4 KB default in Berkeley DB (configurable to 64 KB),
16 KB in InnoDB, 4 KB in SQLite by default. A page holds a header, an array of item pointers
("line pointers", 4 bytes each in PostgreSQL), the items themselves, and a "special space" at
the end for sibling pointers and flags.

PostgreSQL B-tree leaf item for an 8-byte integer key: 8-byte `IndexTupleData` header (6-byte
heap TID + 2 bytes info) + 8-byte key = 16 bytes, aligned to 8 -> 16 bytes, plus a 4-byte line
pointer = 20 bytes. Page usable space ~8 KB - 24 header - 16 special ~ 8150 bytes -> about
400 items per leaf at 100% fill; with the default leaf fill factor of 90% at build time, ~360.
Internal pages hold the same tuple shape (the TID field repurposed as the child block number),
so fan-out is also ~400. So:

| Rows | Leaf pages | Height (levels incl. root) |
|---|---|---|
| 10^5 | ~280 | 2 |
| 10^7 | ~28 000 | 3 |
| 10^9 | ~2 800 000 | 4 |

Index size for 10^9 bigint keys: ~2.8M pages x 8 KB ~ 22 GB. The root and the ~400 level-2
pages (3 MB) stay in the buffer pool permanently; a point lookup on a cold leaf is 1 to 2
random I/Os. These are the numbers to have in your head when someone asks "how big is the
index and how many reads is a lookup".

Wider keys (text, UUID) mean fewer items per page, lower fan-out, taller trees, and — for
random UUIDs — inserts spread over every leaf, so no page stays hot in cache. That is why
"UUID v4 primary keys are bad for B-tree indexes" is a thing, and why time-ordered UUIDs (v7)
exist.

### 2.7 Cursor traversal

A **cursor** is a position in the leaf chain: (leaf page, slot index). Operations:
- `seek(key)` / `DB_SET_RANGE`: descend from the root, `lower_bound` in the leaf; the cursor
  sits on the first key >= `key`.
- `next()` / `DB_NEXT`: `slot + 1`; if past the end of the page, follow the right sibling
  pointer and go to slot 0. Amortised O(1) per step; O(n / fan-out) page reads for a full scan.
- `prev()` / `DB_PREV`: symmetric with the left sibling pointer.
- `first()` / `DB_FIRST`: descend always-left; `last()` / `DB_LAST`: always-right.
- `get(key)` / `DB_SET`: seek and check equality.

Your migration engine: `cursor->get(cursor, &key, &data, DB_FIRST)` then a loop of `DB_NEXT`
until `DB_NOTFOUND`. Underneath: one root-to-leaf descent, then a walk of the leaf chain
reading each leaf page once, in key order. That is the most efficient way to read a whole
B-tree — sequential leaf pages, no repeated descents — and it is why the migration read in
key order "for free" and why bulk-loading the destination in that same order was cheap
(sequential inserts hit the rightmost leaf, which stays in cache).

What makes cursors hard in a real engine: the tree changes under the cursor. A concurrent
insert splits the leaf the cursor is on; the cursor's slot index is now wrong. Solutions:
remember the **key** rather than the slot and re-seek after a change (PostgreSQL does this
when it has to drop its pin on the page); or pin the page so it cannot be split until the
cursor moves on (Berkeley DB keeps the page locked/pinned while the cursor is positioned on
it, which is why a cursor left open in your code blocks writers — a real operational hazard).

### 2.8 Concurrency: latch coupling and B-link trees

Readers and writers in the same tree at the same time:
- **Latch coupling** (lock crabbing): take a read latch on a child before releasing the parent.
  Writers take write latches top-down pessimistically (if a child might split) or
  optimistically (read latches down, write latch on the leaf only, restart if a split is
  needed).
- **B-link tree** (Lehman & Yao, 1981): each node has a right-sibling pointer and a **high
  key** (upper bound of what it may contain). A reader that arrives at a node and finds its key
  is greater than the high key simply moves right — the node was split and the key moved to the
  new sibling. No latch coupling needed for readers; splits are done right-then-up so a
  concurrent reader always finds a consistent path. PostgreSQL's nbtree is a B-link tree; the
  "move right" logic in `_bt_moveright` is exactly this. Every leaf page stores its high key as
  the first item.

You do not implement this in an interview. You mention it when asked "how do databases allow
concurrent reads and writes to an index".

### 2.9 Engineering refinements you should be able to name

- **Fill factor**: leave pages 10% empty at build so the first inserts do not split.
- **Rightmost-leaf fast path**: sequential inserts always go to the rightmost leaf; PostgreSQL
  caches that page and skips the descent when the key is larger than its high key.
- **Suffix truncation**: internal separators need only be long enough to separate the two
  children: between `"smith"` and `"stevens"` the separator `"sn"` suffices. Smaller separators
  -> higher fan-out.
- **Prefix compression**: keys in a leaf share a prefix; store it once.
- **Deduplication** (PostgreSQL 13+): a leaf item with many TIDs for one key value instead of
  one item per TID.
- **Bulk loading**: sort the data (module 08 external merge sort), fill leaves left-to-right at
  the fill factor, build internal levels bottom-up. O(n) after the sort, perfectly packed. This
  is `CREATE INDEX` and Berkeley DB's `DB_BULK` loading.
- **Bottom-up deletion / vacuum**: deleted keys are first marked dead, physically removed
  later when the page needs space, so a delete does not immediately cause merges.
- **Siblings are not always merged on underflow**: PostgreSQL never merges; it deletes a page
  only when it becomes completely empty (and leaves a tombstone until no scan can still be
  pointing at it). Underflow merging is textbook; "delete empty pages lazily" is practice.

### 2.10 Why not a hash index?

Hash: O(1) point lookup, no range scans, no ordering, no prefix matches, rehash cost, poor
locality for sequential keys. B+tree: O(log n) with tiny constants, range scans, `ORDER BY`
for free, prefix and `LIKE 'abc%'` matches, min/max in O(log n), excellent locality for
sequential keys. In PostgreSQL a B-tree index is the default for a reason; hash indexes are
for equality-only on wide keys. The same reasoning applies in memory: `std::map` (ordered)
versus `std::unordered_map` (not).

## 3. Code

Files: `code/m11_avl.cpp` (AVL tree with `unique_ptr`, insert and erase with all four rotation
cases, height field maintenance, invariant validation, differential test against `std::set`,
height bound check), `code/m11_bplus_tree.cpp` (in-memory B+tree templated on key, value and
max keys per node: search, insert with leaf and internal splits, erase with borrow and merge,
range scan, a `Cursor` class with `seek`/`next`/`valid`, invariant validator, differential test
against `std::map` with random operations, and a printout of the tree shape for a small
fan-out so you can see splits happen).

### B+tree node and insert (key parts)

```cpp
template <typename K, typename V, std::size_t kMaxKeys = 4>
class BPlusTree {
    struct Node {
        bool leaf;
        std::vector<K> keys;                              // sorted
        std::vector<std::unique_ptr<Node>> children;     // internal: keys.size() + 1
        std::vector<V> values;                            // leaf: keys.size()
        Node* next = nullptr;                             // leaf chain (right sibling)
    };
    struct Split { K separator; std::unique_ptr<Node> right; };   // result of an overflow

    std::optional<Split> insert_rec(Node* n, const K& key, const V& value, bool& inserted) {
        if (n->leaf) {
            auto pos = std::lower_bound(n->keys.begin(), n->keys.end(), key);
            std::size_t i = pos - n->keys.begin();
            if (pos != n->keys.end() && *pos == key) { n->values[i] = value; inserted = false; return std::nullopt; }
            n->keys.insert(pos, key);
            n->values.insert(n->values.begin() + i, value);
            inserted = true;
            if (n->keys.size() <= kMaxKeys) return std::nullopt;
            return split_leaf(n);
        }
        std::size_t i = std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin();  // child index
        auto s = insert_rec(n->children[i].get(), key, value, inserted);
        if (!s) return std::nullopt;
        n->keys.insert(n->keys.begin() + i, std::move(s->separator));     // new separator at i
        n->children.insert(n->children.begin() + i + 1, std::move(s->right));
        if (n->keys.size() <= kMaxKeys) return std::nullopt;
        return split_internal(n);
    }

    Split split_leaf(Node* n) {
        std::size_t mid = n->keys.size() / 2;
        auto right = std::make_unique<Node>(); right->leaf = true;
        right->keys.assign(n->keys.begin() + mid, n->keys.end());
        right->values.assign(n->values.begin() + mid, n->values.end());
        n->keys.resize(mid); n->values.resize(mid);
        right->next = n->next; n->next = right.get();                     // link into the chain
        return {right->keys.front(), std::move(right)};                   // COPY first key up
    }
    Split split_internal(Node* n) {
        std::size_t mid = n->keys.size() / 2;
        K sep = n->keys[mid];                                             // MOVES up: not kept below
        auto right = std::make_unique<Node>(); right->leaf = false;
        right->keys.assign(n->keys.begin() + mid + 1, n->keys.end());
        for (std::size_t j = mid + 1; j < n->children.size(); ++j) right->children.push_back(std::move(n->children[j]));
        n->keys.resize(mid); n->children.resize(mid + 1);
        return {sep, std::move(right)};
    }
```

### Cursor

```cpp
class Cursor {
    const Node* leaf_ = nullptr; std::size_t pos_ = 0;
public:
    bool valid() const { return leaf_ && pos_ < leaf_->keys.size(); }
    const K& key() const { return leaf_->keys[pos_]; }
    const V& value() const { return leaf_->values[pos_]; }
    void next() {                                   // DB_NEXT
        if (!valid()) return;
        if (++pos_ >= leaf_->keys.size()) { leaf_ = leaf_->next; pos_ = 0; }   // hop to the sibling
    }
};
Cursor seek(const K& key) const {                   // DB_SET_RANGE: first key >= key
    const Node* n = root_.get();
    while (!n->leaf) n = n->children[std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin()].get();
    std::size_t pos = std::lower_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin();
    Cursor c(n, pos);
    if (pos == n->keys.size()) c.next();            // landed past the end of this leaf
    return c;
}
```

A range scan `[lo, hi)` is `for (auto c = t.seek(lo); c.valid() && c.key() < hi; c.next())`.

## 4. Under the hood (senior-level)

### 4.1 A PostgreSQL B-tree page, byte by byte

```
offset 0    PageHeaderData (24 bytes): LSN of last WAL record that touched the page, checksum,
            flags, pd_lower (end of line pointer array), pd_upper (start of item data),
            pd_special (start of special space)
offset 24   ItemIdData[] line pointers, 4 bytes each: (offset, flags, length). Items are
            addressed by line pointer number, so items can move within the page without
            changing their identity.
   ...      free space
pd_upper    IndexTuples packed from the end backwards. Each: ItemPointerData t_tid (6 bytes:
            block number + offset -> heap row, or child block for internal pages), t_info
            (2 bytes: size + flags), then the key datum(s), aligned.
pd_special  BTPageOpaqueData (16 bytes): btpo_prev, btpo_next (sibling block numbers),
            btpo_level (0 = leaf), btpo_flags (leaf/root/half-dead/...), btpo_cycleid.
```

The **high key** (first item on every non-rightmost page) is the upper bound of the page's
key range: a reader checks it to decide whether to move right (B-link). Search within a page
is binary search over the line pointer array (`_bt_binsrch`), comparing keys via the
operator class's comparison function — so a text index costs a `strcoll` per comparison,
which is why collation choice affects index speed.

### 4.2 Buffer pool and I/O

Pages live on disk and are cached in a **buffer pool** (`shared_buffers`, Berkeley DB
`DB_ENV->set_cachesize`). A lookup descends through pages that are usually cached (root,
level 2) to a leaf that may not be: one random read, 100 us on SSD, 10 ms on spinning disk. A
range scan reads consecutive leaves; if the index was built in one go they are physically
sequential and the OS read-ahead helps; after years of splits they are scattered — "index
fragmentation", fixed by `REINDEX`. Modifications are written to the WAL first (so a split is
atomic across crashes), then the dirty page is written back later by the checkpointer. A split
touches 3 pages (left, right, parent) and logs all three.

### 4.3 Why B+trees beat binary trees in RAM too

A `std::map` with 10^7 entries: ~23 levels, each a cache miss on a 48-byte node: ~23 misses
per lookup. A B+tree with 64-byte nodes holding 7 keys (one cache line each): log_7(10^7) ~ 8
levels, 8 misses. With 4 KB nodes: 3 levels, but each node's binary search touches ~log2(500)
~ 9 lines, several of which are prefetchable; ~12 misses. Measured, in-memory B+trees
(Google's `absl::btree_map`, which uses 256-byte nodes) are 2 to 4x faster than `std::map` for
lookups and use half the memory. `absl::btree_map` is a drop-in replacement for `std::map`
except that iterators are invalidated by insert/erase (keys move within nodes). If an
interviewer asks "how would you speed up `std::map`", this is the answer, with the iterator
caveat.

### 4.4 Split cost and the "sequential insert" hot spot

Sequential keys (auto-increment ids, timestamps) always insert into the rightmost leaf. Good:
that leaf is always cached; only one leaf is being written; bulk inserts are almost O(1) each.
Bad: that one page is a contention point under many concurrent writers (every writer wants
the write latch on the same page) — the "rightmost leaf hot spot". PostgreSQL's fast path
helps the latency; some systems (e.g. Oracle's reverse-key indexes) scatter inserts on
purpose, trading range-scan locality for write concurrency. Random keys (UUID v4) have the
opposite profile: no hot spot, but every insert touches a random leaf (cache miss, possible
disk read, random write), and leaf pages end up ~70% full on average because splits happen
everywhere.

### 4.5 Berkeley DB specifics you can speak to

`DB_BTREE` access method, default page size 4 KB (or the filesystem block size), configurable
with `DB->set_pagesize` up to 64 KB. Keys and data are both stored in leaf pages (B+tree);
overflow pages hold items larger than a quarter of a page. Internal pages store the first key
of each child (no suffix truncation by default). Duplicates go in on-page duplicate sets or
off-page duplicate trees (`DB_DUP`, `DB_DUPSORT`). `DB->set_bt_compare` installs a comparison
function — your migration engine would have had to match the exact comparison semantics to
read keys in the right order, or ordering would differ from the source. Cursors
(`DB->cursor`, `DBC->get` with `DB_FIRST`/`DB_NEXT`/`DB_SET_RANGE`) hold a page lock while
positioned, so an open cursor blocks concurrent writers to that page; `DB_READ_COMMITTED`
degree-2 isolation releases locks as the cursor moves. Statistics (`DB->stat`) report the
tree height (`bt_levels`), page fill, and overflow pages — the first things to check when a
Berkeley DB store is slow.

## 5. In production

- **Choosing the key order.** A composite index on `(account_id, txn_date)` supports
  `WHERE account_id = ? AND txn_date BETWEEN ? AND ?` as one range scan on the leaf chain.
  `(txn_date, account_id)` does not — the account rows are scattered across the date range.
  "Equality columns first, then the range column" is the rule, and it falls directly out of
  how the leaf chain is ordered.
- **Covering indexes.** If the index leaf contains every column the query needs (`INCLUDE` in
  PostgreSQL), the scan never touches the heap: index-only scan. The leaf chain *is* the
  result set.
- **Index bloat.** Deletes leave dead items; splits leave half-empty pages. An index that was 2
  GB at build grows to 6 GB over a year of churn with the same row count. `pgstattuple` or
  `bt_index_check` to measure; `REINDEX CONCURRENTLY` to rebuild. Watch for it when "the same
  query got slower and nothing changed".
- **Monitoring a B-tree store.** Height (should be stable), pages per level, average fill,
  cache hit ratio on the buffer pool, number of splits per second (a sudden rise means a new
  insert pattern), long-held cursors (blocking writers).
- **Migration lesson.** Reading a source B-tree with a cursor in key order and inserting into
  a destination in that order is the optimal pattern for both sides: sequential leaf reads on
  the source, rightmost-leaf appends on the destination. If the destination has a different
  key order (different comparator, different collation, or a distributed system that hashes
  the key like YugabyteDB does by default for its primary index), the inserts become random
  and throughput can drop 10x. When you describe your migration engine, this is the detail
  that shows you understood what you were doing: key order, cursor semantics, and batch size
  versus lock hold time.
- **3 AM story (typical).** Writes stall every few seconds. The buffer pool is full of dirty
  index pages; the checkpointer is writing them out and the WAL volume is 5x normal. Cause: a
  new feature inserts UUID v4 keys into a large index, so every insert dirties a random leaf
  page. Fix: switch to a time-ordered key (UUID v7, or a bigint sequence), or move the UUID
  to a non-indexed column with a hash index for equality. Understanding "random key = random
  leaf = random I/O" is what gets you from symptom to cause.

## 6. Try it

1. **(easy)** Draw by hand the B+tree with max 3 keys per node after inserting 1..10 in order.
   Then after inserting 10..1 in reverse order. Done when both drawings have all leaves at the
   same depth and you can point to each separator's origin (which split copied it up).
2. **(easy)** Compute the fan-out and height for an index on a 16-byte UUID key with 8 KB
   pages and 10^9 rows, using the PostgreSQL item layout from section 2.6. Done when you have
   items per page, leaf page count, height, and approximate index size in GB.
3. **(medium)** In `m11_avl.cpp`, insert 1..1 000 000 in order and print the height. Compare
   with the bound 1.44 log2(n + 2). Then count rotations performed. Done when you can explain
   why sorted insertion into an AVL tree produces the minimum possible number of rotations
   per insert (about 1 every other insert).
4. **(medium)** Add a `prev()` to the B+tree cursor by adding a `prev` sibling pointer to leaves
   and maintaining it in `split_leaf` and in leaf merge. Done when a backwards scan of the
   whole tree equals the reversed forward scan after 100 000 random inserts and 50 000 deletes.
5. **(hard)** Implement bulk loading: `BPlusTree::from_sorted(const std::vector<std::pair<K,V>>&,
   double fill_factor)` that packs leaves to `fill_factor * kMaxKeys` and builds internal levels
   bottom-up. Done when the result validates, `height()` equals the formula's prediction, and
   loading 10^6 sorted pairs is at least 5x faster than inserting them one by one.
6. **(hard)** Simulate a B-link search: add a `high_key` to each node, perform a split "right
   first, then parent", and write a `search_blink` that moves right when `key > high_key`.
   Insert from one thread and search from another under a mutex that is released between the
   two halves of the split; the search must still find every key that was fully inserted. Done
   when 10^5 concurrent operations under `-fsanitize=thread` report no race and no lost key
   (see 04_Concurrency for the synchronisation primitives).

## 7. Interview questions

**Q: What is a rotation and why does it preserve BST order?**
A local re-wiring of a node and one child that swaps their parent/child roles and moves one
subtree across. The in-order sequence `A x B y C` is unchanged because the subtrees keep
their relative positions; only the depths change.

**Q: AVL vs red-black — trade-offs?**
AVL: stricter balance (height <= 1.44 log n), faster lookups, more rotations on update (O(log
n) on delete). Red-black: looser (height <= 2 log n), at most 2 to 3 rotations per update,
amortised O(1) recolourings. `std::map` is red-black because updates are cheaper and
rotations under concurrency are expensive.

**Q: What is a B-tree and why do databases use it instead of a binary tree?**
A balanced search tree whose nodes are disk pages holding hundreds of sorted keys, so each
page read narrows the search by a factor of hundreds. Height 3 or 4 for a billion keys means
1 to 2 uncached reads per lookup. All leaves are at the same depth; the tree grows by
splitting at the root.

**Q: B-tree vs B+tree?**
B+tree keeps values only in the leaves and links the leaves into a chain. Internal nodes hold
only separators, so fan-out is higher and the tree shorter; range scans walk the leaf chain
without revisiting internal nodes. Every database index is a B+tree.

**Q: Walk me through an insert that splits.**
Descend by `upper_bound` on separators to the leaf; insert in sorted position; if the leaf
exceeds max keys, move the upper half to a new right sibling, link it into the chain, and copy
its first key into the parent as a separator; if the parent overflows, split it with the
median moving up; if the root splits, create a new root with one separator.

**Q: How does a cursor do a range scan?**
`seek(lo)`: descend to the leaf, `lower_bound` to the first key >= lo. Then `next()` repeatedly:
advance the slot; at the end of a leaf follow the right-sibling pointer. Stop when the key
reaches `hi`. One descent plus a sequential walk of the leaf pages.

**Q: How many pages does a lookup read in a 10^9-row bigint index?**
About 400 items per 8 KB page -> ~2.8M leaves -> height 4. Root and second level (a few MB) are
always cached, so 1 to 2 physical reads for the leaf (and level 3 if cold).

**Q: Why are random UUID primary keys bad for a B-tree?**
Each insert goes to a random leaf: a cache miss or disk read, a random dirty page, and splits
spread everywhere leaving pages ~70% full. Sequential keys append to the cached rightmost
leaf. Use time-ordered ids where range locality or write throughput matters.

**Q: How do databases let readers and writers use the index concurrently?**
Latch coupling (hold the parent until the child is latched), or B-link trees: each page has a
right-sibling pointer and a high key, so a reader that finds its key above the high key just
moves right; splits are done child-then-parent so readers never see an inconsistent path.
PostgreSQL uses the B-link scheme.

## 8. Recap

- Rotations re-wire three pointers and preserve in-order; AVL uses them to keep child heights
  within 1; red-black uses colour invariants for height <= 2 log n with fewer rotations.
- `std::map` is a red-black tree: O(log n) everything, a cache miss per level.
- B-tree: nodes are pages with up to m-1 sorted keys; all leaves at the same depth; grows by
  splitting upward from the leaf, shrinks by merging downward from the root.
- B+tree: values only in leaves, leaves chained; leaf split copies the first right key up,
  internal split moves the median up.
- Descent is `upper_bound` on separators; leaf positioning is `lower_bound`.
- 8 KB page, bigint key: ~400 items per page, height 4 for 10^9 rows, ~22 GB index.
- Cursor = (leaf, slot); `seek` descends once, `next` walks the chain; open cursors can block
  writers in Berkeley DB.
- Sequential keys append to the cached rightmost leaf; random keys dirty random pages.
- Readers and writers coexist via latch coupling or B-link (sibling pointer + high key).
- Bulk load = sort, pack leaves at fill factor, build levels bottom-up; `REINDEX` fixes bloat.
