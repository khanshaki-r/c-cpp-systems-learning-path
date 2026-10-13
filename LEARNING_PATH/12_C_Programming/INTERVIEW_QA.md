# 12_C_Programming — interview questions and model answers

40 questions, ordered from basic to senior. Each answer is what you would say out loud in 30 to 60
seconds. The module that covers the topic in depth is given in brackets. Five questions (6, 9, 14,
24 and 33) are "what does this print / what is wrong with this code" snippets of the kind used in
written tests and language rounds; their outputs were checked with GCC 13.3 and glibc 2.39 on
Ubuntu 24.04.

Answers describe GCC 13 and glibc on x86-64 Linux. Where MSVC or Windows behaves differently in a
way that breaks ported code, the answer says so: for a Linux role, knowing both is a strength, and
it is the honest way to connect your Win32 server work to the question.

Practise by covering the answer, saying yours aloud, then comparing. If yours is longer than the
model answer, cut it. The coding questions (`strlen`, `memmove`, `atoi`, list reversal, loop
detection) are in module 16, section 2.4, and in exercise 16.1.

---

## Part A — Building and linking

**1. What happens between `gcc main.c util.c` and a running program, and which tool reports "undefined reference" and "multiple definition"? [01]**
The preprocessor expands `#include`s and macros in each `.c` file, giving one translation unit per
file; the compiler turns each unit into assembly and the assembler into an object file holding
machine code, a symbol table and relocations. The linker resolves every undefined symbol against
the other objects and libraries, lays out the sections and patches the addresses; at run time the
loader maps the program and the C runtime calls `main`. Both errors come from the linker, because
only the linker sees all objects together: "undefined reference" means a symbol is used but no
object or library defines it, "multiple definition" means two objects define the same external
symbol. MSVC reports the same two situations as LNK2019 and LNK2005.

**2. Two `.c` files both contain `int counter;` at file scope, and a third declares `extern long counter;`. What happens? [01]**
`int counter;` without an initialiser is a tentative definition. GCC 9 and older emitted it as a
"common" symbol and the linker silently merged the two copies; since GCC 10 the default is
`-fno-common`, so GCC 13 fails with "multiple definition of `counter`". The `extern long` file
links anyway, because object files record names, not types, so on Linux it reads 8 bytes where only
4 exist (on Windows `long` is 4 bytes, so the same bug hides there). C has no cross-file check like
C++'s one-definition rule; the defence is one header declaring `extern int counter;`, included by
every user and by the one `.c` file that defines it, so the compiler compares declaration and
definition. Everything that does not need to be shared should be `static`.

**3. What does `static` mean in C? Give all three contexts. [01, 04, 16]**
On a local variable it gives static storage duration: one object, initialised once before `main`
(with a constant expression only), that keeps its value between calls and is shared by every
thread, so concurrent use needs a lock or an atomic. At file scope, on a variable or a function, it
gives internal linkage: the name is invisible to other translation units, so two files can each
have a private `helper()` without a link clash. In an array parameter, `void f(int a[static 4])`
promises that the argument points to at least four elements, which lets the compiler optimise and
warn about a NULL or too-short argument. The first two share a keyword but are unrelated features.

## Part B — Types and integers

**4. What is the difference between LP64 and LLP64, and why does it matter? [02]**
They are the two common 64-bit data models: on LP64 (Linux, macOS) `long` and pointers are 64 bits;
on LLP64 (64-bit Windows) `long` stays 32 bits and only `long long` and pointers are 64 bits. `int`
is 32 bits on both. Code written on one breaks on the other: a pointer or file offset stored in a
`long` is truncated on Windows, `%ld` is the wrong specifier for `int64_t` on one of them, and a
struct containing `long` changes size and layout, so a file written on one platform is misread on
the other. Use `<stdint.h>` types for anything with a required width, `size_t` for sizes,
`intptr_t`/`uintptr_t` for pointers stored as integers, and the `PRId64` family for printing.

**5. What are the integer promotions and the usual arithmetic conversions, and how do they cause signed/unsigned bugs? [02]**
Before arithmetic, every type narrower than `int` (`char`, `short`, `uint8_t`, small bit-fields) is
promoted to `int`, so `(uint8_t)200 + (uint8_t)100` is the `int` 300, and `~(uint8_t)0` is the
`int` -1. Then the two operands are converted to a common type, and when a signed and an unsigned
type of the same rank meet, the signed one becomes unsigned: `-1 < 1u` is false because -1 becomes
`UINT_MAX`. That is why a negative `int` length compares as huge against a `sizeof` (a `size_t`),
or turns into a huge count when passed to `memcpy`. The rules also depend on the data model:
`-1L < 1u` is true on Linux (a 64-bit `long` holds every `unsigned int`) and false on 64-bit
Windows (both become `unsigned long`). Build with `-Wsign-compare` (in `-Wextra` for C) and
`-Wconversion`, or `/W4` on MSVC.

**6. What does this print on x86-64 Linux? [02]**
```c
#include <stdio.h>

int main(void)
{
    unsigned char a = 200, b = 100;
    int sum = a + b;
    unsigned char wrapped = a + b;
    int len = -1;

    printf("%d %d\n", sum, wrapped);
    if (len < sizeof(int))
        puts("small");
    else
        puts("huge");
    printf("%d\n", -1L < 1u);
    return 0;
}
```
It prints `300 44`, then `huge`, then `1`. `a + b` is computed in `int` after promotion, so `sum`
is 300; storing 300 into an `unsigned char` reduces it modulo 256, giving 44, which is well defined
because the target is unsigned. `len < sizeof(int)` compares an `int` with a `size_t`, so -1 is
converted to `SIZE_MAX` and the test is false; GCC warns with `-Wsign-compare` under `-Wextra`. The
last line prints 1 on Linux and 0 on 64-bit Windows, for the data-model reason in question 5.

**7. Why is signed overflow undefined, and how do you check for it? [02, 09]**
Unsigned arithmetic is defined to wrap modulo 2^N, but signed overflow is undefined, so the
compiler may assume it never happens: it can treat `i + 1 > i` as always true and widen a 32-bit
loop counter to 64 bits without re-checking it. The price is that after-the-fact checks are folded
away: GCC 13 turns `b > 0 && a + b < a` into constant false, even at `-O0`. Check **before** the
operation with comparisons that cannot overflow (`b > 0 && a > INT_MAX - b`), or use
`__builtin_add_overflow` and `__builtin_mul_overflow` (C23 standardises them as `ckd_add` and
`ckd_mul` in `<stdckdint.h>`, available from GCC 14). MSVC has no such builtins in C, so Windows
code uses hand-written pre-checks or `<intsafe.h>` helpers such as `SizeTMult`. For legacy code,
`-fwrapv` makes wrapping defined, and UBSan finds the overflows at run time.

## Part C — Pointers, memory and lifetime

**8. What is the difference between an array and a pointer, and what is decay? [03]**
An array is a contiguous block of elements whose length is part of its type; a pointer is a
variable that holds an address. In most expressions an array name is converted ("decays") to a
pointer to its first element, which is why `a[i]` and `p[i]` look the same (`a[i]` is defined as
`*(a + i)`). It does not decay as the operand of `sizeof` (you get the whole array's size), of
unary `&` (`&a` has type `int (*)[N]`: same address, different type), or when a string literal
initialises a `char` array. Arrays cannot be assigned or returned, and `char a[] = "hi"` is a
modifiable 3-byte copy while `char *p = "hi"` points at a read-only literal. The difference also
shows across files: `extern int *table;` for an object defined as `int table[4]` reads the array's
first bytes as an address and crashes.

**9. What is wrong with this code? [03, 16]**
```c
#include <string.h>

void clear_balances(long balances[16])
{
    memset(balances, 0, sizeof balances);
}

int main(void)
{
    long b[16];
    clear_balances(b);      /* most of b still holds garbage */
    return 0;
}
```
A parameter declared as an array is adjusted to a pointer, so `balances` is a `long *` and
`sizeof balances` is 8 on x86-64: only the first 8 bytes are cleared, which is one `long` on Linux
and two on Windows. The `16` in the parameter is ignored and nothing checks it. GCC 13 warns twice:
`-Wsizeof-array-argument` (on by default) and `-Wsizeof-pointer-memaccess` (in `-Wall`). Pass the
count (`void clear_balances(long *b, size_t n)` with `memset(b, 0, n * sizeof *b)`), or pass a
pointer to the array, `long (*b)[16]`, where `sizeof *b` is the full size.

**10. What is the difference between `const int *p` and `int *const p`? [03, 16]**
Read the declaration from the name outward, right to left. `const int *p` (the same as
`int const *p`) is a pointer to a const int: you can re-point `p`, but cannot write `*p`.
`int *const p` is a const pointer to an int: `*p = 5` is fine, `p = &other` is an error; `const int
*const p` fixes both. A `const` on the pointee is a promise in an API ("I will not modify your
buffer", as in `strlen(const char *)`), not a guarantee that the object never changes, because
other non-const pointers may still modify it. C does not convert `char **` to `const char **`
implicitly, because that would let you store the address of a const object into a plain `char *`.

**11. What can you do with a `void *`, and what can you not? [03]**
`void *` is the generic object pointer: any object pointer converts to it and back without a cast
and without loss, which is why `malloc` returns it and `qsort` and callback APIs take it. You
cannot dereference it, and standard C does not allow arithmetic on it: GCC allows `void *`
arithmetic as an extension (`-Wpointer-arith` warns), MSVC rejects it, so convert to
`unsigned char *` for bytes. It carries no type information, so casting it to the wrong type in a
callback is silent undefined behaviour. In C, `int *p = malloc(n);` needs no cast (in C++ it does),
and function pointers are not object pointers: converting them to `void *` is not ISO C, although
POSIX requires it to work for `dlsym`.

**12. What does `restrict` mean, and how does it relate to `memcpy` versus `memmove`? [03, 05]**
`restrict` on a pointer parameter promises that, for the duration of the call, the object it points
to is accessed only through that pointer (or pointers derived from it). The compiler can then keep
values in registers across stores made through other pointers, and vectorise loops without
run-time overlap checks; breaking the promise is undefined behaviour. `memcpy`'s parameters are
`restrict`, so overlapping source and destination is UB, and real programs broke when glibc changed
its copy direction; `memmove` must handle overlap, typically by choosing the copy direction. C++
has no `restrict`; GCC, Clang and MSVC all accept `__restrict` as an extension there.

**13. What are the four storage durations, and what is the difference between scope and lifetime? [04]**
Automatic: locals and parameters, alive from block entry to block exit, indeterminate unless
initialised. Static: globals and `static` locals, alive for the whole run, zero- or
constant-initialised before `main`. Allocated: from `malloc`, `calloc` or `realloc` until `free`.
Thread: `_Thread_local` objects, one instance per thread for that thread's lifetime. Scope is the
part of the source where a name is visible; lifetime is when the object exists at run time, so a
`static` local has block scope but program lifetime, and a heap block has a lifetime but no name.
A dangling pointer is a pointer used after the lifetime of its object has ended, whatever its scope.

**14. What is wrong with this code, and what does it print when built with GCC 13? [04]**
```c
#include <stdio.h>

const char *format_account(int id)
{
    char buf[16];
    snprintf(buf, sizeof buf, "ACC-%06d", id);
    return buf;
}

int main(void)
{
    const char *a = format_account(42);
    const char *b = format_account(7);
    printf("%s %s\n", a, b);
    return 0;
}
```
`buf` has automatic storage, so its lifetime ends at `return` and both pointers dangle. GCC 13 warns
(`-Wreturn-local-addr`, on by default) and deliberately compiles the return as NULL, so the program
prints `(null) (null)` (glibc's text for a NULL `%s`) at every optimisation level. MSVC warns
(C4172) but returns the real stack address; the bytes are often still there, so such code "works"
until another call reuses that stack: here the second call overwrites the first result, and
`printf`'s own frame can overwrite both. Fix it
with a caller-provided buffer (`int format_account(char *out, size_t outsz, int id)`), a heap
result the caller frees, or a struct returned by value; a `static` buffer stops the crash but is
neither thread-safe nor safe to call twice in one `printf`.

**15. State the contracts of `malloc`, `calloc`, `realloc` and `free`. What is the realloc leak? [04, 10]**
`malloc(n)` returns uninitialised memory aligned for any type, or NULL; `malloc(0)` may return NULL
or a unique pointer (glibc returns a pointer), which must still be freed. `calloc(count, size)`
returns zeroed memory and checks `count * size` for overflow, which `malloc(count * size)` does not.
`realloc(p, n)` may move the block: on success the old pointer is invalid; on failure it returns
NULL and leaves the old block allocated and unchanged, so `p = realloc(p, n)` leaks the block and
loses your data exactly when memory is short. The idiom is `void *tmp = realloc(p, n); if (!tmp)
return ERR; p = tmp;`, after checking the size calculation for overflow. `free(NULL)` is a no-op,
a double free is UB, `realloc(p, 0)` is implementation-defined in C17 and undefined in C23, and on
Windows a block must be freed by the same CRT that allocated it (a classic DLL-boundary bug).

## Part D — Strings

**16. Why is `strncpy` not a safe `strcpy`, and what do you use instead? [05]**
`strncpy(d, s, n)` was designed for fixed-width, NUL-padded record fields: it always writes exactly
`n` bytes, padding with zeros when `s` is shorter, and writes no terminator at all when `s` has `n`
or more characters, so the result may not be a string. `snprintf(d, sizeof d, "%s", s)` always
terminates (when the size is non-zero) and returns the length it wanted to write, so
`ret < 0 || (size_t)ret >= sizeof d` detects truncation. A `strlcpy`-style helper (copy
`min(len, size - 1)` bytes, add a NUL, return `strlen(src)`) does the same without format parsing;
glibc has had `strlcpy` since 2.38, but portable code still carries its own. On MSVC, the old
`_snprintf` does not terminate on truncation and returns -1 (only `snprintf` from Visual Studio 2015
on is conforming), and `strncpy_s` truncates only if you pass `_TRUNCATE`.

**17. Why is `atoi` dangerous, and how do you use `strtol` correctly? [05, 10]**
`atoi` cannot report errors: `"abc"` gives 0 and an out-of-range value is undefined behaviour. With
`strtol`: set `errno = 0`, pass an explicit base (base 0 treats a leading zero as octal, so `"010"`
becomes 8), then check that `end != s` (some digits were read), that `errno != ERANGE` (on overflow
the value is clamped to `LONG_MIN` or `LONG_MAX`), that `*end == '\0'` (no trailing junk), and that
the value fits your target type. The range check matters across platforms: `strtol("3000000000")`
succeeds on Linux but gives `ERANGE` on Windows, where `long` is 32 bits; use `strtoll` for 64-bit
values. `strtoul` accepts `"-1"` and returns `ULONG_MAX`, so reject a minus sign yourself, and wrap
all of this once in a helper that writes `*out` only on success.

## Part E — Structs, layout and binary data

**18. Why is `sizeof` a struct often larger than the sum of its members, and what is `offsetof` for? [06]**
Each member is placed at an offset that is a multiple of its alignment, and the total size is
rounded up to a multiple of the largest member alignment so that array elements stay aligned. On
x86-64, `struct { char a; double b; char c; }` is 24 bytes (`b` at offset 8, then 7 bytes of tail
padding), while `struct { double b; char a; char c; }` is 16, so sorting members by decreasing
alignment usually minimises padding. `offsetof(type, member)` from `<stddef.h>` gives a member's
byte offset; combine it with `_Static_assert` to lock layouts that matter, and use `pahole` to see
the holes. Padding bytes have unspecified values, so `memcmp` on structs or writing them out byte
for byte compares or leaks garbage.

**19. What do packed structs cost, and what is the problem with unaligned access? [06, 09]**
`__attribute__((packed))` in GCC, or `#pragma pack(push, 1)` ... `#pragma pack(pop)` in MSVC (GCC
accepts it too), removes padding so a struct matches an external byte layout. Accessing a member by
name stays correct, because the compiler knows it is misaligned and emits safe loads, but it can be
slower. The danger is taking a member's address: an `int *` to a misaligned `int` is undefined
behaviour when dereferenced, faults on strict-alignment CPUs, can break vectorised code even on x86,
and GCC warns with `-Waddress-of-packed-member`; casting `buf + 1` to `uint32_t *` is the same bug,
and UBSan's alignment check reports it. Atomics and locks inside a packed struct may not be atomic.
Decode external formats field by field from a byte buffer instead, and when you must pack, always
use push/pop so the setting does not leak into other headers.

**20. What is the strict aliasing rule, and how do you reinterpret bits legally? [06, 09]**
C17 6.5p7 says an object may be accessed only through an lvalue of its own (compatible) type, its
signed or unsigned variant, a qualified version, an aggregate or union containing it, or a
character type. So reading a `float` through `*(uint32_t *)&f` is undefined: at `-O2` GCC assumes a
`uint32_t *` and a `float *` never refer to the same object, and may reuse stale register values or
reorder the accesses. Legal ways to reinterpret bits are `memcpy(&u, &f, sizeof u)`, which compiles
to a single register move; reading the bytes through `unsigned char *`; or, in C but not in C++,
writing one union member and reading another. Legacy code that casts buffers to struct pointers can
be built with `-fno-strict-aliasing` (the Linux kernel does). MSVC does not, in practice, optimise
on strict aliasing, which is why code ported from Windows can break only under GCC or Clang.

**21. Why should you never `fwrite` a struct to a file or a socket? [06, 13]**
The in-memory bytes of a struct depend on the compiler and platform: padding (garbage, and possibly
leaked old data), member sizes (`long` is 8 bytes on Linux and 4 on Windows), alignment and packing
rules, and host byte order. So a record written by one build is misread by another compiler, OS or
CPU, or by the same program after someone adds a field, and nothing in the bytes says which layout
wrote them. Define an explicit format instead: a magic number, a version, fixed-width fields at
documented offsets in a stated byte order, lengths and a checksum, encoded and decoded field by
field with bounds checks. A versioned format also lets the reader accept old files, which a struct
dump never can.

## Part F — Functions and the preprocessor

**22. How do function pointers and callbacks with a `void *` context work in C? [07]**
A function pointer holds the address of code: `int (*cmp)(const void *, const void *)` points to a
function taking two `const void *` and returning `int`, and a typedef keeps such declarations
readable. C callback APIs pass a `void *ctx` next to the function; the library stores it and hands
it back unchanged, and the callback casts it to the real type, so one callback can serve many
callers and threads without globals. The costs are no type checking across that cast and a
lifetime rule: the context must outlive every invocation, including ones on other threads or after
an unsubscribe. `qsort` has no context parameter, which is why glibc added `qsort_r` (comparator
`(a, b, ctx)`) and MSVC has `qsort_s` (comparator `(ctx, a, b)`): same idea, different argument
order. A dispatch table of handlers indexed by a range-checked opcode is the same mechanism used to
route requests in a server.

**23. How does a variadic function work, and what is a format-string bug? [07]**
A variadic function reads its extra arguments with `va_start`, `va_arg(ap, type)` and `va_end`, but
it cannot know how many there are or their types; caller and callee agree through a count, a
sentinel or a format string. Default argument promotions apply (`float` becomes `double`, `char`
and `short` become `int`), and reading an argument as the wrong type is undefined, which is why
`%ld` for an `int64_t` works on Linux and fails on Windows (use `PRId64`). A format-string
vulnerability is passing untrusted data as the format, `printf(msg)`: `%x` leaks stack contents,
`%s` crashes and `%n` writes to memory. Always use a literal format (`printf("%s", msg)`), mark
your own wrappers `__attribute__((format(printf, 2, 3)))` so GCC checks every caller, and build
with `-Wformat=2`. MSVC checks literal formats passed to the CRT's own printf family (warnings such as
C4477, since Visual Studio 2015), checks your own wrappers only under `/analyze` with
`_Printf_format_string_` annotations, and disables `%n` by default.

**24. What does `check(0, 3)` print, and what is wrong with these macros? [08]**
```c
#include <stdio.h>

#define SQUARE(x)      x * x
#define LOG_FAIL(msg)  fprintf(stderr, "%s\n", msg); ++g_failures

static int g_failures = 0;

static void check(int rc, int n)
{
    printf("%d\n", SQUARE(n + 1));
    if (rc != 0)
        LOG_FAIL("transfer failed");
}
```
It prints `7`, not 16, and increments `g_failures` even though `rc` is 0. `SQUARE(n + 1)` expands
to `n + 1 * n + 1`, so parameters and the whole body need parentheses, `((x) * (x))`, and even then
`SQUARE(i++)` evaluates `i++` twice (unsequenced modifications: UB). `LOG_FAIL` expands to two
statements, so the `if` controls only the `fprintf` (GCC warns with `-Wmultistatement-macros`, in
`-Wall`). Bare braces do not fix it, because `if (rc) LOG_FAIL("x"); else ...` becomes `{ ... };
else`, a syntax error; the fix is `do { fprintf(stderr, "%s\n", (msg)); ++g_failures; } while (0)`,
one statement that takes exactly one semicolon and that the compiler optimises away. For
computations, a `static inline` function avoids every one of these traps and is type-checked.

**25. What is an X-macro, and why use one? [08]**
An X-macro keeps one list of items in a macro that takes another macro as its parameter, and
expands the list several times with different definitions of that parameter. For example
`#define OPCODES(X) X(OP_BALANCE, h_balance, "balance") X(OP_TRANSFER, h_transfer, "transfer")`
can generate the opcode `enum`, the table of names for logs and the dispatch table of handlers. Adding
an opcode is one line and the generated artefacts cannot drift apart; a `_Static_assert` comparing
the table size with an `OP_COUNT` catches what remains. The cost is code that is harder to read and
grep, so keep the list in one obvious place and put a sample `gcc -E` expansion in a comment.

**26. What is `_Generic`, and what are its traps? [08]**
`_Generic` (C11) selects an expression at compile time by the type of a controlling expression:
`_Generic((x), int: print_int, double: print_double, const char *: print_str, default: print_other)(x)`
calls the function that matches `x`'s type, after lvalue conversion (top-level qualifiers dropped,
arrays decayed). It is how `<tgmath.h>` makes `sqrt` work for all floating types, and how you write
type-safe macros without C++ overloading. The associations must be distinct types, so listing both
`long` and `int64_t` fails on Linux with "'_Generic' specifies two compatible types" (they are the
same type there), while `long long` and `int64_t` fails on Windows; list the base types and let
typedefs fall into them. Character literals are `int` in C, so `_Generic('a', char: ..., int: ...)`
picks `int`. MSVC supports `_Generic` from Visual Studio 2019 16.8 with `/std:c11` or `/std:c17`.

## Part G — volatile, cleanup and errors

**27. What does `volatile` do, and why is it not a tool for threads? [09, 14, 16]**
`volatile` tells the compiler that every read and write of the object is an observable side effect:
each access must actually happen, in program order relative to other volatile accesses, with no
caching in registers. That is what memory-mapped hardware registers need, and what a
`volatile sig_atomic_t` flag written by a signal handler needs. It does nothing for threads: `v++`
is still a separate load, add and store, ordinary memory around it is not ordered, no CPU barrier is
emitted, and a concurrent non-atomic access is still a data race, which is undefined. Use
`_Atomic`/`<stdatomic.h>` or a mutex. Windows code sometimes gets away with `volatile` flags
because MSVC on x86 and x64 defaults to `/volatile:ms`, which gives volatile accesses
acquire/release semantics; that is a vendor extension, off by default when targeting ARM, and GCC
never does it.

**28. How do you release resources on every error path in C? Explain the goto-cleanup idiom. [10]**
C has no destructors, so a function that acquires several resources needs one place that releases
them. Initialise every handle to a "nothing" value (NULL, -1) at the top, acquire in order, and on
any failure set the error code and `goto cleanup`; the cleanup block at the end releases, in reverse
order, whatever was acquired, followed by a single `return`. The jumps only go forward to one place,
so the flow stays readable, and adding a resource means one acquire and one release instead of
editing every early return. The Linux kernel coding style recommends it (often with one label per
stage) and CERT C MEM12-C describes it; it is the C counterpart of RAII. GCC's
`__attribute__((cleanup))` automates it but is not available in MSVC; either way, prove it with
fault injection that fails the Nth allocation or open for every N.

**29. What are the rules for using `errno` correctly? [10]**
`errno` is meaningful only immediately after a function has reported failure through its return
value (-1, NULL, EOF); successful calls may change it, nothing resets it to 0, and any later call,
including the logging call you make next, can overwrite it, so save it into a local at once.
Functions whose return value cannot signal failure, such as `strtol` and some `<math.h>` functions,
need `errno = 0` before the call. Many APIs do not use it: pthread functions return the error number
directly, `getaddrinfo` returns `EAI_*` codes, Win32 calls report through `GetLastError()` (which
also must be saved before any other API call) and Winsock through `WSAGetLastError()`. `errno` is
thread-local in every modern C library, so threads do not overwrite each other's value. A library
should return its own error codes and provide a `lib_strerror` rather than make `errno` its API.

## Part H — Data structures and allocators

**30. What is an intrusive list, and how does `container_of` work? [06, 11]**
In an intrusive list the link fields are embedded in the object
(`struct account { ...; struct dlink by_branch; struct dlink by_status; }`) instead of in a
separately allocated node that points to the object. Linking and unlinking need no allocation, an
object can be on several lists at once, and given the object you can remove it in O(1). To get from
a link back to its object, `container_of(ptr, type, member)` subtracts the member's offset:
`(type *)((char *)(ptr) - offsetof(type, member))`. The Linux kernel's `list_head` and most LRU
caches in C servers work this way. The costs: the type must know in advance which lists it can join,
and a wrong `type` or `member` argument silently yields a garbage pointer, so hide it behind typed
helper functions.

**31. When do you use an arena, a pool, or plain `malloc`? [04, 12]**
`malloc` is general purpose: any size, frees in any order, at the cost of a per-block header (the
smallest glibc chunk is 32 bytes on 64-bit), locking or per-thread caches, and fragmentation over
time. An arena hands out pieces of a large block by bumping an aligned offset and frees everything
at once with a reset: ideal when many objects share one lifetime (a request, a parse, a migration
batch), wrong when objects need individual frees. A pool serves one fixed size from a free list
threaded through the free slots themselves: O(1) allocation and free and no external fragmentation,
ideal for millions of same-size objects such as sessions or connections. The arena's risk is its
contract: every pointer is borrowed until the next reset, so storing one in a long-lived cache
gives silent wrong data later. Both hide bugs from ASan and Valgrind unless you add manual
poisoning or a build switch that routes them to `malloc`; on Windows, `HeapCreate`/`HeapDestroy`
gives you an arena-like private heap.

**32. How does `malloc` work internally? [12]**
glibc's allocator (ptmalloc2) splits memory into chunks, each with a size header whose low bits
hold flags (previous chunk in use, mmapped, non-main arena); the previous chunk's size field acts as
a boundary tag, so `free` can coalesce with both neighbours in O(1). Free chunks live in bins by
size: a per-thread tcache (by default up to 7 chunks per size, for requests up to about 1 KiB, with
no lock), fastbins, an unsorted bin and sorted small and large bins; allocation tries roughly that
order and splits a bigger chunk when needed. The heap grows with `brk`; requests above the mmap
threshold (128 KiB initially, adjusted dynamically) get their own `mmap` and go back to the OS on
`free`, while freed memory inside the heap is returned only when the top can be trimmed, which is
why RSS can stay high after a burst with no leak. Contending threads get extra arenas (up to 8 per
core on 64-bit, capped by `MALLOC_ARENA_MAX`). Errors such as `free(): invalid next size` are
glibc's integrity checks finding a corrupted header, usually long after the overflow; on Windows
the equivalent layers are the NT heap and the Low Fragmentation Heap, and PageHeap catches the
overflow at the faulting write.

## Part I — Files and I/O

**33. What does this print on a terminal, and what does it print with `./a.out | cat`? [13]**
```c
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    printf("start\n");
    if (fork() == 0) {
        printf("child\n");
        return 0;
    }
    wait(NULL);
    printf("parent\n");
    return 0;
}
```
On a terminal: `start`, `child`, `parent`. Piped: `start`, `child`, `start`, `parent`. stdout is
line buffered when it is a terminal, so `start\n` is written before `fork`; when it is a pipe or a
file it is fully buffered, so `start\n` is still in the buffer, `fork` copies the buffer into the
child, and each process flushes its own copy at `exit` (stderr is unbuffered, so it never has this
problem). Fix it with `fflush(stdout)`, or `fflush(NULL)` for every stream, before `fork`, and end a
child that does not `exec` with `_exit` after flushing only its own output. The same mechanism
explains "lost" last log lines when a process crashes with stdout redirected: `exit` flushes stdio,
but `_exit`, `abort` and a crash do not. On Windows there is no `fork`, but note that MSVC's CRT
treats `_IOLBF` as full buffering, so redirected output is also block-buffered there.

**34. Why must `read` and `write` be called in a loop, and what is `EINTR`? [13]**
Both may transfer fewer bytes than requested: routinely on pipes, sockets and terminals, and on
regular files at end of file, on a full disk, or when a signal arrives after some data has moved. A
blocking call interrupted by a signal handler before transferring anything fails with `EINTR`
unless the handler was installed with `SA_RESTART`, and some calls (`poll`, `select`, `epoll_wait`)
never restart. So "send these N bytes" or "read this N-byte header" is a loop: advance by the count
returned, retry on `EINTR`, treat 0 from `read` as end of file (the peer closed), and stop on any
other error. Library code always retries on `EINTR` because it cannot know how the application
installed its handlers. On non-blocking sockets the loop also waits in `poll` on `EAGAIN`; Winsock's
`send` and `recv` can be partial too, with `WSAEWOULDBLOCK` in place of `EAGAIN`.

**35. Does a successful `write` mean the data is on disk? How do you make a file update durable? [13]**
No: it means the data is in the kernel's page cache, which Linux writes back later (typically
within about 30 seconds), so a power cut or kernel crash can lose it, although a process crash
cannot. `fsync(fd)` returns only when the file's data and metadata are on stable storage, and asks
the drive to flush its own cache; `fdatasync` skips metadata that is not needed to read the data
back. To replace a file atomically and durably: write a temporary file in the same directory,
`fsync` it, `rename` it over the target (atomic within one filesystem), then `fsync` the directory
so the rename itself survives. Treat an `fsync` failure as data loss, not as something to retry:
Linux may already have dropped the dirty pages, so a second `fsync` can succeed without the data
ever reaching disk (the 2018 PostgreSQL "fsyncgate" finding). With stdio, `fflush` first, then
`fsync(fileno(f))`; on Windows the equivalents are `FlushFileBuffers` and `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.

## Part J — Threads

**36. Why must `pthread_cond_wait` be called in a `while` loop? [14]**
`pthread_cond_wait(&cv, &m)` must be called with `m` held; it atomically releases the mutex and
blocks, and reacquires the mutex before returning. A return does not mean the condition is true:
POSIX allows spurious wakeups, another thread may consume the item between the signal and your
reacquiring the mutex, and one condition variable may announce several conditions (item available,
shutdown). So the only correct form is `while (!predicate) pthread_cond_wait(&cv, &m);` with the
predicate stored in shared state protected by `m`. The signalling side must change that state under
the same mutex; otherwise it can set the flag and signal between the waiter's check and its sleep,
and the wakeup is lost for good. Win32's `SleepConditionVariableSRW` and `SleepConditionVariableCS`
follow exactly the same rules, whereas auto-reset Events are a different, coalescing mechanism.

**37. What is the difference between thread-safe and reentrant? [14]**
Thread-safe means correct when called from several threads at the same time, possibly by using locks
or per-thread state. Reentrant means correct when called again before a previous call in the same
thread has finished (recursion, a callback, a signal handler), which requires no hidden shared state
at all. `strtok`, `localtime`, `strerror` and `inet_ntoa` are neither, because they keep a static
cursor or buffer; their `_r` versions take that state from the caller and are both, while a static
buffer moved to `_Thread_local` becomes thread-safe but still not reentrant (two calls in one
`printf` still overwrite each other). `malloc` and `printf` are thread-safe because they lock, which
is exactly why they are not async-signal-safe: a handler may call only functions such as `write` and
`_exit`, or set a `volatile sig_atomic_t` flag. MSVC's CRT keeps per-thread buffers for `strtok` and
`localtime`, so Windows code that used them from threads breaks when ported to glibc; the Windows
replacements are `strtok_s` and `localtime_s` (whose argument order is the reverse of C11 Annex K's).

**38. What do C11 atomics give you, and when do you use them instead of a mutex? [14]**
`<stdatomic.h>` provides `_Atomic` types (`atomic_int`, `atomic_bool`, `_Atomic(struct node *)`)
and indivisible, race-free operations such as `atomic_load`, `atomic_store`, `atomic_fetch_add` and
`atomic_compare_exchange_weak`. The plain forms are `memory_order_seq_cst`; the `_explicit` forms
take an order: `relaxed` gives atomicity only (right for statistics counters), a `release` store
read by an `acquire` load makes everything written before the store visible after the load (how
you publish a filled buffer or pointer), and `seq_cst` adds one global order of all such operations.
On x86, loads are plain moves in every order, a `seq_cst` store becomes `xchg`, and read-modify-writes
are `lock`-prefixed instructions. Use an atomic for a single variable with a simple update; once an
invariant spans several fields, use a mutex. MSVC supports `<stdatomic.h>` only in recent Visual
Studio 2022 releases (initially behind `/experimental:c11atomics`), so Windows C code traditionally
uses the `Interlocked*` functions, which are full barriers.

## Part K — Beyond the language

**39. What are the differences between C and C++ that matter in practice? [16]**
Some code compiles in both but means different things: `'a'` is an `int` in C (`sizeof 'a'` is 4)
and a `char` in C++; `void f()` declares unspecified parameters in C before C23 but no parameters in
C++; reading a union member other than the last one written is defined in C and undefined in C++;
and `const int n = 10;` is a constant expression in C++, while in C `int a[n]` inside a function is
a VLA. Some valid C is not valid C++: `int *p = malloc(n)` without a cast, out-of-order or array
designated initialisers, compound literals, VLAs and `restrict`; and a file-scope `const` has
external linkage in C but internal linkage in C++. C++ mangles names to encode parameter types, so
a C library header must wrap its declarations in `extern "C" { }` (inside `#ifdef __cplusplus`) for
C++ callers to link. Plain C99 `inline` also differs: a header `inline` function with no external
definition can fail to link at `-O0`, which is why C headers use `static inline`. More broadly, C
leaves ownership to discipline (goto cleanup, ownership comments, `void *` generics) where C++
encodes it in types (RAII, smart pointers, templates).

**40. You join a team with a large, unfamiliar legacy C code base. How do you get productive and change it safely? [15]**
First make it build and run locally, and generate a `compile_commands.json` (for example with
`bear -- make`) so clangd, ctags or cscope can index it; on Windows, Visual Studio's "Find All
References" and call hierarchy do the same job. Then read the core data structures, find the entry
points (`main`, thread start routines, the request dispatch table), and trace one real request end
to end in a debugger, writing a one-page map as you go. Before changing anything, look for hidden
coupling: globals written from several files, functions returning static buffers, non-reentrant
library calls, and which lock protects which data. For each fix, write a characterisation test that
pins today's behaviour, isolate the code through a seam (`-Wl,--wrap`, link substitution, function
pointers), make the smallest change that removes the root cause, and keep refactoring in separate
commits. Use `git log -L`, `git blame -w -C` and `git bisect run` to learn why the code is the way it
is, and run sanitizers, `-fanalyzer` and cppcheck against a recorded baseline so CI fails only on
new findings.
