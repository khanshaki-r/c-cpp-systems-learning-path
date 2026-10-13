# 16. Modern C and the C interview playbook

**Time:** ~3 hours (plus practice: write every coding answer twice on paper)   **Prerequisites:** modules 01 to 15

## 1. In plain words

A C interview tests three things. First, **precise knowledge of the language**: what `static`
means in each place, why `volatile` is not for threads, what `sizeof` gives for an array
parameter. Second, **writing small correct functions without a library**: `strlen`, `memmove`,
`atoi`, reversing a list. Third, **care**: do you mention the empty string, the NULL pointer, the
overflow, the overlap, before the interviewer has to ask?

Think of a driving test. Everyone can drive in a straight line. The examiner is watching whether
you check the mirrors at every turn. In a C interview the mirrors are the edge cases and the
undefined behaviour, and saying them out loud is how the interviewer knows you checked.

This module is the reference sheet for that day: what each C standard added (including C23), the C
versus C++ differences interviewers like, the classic conceptual questions with model answers, the
classic coding questions with complete tested solutions in `code/`, and how C rounds are run at
embedded, systems and product companies.

## 2. The real thing

### 2.1 The C standards in one table

| Standard | Year | What it added that you are expected to know |
|----------|------|-----------------------------------------------|
| K&R C | 1978 | the original language of the book |
| C89 / C90 | 1989 / 1990 | ANSI then ISO standard: prototypes, `void *`, `const`, `volatile`, the standard library |
| **C99** | 1999 | `//` comments; declarations anywhere and in `for`; `long long`; `<stdint.h>`, `<inttypes.h>`, `<stdbool.h>`; `inline`; `restrict`; variable length arrays; designated initialisers; compound literals; flexible array members; `snprintf`; `__func__`; variadic macros; `static` in array parameters; integer division defined to truncate toward zero |
| **C11** | 2011 | `_Static_assert`; `_Generic`; `_Alignas` / `_Alignof`; `_Noreturn`; `_Thread_local`; `<threads.h>`; `<stdatomic.h>` and a memory model; anonymous structs and unions; `aligned_alloc`; `quick_exit`; `gets` removed; VLAs made optional (`__STDC_NO_VLA__`); optional Annex K (`_s` functions) |
| **C17** (also called C18) | 2018 | defect fixes only, no new features; `__STDC_VERSION__` is `201710L` |
| **C23** | ISO/IEC 9899:2024 | `bool`, `true`, `false`, `nullptr`, `static_assert`, `alignas`, `thread_local` as keywords; `constexpr` objects; `typeof`; `auto` type inference; `[[nodiscard]]`, `[[maybe_unused]]`, `[[fallthrough]]`, `[[deprecated]]`; digit separators `1'000'000`; binary literals `0b1010`; `= {}` empty initialiser; `#embed`; `#elifdef`; `<stdckdint.h>` (`ckd_add`, `ckd_mul`); `memset_explicit`; `strdup` standardised; `_BitInt(N)`; two's complement required; `void f()` now means "no parameters"; old K&R function definitions removed |

GCC 13 implements most of C23 under `-std=c2x`. This snippet uses several C23 features; it
compiles cleanly with `gcc -std=c2x -Wall -Wextra -Wpedantic` on GCC 13.3 and prints
`1000 10 1 1 3.5 1`:

```c
#include <stdio.h>
[[nodiscard]] static int f(void) { return 1; }
int main(void) {
    constexpr int N = 1'000;          /* constexpr object, digit separator */
    typeof(N) m = 0b1010;             /* typeof, binary literal */
    int *p = nullptr;                 /* nullptr */
    bool ok = true;                   /* bool is a keyword, no header */
    auto x = 3.5;                     /* type inferred: double */
    static_assert(sizeof(int) == 4);  /* message now optional */
    int arr[4] = {};                  /* empty initialiser */
    printf("%d %d %d %d %g %d\n", N, m, p == nullptr, ok, x, arr[0] + f());
    return 0;
}
```

GCC 13 does **not** have `<stdckdint.h>` (it arrived in GCC 14; `__builtin_add_overflow` is the
GCC 13 equivalent) or `#embed` (GCC 15). Most production C today is C99 or C11; knowing what C23
changes is a plus, assuming it in an interview answer is not.

### 2.2 C versus C++: the differences interviewers ask about

| Topic | C | C++ |
|-------|---|-----|
| `void *` conversion | implicit: `int *p = malloc(n);` is correct and idiomatic | needs a cast; g++ says `error: invalid conversion from 'void*' to 'int*' [-fpermissive]` |
| Struct tags | `struct point p;` unless you `typedef` it | `point p;` works: the tag is a type name |
| `const int N = 10;` | a read-only object, **not** a constant expression: at file scope `int a[N];` is an error, at block scope it makes a VLA | a constant expression: `int a[N];` is a normal array |
| `const` at file scope | external linkage | internal linkage |
| `bool` | `_Bool` plus `<stdbool.h>` macros (keywords since C23) | keyword |
| `sizeof('a')` | `sizeof(int)`: a character literal is an `int` | 1: it is a `char` |
| VLAs | C99; optional since C11 | not in the standard (a GCC extension) |
| Designated initialisers | any order, arrays (`[3] = 7`), nested | C++20, declaration order only, no array designators |
| Compound literals | yes | no (extension) |
| `void f()` | before C23: unspecified parameters; C23: no parameters | no parameters |
| `restrict` | yes | no (compilers offer `__restrict`) |
| Union type punning | defined: the bytes are reinterpreted | undefined behaviour; use `memcpy` or `std::bit_cast` |
| `int x; int x;` at file scope | legal (tentative definitions) | error: redefinition |
| `goto` over an initialisation | allowed (except into a VLA's scope) | error |
| `inline` | C99 rules: an `inline` definition without one `extern` declaration in some `.c` file gives `undefined reference` at `-O0` | ODR rules: just works |
| Names in object files | `kv_close` | mangled: `_Z8kv_closeP2kv` |

**Name mangling and `extern "C"`.** C++ encodes parameter types into symbol names so that
overloads can coexist. Real `nm` output for the same function compiled as C++, with and without
`extern "C"`:

```text
0000000000000000 T _Z8kv_closeP2kv      <- int kv_close(struct kv *)   (C++ linkage)
0000000000000019 T kv_close_c           <- extern "C" int kv_close_c(struct kv *)
```

`_Z` starts a mangled name, `8kv_close` is the name with its length, `P2kv` is "pointer to the type
named `kv`". A C library header used from C++ must therefore wrap its declarations, or the C++
caller looks for `_Z8kv_closeP2kv` and the link fails with `undefined reference`:

```c
#ifdef __cplusplus
extern "C" {
#endif
int  kv_open(kv_t **out, size_t capacity);
void kv_close(kv_t *kv);
#ifdef __cplusplus
}
#endif
```

**The C99 `inline` trap**, real GCC 13 output for `inline int twice(int x) { return 2 * x; }`
called from `main` in one file at `-O0`:

```text
/usr/bin/ld: /tmp/ccwj7xer.o: in function `main':
inl.c:(.text+0xe): undefined reference to `twice'
```

At `-O2` the call is inlined and it links, so the bug hides in release builds. In C, a plain
`inline` definition is only an inline definition; one translation unit must also provide the
external definition (`extern inline int twice(int);`). The everyday fix: `static inline` in headers.

### 2.3 Classic conceptual questions

**`static` in three contexts.**
1. Inside a function: the variable has **static storage duration**: it is initialised once,
   before `main`, and keeps its value between calls (not thread-safe without a lock or atomic).
2. At file scope, on a function or variable: **internal linkage**: the name is invisible to other
   `.c` files, so two files can each have a private `helper()` without a link clash.
3. In an array parameter (C99): `void f(int a[static 4])` promises the caller passes at least 4
   valid elements, so the compiler may optimise and can warn on violations.

**`const int *p`, `int *const p`, `const int *const p`.** Read right to left from the name:
`p` is a pointer to a const int (you cannot write `*p`, you can change `p`); `p` is a const pointer
to an int (you can write `*p`, not `p`); both const. `int const *p` means the same as
`const int *p`. A `const` on the pointer itself is top-level and is dropped when the value is
copied, which is why `_Generic` sees `int *const cpi` as `int *` (`m16_modern_c_features.c` checks it).

**`volatile`, and why it is not for threads.** `volatile` tells the compiler that every read and
write of the object is a visible side effect: it must not cache the value in a register, merge or
remove accesses. That is what memory-mapped hardware registers need, and what a
`volatile sig_atomic_t` flag set by a signal handler needs (the only object type a handler may
portably write). It does **not** make a read-modify-write atomic, it does not order other memory
accesses around it, and a concurrent non-atomic access from two threads is still a data race,
which is undefined behaviour. Use `_Atomic` or a mutex (module 14). (MSVC historically gave
`volatile` acquire/release semantics on x86 as an extension, `/volatile:ms`; code relying on it
breaks on other compilers and on ARM.)

**`sizeof` of an array parameter.** An array parameter is adjusted to a pointer, so
`void f(int a[10])` is `void f(int *a)` and `sizeof a` is 8 on a 64-bit system. GCC warns:

```text
warning: 'sizeof' on array function parameter 'a' will return size of 'int *' [-Wsizeof-array-argument]
```

Pass the length separately (`f(int *a, size_t n)`), or pass a pointer to the array
(`void f(int (*a)[10])`, where `sizeof *a` is 40).

**Array versus pointer.** `char a[] = "hi";` is a 3-byte array you may modify; `sizeof a` is 3.
`char *p = "hi";` is a pointer to a string literal, which you must not modify (it is usually in
read-only memory); `sizeof p` is 8. An array name decays to a pointer to its first element in most
expressions, except as the operand of `sizeof`, `&`, or as a string literal initialising an array.

**More you should answer in two sentences each:** `i++ + ++i` (unsequenced modifications: UB);
`-1 < 1u` is **false** (the `-1` is converted to `unsigned int`, a huge value); `unsigned char`
arithmetic is done in `int` (integer promotion); `malloc` versus `calloc` (zeroed, and checks the
`n * size` overflow) versus `realloc` (may move; keep the old pointer until it succeeds);
`free(NULL)` is a no-op; macros versus `static inline` functions (`MAX(a++, b)` evaluates `a++`
twice); why `do { ... } while (0)` in macros (one statement, safe after `if` without braces);
header guards versus `#pragma once`; `extern` declarations versus definitions; what runs before
`main` (section 4).

### 2.4 Classic coding questions: approach and edge cases

All of these are implemented and tested in `code/m16_string_questions.c` and
`code/m16_classic_questions.c`. For each, say the edge cases **before** you write code.

| Question | Approach | Edge cases to say out loud |
|----------|----------|----------------------------|
| `strlen` | walk to the NUL, return the pointer difference | empty string; NULL is the caller's bug (the standard function does not check) |
| `strcpy` | copy including the NUL; return `dst` | overlap is UB; caller must size `dst`; prefer a bounded copy |
| `strncpy` trap | does **not** NUL-terminate when the source is too long, and pads with zeros | why `strlcpy` (or `snprintf(dst, n, "%s", src)`) is the safe answer |
| `strcmp` | compare as `unsigned char` until a difference or NUL | bytes above 127; return sign only |
| `memcpy` | byte loop; `restrict` says no overlap | overlap is UB; `n == 0` |
| `memmove` | copy forwards if `dst < src`, backwards if `dst > src` | both overlap directions; equal pointers; comparing unrelated pointers |
| `atoi` with overflow | skip spaces, sign, digits; check before `acc * 10 + d` | `INT_MIN` (magnitude one more than `INT_MAX`), `"+"`, `"-"`, `""`, trailing junk |
| Reverse a string in place | two indices moving inward | empty, one character, even and odd lengths |
| Reverse a linked list | three pointers: prev, cur, next | empty list, one node; recursive version uses O(n) stack |
| Detect a loop | Floyd: slow moves 1, fast moves 2; they meet inside the loop | loop at the head, self-loop, no loop |
| Find where the loop starts | after meeting, move one pointer to the head; step both by 1 | whole list is the loop |
| Endianness | look at the first byte of `uint32_t 1` through `unsigned char *` or a union | say "union punning is legal in C, not in C++" |
| Count set bits | `v &= v - 1` clears the lowest set bit; loop count = bits | `0`, all ones; `__builtin_popcount` |
| Power of two | `x != 0 && (x & (x - 1)) == 0` | zero is not a power of two |
| Stack | array + length + capacity, grow by doubling | pop on empty returns an error, never UB |
| Tokenizer | `strspn` to skip delimiters, `strcspn` to find the end | empty fields (a CSV needs them; `strtok` drops them), state in the caller |
| Rotate an array right by k | reverse all, reverse first k, reverse the rest | `k >= n` (use `k % n`), `n == 0` |

Four of them in full, because they are asked most:

```c
/* memmove: overlap allowed. If dst is after src, copy backwards so we never read a byte we
 * already overwrote. Comparing pointers into DIFFERENT objects with < is undefined in C, so
 * compare their integer values (implementation-defined, correct on flat-memory platforms). */
void *my_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d < (uintptr_t)s) {
        while (n--)
            *d++ = *s++;
    } else if ((uintptr_t)d > (uintptr_t)s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

/* atoi with overflow detection: accumulate in unsigned so INT_MIN's magnitude fits. */
int my_atoi(const char *s, int *out, const char **end)
{
    while (isspace((unsigned char)*s))       /* cast: isspace(negative char) is UB */
        s++;
    int neg = 0;
    if (*s == '+' || *s == '-')
        neg = (*s++ == '-');
    if (!isdigit((unsigned char)*s)) {
        if (end) *end = s;
        return ATOI_NO_DIGITS;
    }
    unsigned limit = neg ? (unsigned)INT_MAX + 1u : (unsigned)INT_MAX;
    unsigned acc = 0;
    for (; isdigit((unsigned char)*s); s++) {
        unsigned d = (unsigned)(*s - '0');
        if (acc > (limit - d) / 10) {         /* acc * 10 + d would exceed limit */
            *out = neg ? INT_MIN : INT_MAX;   /* clamp, like strtol */
            while (isdigit((unsigned char)*s))
                s++;
            if (end) *end = s;
            return ATOI_OVERFLOW;
        }
        acc = acc * 10 + d;
    }
    if (neg)
        *out = acc == (unsigned)INT_MAX + 1u ? INT_MIN : -(int)acc;
    else
        *out = (int)acc;
    if (end) *end = s;
    return ATOI_OK;
}

/* Reverse a singly linked list: each step turns one arrow around. O(n) time, O(1) space. */
static struct node *reverse_iter(struct node *head)
{
    struct node *prev = NULL;
    while (head) {
        struct node *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

/* Floyd's cycle detection, plus the start of the loop. */
static const struct node *loop_start(const struct node *head)
{
    const struct node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            for (slow = head; slow != fast; slow = slow->next, fast = fast->next)
                ;
            return slow;
        }
    }
    return NULL;
}
```

Why the loop start works: let the distance from the head to the loop start be `a`, and the
meeting point be `b` steps into a loop of length `L`. Fast travelled twice as far as slow:
`2(a + b) = a + b + kL`, so `a = kL - b`. Walking `a` steps from the head and `a` steps from the
meeting point therefore lands both pointers on the loop start.

The overflow check in `my_atoi` deserves one sentence in the interview: "I check before
multiplying, because once `acc * 10 + d` has overflowed it is too late to detect it", which links
straight back to module 09.

### 2.5 How C interview rounds run

The format varies by company type more than by country. Typical patterns:

- **Embedded and firmware** (automotive, semiconductors, devices). A written or online test with
  "what does this print" snippets (operator precedence, integer promotion, `sizeof`, pointer
  arithmetic, `static`), bit manipulation (set, clear, toggle a bit; count bits; swap nibbles),
  `volatile` and interrupt service routines (keep them short, no `malloc`, no `printf`), memory-mapped
  registers, struct packing and alignment, endianness, fixed-point arithmetic, and RTOS questions
  (mutex versus semaphore, priority inversion). Coding without dynamic memory is common.
- **Systems** (operating systems, networking, storage, databases, infrastructure). Pointers and
  memory layout, writing an allocator or a hash table in C, concurrency (a bounded queue, a race to
  spot), system calls (`fork`/`exec`, `read`/`write` loops, sockets, `mmap`), and a debugging
  discussion: "this server crashes once a day; here is the stack; what do you do?" Your RCA and
  crash-dump experience is the strongest card here (05_Memory_Debugging_RCA,
  11_Interview_Prep_Job_Search/06_debugging_and_rca_round.md).
- **Product companies hiring for C, C++ backend roles.** One or two data-structures rounds where
  you usually may choose C++ (use it: the STL saves time; 03_Data_Structures_Algorithms/16_interview_coding_playbook.md),
  a language round that mixes C and C++ (manual memory versus RAII, `extern "C"`, undefined
  behaviour), and a design round (11_Interview_Prep_Job_Search/01_how_cpp_systems_interviews_work.md).

**How to answer a C coding question, every time:**

1. Repeat the problem and ask about the contract: can the input be NULL? Can buffers overlap?
   What should happen on overflow: clamp, error, or is it the caller's problem?
2. List the edge cases out loud (table in 2.4).
3. Write the code, small and plain: no clever tricks you cannot explain.
4. Walk through it by hand with one normal case and two edge cases.
5. State complexity, and name any undefined behaviour you avoided ("I compare as `unsigned char`",
   "I check before multiplying").

## 3. Code

### 3.1 `m16_string_questions.c`

`my_strlen`, `my_strcmp`, `my_strcpy`, `my_strlcpy`, `my_memcpy`, `my_memmove`, `my_atoi`,
`reverse_in_place`, `my_strtok_r` and a non-destructive `next_field`, each tested against the C
library or a table. The tests include both overlap directions for `memmove`, every `atoi` boundary
(`"2147483647"`, `"2147483648"`, `"-2147483648"`, `"-2147483649"`, `"-"`, `""`, `"   -17xyz"`),
and a CSV line where `strtok` loses the empty fields that `next_field` keeps:

```c
const char *csv = "TXN,,ACC-1001,";
const char *cur = csv, *f;
size_t flen, nf = 0;
size_t lens[] = { 3, 0, 8, 0 };
while (next_field(&cur, ',', &f, &flen))
    assert(nf < 4 && flen == lens[nf++]);
assert(nf == 4);
```

It also runs clean under ASan and UBSan:
`gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -o m16_strings_asan m16_string_questions.c`.

### 3.2 `m16_classic_questions.c`

Lists (iterative and recursive reverse, Floyd, loop start, middle), bits (endianness, byte swap,
three popcounts cross-checked on 100 000 values, power of two), a growable stack with a
balanced-brackets checker, rotation checked against a naive version for every `n` up to 7 and every
`k` up to `2n + 1`, the XOR swap and its aliasing trap, and `sizeof` through a pointer to an array.
Real output:

```text
endianness: little; popcount(0x9E3779B9) = 20
all checks passed
```

The XOR swap is in the file to show why it is a bad answer: `xor_swap(&a, &a)` sets `a` to 0
unless you guard against equal pointers, and it is slower than a temporary on modern CPUs.

### 3.3 `m16_modern_c_features.c`

C99 and C11 features with checks: designated initialisers (struct and array), compound literals,
a flexible array member, `_Static_assert` (including `sizeof('a') == sizeof(int)`), `_Generic`,
`bool` conversion, `PRId64`, `alignas`, VLAs, integer division, `static` in all three contexts,
`const` pointer forms, `volatile sig_atomic_t` set by a real signal handler, and `_Noreturn`.
Real output:

```text
__STDC_VERSION__ = 201710L, TYPE_NAME(cpi) = int *, __func__ = main
all checks passed
```

```c
#define TYPE_NAME(x) _Generic((x),                                              \
    int: "int", unsigned: "unsigned", long: "long", double: "double",           \
    char *: "char *", const char *: "const char *", int *: "int *",              \
    const int *: "const int *", default: "other")

char c1 = 'a', c2 = 'b';
assert(strcmp(TYPE_NAME(c1 + c2), "int") == 0);     /* integer promotion, made visible */
```

## 4. Under the hood (senior-level)

**What happens before and after `main`.** The kernel starts the program at `_start` (from
`crt1.o`), which calls glibc's `__libc_start_main`. That sets up the C library (TLS, stdio,
`atexit` machinery), runs the functions in `.init_array` (constructors: GCC
`__attribute__((constructor))`, and C++ static initialisers), then calls `main(argc, argv, envp)`.
When `main` returns, its value goes to `exit`, which runs `atexit` handlers and `.fini_array`
destructors in reverse order, flushes and closes stdio streams, and calls `_exit`. A crash, `abort`
or `_exit` skips all of that, which is why buffered output is lost (module 13).

**How glibc's `memcpy` and `strlen` are really written.** They are not the byte loops above. glibc
selects an implementation at load time through an **IFUNC** resolver based on the CPU (SSE2, AVX2,
AVX-512, `rep movsb` with ERMS). Small copies use overlapping unaligned loads and stores instead of
loops. `strlen` reads a whole word or vector at a time and detects a zero byte with a bit trick: for
32-bit words, `(x - 0x01010101) & ~x & 0x80808080` is non-zero exactly when some byte of `x` is
zero. Reading a full aligned word may read past the terminator, which would be undefined in C but is
safe in hand-written assembly because an aligned word never crosses a page boundary. Mentioning
that in an interview, after writing the simple loop, is a good senior signal.

**The 2010 `memcpy` story.** In 2010 glibc changed its x86-64 `memcpy` so that, on some CPUs, it
copied backwards. Programs that had (incorrectly) used `memcpy` on overlapping buffers started
producing corrupt output; Adobe's Flash plugin on Fedora was the famous case. The programs had
always had undefined behaviour; a faster library exposed it. It is the clearest real example of
why "memcpy for non-overlapping, memmove for overlapping" is a rule, not a style preference.

**`popcount` in hardware.** With `-mpopcnt` or `-march=native`, `__builtin_popcount` becomes one
`POPCNT` instruction; without it GCC emits a short bit-twiddling sequence or a library call. Many
compilers also recognise the Kernighan loop and replace it with `POPCNT`. Interviewers like hearing
both the loop and the builtin.

**Reading C++ mangled names.** `c++filt _Z8kv_closeP2kv` prints `kv_close(kv*)`. When a mixed C and
C++ build fails with `undefined reference to 'kv_close(kv*)'`, the demangled signature in the error
tells you at once that a C function was declared without `extern "C"`.

## 5. In production

- **`strncpy` in old code.** It was designed for fixed-width fields in old Unix directory entries,
  not as a "safe `strcpy`". Production code that uses it to copy into a buffer and then prints the
  buffer has a missing-terminator bug waiting for a long enough input. When you see
  `strncpy(dst, src, sizeof dst)` without `dst[sizeof dst - 1] = '\0'`, you have found a bug.
- **`atoi` on input.** `atoi("12abc")` is 12 and `atoi("abc")` is 0, with no error. In a banking
  message parser that turns a corrupted amount into a valid one. Use `strtol` with the checks from
  module 10 or a parser like `my_atoi` that reports errors.
- **Mixed C and C++ builds.** When a C++ service links a C library (Berkeley DB, libpq, OpenSSL),
  missing `extern "C"` produces link errors, and mismatched struct definitions between the C and C++
  sides produce memory corruption. Include the library's own header; never re-declare its structs.
- **Interview answers become code review comments.** Every edge case in the table in 2.4 is a real
  review comment you will write on a colleague's patch: "this compares `char` as signed", "this
  memcpy overlaps", "this atoi accepts garbage". Practising the questions is practising the job.

## 6. Try it

1. **Easy - five from memory.** On paper, with no references, write `strlen`, `strcmp`, `memmove`,
   `reverse_iter` and `is_power_of_two`. Type them in, add them to a copy of the test files, and
   run. Done when all tests pass on the first compile, or you know exactly which edge case you
   missed.
2. **Easy - what does this print?** Predict, then run: `printf("%zu %zu\n", sizeof("abc"),
   strlen("abc"));`, `printf("%d\n", -1 < 1u);`, `unsigned char c = 200; printf("%d\n", c + c);`,
   `int a[5]; printf("%td\n", &a[4] - &a[0]);`. Done when every prediction matches and you can give
   the rule behind each.
3. **Medium - strtod-free parser.** Write `parse_amount("1234.56", &paise)` that accepts an optional
   sign, digits, and an optional point with exactly two digits, and rejects everything else
   (including overflow of `long long`). Done when a table test of 20 inputs passes under UBSan.
4. **Medium - C23 tour.** Rewrite `m16_modern_c_features.c` for `-std=c2x`: `bool` without the
   header, `nullptr`, `constexpr`, `typeof`, `[[nodiscard]]` on a function whose result you then
   ignore (read the warning), `static_assert` without a message. Done when it builds with GCC 13
   `-std=c2x -Wall -Wextra -Wpedantic` and you have listed which C23 features GCC 13 lacks.
5. **Hard - word-at-a-time `strlen`.** Implement `strlen` that first walks byte by byte to an
   8-byte boundary, then reads `uint64_t` words with `memcpy` and uses the zero-byte bit trick.
   Done when it matches `strlen` on every length from 0 to 100 at every starting alignment 0 to 7,
   and you can explain why the C version must not read past the buffer while glibc's assembly may.
6. **Hard - mock interview.** Have someone pick five questions from section 2.3 and three coding
   questions from 2.4, 45 minutes in total, coding in a plain editor without compiling. Done when
   you have done it twice and written down every edge case you forgot to say out loud.

## 7. Interview questions

**Q: What does `static` mean in C?**
A: Inside a function it gives a local variable static storage duration: initialised once and kept
between calls. At file scope it gives a function or variable internal linkage, hiding it from other
translation units. In an array parameter, `int a[static 4]` promises that the argument points to at
least four elements.

**Q: Why is `volatile` not suitable for sharing data between threads?**
A: It only stops the compiler from caching or removing accesses; it provides no atomicity for
read-modify-write operations and no ordering relative to other memory, and a concurrent
non-atomic access is still a data race, which is undefined. Use C11 atomics or a mutex. `volatile`
is for memory-mapped I/O and `volatile sig_atomic_t` signal-handler flags.

**Q: What is the difference between `const int *p` and `int *const p`?**
A: In `const int *p` the pointed-to int is read-only through `p` but `p` can be re-pointed. In
`int *const p` the pointer itself is fixed but the int can be modified through it. `const int *const p`
fixes both. Reading the declaration from the name outward, right to left, gives the meaning.

**Q: What does `sizeof` return for an array parameter, and why?**
A: The size of a pointer, because a parameter declared as an array is adjusted to a pointer to its
element type; the array's length is not passed. GCC warns with `-Wsizeof-array-argument`. Pass the
length as a separate argument or pass a pointer to an array of known size.

**Q: Write `memmove`. How is it different from `memcpy`?**
A: `memmove` must work when source and destination overlap, so it copies forwards when the
destination is before the source and backwards when it is after, so it never reads a byte it has
already overwritten. `memcpy` may assume no overlap (its parameters are `restrict`), which allows
faster copying; using it on overlapping buffers is undefined behaviour.

**Q: How do you detect overflow in `atoi`?**
A: Accumulate the magnitude in an unsigned type and, before each `acc * 10 + digit`, check
`acc > (limit - digit) / 10`, where the limit is `INT_MAX` for positive numbers and `INT_MAX + 1`
for negative ones so that `INT_MIN` parses. Checking after the multiplication is too late, since
signed overflow is undefined. Report where parsing stopped so trailing junk can be rejected.

**Q: Why does C++ need `extern "C"` to call C functions?**
A: C++ mangles function names to encode parameter types, so `int kv_close(struct kv *)` becomes
`_Z8kv_closeP2kv`, while the C library exports plain `kv_close`. `extern "C"` tells the C++
compiler to use C linkage and the unmangled name. C headers wrap their declarations in
`#ifdef __cplusplus extern "C" { ... }`.

**Q: Name three differences between C and C++ that can change the meaning of the same code.**
A: `sizeof('a')` is `sizeof(int)` in C and 1 in C++; reading a different union member than the one
last written is defined in C and undefined in C++; and `void f()` declares a function with
unspecified parameters in C before C23 but no parameters in C++. Also, `const int n = 10;` is a
constant expression in C++ but not in C, where `int a[n]` inside a function creates a VLA.

## 8. Recap

- C99 gave most of modern C (stdint, bool, inline, restrict, VLAs, designated initialisers,
  compound literals, flexible arrays); C11 added `_Generic`, `_Static_assert`, alignment, threads
  and atomics; C17 fixed defects; C23 adds `nullptr`, `constexpr`, `typeof`, attributes,
  `ckd_add`, `#embed` and makes `bool` a keyword.
- GCC 13 supports most of C23 with `-std=c2x`, but not `<stdckdint.h>` (GCC 14) or `#embed` (GCC 15).
- C versus C++: implicit `void *` conversion, struct tags, `const` and constant expressions,
  `sizeof('a')`, union punning, `void f()`, VLAs, designated initialisers, C99 `inline`.
- C++ mangles names (`_Z8kv_closeP2kv`); C headers need `extern "C"` guards.
- `static`: storage duration inside a function, internal linkage at file scope, minimum size in
  array parameters.
- `volatile` is for hardware registers and signal flags, never for thread synchronisation.
- An array parameter is a pointer; `sizeof` gives 8; pass the length.
- Coding answers: state the contract and edge cases first, write plain code, trace two edge cases
  by hand, state complexity and the UB you avoided.
- `memmove` copies backwards when the destination is after the source; `atoi` checks before
  multiplying and parses `INT_MIN`; Floyd finds loops and their start.
- Embedded rounds stress bits, `volatile` and no-malloc code; systems rounds stress memory,
  concurrency, syscalls and debugging; product rounds mix DSA (use C++) with C and C++ language
  questions.
