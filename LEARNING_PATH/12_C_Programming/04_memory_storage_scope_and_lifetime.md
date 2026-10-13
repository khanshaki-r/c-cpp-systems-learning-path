# 04. Memory: storage duration, scope and lifetime

**Time:** ~3 hours   **Prerequisites:** 01, 03

## 1. In plain words

Every object in C has a **lifetime**: the period during which it exists and its memory is reserved
for it. Separately, every *name* has a **scope**: the region of source code where the name can be
used. The two are easy to confuse, and most memory bugs live in the gap between them.

Analogy: a hotel. The **storage duration** is how the room is booked. An automatic variable gets a
room until the function checks out. A static variable is a permanent resident for the life of the
program. Heap memory from `malloc` is booked until someone explicitly checks out with `free`. A
thread-local variable is one room per guest thread. The **scope** is only whose name is on the door
directory. A dangling pointer is a room key kept after checkout: the key still opens the door, but
the room now belongs to someone else, and what you find (or break) in there is not yours.

C gives you no automatic cleanup (C++ has destructors: 02_Modern_CPP/03_raii_the_core_idea.md). So in C the
question "who frees this, and when?" must be answered by **convention**, written down in comments.
This module covers the rules of each storage duration, the exact contracts of `malloc`, `calloc`,
`realloc` and `free`, how to write ownership down, and the arena allocator: the pattern behind the
"arena allocator with explicit ownership contracts" line on your resume.

## 2. The real thing

### 2.1 The four storage durations

| Duration | How you get it | Starts | Ends | Initial value | Where it lives |
|---|---|---|---|---|---|
| automatic | local variable, parameter, compound literal in a block | entry to its block | exit from its block | **indeterminate** (garbage) unless initialised | stack, or only in registers |
| static | file-scope variable, or `static` local | before `main` | program exit | zero, or the constant initialiser | `.data` (initialised), `.bss` (zero) |
| allocated | `malloc`, `calloc`, `realloc`, `aligned_alloc` | the call | `free` (or `realloc` that moves it) | indeterminate (`calloc`: zero) | heap |
| thread | `_Thread_local` (C11; `thread_local` in C23) | thread start | thread exit | zero, or the constant initialiser | per-thread TLS block |

Two details people miss:

- An automatic object's lifetime ends at the end of its **block**, not its function. A pointer to
  a variable declared inside an `if` or a loop body is dangling once that block ends.
- Static objects are initialised before `main`, with constant expressions only. A `static` local
  is not "initialised on first call" as in C++; it already has its value when the program starts.

### 2.2 Scope, lifetime and linkage are three different things

| Declaration | Scope (where the name works) | Lifetime (when the object exists) | Linkage |
|---|---|---|---|
| `int x;` inside a function | the block | the block's execution | none |
| `static int x;` inside a function | the block | the whole program | none |
| `static int x;` at file scope | rest of the file | the whole program | internal |
| `int x;` at file scope | rest of the file (other files via `extern`) | the whole program | external |
| `malloc(n)` | no name at all | until `free` | n/a |

A `static` local is the case that shows the difference best: its name is visible only inside the
function, but the object outlives every call. That is why a function can safely return a pointer
to a `static` local (with the caveats in section 3.4) but never to an automatic one.

### 2.3 Every meaning of `static` and `extern`

| Where | Keyword | Meaning |
|---|---|---|
| file scope, object or function | `static` | internal linkage: private to this translation unit |
| block scope, object | `static` | static storage duration: one object for the whole program |
| file or block scope | `extern` | declaration only; the definition is elsewhere (external linkage) |
| array parameter | `static` | `void f(int a[static 10])` (C99): caller promises at least 10 elements, and that `a` is not NULL |

The last one is rare but it is a fair interview curiosity, and GCC 13 uses it to check callers:
passing a 4-element array gives `'sum10' reading 40 bytes from a region of size 16
[-Wstringop-overread]`, and passing NULL gives `argument 1 to 'int[static 10]' is null where
non-null expected [-Wnonnull]`.

### 2.4 Stack frames

Each function call pushes a **frame** on the calling thread's stack: the return address, saved
registers, and space for locals that do not fit in registers. Returning pops it, and the next call
reuses the same memory.

```
 higher addresses
 ┌──────────────────────────┐
 │ main's frame             │
 │   int n = 1              │
 │   char out[16]           │
 ├──────────────────────────┤ ◄── frame of format_id_bug() while it runs
 │ return address into main │
 │ saved rbp                │
 │ char buf[16] "ACC-000005"│ ◄── the function returns &buf[0] ...
 ├──────────────────────────┤
 │ (free stack)             │     ... and after the return this region is
 └──────────────────────────┘         reused by the next call (printf, say)
 lower addresses (the stack grows down on x86-64)
```

Stack sizes are small and fixed: 8 MiB for the main thread on Linux (`ulimit -s` prints 8192 on
WSL) and, by default, for threads created by glibc; **1 MiB** by default on Windows, for the main
thread and for `CreateThread`, unless the linker option `/STACK` or the `dwStackSize` argument says
otherwise. A 64 KiB local buffer in a function that is three levels deep in a recursive parser is
how Windows servers die with exception `0xC00000FD` (stack overflow). Frame layout and calling
conventions in detail: 05_Memory_Debugging_RCA/02_stack_frames_and_calling_conventions.md.

### 2.5 The heap API: exact contracts

| Function | Contract |
|---|---|
| `void *malloc(size_t n)` | Returns uninitialised memory aligned for any type (`_Alignof(max_align_t)`, 16 on x86-64 glibc), or NULL. `malloc(0)` returns NULL or a unique pointer (glibc: non-NULL) that you must still `free`. |
| `void *calloc(size_t count, size_t size)` | Like `malloc(count * size)` but **zero-filled** and **checks the multiplication for overflow** (returns NULL). |
| `void *realloc(void *p, size_t n)` | Resizes. May **move** the block: contents up to the smaller size are copied, the old block is freed, and every old pointer into it is now dangling. On failure returns NULL and leaves the old block **untouched and still yours**. `realloc(NULL, n)` is `malloc(n)`. `realloc(p, 0)` is implementation-defined in C17 and undefined in C23: never use it. |
| `void free(void *p)` | `p` must come from this allocator and not be freed yet. `free(NULL)` does nothing. After `free`, the pointer's value is indeterminate: even comparing it is formally undefined. |
| `void *aligned_alloc(size_t align, size_t n)` | C11, for alignments above 16 (cache lines, pages). Not provided by MSVC: use `_aligned_malloc` and `_aligned_free` there (and they must be paired). POSIX alternative: `posix_memalign`. |

### 2.6 The realloc-leak idiom

```c
/* WRONG */
b->data = realloc(b->data, cap);   /* on failure: NULL overwrites the only pointer to the old block */

/* RIGHT */
char *p = realloc(b->data, cap);
if (p == NULL) return -1;          /* b->data is still valid and still owned */
b->data = p;
```

The wrong version leaks the old block and leaves the struct claiming `len` bytes of data at a NULL
pointer, so the *next* access crashes. It only misbehaves when `realloc` fails, which is why it
survives code review and testing and then appears under memory pressure in production.

Also check the size arithmetic **before** calling: `n * size` can wrap (module 02), and doubling a
capacity can wrap. `calloc` checks its own multiplication; `malloc` and `realloc` do not.

### 2.7 Writing ownership down

C has no destructor and no smart pointer, so every pointer in an interface needs a sentence saying
who owns the memory. Use the same small vocabulary everywhere:

| Phrase in the comment | Meaning |
|---|---|
| **Returns an owned pointer; free with `x_free()`** | caller must release it, with exactly that function |
| **Borrows `p`** | callee uses it during the call only; does not keep it, does not free it |
| **Borrowed result, valid until ...** | caller must not free it; say what invalidates it (next call, `x_reset`, `x_free(parent)`) |
| **Takes ownership of `p`** | callee will free it; caller must not touch it afterwards. **Say what happens on failure**: does the callee still free it? |
| **Caller provides `buf`, `bufsz`** | no ownership transfer at all; usually the best API |

Paired functions make the rules mechanical:

- `x_new()` / `x_free()` for objects the library allocates (opaque types, module 06);
- `x_init(&x)` / `x_destroy(&x)` for objects the caller allocates (on the stack, or inside
  another struct) but that own resources inside.

`x_free(NULL)` should do nothing, like `free(NULL)`, so that cleanup paths stay simple. After
freeing a pointer that lives on in a struct, set it to NULL so a second cleanup is harmless.
Module 10 of this course (10_error_handling_and_resource_cleanup.md) builds error handling and
goto-cleanup on top of these rules. How C++
encodes the same contracts in types is in 02_Modern_CPP/05_smart_pointers_and_ownership.md and
09_OOP_Design_Patterns_Architecture/03_ownership_and_object_lifetime_design.md.

### 2.8 Alignment

Every type has an alignment: its address must be a multiple of `_Alignof(T)` (`alignof` with
`<stdalign.h>`). A struct's alignment is the largest alignment of its members. `malloc` returns
memory aligned for `max_align_t`, so it is good for any standard type. When you carve your own
allocations out of a big block (an arena, a pool, a buffer), you must align each one yourself:

```c
/* round x up to a multiple of a, where a is a power of two */
uintptr_t aligned = (x + (a - 1)) & ~(uintptr_t)(a - 1);
```

`_Alignas(64) char buf[256];` over-aligns an object (for example to a cache line, to avoid false
sharing: 04_Concurrency/10_cache_lines_false_sharing_performance.md).

### 2.9 VLAs and `alloca`: avoid

`int vla[n];` (a variable length array, C99) and `alloca(n)` both allocate `n`-dependent space on
the stack. In server code they are a liability:

- There is no failure path. If `n` is too big, the program does not get NULL: it runs off the end
  of the stack and crashes, or worse, jumps over the guard page into other memory.
- If `n` comes from a request (a length field), an attacker chooses your stack usage.
- MSVC never supported VLAs; C11 made them optional; `alloca` is not standard at all.

Use a fixed-size local buffer for the common small case and fall back to `malloc` for the rare
large case. GCC's `-Wvla` flags every VLA (`ISO C90 forbids variable length array 'vla'`).

### 2.10 Returning the address of a local

```c
static char *format_id_bug(int id)
{
    char buf[16];
    snprintf(buf, sizeof buf, "ACC-%06d", id);
    return buf;                 /* buf's lifetime ends here */
}
```

GCC warns even without `-Wall`:

```
m04_returning_buffers.c:60:12: warning: function returns address of local variable [-Wreturn-local-addr]
```

And GCC 13 then compiles the function to **return NULL**, so the bug becomes a clean crash at the
first use. Other compilers return the real stack address, the caller reads the string successfully
(the bytes are still there), and the program "works" until the next function call overwrites that
stack region. Section 3.4 shows the four correct alternatives.

## 3. Code

### 3.1 Storage durations side by side: `code/m04_storage_durations.c`

```c
int g_total_calls;                  /* static storage, file scope, EXTERNAL linkage, starts at 0 */
static int s_module_calls;          /* static storage, file scope, INTERNAL linkage, starts at 0 */
static _Thread_local int t_calls;   /* thread storage: every thread gets its own copy, starts at 0 */

static int next_sequence(void)
{
    static int seq = 100;           /* static storage, BLOCK scope: initialised once, before main,
                                       and keeps its value between calls */
    ++g_total_calls;
    ++s_module_calls;
    return seq++;
}

static int worker(void *arg)
{
    int n = *(const int *)arg;
    for (int i = 0; i < n; ++i)
        ++t_calls;                  /* no lock: no other thread can see this thread's t_calls */
    return t_calls;
}
```

It uses C11 `<threads.h>` (`thrd_create`, `thrd_join`); glibc 2.34 and later need no `-pthread`.

```
static: seq continued 100 -> 101, g_total_calls = 2, s_module_calls = 2
automatic: automatic_demo() returned 6 twice (fresh local each call)
allocated: block freed, and the copy of its address is now dangling
thread: A counted 1000, B counted 250, main's own t_calls is still 0
```

### 3.2 Heap contracts and a growable buffer: `code/m04_malloc_contracts.c`

```c
/* A growable byte buffer.
 * OWNERSHIP: a struct buf owns `data`. Release with buf_free(). Copying the struct by value
 * creates two owners of one block: do not do it. */
struct buf {
    char  *data;
    size_t len;
    size_t cap;
};

/* Ensures cap >= need. On failure returns -1 and leaves *b UNCHANGED and still valid. */
static int buf_reserve(struct buf *b, size_t need)
{
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return -1;          /* doubling would wrap */
        cap *= 2;
    }
    char *p = realloc(b->data, cap);
    if (p == NULL) return -1;                       /* b->data still points to the old block */
    b->data = p;                                    /* the block may have MOVED */
    b->cap = cap;
    return 0;
}

static void buf_free(struct buf *b)
{
    free(b->data);                                  /* free(NULL) is fine: no check needed */
    b->data = NULL;                                 /* a second buf_free is now harmless */
    b->len = b->cap = 0;
}
```

```
calloc(SIZE_MAX / 2, 4) -> NULL (overflow detected)
appended 10000 bytes; capacity is 16384 after 11 reallocations (doubling)
all allocations released
```

`valgrind --leak-check=full -q ./m04_malloc_contracts` prints nothing beyond the program's own
output: no leaks, no invalid accesses.

### 3.3 An arena allocator with an ownership contract: `code/m04_arena_allocator.c`

An **arena** (also called a bump or region allocator) grabs one big block up front and hands out
pieces by moving a pointer forward. Individual pieces are never freed; the whole arena is reset at
once. It fits work that has a natural "batch" or "request" boundary: parse a request, build the
response, reset; or decode a batch of records, write them out, reset.

```c
/* OWNERSHIP CONTRACT (the comment every arena-using module should carry):
 *   1. The arena OWNS one block from malloc. Every pointer handed out by arena_alloc() is
 *      BORROWED from the arena; callers never free() it.
 *   2. A borrowed pointer is valid until the next arena_reset() or arena_destroy() of THAT arena.
 *      Storing it anywhere that outlives the reset is a use-after-free waiting to happen.
 *   3. Allocation failure returns NULL; the arena stays valid.
 *   4. Not thread-safe. One arena per thread, or per request.
 */

/* align must be a power of two. Rounds the current position up to a multiple of align. */
static void *arena_alloc(arena *a, size_t size, size_t align)
{
    assert(align != 0 && (align & (align - 1)) == 0);
    uintptr_t cur = (uintptr_t)(a->base + a->used);
    uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t pad = (size_t)(aligned - cur);
    /* Written as subtractions so that a huge `size` cannot wrap the check. */
    if (pad > a->cap - a->used || size > a->cap - a->used - pad) return NULL;
    a->used += pad + size;
    if (a->used > a->high_water) a->high_water = a->used;
    return (void *)aligned;
}

#define ARENA_NEW(a, T) ((T *)arena_alloc((a), sizeof(T), alignof(T)))

static void arena_reset(arena *a) { a->used = 0; }     /* frees EVERYTHING at once, O(1) */
```

```
1-byte alloc, then customer (size 24, align 8) placed at offset 8; used = 32
customer 42 "R. Kumar" at 725 bp; used = 41
request 1 used 240 bytes
request 2 used 480 bytes
request 3 used 720 bytes
high-water mark: 720 of 4096 bytes
arena destroyed: every borrowed pointer is now invalid
```

The first line shows alignment at work: after a 1-byte allocation, the 8-byte-aligned `customer`
starts at offset 8, not 1, and 7 bytes are padding. The high-water mark tells you how big the arena
really needs to be. Module 12 of this course (12_memory_allocators_arena_pool_and_malloc.md) goes further with arenas, pools and a debugging
allocator.

### 3.4 Four ways to return a string, and the bug: `code/m04_returning_buffers.c`

```c
/* 1. Caller provides the buffer. Best default: no allocation, thread-safe. */
static int format_id(char *out, size_t outsz, int id);

/* 2. Static buffer. NOT thread-safe and overwritten by the next call
 *    (strerror, inet_ntoa, asctime and localtime work this way, hence their _r versions). */
static const char *format_id_static(int id);

/* 3. Heap. OWNERSHIP: the caller owns the result and must free() it. NULL on failure. */
static char *format_id_alloc(int id);

/* 4. Return a struct by value: the array travels inside the struct as a copy. */
static struct id_text format_id_value(int id);
```

```
1. caller buffer : ACC-000042
2. static buffer : ACC-000001 and ACC-000001   <- same text twice
3. heap          : ACC-000007 (caller frees)
4. struct value  : ACC-000009
```

Line 2 is the static-buffer trap in one `printf`: both arguments are the same pointer, so both
print whichever call ran last. In a multithreaded server the same thing happens between threads,
silently.

The file also contains the bug, compiled only with `-DSHOW_BUG` (deliberate undefined behaviour;
build without `-Werror` because GCC warns):

```bash
gcc -std=c17 -Wall -Wextra -g -DSHOW_BUG m04_returning_buffers.c -o m04_bug && ./m04_bug
```

```
m04_returning_buffers.c: In function 'format_id_bug':
m04_returning_buffers.c:60:12: warning: function returns address of local variable [-Wreturn-local-addr]
   60 |     return buf;
      |            ^~~
1. caller buffer : ACC-000042
2. static buffer : ACC-000001 and ACC-000001   <- same text twice
3. heap          : ACC-000007 (caller frees)
4. struct value  : ACC-000009
BUG: format_id_bug returned (nil)
```

## 4. Under the hood (senior-level)

### Where each duration physically lives

- **Static**: in `.data` or `.bss`, addressed relative to the instruction pointer:
  `movl s_calls(%rip), %eax`. The address is fixed once the program is loaded.
- **Automatic**: at an offset from `rsp` (or `rbp`), or only in a register. An automatic variable
  whose address is never taken may never touch memory at all, which is why gdb says "optimized
  out" in Release builds.
- **Allocated**: in heap memory that glibc's `malloc` obtains from the kernel with `brk` (small
  blocks, the main arena) or `mmap` (large blocks, by default 128 KiB and above, adjusted
  dynamically). Each block has a 16-byte header just before the pointer you receive, which records
  its size: that is how `free` knows how much to release, and why `free` of a pointer that did not
  come from `malloc` aborts with `free(): invalid pointer`. Details:
  05_Memory_Debugging_RCA/03_the_heap_malloc_internals.md.
- **Thread**: in a per-thread block that the `fs` segment register points at on x86-64 Linux (`gs`
  and the TEB on Windows x64). GCC 13 `-O2` for `++t_calls` in an executable:

```asm
bump_tls:
	movl	%fs:t_calls@tpoff, %eax     # fixed offset from this thread's TLS base
	addl	$1, %eax
	movl	%eax, %fs:t_calls@tpoff
	ret
```

  In a shared library (`-fPIC`) the same access becomes a call to `__tls_get_addr`, because the
  library's TLS block offset is not known until load time. Thread-local variables in hot paths of a
  `.so` can therefore cost a function call per access.

### Why use-after-return often "works"

Returning from a function only moves `rsp`; nothing erases the old frame. The dangling pointer
still points at the old bytes, which stay intact until the next call reuses that region. So the
bug shows up as "the string is correct in the log line but corrupted in the next one", depending on
which function ran in between. AddressSanitizer catches it with
`ASAN_OPTIONS=detect_stack_use_after_return=1`.

### What `realloc` really does

glibc first tries to grow the block in place (if the next chunk is free or the block is at the top
of the heap). Otherwise it allocates a new block, copies, and frees the old one. For blocks that
were `mmap`ed, it uses `mremap`, which moves the pages without copying their contents. Doubling the
capacity makes the total copying cost O(n) over n appends (amortised O(1) per append): 10,000
appended bytes needed only 11 reallocations in section 3.2.

### Why `free` does not shrink the process

Freed blocks go back to glibc's free lists, not to the kernel, unless they were `mmap`ed or sit at
the top of the heap. So a server's RSS (resident memory) stays high after a burst even with no
leak. Telling a leak from growth and fragmentation is the subject of
05_Memory_Debugging_RCA/09_leaks_and_memory_growth.md. Arenas sidestep fragmentation entirely:
one block in, one block out.

## 5. In production

- **Cross-module free on Windows.** Each DLL linked with a different C runtime, or statically with
  `/MT`, has its **own heap**. Memory allocated by `malloc` in one DLL and freed by `free` in
  another goes to the wrong heap: heap corruption, reported later and elsewhere (often in
  `ntdll!RtlpFreeHeap`). The rule: the module that allocates also frees. Export `x_free()` next to
  every function that returns owned memory.
- **Stack overflow in worker threads.** Windows threads get 1 MiB by default. A large local buffer
  (say `char row[65536]`) in a function called from a recursive parser or a deep call chain blows
  it under one particular input: exception `0xC00000FD`. On Linux it is SIGSEGV on the guard page
  below the stack. Move big buffers to the heap or to per-thread storage allocated once.
- **Static buffers in a multithreaded server.** `strtok`, `strerror`, `inet_ntoa`, `asctime`,
  `localtime` and any function of yours with a `static char buf[]` are data races when two worker
  threads call them. Use `strtok_r`/`strtok_s`, `strerror_r`/`strerror_s`, `inet_ntop`,
  `localtime_r`/`localtime_s`, or a caller-provided buffer. See
  04_Concurrency/02_data_races_and_why_they_are_ub.md.
- **Ownership ambiguity on error paths.** A function "takes ownership" of a buffer, fails halfway,
  and frees it; the caller, seeing the failure, also frees it: double free. Or neither frees it:
  leak. Every "takes ownership" comment must say what happens on failure, and the tests must cover
  the failure path (fault injection: make the Nth allocation fail).
- **Callback contexts that die first.** A function registers `&local_ctx` as the context of an
  asynchronous callback and returns. The callback fires later and writes into a stack frame that
  now belongs to someone else. Contexts for asynchronous callbacks must be heap-allocated (or
  static) and freed when the callback is unregistered.
- **Arena pointers that escape.** In a batch job using an arena per batch (for example a migration
  engine decoding records), a pointer to a decoded field is stored in a long-lived cache or a
  "last error" global. After `arena_reset` it points into the next batch's data: wrong values, no
  crash. The contract in section 3.3 is what reviewers check: nothing allocated from the arena may
  be stored anywhere that outlives the reset; copy it out instead.

## 6. Try it

1. **(easy)** Annotate every object in `m04_storage_durations.c` with its storage duration, scope
   and linkage. Done when your table has no blank cells and you can say when each object is
   created and destroyed.
2. **(easy)** Build `m04_returning_buffers.c` with `-DSHOW_BUG`, then rewrite `format_id_bug` into
   each of the four correct forms in turn. Done when each version passes the existing asserts and
   you can name one disadvantage of each.
3. **(medium)** Fault injection: write `void *test_realloc(void *p, size_t n)` that fails on the
   Nth call (N from an environment variable) and use it in `buf_reserve`. Show with Valgrind that
   the WRONG idiom leaks and leaves `data == NULL` when the failure hits, and the RIGHT one does not.
   Done when you have both Valgrind reports.
4. **(medium)** Extend the arena with `size_t arena_mark(arena *)` and
   `void arena_release_to(arena *, size_t mark)` (stack-like nested scopes), and an
   `arena_calloc(a, count, size, align)` with an overflow check. Done when a test allocates in a
   nested scope, releases to the mark, and gets the same addresses again.
5. **(medium)** Write the header comment block for a small API: `cust_parse(const char *line)`
   returning a record, `cust_name(const cust *)`, `cust_free(cust *)`, and
   `cust_set_name(cust *, char *name)` which takes ownership of `name`. Done when every pointer
   parameter and return value uses a phrase from the table in section 2.7, including what happens to
   `name` if `cust_set_name` fails.
6. **(hard)** Write a recursive function with a 4 KiB local array and find the recursion depth at
   which it crashes in the main thread (8 MiB stack) and in a thread created with
   `pthread_attr_setstacksize` of 256 KiB. Done when both measured depths match your calculation
   within 10 % and you can explain the difference (frame overhead, guard page).

## 7. Interview questions

**Q: What are the four storage durations in C?**
Automatic (locals: from block entry to block exit, indeterminate initial value), static (globals
and `static` locals: the whole program, zero or constant-initialised before `main`), allocated
(`malloc` to `free`), and thread (`_Thread_local`: one instance per thread, for the thread's
lifetime).

**Q: What is the difference between scope and lifetime?**
Scope is the region of source where a name can be used; lifetime is the period during execution
when the object exists. A `static` local has block scope but program lifetime; a `malloc`ed block
has a lifetime but no name at all. Dangling pointers are pointers used after the lifetime ended,
regardless of scope.

**Q: What happens if `realloc` fails? Show the correct way to call it.**
It returns NULL and leaves the original block allocated and unchanged. Assigning the result
directly to the only pointer (`p = realloc(p, n)`) leaks the block and loses the data. Assign to a
temporary, check it, then replace the pointer; also check the size calculation for overflow first.

**Q: `malloc` versus `calloc`?**
`malloc(n)` returns uninitialised memory. `calloc(count, size)` returns zeroed memory and checks
`count * size` for overflow, returning NULL instead of a short block. For large blocks glibc's
`calloc` can skip the zeroing because fresh pages from `mmap` are already zero.

**Q: Why is returning the address of a local variable wrong, and why does it sometimes appear to work?**
The local's lifetime ends when the function returns, and its stack memory is reused by the next
call. Nothing erases the bytes on return, so an immediate read often sees the old value, and the
corruption appears later. GCC warns and in fact returns NULL; ASan detects it with
`detect_stack_use_after_return`.

**Q: Why avoid VLAs and `alloca` in server code?**
They allocate on a small fixed-size stack with no failure path: a large size crashes instead of
returning NULL, and if the size comes from input, an attacker controls your stack usage. MSVC does
not support VLAs. Use a fixed buffer for the common case and fall back to the heap.

**Q: What is an arena allocator, and what is its ownership contract?**
It allocates one large block and hands out aligned pieces by bumping an offset; nothing is freed
individually and a reset releases everything in O(1). The contract: the arena owns the memory,
callers only borrow pieces, and every piece is invalid after the next reset or destroy, so nothing
allocated from it may be stored beyond that point. It fits per-request or per-batch work and
removes per-object `free` calls, fragmentation and most leak opportunities.

**Q: How do you document ownership in a C API?**
Say for every pointer whether the caller owns the result (and which function frees it), whether
the callee borrows a parameter (and for how long a borrowed result stays valid), or whether the
callee takes ownership (including on failure). Pair `x_new`/`x_free` and `x_init`/`x_destroy`, make
`x_free(NULL)` a no-op, and prefer caller-provided buffers, which need no transfer at all.

## 8. Recap

- Four storage durations: automatic, static, allocated, thread. Know when each starts, ends and
  what its initial value is.
- Scope is about names, lifetime about objects; automatic lifetime ends at the end of the block.
- `static` at file scope = internal linkage; `static` in a block = static storage duration.
- Stacks are small (8 MiB Linux, 1 MiB Windows threads by default); big locals and VLAs crash
  servers.
- `realloc` may move the block and on failure leaves the old one intact: assign to a temporary.
- `calloc` zeroes and checks overflow; `free(NULL)` is a no-op; never `realloc(p, 0)`.
- Write ownership down: owns, borrows (until when), takes ownership (and on failure?).
- Align carved allocations with `(x + a - 1) & ~(a - 1)`.
- Never return the address of a local; use a caller buffer, a static (not thread-safe), the heap,
  or a struct by value.
- An arena owns one block, lends pieces, and invalidates them all at reset; nothing from it may
  outlive the reset.
