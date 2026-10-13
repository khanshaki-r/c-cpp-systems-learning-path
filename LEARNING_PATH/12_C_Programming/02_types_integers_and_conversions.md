# 02. Types, integers and conversions

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

Every value in C has a type, and the type fixes two things: how many bytes the value occupies, and
how its bits are read (as a signed number, an unsigned number, a floating-point number). So far so
simple. The trouble is that C **converts types silently** in almost every expression. Small types
grow into `int` before any arithmetic. When a signed value meets an unsigned one, the signed one
is turned into unsigned. A negative number becomes a huge positive one. No warning, no exception:
just a different number.

Analogy: a bank where every counter quietly exchanges your money into its own currency. Most
counters use rupees, so nobody notices. But one counter (unsigned arithmetic) has no concept of
debt: hand it minus one rupee and it gives you 4,294,967,295. A length check that should reject a
corrupt negative length now passes, and `memcpy` copies 18 quintillion bytes.

Many C bugs that end up as CVEs (published security vulnerabilities) start here: a wrong size, a
sign flip, an overflow in a length calculation. And in banking code there is a second, quieter
trap: storing money in a `double`, which cannot represent 0.10 exactly. This module covers the
rules precisely enough that you can predict every one of these results before running the code.

## 2. The real thing

### 2.1 Sizes and the data model

The standard fixes only **minimum ranges** and an ordering: `char` at least 8 bits, `short` and
`int` at least 16, `long` at least 32, `long long` at least 64, and
`sizeof(short) <= sizeof(int) <= sizeof(long) <= sizeof(long long)`. `sizeof(char)` is 1 by
definition. Everything else is the platform's choice, summarised as a **data model**:

| Data model | `int` | `long` | `long long` | pointer | Where |
|---|---|---|---|---|---|
| ILP32 | 32 | 32 | 64 | 32 | 32-bit Windows and Linux |
| **LP64** | 32 | **64** | 64 | 64 | 64-bit Linux, macOS, BSD |
| **LLP64** | 32 | **32** | 64 | 64 | 64-bit Windows (MSVC and MinGW) |

The only difference between Linux and Windows is `long`, and it is enough to break code. On
Windows, `long` cannot hold a pointer, a file size above 2 GiB, or an amount above 21,474,836.47
rupees counted in paise (`INT32_MAX` paise). Code that casts a pointer to `long` works on 64-bit
Linux and truncates on 64-bit Windows. Use `intptr_t`/`uintptr_t` for pointers-as-integers, and the
fixed-width types from `<stdint.h>` whenever the size matters. Win32 has its own names for the same
idea: `DWORD` is a 32-bit `unsigned long`, while `DWORD_PTR`, `ULONG_PTR` and `SIZE_T` are
pointer-sized.

On WSL Ubuntu 24.04 (`code/m02_data_model.c`):

```
sizes on this platform:
  char          1 bytes
  short         2 bytes
  int           4 bytes
  long          8 bytes
  long long     8 bytes
  void *        8 bytes
  size_t        8 bytes
  ptrdiff_t     8 bytes
  float         4 bytes
  double        8 bytes
  long double  16 bytes
data model: LP64 (64-bit Linux, macOS)
CHAR_BIT = 8, plain char is signed
```

`long double` is 16 bytes of storage holding the x87 80-bit extended format on x86-64 Linux. On
MSVC it is just a `double` (8 bytes). Plain `char` is signed on x86 with both GCC and MSVC, and
unsigned on ARM Linux: never rely on it.

### 2.2 Two's complement

Every mainstream CPU stores signed integers in **two's complement**: the top bit has weight
-2^(N-1) instead of +2^(N-1). C23 finally requires it; before C23 the standard also allowed
sign-magnitude and ones' complement, which no one ships.

```
 8-bit pattern    unsigned    signed (two's complement)
 0000 0000             0         0
 0000 0001             1         1
 0111 1111           127       127   <- INT8_MAX
 1000 0000           128      -128   <- INT8_MIN (no positive partner)
 1111 1110           254        -2
 1111 1111           255        -1   <- "all ones" is -1
```

Consequences you need to know cold:

- The range is asymmetric: `INT_MIN` is -2147483648 but `INT_MAX` is 2147483647. So `-INT_MIN`,
  `abs(INT_MIN)` and `INT_MIN / -1` all overflow.
- The CPU's `add` instruction is the same for signed and unsigned. Only the interpretation of the
  result (and of the overflow flags) differs.
- Converting signed to unsigned is **defined** by the standard as "add or subtract 2^N until it
  fits", which on two's complement is just "keep the bits": `(unsigned)-1 == UINT_MAX`.
- Converting an unsigned or wider value to a signed type that cannot hold it is
  **implementation-defined** (GCC and MSVC keep the low bits). Case 9 in section 3 shows
  5000000000 becoming 705032704.

### 2.3 Integer promotions

Before any arithmetic, every operand whose type is narrower than `int` (`char`, `signed char`,
`unsigned char`, `short`, `unsigned short`, `_Bool`, bit-fields) is converted to `int`, or to
`unsigned int` if `int` cannot hold all its values. This is the **integer promotion**. It is why:

- `uint8_t a = 200, b = 100; a + b` is 300, an `int`. It wraps to 44 only if you store it back
  into a `uint8_t`.
- `~(uint8_t)0x0F` is `~(int)15` = -16, not `0xF0`. `if (~mask == 0xF0)` is always false; GCC
  warns: `comparison of promoted bitwise complement of an unsigned value with constant`.
- `uint8_t x = 0x80; x << 24` is computed in `int`, and `0x80 << 24` overflows `int`: undefined.
  Cast first: `(uint32_t)x << 24`.
- A `char` holding byte `0xE9` promotes to -23 on x86. Used as an array index (`counts[c]`), it
  indexes before the array. Use `unsigned char` for bytes.

### 2.4 The usual arithmetic conversions

When a binary operator (`+ - * / % < > == & | ^ ?:`) gets two operands of different types, after
promotion, C converts them to a common type with this algorithm:

1. If either is `long double`, `double` or `float`, convert the other to that floating type.
2. Otherwise both are integers. If they have the same signedness, convert to the one with greater
   **rank** (`long long` > `long` > `int`).
3. If the unsigned one has rank greater than or equal to the signed one, convert the signed one to
   the **unsigned** type. (`int` meets `unsigned int`: both become `unsigned int`.)
4. If the signed one has greater rank and can represent every value of the unsigned type, convert
   to the signed type. (`long` meets `unsigned int` on LP64: both become `long`.)
5. Otherwise convert both to the unsigned version of the signed type.

Rule 4 versus rule 5 is a portability bomb: the **same expression gives different answers on Linux
and Windows**.

| Expression | LP64 (Linux) | LLP64 (Windows) |
|---|---|---|
| `-1 < 1u` | `int` vs `unsigned`: rule 3, `-1` becomes 4294967295: **false** | same: **false** |
| `-1L < 1u` | `long` (64) can hold every `unsigned` (32): rule 4, **true** | `long` (32) cannot: rule 5, both `unsigned long`: **false** |
| `-1 < (size_t)3` | `size_t` is `unsigned long`: rule 3, **false** | `size_t` is `unsigned long long`: **false** |

### 2.5 The signed/unsigned bugs, catalogued

These are the real-world patterns. Each is in `code/m02_promotions_and_conversions.c`.

```c
/* 1. The negative length that passes the check */
int len = read_len_from_wire();             /* -5 from a corrupt packet */
if (len > (int)sizeof buf) return ERR;      /* -5 > 64 is false: check passes */
memcpy(buf, src, len);                      /* len converts to size_t: 18446744073709551611 */

/* 2. Subtracting sizes */
if (strlen(a) - strlen(b) > 0)              /* size_t never goes negative: true unless equal */

/* 3. Counting down with an unsigned index */
for (size_t i = n - 1; i >= 0; --i)         /* i >= 0 is always true: infinite loop, then crash */
```

GCC catches the third with `-Wextra`:

```
loop.c:2:57: warning: comparison of unsigned expression in '>= 0' is always true [-Wtype-limits]
```

The correct count-down idiom is `for (size_t i = n; i-- > 0; )`: the test happens before the
decrement, so the body sees `n-1 ... 0` and the loop stops after 0. For the length check, validate
**before** converting: reject `len < 0` explicitly, or read the length as unsigned from the start.

### 2.6 Overflow: undefined for signed, wrapping for unsigned

- **Unsigned** arithmetic is defined modulo 2^N. `UINT_MAX + 1u == 0`. This is not an error as far
  as C is concerned, which is exactly why `malloc(count * size)` with a wrapped product allocates
  a tiny buffer and the following loop overflows it.
- **Signed** overflow is **undefined behaviour**. The compiler assumes it never happens. GCC folds
  `x + 1 < x` to `false` even at `-O0`, so the "overflow check" disappears.
- Other integer UB: dividing by zero; `INT_MIN / -1` and `INT_MIN % -1`; shifting by a negative
  count or by `>=` the width of the (promoted) type; left-shifting a negative value or shifting a
  1 into the sign bit (`1 << 31` on 32-bit `int`).
- Implementation-defined: right shift of a negative value (`-8 >> 1` is -4 on GCC and MSVC: an
  arithmetic shift).
- Defined but surprising: integer division truncates toward zero since C99, so `-7 / 2 == -3` and
  `-7 % 2 == -1`.

Checking correctly means **testing before the operation**, using only operations that cannot
overflow:

```c
static bool add_i64_checked(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *out = a + b;
    return true;
}

static bool mul_size_checked(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}
```

GCC and Clang also provide `__builtin_add_overflow(a, b, &result)` (and `_sub_`, `_mul_`), which
compile to the arithmetic instruction plus a jump on the overflow flag. C23 standardises them as
`ckd_add`, `ckd_sub`, `ckd_mul` in `<stdckdint.h>`. MSVC has neither; use the portable form.

Compiler switches that change the rules: `-fwrapv` makes signed overflow wrap (defined), at some
optimisation cost; `-ftrapv` aborts on signed overflow; `-fsanitize=undefined` reports it at run
time (section 3.3 shows the report).

### 2.7 `size_t`, `ptrdiff_t` and friends

| Type | Header | What it is | Print with |
|---|---|---|---|
| `size_t` | `<stddef.h>` | unsigned; type of `sizeof`, every size and count in the library | `%zu` |
| `ptrdiff_t` | `<stddef.h>` | signed; type of `p - q` for pointers into the same array | `%td` |
| `ssize_t` | `<sys/types.h>` (POSIX, not ISO C) | signed `size_t`; `read`/`write` return it, -1 on error | `%zd` |
| `intptr_t`, `uintptr_t` | `<stdint.h>` | integers wide enough to hold a pointer | `PRIdPTR`, `PRIuPTR` |
| `intmax_t` | `<stdint.h>` | widest signed integer | `%jd` |

Whether to index with `int` or `size_t` is a style debate. The senior answer: the bugs come from
**mixing** them. Pick one per interface, keep comparisons between same-signedness values, and turn
on `-Wsign-compare` (in `-Wextra`) so the compiler shows you every mix.

### 2.8 `<stdint.h>`: say how big you mean

- Exact width: `int8_t`, `int16_t`, `int32_t`, `int64_t` and the `uint` versions. Use them for
  anything stored on disk, sent over the network, or holding money.
- At least / fastest: `int_least32_t`, `int_fast32_t`. Rarely needed.
- Literal macros: `INT64_C(5000000000)`, `UINT32_C(1)`. They append the right suffix (`L` on
  Linux, `LL` on Windows), so `UINT64_C(1) << 40` works everywhere while `1 << 40` is UB.
- Limits: `INT64_MAX`, `UINT32_MAX`, `SIZE_MAX`.

### 2.9 printf and scanf format specifiers

A mismatch between the format and the argument's type is **undefined behaviour**, and on 64-bit
it really breaks: `printf("%d", some_size_t)` reads 4 bytes of an 8-byte argument. GCC checks
literal formats with `-Wformat` (in `-Wall`).

| Type | printf | scanf |
|---|---|---|
| `int` / `unsigned` | `%d` / `%u`, `%x` | same |
| `long` / `unsigned long` | `%ld` / `%lu` | same |
| `long long` | `%lld` / `%llu` | same |
| `size_t` / `ptrdiff_t` | `%zu` / `%td` | same |
| `int64_t` / `uint64_t` | `"%" PRId64` / `"%" PRIu64`, `PRIx64` | `"%" SCNd64` |
| `int32_t` / `uint32_t` | `PRId32` / `PRIu32` | `SCNd32` |
| `void *` | `%p` | rarely |
| `double` | `%f`, `%g`, `%.17g` for round-trip | `%lf` (scanf needs the `l`) |

`int64_t` is `long` on Linux and `long long` on Windows, so no single `%ld` or `%lld` is right
everywhere. That is what the `PRId64` macros from `<inttypes.h>` solve. Legacy MSVC code uses
`%I64d`; it still works on Windows but is not portable.

### 2.10 Floating point basics, and why money is never a `double`

A `double` is IEEE 754 binary64: 1 sign bit, 11 exponent bits, 52 fraction bits. It holds about 15
to 17 significant decimal digits and represents numbers as `fraction x 2^exponent`. Any value that
is not a sum of powers of two, such as 0.1, is stored as the nearest representable binary value:

- `0.1 + 0.2` is `0.30000000000000004`, so `0.1 + 0.2 == 0.3` is false.
- Addition is not associative: `(0.1 + 0.2) + 0.3` is `0.60000000000000009` but
  `0.1 + (0.2 + 0.3)` is `0.59999999999999998`. Two systems that sum the same transactions in a
  different order get different totals.
- `printf("%.2f", 2.675)` prints `2.67`, because the stored value is slightly below 2.675.
- Every integer up to 2^53 (9,007,199,254,740,992) is exact; above that, only even numbers are.
  A `float` is exact only up to 2^24 = 16,777,216: `16777217.0f` prints as `16777216.0`.
- `NaN != NaN`, and `0.0 == -0.0` is true although the bits differ.

For money this is unacceptable. Rules for banking code:

1. **Store amounts as integers of the smallest unit**: `int64_t` paise or cents. An `int64_t` holds
   about 92 quadrillion rupees in paise, so range is not the issue. Some currencies have three
   decimals (Kuwaiti dinar, 1000 fils), so a multi-currency system stores the scale per currency.
2. **Or use a decimal type** where one exists: PostgreSQL and YugabyteDB `NUMERIC(18,2)`, COBOL
   packed decimal, Java `BigDecimal`. Never convert through `double` on the way in or out
   (07_Databases_Internals/12_migrating_a_kv_store_to_relational.md, decoding rule 4).
3. **Make rounding explicit**: where it happens (per transaction, per day, at the end), and which
   rule (half-up, or half-even "banker's rounding", which rounds ties to the even digit to avoid
   systematic bias). Write it in the code and the specification.
4. Interest = principal x rate x time involves division, so it needs rounding. Do it in integers
   with the rate in basis points (1 bp = 0.01 %), multiply first, check for overflow, and round
   once at the end. Section 3.4 shows it.

### 2.11 Endianness

A multi-byte integer is stored at several consecutive addresses. **Little-endian** puts the least
significant byte at the lowest address (x86, x86-64, and ARM as configured by every mainstream OS).
**Big-endian** puts the most significant byte first (network protocols, "network byte order";
older SPARC, POWER/AIX and mainframe systems).

```
value 0x11223344 at address A:
                 A     A+1   A+2   A+3
little-endian:  44    33    22    11      (x86: what memcpy into a uint32_t gives you)
big-endian:     11    22    33    44      (network byte order)
```

Endianness matters whenever bytes cross a boundary: a socket, a file, a database record, a
memory-mapped device. Within one process it never matters, as long as you do not reinterpret
memory. The portable way to read or write a multi-byte field from a byte buffer is to assemble it
with shifts; it works on any host, needs no `#ifdef`, and the compiler turns it into one
instruction (section 4). `htonl`/`ntohl` (from `<arpa/inet.h>`, or `<winsock2.h>` on Windows) do
the same for 16 and 32 bits.

## 3. Code

Build any file with `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g file.c -o file`.

### 3.1 Data model and portable printing: `code/m02_data_model.c`

```c
int64_t   balance_paise = INT64_C(-123456789012);
uint32_t  account_no    = UINT32_C(4000000000);
printf("int64_t   via PRId64: %" PRId64 "\n", balance_paise);
printf("uint32_t  via PRIu32: %" PRIu32 "\n", account_no);
printf("size_t    via %%zu  : %zu\n", n);
printf("ptrdiff_t via %%td  : %td\n", gap);
```

Output after the size table in section 2.1:

```
INT_MAX  = 2147483647
LONG_MAX = 9223372036854775807
SIZE_MAX = 18446744073709551615
int64_t   via PRId64: -123456789012
uint32_t  via PRIu32: 4000000000
size_t    via %zu  : 10
ptrdiff_t via %td  : 5
intmax_t  via %jd  : 9223372036854775807
```

### 3.2 Promotions and conversions: `code/m02_promotions_and_conversions.c`

Nine cases, each with an assert. Two of them compare signed with unsigned on purpose; GCC rejects
that under `-Werror` (`-Wsign-compare`, part of `-Wextra`; MSVC warning C4018), so the file turns
the warning off for those lines only:

```c
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    int lt = (minus_one < one);            /* -1 becomes 4294967295u, so this is FALSE */
#pragma GCC diagnostic pop
```

That pragma is for teaching. In real code the warning is telling you to fix the types.

```c
    char buf[64];
    int len = -5;                          /* e.g. a corrupt length field read from the wire */
    int passes_check = (len <= (int)sizeof buf);            /* -5 <= 64: true, check passes */
    size_t what_memcpy_sees = (size_t)len;                  /* 18446744073709551611 */
```

Output:

```
1. uint8_t 200 + 100 = 300 in int, 44 when stored back into uint8_t
2. ~(uint8_t)0x0F = -16 as int, 0xF0 after casting back to uint8_t
3. char holding 0xE9 -> -23 as int, 233 via unsigned char
4. (-1 < 1u) is false
5. len=-5 passes 'len <= sizeof buf': yes; memcpy would copy 18446744073709551611 bytes
6. strlen("abc") - strlen("abcdef") = 18446744073709551613
7. (-1 < (size_t)3) is false
8. UINT32_C(1) << 31 = 2147483648, UINT64_C(1) << 40 = 1099511627776
9. int64_t 5000000000 squeezed into int32_t = 705032704
```

### 3.3 Overflow: `code/m02_overflow.c`

This file contains **deliberate undefined behaviour** (it says so in its header): it calls
`naive_increment_overflows(INT_MAX)`, which evaluates `INT_MAX + 1`. It still compiles cleanly with
`-Werror`, which is the frightening part: no warning at all.

```c
/* UB when x == INT_MAX. GCC folds "x + 1 < x" to 0 (false) even at -O0. */
static int naive_increment_overflows(int x) { return x + 1 < x; }
```

Output:

```
naive_increment_overflows(INT_MAX) = 0   <- UB: 0 means the check was deleted
UINT_MAX + 1u = 0                       <- defined: unsigned arithmetic wraps
add_i64_checked(INT64_MAX - 10, 50)   -> rejected, would overflow
__builtin_add_overflow(LLONG_MAX, 1)  -> overflow detected
mul_size_checked(2^62, 8)             -> rejected, would wrap to 0
unchecked (size_t)2^62 * 8            =  0
INT_MIN = -2147483648 has no positive partner: INT_MAX = 2147483647
```

The first line is 0 at `-O0` and at `-O2`. With `-fwrapv` it becomes 1 (signed overflow is then
defined to wrap). Built with `gcc -std=c17 -g -fsanitize=undefined m02_overflow.c`, UBSan reports
the overflow at run time, and its instrumentation also stops the folding:

```
m02_overflow.c:21:56: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
naive_increment_overflows(INT_MAX) = 1   <- UB: 0 means the check was deleted
```

### 3.4 Money: `code/m02_money_and_float.c`

```c
typedef int64_t paise_t;

/* Simple interest for `days` at `rate_bp` basis points a year (1 bp = 0.01 %), ACT/365,
 * rounded half-up to the paisa. Every step is integer arithmetic with an overflow check. */
static bool simple_interest(paise_t principal, int64_t rate_bp, int64_t days, paise_t *out)
{
    if (principal < 0 || rate_bp < 0 || days < 0) return false;
    if (rate_bp != 0 && principal > INT64_MAX / rate_bp) return false;
    int64_t t = principal * rate_bp;
    if (days != 0 && t > INT64_MAX / days) return false;
    t *= days;
    const int64_t denom = 10000 * 365;
    if (t > INT64_MAX - denom / 2) return false;
    *out = (t + denom / 2) / denom;           /* add half the divisor, then truncate: half-up */
    return true;
}
```

The formatter computes the magnitude as `uint64_t` so that even `INT64_MIN` prints correctly
(negating `INT64_MIN` as a signed value would overflow):

```c
uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
```

Output:

```
0.1 + 0.2 = 0.30000000000000004, equal to 0.3? no
one million x 0.10: double = 100000.0000013329, int64 paise = 100000.00
printf("%.2f", 2.675) = 2.67 (you expected 2.68)
int64 9007199254740993 -> double -> int64 9007199254740992
interest on 1000000.00 at 7.25% for 30 days = 5958.90
INT64_MIN paise formats as -92233720368547758.08
```

### 3.5 Endianness: `code/m02_endianness.c`

```c
/* The (uint32_t) casts matter: p[0] << 24 alone promotes p[0] to int, and 0x80 << 24 overflows int. */
static uint32_t load_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
```

Output:

```
this host is little-endian
0x11223344 as stored in memory:    44 33 22 11
store_be32 (network byte order):   11 22 33 44
store_le32:                        44 33 22 11
length 256 sent big-endian: memcpy reads 65536, load_be32 reads 256
```

The last line is a real protocol bug in one line: a peer sends the length 256 in network order and
the x86 receiver `memcpy`s it into a `uint32_t`, getting 65536.

## 4. Under the hood (senior-level)

### One adder, two interpretations

x86 `add` produces the same bits for signed and unsigned operands and sets two flags: CF (carry,
meaning unsigned overflow) and OF (overflow, meaning signed overflow). C exposes neither, which is
why checked arithmetic is either a pre-test or a compiler builtin. `__builtin_add_overflow` on
signed operands compiles to `add` followed by `jo` (jump if OF); on unsigned, `add` then `jc`.
Unsigned wrap costs nothing because it is literally what the hardware does.

### Why signed overflow is undefined: loop optimisation

Consider `for (int i = 0; i <= n; ++i) a[i] = 0;` on x86-64. Because signed overflow cannot
happen, the compiler may assume the loop runs exactly `n + 1` times, keep `i` in a 64-bit register
(no sign-extension on every array access), and vectorise. With an `unsigned` 32-bit index it must
preserve wrap-around at 2^32, which can block both. This is the performance argument the standard
committee accepted, and it is also why "obviously harmless" overflow can produce very non-obvious
code.

### Promotions in machine code

x86 has 8- and 16-bit registers, but C semantics are defined in `int`. Loading a `char` produces
`movsx` (sign-extend, because `char` is signed); loading an `unsigned char` produces `movzx`
(zero-extend). When you see a byte value that came out as `0xFFFFFFE9` in a debugger, a `movsx`
on a signed `char` is why.

### What the endian helpers compile to

GCC 13 at `-O2` recognises the shift-and-or pattern and emits one load, plus `bswap` for the
foreign byte order:

```asm
load_be32:
	movl	(%rdi), %eax
	bswap	%eax
	ret
load_le32:
	movl	(%rdi), %eax
	ret
```

So the portable code costs nothing. There is no reason to cast a buffer to `uint32_t *`, which is
both an alignment hazard and a strict-aliasing violation (module 06).

### Excess precision: Debug and Release disagree on floating point

On 32-bit x86 with the old x87 FPU (`gcc -m32 -mfpmath=387`, old MSVC `/arch:IA32`), intermediate
results are kept in 80-bit registers and only rounded to 64 bits when stored to memory. Whether a
value is spilled to memory depends on optimisation and register pressure, so the same calculation
can give different last digits in Debug and Release. `FLT_EVAL_METHOD` in `<float.h>` tells you
which mode applies (0 on x86-64, where SSE2 is used). One more reason money stays in integers.

## 5. In production

- **The 21-lakh-rupee overflow.** A balance or a batch total kept as `long` paise on Windows, or as
  `int` anywhere, overflows at `INT32_MAX` = 2,147,483,647 paise = 21,474,836.47 rupees. A single
  large corporate payment or a daily branch total crosses it. Symptoms: totals that turn negative,
  or reconciliation reports that are off by exactly 2^32 paise. Use `int64_t` for every amount and
  every sum of amounts.
- **Windows to Linux port.** Code that used `long` as "the 32-bit type" now has 64-bit values in
  structs that are written to disk or sent over sockets: record sizes change and old files no
  longer parse. Code that used `long` to hold a pointer worked on Linux and truncates on Windows.
  Use `<stdint.h>` types in every on-disk and on-wire structure.
- **MSVC C4267 and C4018.** `conversion from 'size_t' to 'int', possible loss of data` and
  `signed/unsigned mismatch` appear by the hundred in old code bases and get disabled. Each one is
  a candidate for the bugs in section 2.5. When a crash involves a length or an index, grep the
  build log for these warnings on the lines involved.
- **Length fields from the network or from files** must be validated as the type you will use
  them as: reject negative values and values above the buffer size before any conversion. Section
  2.5 bug 1 is the root cause of many buffer overflows; see
  05_Memory_Debugging_RCA/04_the_catalogue_of_memory_bugs.md (integer overflow leading to a bad
  size).
- **Reconciliation mismatches.** Two systems compute the same interest or fee, one in `double` and
  one in decimal, and differ by a paisa on some accounts. The fix is to agree on integer or decimal
  arithmetic and on the rounding rule, not to add a tolerance.
- **Finding these bugs.** `-Wconversion -Wsign-conversion` are noisy but show every silent
  narrowing (`conversion from 'int64_t' to 'int32_t' may change value`); turn them on per file
  when investigating. Run tests with `-fsanitize=undefined`. On MSVC, `/W4` and do not suppress
  C4244, C4267, C4018.

## 6. Try it

1. **(easy)** Without running anything, write down what `m02_data_model.c` prints on 64-bit
   Windows. Done when you have the full size table and the data model line, and can name the one
   line that differs from Linux.
2. **(easy)** Before running `m02_promotions_and_conversions.c`, predict all nine output lines.
   Done when you can explain each wrong prediction using the rule from section 2.3 or 2.4.
3. **(medium)** Write `bool mul_i64_checked(int64_t a, int64_t b, int64_t *out)` without builtins,
   covering all sign combinations and `INT64_MIN * -1`. Done when a loop of 100,000 random pairs
   (include the edge values 0, 1, -1, `INT64_MAX`, `INT64_MIN`) agrees with
   `__builtin_mul_overflow` on every pair.
4. **(medium)** Write three correct versions of "visit indices n-1 down to 0" with a `size_t` index,
   and one deliberately broken version. Done when the broken one triggers `-Wtype-limits` and the
   three correct ones handle `n == 0` without touching the array.
5. **(medium)** Monthly compound interest: 1,00,000.00 rupees at 9 % a year for 12 months, rounding
   to the paisa every month. Implement it once with `double` and once with `int64_t` paise using
   half-even rounding. Done when you print both results side by side and can explain any
   difference, and your integer version matches a hand calculation for month 1.
6. **(hard)** Write `load_be64`, `store_be64`, and a parser for a 16-byte header
   (`u32 magic, u16 version, u16 opcode, u64 length`, all big-endian). Compile with `-O2 -S`. Done
   when you can show the `bswap` (or `movbe` with `-march=native` on a CPU that has it) in the
   assembly and your test round-trips 1000 random headers.

## 7. Interview questions

**Q: What is the difference between LP64 and LLP64, and why does it matter?**
LP64 (Linux, macOS) has 64-bit `long` and pointers; LLP64 (64-bit Windows) keeps `long` at 32 bits
and makes only `long long` and pointers 64-bit. Code that stores pointers, file sizes or large
amounts in `long` works on one and truncates on the other, and struct layouts written to disk
change. Use `<stdint.h>` fixed-width types and `intptr_t` where the size matters.

**Q: What are the integer promotions? What is `(uint8_t)200 + (uint8_t)100`?**
Operands narrower than `int` are converted to `int` (or `unsigned int` if `int` cannot hold them)
before arithmetic. So the sum is the `int` 300; it only becomes 44 if stored back into a
`uint8_t`. The same rule makes `~` on a `uint8_t` produce a negative `int`.

**Q: What does `-1 < 1u` evaluate to, and why?**
False. Under the usual arithmetic conversions, `int` meeting `unsigned int` converts the `int` to
`unsigned int`, so -1 becomes `UINT_MAX`. The same happens with `size_t`, which is why comparing a
signed length with `sizeof` is dangerous, and why `-Wsign-compare` exists.

**Q: Why is signed overflow undefined while unsigned overflow is defined? How do you check for it?**
Unsigned arithmetic is defined modulo 2^N, matching the hardware. Signed overflow is undefined so
the compiler can assume it does not happen, which enables loop and range optimisations; as a result
checks written as `a + b < a` are deleted. Check before the operation with comparisons that cannot
overflow (`a > INT64_MAX - b`), or use `__builtin_add_overflow` / C23 `ckd_add`.

**Q: How do you print an `int64_t` and a `size_t` portably?**
`printf("%" PRId64, x)` using the macro from `<inttypes.h>`, because `int64_t` is `long` on Linux
and `long long` on Windows; and `%zu` for `size_t`. A mismatched specifier is undefined behaviour
and on 64-bit platforms prints garbage.

**Q: Why should money never be stored in a `double`, and what do you use instead?**
Binary floating point cannot represent most decimal fractions exactly, so sums drift, rounding at
display differs from expectations, and summing in a different order gives different totals. Use
integers of the smallest unit (`int64_t` paise) or a decimal type such as SQL `NUMERIC`, and define
the rounding rule explicitly.

**Q: How do you read a big-endian 32-bit integer from a byte buffer portably?**
Assemble it with shifts on `uint32_t`-cast bytes:
`(uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]`. It is independent of
host byte order and alignment, avoids strict-aliasing problems, and compilers turn it into a single
load plus `bswap`.

**Q: What is wrong with `for (size_t i = n - 1; i >= 0; --i)`?**
`i >= 0` is always true for an unsigned type, so after 0 the index wraps to `SIZE_MAX` and the loop
reads far out of bounds; it also misbehaves for `n == 0`. Use `for (size_t i = n; i-- > 0; )`.

## 8. Recap

- Only minimum ranges are guaranteed; the data model decides the rest. Linux is LP64, Windows is
  LLP64: `long` is the difference.
- Two's complement: -1 is all ones, the range is asymmetric, `INT_MIN` has no positive partner.
- Integer promotion: anything narrower than `int` becomes `int` before arithmetic.
- Usual arithmetic conversions: signed meets unsigned of equal or higher rank, the signed value
  becomes unsigned; `-1 < 1u` is false; `-1L < 1u` differs between Linux and Windows.
- Signed overflow is UB and checks after the fact get deleted; unsigned wraps. Check before you
  compute, or use the builtins / C23 `ckd_*`.
- `size_t` for sizes (`%zu`), `ptrdiff_t` for pointer differences (`%td`), `PRId64` for
  `int64_t`; a wrong format specifier is UB.
- Use `<stdint.h>` exact-width types for disk, network and money.
- Money is `int64_t` minor units or a decimal type, with an explicit rounding rule; never `double`.
- Endianness only matters at boundaries; read and write multi-byte fields with shifts, which cost
  one instruction after optimisation.
