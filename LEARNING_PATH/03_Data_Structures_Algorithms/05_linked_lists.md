# 05. Linked lists

**Time:** ~4 hours   **Prerequisites:** 01, 02, 04

## 1. In plain words

An array keeps its elements side by side; to insert in the middle you shift everything after
it. A linked list keeps each element in its own box anywhere in memory, and each box stores
the address of the next one. Inserting in the middle is now two pointer writes, no shifting.
The price: to reach box number 500 you must follow 500 pointers, because there is no formula
for "where is element i".

You have written this in C: `struct node { int val; struct node* next; }` with `malloc` and
`free`, and you have debugged the use-after-free and the forgotten `free` that go with it. C++
gives you `std::unique_ptr` so the "who deletes this" question has a compile-time answer, and
`std::list` / `std::forward_list` so you rarely write a list yourself in production.

So why a whole module? Three reasons. Interviews love lists because a pointer bug shows
immediately and the problems (reverse, cycle, merge, nth from end) test whether you can hold
an invariant in your head. The LRU cache — a doubly linked list glued to a hash map — is the
single most-asked design problem for systems roles and appears in every cache, buffer pool
and connection manager you will ever read. And intrusive lists (the node lives inside the
object) are everywhere in kernels, databases and your RPC server's queues.

## 2. The real thing

### 2.1 Singly linked list

```
head -> [3 | *] -> [7 | *] -> [1 | *] -> nullptr
```

| Operation | Cost | Note |
|---|---|---|
| push_front | O(1) | new node, point at old head, head = new |
| push_back | O(1) with tail pointer, else O(n) | |
| insert after node p | O(1) | `n->next = p->next; p->next = n;` |
| delete after node p | O(1) | `dead = p->next; p->next = dead->next; delete dead;` |
| delete node p itself | O(n) | need the predecessor; or O(1) if you copy the next node into p |
| find by value / index | O(n) | |
| reverse | O(n) | three pointers |

The order of the two pointer writes on insert matters: set the new node's `next` **first**, or
you lose the rest of the list. On delete, save `next` **before** freeing.

### 2.2 Doubly linked list

Each node also has `prev`. Deleting a node given only its pointer is O(1):
`p->prev->next = p->next; p->next->prev = p->prev;`. Costs one more pointer per node. This is
what `std::list` is, and what the LRU cache needs.

### 2.3 The dummy head (sentinel)

Many list bugs are special cases for "the list is empty" or "the node to delete is the head".
A **sentinel** node that always exists, holds no value, and sits before the first real node
removes those cases: there is always a predecessor. In a circular doubly linked list the
sentinel also serves as the tail marker: `head->prev` is the last element and the list is
empty when `head->next == head`. `std::list` is implemented this way. In interview code,
`ListNode dummy(0); dummy.next = head; ListNode* prev = &dummy;` is the idiom; return
`dummy.next` at the end.

### 2.4 Ownership: `unique_ptr` versus raw pointers

`std::unique_ptr<T>` is a pointer that deletes what it points to when it goes out of scope,
cannot be copied, and can be moved. For a list, `struct Node { int val; std::unique_ptr<Node>
next; }` means: the list owns its nodes, destroying the head destroys the chain, and there is
no way to leak or double-free. Traversal uses raw pointers obtained with `.get()`: raw
pointers *observe*, `unique_ptr` *owns*. That split is the whole rule.

Two caveats:
1. **Recursive destruction.** Destroying the head destroys `next`, which destroys its `next`,
   ... recursively. A list of 1M nodes overflows the stack. Write an explicit destructor that
   pops nodes iteratively (`while (head) head = std::move(head->next);`), which is what the
   code file does.
2. **Doubly linked with `unique_ptr`:** `next` owns, `prev` is a raw back-pointer. Two owning
   pointers to the same node would double-free.

In interview problems the `ListNode` is almost always given as a raw-pointer struct, and you
do not free anything. Use raw pointers there, say "in production I would use `unique_ptr` or
`std::list`", and move on. The code file `m05_list_problems.cpp` follows that convention and
frees the test lists in a helper.

### 2.5 Classic problems and their invariants

**Reverse** (iterative): `prev = nullptr; while (cur) { next = cur->next; cur->next = prev;
prev = cur; cur = next; } return prev;`. Invariant: `prev` heads the reversed prefix, `cur`
heads the unprocessed suffix. Recursive: reverse the rest, then `head->next->next = head;
head->next = nullptr;` — O(n) stack, do not use for long lists.

**Middle node**: slow moves one, fast moves two; when fast hits the end slow is at the middle
(the second middle for even length).

**Cycle detection (Floyd)**: slow +1, fast +2. If there is a cycle, fast laps slow and they
meet; if not, fast hits `nullptr`. Why they must meet: once both are inside the cycle the gap
shrinks by exactly 1 per step, so it reaches 0 within one cycle length. To find the **cycle
start**: after they meet, reset one pointer to head and advance both by 1; they meet at the
start. Proof: let the distance head -> start be a, start -> meeting point be b, cycle length
L. Slow travelled a + b; fast travelled 2(a + b) = a + b + kL, so a + b = kL, so a = kL - b:
walking a from head lands exactly where walking a from the meeting point does, i.e. the start.

**Merge two sorted lists**: dummy head, `tail` pointer; take the smaller head each time; attach
the leftover. O(n + m), O(1), no allocation.

**Remove nth from end**: two pointers n apart; when the lead hits the end, the trailer's `next`
is the node to remove. Use the dummy so removing the head is not special.

**Palindrome**: find middle, reverse the second half in place, compare, (optionally restore).
O(n), O(1).

**Reorder L0 Ln L1 Ln-1 ...**: middle, reverse second half, interleave. Combines three
primitives — a good test of whether you can compose.

### 2.6 LRU cache

Least Recently Used: a fixed-capacity key-value store that evicts the entry unused for the
longest time. Needed: O(1) `get`, O(1) `put`, O(1) "move to front", O(1) "evict from back".

- A doubly linked list in recency order (front = most recent). Move-to-front and pop-back
  are O(1) given a node pointer.
- A hash map key -> node pointer, so that `get` finds the node in O(1).

```
map: {k1 -> n1, k2 -> n2, k3 -> n3}
list: [sentinel] <-> n2(k2) <-> n1(k1) <-> n3(k3) <-> [sentinel]
                     most recent              least recent
get(k1): map lookup -> n1; unlink n1; insert after sentinel. list: n1 n2 n3
put(k4) at capacity 3: evict back (n3): map.erase(k3), unlink n3; insert n4 at front.
```

With `std::list` and `std::unordered_map<K, std::list<...>::iterator>`, `list::splice` does
move-to-front in O(1) without allocating. The code file has both that version and a
hand-rolled node version, because interviewers sometimes ask for it without the library.

## 3. Code

Files: `code/m05_singly_list.cpp` (owning list with `unique_ptr`, iterative destructor,
push/pop/insert/erase/reverse/find, iteration), `code/m05_list_problems.cpp` (raw-pointer
`ListNode` problems: reverse both ways, middle, Floyd cycle + start, merge, nth from end,
palindrome, reorder, remove duplicates), `code/m05_lru_cache.cpp` (LRU with `std::list` +
`unordered_map`, and a hand-rolled doubly linked version).

### Owning singly linked list (key parts)

```cpp
class SinglyList {
    struct Node { int val; std::unique_ptr<Node> next; };
    std::unique_ptr<Node> head_;
    Node* tail_ = nullptr;          // observer: the last node, owned via the chain
    std::size_t size_ = 0;
public:
    ~SinglyList() { while (head_) head_ = std::move(head_->next); }  // iterative: no deep recursion

    void push_front(int v) {
        auto n = std::make_unique<Node>(Node{v, std::move(head_)});  // steal old head into n->next
        if (!tail_) tail_ = n.get();
        head_ = std::move(n);
        ++size_;
    }
    void push_back(int v) {
        auto n = std::make_unique<Node>(Node{v, nullptr});
        Node* raw = n.get();
        if (tail_) tail_->next = std::move(n); else head_ = std::move(n);
        tail_ = raw;
        ++size_;
    }
    bool erase_first(int v) {
        // Pointer-to-unique_ptr walk: `link` is the owner slot that leads to the current node,
        // so unlinking the head is no special case (same trick as Node** in C).
        std::unique_ptr<Node>* link = &head_;
        Node* prev = nullptr;
        while (*link) {
            if ((*link)->val == v) {
                if (link->get() == tail_) tail_ = prev;
                *link = std::move((*link)->next);   // old node is destroyed here
                --size_;
                return true;
            }
            prev = link->get();
            link = &(*link)->next;
        }
        return false;
    }
    void reverse() {
        std::unique_ptr<Node> prev, cur = std::move(head_);
        tail_ = cur.get();
        while (cur) {
            std::unique_ptr<Node> next = std::move(cur->next);
            cur->next = std::move(prev);
            prev = std::move(cur);
            cur = std::move(next);
        }
        head_ = std::move(prev);
    }
};
```

### LRU with the standard library (key parts)

```cpp
class LRUCache {
    using Entry = std::pair<int, int>;                       // key, value
    std::size_t cap_;
    std::list<Entry> order_;                                 // front = most recently used
    std::unordered_map<int, std::list<Entry>::iterator> pos_;
public:
    explicit LRUCache(std::size_t capacity) : cap_(capacity) {}
    std::optional<int> get(int key) {
        auto it = pos_.find(key);
        if (it == pos_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second);   // O(1) move-to-front, iterator stays valid
        return it->second->second;
    }
    void put(int key, int value) {
        auto it = pos_.find(key);
        if (it != pos_.end()) {
            it->second->second = value;
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        if (order_.size() == cap_) {                          // evict least recently used
            pos_.erase(order_.back().first);
            order_.pop_back();
        }
        order_.emplace_front(key, value);
        pos_[key] = order_.begin();
    }
};
```

`std::optional<int>` (C++17) is "either an int or nothing"; it replaces returning -1 as a
sentinel. `splice` relinks the node without invalidating the iterator stored in the map —
that is the property that makes `std::list` the right container here, and the reason
`std::vector` cannot do this job.

## 4. Under the hood (senior-level)

### 4.1 Memory layout and why lists are slow

Each `new Node` goes through `malloc`: for a 16-byte node (int + pointer, padded) glibc hands
out a 32-byte chunk (16 bytes header-adjacent overhead and alignment), from whatever free
region it has. Consecutive allocations are often adjacent when the heap is fresh, so a freshly
built list happens to be sequential and benchmarks well. After hours of mixed
allocate/free in a server, nodes are scattered: each `next` is a cache miss (~100 ns). A
1M-node traversal is ~100 ms; the same 1M ints in a vector traverse in ~1 ms.

`std::list<int>` node on libstdc++: prev + next + int + padding = 24 bytes, allocated as 32.
So 4 bytes of payload cost 32 bytes of memory plus the allocator's bookkeeping. This is why
"use a list for O(1) middle insertion" is usually wrong advice: the vector's `memmove` of a
few KB beats a cache miss, and the vector uses 8x less memory.

### 4.2 When lists are right

- You hold pointers/iterators to elements that must stay valid while other elements are
  inserted or removed (vector invalidates; list never does). The LRU cache is exactly this.
- You splice ranges between lists in O(1) (`std::list::splice`): moving a connection from the
  "idle" list to the "active" list without copying.
- Elements are large or non-movable.
- **Intrusive lists**: the `next`/`prev` fields live inside the object itself (Linux kernel
  `struct list_head`, Boost.Intrusive). No separate node allocation, the object can be in
  several lists at once, and removing it from all of them is O(1) given the object. Your RPC
  server's per-connection structure almost certainly sits in an intrusive list of active
  connections and another of pending timers.

### 4.3 `unique_ptr` codegen

`std::unique_ptr<Node>` is one pointer: `sizeof == 8`. Move is a pointer copy plus nulling the
source. Destruction is `if (p) delete p;`. With optimisation on, a `unique_ptr`-based list
compiles to the same machine code as raw pointers, plus the destructor calls the compiler
inserts at scope exit — which are the ones you would have forgotten.

### 4.4 Floyd's algorithm correctness, once more

With slow at position s and fast at position 2s (steps since start), both inside a cycle of
length L after at most a steps (a = distance to the cycle): the gap `(2s - s) mod L = s mod L`
hits 0 when s is a multiple of L, i.e. within L more steps. Total steps <= a + L <= n. So the
algorithm is O(n) with two pointers of state: this is why it beats the "hash set of visited
nodes" solution, which is also O(n) but needs O(n) memory.

### 4.5 LRU in real caches

A true LRU with one global list has a problem under concurrency: every `get` writes to the
list (move-to-front), so a read-heavy cache serialises on one lock. Real systems approximate:
PostgreSQL's buffer pool uses a **clock sweep** (a usage counter per page, a hand that
decrements and evicts at zero); Linux uses two lists (active/inactive) with promotion on
second access; CPUs use pseudo-LRU bits. Memcached uses a segmented LRU with per-slab locks.
Berkeley DB's mpool uses an LRU-ish priority with a buffer-count-based clock. When an
interviewer asks "how would you make this LRU thread-safe", the answer is "shard it by key
hash, one lock and one list per shard, and accept that eviction is approximate across shards".

## 5. In production

- **Connection management.** A doubly linked list of idle connections ordered by last
  activity, plus a map from socket fd to node, is the LRU cache with a different name. Idle
  timeout sweep: pop from the back while `now - last_used > timeout`. O(k) for k expired
  connections, not O(n).
- **Free lists.** Your memory pool keeps freed blocks on a singly linked list threaded through
  the blocks themselves (the first 8 bytes of a free block hold the `next` pointer). O(1)
  allocate and free, no metadata. This is how `malloc`'s small-size bins work.
- **Timer queues.** A sorted doubly linked list of timers works when timers are mostly added
  at the end (same timeout value). For arbitrary timeouts use a heap (module 12) or a timer
  wheel.
- **Debugging a corrupted list at 3 AM.** Symptom: a traversal spins forever (accidental
  cycle) or crashes on a wild pointer. In a crash dump, walk the list by hand with a counter;
  if it exceeds the known size you have a cycle. Common causes: a node freed while still
  linked (use-after-free), a node inserted into two lists with one set of link fields, or
  a `next` write done before saving the old `next`. A debug build that poisons freed nodes
  (`memset(0xDD)`) makes the crash happen at the bug, not later.
- **Lock-free stacks and queues** (Treiber stack, Michael-Scott queue) are linked lists with
  compare-and-swap on the head pointer; see 04_Concurrency for the ABA problem that makes them
  hard.

## 6. Try it

1. **(easy)** Reverse a singly linked list in place with raw pointers, then with `unique_ptr`.
   Done when `1->2->3->4` becomes `4->3->2->1` and valgrind (or `-fsanitize=address`) reports
   no leaks.
2. **(easy)** Delete all nodes with a given value in one pass using the dummy-head idiom.
   Done when `[1,2,6,3,4,5,6], 6 -> [1,2,3,4,5]` and `[7,7,7], 7 -> []`.
3. **(medium)** Detect a cycle and return the index of the node where it starts. Build a test
   list where the tail points to node 2. Done when you can write the a = kL - b proof on paper.
4. **(medium)** Merge k sorted lists two at a time (O(nk)) and then with a heap (O(n log k),
   module 12). Done when both give identical output on three lists of 1000 random sorted ints.
5. **(hard)** Implement `LRUCache` by hand with your own doubly linked nodes and
   `std::unordered_map<int, Node*>`, no `std::list`. Done when the test sequence in
   `m05_lru_cache.cpp` passes against both implementations and `put` on an existing key
   updates the value and recency.
6. **(hard)** Make the LRU cache thread-safe with N shards (`key % N`), each with its own mutex,
   list and map. Done when 8 threads doing 1M mixed operations produce no data race under
   `-fsanitize=thread` and total capacity is still respected per shard.

## 7. Interview questions

**Q: Array vs linked list — when do you use a list?**
When you need stable pointers to elements across insertions elsewhere, O(1) splicing, or O(1)
removal of an element you already hold a pointer to (LRU, intrusive queues). For everything
else a vector is faster because of cache locality and lower memory overhead.

**Q: Reverse a linked list.**
Three pointers: `prev`, `cur`, `next`. Save `next`, point `cur->next` at `prev`, advance both.
Return `prev`. O(n) time, O(1) space. The recursive version is O(n) stack.

**Q: How do you detect a cycle, and find where it starts?**
Floyd: slow +1, fast +2 — they meet if and only if there is a cycle, in O(n) with O(1) space.
To find the start, move one pointer to the head and advance both by one; they meet at the
cycle entry because the head-to-entry distance equals the meeting-point-to-entry distance
modulo the cycle length.

**Q: Design an LRU cache with O(1) get and put.**
Doubly linked list in recency order plus a hash map from key to list node. `get`: map lookup,
move node to front. `put`: if present update and move to front; else if full, remove the back
node and its map entry; insert at front and in the map. With `std::list` use `splice` so
iterators stay valid.

**Q: Why not use a vector for the LRU recency order?**
Move-to-front in a vector is O(n) (shift), and it invalidates the indices stored in the map.
The list gives O(1) unlink/relink and stable node addresses.

**Q: What is a sentinel/dummy head and why use it?**
A node that always exists before the first element so that insert/delete at the head is not
a special case. It eliminates the "if (head == nullptr)" and "if (node == head)" branches that
cause most list bugs.

**Q: What goes wrong with `unique_ptr<Node> next` on a very long list?**
The destructor is recursive — each node's destructor destroys the next — so a list of ~100k
nodes overflows the stack. Write an iterative destructor that pops the head repeatedly.

**Q: Why does `std::list::splice` matter for the LRU cache?**
It relinks a node to a new position in O(1) without allocating or copying and without
invalidating iterators to it, so the map's stored iterator remains correct after move-to-front.

## 8. Recap

- A list trades O(1) index access for O(1) insert/delete at a known node; every `next` is a
  potential cache miss.
- Set the new node's `next` before linking it in; save `next` before freeing.
- Dummy head removes empty-list and delete-head special cases.
- `unique_ptr` for ownership, raw pointers for observation; write the destructor iteratively.
- Reverse: prev/cur/next. Middle: slow/fast. Cycle: Floyd, meet then reset to head for the start.
- Merge: dummy + tail, take the smaller head. Nth from end: two pointers n apart.
- LRU = doubly linked list (recency) + hash map (key -> node); `splice` keeps iterators valid.
- `std::list<int>` costs ~32 bytes per 4-byte element; vector costs 4.
- Intrusive lists (links inside the object) are how kernels and servers queue things.
- Real caches approximate LRU (clock sweep, segmented LRU) to avoid a global lock.
