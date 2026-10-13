# 05. Strings and buffers

**Time:** ~3 hours   **Prerequisites:** 02, 03, 04

## 1. In plain words

C has no string type. A "string" is a **convention**: some bytes in a `char` array, ending at the
first byte whose value is zero (the NUL character, written `'\0'`). Every string function walks
the bytes until it finds that zero. Everything else in this module follows from that one fact:

- The **length** of a string (bytes before the NUL) and the **size** of the array holding it are
  different numbers, and C tracks neither for you.
- A string of length 5 needs 6 bytes. Forget the `+ 1` and you write one byte past the end.
- If the NUL is missing, functions keep walking into whatever memory comes next.

Analogy: a train whose only end marker is the red caboose. The platform (the buffer) has a fixed
length. The train (the string) must fit on the platform *including the caboose*. Someone who
counts carriages walks until they see the caboose; if it fell off, they keep walking onto the next
train's track. A buffer overflow is a train longer than its platform: the extra carriages crash into
whatever is parked next to it, which in memory is another variable, a heap header, or the return
address of the current function.

For a maintenance engineer this module is the highest return on time: string handling is where the
largest share of crashes, corrupted log lines and security bugs in old C servers comes from. You
will learn which standard functions are traps, how to build strings safely with `snprintf`, how to
parse numbers without `atoi`, and what UTF-8 means for "length".

## 2. The real thing

### 2.1 The representation

```c
char buf[8] = "hi";
```

```
 index:   0     1     2     3     4     5     6     7
 buf:   ['h'] ['i'] [\0]  [\0]  [\0]  [\0]  [\0]  [\0]    sizeof buf == 8, strlen(buf) == 2
                     ^ the string ends here; the rest is spare capacity
```

- `'\0'` is the character with value 0. `'0'` is the digit zero (value 48). `NULL` is a null
  *pointer*. Three different things.
- A string literal `"hi"` has type `char[3]` (NUL included) and lives in read-only memory. Writing
  to it is undefined behaviour and in practice a crash.
- Any function that writes a string needs to know the **capacity** of the destination; any function
  that reads one relies on the **terminator**. Bugs come from violating one or the other.

### 2.2 `<string.h>`: what each function really promises

| Function | Bounds | Traps |
|---|---|---|
| `strlen(s)` | none | O(n) every call; reads past the end if `s` is not terminated |
| `strcpy(d, s)` | **none** | overflows `d` if `strlen(s) + 1 > sizeof d` |
| `strcat(d, s)` | **none** | same, plus re-scans `d` from the start each call: a loop of `strcat`s is O(n squared) |
| `strncpy(d, s, n)` | writes exactly `n` | **no terminator** when `strlen(s) >= n`; zero-pads when shorter (section 2.3) |
| `strncat(d, s, n)` | appends at most `n` chars, then a NUL | `n` is the *append limit*, not the size of `d`. Correct `n` is `sizeof d - strlen(d) - 1` |
| `strcmp(a, b)` | none | returns <0, 0, >0 (not -1/0/1); **0 means equal**, so `if (strcmp(a, b))` means "different" |
| `strncmp(a, b, n)` | compares at most `n` | handy for prefixes: `strncmp(s, "GET ", 4) == 0` |
| `strchr`, `strrchr`, `strstr` | none | return a pointer into the string, or NULL: check it |
| `strspn`, `strcspn` | none | lengths of the leading run of characters in (or not in) a set: the basis of safe tokenising |
| `memcpy(d, s, n)` | exactly `n` | `d` and `s` must **not overlap** |
| `memmove(d, s, n)` | exactly `n` | handles overlap |
| `memset`, `memcmp`, `memchr` | exactly `n` | no NUL handling at all: work on bytes |
| `strdup(s)` | allocates | POSIX (and C23); caller frees; MSVC spells it `_strdup` |

And two comparisons people get wrong: `if (s == "PAID")` compares **addresses** (GCC warns,
`-Waddress`), and two arrays with equal contents are never `==`. Use `strcmp`.

### 2.3 Why `strncpy` is not a safe `strcpy`

`strncpy` was designed in early Unix for fixed-width, NUL-padded fields (14-byte file names in
directory entries), not as a bounded copy. From `code/m05_string_traps.c`, copying into
`char dst[8]` that was pre-filled with `#`:

```
strncpy(dst[8], src):           a  b  c \0 \0 \0 \0 \0
  NUL-terminated?              yes
strncpy(dst[8], src):           A  B  C  D  E  F  G  H
  NUL-terminated?              NO - strlen(dst) would read past the end
```

Two surprises:

1. When the source is as long as `n` or longer, **no terminator** is written. The next `strlen`,
   `printf("%s")` or `strcpy` on `dst` runs off the end.
2. When the source is shorter, it **pads the rest with zeros**, always writing all `n` bytes.
   `strncpy(buf, "x", 65536)` writes 64 KiB.

GCC recognises the trap when it can see the sizes (`-Wstringop-truncation`, part of `-Wall`):

```
warning: '__builtin_strncpy' output truncated before terminating nul copying as many bytes from a string as its length [-Wstringop-truncation]
```

Safer options: `snprintf(d, sizeof d, "%s", s)`; a `strlcpy`-style helper (BSD; in glibc since
2.38; section 3.2 writes one); on Windows `strcpy_s` or `strncpy_s(d, sizeof d, s, _TRUNCATE)`.
The one legitimate use of `strncpy` is its original one: filling a fixed-width field that is
defined as "NUL-padded, not necessarily terminated".

### 2.4 `snprintf`: the safe string builder

```c
int n = snprintf(dst, size, fmt, ...);
```

The contract (C99 and later):

- Writes at most `size - 1` characters and **always** a terminating NUL, when `size > 0`.
- Returns the number of characters it **would** have written with unlimited space (not counting
  the NUL), or a negative value on an encoding error.
- So truncation happened if and only if `n < 0 || (size_t)n >= size`.

The append pattern keeps an offset and stops on the first truncation:

```c
size_t used = 0;
int n = snprintf(buf + used, sizeof buf - used, "acct=%s ", acct);
if (n < 0 || (size_t)n >= sizeof buf - used) goto truncated;
used += (size_t)n;
```

`vsnprintf` is the same with a `va_list`, which is how you build your own printf-style helpers
(section 3.2, and module 07). Two cautions: before Visual Studio 2015, MSVC had only `_snprintf`,
which does **not** terminate on truncation and returns -1, and old Windows code still calls it; and
the format must be a literal you wrote, never data you received (module 07).

### 2.5 `memcpy` versus `memmove`

`memcpy` is declared with `restrict` pointers (module 03): source and destination must not overlap.
`memmove` behaves as if it copied through a temporary buffer, so overlap is fine; in practice it
chooses the copy direction. Shifting elements inside one array (insert at the front, remove from
the middle, compact a receive buffer after consuming a message) always overlaps:

```c
memmove(&q[1], &q[0], used * sizeof q[0]);   /* shift right by one: overlapping, so memmove */
```

Calling `memcpy` on overlapping buffers is undefined behaviour, and it does break: in 2010 a glibc
optimisation made `memcpy` copy backwards on some CPUs, and programs that had been passing
overlapping buffers for years (Adobe Flash among them) started producing corrupted output. If in
doubt, use `memmove`; the cost difference is negligible.

### 2.6 Parsing numbers: never `atoi`

`atoi("12abc")` is 12, `atoi("abc")` is 0, `atoi("1O0")` (letter O) is 1, and an out-of-range input
is undefined behaviour. There is no way to tell "zero" from "not a number". In banking code that is
how a malformed amount field becomes a zero-amount transaction.

Use the `strto*` family (`strtol`, `strtoll`, `strtoul`, `strtoull`, `strtod`), and check all four
failure modes:

```c
char *end;
errno = 0;                                   /* 1. strtoll SETS errno on error, never clears it */
long long v = strtoll(s, &end, 10);          /* 2. always give the base: base 0 reads "010" as octal 8 */
if (end == s)          -> not a number       /* 3. no digits consumed */
if (errno == ERANGE)   -> out of range       /*    v is LLONG_MAX or LLONG_MIN */
if (*end != '\0')      -> trailing garbage   /* 4. "12abc" */
```

More traps:

- `strtoul("-1", ...)` **succeeds** and returns `ULONG_MAX`: the unsigned functions accept a minus
  sign and negate. Reject the `-` yourself.
- `strtol` skips leading whitespace; trailing whitespace is up to you (allow it if input comes from
  `fgets`, which keeps the `\n`).
- Base 0 auto-detects `0x` (hex) and a leading `0` (**octal**): `strtol("010", NULL, 0)` is 8.
  Account numbers and branch codes with leading zeros must be parsed with base 10.
- Range-check against the type you will store into (`strtol` into `long` then cast to `int`
  silently truncates on Linux).
- `strtod` honours the current locale's decimal point. A program that calls
  `setlocale(LC_ALL, "")` on a machine with a comma locale parses "12.50" as 12.

### 2.7 `sscanf` pitfalls

`sscanf` looks convenient and has five traps:

1. It returns the number of **successful conversions**. `sscanf("12 abc", "%d %d", &a, &b)` returns
   1 and leaves `b` unchanged. Always compare the return value with the number you expected.
2. `%s` and `%[...]` with no width are as dangerous as `gets`. The width is the maximum number of
   characters, so for `char name[8]` write `%7s`.
3. `%d` on a number that does not fit in an `int` is **undefined behaviour**. For untrusted input,
   extract the digits and use `strtol`.
4. Trailing garbage is silently ignored: `"250xyz"` matches `%d`. Use `%n` to learn how many
   characters were consumed, then check that you are at the end.
5. Most conversions skip leading whitespace, but `%c` and `%[` do not.

### 2.8 `strtok` versus `strtok_r`

`strtok(s, delims)` splits a string in place. It has three problems:

- It **modifies the input**: every delimiter it passes becomes `'\0'`. You cannot tokenise a string
  literal or a buffer you still need.
- It keeps its position in **one hidden static variable** for the whole process. Nested
  tokenising (split records on `;`, then each record on `=`) destroys the outer position, and two
  threads using it at once corrupt each other.
- It treats a run of delimiters as one, so `"a,,b"` gives two tokens; CSV needs three, with an
  empty middle field.

`strtok_r(s, delims, &saveptr)` (POSIX) keeps the position in a variable you own, so nesting and
threads are fine. On Windows the equivalent is `strtok_s(s, delims, &context)` with the same three
parameters. Beware: C11 Annex K also defines a `strtok_s`, with **four** parameters and different
semantics; it is not the same function. For fields that may be empty, use `strcspn` to find each
delimiter without modifying anything (section 3.4), or `strsep` (BSD and glibc), which keeps empty
fields but still modifies the input.

### 2.9 UTF-8 basics

Text from outside your program (customer names, addresses, narration fields) is increasingly UTF-8.
UTF-8 encodes each Unicode code point in 1 to 4 bytes:

| Code points | Bytes | Pattern |
|---|---|---|
| U+0000 to U+007F | 1 | `0xxxxxxx` (plain ASCII) |
| U+0080 to U+07FF | 2 | `110xxxxx 10xxxxxx` |
| U+0800 to U+FFFF | 3 | `1110xxxx 10xxxxxx 10xxxxxx` |
| U+10000 to U+10FFFF | 4 | `11110xxx 10xxxxxx 10xxxxxx 10xxxxxx` |

Consequences:

- ASCII is valid UTF-8, and no multi-byte sequence contains a zero byte, so NUL-terminated
  functions work on UTF-8 **as bytes**.
- `strlen` counts **bytes**. The rupee sign U+20B9 is 3 bytes (`E2 82 B9`), so `strlen("₹100")` is
  6 while a person counts 4 characters. Column widths, field limits and truncation must be defined
  in bytes or in code points, and the code must agree with the database.
- Continuation bytes always look like `10xxxxxx`. Count code points by counting bytes that are not
  continuation bytes. Truncate by backing up to a byte that is not a continuation byte, or you
  produce invalid UTF-8 that the database rejects.
- Validation must reject overlong encodings (`C0 AF` for `/`, a classic filter-bypass trick),
  UTF-16 surrogates (U+D800 to U+DFFF), values above U+10FFFF, and truncated sequences.
- `<ctype.h>` functions (`isalpha`, `toupper`) take an `int` that must be `EOF` or an
  `unsigned char` value. Passing a plain `char` holding a byte >= 0x80 (negative on x86) is
  undefined behaviour: cast to `unsigned char`. They classify single bytes in the current locale;
  they cannot upper-case a multi-byte character.
- Windows uses UTF-16 internally (`wchar_t` is 2 bytes; the `...W` Win32 APIs), Linux uses UTF-8
  (`wchar_t` is 4 bytes). Convert at the boundary with `MultiByteToWideChar(CP_UTF8, ...)` and
  `WideCharToMultiByte`. Old banking data is often in a single-byte code page such as Windows-1252;
  convert it explicitly before it reaches a UTF-8 database
  (07_Databases_Internals/12_migrating_a_kv_store_to_relational.md, rule 3).

### 2.10 Rules for your own string helpers

Every helper that writes into a buffer should follow the same contract, so callers never have to
remember which one is which:

1. Take the destination **and its size**, as `char *dst, size_t dstsz`.
2. **Always terminate** when `dstsz > 0`, even on truncation.
3. **Report truncation**: return the length that was needed (the `snprintf`/`strlcpy` convention)
   or an explicit status. Never truncate silently.
4. Treat non-terminated input (fixed-width record fields) with a separate function that takes the
   field width, so nobody ever calls `strlen` on such a field.

## 3. Code

### 3.1 The traps, demonstrated safely: `code/m05_string_traps.c`

Two demos hand their input through a function parameter. If GCC could see the literals it would
refuse to compile (`-Wstringop-truncation`, `-Wformat-truncation`): the compiler knows these traps
and catches the cases it can prove at compile time. In real code the data comes from outside, so it
cannot.

```
1. sizeof p = 8, sizeof a = 32, strlen = 5
strncpy(dst[8], src):           a  b  c \0 \0 \0 \0 \0
  NUL-terminated?              yes
strncpy(dst[8], src):           A  B  C  D  E  F  G  H
  NUL-terminated?              NO - strlen(dst) would read past the end
3. strncat with the right n: "/var/log/server" (15 chars, fits in 16)
4. strcmp("abc","abd") < 0, strcmp("abc","abc") == 0 is true
5. two "PAID" arrays: == says different, strcmp says equal
6. snprintf wanted 13 chars, buffer holds 7: got "TXN-123" -> TRUNCATED
7. "IFSC0001" needs 9 bytes, not 8
```

### 3.2 Safe helpers: `code/m05_safe_string_helpers.c`

```c
/* Copy with the strlcpy contract: always NUL-terminates when dstsz > 0 and returns strlen(src).
 * Truncation happened if and only if the return value >= dstsz. */
static size_t str_copy(char *dst, size_t dstsz, const char *src)
{
    size_t srclen = strlen(src);
    if (dstsz > 0) {
        size_t n = srclen < dstsz - 1 ? srclen : dstsz - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}

/* Fixed-width record fields are space-padded and NOT NUL-terminated.
 * Turn one into a C string, trimming trailing spaces. */
static size_t field_to_cstr(char *dst, size_t dstsz, const char *field, size_t width);

/* A string builder over a caller-supplied buffer. Once anything is cut off, `truncated` stays
 * true so the caller checks once at the end. */
__attribute__((format(printf, 2, 3)))           /* lets -Wformat check every call site */
static void sb_printf(strbuf *sb, const char *fmt, ...)
{
    if (sb->truncated) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->truncated = true;
        sb->len = sb->cap - 1;                  /* vsnprintf filled and terminated the rest */
    } else {
        sb->len += (size_t)n;
    }
}
```

```
str_copy into 8 bytes: "ACCOUNT", needed 15 -> truncated
str_append: "/srv/bank/ledger.dat" (20 chars, fits)
field_to_cstr: "LN0042" (6 chars)
strbuf: "acct=ACC-000042 amount=1234.05 " truncated=no
strbuf: "acct=ACC-000042 amount=1234.05 status=S" truncated=yes
```

`str_append` uses `strnlen`, which is POSIX (and C23), so the file defines
`_POSIX_C_SOURCE 200809L` before its first `#include`: with `-std=c17`, glibc hides everything that
is not ISO C unless you ask for it.

### 3.3 Parsing numbers: `code/m05_parse_numbers.c`

```c
static parse_status parse_i64(const char *s, int64_t *out)
{
    if (s == NULL || *s == '\0') return PARSE_EMPTY;
    char *end;
    errno = 0;                                   /* strtoll SETS errno on error, never clears it */
    long long v = strtoll(s, &end, 10);
    if (end == s) return PARSE_NOT_A_NUMBER;     /* not a single digit was consumed */
    if (errno == ERANGE) return PARSE_OUT_OF_RANGE;   /* v is LLONG_MAX or LLONG_MIN */
    while (isspace((unsigned char)*end)) ++end;  /* allow "42\n" from fgets */
    if (*end != '\0') return PARSE_TRAILING_GARBAGE;
    *out = (int64_t)v;
    return PARSE_OK;
}
```

```
atoi("12abc") = 12, atoi("abc") = 0, atoi("1O0") = 1   (letter O)
parse_i64("42"                     -> 42
parse_i64("  -17\n"                -> -17
parse_i64(""                       -> error: empty
parse_i64("abc"                    -> error: not a number
parse_i64("12abc"                  -> error: trailing garbage
parse_i64("99999999999999999999"   -> error: out of range
parse_i64("1O0"                    -> error: trailing garbage
strtoul("-1") = 18446744073709551615 with no error; parse_u32("-1") -> out of range
sscanf("12 abc", "%d %d") returned 1; b is still -1
sscanf("SBIN0001234", "%7s") -> "SBIN000" (bounded)
sscanf("250xyz", "%d%n") -> value 250, stopped at offset 3 ("xyz" left over)
```

### 3.4 Tokenising and moving bytes: `code/m05_tokenize_and_memmove.c`

```c
/* Nested strtok: the inner loop clobbers the ONE hidden position the outer loop relies on. */
for (char *item = strtok(s, ";"); item != NULL; item = strtok(NULL, ";")) {
    char *key = strtok(item, "=");
    char *val = strtok(NULL, "=");
    ...
}

/* A non-destructive splitter that KEEPS empty fields. */
for (;;) {
    size_t n = strcspn(s, set);             /* length up to the next delimiter or the end */
    cb(s, n, ctx);
    if (s[n] == '\0') break;
    s += n + 1;
}
```

```
nested strtok   found 1 pair(s)
nested strtok_r found 3 pair(s)
after strtok, the input buffer reads "a" (it was modified in place)
strtok on "ACC1,,250.00," sees 2 fields; split_keep_empty sees: [ACC1] [] [250.00] []  (4 fields)
after memmove insert at front: 10 20 30 40 50
```

`printf("%.*s", (int)n, f)` prints exactly `n` bytes of a field that is not terminated: the
standard way to print a slice of a buffer.

### 3.5 UTF-8: `code/m05_utf8_basics.c`

Non-ASCII text is written with `\x` escapes (`"\xE2\x82\xB9"` for the rupee sign) so the source file
is pure ASCII and compiles identically with GCC and MSVC (MSVC assumes the local code page for
source files unless you pass `/utf-8`).

```c
/* Copy at most dstsz - 1 bytes without cutting a multi-byte character in half. */
static size_t utf8_truncate_copy(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;   /* back off to a lead byte */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}
```

```
"₹100": strlen = 6 bytes, 4 code points
"José": 5 bytes, 4 code points
invalid (overlong '/') rejected: yes
invalid (truncated) rejected: yes
invalid (surrogate) rejected: yes
invalid (above U+10FFFF) rejected: yes
invalid (stray continuation) rejected: yes
truncate "₹100" to 3 bytes -> "₹" (3 bytes, the rupee sign kept whole)
truncate to 2 bytes -> "" (0 bytes: cutting inside the sign is not allowed)
isalpha((unsigned char)0xC3) in the "C" locale -> false
```

## 4. Under the hood (senior-level)

### How `strlen` is really implemented

A byte-at-a-time loop is far too slow for glibc. The classic word-at-a-time trick tests 8 bytes at
once: for a 64-bit word `x`, `(x - 0x0101010101010101) & ~x & 0x8080808080808080` is non-zero
exactly when some byte of `x` is zero. Modern glibc goes further with SSE2/AVX2 (compare 16 or 32
bytes against zero in one instruction, `pmovmskb` to get a bit mask, `tzcnt`/`bsf` to find the
first set bit). These versions read **aligned** blocks and may read a few bytes past the
terminator. That is safe because an aligned block never crosses a page boundary, so it can never
touch an unmapped page; but it is formally out of bounds, which is why Valgrind replaces the string
functions with its own versions instead of checking glibc's.

### How `memcpy` and `memmove` are implemented

glibc picks an implementation at load time for your CPU (through an IFUNC resolver). Small copies
use a couple of possibly overlapping unaligned loads and stores; medium ones use vector registers;
large ones use `rep movsb` on CPUs with fast string instructions; very large ones may use
non-temporal stores that bypass the cache. `memmove` is the same code plus a direction check. Since
the overlap check is a pair of comparisons, `memmove` costs almost nothing extra.

### Where string literals live

Literals go into `.rodata` and identical ones are usually merged (`-fmerge-constants`), so two
`"PAID"` literals in different functions may have the **same address**. That is why
`if (status == "PAID")` sometimes "works" in testing and fails when the string comes from a buffer.

### What catches overflows at run time, and how

- **Stack protector** (`-fstack-protector-strong`, on by default on Ubuntu): the compiler puts a
  random "canary" value between local buffers and the saved return address, and checks it before
  returning. Overflowing a local buffer far enough to reach the return address changes the canary:
  `*** stack smashing detected ***: terminated`, then `abort`.
- **FORTIFY_SOURCE**: with optimisation enabled, `strcpy`, `memcpy`, `sprintf` and friends are
  replaced by checking versions (`__strcpy_chk`) whenever the compiler knows the destination size
  (`__builtin_object_size`). Ubuntu 24.04's GCC defines `_FORTIFY_SOURCE=3` by default at `-O1` and
  above. Overflowing `char buf[8]` with `strcpy(buf, argv[1])` then aborts with
  `*** buffer overflow detected ***: terminated` before any damage. It cannot help when the size is
  unknown (a pointer into the heap passed through several functions). The course builds use `-g`
  without `-O`, so FORTIFY is off there; build with `-O2` to see it.
- **AddressSanitizer** finds overflows of any buffer, stack, heap or global, at the exact
  instruction: 05_Memory_Debugging_RCA/08_valgrind_and_sanitizers.md.

### What an overflow destroys

On the stack, the bytes past a local array are other locals, then the saved frame pointer and the
**return address**: a long string controls where the function returns to (the classic exploit).
On the heap, the bytes past a block are the **next chunk's header**; glibc notices the damage only
when that chunk is freed or merged, with messages like `free(): invalid next size (fast)`, often
long after and far from the overflowing code (05_Memory_Debugging_RCA/04_the_catalogue_of_memory_bugs.md).

## 5. In production

- **Unterminated strings in logs.** A field copied with `strncpy` (or legacy `_snprintf`) into an
  exactly-sized buffer has no terminator, and the next `printf("%s")` prints it plus whatever
  follows in memory. Symptoms: log lines with garbage tails, or worse, response messages that leak
  another customer's data from an adjacent buffer. The fix is a helper that always terminates.
- **`strtok` in a multithreaded request parser.** Works in testing, corrupts fields under load when
  two worker threads parse at once. Grep the code base for `strtok(` and replace with `strtok_s`
  (Windows) or `strtok_r` (POSIX).
- **`atoi` on amounts and account numbers.** An empty field becomes 0, `"1,250.00"` becomes 1,
  `"0123"` parsed with base 0 becomes 83. Each produces a valid-looking but wrong transaction.
  Every numeric field from a file, a socket or a database string goes through a checked parser
  like `parse_i64`, and a parse failure is a rejected record, never a zero.
- **Silent truncation.** A narration, address or reference field longer than the buffer gets cut,
  and two different references now look the same. Use the `snprintf` return value; decide per field
  whether truncation is an error.
- **Encoding errors during migration.** Names in a Windows-1252 code page sent to a UTF-8 PostgreSQL
  or YugabyteDB fail with errors such as `invalid byte sequence for encoding "UTF8": 0xe9`, or worse,
  get inserted mis-converted. Validate UTF-8 (section 3.5) and convert code pages explicitly at the
  boundary; count invalid records instead of crashing on them.
- **Finding these in an old code base.** Grep for `strcpy`, `strcat`, `sprintf`, `strncpy`,
  `_snprintf`, `strtok(`, `atoi`, `gets`, `scanf("%s"`. Many teams keep a "banned functions"
  header that turns these into compile errors (Microsoft's `banned.h`, and Git's `banned.h`, do
  exactly this). Build tests with ASan and fuzz every parser, as in
  05_Memory_Debugging_RCA/13_defensive_design_to_prevent_the_bugs.md.

## 6. Try it

1. **(easy)** For `char d[6]`, predict the 6 bytes after `strncpy(d, s, sizeof d)` for `s` =
   `""`, `"abc"`, `"abcde"`, `"abcdef"`, `"abcdefgh"`. Done when all five predictions match a
   program that dumps the bytes, and you can say which ones are terminated.
2. **(easy)** Rewrite `sprintf(buf, "%s-%06d-%s", branch, seq, suffix)` into a function that uses
   `snprintf`, returns an error on truncation, and never leaves `buf` unterminated. Done when a test
   with a 10-byte buffer reports truncation and a 64-byte buffer succeeds.
3. **(medium)** Write `int parse_amount_paise(const char *s, int64_t *out)` for inputs like
   `"1234.50"`, `"-0.75"`, `"12"`, using **no floating point**. Reject more than two decimals,
   empty input, stray characters, and anything that overflows `int64_t`. Done when a table of at
   least 15 cases (including `"."`, `"1."`, `"-"`, `"9223372036854775807"`) passes.
4. **(medium)** Write a non-destructive CSV splitter that handles quoted fields containing commas
   and doubled quotes (`a,"b,c","d""e"` gives `a`, `b,c`, `d"e`). Done when it passes 10 cases and
   never writes into the input.
5. **(medium)** Write `int utf8_next(const char **s, uint32_t *cp)` that decodes one code point and
   advances, returning an error for every invalid case in section 2.9. Done when it agrees with
   `utf8_valid` on 100,000 random byte strings.
6. **(hard)** Write a program with a `char buf[16]` overflowed by `strcpy` from `argv[1]`. Run it
   built with `-O2` (FORTIFY on), with `-O0 -fstack-protector-strong`, and with `-fsanitize=address`.
   Done when you have all three reports and can explain why FORTIFY stops a `strcpy` into a stack
   array but not into a heap buffer whose size the compiler cannot see.

## 7. Interview questions

**Q: Why is `strncpy` not a safe `strcpy`?**
It does not write a terminator when the source is at least `n` bytes long, so the result may not be
a string at all, and it zero-pads the rest of the buffer when the source is shorter, which wastes
time. It was designed for fixed-width NUL-padded fields. Use `snprintf(d, sizeof d, "%s", s)` or a
`strlcpy`-style function that always terminates and reports truncation.

**Q: How do you detect truncation with `snprintf`?**
It returns the length it would have written given unlimited space. If the return value is negative
or greater than or equal to the buffer size, the output was truncated (it is still terminated when
the size is non-zero). Old MSVC `_snprintf` is different: no terminator on truncation and a return
of -1.

**Q: What is the difference between `memcpy` and `memmove`?**
`memcpy` requires that source and destination do not overlap (its parameters are `restrict`);
overlapping use is undefined and has broken real programs when glibc changed its copy direction.
`memmove` handles overlap correctly, typically by choosing the copy direction. Shifting elements
within one array needs `memmove`.

**Q: Why is `atoi` dangerous, and how do you use `strtol` correctly?**
`atoi` cannot report errors: a non-number returns 0 and overflow is undefined. With `strtol`, set
`errno = 0` first, pass an explicit base, check that `end != s` (some digits), that `errno` is not
`ERANGE`, that `*end` is the terminator (no trailing garbage), and that the value fits the target
type. Reject a minus sign yourself for the unsigned variants.

**Q: What is wrong with `strtok`?**
It modifies its input, merges consecutive delimiters so empty fields disappear, and keeps its
position in a single static variable, which breaks nested tokenising and is a data race between
threads. Use `strtok_r` (POSIX) or `strtok_s` (MSVC, three arguments), or a non-destructive
`strcspn` loop.

**Q: What does `strlen` return for a UTF-8 string, and why does it matter?**
The number of bytes before the NUL, not characters; the rupee sign alone is 3 bytes. Buffer sizes
and database column limits must be agreed in bytes or code points, and truncation must not split a
multi-byte sequence, or the result is invalid UTF-8.

**Q: What happens when you write past the end of a stack buffer? A heap buffer?**
On the stack you overwrite neighbouring locals, the saved frame pointer and the return address; the
stack protector detects canary damage at function return and aborts. On the heap you overwrite the
next chunk's metadata or another object; glibc detects it only when that chunk is freed or merged,
often much later, which is why heap overflows are found with ASan or Valgrind rather than from the
crash site.

**Q: Write a `strlcpy`-style function.**
Compute `srclen = strlen(src)`; if `dstsz > 0`, copy `min(srclen, dstsz - 1)` bytes with `memcpy`
and write a NUL after them; return `srclen`. The caller detects truncation with
`return value >= dstsz`.

## 8. Recap

- A C string is bytes up to the first NUL; length (strlen) and capacity (sizeof) are different, and
  length n needs n + 1 bytes.
- `strcpy`, `strcat`, `sprintf`, `gets` have no bounds; `strncpy` may not terminate; `strncat`'s n
  is an append limit.
- `snprintf` always terminates and returns the needed length: truncated iff `n < 0 || n >= size`.
- `memcpy` for non-overlapping buffers, `memmove` when they might overlap.
- Never `atoi`. `strtol` with `errno = 0`, explicit base, `end` checks, range check;
  `strtoul` accepts "-1".
- `sscanf`: check the return value, always give `%s` a width, use `%n` for trailing garbage.
- `strtok` is destructive, merges delimiters and is not thread-safe: use `strtok_r` / `strtok_s`.
- UTF-8: `strlen` counts bytes; truncate on code point boundaries; cast to `unsigned char` for
  `<ctype.h>`.
- Write helpers that take a size, always terminate, and report truncation.
- Stack protector, FORTIFY_SOURCE and ASan catch overflows at run time; heap overflows surface late.
