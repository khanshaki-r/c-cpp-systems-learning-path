# 03. Pointers from the ground up

**Time:** ~3 hours   **Prerequisites:** 01, 02

## 1. In plain words

Memory is a very long street of numbered houses. Each house holds one byte. The house number is
the **address**. A variable is a group of neighbouring houses with a name: an `int` occupies four
houses, a `double` eight.

A **pointer** is a slip of paper with a house number written on it. The slip itself also lives in
some houses (eight of them on a 64-bit machine). Two operators move you between the slip and the
house:

- `&x` asks "what is the house number of `x`?" and gives you a slip.
- `*p` says "go to the house written on slip `p`" and lets you read or change what lives there.

The pointer's **type** tells C how many houses the thing at that address occupies and how to read
them. That is why `p + 1` on an `int *` moves four houses, not one: it moves to the *next int*.

Everything confusing about pointers comes from a handful of rules: arrays are not pointers but
turn into one at the slightest touch; `sizeof` sees the difference and nothing else does; `const`
can apply to the slip or to the house; a pointer to a pointer is a slip that tells you where
another slip is kept. This module draws each of these as memory, because when you read a crash
dump, memory is all you get.

## 2. The real thing

### 2.1 Objects, addresses and pointer variables

```c
int x = 7;
int *p = &x;
```

On a 64-bit little-endian machine (addresses invented for the picture):

```
 address    bytes                       name   type
 0x1000     07 00 00 00                 x      int        (4 bytes)
 0x1004     .. .. .. ..                 (padding or something else)
 0x1008     00 10 00 00 00 00 00 00     p      int *      (8 bytes, holds 0x1000)

   p ──────────────► x
 [0x1000]           [7]
```

- `p` is an object like any other. It has its own address (`&p` is `0x1008`, of type `int **`).
- `*p` is an **lvalue** (something with a location) referring to `x`. `*p = 8;` changes `x`.
- C has no references. "Pass by reference" in C means passing a pointer by value
  (`set_to_42(&x)` in section 3.1).

Declaration syntax mirrors use: `int *p` reads "`*p` is an `int`". The `*` binds to the name, not
the type, which is why this declares one pointer and one `int`:

```c
int* a, b;     /* a is int *, b is int. Write one declaration per line, star next to the name. */
```

### 2.2 Pointer arithmetic

Adding `n` to a pointer moves it by `n * sizeof(*p)` bytes. Subtracting two pointers into the same
array gives the number of **elements** between them, as a `ptrdiff_t`.

```
int ia[4] = {10, 20, 30, 40};          (int is 4 bytes)

 address   0x2000  0x2004  0x2008  0x200C  0x2010
           [ 10 ]  [ 20 ]  [ 30 ]  [ 40 ]  (one past the end)
             ^       ^                       ^
            ia      ia+1                    ia+4  may be formed and compared, never dereferenced

 &ia[3] - &ia[0] == 3 elements == 12 bytes
```

`a[i]` is **defined** as `*(a + i)`. Addition commutes, so `i[a]` is legal and means the same
thing; you will only ever see it in quiz questions.

The rules that matter in production:

- You may form pointers to every element of an array and to **one past the last element**.
  Computing any other out-of-range pointer is undefined, even if you never dereference it
  (`p - 1` when `p` points at element 0 is already UB).
- Subtracting or comparing (`<`, `>`) pointers is only defined within the same array (or object).
  `==` and `!=` are fine between any two pointers.
- A plain object counts as an array of one element for these rules.

### 2.3 Arrays are not pointers: decay

An array is a block of N elements. Its name is **not** a variable holding an address: there is no
pointer stored anywhere. But in almost every expression, an array is converted ("decays") into a
pointer to its first element. The three exceptions:

1. operand of `sizeof`: `sizeof arr` is the size of the whole array;
2. operand of unary `&`: `&arr` has type "pointer to array of N", same address, different type;
3. a string literal used to initialise a `char` array: `char s[] = "hello";` copies the bytes.

```
char s1[] = "hello";          char *s2 = "hello";   (better: const char *)

 s1: [h][e][l][l][o][\0]       s2: [0x4010] ─────► [h][e][l][l][o][\0]   (read-only page)
     6 bytes, writable             8-byte pointer   string literal, shared, not writable
 sizeof s1 == 6                sizeof s2 == 8
```

Because of decay, a function can never receive an array. These three parameter declarations are
**identical**: all three declare `int *a`.

```c
void f(int a[10]);    void f(int a[]);    void f(int *a);
```

GCC warns if you take `sizeof` of such a parameter:

```
w1.c:2:46: warning: 'sizeof' on array function parameter 'arr' will return size of 'int *' [-Wsizeof-array-argument]
    2 | static size_t f(int arr[10]) { return sizeof arr; }
```

So every function that takes an array must also take its length, or use a sentinel such as the
terminating NUL of a string.

**The extern trap.** One file defines `int table[4] = {1, 2, 3, 4};` and another declares
`extern int *table;`. It links (symbols have no types, module 01). The second file now reads the
first 8 bytes of the array, `01 00 00 00 02 00 00 00`, *as a pointer* (`0x0000000200000001`) and
dereferences it. The declaration must match: `extern int table[];`.

### 2.4 `sizeof` traps

| Expression | Value on x86-64 | Why |
|---|---|---|
| `sizeof arr` where `int arr[10]` | 40 | whole array |
| `sizeof arr` inside `f(int arr[10])` | 8 | it is a pointer |
| `sizeof p` where `char *p = "hello"` | 8 | size of the pointer |
| `sizeof "hello"` | 6 | array of 6 chars, NUL included |
| `strlen("hello")` | 5 | characters before the NUL |
| `sizeof *p` where `struct account *p` | size of the struct | does **not** dereference: `sizeof` does not evaluate its operand |

Two allocation idioms remove a whole class of bugs:

```c
struct account *a = malloc(sizeof *a);          /* not sizeof(struct account): stays right if the type changes */
int *v = malloc(n * sizeof *v);                  /* not sizeof(int *), not sizeof v */
memset(a, 0, sizeof *a);                         /* not sizeof a: that is 8 bytes */
```

`malloc(sizeof p)` instead of `malloc(sizeof *p)` allocates 8 bytes for a struct of 64, and every
write after the eighth byte corrupts the heap. `-Wall` does not catch it.

`#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))` works only on real arrays; on a pointer it
silently returns `8 / sizeof(element)`.

### 2.5 Pointers to pointers

A `T **` points at a `T *`. You need one when a function must **change the caller's pointer**:

```
struct node *head = NULL;          push_front(&head, 7);

 main's frame                      push_front's frame
 head: [NULL] ◄──────────────────  head: [&main.head]       (struct node **)
                                   *head = n;  writes into main's variable
 after:
 head: [0x5000] ───► node@0x5000 { value 7, next NULL }
```

Without the extra level, `push_front(head, 7)` would receive a copy of `NULL`, change the copy,
and leak the node.

The same idea gives the cleanest linked-list removal. Instead of tracking "previous node" and
special-casing the head, walk with a pointer **to the link** that leads to the current node:

```
 head ──► [7| ]──► [4| ]──► [7| ]──► NULL
  ^          ^
  link       link after one step = &first_node->next
 if (*link)->value matches:  *link = (*link)->next  rewires whichever pointer led here
```

`remove_all` in section 3.3 is six lines and has no head special case.

Other common `**` uses: `char **argv` (an array of string pointers), out-parameters that return an
allocated object (`int make_greeting(const char *name, char **out)`), and jagged 2-D arrays.

### 2.6 `const` placement

Read the declaration from the name outwards, right to left:

| Declaration | Read as | Change the pointer? | Change the pointee? |
|---|---|---|---|
| `const int *p` | p is a pointer to const int | yes | no |
| `int const *p` | same as above | yes | no |
| `int *const p` | p is a const pointer to int | no | yes |
| `const int *const p` | const pointer to const int | no | no |

In an API, `const` on the pointee is a promise: `size_t strlen(const char *s)` promises not to
write through `s`. Use it on every parameter you only read. It documents the contract and lets the
compiler reject accidental writes.

Two rules people get wrong:

- `char **` does not convert implicitly to `const char **`. If it did, you could store a pointer
  to a truly const string into the `const char **`, then write through the original `char **`.
  C++ does allow the safe version, `char **` to `const char *const *`, implicitly; C does not, so
  C code needs a cast even there.
- Casting away `const` is legal; **writing** to an object that was defined `const` (or to a string
  literal) is undefined behaviour and usually a crash, because such objects live in read-only pages.

### 2.7 `void *` and `NULL`

`void *` is the generic object pointer. Any object pointer converts to `void *` and back
**implicitly** in C (C++ requires a cast, which is why code bases compiled as C++ cast the result
of `malloc`). What you cannot do with a `void *`:

- dereference it (there is no type to read);
- do arithmetic on it (GCC allows it as an extension and treats it as `char *`; `-Wpedantic`
  flags it). Convert to `unsigned char *` to walk bytes;
- convert a **function** pointer to it, in ISO C. POSIX requires it to work because `dlsym`
  returns `void *`, and on all mainstream platforms it does.

`qsort`, `bsearch`, `memcpy` and every generic container in C are built on `void *` plus an element
size plus a callback; section 3.5 shows the pattern.

`NULL` is a null pointer constant (`0` or `((void *)0)`). A null pointer compares unequal to every
pointer to an object. Dereferencing it is undefined. On Linux and Windows the first pages of the
address space are never mapped, so it faults: SIGSEGV on Linux, access violation `0xC0000005` on
Windows. Note that `p->field` with `p == NULL` faults at address `offsetof(field)`, not 0: a struct
field at offset `0x20` faults at `0x20`. C23 adds `nullptr`.

### 2.8 Two-dimensional arrays

`int grid[3][4]` is an array of 3 elements, each of which is an array of 4 `int`s, stored
**row-major** (one row after another):

```
grid[0][0] grid[0][1] grid[0][2] grid[0][3] grid[1][0] ... grid[2][3]
[   0    ][   1    ][   2    ][   3    ][   10   ] ... [   23   ]
 \_____________ row 0 ______________/ \___ row 1 ...
 grid decays to int (*)[4]: a pointer to a whole row; +1 skips 16 bytes
```

A function that takes it must know the column count: `int sum(const int (*m)[4], int rows)` (or
`int m[][4]`). Three ways to have a 2-D structure at run time:

```
 contiguous (one malloc):   int (*m)[COLS] = malloc(rows * sizeof *m);
   m ──► [r0c0 r0c1 r0c2 r0c3 | r1c0 ... ]          m[r][c]: one load, address = m + r*16 + c*4

 flat (one malloc, manual index):   int *m = malloc(rows * cols * sizeof *m);   m[r * cols + c]

 jagged (array of row pointers):   int **m;
   m ──► [ptr0][ptr1][ptr2]
           │     │     └──► [r2c0 r2c1]
           │     └────────► [r1c0 r1c1 r1c2]
           └──────────────► [r0c0]                  m[r][c]: TWO loads, rows can differ in length
```

The jagged form uses the same `m[r][c]` syntax for completely different memory, and `int **` is
not compatible with `int (*)[4]`. Passing one where the other is expected (with a cast to silence
the compiler) reads row pointers as if they were integers. C99 also allows variably-modified
parameters, `void f(int rows, int cols, int m[rows][cols])`; GCC supports them, MSVC does not
(they are VLA types).

### 2.9 Returning pointers

A function may return a pointer to: a `static` object, heap memory, or memory the caller passed
in. It must never return the address of one of its own local variables, because that storage is
reused by the next function call. Module 04 shows all four options and the bug. Whenever a
function returns a pointer, its comment must say **who owns** the memory and **how long** it
stays valid.

### 2.10 `restrict`

`restrict` (C99) on a pointer parameter is a promise: during this call, the object it points to is
accessed only through this pointer. The compiler may then keep values in registers across writes
through other pointers, and vectorise loops. `memcpy` is declared with `restrict` parameters; that
is the formal reason overlapping copies must use `memmove`. Breaking the promise is undefined
behaviour, and the compiler will not warn you. Section 4 shows the generated code.

## 3. Code

### 3.1 Basics and arithmetic: `code/m03_pointer_basics.c`

```c
static void set_to_42(int *p) { *p = 42; }   /* the only "pass by reference" C has: pass the address */

    printf("int *    p + 1 moves %td bytes\n", (char *)(ia + 1) - (char *)ia);
    printf("double * p + 1 moves %td bytes\n", (char *)(da + 1) - (char *)da);

    for (const int *it = ia; it != ia + 4; ++it)    /* ia + 4 is one past the end */
        sum += *it;
```

The program prints differences between addresses, never raw addresses, because ASLR changes those
on every run:

```
x = 42, *p = 42, p == &x is true
int *    p + 1 moves 4 bytes
double * p + 1 moves 8 bytes
char *   p + 1 moves 1 bytes
&ia[3] - &ia[0] = 3 elements (12 bytes)
sum by pointer walk = 100
nothing is NULL
(uintptr_t)&ia[1] % _Alignof(int) = 0
```

### 3.2 Decay and `sizeof`: `code/m03_arrays_and_decay.c`

```c
static size_t size_seen_by_function(const int *arr)   /* "int arr[10]" would mean the same */
{
    (void)arr;
    return sizeof arr;
}

    printf("a  + 1 moves %td bytes   (a decays to int *)\n", (char *)(a + 1) - (char *)a);
    printf("&a + 1 moves %td bytes  (&a is int (*)[10])\n", (char *)(&a + 1) - (char *)&a);
```

```
sizeof a in main           = 40 (10 ints)
sizeof a inside a function = 8 (just a pointer)
ARRAY_LEN(a)               = 10
a  + 1 moves 4 bytes   (a decays to int *)
&a + 1 moves 40 bytes  (&a is int (*)[10])
sizeof s1 = 6, sizeof s2 = 8, strlen of both = 5
names has 3 elements of 8 bytes; names[2] = "reversal"
```

The file also shows that a `struct` wrapping an array can be assigned and returned by value, while
a bare array cannot.

### 3.3 `const` and pointer-to-pointer: `code/m03_const_and_double_pointers.c`

```c
/* Pointer-to-pointer removal: `link` points at whichever pointer leads to the current node
 * (the head variable, or some node's next field), so the head needs no special case. */
static void remove_all(struct node **link, int value)
{
    while (*link != NULL) {
        if ((*link)->value == value) {
            struct node *dead = *link;
            *link = dead->next;
            free(dead);
        } else {
            link = &(*link)->next;
        }
    }
}

/* Out-parameter allocation. On success returns 0 and *out owns a malloc'd string (caller frees).
 * On failure returns -1 and *out is untouched. */
static int make_greeting(const char *name, char **out);
```

```
list before: 7 4 7 2 7
list after removing every 7: 4 2
Hello, Shakir (1 letters 'a')
```

### 3.4 Two-dimensional arrays: `code/m03_2d_arrays.c`

```c
    int (*row)[COLS] = grid;        /* grid decays to a pointer to its first ROW */

    int rows = 5;
    int (*dyn)[COLS] = malloc((size_t)rows * sizeof *dyn);   /* contiguous: one malloc, one free */

    int **jag = malloc(ROWS * sizeof *jag);                  /* jagged: row pointers */
    for (int r = 0; r < ROWS; ++r)
        jag[r] = malloc((size_t)lens[r] * sizeof **jag);
```

```
sizeof grid = 48, sizeof grid[0] = 16, sizeof grid[0][0] = 4
row + 1 moves 16 bytes (one whole row)
sum_rows = sum_flat = 138
contiguous dynamic 5x4: sizeof *dyn = 16, sum = 20
jagged rows: [0] [1 2 3] [2 3]
```

Calls from `int (*)[4]` to a `const int (*)[4]` parameter need an explicit cast in C17 (C23 fixed
this); the file shows it.

### 3.5 `void *` and `restrict`: `code/m03_void_ptr_and_restrict.c`

```c
/* Linear search over any array: base address, count, element size, and an equality callback.
 * This is the same shape as the standard qsort and bsearch. */
static void *find_any(const void *base, size_t n, size_t size, const void *key,
                      int (*equal)(const void *elem, const void *key))
{
    const unsigned char *p = base;
    for (size_t i = 0; i < n; ++i, p += size)
        if (equal(p, key)) return (void *)p;     /* like bsearch: returns non-const */
    return NULL;
}

static int update_twice(int *a, int *b)                       { *a = 1; *b = 2; return *a; }
static int update_twice_restrict(int *restrict a, int *restrict b) { *a = 1; *b = 2; return *a; }
```

```
after swap_any: x = 2.5, y = 1.5, t1.id = 2, t2.id = 1
find_any id 11 -> amount -250
update_twice(&same, &same) = 2 (aliasing handled)
update_twice_restrict(&a, &b) = 1 (no aliasing, promise kept)
```

## 4. Under the hood (senior-level)

### What pointer operations compile to

A pointer is an address held in a register; `*p` is a load or a store. x86-64 addressing modes
compute `base + index * scale + displacement` in one instruction, with scale 1, 2, 4 or 8, which
is exactly C's pointer arithmetic. GCC 13 `-O2` (CET instructions removed):

```c
int get(const int *a, long i) { return a[i]; }
long long bal(const struct account *p) { return p->balance; }   /* balance at offset 8 */
int get2d(int (*m)[4], long r, long c) { return m[r][c]; }
int getjag(int **m, long r, long c) { return m[r][c]; }
```

```asm
get:
	movl	(%rdi,%rsi,4), %eax      # a + i*4: one load
	ret
bal:
	movq	8(%rdi), %rax            # p + 8: field offset is a displacement
	ret
get2d:
	salq	$4, %rsi                 # r * 16 (one row = 16 bytes)
	addq	%rsi, %rdi
	movl	(%rdi,%rdx,4), %eax      # one load of data
	ret
getjag:
	movq	(%rdi,%rsi,8), %rax      # FIRST load: the row pointer m[r]
	movl	(%rax,%rdx,4), %eax      # SECOND load: the element
	ret
```

The jagged version needs two dependent loads, and each row may be anywhere in the heap, which
costs cache misses. Contiguous storage is the default for performance; jagged is for rows of
different lengths.

### Arrays have no pointer object

For a local `int a[10]`, the compiler knows `a` as "`rsp` + some offset"; for a global, as a
symbol address. There is no memory cell holding "the address of a". That is why `&a` and `a` have
the same value, and why the `extern int *table` mismatch is so destructive: the code generated for
a pointer **loads 8 bytes from the symbol** and uses them as an address, while the code for an
array uses the symbol's address directly.

### Virtual addresses and what a bad pointer does

A pointer holds a **virtual** address. The MMU translates it through page tables on every access
(05_Memory_Debugging_RCA/01_process_memory_layout.md). On x86-64 only 48 bits of address are used
(57 with 5-level paging); the upper bits must be copies of bit 47 ("canonical" addresses). So a bad
pointer fails in one of three ways:

| Pointer value | What happens | Linux report |
|---|---|---|
| unmapped page (NULL, small offsets, freed and unmapped) | page fault, no mapping | SIGSEGV, `si_addr` = the address |
| mapped page you may not write (literal, code) | page fault, protection | SIGSEGV, `si_addr` = the address |
| non-canonical (garbage like `0x4141414141414141`) | general protection fault | SIGSEGV, `si_code` 128 (`SI_KERNEL`), `si_addr` **0** |

Verified on WSL with gdb's `$_siginfo`: dereferencing `0x4141414141414141` gives `si_code = 128`
and `si_addr = 0x0`, while `p->field` with `p == NULL` and `field` at offset `0x20` gives
`si_addr = 0x20`. So a "fault at address 0" can actually be a garbage pointer, not a NULL: look at
the register used by the faulting instruction before concluding.

The most dangerous bad pointer is the one that points into **mapped, writable** memory: a dangling
pointer into the heap or a wild pointer into another object. It does not fault. It corrupts, and
the crash happens later somewhere else (05_Memory_Debugging_RCA/04_the_catalogue_of_memory_bugs.md).

### `restrict` in the generated code

```asm
update_twice:
	movl	$1, (%rdi)
	movl	$2, (%rsi)
	movl	(%rdi), %eax        # must reload *a: b might point to the same int
	ret
update_twice_restrict:
	movl	$1, (%rdi)
	movl	$1, %eax            # returns the constant: the promise says *a was not changed
	movl	$2, (%rsi)
	ret
```

Call the second function with two pointers to the same `int` and it returns 1 while the int holds 2.
That is what "undefined behaviour" looks like in practice: the code does exactly what the promise
allowed, not what you meant.

### Alignment

Every type has an alignment requirement (`_Alignof(int)` is 4, `_Alignof(double)` 8 on x86-64).
Dereferencing a misaligned pointer is undefined. x86 hardware tolerates most misaligned scalar
loads, so the bug hides there, but ARM may fault, and even on x86 the compiler may emit aligned SSE
instructions (`movaps`) that do fault when it assumes alignment. Casting a `char` buffer at an odd
offset to `int *` is therefore a real bug even on Intel. Use `memcpy` (module 06).

## 5. In production

- **Reading pointer values in a crash dump.** The value of the bad pointer is evidence. On Windows
  Debug builds and debug heaps, these patterns are classic:

  | Value | Usual meaning |
  |---|---|
  | `0x00000000` plus a small offset | NULL pointer, faulting on a field at that offset |
  | `0xCCCCCCCC` | uninitialised stack variable (MSVC `/RTC1`) |
  | `0xCDCDCDCD` | uninitialised heap memory (MSVC Debug CRT) |
  | `0xDDDDDDDD` | memory already freed (MSVC Debug CRT) |
  | `0xFDFDFDFD` | guard bytes just outside a Debug CRT block: an overflow by a few bytes |
  | `0xFEEEFEEE` | memory freed by `HeapFree` with heap debugging on |
  | non-canonical, ASCII-looking (`0x4141...`) | a string overwrote a pointer: buffer overflow |

  glibc has no fill by default; `MALLOC_PERTURB_=165` makes it fill allocated and freed memory so
  the same bugs show up on Linux. See 05_Memory_Debugging_RCA/07_windbg_and_windows_crash_dumps.md.
- **`sizeof` of the wrong thing.** `memset(req, 0, sizeof req)` clears 8 bytes of a 400-byte
  request; the rest keeps the previous request's data. This is a classic in server code that
  reuses buffers: one client sees fields of another. Always `sizeof *req`.
- **Array length lost at a function boundary.** A helper written for a fixed array is later called
  with a shorter buffer and uses `ARRAY_LEN` on its parameter (8 / 4 = 2 elements) or a hard-coded
  size. Pass lengths explicitly; never compute them from a parameter.
- **Off-by-one in pointer loops.** `for (p = buf; p <= buf + len; ++p)` touches one element past
  the end. Use `<` with one-past-the-end and say so in a comment.
- **Pointers into a buffer that moves.** A pointer into a growable buffer is invalid after the
  buffer is `realloc`ed. In a server that parses a request in place while still receiving bytes,
  a pointer kept from before the buffer grew points into freed memory. Store offsets, not
  pointers, into growable buffers.
- **`extern` mismatches** between a definition and a hand-written declaration in another file
  (array versus pointer, `int` versus `long`). Every global should be declared in exactly one
  header that the defining file also includes.

## 6. Try it

1. **(easy)** For `int x = 5; int *p = &x; int **pp = &p;` draw the memory with invented
   addresses, then verify in gdb with `p &x`, `p p`, `p &p`, `p *pp`, `p **pp` and `x/4xb &x`.
   Done when your drawing matches gdb exactly (apart from the actual address values).
2. **(easy)** Predict, then check: `sizeof` of `int a[5]`, `&a`, `*&a`, `a + 0`, `"abc"`,
   `(char *)"abc"`, `a[0]`, and of a `struct { char c; double d; }`. Done when all eight
   predictions are right or you can explain each miss.
3. **(medium)** Implement `remove_all` twice: once with a `prev` pointer and an explicit head
   case, once with the pointer-to-pointer technique. Done when both pass the same test (remove
   from head, middle, tail, all nodes, no nodes) and Valgrind reports no leaks.
4. **(medium)** Write `int **matrix_new(int rows, int cols)` that makes a jagged-looking matrix
   with **one** `malloc`: the row-pointer array followed by the data, with row pointers pointing
   into the data part. Done when `m[r][c]` works, a single `free(m)` releases everything, and you
   have checked the alignment of the data part.
5. **(medium)** Reproduce the extern trap: `a.c` defines `int table[4] = {1, 2, 3, 4};`, `b.c`
   declares `extern int *table;` and prints `table[0]`. Done when you have the crash, the faulting
   address from gdb, and can explain why that address is `0x200000001`.
6. **(hard)** Write a generic `void isort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))`
   (insertion sort, same contract as `qsort`) using only `unsigned char *` arithmetic and
   `memcpy` through a temporary buffer. Done when it sorts `int`s, `double`s and a 40-byte struct
   correctly and matches `qsort` on 1000 random arrays.

## 7. Interview questions

**Q: What is the difference between an array and a pointer?**
An array is a block of elements; a pointer is a variable holding an address. In most expressions an
array decays to a pointer to its first element, which is why they look alike. They differ under
`sizeof` (whole array versus pointer size), under `&` (pointer to array versus pointer to pointer),
in assignability, and in how an `extern` declaration is compiled.

**Q: When does an array not decay to a pointer?**
As the operand of `sizeof`, as the operand of unary `&`, and when a string literal initialises a
`char` array. Also, function parameters declared as arrays are adjusted to pointers, so inside the
function `sizeof` gives the pointer size.

**Q: What is the difference between `const int *p` and `int *const p`?**
`const int *p` is a pointer to a const int: you can move `p` but not write `*p`. `int *const p` is a
const pointer to an int: you can write `*p` but not change `p`. Read the declaration right to left
from the name.

**Q: Why would a function take a `T **`?**
To modify the caller's pointer: allocate and return an object through an out-parameter, insert at
the head of a list, or advance a caller's parsing position. It is also the type of an array of
pointers, such as `argv`. The pointer-to-link technique uses it to remove list nodes without a
head special case.

**Q: What can you not do with a `void *`?**
Dereference it or do arithmetic on it in standard C; you convert it to a concrete pointer type
first (`unsigned char *` for bytes). ISO C also does not allow converting function pointers to
`void *`, although POSIX requires it for `dlsym`. In C, `void *` converts implicitly to any object
pointer, unlike C++.

**Q: What does `restrict` do?**
It promises the compiler that, for the duration of the call, the pointed-to object is accessed only
through that pointer. The compiler can then avoid reloading values after writes through other
pointers and can vectorise. Violating the promise is undefined behaviour; `memcpy` has `restrict`
parameters, which is why overlapping copies need `memmove`.

**Q: What does `p + 1` do for `int *p` versus `char *p`?**
It moves by `sizeof(*p)` bytes: 4 for an `int *` on common platforms, 1 for a `char *`. Pointer
subtraction similarly returns a count of elements, as `ptrdiff_t`.

**Q: A Windows crash dump shows the faulting pointer is `0xDDDDDDDD`. What does that tell you?**
In a Debug build, the Debug CRT fills freed heap blocks with `0xDD`, so the code read a pointer out
of an object that was already freed: a use-after-free. `0xCDCDCDCD` would point to uninitialised
heap memory and `0xCCCCCCCC` to an uninitialised stack variable.

## 8. Recap

- A pointer is an object holding an address; `&` takes an address, `*` follows one.
- `p + n` moves `n * sizeof *p` bytes; pointer subtraction counts elements (`ptrdiff_t`).
- Valid pointers: into an object or one past its end; anything else is UB even without
  dereferencing.
- Arrays decay to pointers except under `sizeof`, `&` and string-literal initialisation;
  array parameters are pointers.
- `malloc(n * sizeof *p)`, `memset(p, 0, sizeof *p)`: size the pointee, never the pointer.
- Use `T **` to change the caller's pointer; the pointer-to-link trick simplifies list removal.
- `const` left of `*` protects the pointee; right of `*` protects the pointer.
- `void *` is generic, converts implicitly in C, has no arithmetic and no dereference.
- Contiguous 2-D arrays need one load per access; jagged `int **` needs two.
- `restrict` is a promise the compiler will exploit; break it and you get wrong results silently.
- In a dump, the pointer value itself (`0x0+offset`, `0xCD...`, `0xDD...`, non-canonical) tells you
  the bug class.
