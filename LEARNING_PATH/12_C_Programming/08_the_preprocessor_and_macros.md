# 08. The preprocessor and macros

**Time:** ~2.5 hours   **Prerequisites:** 01, 07

## 1. In plain words

Before the compiler sees your code, a text-processing robot runs over it: the **preprocessor**. It
does three jobs. It pastes files into other files (`#include`). It replaces names with text
(`#define`). And it deletes chunks of code depending on conditions (`#if`, `#ifdef`). It knows
nothing about types, variables, scopes or functions; it works on **tokens** (words, numbers,
punctuation). Its output is a single long C file, the translation unit of module 01.

Analogy: find-and-replace across a document before it goes to the translator. Find-and-replace is
powerful and completely literal. Replace `x` with `a + b`, and the sentence `2 * x` becomes
`2 * a + b`, which is not what anybody meant. Nearly every macro bug is that sentence in some
disguise.

You will never write a large C program without the preprocessor: headers, platform differences
(`_WIN32` versus POSIX), logging with file and line numbers, and error-code tables all depend on it.
The skill is knowing its traps well enough to use it only where a function or a constant cannot do
the job, and to read the macro-heavy code that legacy servers are full of.

## 2. The real thing

### 2.1 `#include` mechanics

`#include` pastes the named file's text in place of the directive. The two spellings differ only in
where the preprocessor looks first:

| Form | Search order (GCC) |
|---|---|
| `#include "foo.h"` | the directory of the file containing the `#include`, then `-iquote` dirs, then the `<...>` list |
| `#include <foo.h>` | `-I` directories in command-line order, then system directories (`/usr/include`, the compiler's own) |

MSVC is similar (`/I` for both). `gcc -H` prints every header opened (module 01), `cl /showIncludes`
does the same on Windows. Two habits keep include graphs healthy: include what you use (every
header includes the headers it needs, nothing more), and use a forward declaration
(`struct account;`) instead of an include when a header only needs a pointer to the type.

### 2.2 Include guards versus `#pragma once`

A header included twice in one TU would define its structs twice: an error. Two ways to prevent it:

```c
/* bank_account.h */
#ifndef BANK_ACCOUNT_H
#define BANK_ACCOUNT_H
/* ... declarations ... */
#endif /* BANK_ACCOUNT_H */
```

```c
#pragma once
/* ... declarations ... */
```

| | Include guard | `#pragma once` |
|---|---|---|
| Standard | yes | no, but GCC, Clang and MSVC all support it |
| Failure mode | two headers that accidentally use the **same guard name** (copy-pasted header): the second is silently skipped | the same file reached through two different paths (symlinks, network shares, a copied file) may be included twice, or two identical files treated as one |
| Speed | GCC and Clang recognise the guard idiom and skip re-opening the file, so no real difference | fast |

Use either, consistently. Name guards after the project and path (`BANK_CORE_ACCOUNT_H`). Do not
start them with an underscore followed by a capital letter (`_ACCOUNT_H`): those identifiers are
reserved for the implementation.

### 2.3 Object-like and function-like macros

```c
#define MAX_CONN 256                    /* object-like */
#define SQUARE(x) ((x) * (x))           /* function-like: NO space before the ( */
```

- A function-like macro's arguments are **token sequences**, split on top-level commas. Commas
  inside parentheses are protected; commas inside braces are not, so `M({1, 2})` passes two
  arguments, `{1` and `2}`.
- After substitution the result is **rescanned** for more macros, but a macro is never expanded
  inside its own expansion, so macros cannot recurse.
- Predefined macros you will use: `__FILE__`, `__LINE__`, `__DATE__`, `__TIME__` (these two make
  builds non-reproducible), `__STDC_VERSION__`, and the platform and compiler macros in section 2.8.
  `__func__` looks similar but is **not a macro**: it is a predefined `static const char` array inside
  each function. So `"in " __func__` does not compile (string-literal concatenation happens at the
  token level, and `__func__` is not a literal); pass it as an argument instead.
- `gcc -dM -E file.c` dumps every macro defined at the end of preprocessing. Compile with `-g3` and
  gdb can `macro expand LOG(LOG_INFO, "x")` and `info macro LOG`.

### 2.4 The classic macro traps

All of these are in `code/m08_macro_traps.c`, each next to its fix.

**Precedence.** Parenthesise every use of a parameter **and** the whole body:

```c
#define SQUARE_BAD(x) x * x          /* SQUARE_BAD(2 + 3) -> 2 + 3 * 2 + 3 = 11 */
#define SQUARE(x)     ((x) * (x))    /* 25 */
#define DOUBLE_BAD(x) (x) + (x)      /* 10 * DOUBLE_BAD(3) -> 10 * (3) + (3) = 33 */
```

**Double evaluation.** The argument's text is pasted wherever the parameter appears, so side effects
happen as many times:

```c
#define MAX_BAD(a, b) ((a) > (b) ? (a) : (b))
int m = MAX_BAD(i++, 3);   /* ((i++) > (3) ? (i++) : (3)): i++ runs twice, m == 6, i == 7 */
```

Same with function calls: `MAX(read_balance(acct), limit)` reads the balance twice, possibly getting
two different values from another thread. A `static inline` function evaluates each argument once.

**Multi-statement bodies.**

```c
#define BUMP_BOTH_BAD(a, b) (a)++; (b)++
if (enabled)
    BUMP_BOTH_BAD(a, b);     /* expands to: if (enabled) (a)++; (b)++;  -- b changes anyway */
```

GCC 7 and later warn (`-Wmultistatement-macros`, in `-Wall`); MSVC and older compilers say nothing.

**Name capture.** A macro's local variable can collide with the caller's:

```c
#define SWAP(a, b) do { int tmp = (a); (a) = (b); (b) = tmp; } while (0)
SWAP(tmp, x);                /* int tmp = (tmp); ...  -- the caller's tmp is shadowed */
```

**Scope blindness.** Macros ignore scope and namespaces. `<windows.h>` defines `min` and `max` as
macros (define `NOMINMAX` before including it) and `wingdi.h` defines `ERROR` as 0, which breaks any
enum or function of yours named `ERROR` (define `NOGDI` if you do not need GDI). Prefix your macro
names (`BANK_`) and use upper case so macros are recognisable.

**Debuggability.** A macro has no symbol, you cannot step into it, and compiler errors point into
the expansion.

### 2.5 Why `do { ... } while (0)`

A multi-statement macro must behave like **one statement that needs exactly one semicolon**. Braces
alone are not enough:

```c
#define SWAP_BRACES(a, b) { int t = (a); (a) = (b); (b) = t; }
if (x) SWAP_BRACES(a, b); else other();
/* expands to:  if (x) { ... } ; else other();   -> "else without a previous if" */
```

`do { ... } while (0)` is a single statement, requires the trailing semicolon the caller writes, and
the optimiser removes the loop entirely. Every multi-statement macro in this course uses it.

### 2.6 `#`, `##` and variadic macros

```c
#define STR(x)    #x            /* stringise the argument's tokens */
#define XSTR(x)   STR(x)        /* expand first, then stringise */
#define CAT(a, b) a##b          /* paste two tokens into one */

STR(__LINE__)    /* "__LINE__" : # applies to the argument BEFORE it is expanded */
XSTR(__LINE__)   /* "78"       : XSTR's argument is expanded, then passed to STR */
CAT(counter_, debit)   /* counter_debit */
```

Arguments are normally macro-expanded before substitution, **except** where the parameter is used
with `#` or `##`. That is why stringising the *value* of a macro needs the two-level `XSTR`.

Variadic macros (C99) use `...` and `__VA_ARGS__`. In C17 the `...` must receive at least one
argument; with `-Wpedantic`, GCC rejects a call that passes none:

```
error: ISO C99 requires at least one argument for the "..." in a variadic macro [-Werror]
```

GCC's `, ##__VA_ARGS__` extension swallows the comma; C23 adds `__VA_OPT__(,)` for the same job. The
portable C17 solution is to make the format string part of `__VA_ARGS__`:
`#define LOG(lvl, ...) log_write((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__)`. A call always
has at least the format, so there is never an empty `...`.

### 2.7 X-macros

An X-macro is a list written once as a macro that takes another macro as a parameter. Each time you
need the list in a different shape, you define `X` to produce that shape and expand the list:

```c
#define ERROR_TABLE(X)                                                    \
    X(ERR_OK,                 0, "success")                               \
    X(ERR_NOT_FOUND,          1, "account not found")                     \
    X(ERR_INSUFFICIENT_FUNDS, 2, "insufficient funds")                    \
    X(ERR_ACCOUNT_FROZEN,     3, "account is frozen")                     \
    X(ERR_DB_UNAVAILABLE,     4, "database unavailable, retry later")     \
    X(ERR_PROTOCOL,           5, "malformed request")

enum err_code {
#define X(name, value, text) name = value,
    ERROR_TABLE(X)
#undef X
};

static const char *err_name(enum err_code e)
{
    switch (e) {
#define X(name, value, text) case name: return #name;
        ERROR_TABLE(X)
#undef X
    }
    return "ERR_UNKNOWN";
}
```

The enum, the symbolic names for logs, the human messages, a count, and even struct definitions with
matching debug printers all come from one table and can never drift apart. Adding an error is one
line. The cost is readability for people who have not seen the idiom; a comment pointing to the
table fixes that. Note that the table's continuation lines end in `\` and contain no `//` comment:
see section 4 for why.

### 2.8 Conditional compilation and platform macros

```c
#if defined(_WIN32)          /* defined on 32- AND 64-bit Windows; _WIN64 only on 64-bit */
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  include <unistd.h>
#else
#  error "unsupported platform: add a branch to the portability layer"
#endif
```

| Macro | Meaning |
|---|---|
| `_WIN32`, `_WIN64` | Windows target (any compiler, including MinGW) |
| `__linux__`, `__APPLE__`, `__unix__` | Linux, macOS, Unix-like |
| `_MSC_VER` | MSVC, with its version (1938 for VS 2022 17.8) |
| `__clang__` | Clang (test **before** `__GNUC__`, which Clang also defines) |
| `__GNUC__`, `__GNUC_MINOR__` | GCC (and compatible compilers) |
| `__x86_64__` / `_M_X64`, `__aarch64__` / `_M_ARM64` | CPU architecture, GCC/Clang spelling and MSVC spelling |
| `__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__` | byte order (GCC, Clang) |

**Feature-test macros** go the other way: you define them, before the first `#include`, to ask the
C library for more than ISO C. `_POSIX_C_SOURCE 200809L` exposes POSIX functions (`strtok_r`,
`strnlen`, `nanosleep`) when compiling with `-std=c17`; `_GNU_SOURCE` exposes everything glibc has.
On Windows, `WIN32_LEAN_AND_MEAN` trims `<windows.h>`, `NOMINMAX` removes `min`/`max`,
`_WIN32_WINNT` selects the minimum Windows version whose APIs are declared.

Rules for platform code:

- Keep `#ifdef`s in **one small portability layer** (`plat_sleep_ms`, `plat_mutex_lock`); the rest of
  the code calls those functions and never names a platform (section 3.5).
- A misspelled macro in `#if` silently evaluates to 0. `-Wundef` catches it:
  `warning: "ENABLE_AUDIT_LOGS" is not defined, evaluates to 0 [-Wundef]`.
- Use `#if 0 ... #endif` to disable a block; `/* */` comments do not nest.
- Code in a branch that your CI never compiles rots. Build every platform you ship, every time.

### 2.9 `_Static_assert`

A compile-time check (C11). If the constant expression is zero, compilation fails with your message:

```c
_Static_assert(sizeof(struct wire_header) == 12, "wire_header must be exactly 12 bytes");
_Static_assert(offsetof(struct wire_header, length) == 8, "length must be at offset 8");
_Static_assert((PAGE_SIZE & (PAGE_SIZE - 1)) == 0, "PAGE_SIZE must be a power of two");
_Static_assert(ERR_COUNT == 6, "error table changed: update the client documentation");
```

Put one next to every struct that crosses a process boundary, every table that must match an enum,
and every assumption about type sizes. `<assert.h>` also defines the spelling `static_assert` (a
keyword in C23, where the message becomes optional). Legacy code uses the pre-C11 trick of a typedef
with a negative array size, `typedef char check_x[(cond) ? 1 : -1];`; Windows headers wrap the same
trick as `C_ASSERT(cond)`.

### 2.10 `_Generic`

`_Generic(expr, type1: result1, type2: result2, default: result3)` chooses one result by the **type**
of `expr`, at compile time. The controlling expression is not evaluated. Uses:

- type-generic interfaces such as `<tgmath.h>` (`sqrt` picks `sqrtf`, `sqrt` or `sqrtl`);
- a `MAX(a, b)` that dispatches to a real function per type, so each argument is evaluated once;
- type-safe print or serialise macros.

Things it teaches you about C types (from `code/m08_generic_and_static_assert.c`):

```
TYPE_NAME('a')                  = int   (character constants are int in C, char in C++)
TYPE_NAME(sizeof(int))          = unsigned long   (size_t on LP64)
TYPE_NAME(small + small)        = int   (uint8_t promoted)
```

A trap: on Linux `int64_t` **is** `long`, so a `_Generic` that lists both is an error there and fine
on Windows (where `int64_t` is `long long`):

```
error: '_Generic' specifies two compatible types
```

List the basic types (`long`, `long long`) and let the typedefs fall into them.

### 2.11 Logging macros with `__FILE__`, `__LINE__`, `__func__`

Location information must be captured **at the call site**, which only a macro can do. A production
shape:

```c
#define LOG(lvl, ...)                                                                   \
    do {                                                                                \
        if ((lvl) >= LOG_COMPILED_MIN && (lvl) >= g_runtime_min)                        \
            log_write((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);                \
    } while (0)
```

- The level test comes **before** the arguments are evaluated, so a disabled debug line costs one
  comparison, not a `snprintf` of its arguments.
- `LOG_COMPILED_MIN` is a constant, so lines below it are removed by the compiler entirely
  (build with `-DLOG_COMPILED_MIN=1` to drop debug logging from a release binary).
- `log_write` carries `__attribute__((format(printf, 5, 6)))`, so every `LOG` call is format-checked.
- `log_write` formats the whole line into one buffer and emits it with one call, so lines from
  different threads do not interleave mid-line.
- `__FILE__` is the path as given to the compiler, which may be a long build-machine path. Strip it
  at run time (as the example does), use GCC 12+'s `__FILE_NAME__` (just the file name), or
  `-fmacro-prefix-map=/build/dir/=` to rewrite it at compile time.

### 2.12 When to prefer `static inline` functions and constants

Use a function (`static inline` in a header) whenever it can do the job: arguments are type-checked
and evaluated once, it has a symbol you can break on, it respects scope. Use an `enum` for integer
constants that need to be constant expressions. Note the C-specific catch: `static const int N = 16;`
is **not** a constant expression in C (it is in C++), so it cannot size a file-scope array or be a
`case` label:

```
error: variably modified 'table' at file scope
error: case label does not reduce to an integer constant
```

Use `enum { N = 16 };` or `#define N 16` for those.

Macros remain the right tool only for what functions cannot do: capturing `__FILE__`/`__LINE__`/
`__func__` at the call site; stringising and token pasting (`#`, `##`, X-macros); conditional
compilation; constants used in `#if`; type-generic selection (`_Generic`); and control flow at the
call site (`return` or `goto cleanup` inside a `CHECK` macro).

## 3. Code

### 3.1 The traps: `code/m08_macro_traps.c`

One trap (the multi-statement macro) is caught by GCC, so the file silences that warning around that
single statement with `#pragma GCC diagnostic`, with a comment saying why. Run
`gcc -std=c17 -E -P m08_macro_traps.c | tail -60` to see every expansion.

```
SQUARE_BAD(2 + 3) = 11, SQUARE(2 + 3) = 25
10 * DOUBLE_BAD(3) = 33, 10 * DOUBLE(3) = 60
MAX_BAD(i++, 3) = 6 and i is now 7   (i was incremented twice)
max_int(i++, 3) = 5 and i is now 6
BUMP_BOTH_BAD under a false if: a = 0, b = 1   (b changed anyway)
STR(__LINE__) = __LINE__, XSTR(__LINE__) = 78
CAT(counter_, debit) -> counter_debit = 2
```

### 3.2 X-macro error codes and struct printer: `code/m08_xmacro_error_codes.c`

```c
#define TXN_FIELDS(X)                       \
    X(uint32_t, id,           PRIu32)       \
    X(int64_t,  amount_paise, PRId64)       \
    X(uint32_t, branch,       PRIu32)

struct txn {
#define X(type, name, fmt) type name;
    TXN_FIELDS(X)
#undef X
};

static void txn_print(const struct txn *t)
{
#define X(type, name, fmt) printf("  %-13s = %" fmt "\n", #name, t->name);
    TXN_FIELDS(X)
#undef X
}
```

```
6 error codes:
  0 ERR_OK                   success
  1 ERR_NOT_FOUND            account not found
  2 ERR_INSUFFICIENT_FUNDS   insufficient funds
  3 ERR_ACCOUNT_FROZEN       account is frozen
  4 ERR_DB_UNAVAILABLE       database unavailable, retry later
  5 ERR_PROTOCOL             malformed request
txn:
  id            = 9001
  amount_paise  = -250075
  branch        = 7
```

The count is computed by expanding the table with `X` defined as `+1`, and a `_Static_assert` on it
forces whoever adds an error to look at the documentation.

### 3.3 `_Generic` and `_Static_assert`: `code/m08_generic_and_static_assert.c`

```c
static int    max_i(int a, int b)       { return a > b ? a : b; }
static long   max_l(long a, long b)     { return a > b ? a : b; }
static double max_d(double a, double b) { return a > b ? a : b; }
#define MAX(a, b) _Generic((a) + (b), int: max_i, long: max_l, double: max_d)((a), (b))
```

```
TYPE_NAME('a')                  = int   (character constants are int in C, char in C++)
TYPE_NAME(sizeof(int))          = unsigned long   (size_t on LP64)
TYPE_NAME(small + small)        = int   (uint8_t promoted)
TYPE_NAME(1.0f * 2)             = float
TYPE_NAME("abc")                = char *
TYPE_NAME(1u + 1L)              = long
TYPE_NAME((struct wire_header){0}) = something else
MAX(i++, 2) evaluated i++ once; MAX(2.5, 1.0) = 2.5; MAX(10L, 20L) = 20
```

`"abc"` is `char *`, not `const char *`: in C a string literal has type `char[4]` (modifying it is
still undefined). `1u + 1L` is `long` on LP64 by the usual arithmetic conversions (module 02, rule
4); on Windows it would be `unsigned long`. The file also shows that a side effect must never be
placed inside `assert()`: with `NDEBUG` defined (Release builds) the whole expression disappears.

### 3.4 Logging and CHECK macros: `code/m08_logging_and_check_macros.c`

```c
/* CHECK: on failure, record the code, log the failing expression as text, jump to cleanup.
 * Requires a local `int rc` and a `cleanup:` label in the calling function. */
#define CHECK(cond, code)                                                               \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            rc = (code);                                                                \
            LOG_ERRORF("check failed: %s -> rc=%d", #cond, rc);                         \
            goto cleanup;                                                               \
        }                                                                               \
    } while (0)

static int load_customer_file(const char *path, size_t want_bytes)
{
    int rc = 0;
    FILE *f = NULL;
    char *buf = NULL;

    LOG_INFOF("loading %s (%zu bytes)", path, want_bytes);
    buf = malloc(want_bytes);
    CHECK(buf != NULL, -1);
    f = fopen(path, "rb");
    CHECK(f != NULL, -2);
    CHECK(fread(buf, 1, want_bytes, f) == want_bytes, -3);
    LOG_INFOF("loaded %s", path);

cleanup:
    if (f != NULL) fclose(f);
    free(buf);
    return rc;
}
```

```
INFO  m08_logging_and_check_macros.c:94 main(): server starting, pid file /run/bank.pid
DEBUG m08_logging_and_check_macros.c:96 main(): debug now visible, 8 workers
INFO  m08_logging_and_check_macros.c:77 load_customer_file(): loading /nonexistent/customers.dat (64 bytes)
ERROR m08_logging_and_check_macros.c:81 load_customer_file(): check failed: f != NULL -> rc=-2
load_customer_file returned -2; 4 log lines written
```

`#cond` turns the failing expression into the text `f != NULL` in the log: at 3 AM that tells you
which check failed without opening the source. The first `LOG_DEBUGF` in `main` is filtered at run
time and its arguments are never evaluated. Built with `-DLOG_COMPILED_MIN=1`, the debug line
vanishes from the binary and the program prints 3 log lines (its final `assert` adapts). Verified:
`strings` finds the text "debug now visible" once in the default build and zero times in the
`-DLOG_COMPILED_MIN=1` build, at both `-O0` and `-O2`.

### 3.5 One source, two platforms: `code/m08_platform_conditionals.c`

```c
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define PLAT_NAME "Windows"
#  define PLAT_PATH_SEP '\\'
static void plat_sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned long plat_pid(void) { return (unsigned long)GetCurrentProcessId(); }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  include <time.h>
#  include <unistd.h>
#  define PLAT_NAME "POSIX"
#  define PLAT_PATH_SEP '/'
static void plat_sleep_ms(unsigned ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { /* signal: sleep the remainder */ }
}
static unsigned long plat_pid(void) { return (unsigned long)getpid(); }
#else
#  error "unsupported platform: add a branch to the portability layer"
#endif
```

```
[plat] platform: POSIX, path separator '/'
[plat] long is 64-bit (LP64)
[plat] <threads.h> available: yes
[plat] pid is positive: yes
[plat] slept 20 ms through the portability layer
```

The data-model line comes from `#if LONG_MAX == 2147483647L`: limits from `<limits.h>` can be tested
by the preprocessor, `sizeof` cannot (the preprocessor does not know types). `__has_include(<threads.h>)`
is standard in C23 and an extension in GCC and Clang, so the file first checks
`#if defined(__has_include)`.

## 4. Under the hood (senior-level)

### Translation phases: why a backslash ends your comment

The standard describes eight translation phases. The first four are the preprocessor's, and their
**order** explains several surprises:

1. Map source characters (and, before C23, trigraphs like `??/`).
2. **Line splicing**: every backslash immediately followed by a newline is deleted, joining the two
   lines. This is how multi-line macros work.
3. Tokenise; **replace each comment by one space**.
4. Execute directives and expand macros.

Because splicing (phase 2) happens before comments are removed (phase 3), a `//` comment that ends
with a backslash **swallows the next line**:

```c
    int x = 1;
    // add one to x, see C:\temp\
    x = x + 1;
    printf("%d\n", x);     /* prints 1: the increment became part of the comment */
```

GCC warns (`warning: multi-line comment [-Wcomment]`, in `-Wall`) and MSVC has warning C4010
(single-line comment contains line-continuation character), but only if someone reads the warnings.
Windows paths in comments are exactly where trailing backslashes appear. This is also why the
continuation lines of the X-macro tables in this course contain no `//` comments.

### How macro expansion really works

For a function-like macro, the preprocessor collects the arguments, **fully macro-expands each
argument** on its own (unless the parameter is an operand of `#` or `##`), substitutes, then rescans
the result together with the rest of the file. During rescanning, the name of a macro being expanded
is "painted blue" and not expanded again, which is why macros cannot recurse and why
`#define foo foo + 1` is harmless. The `#` exception to argument pre-expansion is the whole reason
`XSTR` needs two levels.

### The multiple-include optimisation

When GCC or Clang preprocess a header whose entire content is wrapped in
`#ifndef X / #define X / ... / #endif`, they remember the file and the guard macro. A later
`#include` of the same file, with `X` still defined, is skipped without even opening the file. So
include guards cost nothing at build time, and `#pragma once` is not faster on these compilers.

### `_Generic` and lvalue conversion

The controlling expression undergoes lvalue conversion: arrays decay to pointers, functions to
function pointers, and top-level qualifiers are dropped (`const int x` selects `int`). Qualifiers
below the top level remain, so `const char *` and `char *` are different associations. Since the
expression is not evaluated, `_Generic(i++, ...)` does not increment `i`; only the chosen branch's
code runs.

### Seeing what the preprocessor did

- `gcc -E file.c` (MSVC `cl /P file.c`): the full translation unit with line markers.
- `gcc -E -P file.c`: without line markers, easier to read.
- `gcc -dM -E file.c`: every macro defined at the end; `echo | gcc -dM -E -` lists only predefined
  ones (that is how you find `__BYTE_ORDER__` or check `_FORTIFY_SOURCE`).
- `-g3` keeps macro definitions in the debug info: gdb `info macro NAME` and `macro expand EXPR`.

## 5. In production

- **Side effects inside `assert`.** `assert(init_audit_log() == 0);` works in the Debug build and
  silently skips initialisation in the Release build, where `NDEBUG` removes the whole expression.
  The audit log then fails much later. Call the function, store the result, assert on the variable.
- **Windows header macros.** `min`/`max` from `<windows.h>` break code that uses those names
  (`NOMINMAX`); `ERROR` from `wingdi.h` breaks enums (`NOGDI`); `<windows.h>` included before
  `<winsock2.h>` drags in the old `winsock.h` and produces redefinition errors (define
  `WIN32_LEAN_AND_MEAN`, or include `winsock2.h` first).
- **Double evaluation with real side effects.** `MAX(next_sequence(), floor)` consumes two sequence
  numbers; `ABS(read_sensor())` reads twice. Found by reading macro definitions whenever a value is
  "occasionally off by one step". Replace with `static inline` functions.
- **The `#ifdef` branch nobody builds.** The Linux branch of the portability layer was written once
  and never compiled again; the first Linux build of the server fails in forty places. Build all
  supported platforms in CI on every change.
- **Silent `#if` typos.** `#if ENABLE_AUDIT_LOG` where the build defines `ENABLE_AUDIT_LOGS`:
  auditing is off and nothing says so. Compile with `-Wundef` and prefer `#if defined(X)` plus
  `#error` for impossible combinations.
- **Expensive disabled logging.** A logging macro that formats its arguments before checking the
  level spends real CPU on lines nobody sees. Profile shows `vsnprintf` high in a server that "logs
  nothing". Check the level first.
- **Build paths in binaries.** `__FILE__` embeds the build machine's absolute paths in every log
  line and in the binary. Use `__FILE_NAME__` or `-fmacro-prefix-map` to keep logs short and builds
  reproducible.

## 6. Try it

1. **(easy)** Run `gcc -std=c17 -E -P m08_macro_traps.c` and find the expansion of every macro use in
   `main`. Done when you can point at the exact text that makes `MAX_BAD(i++, 3)` increment twice
   and `BUMP_BOTH_BAD` escape the `if`.
2. **(easy)** Write a `SWAP` macro with an internal `tmp`, then break it with `SWAP(tmp, x)`. Fix it
   twice: with GCC's `__typeof__` and a less collision-prone name, and with a `static inline` function
   per type. Done when you can argue which fix you would ship and why.
3. **(medium)** Combine this module with module 07: write one X-macro table of opcodes
   (`X(OP_BALANCE, 1, h_balance, "balance")`) that generates the opcode enum, the name strings for
   logs, and the `const` dispatch table. Done when adding an opcode is one line and a
   `_Static_assert` fails if the table and an `OP_COUNT` disagree.
4. **(medium)** Extend the logging macro: add a monotonic timestamp and the thread id, build the whole
   line in one buffer, and emit it with a single `fwrite`. Done when building with
   `-DLOG_COMPILED_MIN=1` leaves no debug format strings in the binary (check with
   `strings ./prog | grep debug`).
5. **(medium)** Add `plat_mutex` (`CRITICAL_SECTION` on Windows, `pthread_mutex_t` on POSIX) and
   `plat_thread_create` to `m08_platform_conditionals.c`, using the callback-with-context shape of
   module 07. Done when the POSIX branch compiles with `-Werror` and is exercised by a two-thread
   counter test, and you have checked every Windows call against its documentation.
6. **(hard)** Write `PRINT(x)` with `_Generic` covering `int`, `unsigned`, `long`, `unsigned long`,
   `long long`, `unsigned long long`, `double`, `char *` and `const char *`. Then try to add
   `int64_t` and read the error. Done when the macro compiles on LP64, you can explain why it would
   also compile on LLP64, and you have a test that prints one value of each type.

## 7. Interview questions

**Q: What does the preprocessor do, and when does it run?**
It runs before compilation proper, on tokens: it splices continued lines, removes comments, executes
directives (`#include` pastes files, `#define` sets up text replacement, `#if` removes code) and
expands macros. Its output is one translation unit per source file. It knows nothing about types or
scopes, which is the source of most macro bugs.

**Q: Include guards or `#pragma once`?**
Both prevent double inclusion. Guards are standard but fail if two headers share a guard name;
`#pragma once` is supported by all major compilers but can be confused by the same file reached via
different paths. GCC and Clang optimise guards so speed is equal. Pick one and be consistent.

**Q: Why are multi-statement macros wrapped in `do { } while (0)`?**
So the macro is a single statement that requires exactly one trailing semicolon. Without a wrapper
only the first statement is controlled by an `if`; with bare braces, `MACRO(); else` becomes `{...};
else`, a syntax error. The loop runs once and is optimised away.

**Q: What are the dangers of function-like macros, and when would you use a `static inline` function instead?**
Missing parentheses change precedence, arguments with side effects are evaluated multiple times,
local names can capture the caller's names, there is no type checking, and macros cannot be
debugged. Use `static inline` functions for computations; keep macros for call-site information
(`__FILE__`, `__LINE__`), stringising and pasting, conditional compilation and type dispatch.

**Q: What is an X-macro?**
A list defined once as a macro taking another macro, expanded several times with different
definitions of that macro to generate parallel artefacts: an enum, its name strings, messages,
dispatch tables, struct fields and printers. It keeps them in sync by construction.

**Q: What do `#` and `##` do, and why does stringising a macro's value need two levels?**
`#` turns an argument into a string literal, `##` pastes two tokens. Arguments are expanded before
substitution except when used with `#` or `##`, so `STR(__LINE__)` gives `"__LINE__"`; an outer
`XSTR(x)` expands `x` first and then calls `STR`.

**Q: Give uses of `_Static_assert`.**
Checking the size and field offsets of structs that are written to disk or the network, that a table
has as many entries as its enum, that a constant is a power of two, and assumptions such as
`sizeof(void *) == 8`. The build fails with a message instead of the program failing at run time.

**Q: What is the type of `'a'` in C, and how would you show it?**
`int` (in C++ it is `char`). `_Generic('a', char: "char", int: "int")` selects `"int"`, and
`sizeof 'a'` is 4 on common platforms.

**Q: How do you write a logging macro that records file, line and function?**
A variadic macro that passes `__FILE__`, `__LINE__` and `__func__` plus `__VA_ARGS__` (with the
format inside it) to a printf-like function marked with the `format` attribute, wrapped in
`do { } while (0)`, with the level check before the call so disabled lines do not evaluate their
arguments, and a compile-time minimum level so debug lines can be removed entirely.

## 8. Recap

- The preprocessor works on tokens before compilation: `#include` pastes, `#define` replaces, `#if`
  removes. `gcc -E` shows its output.
- Include guards or `#pragma once`, consistently; self-contained headers.
- Parenthesise macro parameters and bodies; beware double evaluation; wrap statements in
  `do { } while (0)`.
- `#` stringises, `##` pastes; use two levels to stringise a macro's value; keep the format inside
  `__VA_ARGS__` for portable variadic macros.
- X-macros generate enums, names, messages and tables from one list.
- Confine platform `#ifdef`s to a small portability layer; `_WIN32` is defined on 64-bit Windows too;
  test `__clang__` before `__GNUC__`; use `-Wundef`.
- `_Static_assert` locks layouts and table sizes at compile time.
- `_Generic` selects by type without evaluating; `'a'` is `int`; `int64_t` is `long` on Linux.
- Logging macros capture `__FILE__`/`__LINE__`/`__func__`, check the level first, and call a
  format-checked function.
- Prefer `static inline` functions and `enum` constants; `static const int` is not a constant
  expression in C.
- Never end a `//` comment with a backslash: line splicing happens before comments are removed.
