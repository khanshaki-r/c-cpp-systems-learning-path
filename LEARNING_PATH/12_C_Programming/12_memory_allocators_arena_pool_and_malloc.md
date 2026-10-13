# 12. Memory allocators: arena, pool and a teaching malloc

**Time:** ~3 hours   **Prerequisites:** modules 04 (memory, storage and lifetime), 06 (structs and layout), 10, 11; 05_Memory_Debugging_RCA/03_the_heap_malloc_internals.md (sections 1 and 2)

## 1. In plain words

`malloc` is a general-purpose allocator. It accepts any size, any order of `free` calls, from
any thread, and it must never lose track of a byte. That generality has a price: a header in
front of every block, locks or per-thread caches, searching for a block that fits, fragmentation
over time, and latency that varies from call to call. And when your code misuses memory, `malloc`
usually notices much later, somewhere else, or never.

When you **know** how your program allocates, a special-purpose allocator is simpler, faster and
safer:

- **Arena (bump) allocator.** You allocate many objects that all die at the same moment (one
  request, one batch, one parse). Allocation is "move a pointer forward"; freeing is "reset the
  pointer". No per-object `free`, so no per-object leaks or double frees.
- **Pool allocator.** You allocate many objects of the same size and free them in any order
  (connections, request records, list nodes). Every slot is the same size, so there is no
  searching and no fragmentation.

A restaurant analogy: `malloc` is the a la carte kitchen that can cook anything in any order.
An arena is the buffet tray: fill it during the event, then wash the whole tray at once. A pool
is an egg carton: identical slots, take one, put one back.

This module builds both, then builds a small but real `malloc` so that glibc's internals and
its error messages make sense, and finishes with a debug allocator that catches overflows,
double frees, use after free and leaks with the file and line of the allocation.

## 2. The real thing

### 2.1 Why write your own allocator

| Goal | How a custom allocator gets it |
|------|-------------------------------|
| Speed | Bump-pointer or free-list pop: a few instructions, no lock, no search |
| Predictable latency | No coalescing or searching at random moments; important on hot paths |
| No fragmentation | Same-size slots (pool) or whole-region reset (arena) |
| Fewer lifetime bugs | One `reset` instead of thousands of `free`s; nothing to forget |
| Memory accounting | "This batch used 3.2 MiB" is one counter, not a heap profile |
| Debugging | Guard bytes, fill patterns, allocation tags with file and line |

The costs are real too: you must state the lifetime rules and everyone must follow them, and a
custom allocator **hides misuse from ASan and Valgrind** unless you tell them about it (section 5).

### 2.2 Alignment math

Every type has an alignment: its address must be a multiple of `alignof(T)`. On x86-64,
`alignof(double)` is 8 and `alignof(max_align_t)` is 16, which is why `malloc` returns 16-byte
aligned memory on glibc. A misaligned access is undefined behaviour in C (module 09); on x86 it
is usually just slower, on other CPUs and with SSE or atomic instructions it can fault.

Alignments are powers of two, which makes rounding up a bit trick:

```text
align_up(x, a) = (x + (a - 1)) & ~(a - 1)        a must be a power of two

x = 13, a = 8:   13 + 7 = 20 = 0b10100
                 ~(8 - 1)    = ...11111000
                 20 & ~7     = 0b10000 = 16      13 rounded up to 16
x = 16, a = 8:   16 + 7 = 23, 23 & ~7 = 16       already aligned: unchanged
is_power_of_two(a) = a != 0 && (a & (a - 1)) == 0
```

Rules that follow for allocator writers: every header must be a multiple of the largest
alignment you promise (or the payload after it is misaligned); a block's size should be a
multiple of that alignment too, so the next block starts aligned; and C11 gives you `alignof`,
`alignas` and `aligned_alloc(alignment, size)` (size must be a multiple of the alignment).

### 2.3 Arena allocator and its ownership contract

```text
chunk (newest)                              chunk (older)
+------+---------------------------------+  +------+-------------------------------+
| prev | cap | used | [obj][pad][obj] ... |->| prev | cap | used | [obj][obj]....... |-> NULL
+------+---------------------------------+  +------+-------------------------------+
                         ^ next allocation = align_up(data + used)
```

- **Allocate**: round the current offset up to the requested alignment; if the object fits,
  advance `used` and return the address. If not, allocate a new chunk (big enough for this
  request even if it is larger than the usual chunk size) and push it on the chunk stack.
- **Mark and rewind**: save `(chunk, used)`, allocate temporaries, then rewind to the mark. This
  turns an arena into a scratch stack for one function.
- **Reset**: free everything at once. `m12_arena.c` keeps **one** chunk sized to the high-water
  mark, so a workload that repeats (batch after batch) settles into **zero** `malloc` calls.
- **No individual free.** `realloc` can only grow the most recent allocation in place.

The ownership contract, which is the part that prevents bugs. Put it in the header:

1. Memory from the arena is owned by the arena. Never `free()` it.
2. It stays valid until `arena_reset`, a rewind past it, or `arena_destroy`.
3. Nothing allocated from an arena may be stored in a structure that outlives the arena's next
   reset (a global cache, another thread's queue). Copy it out instead.
4. An arena is not thread-safe. Use one per thread or one per batch.

**Where arenas fit.** Anything batch- or request-scoped: an RPC request whose decoded fields and
response buffers all die when the response is sent; a parser building a tree that is thrown away
after evaluation; and a data-migration job that reads N rows from a source store (for example a
Berkeley DB cursor), converts them into structs, strings and SQL parameter buffers, writes them to
the target (for example YugabyteDB), and then needs none of it. One arena per batch, reset after
each commit, gives constant memory per batch and makes leaks impossible by construction.

### 2.4 Fixed-size pool with an embedded free list

```text
one malloc:  [ slot 0 ][ slot 1 ][ slot 2 ][ slot 3 ] ... [ slot N-1 ]
free list:   head -> slot 2 -> slot 0 -> slot 3 -> NULL
             (a free slot's first 8 bytes hold the pointer to the next free slot)
```

- `alloc` pops the head, `free` pushes the slot back: O(1), no header per object.
- **LIFO reuse**: the slot freed last is handed out next, and it is probably still in cache.
- Slot size = `max(object size, sizeof(void *))`, rounded up to the alignment you promise.
- Debug checks that cost almost nothing: is the pointer inside the pool, on a slot boundary, and
  currently allocated? That catches foreign pointers, interior pointers and double frees.
- A **slab allocator** (the Linux kernel's `kmem_cache`, and the core of jemalloc, tcmalloc and
  mimalloc) is a set of pools, one per size class, that grows by adding pages.

### 2.5 A teaching malloc: headers, free list, splitting, coalescing

`m12_teaching_malloc.c` manages one 64 KiB region. Every block carries its size in a header
**and** a footer (a "boundary tag"). Sizes are multiples of 16, so the low 4 bits are free for
flags; bit 0 means "in use".

```text
   0       8          16         24        32
   | pad   | prologue | prologue | header  | payload (16-aligned) ... | footer | ... | epilogue |
           | 16 | 1   | 16 | 1   | size|u  |                          | size|u |     |  0 | 1   |

free block payload: [ next free ][ prev free ] ......
minimum block: header 8 + next 8 + prev 8 + footer 8 = 32 bytes
```

- **malloc(n)**: block size = `align_up(n + 16, 16)`, at least 32. Walk the **explicit free
  list** (only free blocks are linked) and take the **first fit**.
- **Splitting**: if the free block is at least 32 bytes bigger than needed, cut it in two and put
  the tail back on the free list. Otherwise hand out the whole block (the waste inside is
  **internal fragmentation**).
- **free(p)**: mark the block free, then **coalesce**: if the next block is free (its header says
  so), merge; if the previous block is free (its *footer*, just before our header, says so),
  merge. The footer is what makes "is my left neighbour free?" an O(1) question.
- **Prologue and epilogue** blocks are permanently "in use", so coalescing never needs bounds checks.
- **A heap checker** (`tm_check`) walks every block and verifies alignment, header equals footer,
  no two adjacent free blocks, and that the free list matches the heap. You write this first; it
  turns allocator bugs into immediate assertion failures.

**External fragmentation** is the problem no first-fit allocator escapes: plenty of free bytes,
but in pieces too small for the request. The test fills the heap with 48-byte requests, frees
every other one, and asks for 100 bytes (section 3.3).

**From here to glibc** (05_Memory_Debugging_RCA/03_the_heap_malloc_internals.md): glibc's chunk
header has the same size-plus-flag-bits design (`PREV_INUSE`, `IS_MMAPPED`, `NON_MAIN_ARENA`),
and it uses the previous chunk's size field the way our footer works. Instead of one free list it
has many: per-thread **tcache** bins and fastbins for small sizes (not coalesced immediately),
an unsorted bin, and sorted small and large bins. It grows the heap with `brk`, serves big
requests with `mmap`, and keeps several **arenas** so threads do not fight over one lock. The
error messages you see in crash logs, such as `free(): invalid next size` or
`double free or corruption (!prev)`, are glibc's versions of the checks in `tm_check`.

### 2.6 Debug allocators: guards, canaries, fill patterns, quarantine

A debug allocator wraps every block with extra bytes and bookkeeping:

```text
[ header: magic, size, file, line, live-list links ][ front guard 0xFD x16 ][ user bytes ][ back guard 0xFD x16 ]
```

| Technique | Catches | When |
|-----------|---------|------|
| Guard bytes (red zones) with a known pattern | overflow and underflow | at `free`, or on a periodic check |
| Header magic (canary) | freeing a pointer you never returned; double free | at `free` |
| Fill new memory with `0xCD` | reading before writing: values look like `0xCDCDCDCD` | in the debugger |
| Fill freed memory with `0xDD` and **quarantine** it | use after free (writes break the pattern; reads see `0xDD`) | when the block leaves quarantine |
| Live list with file and line | leaks | at exit or on demand |

The byte values are the ones the MSVC debug CRT uses, so they will look familiar in a Windows
crash dump: `0xCD` clean (allocated, never written), `0xDD` dead (freed), `0xFD` "no man's land"
(guards). Related platform tools: the MSVC debug heap (`_CrtSetDbgFlag`, `_CrtCheckMemory`);
**PageHeap** (`gflags /p /enable app.exe /full`), which puts every allocation at the end of a
page followed by an inaccessible guard page so an overflow faults on the exact instruction; and
on glibc, `MALLOC_PERTURB_` (fill patterns) and `MALLOC_CHECK_=3` (since glibc 2.34 it needs
`LD_PRELOAD=libc_malloc_debug.so.0`). ASan is the industrial
version: red zones and quarantine, with shadow memory checked on every access instead of only at
`free` (05_Memory_Debugging_RCA/08_valgrind_and_sanitizers.md).

## 3. Code

### 3.1 `m12_arena.c`

```c
static void *chunk_take(struct chunk *c, size_t size, size_t align)
{
    uintptr_t base = (uintptr_t)c->data;
    uintptr_t p = align_up(base + c->used, align);
    size_t offset = (size_t)(p - base);
    if (offset > c->cap || size > c->cap - offset)   /* written so nothing can overflow */
        return NULL;
    c->used = offset + size;
    return (void *)p;
}

#define ARENA_NEW(a, T)        ((T *)arena_alloc((a), sizeof(T), alignof(T)))
#define ARENA_ARRAY(a, T, n)   ((T *)arena_alloc_array((a), (n), sizeof(T), alignof(T)))
```

Note the bounds check: `size > c->cap - offset` instead of `offset + size > c->cap`, because the
addition could wrap for a huge `size`. The batch test converts 1000 rows per batch (a pointer
array, a `struct row` and a string per row) and resets after each batch. Real output:

```text
batch 1: 3 malloc calls; batches 2-51: 0 malloc calls in total
all checks passed
```

The first batch needed three 16 KiB chunks. `arena_reset` replaced them with one chunk of the
combined size, and the next 50 batches ran without a single `malloc`.

### 3.2 `m12_pool.c`

```c
static void *pool_alloc(struct pool *p)
{
    void *slot = p->free_list;
    if (!slot)
        return NULL;                                   /* exhausted: caller decides */
    memcpy(&p->free_list, slot, sizeof(void *));       /* free_list = slot->next */
    p->state[((unsigned char *)slot - p->mem) / p->slot_size] = 1;
    p->in_use++;
    return slot;
}
```

`memcpy` reads and writes the embedded next pointer without any alignment or aliasing
assumptions. Real output:

```text
sizeof(struct request)=40, slot_size=48, slots=64
double free -> 3, foreign pointer -> 1, interior pointer -> 2 (all rejected)
all checks passed
```

A 40-byte object gets a 48-byte slot because slots are rounded to `alignof(max_align_t)` = 16.
The file then runs a million alloc/free pairs with no `malloc` involved.

### 3.3 `m12_teaching_malloc.c`

```c
static void tm_free(void *ptr)
{
    /* ... validity and double-free check ... */
    size_t size = tag_size(hdr(bp));
    byte *next = next_blk(bp);
    int prev_used = tag_used(prev_ftr(bp));    /* the boundary tag makes this O(1) */
    int next_used = tag_used(hdr(next));

    if (!next_used) {                          /* merge with the following block */
        list_remove(next);
        size += tag_size(hdr(next));
    }
    if (!prev_used) {                          /* merge with the preceding block */
        byte *prev = prev_blk(bp);
        list_remove(prev);
        size += tag_size(hdr(prev));
        bp = prev;
    }
    set_block(bp, size, 0);
    list_insert(bp);
}
```

All header and footer access goes through `memcpy`-based helpers, and the region comes from
`aligned_alloc` rather than a static `unsigned char` array, so the allocator itself has no
aliasing or alignment UB. Real output:

```text
after freeing a, b, c: 1 free block of 65504 bytes (fully coalesced)
stress: 20000 random ops, heap valid after each, 2 out-of-memory results
fragmented: 32800 bytes free in 512 blocks, largest 96 -> malloc(100) FAILED
all checks passed
```

The last line is external fragmentation made visible: 32 800 bytes free, but no single block can
hold a 100-byte request (which needs a 128-byte block).

### 3.4 `m12_debug_alloc.c`

```c
#define DMALLOC(n) dbg_malloc((n), __FILE__, __LINE__)
#define DFREE(p)   dbg_free((p), __FILE__, __LINE__)

char *s = DMALLOC(8);
memcpy(s, "ACC-1001", 9);            /* 8 chars + NUL = 9 bytes into an 8-byte block */
assert(dbg_check_all() == 1);        /* found before free, by the periodic checker */
r = DFREE(s);
assert(r == DBG_OVERFLOW);
```

Real output:

```text
uninitialised int read from a fresh block: 0xCDCDCDCD
dbg: OVERFLOW past 8-byte block from m12_debug_alloc.c:191, detected at free m12_debug_alloc.c:194
dbg: UNDERFLOW before 16-byte block from m12_debug_alloc.c:198, detected at free m12_debug_alloc.c:200
dbg: DOUBLE FREE at m12_debug_alloc.c:207 of block allocated at m12_debug_alloc.c:204
dbg: WRITE AFTER FREE into 40-byte block from m12_debug_alloc.c:211
dbg: LEAK 7 bytes allocated at m12_debug_alloc.c:220
dbg: LEAK 100 bytes allocated at m12_debug_alloc.c:219
all checks passed
```

The off-by-one in the overflow case is the most common C string bug there is: forgetting the
byte for the terminating NUL. Every deliberate "bug" in the test stays inside memory the debug
allocator owns, so the test itself is defined behaviour and runs clean under ASan and Valgrind.

## 4. Under the hood (senior-level)

**How glibc gets and gives back memory.** The main arena grows the heap with `brk`; requests
above the mmap threshold (128 KiB initially, raised dynamically up to 32 MiB on 64-bit) get their
own `mmap` mapping and are unmapped on `free`. Memory freed inside the `brk` heap returns to the
OS only when the free space at the top exceeds `M_TRIM_THRESHOLD`, or when you call
`malloc_trim(0)`. That is why RSS often stays high after a burst even with no leak.

**Per-allocation overhead.** On 64-bit glibc the smallest chunk is 32 bytes, so `malloc(1)` and
`malloc(24)` cost the same 32 bytes; each chunk carries an 8-byte size field. A pool slot has no
header at all, and an arena object has only alignment padding. For millions of small objects
that difference is the size of the process.

**Threads.** glibc gives each thread a tcache (up to 7 cached chunks per size, for sizes up to
1032 bytes by default) that needs no lock, and creates extra arenas (up to 8 x the number of
cores on 64-bit) when threads contend. Memory freed by one thread into another arena's bins is
the reason multithreaded servers sometimes show RSS far above their live data; `MALLOC_ARENA_MAX`
caps the number of arenas. jemalloc, tcmalloc and mimalloc are built from size-class slabs plus
thread caches, which is section 2.4 scaled up.

**Locality.** A bump allocator returns consecutive addresses, so objects created together sit
together in cache lines and pages: walking a batch of rows allocated from an arena touches far
fewer pages than walking the same rows scattered across the general heap. A pool returns the most
recently freed slot first for the same reason. The flip side: a pool shared by threads hands out
neighbouring slots to different threads, which can put two hot objects in one cache line (false
sharing, 04_Concurrency/10_cache_lines_false_sharing_performance.md).

**Why a custom allocator blinds ASan.** ASan instruments `malloc` and `free`. If your arena takes
one 16 KiB chunk from `malloc` and carves objects out of it, ASan sees one valid 16 KiB block:
overflowing one object into the next, or using an object after `arena_reset`, is invisible.
Fix it with the manual poisoning API (`ASAN_POISON_MEMORY_REGION` and
`ASAN_UNPOISON_MEMORY_REGION` from `<sanitizer/asan_interface.h>`) on reset and between objects,
or with a build switch that routes every arena allocation to plain `malloc` in sanitizer builds.

## 5. In production

- **The classic arena bug is a broken contract, not a broken allocator.** Someone stores a
  pointer to an arena-allocated string in a long-lived cache "to save a copy". After the next
  reset the cache points at memory now holding the next batch's data: no crash, just wrong
  values, which in a banking system means wrong account data. Defend the contract in code review,
  in the header comment, and with the sanitizer build switch from section 4.
- **Pools need a sizing policy.** A pool that returns NULL when exhausted must be sized for the
  peak, or allowed to grow by adding blocks. Expose `in_use` and `capacity` as metrics; a pool at
  95% is an early warning that a matching `free` is missing somewhere (a leak in pool form).
- **Windows has arenas built in.** `HeapCreate` gives a private heap; `HeapDestroy` frees
  everything in it at once, which is an arena with a general allocator inside. A per-subsystem
  heap also makes heap corruption easier to localise in a dump: the corrupted heap tells you which
  subsystem did it. The Low Fragmentation Heap (LFH) is Windows' size-class pool layer.
- **Use the debug allocator to catch the bug where it happens.** Heap corruption in a C server
  usually crashes far from the bug, in an innocent `malloc`. Turning on guard-byte checking in a
  test environment, or calling a `check_all` function from a debug command or a timer, moves the
  detection close to the culprit. On Windows, enable full PageHeap for the process in a test
  environment and the first overflowing write faults with the guilty stack.
- **Measure before replacing `malloc`.** glibc's allocator is good for general workloads. Custom
  allocators pay off for clear patterns: per-request or per-batch lifetimes, millions of
  same-size objects, or hard latency limits. Profile first (`perf`, `heaptrack`).

## 6. Try it

1. **Easy - alignment by hand.** Without running code, compute `align_up(x, a)` for
   (1, 8), (8, 8), (9, 16), (100, 64), (0, 32), and explain why `align_up(5, 12)` is meaningless.
   Done when you check your answers with a five-line C program.
2. **Easy - arena_realloc.** Add `arena_realloc(a, p, old, new)` that grows in place when `p` is
   the most recent allocation and there is room, and otherwise allocates and copies. Done when a
   test shows the in-place case returns the same pointer and the other case copies correctly.
3. **Medium - best fit.** Change `tm_malloc` in `m12_teaching_malloc.c` to best fit and rerun the
   stress and fragmentation tests. Done when you can report the out-of-memory count and the
   largest free block for both policies and explain the difference.
4. **Medium - tm_realloc.** Implement `tm_realloc` that grows in place by absorbing a free next
   block (and splitting off the excess), shrinks in place by splitting, and otherwise falls back
   to malloc-copy-free. Done when `tm_check` passes after every call in a random test.
5. **Hard - ASan-aware arena.** Add `ASAN_POISON_MEMORY_REGION` calls to `m12_arena.c` so that
   the unused tail of each chunk and everything released by `arena_rewind` and `arena_reset` is
   poisoned. Build with `-fsanitize=address` and write a test that reads an object after reset.
   Done when ASan reports `use-after-poison` on that read and the normal tests still pass.
6. **Hard - thread-local pools.** Give each worker thread its own pool through a `_Thread_local`
   pointer, with a fallback to a shared, mutex-protected pool when the local one is empty. Done
   when a 4-thread test runs clean under TSan and you have measured it against plain `malloc`.

## 7. Interview questions

**Q: When would you use an arena allocator instead of malloc?**
A: When many objects share one lifetime, such as everything for one request, one parse or one
batch. Allocation becomes a pointer bump, there is no per-object free to forget or repeat, and the
whole region is released in one operation. It is wrong for objects with independent lifetimes,
because nothing can be freed individually.

**Q: How does a fixed-size pool allocator work?**
A: It allocates one large block divided into equal slots and threads a singly linked free list
through the free slots themselves, so it needs no extra memory. Allocation pops the list head and
free pushes the slot back, both O(1) with no searching and no fragmentation. Debug builds can
check that a freed pointer lies inside the pool, on a slot boundary, and is currently allocated.

**Q: How do you round a size or address up to an alignment?**
A: For a power-of-two alignment a, `(x + a - 1) & ~(a - 1)`. Adding a - 1 moves x past the next
boundary unless it is already on one, and masking clears the low bits. Headers and block sizes in
an allocator must be multiples of the maximum alignment it promises, so every payload stays aligned.

**Q: What are boundary tags and why does malloc need them?**
A: A footer at the end of each block that repeats its size and in-use flag. When a block is
freed, the allocator reads the footer just before the block's header to learn in O(1) whether the
previous block is free and how big it is, so it can coalesce in both directions. glibc stores the
previous chunk's size for the same purpose, valid only while that chunk is free.

**Q: Internal versus external fragmentation?**
A: Internal fragmentation is waste inside an allocated block, from rounding up to an alignment or
size class. External fragmentation is free memory split into pieces too small for a request,
so allocation fails or grows the heap although enough total memory is free. Pools remove
external fragmentation for one size; arenas avoid it by freeing whole regions.

**Q: How does a debug allocator detect overflows and use after free?**
A: It surrounds each block with guard bytes holding a known pattern and checks them on free or on
demand, which catches overflow and underflow. It fills freed memory with a pattern and keeps it in
a quarantine instead of reusing it, so a write after free changes the pattern and a double free
finds the block already marked dead. ASan does the same with shadow memory, checked on every access.

**Q: Why might a custom allocator make memory bugs harder to find?**
A: ASan and Valgrind only see the large blocks the custom allocator takes from malloc, so overflows
between objects inside a block, or uses after an arena reset, look like valid accesses. You need
manual poisoning, a sanitizer build that bypasses the custom allocator, or the allocator's own
guard checks.

## 8. Recap

- `malloc` is general and pays for it in headers, locking, searching, fragmentation and late
  error detection; a known allocation pattern deserves a matching allocator.
- `align_up(x, a) = (x + a - 1) & ~(a - 1)` for power-of-two a; headers and block sizes must be
  multiples of the promised alignment (16 on x86-64).
- Arena: bump pointer, mark/rewind, reset; the ownership contract (no free, valid until reset,
  never store in longer-lived structures, one per thread) is what makes it safe.
- Resetting to one chunk at the high-water mark gave 0 `malloc` calls for batches 2 to 51.
- Pool: equal slots, embedded free list, O(1), LIFO reuse; cheap debug checks catch double free,
  foreign and interior pointers.
- Teaching malloc: header and footer tags, explicit free list, first fit, split, coalesce both
  ways, prologue/epilogue, and a heap checker written first.
- glibc adds tcache, fastbins, sorted bins, arenas per thread, `brk` and `mmap`; its abort
  messages are failed consistency checks like `tm_check`.
- Debug allocators: guards (`0xFD`), fill patterns (`0xCD` new, `0xDD` freed), header magic,
  quarantine, leak list with file and line; PageHeap and ASan are the industrial versions.
- Custom allocators hide bugs from ASan unless you poison memory manually or bypass them in
  sanitizer builds.
