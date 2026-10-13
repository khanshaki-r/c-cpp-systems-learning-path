# 06. Structs, unions, enums and memory layout

**Time:** ~3 hours   **Prerequisites:** 02, 03, 04

## 1. In plain words

A `struct` is a paper form with labelled boxes printed in a fixed order. The compiler decides where
on the sheet each box goes, and it is allowed to leave **blank gaps** between boxes so that each box
starts at a position its type likes (an 8-byte number likes to start at a multiple of 8). Those gaps
are **padding**. You do not see them in the source, but they are in memory, in `sizeof`, and in every
byte you write to a file or a socket.

A `union` is one box that can hold one of several kinds of things, all starting at the same place:
writing one kind overwrites the others. An `enum` is a set of named integer constants.

Why spend three hours on this? Because the layout is what you actually see in a crash dump (raw
bytes, not field names); because decoding records written by another program (the Berkeley DB
records in your migration work) is entirely about layout; and because "write the struct to disk with
`fwrite`" is one of the most common and most damaging shortcuts in old C code. By the end you will
be able to predict every offset, know when packing is acceptable, use unions without undefined
behaviour, hide a struct's layout behind an opaque pointer, and design a byte format that survives
compilers, platforms and versions.

## 2. The real thing

### 2.1 The layout rules

For an ordinary struct, every mainstream C compiler follows the same rules:

1. Members are placed **in declaration order**. The compiler never reorders them.
2. The first member is at offset 0, so a pointer to a struct, converted, points to its first member.
3. Each member is placed at the next offset that is a multiple of **its alignment**; any gap before
   it is padding.
4. The struct's alignment is the **largest alignment** of its members.
5. The struct's size is rounded up to a multiple of its alignment (**trailing padding**), so that
   in an array every element is correctly aligned.

From `code/m06_layout_and_padding.c` (x86-64):

```c
struct txn_bad {           /* declared in "logical" order */
    char    type;          /* 'D' or 'W' */
    int64_t amount_paise;
    char    channel;       /* 'B'ranch, 'A'TM, 'N'et */
    int32_t branch;
};
```

```
offset: 0    1 ... 7    8 ............ 15   16   17 18 19   20 ...... 23
        [type][ pad 7 ][  amount_paise   ][chan][ pad 3 ][   branch    ]     sizeof = 24
```

The same fields, largest first:

```
offset: 0 ............ 7    8 ...... 11   12     13     14 15
        [  amount_paise   ][   branch    ][type ][chan ][pad 2]               sizeof = 16
```

```
struct txn_bad:  sizeof 24, alignof 8
  type           offset  0 size 1
  amount_paise   offset  8 size 8
  channel        offset 16 size 1
  branch         offset 20 size 4
struct txn_good: sizeof 16, alignof 8
  amount_paise   offset  0 size 8
  branch         offset  8 size 4
  type           offset 12 size 1
  channel        offset 13 size 1
array stride = 16 bytes
```

A third smaller for free. For a struct held a million times in memory (a cache of accounts), or
sent a million times a day, that matters. The tool `pahole` (from the `dwarves` package) prints
every hole in every struct of a binary built with `-g`.

### 2.2 `offsetof` and `container_of`

`offsetof(type, member)` from `<stddef.h>` gives a member's byte offset as a `size_t`. Its most
important use is the reverse direction: given a pointer to a member, get back the enclosing struct.
The Linux kernel calls this `container_of`:

```c
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

struct link { struct link *next; };
struct item { int id; struct link node; };   /* the link lives INSIDE the item */

struct link *l = ...;                        /* found by walking a generic list */
struct item *it = container_of(l, struct item, node);
```

This is how **intrusive** data structures work: the list or tree code knows only `struct link`, the
node memory is part of your object (no separate allocation per node), and one object can sit in
several lists through several `link` members. Module 11 of this course (11_data_structures_in_c.md) builds containers in C.

### 2.3 Padding bytes have unspecified values

The standard says padding bytes take **unspecified values** whenever a member is stored. In practice
they hold whatever was in that memory before. Consequences:

- `memcmp(&a, &b, sizeof a)` can report two structs with equal fields as different. The program
  pre-fills `x` with `0xAA` and `y` with `0x00`, then assigns the same field values to both:

```
fields equal: yes, memcmp equal: no
x bytes:  44 AA AA AA AA AA AA AA  F0 49 02 00 00 00 00 00  41 AA AA AA 07 00 00 00
y bytes:  44 00 00 00 00 00 00 00  F0 49 02 00 00 00 00 00  41 00 00 00 07 00 00 00
```

- Hashing a struct's bytes, or using them as a cache key, has the same problem.
- Writing a struct's bytes to a socket or a file **leaks** whatever was in the padding: fragments of
  previous requests, stack contents, other customers' data. Operating system kernels have shipped
  exactly this information-leak bug many times.

If the raw bytes of a struct ever matter, `memset` it to zero before filling it, and compare field
by field. Better still, never use raw struct bytes outside the process (section 2.9).

### 2.4 Packing

Packing tells the compiler to drop padding, so members sit at unaligned offsets:

```c
#pragma pack(push, 1)        /* MSVC, GCC and Clang all accept this form */
struct rec_pragma { uint8_t version; uint32_t customer_id; int64_t amount_paise; uint16_t branch; };
#pragma pack(pop)            /* restore: without it the packing leaks into every later struct */

struct rec_attr { uint8_t version; uint32_t customer_id; int64_t amount_paise; uint16_t branch; }
    __attribute__((packed)); /* GCC and Clang only */
```

```
natural: sizeof 24, amount_paise at offset 8
pragma : sizeof 15, amount_paise at offset 5
attr   : sizeof 15, amount_paise at offset 5
```

What packing costs:

- **Unaligned access.** Accessing a packed member *by name* is safe: the compiler knows it may be
  misaligned. On x86-64 an unaligned load inside one cache line costs about the same as an aligned
  one, but one that straddles two 64-byte cache lines or two pages is slower. On strict-alignment
  CPUs (some ARM, SPARC, older MIPS) the compiler must emit byte loads and shifts.
- **Pointers to packed members are wrong.** `int64_t *p = &r.amount_paise;` creates an `int64_t *`
  that is not 8-byte aligned, which is undefined behaviour; code that receives it assumes alignment.
  GCC 13 warns for `__attribute__((packed))` structs (`taking address of packed member of 'struct
  rec_attr' may result in an unaligned pointer value [-Waddress-of-packed-member]`) but, verified on
  this toolchain, says **nothing** for the `#pragma pack` version: the Windows-style code is the
  silent one.
- **Atomic operations** on misaligned fields are not atomic, or on x86 become "split locks" that
  stall the whole memory bus; recent Linux kernels can detect and throttle processes that do it.

Packing is acceptable for describing an **external** byte format that you cannot change. Even then
the robust way to *read* such data is not to overlay a packed struct on the buffer, but to decode
each field from its offset (section 2.9 and 07_Databases_Internals/12_migrating_a_kv_store_to_relational.md).

### 2.5 Bit-fields

```c
struct acct_flags {
    unsigned int active    : 1;
    unsigned int frozen    : 1;
    unsigned int kyc_level : 3;   /* 0..7 */
    unsigned int reserved  : 27;
};
```

Compact and readable, and almost everything about their layout is **implementation-defined**:

- the order of fields within the storage unit (GCC and MSVC on x86 fill from the least significant
  bit; big-endian ABIs typically fill from the most significant);
- whether a field may straddle two units;
- whether a plain `int` bit-field is signed (GCC: yes, so `int x : 3` holding 7 reads back as -1);
- which types are allowed besides `int`, `unsigned int` and `_Bool`, and how mixing them packs.
  `struct { unsigned char a : 4; unsigned short b : 4; }` is 2 bytes with GCC; MSVC starts a new
  storage unit whenever the declared type's size changes, giving 4.

And one rule that is not about layout: a bit-field is not a separately addressable object. Writing
one field is a read-modify-write of the whole unit, and the C11 memory model treats adjacent
bit-fields as **one memory location**. Two threads updating `active` and `frozen` of the same
struct, each under its own lock (or none), race and lose updates.

So: bit-fields are fine for in-memory flags owned by one thread. For anything stored, sent, or
shared between threads, use a fixed-width integer with explicit masks:

```c
#define ACCT_ACTIVE      (UINT32_C(1) << 0)
#define ACCT_FROZEN      (UINT32_C(1) << 1)
#define ACCT_KYC_SHIFT   2
#define ACCT_KYC_MASK    (UINT32_C(0x7) << ACCT_KYC_SHIFT)
```

```
bitfield struct: sizeof 4, active=1 kyc=5 stored as 0x00000015 (GCC on x86: first field in the lowest bit)
mask version   : 0x00000015, active=1 frozen=0 kyc=5
```

### 2.6 Unions, type punning and the strict aliasing rule

A union's members all start at offset 0; its size is the largest member's size, rounded up to the
union's alignment. The safe everyday use is the **tagged union** (a variant): a struct with an enum
tag saying which member is currently valid, and a `switch` on the tag everywhere it is read.

```c
struct value {
    enum value_kind kind;
    union {
        int64_t i;
        char    text[16];
        int64_t paise;
    } as;
};
```

**Type punning** means reading the bytes of an object as a different type, for example the bits of a
`float` as a `uint32_t`. There are three ways to write it:

| Code | C | C++ |
|---|---|---|
| `memcpy(&u, &f, sizeof u);` | correct | correct |
| `union { float f; uint32_t u; } x = {.f = f}; return x.u;` | correct (C11 6.5.2.3, footnote 95) | undefined |
| `return *(uint32_t *)&f;` | **undefined** (strict aliasing) | undefined |

The **strict aliasing rule** (C17 6.5p7): an object may be accessed only through an lvalue of its own
type (or a compatible type, a signed/unsigned variant, a qualified version, an aggregate or union
containing it) or through a **character type**. Because of it, the compiler may assume that an
`int *` and a `float *` never point to the same memory, and optimise accordingly (section 4). Reading
any object's bytes through `unsigned char *` is always allowed, which is why every hex dump in this
course uses it.

The `memcpy` form is not slow: GCC compiles it to one register move (section 4). Prefer it in code
that may one day be compiled as C++.

### 2.7 Enums

- Enumeration constants have type `int` in C17. The enum *type* is compatible with some integer
  type the compiler picks (usually `int` or `unsigned int`; `-fshort-enums` makes GCC pick the
  smallest). So `sizeof(enum x)` is not portable; never put an enum in a byte format.
- An enum variable can hold any value of its underlying type, not just the listed ones. Validate
  enums read from outside.
- `switch` on an enum without a `default` gets a `-Wswitch` warning (in `-Wall`) when an enumerator
  is not handled. That is a feature: leave `default` out where you want the compiler to tell you
  about new enum values, and handle "unknown" after the `switch`.
- Use a `..._COUNT` last enumerator to size tables, and designated initializers to index them
  (section 2.9).
- C23 adds a fixed underlying type: `enum opcode : uint8_t { ... };`.

### 2.8 Flexible array members

A struct whose last member is an array of unspecified size (C99):

```c
struct message {
    uint32_t      type;
    uint32_t      len;
    unsigned char payload[];      /* must be last; contributes nothing to sizeof */
};

struct message *m = malloc(sizeof *m + len);     /* header and payload in ONE allocation */
```

One `malloc`, one `free`, header and data adjacent in memory (good for the cache and for sending).
Rules: it must be the last member; the struct cannot be an array element or a member of another
struct (except as the last member, as a GCC extension); and struct assignment copies only the
header.

Older code uses the "struct hack", `unsigned char payload[1];` (or GNU `payload[0]`), and indexes
past the end. Formally that is undefined, and it now bites: GCC 13's `-fstrict-flex-arrays` and
`_FORTIFY_SOURCE=3` can treat `[1]` as a real one-element array and abort on access to element 1.
Convert struct hacks to `[]` when you touch them.

### 2.9 Designated initializers and compound literals

```c
static const struct server_config k_defaults = {
    .bind_addr = "0.0.0.0",
    .port = 7000,
    .max_connections = 256,
    .timeout_ms = 5000,           /* .tls is not named: it is zero, i.e. false */
};

static const char *const k_level_names[LOG_LEVEL_COUNT] = {
    [LOG_ERROR] = "ERROR",        /* index by enum: reordering the enum cannot misalign names */
    [LOG_WARN] = "WARN",
    [LOG_INFO] = "INFO",
    [LOG_DEBUG] = "DEBUG",
};

print_config(&(struct server_config){.bind_addr = "127.0.0.1", .port = 7001});   /* compound literal */
```

Members you do not name are zero-initialised. Adding a field to the struct never shifts the meaning
of an initializer, which positional initializers (`{"0.0.0.0", 7000, 256, 5000}`) cannot promise. A
compound literal is an unnamed object whose lifetime is the enclosing block (static if at file
scope). MSVC has supported both for C since Visual Studio 2013.

### 2.10 Opaque types: encapsulation in C

Put only an **incomplete** declaration in the header:

```c
/* account.h */
typedef struct account account;          /* fields unknown to callers */
account *account_open(const char *id, int64_t opening_paise);
int      account_deposit(account *a, int64_t paise);
int64_t  account_balance(const account *a);
void     account_close(account *a);
```

and the full `struct account { ... };` only in `account.c`. Callers can hold and pass `account *`
but cannot read fields, allocate one, or take `sizeof`. Benefits: invariants are enforced in one
file; you can add, remove or reorder fields **without recompiling callers** (a stable ABI for a
shared library); callers cannot depend on internals. Costs: every object is heap-allocated by the
library, and every field access is a function call (no inlining across the boundary unless you use
LTO). `FILE *` and Win32 `HANDLE` are opaque types you use daily. This is C's version of a class
with private members (09_OOP_Design_Patterns_Architecture/01_oop_for_a_c_programmer.md).

### 2.11 Why you never `fwrite` a raw struct

`fwrite(&rec, sizeof rec, 1, f)` (or `send(sock, &rec, sizeof rec, 0)`) ties the file format to:

1. **Padding**: the size and offsets depend on the compiler, the flags and `#pragma pack` state;
   the padding bytes themselves are garbage, or leaked data.
2. **Type sizes**: `long` is 4 bytes on Windows and 8 on Linux; enums vary; `bool` varies in old code.
3. **Endianness**: the bytes are in host order; a big-endian reader gets nonsense.
4. **No version**: the day you add a field, every old file becomes unreadable, and nothing in the
   file tells the reader which layout it has.
5. **Pointers**: a pointer member is a meaningless number in another process.

The fix is an explicit **wire format**: a version byte first, fixed-width fields at documented
offsets, a defined byte order, a length for variable parts, and an encoder and decoder that move one
field at a time with shifts (module 02). The decoder validates everything and rejects unknown
versions. This is exactly what decoding the packed Berkeley DB records requires
(07_Databases_Internals/12_migrating_a_kv_store_to_relational.md, section 2.2: never cast the buffer
to the struct, decode per field, per-field byte order, versioned records) and what every network
protocol needs (08_Networking_Distributed/04_framing_and_protocol_design.md).

## 3. Code

### 3.1 Layout and padding: `code/m06_layout_and_padding.c`

The full output is in sections 2.1 and 2.3. The macro that prints each field uses `offsetof` and a
`sizeof` on a member of a null pointer, which is fine because `sizeof` never evaluates its operand:

```c
#define SHOW_FIELD(T, f) \
    printf("  %-14s offset %2zu size %zu\n", #f, offsetof(T, f), sizeof(((T *)0)->f))
```

### 3.2 Packing and bit-fields: `code/m06_packed_and_bitfields.c`

```c
    struct rec_pragma r = {2, 1001, -250000, 7};
    int64_t amount;
    memcpy(&amount, (const unsigned char *)&r + offsetof(struct rec_pragma, amount_paise), sizeof amount);
```

```
natural: sizeof 24, amount_paise at offset 8
pragma : sizeof 15, amount_paise at offset 5
attr   : sizeof 15, amount_paise at offset 5
packed amount read via member = -250000, via memcpy = -250000
bitfield struct: sizeof 4, active=1 kyc=5 stored as 0x00000015 (GCC on x86: first field in the lowest bit)
mask version   : 0x00000015, active=1 frozen=0 kyc=5
```

### 3.3 Unions and punning: `code/m06_unions_and_punning.c`

```c
static uint32_t bits_memcpy(float f)        /* always correct, in C and C++; compiles to one mov */
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static uint32_t bits_union(float f)         /* defined in C (C11 6.5.2.3 footnote 95), UB in C++ */
{
    union { float f; uint32_t u; } x = {.f = f};
    return x.u;
}
```

```
sizeof(struct value) = 24 (tag + padding + largest member)
int 42
text "NEFT"
amount 12345.05
bits of 1     = 0x3F800000 (memcpy) 0x3F800000 (union)
bits of -0    = 0x80000000 (memcpy) 0x80000000 (union)
bits of 0.1   = 0x3DCCCCCD (memcpy) 0x3DCCCCCD (union)
first byte of 0xA1B2C3D4 in memory: 0xD4
```

The file also contains a strict-aliasing violation, compiled only with `-DSHOW_ALIASING_UB`
(**deliberate undefined behaviour**, documented in the file header):

```c
static int alias_demo(int *i, float *f)
{
    *i = 1;
    *f = 2.0f;           /* GCC -O2 assumes this cannot change *i (different types) */
    return *i;           /* ...so it returns the constant 1 without reloading */
}
    int target = 0;
    int r = alias_demo(&target, (float *)&target);
```

```bash
gcc -std=c17 -O2 -Wall -DSHOW_ALIASING_UB m06_unions_and_punning.c -o m06_alias && ./m06_alias
```

```
UB: alias_demo returned 1, but target now holds 0x40000000 (the float 2.0f)
```

At `-O0` the same program returns `1073741824` (the reloaded bits of 2.0f): the result depends on the
optimisation level, the signature of undefined behaviour.

### 3.4 Flexible arrays, designated initializers, opaque types: `code/m06_flexible_array_and_opaque.c`

```
sizeof(struct message) = 8, offsetof payload = 8, this message uses 22 bytes
payload: BAL?ACC-000042
config 0.0.0.0:7000 max=256 timeout=5000ms tls=off
config 0.0.0.0:9000 max=256 timeout=5000ms tls=on
config 127.0.0.1:7001 max=0 timeout=0ms tls=off
k_level_names[LOG_WARN] = WARN
opaque account balance: 125050 paise
```

The third config line is a reminder of how designated initializers work: unnamed members of the
compound literal are zero, not copied from `k_defaults`. To override a few fields of the defaults,
copy the defaults and assign (the second line).

### 3.5 An explicit, versioned record format: `code/m06_serialize_record.c`

```c
/* On the wire / on disk, version 1. Little-endian, no padding, every offset written down:
 *   off  0  u8   version (= 1)
 *   off  1  u8   type
 *   off  2  u16  branch
 *   off  4  u32  account_id
 *   off  8  i64  amount_paise (two's complement)
 *   total 16 bytes                                                                         */

static int txn_decode(const unsigned char *in, size_t insz, struct txn *t)
{
    if (insz < 1) return -1;
    if (in[0] != TXN_V1) return -1;                 /* unknown version: quarantine, do not guess */
    if (insz < TXN_V1_SIZE) return -1;
    if (in[1] != 'D' && in[1] != 'C') return -1;
    t->type = (char)in[1];
    t->branch = get_u16(in + 2);
    t->account_id = get_u32(in + 4);
    t->amount_paise = u64_to_i64(get_u64(in + 8));
    return 0;
}
```

```
sizeof(struct txn) = 24, wire size = 16
raw struct bytes:      44 AA AA AA AA AA AA AA  25 2F FC FF FF FF FF FF  01 28 6B EE 01 02 AA AA
encoded v1:            01 44 01 02 01 28 6B EE  25 2F FC FF FF FF FF FF
decoded: type=D amount=-250075 account=4000000001 branch=513
decode of version 9 -> rejected
```

The raw struct line is what `fwrite(&t, sizeof t, 1, f)` would have written: 24 bytes, 9 of them
padding (`AA`, which in a real program would be leftover memory). The encoded record is 16 bytes, all
meaningful, in a documented order. The encoder writes at offset 1 of its buffer on purpose: because
it moves bytes, alignment never matters. `u64_to_i64` converts the decoded bits back to a negative
amount without relying on implementation-defined conversion.

## 4. Under the hood (senior-level)

### Strict aliasing in the generated code

GCC 13 `-O2` for `alias_demo` (and for the `memcpy` punning function):

```asm
alias:
	movl	$1, (%rdi)            # *i = 1
	movl	$1, %eax              # return value decided NOW: *f cannot alias *i
	movl	$0x40000000, (%rsi)   # *f = 2.0f
	ret
bits:
	movd	%xmm0, %eax           # the whole memcpy: one register move
	ret
```

The Linux kernel and some other large C code bases build with `-fno-strict-aliasing` to switch this
assumption off, trading some optimisation for safety with legacy casts. Know the flag exists; do not
rely on it in new code.

### Unaligned access in hardware

x86-64 handles unaligned scalar loads in hardware. The cost appears when an access straddles a
64-byte cache line (two lines must be fetched) or a 4 KiB page (two address translations). Locked
instructions (`lock cmpxchg`, used for atomics) on a line-straddling address become **split locks**,
which lock the memory bus for all cores. On ARMv8 Linux, ordinary unaligned loads work, but
exclusive and atomic instructions on unaligned addresses fault. For data you control, natural
alignment is free performance; packing is for formats you do not control.

### How bit-field access compiles

Reading `f.kyc_level` is: load the 32-bit unit, shift right by 2, AND with 7. Writing it is: load the
unit, clear bits 2 to 4, OR in the new value shifted left, store the unit. That load-modify-store
sequence is why two threads writing different bit-fields of one unit lose each other's updates: each
writes back a stale copy of the other's bits.

### Structs across function calls

On the System V x86-64 ABI (Linux), a struct of up to 16 bytes made of integer fields is passed and
returned in registers (`rdi`/`rsi`, returned in `rax`/`rdx`); larger ones are copied to the stack,
and the caller passes a hidden pointer for the return value. On Windows x64 only structs of exactly
1, 2, 4 or 8 bytes travel in a register; everything else goes by hidden pointer. So returning a
small struct by value (module 07) is cheap on both. Details:
05_Memory_Debugging_RCA/02_stack_frames_and_calling_conventions.md.

### Reading layouts in a debugger

In gdb, `ptype /o struct txn_bad` prints every member with its offset and size and marks the holes,
just like `pahole`. In WinDbg, `dt module!txn_bad` prints offsets, and `dt module!txn_bad <address>`
decodes a struct at an address in a dump. When a field "looks wrong" in a dump, compare the offsets
your debugger shows with the offsets the writing code was compiled with: a `#pragma pack` mismatch
between two modules shows up exactly here.

## 5. In production

- **Decoding legacy records.** Records written by C code with `#pragma pack(1)` (or without it, with
  whatever padding that compiler chose) are the input of every migration. Casting
  `(struct rec *)dbt.data` is undefined behaviour (alignment) and silently depends on today's
  compiler matching the writer's. Decode each field from its offset, per-field byte order, version
  byte first, and test against hand-verified golden records
  (07_Databases_Internals/12_migrating_a_kv_store_to_relational.md).
- **Pack state leaking across modules.** A header that does `#pragma pack(1)` without `pop` makes
  the same struct 15 bytes in one DLL and 24 in another. Fields after the first padding read as
  garbage only when the struct crosses that module boundary. Use `push`/`pop` in every header, and
  add `_Static_assert(sizeof(struct x) == N, "...")` next to every struct that crosses a boundary
  (module 08).
- **Information leak through padding.** A response struct sent with `send(s, &resp, sizeof resp, 0)`
  carries uninitialised padding: bytes of earlier requests, possibly account numbers. Encode
  explicitly, or at minimum `memset` the struct first.
- **Lost flag updates.** A connection struct has `unsigned closing : 1; unsigned authenticated : 1;`
  set by different threads. Occasionally a connection "forgets" it was authenticated. Bit-fields in
  the same unit are one memory location; use separate `bool`/byte fields or atomic masks.
- **Change detection with `memcmp`.** "Has the record changed?" implemented as `memcmp` of two
  structs reports spurious changes because of padding, causing needless writes or replication
  traffic. Compare fields.
- **Adding a field to a persisted struct.** If the struct is written raw, every old file is now
  unreadable and nothing detects it: reads succeed and return shifted fields. With a versioned wire
  format, the decoder handles v1 and v2 and rejects unknown versions loudly.

## 6. Try it

1. **(easy)** Predict `sizeof` and every `offsetof` for: `struct { char a; double b; char c; }`,
   `struct { char a; char c; double b; }`, `struct { int a; char b[3]; }`,
   `struct { short s; struct { char c; int i; } in; }`. Done when a program using `offsetof` confirms
   all of them, or you can explain each miss.
2. **(easy)** Take a struct with nine fields of mixed sizes, reorder it for minimum size, and lock the
   result in with `_Static_assert(sizeof(...) == N, ...)`. Done when the assert compiles and fails if
   you swap two fields back.
3. **(medium)** Implement `container_of` and an intrusive doubly linked list (`struct dlink { struct
   dlink *prev, *next; }`). Put one `struct account` into two lists at once (by branch and by
   status). Done when removing it from one list leaves it in the other, with no extra allocation per
   list node.
4. **(medium)** Write a decoder for the 35-byte `loan_rec_v2` layout shown in
   07_Databases_Internals/12_migrating_a_kv_store_to_relational.md (little-endian ids and amount,
   big-endian branch, BCD date, space-padded fixed-width strings) using only byte offsets, no packed
   struct. Done when it decodes three hand-built golden records correctly and rejects a record with a
   wrong version or a short length.
5. **(medium)** Bit-field race: a struct with `unsigned a : 1; unsigned b : 1;`. Thread 1 toggles `a`
   one million times, thread 2 toggles `b` one million times, each counting its own toggles. Done when
   you observe a final state inconsistent with the counts (run it several times, or under TSan), then
   fix it by making `a` and `b` separate `unsigned char` fields and can explain why that fixes it.
6. **(hard)** Add version 2 to the format in `m06_serialize_record.c` with a 3-letter currency code.
   The decoder must accept v1 (currency defaults to "INR") and v2; the encoder writes v2. Then fuzz
   `txn_decode` with random buffers of random length for one minute under ASan and UBSan. Done when
   both versions round-trip in tests and the fuzzer finds nothing.

## 7. Interview questions

**Q: Why is `sizeof` a struct often larger than the sum of its members?**
The compiler inserts padding so that each member starts at a multiple of its alignment, and pads the
end so that the size is a multiple of the struct's alignment, which keeps array elements aligned.
Order matters: the same fields sorted largest first usually need much less padding.

**Q: What is `offsetof` used for, and what is `container_of`?**
`offsetof` gives a member's byte offset in a struct. `container_of` subtracts that offset from a
pointer to the member to recover a pointer to the enclosing struct; it is how intrusive lists and
trees (the Linux kernel's, for example) link objects without separate node allocations.

**Q: What does `#pragma pack(1)` do and what does it cost?**
It removes padding, placing members at unaligned offsets. Member access by name stays correct but can
be slower, especially across cache-line boundaries and on strict-alignment CPUs; pointers to packed
members are misaligned and dangerous; atomics on them are not atomic. Use it only to describe external
formats, always with `push`/`pop`, and prefer field-by-field decoding.

**Q: Are bit-fields portable? Are they thread-safe?**
Their layout (bit order, straddling, allowed types, signedness of plain `int` fields) is
implementation-defined, and MSVC and GCC differ for mixed types, so they must not define file or wire
formats. Adjacent bit-fields form one memory location, so concurrent writes to different fields of the
same unit race. Use masks on fixed-width integers for portable or shared flags.

**Q: How do you reinterpret a `float`'s bits as a `uint32_t` without undefined behaviour?**
`memcpy` into a `uint32_t`, which is correct in C and C++ and compiles to one register move. A union
also works in C (not C++). Casting `(uint32_t *)&f` violates the strict aliasing rule: the compiler
assumes pointers to different types do not alias and may produce stale results.

**Q: What is a flexible array member, and how does it differ from the "struct hack"?**
A last member declared `T data[]`; `sizeof` ignores it and you allocate `sizeof(struct) + n`. The old
hack declared `data[1]` and indexed past the end, which is undefined behaviour and is now caught by
`-fstrict-flex-arrays` and `_FORTIFY_SOURCE=3`. Both give a single allocation for header and payload.

**Q: What is an opaque type and why use one?**
A struct declared but not defined in the public header, used only through pointers and functions. It
hides the layout, enforces invariants in one file, and lets you change the struct without recompiling
callers (stable ABI). The cost is a heap allocation per object and a function call per access. `FILE *`
is the classic example.

**Q: Why should you never `fwrite` a struct to a file or a socket?**
Its bytes include padding (garbage or leaked data), depend on the compiler's type sizes and packing,
are in host byte order, and carry no version. Use an explicit format: version byte, fixed-width fields
at documented offsets, defined endianness, encoded and decoded field by field with validation.

## 8. Recap

- Members stay in declaration order; each is aligned to its type; the struct is aligned to its
  largest member and padded at the end.
- Order fields largest first to remove padding; `pahole` or `ptype /o` shows the holes.
- `offsetof` gives offsets; `container_of` turns a member pointer back into the struct pointer.
- Padding bytes are unspecified: no `memcmp`, no hashing, no raw writes of structs.
- `#pragma pack(push, 1)`/`pop` for external formats only; never take the address of a packed member
  (GCC warns for the attribute form, not for the pragma).
- Bit-field layout is implementation-defined and adjacent bit-fields are one memory location; use
  masks for anything stored, sent or shared.
- Tagged unions for variants; `memcpy` for type punning; casts that break strict aliasing are UB.
- Enums are `int` constants with a compiler-chosen size; keep them out of byte formats.
- Flexible array members give one allocation for header plus payload; replace struct hacks.
- Opaque types are C's encapsulation; designated initializers keep initialisation robust.
- On disk and on the wire: version byte, fixed-width fields, explicit endianness, field-by-field
  encode and decode.
