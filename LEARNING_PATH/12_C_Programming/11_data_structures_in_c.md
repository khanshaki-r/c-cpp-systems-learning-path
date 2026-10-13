# 11. Data structures in C

**Time:** ~3 hours   **Prerequisites:** modules 03 (pointers), 04 (memory, storage and lifetime), 06 (structs and layout), 07 (function pointers), 10

## 1. In plain words

C gives you three building blocks: arrays, structs and pointers. There is no `vector`, no
`map`, no `list`. Every C codebase of any size therefore contains its own growable array, its
own linked list, its own hash table, and usually a ring buffer and a priority queue. In an old
server you will find several versions of each, written by different people over the years.

Writing them is not hard. Writing them so they are **correct under every edge case, safe when
sizes come from outside, and usable for more than one element type** is the senior part. Two
questions decide the design of every C container:

1. **Who owns the memory?** Does the container copy the element, or keep a pointer to something
   the caller owns? When does a pointer you got from the container stop being valid?
2. **How is it generic?** C has no templates. You choose between `void *` plus an element size
   plus a comparator (the `qsort` way), macros that generate a typed version per element type,
   or intrusive links embedded in your own structs (the Linux kernel way).

Think of a warehouse. A dynamic array is one long shelf that you replace with a longer shelf
when it fills (and everything moves). A linked list is boxes chained with string: easy to insert
anywhere, slow to walk. An intrusive list is boxes with the hook already built into the box. A
hash table is a wall of numbered pigeonholes. A ring buffer is a carousel. A heap is the
"most urgent first" tray.

## 2. The real thing

### 2.1 Dynamic array

State: a pointer, a length, a capacity, and for the generic version the element size.

- **Growth.** When full, multiply capacity by a constant factor (2 here; 1.5 is also common) and
  `realloc`. Geometric growth makes `push` amortised O(1): the total copying over n pushes is at
  most about 2n element copies. Growing by a constant (+16) makes it O(n) per push on average.
- **Overflow.** `cap * elem_size` must not wrap. Check `ncap > SIZE_MAX / elem_size` before
  multiplying. A wrapped size gives a tiny allocation followed by a heap overflow.
- **`realloc` may move the block.** Every pointer into the array is invalid after a push that
  reallocates. This is the C version of "iterator invalidation" and a common use-after-free.
- **Removal in the middle** shifts the tail with `memmove` (not `memcpy`: the regions overlap).
  If order does not matter, move the last element into the hole instead: O(1).
- **Failure must leave the array usable.** Assign `realloc`'s result to a temporary first.

### 2.2 Singly linked list and the pointer-to-pointer trick

Most people write list deletion with a special case for the head. Using a pointer to "the link
that points at the current node" removes the special case:

```c
struct snode **pp = head;              /* first: the address of the head variable */
while (*pp) {
    if ((*pp)->value == value) {
        struct snode *dead = *pp;
        *pp = dead->next;              /* relinks the head OR a previous node's next */
        free(dead);
    } else {
        pp = &(*pp)->next;             /* now: the address of this node's next field */
    }
}
```

Two more rules: read `next` **before** you `free` a node, and remember that a list costs a cache
miss per node, so walking a long list is much slower than scanning an array of the same length.
Lists win when you need O(1) insertion or removal at a known position, stable element addresses,
or splicing whole lists together.

### 2.3 Intrusive doubly linked lists and `container_of`

A normal ("non-intrusive") list allocates a node that points to your data: two allocations per
element and an extra pointer hop. An **intrusive** list puts the links inside your struct:

```c
struct list_head { struct list_head *next, *prev; };

struct session {
    int              id;
    char             user[16];
    struct list_head all_link;    /* on the list of all sessions */
    struct list_head lru_link;    /* and, at the same time, on the LRU list */
};
```

The list code only ever sees `struct list_head *`. To get back from a link to the object that
contains it, subtract the link's offset inside the struct:

```c
#define container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))
```

Design points of the Linux kernel's `list_head` that `m11_lists.c` copies:

- **Circular with a sentinel head.** An empty list's head points at itself. Insertion and
  removal never test for NULL, which removes a whole class of edge-case bugs.
- **One object, many lists, zero allocations.** Adding a session to the LRU list costs four
  pointer writes and no `malloc`. Removing it, given only the object, is O(1).
- **`list_for_each_safe`** reads the next pointer before the loop body runs, so the body may
  remove (and free) the current element.

Windows has the same idea: `LIST_ENTRY` and the `CONTAINING_RECORD` macro (both in `winnt.h`),
plus `InsertTailList` and `RemoveEntryList` in the driver kit headers, are `list_head`,
`container_of`, `list_add_tail` and `list_del` under other names.

### 2.4 Open-addressing hash table

All entries live in one array of slots. To insert, hash the key, take `hash & (cap - 1)` (cap is
a power of two) and probe forward until you find the key or an empty slot (**linear probing**).

```text
cap = 8, keys A (hash -> 3), B (hash -> 3), C (hash -> 4)

slot:  0     1     2     3     4     5     6     7
      [   ] [   ] [   ] [ A ] [ B ] [ C ] [   ] [   ]
                         ^home of A and B; B probed to 4; C's home 4 taken, probed to 5
```

- **Deletion needs tombstones.** If you delete B by marking slot 4 empty, a lookup for C starts
  at 4, sees "empty" and stops: C is lost. Mark it **TOMB** instead: lookups skip it, inserts may
  reuse it.
- **Load factor.** Keep (live + tombstones) / cap below about 0.7. Above that, probe chains get
  long fast. Rebuilding (rehashing into a new array) drops all tombstones; if most slots are
  tombstones, rebuild at the same size instead of growing.
- **Store the full hash in the slot.** A mismatch is then usually rejected by comparing two
  integers, without touching the key string in another cache line.
- **Own the keys.** Copy the key on insert, or document that the caller must keep it alive. A
  key pointing into a request buffer that is later freed is a classic dangling-pointer bug.
- **Hash function.** FNV-1a is simple and fine for trusted keys. When an attacker chooses the
  keys (HTTP headers, account ids in requests) they can force every key to collide and turn O(1)
  into O(n) per operation (hash flooding). Use a keyed hash such as SipHash with a random seed.

Separate chaining (an array of linked lists) is the other design; it tolerates high load and
simple deletion, but costs an allocation and a pointer hop per entry.
03_Data_Structures_Algorithms/04_hashing_hash_tables.md compares the two in depth.

### 2.5 Ring buffer

A fixed array used as a FIFO queue. The classic implementation keeps `head` and `tail` indexes
and wastes one slot to tell "full" from "empty". The cleaner version keeps **free-running**
unsigned counters and a power-of-two capacity:

```c
count = head - tail;                     /* correct even after head wraps past UINT_MAX */
buf[head & (cap - 1)] = v; head++;       /* push */
v = buf[tail & (cap - 1)]; tail++;       /* pop  */
```

Unsigned arithmetic is defined to wrap modulo 2^N, so `head - tail` is the element count even
when `head` has wrapped and `tail` has not. (With signed counters this would be UB, module 09.)
A single-producer single-consumer version with atomics for `head` and `tail` is the basis of
lock-free queues between two threads (04_Concurrency/08_lock_free_basics.md).

### 2.6 Binary heap

A complete binary tree stored in an array. For index i: parent `(i - 1) / 2`, children `2i + 1`
and `2i + 2`. The root holds the minimum (for a min-heap).

- **push**: append at the end, then *sift up* (swap with the parent while smaller): O(log n).
- **pop**: take the root, move the last element to the root, *sift down* (swap with the smaller
  child while larger): O(log n).
- **heapify** an existing array bottom-up: O(n), not O(n log n).
- **Top-k from a stream**: keep a **min**-heap of size k; replace the root when a bigger item
  arrives. O(n log k) time, O(k) memory.

Servers use heaps for timers (the next timeout is at the root) and schedulers.

### 2.7 Three ways to be generic in C

| Approach | Example | Type safety | Speed | Cost |
|----------|---------|-------------|-------|------|
| `void *` + element size + function pointers | `qsort`, `bsearch`, `struct vec` in `m11_vec.c`, `struct heap` in `m11_ring_heap.c` | none: a wrong cast compiles | `memcpy` of a runtime size per element; comparator is an indirect call that usually cannot be inlined | one copy of the code |
| Macros that stamp out typed code | `DEFINE_VEC(T, name)` in `m11_vec.c`; BSD `<sys/queue.h>`; klib's `khash.h` | full | as fast as hand-written | code bloat, unreadable compiler errors, debugger steps into macros |
| Intrusive links | `list_head` + `container_of` (Linux), `LIST_ENTRY` + `CONTAINING_RECORD` (Windows) | partial (the `container_of` type is trusted) | best: no extra allocation or indirection | the object's struct must know which lists it can be on |

The comparator rule for the `void *` approach: **never `return a - b;`**. For `int`s,
`2000000000 - (-2000000000)` overflows (UB) and gives the wrong sign. Write
`return (a > b) - (a < b);`.

C++ templates are the third row done properly: the compiler generates typed code, type-checks it
and can inline the comparator. That is why `std::sort` with a lambda is usually faster than
`qsort` on the same data.

## 3. Code

### 3.1 `m11_vec.c`: generic vector and macro-generated typed vector

```c
static int vec_reserve(struct vec *v, size_t want)
{
    if (want <= v->cap)
        return 0;
    size_t ncap = v->cap ? v->cap : 8;
    while (ncap < want) {
        if (ncap > SIZE_MAX / 2)
            return ENOMEM;
        ncap *= 2;
    }
    if (ncap > SIZE_MAX / v->elem_size)
        return ENOMEM;
    void *p = realloc(v->data, ncap * v->elem_size);
    if (!p)
        return ENOMEM;        /* v->data is still valid: nothing lost, nothing dangling */
    v->data = p;
    v->cap = ncap;
    v->reallocs++;
    return 0;
}

#define DEFINE_VEC(T, name)                                                     \
    struct name { T *data; size_t len; size_t cap; };                           \
    static inline int name##_push(struct name *v, T x) { /* grow, then */      \
        /* ... */ v->data[v->len++] = x; return 0; }

DEFINE_VEC(double, dvec)            /* no ';' after it: -Wpedantic rejects a stray one */
DEFINE_VEC(struct txn, txnvec)
```

(The macro above is shortened; the file has the full body.) Real output:

```text
1000 pushes: capacity 1024, 8 reallocations
all checks passed
```

Eight reallocations for 1000 pushes: capacities 8, 16, 32, ..., 1024. The test also sorts
`struct txn` records with a two-key comparator, uses `bsearch`, removes with `memmove`, and
shows that `2000000000 - (-2000000000)` does not fit in an `int`.

### 3.2 `m11_lists.c`: pointer-to-pointer singly list, and an intrusive LRU

The session table keeps every session on two intrusive lists at once and evicts from the tail
of the LRU list:

```c
static void session_touch(struct session_table *t, struct session *s)
{
    list_del(&s->lru_link);
    list_add(&s->lru_link, &t->lru);      /* O(1) move to front: no search */
}

static struct session *session_evict_lru(struct session_table *t)
{
    if (list_empty(&t->lru))
        return NULL;
    struct session *s = list_entry(t->lru.prev, struct session, lru_link);
    list_del(&s->lru_link);
    list_del(&s->all_link);               /* O(1) removal from the other list too */
    t->count--;
    return s;                             /* caller owns it now */
}
```

Real output:

```text
evicted session 101 (bilal); container_of recovered it from &lru_link
offsetof(struct session, lru_link) = 40
all checks passed
```

`offsetof` is 40 because `id` (4 bytes) + `user` (16 bytes) + 4 bytes of padding + `all_link`
(16 bytes) come first. `container_of` subtracts exactly that from the address of `lru_link`.
Add a hash table from session id to `struct session *` and you have the classic O(1) LRU cache.

### 3.3 `m11_hash_table.c`: linear probing with tombstones

```c
static struct slot *ht_probe(const struct htab *t, const char *key, uint64_t h, int for_insert)
{
    size_t mask = t->cap - 1;
    struct slot *first_tomb = NULL;
    for (size_t i = (size_t)h & mask;; i = (i + 1) & mask) {
        struct slot *s = &t->slots[i];
        if (s->state == SLOT_EMPTY)
            return for_insert ? (first_tomb ? first_tomb : s) : NULL;
        if (s->state == SLOT_TOMB) {
            if (!first_tomb)
                first_tomb = s;
        } else if (s->hash == h && strcmp(s->key, key) == 0) {
            return s;
        }
    }
}
```

The loop has no exit condition of its own: it terminates because the load rule guarantees at
least one EMPTY slot. That invariant is the kind of thing you write next to the code. Real output:

```text
after 10000 inserts: cap=16384 live=10000 load=0.61 rebuilds=11
after 200000 put/del pairs: cap=16384 live=5000 tombs=4600 rebuilds=28
all checks passed
```

The churn test inserts and deletes 200 000 temporary keys. Tombstones pile up, a same-size
rebuild clears them 17 times, and the capacity never grows: memory stays flat under a
workload of short-lived keys, such as sessions that log in and out.

### 3.4 `m11_ring_heap.c`: ring buffer across the wrap point, generic heap, top-k

The ring test starts both counters at `UINT_MAX - 3` so the head wraps during the test, and the
heap is checked against `qsort` on 1000 values and then used for a top-3 query over 10 000
transactions:

```text
ring: counters wrapped (head=4 tail=4), FIFO order kept
top-3 amounts: 999745 999730 999678
all checks passed
```

## 4. Under the hood (senior-level)

**What `realloc` really costs.** glibc first tries to grow the block in place (if the next
chunk is free or it is the top chunk). Blocks above the mmap threshold (128 KiB by default,
adjusted dynamically) live in their own `mmap` mapping, and growing them uses `mremap`, which
moves page-table entries instead of copying bytes. So a vector of a few MiB grows cheaply, while
a vector of 64 KiB records in a fragmented heap copies. Either way, your pointers into it are
invalid. See 05_Memory_Debugging_RCA/03_the_heap_malloc_internals.md.

**Cache behaviour decides more than big-O.** A cache line is 64 bytes on current x86 CPUs. An
array of 8-byte values gives eight elements per miss, and the hardware prefetcher hides most
misses on a sequential scan. A linked list gives one element per miss and the prefetcher cannot
guess the next address. Linear probing works well for the same reason: the next probe is usually
in the same or the next cache line. A binary heap's children are at `2i + 1` and `2i + 2`, next
to each other, so a sift-down touches one line per level.

**`void *` genericity in machine code.** `qsort` calls your comparator through a function
pointer for every comparison: an indirect call the compiler cannot inline, and it copies
elements with a runtime-sized loop. A typed macro version or a C++ template sees the element
type and the comparator at compile time, inlines both, and can vectorise. With link-time
optimisation (`-flto`) GCC can sometimes specialise a `static` generic function for a constant
comparator, but you should not rely on it.

**`container_of` and the standard.** It subtracts an offset from a pointer to a member to get
a pointer to the enclosing struct. This relies on the member really being inside a struct of
that type; if you pass a `list_head` that is not embedded in a `struct session`, the result
points at garbage and nothing checks it. The kernel's version adds a compile-time type check
using `typeof`, a GNU extension (standard in C23). The conversion through `char *` is what keeps
the arithmetic byte-exact.

**Tombstones and latency.** Without same-size rebuilds, a table under insert/delete churn fills
with tombstones until every miss scans the whole array. The rebuild itself is O(n) and happens
on some unlucky insert: a latency spike. Production tables either rebuild incrementally (move a
few slots per operation) or use a deletion scheme that needs no tombstones, such as backward-shift
deletion in Robin Hood hashing.

## 5. In production

- **The dangling element pointer.** `struct conn *c = vec_at(&conns, i);` followed later by
  `vec_push(&conns, &new_conn);` and then `c->state = ...`. If the push reallocated, `c` points
  into freed memory. In a server this shows up as a corrupted connection object only under load
  (when the vector grows). Rule: store indexes or ids, not pointers into growable arrays; or use
  an intrusive list or a pool so addresses are stable (module 12).
- **A server's core structures are this module.** A connection table keyed by socket or session
  id (hash table), a request queue between the acceptor and the workers (ring buffer or intrusive
  list protected by a lock, module 14), timers for idle connections (heap), and an LRU cache of
  account records (hash table plus intrusive list). Knowing how each one fails is most of
  debugging a C server.
- **Unbounded growth.** Every container needs a maximum, enforced at insert, with a clear error.
  A queue with no bound turns a slow database into an out-of-memory crash instead of back-pressure.
- **Hash flooding.** If request fields become hash keys, an attacker can send keys that all
  collide. With a public, unkeyed hash such as FNV-1a, CPU goes to 100% on one thread. Use a keyed
  hash with a per-process random seed for anything derived from input.
- **Old codebases have several hand-written lists.** Before fixing a bug in one, check whether
  the others have the same bug (`grep` for the same idiom). That is often where the second and
  third production incidents are hiding.

## 6. Try it

1. **Easy - shrink policy.** Add `vec_shrink_to_fit` to `m11_vec.c`, and make `vec_pop` halve
   the capacity when the length drops below a quarter of it. Done when a test that pushes 1000
   and pops 990 ends with a capacity of at most 64 and no reallocation ping-pong when you then
   alternate one push and one pop.
2. **Easy - reverse and find the middle.** Add `slist_reverse` and `slist_middle` to the singly
   list in `m11_lists.c` using the pointer techniques from module 16. Done when both pass on
   lists of length 0, 1, 2 and 5.
3. **Medium - O(1) LRU cache.** Combine `m11_hash_table.c` (key: account id, value: pointer to a
   `struct session`) with the intrusive LRU list from `m11_lists.c` into `lru_get` and `lru_put`
   with a fixed capacity. Done when every operation is O(1) and a test with capacity 3 evicts in
   the right order. Compare with 03_Data_Structures_Algorithms/05_linked_lists.md.
4. **Medium - typed heap by macro.** Write `DEFINE_HEAP(T, name, less)` that generates a typed
   heap where `less` is a macro or inline function. Benchmark it against the generic heap on
   1 000 000 ints. Done when you have both timings and can explain the difference with section 4.
5. **Hard - backward-shift deletion.** Replace tombstones in `m11_hash_table.c` with
   backward-shift deletion (after removing a slot, shift following entries back if that moves
   them closer to their home slot). Done when the churn test passes with zero tombstones and no
   same-size rebuilds.
6. **Hard - lock-free SPSC ring.** Make the ring buffer safe for exactly one producer thread and
   one consumer thread using `_Atomic unsigned` head and tail with release/acquire ordering.
   Done when a 10 000 000-item test passes and TSan is clean (module 14).

## 7. Interview questions

**Q: How does a dynamic array achieve amortised O(1) append?**
A: It grows its capacity geometrically, usually by a factor of 1.5 or 2, so each reallocation
copies all elements but happens after a number of pushes proportional to the current size. Over
n pushes the total copying is O(n), so each push is O(1) on average. Growing by a constant amount
would make the total copying O(n squared).

**Q: What is an intrusive list and why does the Linux kernel use it?**
A: The link fields live inside the object (`struct list_head` embedded in the struct) instead of
in a separately allocated node that points to the object. Adding to or removing from a list then
needs no allocation, an object can be on several lists at once, and removal given the object is
O(1). `container_of` recovers the object from the link by subtracting the member's `offsetof`.

**Q: Why does open addressing need tombstones, and what is their cost?**
A: A lookup stops at the first empty slot, so deleting an entry by emptying its slot would break
the probe chain for keys inserted after it. A tombstone marks the slot as deleted: lookups skip
it and inserts may reuse it. Tombstones lengthen probes and count toward the load factor, so the
table must periodically rebuild to drop them.

**Q: How do you write a generic container in C?**
A: Either store elements as bytes with `void *`, an element size and function pointers for
comparison (like `qsort`), which is one copy of code but has no type safety and an indirect call
per comparison; or generate typed code per element type with macros, which is type-safe and fast
but bloats code and is hard to debug; or embed intrusive links in the element type. C++
templates do the macro approach with type checking.

**Q: Why is `return a - b;` a bad comparator?**
A: For signed integers the subtraction can overflow, which is undefined behaviour and in practice
gives the wrong sign, so the sort silently produces a wrong order. Use
`return (a > b) - (a < b);`, which yields -1, 0 or 1 without arithmetic overflow.

**Q: How do you tell "full" from "empty" in a ring buffer?**
A: Either waste one slot (full when `(head + 1) % cap == tail`), keep a separate count, or use
free-running unsigned head and tail counters with a power-of-two capacity, where
`head - tail` is the count even after wraparound because unsigned arithmetic wraps by definition.

**Q: You store a pointer to a vector element and later the program crashes reading it. Why?**
A: A later append reallocated the array to a new address and freed the old block, so the stored
pointer dangles into freed memory. Store an index or id, reserve the final capacity up front, or
use a structure with stable addresses such as a pool or an intrusive list.

## 8. Recap

- Every C container design answers two questions: who owns the memory, and how it is generic.
- Dynamic array: geometric growth for amortised O(1), overflow-checked sizes, `realloc` into a
  temporary, and pointers into it die on growth.
- Singly list: the pointer-to-pointer walk removes the head special case; read `next` before
  `free`.
- Intrusive lists embed `list_head` in the object; `container_of` uses `offsetof`; circular with
  a sentinel means no NULL checks; Windows calls it `LIST_ENTRY` and `CONTAINING_RECORD`.
- Open addressing: power-of-two capacity and a mask, stored hashes, tombstones on delete,
  rebuild below about 70% load, keyed hashes for untrusted keys.
- Ring buffer: free-running unsigned counters; `head - tail` is the count.
- Heap: array layout `(i-1)/2`, `2i+1`, `2i+2`; push and pop O(log n); top-k with a min-heap of
  size k.
- Generic C: `void *` + size + comparator, typed macros, or intrusive; never `return a - b`.
- Cache behaviour (arrays and probing good, pointer chasing bad) matters as much as big-O.
