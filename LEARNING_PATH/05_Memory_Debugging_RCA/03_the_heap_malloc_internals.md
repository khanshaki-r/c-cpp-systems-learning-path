# 03. The heap: malloc internals

**Time:** ~4 hours   **Prerequisites:** 01

## 1. In plain words

The heap is the pool of memory you ask for with `malloc`/`new` and give back with
`free`/`delete`. The allocator is the library code that manages that pool. It is not the kernel;
it is `malloc` inside the C library. The kernel gives the allocator big slabs of pages; the
allocator chops them into the small pieces your program asks for and remembers which pieces are
free so it can reuse them.

To remember all that, the allocator keeps bookkeeping data right next to your blocks: a little
header just before each block saying how big it is, and when a block is free it reuses the first
few bytes of your old data to link it into a list of free blocks. That is the single most
important fact in this whole course. Your data and the allocator's bookkeeping are interleaved.
So if you write one byte past the end of your array, you are not scribbling on empty space, you
are scribbling on the size field of the next block. The allocator will not notice until much
later, when it tries to use that corrupted size. That is why heap bugs crash far from where they
happen, often in an innocent `free` or `malloc` called by code that did nothing wrong.

Understanding the layout turns a baffling "crash in malloc" into a specific sentence: "something
wrote N bytes past a block of about this size, some time before this `free`." That sentence is
most of an RCA.

## 2. The real thing (glibc ptmalloc2)

glibc's allocator is ptmalloc2, derived from Doug Lea's dlmalloc. The following is x86-64,
glibc 2.35-2.39 (Ubuntu 22.04/24.04).

### 2.1 The chunk

Everything the allocator hands out is a **chunk**. The pointer you get points just past the
chunk header.

```
         +-------------------+  <- chunk start (16-byte aligned)
 -16     | prev_size         |  8 bytes: size of previous chunk, ONLY valid if prev is free
 -8      | size        |A|M|P|  8 bytes: this chunk's size (incl header), low 3 bits are flags
  0      | user data...      |  <- malloc returns THIS address
         | ...               |
         | (prev_size of next chunk can overlap the last 8 bytes of our data if next is in use)
         +-------------------+
```

- Flags in the low 3 bits of `size` (sizes are always multiples of 16, so the bits are free):
  - `P` (PREV_INUSE, 0x1): the **previous** chunk is in use. If clear, `prev_size` is valid and
    the previous chunk can be coalesced with this one.
  - `M` (IS_MMAPPED, 0x2): this chunk came from `mmap` (a large request), not an arena.
  - `A` (NON_MAIN_ARENA, 0x4): this chunk belongs to a secondary arena, not the main one.
- Minimum chunk size is 32 bytes; sizes round up so the usable size is a multiple of 16. The
  header steals 8 bytes, but because the `prev_size` of the *next* chunk can be borrowed when
  this chunk is in use, `malloc(24)` fits in a 32-byte chunk (`size` field `0x21` = 32 | P).
  `malloc(25)` needs 48 (`0x31`). This off-by-one in your head matters when reading a dump.

`code/m03_chunk_inspect.c` prints exactly this for several sizes. Run it before reading on.

### 2.2 Free lists: bins

When you `free` a chunk it is not returned to the OS; it goes on a free list ("bin") so the next
`malloc` can reuse it fast. ptmalloc has several, fastest first:

| Bin | What it holds | Policy | Coalesce? |
|-----|---------------|--------|-----------|
| **tcache** (per-thread, glibc 2.26+) | up to 7 chunks per size class, sizes 32..1040 bytes | LIFO singly-linked, no locking | no |
| **fastbins** | small chunks (<= 160 bytes default), per size | LIFO singly-linked, lock held | no (deferred) |
| **unsorted bin** | recently freed/split chunks, mixed sizes | FIFO, a staging area | on the way through |
| **small bins** | exact sizes < 1024 bytes | doubly-linked, FIFO per size | yes |
| **large bins** | size ranges >= 1024 bytes | doubly-linked, sorted | yes |
| **top chunk** | the one big free chunk at the end of the arena | split from / returned to | grows via `brk`/`mmap` |

A `malloc` checks tcache, then fastbins, then the unsorted/small/large bins, then splits the top
chunk, then asks the kernel for more (`sbrk`/`brk` for the main arena, `mmap` for others).
A `free` pushes to tcache if there is room, else fastbin for small sizes, else coalesces with
free neighbours and goes to the unsorted bin.

**tcache is why so many things changed since 2.26.** It is per-thread and lock-free, so most
small alloc/free pairs never touch a lock. It is also singly linked through the **first 8 bytes
of the freed user data** (the `next` pointer) with the next 8 bytes as a `key` used to detect
double frees. Two consequences you will see in dumps:

1. After `free(p)`, `p[0..7]` is a pointer (to the next tcache entry, or 0) and `p[8..15]` is the
   tcache key (a per-arena random value). A use-after-free that reads the first 8 bytes of a
   freed object gets a **pointer-looking value** or 0, not your old data. Recognise it.
2. glibc 2.32+ stores that `next` pointer **safe-linked**: `stored = (address_of_next_field >>
   12) XOR next`. So a freed chunk's first word looks like a shifted-and-xored pointer, not a
   clean heap address. `m03_chunk_inspect.c` prints this.

### 2.3 Arenas and threads

- The **main arena** uses the `brk` heap (the `[heap]` mapping). It is one contiguous region
  grown with `sbrk`.
- Secondary arenas are created when threads contend on the main arena's lock. Each is a
  64 MiB `mmap` reservation (committed lazily). A thread sticks to an arena. Default max arenas
  is `8 * number_of_cores` (`MALLOC_ARENA_MAX` overrides). This is why a multithreaded server's
  RSS can be `threads * arena_working_set`, and why `MALLOC_ARENA_MAX=2` is a common tuning knob
  for memory-constrained servers at the cost of lock contention.
- Large requests (>= `M_MMAP_THRESHOLD`, 128 KiB default, auto-tuned upward) bypass arenas and
  get a dedicated `mmap`. `free` on them calls `munmap`, returning memory to the OS immediately.
  That is the one case where memory shrinks right away.

### 2.4 How corruption is detected (the glibc messages)

glibc does cheap consistency checks and aborts with a message when they fail. Memorise these;
each maps to a bug class:

| Message | What glibc noticed | Likely cause |
|---------|--------------------|--------------|
| `free(): invalid pointer` | pointer not 16-aligned, or outside any arena | freeing a non-heap pointer, a stack/global address, or an interior pointer |
| `free(): invalid size` | size field not a sane multiple of 16, or absurd | the size field was overwritten (overflow of the previous chunk) |
| `free(): invalid next size (fast/normal)` | the **next** chunk's size field is insane | overflow past the end of the chunk being freed, corrupting the neighbour's header |
| `malloc(): corrupted top size` | the top chunk's size was overwritten | overflow of the last allocated chunk |
| `free(): double free detected in tcache 2` | tcache key matches -> already in this tcache | double free of a tcache-sized chunk |
| `malloc(): memory corruption (fast)` / `corrupted unsorted chunks` | a bin's linked list is inconsistent | overflow into a free chunk's fd/bk pointers, or a UAF write |
| `malloc_consolidate(): ...` | corruption found while merging fastbins | overflow, surfaced later during consolidation |
| `munmap_chunk(): invalid pointer` | freeing an mmapped chunk whose header is wrong | overflow of a large block, or freeing a `realloc`-moved pointer |

The lesson repeated: the message names the **symptom location** (a `free`/`malloc` operating on
a chunk), not the **bug location** (whoever wrote past a buffer earlier). "invalid next size"
literally means "the chunk after the one I'm freeing has a corrupted header", i.e. someone
overflowed the chunk being freed. Tools in module 08 find the write; glibc only finds the
aftermath.

### 2.5 Fragmentation

Two kinds:

- **External fragmentation**: free memory exists but is split into pieces too small for the next
  request, so the allocator grows the heap instead of reusing. Classic cause: a long-lived small
  allocation sitting at a high address keeps the top chunk from shrinking, and a sea of freed
  small chunks cannot satisfy a medium request. `m09_growth_not_leak.cpp` reproduces it.
- **Internal fragmentation**: rounding up. `malloc(17)` uses a 32-byte chunk; 15 bytes wasted.
  Millions of small allocations waste a lot. `malloc_usable_size(p)` tells you the real size.

Fragmentation is not a leak: the bytes are free, the allocator just cannot give them back. Leak
tools report nothing. You diagnose it with `malloc_stats()`/`mallinfo2()` (in-use bytes much
less than system bytes) and fix it with arena allocators, size-class pooling, or periodic
`malloc_trim(0)` (module 09).

### 2.6 The Windows heap and the LFH

Windows has its own allocator, unrelated to glibc. Several heaps per process: the **process
heap** (`GetProcessHeap`), the **CRT heap** (what C++ `new`/`malloc` use, which is the process
heap on modern CRTs), and any **private heaps** (`HeapCreate`).

- The classic NT heap keeps **free lists** and a header per block. The header is **encoded**
  (XORed with a per-heap random key) since Windows Vista, so a naive overflow of the next block's
  header produces a detectably invalid decode and a **heap corruption** fast-fail.
- The **Low Fragmentation Heap (LFH)** is a front-end, on by default since Vista, that serves
  small blocks (<= 16 KiB) from per-size-class **subsegments** of fixed-size buckets, like a
  slab allocator. It cuts fragmentation and (incidentally) changes corruption behaviour: an
  overflow lands in the next bucket of the same size class, which is often another live block.
- **Heap corruption on Windows is reported as a fail-fast**: exception code
  `STATUS_HEAP_CORRUPTION 0xC0000374`, usually raised by `RtlReportCriticalFailure` ->
  `__fastfail` at the moment the heap manager notices an inconsistency (on free, allocate or a
  background validation). Like glibc, this is the aftermath, not the bug. The tool that finds the
  actual write is **PageHeap/Application Verifier** (module 07): it puts each allocation on its
  own page with an unmapped guard page right after, so the overflowing write faults immediately
  with the exact call stack.

| Concept | glibc ptmalloc | Windows NT heap |
|---------|----------------|-----------------|
| Per-thread fast path | tcache | LFH per-processor affinity, lookaside (older) |
| Header | 8-byte size+flags, plaintext | encoded (XOR key) 8/16-byte header |
| Corruption signal | `abort()` with a message, SIGABRT | fail-fast `0xC0000374` |
| Large blocks | `mmap`, freed to OS on `free` | `VirtualAlloc`, freed to OS on `HeapFree` |
| Return to OS | `malloc_trim`, munmap of large | `HeapFree` decommits, `HeapCompact` |
| "Find the write" tool | ASan, Valgrind | PageHeap / Application Verifier |
| Free-fill pattern | none by default (tcache next/key) | `0xFEEEFEEE` freed, `0xBAADF00D` uninit (debug CRT), `0xABABABAB`/`0xCDCDCDCD` guard/no-mans-land |

### 2.7 Why arena / pool allocators avoid all of this

Shakir's Berkeley-DB-to-YugabyteDB migration engine is exactly the kind of code that benefits
from an **arena** (a.k.a. region or bump) allocator. The idea: for a phase of work with a clear
lifetime (processing one batch, one record, one request), allocate a big block once and hand out
pieces by just bumping a pointer; free nothing individually; at the end of the phase, free (or
reset) the whole block in one call.

```c
typedef struct { char *base, *cur, *end; } Arena;
void  arena_init(Arena *a, size_t n){ a->base=a->cur=malloc(n); a->end=a->base+n; }
void *arena_alloc(Arena *a, size_t n){ n=(n+15)&~15u;              /* align */
                                       if (a->cur+n > a->end) return NULL;   /* or grow */
                                       void *p=a->cur; a->cur+=n; return p; }
void  arena_reset(Arena *a){ a->cur=a->base; }   /* "free" everything, O(1) */
void  arena_free (Arena *a){ free(a->base); }
```

Why it is safer and faster:

- **No per-object free**, so no double free, no use-after-free within the phase (lifetimes are
  tied to the arena, not tracked per object), no free-list metadata to corrupt.
- **No fragmentation** within a phase: allocation is a pointer bump.
- **Fast**: no locks, no bin search, cache-friendly sequential layout.
- Trade-offs: you cannot free individual objects early (so a long phase holding many dead
  objects wastes memory), and a buffer overflow still corrupts the *next object in the arena*
  (it just will not corrupt allocator metadata or crash `free`). Pair it with guard regions or
  ASan during testing.

This is the standard pattern in databases (PostgreSQL's `MemoryContext` is a hierarchical arena;
you write PL/pgSQL on top of it), compilers, and request-scoped server code. When an interviewer
asks "how would you make a hot allocation path safe and fast", arenas are the senior answer.

## 3. Code

`code/m03_chunk_inspect.c` reads the chunk header (the 8 bytes before the returned pointer) for
several allocation sizes and shows the free-chunk contents after `free`. It deliberately reads
outside the allocation to show you the metadata, so **do not run it under ASan** (ASan would
correctly flag the header read). Key output to understand:

```
malloc(24)  a  size_field=0x21   chunk=32   PREV_INUSE
malloc(24)  b  size_field=0x21   chunk=32   PREV_INUSE
malloc(25)  c  size_field=0x31   chunk=48   PREV_INUSE
malloc(100) d  size_field=0x71   chunk=112  PREV_INUSE
malloc(1MiB) e size_field=...    IS_MMAPPED
b - a = 32 bytes                       # consecutive same-size chunks are one chunk apart
a[0..7] after free:  <safe-linked ptr> # tcache next pointer overwrote your first 8 bytes
a[8..15] after free: <tcache key>
malloc(24) again -> same address as b  # tcache is LIFO
```

Build and run:

```bash
gcc -std=c11 -Wall -Wextra -g -O0 -o build/m03_chunk_inspect m03_chunk_inspect.c
./build/m03_chunk_inspect
```

To watch bins change live in gdb, use the `pwndbg` or `gef` extension (`heap`, `bins`, `vis`
commands) if you can install one; plain gdb works too:

```
(gdb) p main_arena                 # with libc debug symbols installed (libc6-dbg)
(gdb) p main_arena.top
(gdb) x/4xg <chunk address>
```

The heap-overflow program `code/m04_heap_overflow.cpp` (module 04) shows the corruption side:
an off-by-one write that trips `free(): invalid next size` or is caught cleanly by ASan.

## 4. Under the hood (senior-level)

**Why `free(): invalid next size` points backward in time.** When you `free(p)`, glibc reads
the size field of `p` to find the next chunk at `p + size`, then reads *that* chunk's size to
decide whether to coalesce. If someone overflowed `p` earlier, they wrote past `p`'s user data
into the next chunk's header, so the next size is garbage and the check fails. The write happened
whenever the overflow ran; the detection happens now, at `free`. Between the two, the program ran
normally. This temporal gap is the entire reason heap bugs are hard and why you need a tool that
traps the write itself.

**tcache poisoning and the security angle.** Because the tcache `next` pointer lives in freed
user data, an attacker with a use-after-free or overflow can overwrite it to make `malloc` return
an arbitrary address ("tcache poisoning"). Safe-linking (the `>> 12` XOR) and the alignment check
added in 2.32 raise the bar. You do not exploit this, but you will read CVEs and hardening notes
that reference it, and it explains why freed-chunk contents look mangled.

**fastbins defer coalescing.** Fastbin chunks keep `PREV_INUSE` set even though they are free, so
neighbours do not coalesce them. This speeds the common case but means a double free into a
fastbin is only caught by a shallow "is the top of the fastbin the same chunk" check (easily
bypassed historically). `malloc_consolidate` later flushes fastbins into the unsorted bin and
does the real coalescing; corruption written into a fastbin chunk often surfaces there, hence
`malloc_consolidate(): ...` messages far from the bug.

**Alignment and `memalign`.** `malloc` guarantees 16-byte alignment on x86-64 (enough for any
scalar and `max_align_t`). `posix_memalign`/`aligned_alloc`/`new` with over-aligned types create
chunks with padding and a special header so `free` can find the real chunk start. Freeing a
pointer that is not a chunk start (e.g. the aligned pointer handed back by a naive manual
alignment) gives `free(): invalid pointer`.

**`malloc_trim` and `MADV_DONTNEED`.** glibc can return free pages in the middle of the heap to
the OS with `madvise(MADV_DONTNEED)` (the pages fault back in zeroed if touched again), not only
trim the top. This is why RSS can drop sharply after `malloc_trim(0)` even when the heap's top
chunk has not moved. The automatic trimming threshold is `M_TRIM_THRESHOLD` (128 KiB of
contiguous free top).

**Environment tunables worth knowing.** `MALLOC_ARENA_MAX`, `MALLOC_TRIM_THRESHOLD_`,
`MALLOC_MMAP_THRESHOLD_`, `MALLOC_TOP_PAD_`, `MALLOC_PERTURB_` (fills alloc/free with a byte to
expose uninitialised-use and UAF bugs cheaply), `MALLOC_CHECK_=3` (enables extra consistency
checks and aborts with a report; a poor-man's debug malloc). `GLIBC_TUNABLES=glibc.malloc.*` is
the modern interface. `MALLOC_PERTURB_=165` in a test environment turns many silent UAF/uninit
bugs into obvious garbage-value failures for almost no cost.

**jemalloc / tcmalloc.** Servers often `LD_PRELOAD` a different allocator for lower fragmentation
and better multithread scaling. They have their own metadata layout, their own corruption
reports (`<jemalloc>: Invalid chunk ...`) and their own profiling (`jeprof`, tcmalloc's pprof).
If Shakir's server links one, the glibc messages above do not apply; know which allocator you
are running (`ldd` the binary, check `LD_PRELOAD`).

## 5. In production

- **"Crash in `free` / `malloc` / `_int_malloc`"** in a core is almost never a bug in glibc. It
  is your code having corrupted a chunk earlier. Read the glibc abort message (it is on stderr
  and often in the log): it names which check failed, which tells you overflow vs double-free vs
  wild pointer. Then reproduce under ASan (module 08) to catch the actual write.
- **Intermittent heap corruption that moves around** is the signature of an overflow whose
  victim depends on allocation order, which depends on timing and input. It will "move" when you
  add logging (you changed the allocation pattern). Do not chase the moving symptom; trap the
  write with ASan/PageHeap.
- **RSS climbs then plateaus, no leak reported**: fragmentation or arena growth, not a leak.
  Check `malloc_stats()` in-use vs system bytes; try `MALLOC_ARENA_MAX=2` and a periodic
  `malloc_trim(0)` on an idle tick. Measure before and after (module 09).
- **Multithreaded server using more memory than expected**: per-thread arenas. Each worker
  thread may hold a 64 MiB arena's worth of working set. Cap arenas or use a thread pool with
  fewer, long-lived threads.
- **Migration engine war story shape**: a bump/arena allocator for per-record scratch turns a
  class of free-ordering bugs into non-bugs and speeds the hot loop. But a single overflow inside
  the arena silently corrupts the next record's fields and shows up as wrong data in Yugabyte,
  not a crash. That is worse for a bank than a crash. Run the migration under ASan in CI and keep
  guard bytes between arena objects in debug builds.
- **Windows: `0xC0000374` in a dump.** Heap corruption fail-fast. `!heap -p -a <address>` and
  `!heap -s` (module 07) give the heap state; to find the writer, re-run under PageHeap
  (`gflags /p /enable yourapp.exe /full`) or Application Verifier, which fault at the overflowing
  write with the allocating and faulting stacks.

## 6. Try it

1. **Easy - chunk sizes.** Run `m03_chunk_inspect`. For `malloc(1)`, `malloc(16)`, `malloc(17)`,
   `malloc(24)`, `malloc(40)`, predict the chunk size and `size` field before running, then
   verify. Done when all five predictions are right and you can state the formula.
2. **Easy - tcache LIFO.** Modify the program to malloc three 24-byte blocks a,b,c, free them in
   order a,b,c, then malloc three more and record the addresses. Done when you can explain the
   order they come back in (c,b,a) from the tcache LIFO rule.
3. **Medium - read the abort message.** Build `m04_heap_overflow` (no ASan), run it with several
   `n` values until it aborts, and capture the exact glibc message. Done when you can say which
   check failed and translate it into "a write of about X bytes past a ~Y-byte block".
4. **Medium - perturb.** Run `m04_uninit_read` (module 04) normally, then with
   `MALLOC_PERTURB_=165 ./build/...`. Done when you can explain why the "flags" value became a
   consistent nonzero pattern and how that would help you spot the bug in testing.
5. **Hard - fragmentation vs leak.** Run `m09_growth_not_leak` and read its `malloc_stats()`
   output. Then run it under Valgrind `--leak-check=full`. Done when you can explain why
   Valgrind reports zero leaks while RSS was high, and what `malloc_trim(0)` changed.
6. **Hard - arena allocator.** Implement the `Arena` above, use it to allocate 100000 small
   records in a loop, and compare the time and peak RSS against the same loop using `malloc`/
   `free` per record. Then deliberately overflow one arena object by one byte and observe that
   `free`/exit does **not** crash (unlike the malloc version) but a neighbouring record's field
   is wrong. Done when you can state the safety trade-off of arenas in one sentence.

## 7. Interview questions

**Q: What is stored in a glibc malloc chunk header and why does it matter for debugging?**
A: Eight bytes of size with three low flag bits (PREV_INUSE, IS_MMAPPED, NON_MAIN_ARENA), and a
`prev_size` field used only when the previous chunk is free. It matters because the header sits
immediately before your data, so a buffer overflow corrupts the next chunk's header, and the
allocator only notices later during `free`/`malloc` - the crash is far from the bug.

**Q: Why does heap corruption usually crash far away from the actual bug?**
A: The bad write corrupts allocator metadata (a size field or a free-list pointer) but the
program keeps running normally until the allocator next touches that metadata - a later `free`
or `malloc` of a neighbouring chunk - which is where it aborts. The time and code distance
between the write and the detection is why these are hard and why you need a tool that traps the
write itself.

**Q: What is the tcache and how did it change what you see in a freed block?**
A: A per-thread, lock-free cache of up to 7 recently freed chunks per small size class (glibc
2.26+). It links chunks through the first 8 bytes of the freed user data and stores a key in the
next 8 for double-free detection, and since 2.32 the next pointer is "safe-linked" (shifted and
XORed). So a use-after-free that reads a freed small block sees a pointer-looking value or 0, not
the old contents.

**Q: What does `free(): invalid next size` mean?**
A: When freeing a chunk, glibc read the size field of the physically next chunk and found it
insane. That means something overwrote the next chunk's header, i.e. the chunk being freed was
overflowed earlier. It is a heap buffer overflow, detected at free time.

**Q: How is the Windows heap different, and how do you find a heap overflow on each platform?**
A: Windows has multiple heaps, encoded headers, and a Low Fragmentation Heap front-end serving
fixed-size buckets; corruption is reported as fail-fast 0xC0000374. glibc uses plaintext headers,
tcache/bins, and aborts with a text message. To find the actual overflowing write, use ASan or
Valgrind on Linux and PageHeap/Application Verifier on Windows - all of them trap the write at
the moment it happens instead of at the later allocator operation.

**Q: When would you use an arena allocator and what does it not protect against?**
A: For request- or phase-scoped work with a common lifetime - request handlers, per-record
migration scratch, parsers - where you allocate many objects and free them all at once. It
removes per-object free bugs (double free, UAF within the phase) and fragmentation, and is fast
and lock-free. It does not stop a buffer overflow from corrupting the next object in the arena;
it just stops that corruption from crashing the allocator, which can be worse because it turns a
crash into silent data corruption.

**Q: Your multithreaded server uses far more memory than the sum of live objects. No leak is
reported. What is going on?**
A: Most likely per-thread arenas (glibc creates up to 8*cores of them) each holding working set,
plus fragmentation and freed-but-not-returned memory. Confirm with `malloc_stats()` (system
bytes >> in-use bytes) and `/proc/PID/smaps`; mitigate with `MALLOC_ARENA_MAX`, fewer long-lived
threads, periodic `malloc_trim(0)`, or a different allocator (jemalloc/tcmalloc).

## 8. Recap

- malloc hands out chunks; each has an 8-byte size+flags header right before your pointer; data
  and metadata are interleaved.
- `size` low bits: PREV_INUSE, IS_MMAPPED, NON_MAIN_ARENA. Min chunk 32 bytes, sizes multiples
  of 16; `malloc(24)` -> 32-byte chunk.
- Free lists fastest-first: tcache (per-thread, 7/size), fastbins, unsorted, small, large, top.
- Freed small blocks hold a safe-linked tcache next pointer and key in their first 16 bytes -
  recognise it in a UAF.
- glibc abort messages name the symptom (a chunk operation); the bug is an earlier overflow,
  double free or wild pointer. "invalid next size" = overflow past the freed chunk.
- Secondary arenas (64 MiB each, up to 8*cores) explain multithread memory use; big allocations
  use mmap and return to the OS on free.
- Windows: multiple heaps, encoded headers, LFH buckets, fail-fast 0xC0000374; find the write
  with PageHeap/AppVerifier (Linux: ASan/Valgrind).
- Arena allocators remove per-object free bugs and fragmentation and are fast, but an overflow
  inside an arena silently corrupts the next object - test them under ASan.
