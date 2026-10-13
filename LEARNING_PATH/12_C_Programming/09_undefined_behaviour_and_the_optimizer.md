# 09. Undefined behaviour and the optimizer

**Time:** ~3 hours   **Prerequisites:** modules 01 (how C works), 02 (integers and conversions), 03 (pointers)

## 1. In plain words

The C standard is a contract between you and the compiler. You promise to follow its rules.
The compiler promises that your program behaves as written, but only for runs that follow the
rules. When a run breaks a rule, for example a signed `int` overflows or a null pointer is
dereferenced, the standard says the behaviour is **undefined**. It does not mean "the hardware
does whatever it does". It means the contract is void. The compiler owes you nothing for that run.

The surprise is not that broken code crashes. The surprise is that the compiler **uses the rules
as facts while optimising**. If you dereference `p`, the compiler concludes "p is not null, or
the programmer broke the contract", and it deletes your later `if (p == NULL)` check. If you
write `a + b < a`, it concludes "signed addition cannot overflow", and it folds the whole check
to `false`. Your safety check disappears from the binary without a warning.

An analogy: a bank teller who is told "customers never present forged cheques". She does not
check for forgery, because she was told it cannot happen. Every rule you break is a forged
cheque the system was told it would never see. Usually nothing happens. Then one day, after a
compiler upgrade, it does, and only in the release build.

This module teaches the categories, the catalogue of undefined behaviour (UB), real GCC 13
examples you can reproduce, and the flags, warnings and sanitizers that defend you.

## 2. The real thing

### 2.1 Three kinds of "the standard does not say exactly"

| Kind | Meaning | Must the compiler document it? | Examples |
|------|---------|-------------------------------|----------|
| **Implementation-defined** | The compiler picks one behaviour and documents it | Yes | `sizeof(int)`, whether plain `char` is signed, right shift of a negative `int`, converting an out-of-range value to a signed type |
| **Unspecified** | One of several allowed behaviours, chosen silently, may differ per call | No | order of evaluation of function arguments (`f(a(), b())`), whether two identical string literals share memory, the value of padding bytes |
| **Undefined** | No requirements at all, for the whole run | No | signed overflow, null dereference, out-of-bounds access, data race |

The difference matters. Implementation-defined code is portable **if you check the documented
choice** (GCC documents its choices in the "C Implementation-Defined Behavior" chapter of its
manual). Unspecified code is fine as long as every allowed outcome is acceptable to you.
Undefined code is never fine, even when it "works".

Two more facts that interviewers like:

- **UB is a property of a run, not of the source file.** `a + b` is perfectly defined for
  `a = 1, b = 2`. It is undefined only for the inputs that overflow.
- **UB can travel backwards in time.** Because the compiler may assume UB never happens, it can
  reason "the code after this point would be UB if x were 0, so x is not 0", and that conclusion
  can change code that runs *before* the bad operation. You cannot rely on "it printed my log
  line before crashing, so everything before the crash ran as written".

### 2.2 The catalogue

These are the classes you must be able to name, show a one-line example of, and say how to catch.

| UB | One-line example | What the optimiser does with it | Catch it with |
|----|------------------|-----------------------------------|---------------|
| Signed integer overflow | `INT_MAX + 1`, `x * 2` | assumes it never happens: folds overflow checks, widens loop counters | UBSan `signed-integer-overflow`, `-ftrapv`, `-fwrapv` to define it |
| Null pointer dereference | `int n = p->len;` with `p == NULL` | concludes `p != NULL` afterwards, deletes later null checks | UBSan `null`, ASan, the crash itself |
| Out-of-bounds access | `a[4]` for `int a[4]`; also forming `p + 5` for a 4-element array | assumes indexes stay in range: may delete loop exits or return constants | ASan, UBSan `bounds`, `-Warray-bounds` |
| Reading an uninitialised local | `int v; if (c) v = 1; return v;` | picks whatever value is convenient (often the other branch's value) | Valgrind, MSan (clang), `-Wmaybe-uninitialized` (not reliable) |
| Strict-aliasing violation | `*(float *)&some_int = 0.0f;` | assumes an `int *` and a `float *` never point to the same object, keeps stale values in registers | `-Wstrict-aliasing` (at `-O2`), `-fno-strict-aliasing` to switch the assumption off |
| Shift too wide or negative | `1u << 32`, `1 << -1`, `-1 << 1` | assumes the count is in range | UBSan `shift`, `-Wshift-count-overflow` for constants |
| Unsequenced modification | `i = i++ + 1;`, `a[i] = i++;` | any order, or both, or neither | `-Wsequence-point` (in `-Wall`) |
| Data race | two threads, one writes, no lock and no atomic | assumes no other thread writes: hoists loads out of loops, merges stores | TSan |
| Division by zero, `INT_MIN / -1` | `x / 0` | assumes the divisor is valid | UBSan `integer-divide-by-zero`, SIGFPE on x86 |
| Misaligned access | `*(int *)(buf + 1)` | may emit instructions that require alignment (SSE, ARM) | UBSan `alignment` |
| Use after the lifetime ends | using freed memory, a dangling pointer to a local | anything (see 05_Memory_Debugging_RCA/04_the_catalogue_of_memory_bugs.md) | ASan, Valgrind |
| Bad arguments to library functions | `memcpy(dst, NULL, 0)`, overlapping `memcpy` | uses glibc's `nonnull` attributes as facts | UBSan `nonnull-attribute`, ASan |
| Modifying a string literal | `char *s = "abc"; s[0] = 'X';` | literal is in read-only memory: SIGSEGV | the crash; declare `const char *` |

The last few are covered in modules 03, 04 and 05 of this course; this module concentrates on
the ones where **the optimiser turns a latent bug into a different program**.

### 2.3 How the optimiser uses UB, step by step

Compilers do not hunt for UB in order to punish you. They run ordinary optimisations that are
only correct if the rules hold:

1. **Value-range reasoning.** For signed `x`, `x + 1 > x` is "always true" in a world without
   overflow. GCC's constant folder rewrites it to `1`. With overflow defined as wrapping
   (`-fwrapv`) it cannot.
2. **Facts from dereferences and attributes.** After `*p` or after passing `p` to a parameter
   declared `nonnull`, the compiler records "p is not null" and deletes any later test of it.
   GCC's flag for this is `-fdelete-null-pointer-checks`, on by default at `-O2`.
3. **Type-based alias analysis (TBAA).** The strict aliasing rule (C17 6.5p7) says an object may
   only be accessed through its own type, a compatible type, a signed or unsigned variant, or a
   character type. So a store through `float *` cannot change an `int`, and the compiler keeps
   the `int` in a register across it.
4. **Loop analysis.** A loop `for (int i = 0; i <= n; i++)` with a signed counter cannot wrap,
   so it runs exactly `n + 1` times and `i` can be promoted to a 64-bit register without
   sign-extension on every iteration. With `unsigned i` the compiler must allow for wraparound.
   This is the legitimate performance reason signed overflow is UB.

None of these is a bug in the compiler. All of them turn **your** bug into surprising machine code.

### 2.4 The flags that change the contract

| Flag | Effect | Cost |
|------|--------|------|
| `-fwrapv` | Signed overflow wraps (two's complement) and is no longer UB | Loses some loop optimisations |
| `-ftrapv` | Signed overflow aborts at run time | Slow; GCC's implementation is incomplete; prefer UBSan |
| `-fno-strict-aliasing` | Disables TBAA; any pointer may alias any object | Fewer values kept in registers; often a few percent in hot loops (typical, measure yours) |
| `-fno-delete-null-pointer-checks` | Keeps null checks after dereferences | Tiny |
| `-fno-strict-overflow` | In current GCC, the same as `-fwrapv` plus pointer wrapping | As `-fwrapv` |

The Linux kernel builds with `-fno-strict-aliasing`, `-fno-delete-null-pointer-checks` and
`-fno-strict-overflow`: kernel code does type punning and low-address tricks on purpose, and
its maintainers decided the safety was worth the speed. These flags are a **safety net for
legacy code**, not a licence to write new UB. Code that needs them is still non-portable: MSVC
and Clang have their own, different sets of switches.

### 2.5 Tools: warnings first, sanitizers second

Warnings catch the forms that are visible in one function. This is real GCC 13 output for a file
with four classic mistakes (`gcc -std=c17 -Wall -Wextra -O2 -c w1.c`):

```text
w1.c:2:22: warning: operation on 'i' may be undefined [-Wsequence-point]
    2 | int unseq(int i) { i = i++ + 1; return i; }
w1.c:3:41: warning: 'sizeof' on array function parameter 'a' will return size of 'int *' [-Wsizeof-array-argument]
    3 | size_t param(int a[10]) { return sizeof a; }
w1.c:4:28: warning: dereferencing type-punned pointer will break strict-aliasing rules [-Wstrict-aliasing]
    4 | float pun(int x) { return *(float *)&x; }
```

The fourth mistake was `int maybe(int c) { int v; if (c) v = 1; return v; }`. GCC 13 printed
**no warning** for it at `-O2`: the optimiser had already decided that `v` is `1` on both paths
(it is allowed to, because reading the uninitialised `v` is UB) and then had nothing left to warn
about. Lesson: `-Wmaybe-uninitialized` is useful but not a proof. Valgrind and MSan are the tools
for uninitialised reads.

Sanitizers check at run time, on the inputs you actually run:

| Sanitizer | Flag | Catches | Overhead (typical) |
|-----------|------|---------|-------------------|
| UBSan | `-fsanitize=undefined` | overflow, shifts, null, bounds (static arrays), alignment, bad `nonnull` args, invalid `bool`/enum loads | small, usable in test builds |
| ASan | `-fsanitize=address` | out-of-bounds heap/stack/global, use-after-free, double free, leaks | about 2x CPU, 2-3x memory |
| TSan | `-fsanitize=thread` | data races, some lock-order problems | 5-15x CPU |
| MSan | `-fsanitize=memory` (clang only) | uninitialised reads | about 3x |

Make UBSan fatal in CI with `-fno-sanitize-recover=all`, otherwise it prints and carries on and a
test that "passed" may have hit UB. The deep dive on all sanitizers is
05_Memory_Debugging_RCA/08_valgrind_and_sanitizers.md; the flag reference is
06_Linux_Toolchain_DevOps/03_gcc_and_clang_flags_that_matter.md.

## 3. Code

Four programs in `code/`. Each says in its header comment that it contains deliberate UB, gives
the exact build line, and still compiles cleanly with
`gcc -std=c17 -Wall -Wextra -Wpedantic -Werror`. The UB functions are marked
`__attribute__((noinline))` and fed through `volatile` variables so the optimiser cannot fold
the whole demo at compile time.

### 3.1 The overflow check that disappears (`m09_overflow_check.c`)

```c
/* WRONG: by the time we compare, the overflow (UB) has already happened. */
__attribute__((noinline)) bool naive_add_overflows(int a, int b)
{
    return b > 0 && a + b < a;
}

/* RIGHT 1: compare against the limit BEFORE adding. Works on every compiler. */
static bool add_overflows_precheck(int a, int b)
{
    if (b > 0) return a > INT_MAX - b;   /* INT_MAX - b cannot overflow when b > 0 */
    if (b < 0) return a < INT_MIN - b;   /* INT_MIN - b cannot overflow when b < 0 */
    return false;
}

/* RIGHT 2: let the compiler do it. C23 standardises this as ckd_add() (GCC 14+). */
static bool add_overflows_builtin(int a, int b, int *sum)
{
    return __builtin_add_overflow(a, b, sum);
}

/* RIGHT 4: the size_t check you need before every malloc(count * size). */
static bool mul_size_overflows(size_t count, size_t size)
{
    return size != 0 && count > SIZE_MAX / size;
}
```

Real results for `naive_add_overflows(INT_MAX, 1)` with GCC 13.3 on x86-64:

| Build | Result | Why |
|-------|--------|-----|
| `-O0` | `0` | folded even without optimisation: GCC's front end rewrites `a + b < a` to `b < 0` |
| `-O2` | `0` | same, and the function body becomes `xorl %eax,%eax; ret` |
| `-O2 -fwrapv` | `1` | overflow is now defined as wrapping, so the fold is not allowed |
| `-O0 -g -fsanitize=undefined` | `1` plus a report | UBSan instruments the add, which blocks the fold |

The `-O2` assembly, from `gcc -std=c17 -O2 -S -o - m09_overflow_check.c`:

```text
naive_add_overflows:
	endbr64
	xorl	%eax, %eax
	ret
```

The UBSan report:

```text
m09_overflow_check.c:30:23: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
```

The point to remember: **"it fails at -O2 but works at -O0" is a common sign of UB, but not a
reliable test.** Here even `-O0` was affected.

### 3.2 The null check that disappears (`m09_null_check_deleted.c`)

```c
/* The CVE-2009-1897 shape (Linux tun driver): use the pointer first, check it later. */
__attribute__((noinline)) int record_len(const struct record *r)
{
    int n = r->len;            /* line A: if r were NULL, this is already UB ... */
    if (r == NULL)             /* line B: ... so GCC deletes this at -O2         */
        return -1;
    return n;
}

/* A version whose deletion you can SEE at run time without crashing. */
__attribute__((noinline)) int copy_then_check(char *dst, const char *src, size_t n)
{
    memcpy(dst, src, n);       /* UB if src == NULL, even with n == 0 */
    if (src == NULL)           /* GCC: "src went to a nonnull parameter, so it is not NULL" */
        return -1;
    return 0;
}
```

The assembly of `record_len` at `-O2`, then with `-fno-delete-null-pointer-checks`:

```text
record_len:                         record_len:
	endbr64                             endbr64
	movl	(%rdi), %eax                testq	%rdi, %rdi
	ret                                 movl	$-1, %eax
	                                    cmovne	(%rdi), %eax
	                                    ret
```

`copy_then_check(dst, NULL, 0)` at run time:

| Build | Result |
|-------|--------|
| `-O0` | `-1` (check survived) |
| `-O2` | `0` (check deleted) |
| `-O2 -fno-delete-null-pointer-checks` | `-1` |
| UBSan | `-1`, plus `runtime error: null pointer passed as argument 2, which is declared to never be null` |

`memcpy(dst, NULL, 0)` "works" on every C library you will meet, which is exactly why this bug
survives code review. The fix is boring: validate first, and skip the call when `n == 0`.

### 3.3 Strict aliasing (`m09_strict_aliasing.c`)

```c
__attribute__((noinline)) int store_both(int *i, float *f)
{
    *i = 1;
    *f = 0.0f;       /* a float store "cannot" change an int ... */
    return *i;       /* ... so at -O2 this returns the cached 1 */
}
```

Called as `store_both(&x, (float *)&x)`:

```text
-O0                       store_both returned 0, x now holds bits 0x00000000
-O2                       store_both returned 1, x now holds bits 0x00000000
-O2 -fno-strict-aliasing  store_both returned 0, x now holds bits 0x00000000
```

At `-O2` the function returns 1 while memory holds 0. That is the shape of the real bug: a value
in a register disagrees with memory. The three legal ways to reinterpret bytes:

```c
uint32_t u; memcpy(&u, &f, sizeof u);                    /* 1. memcpy: compiles to one move */
union { float f; uint32_t u; } pun = { .f = f };         /* 2. union: legal in C, not in C++ */
unsigned char first = *(const unsigned char *)&one;      /* 3. character types may alias anything */
```

The same file parses a wire header that sits at an odd offset in a buffer with `memcpy` into a
local struct, which is also the cure for misaligned access.

### 3.4 Let UBSan name each class (`m09_ubsan_catalogue.c`)

With no argument it runs only safe helpers (`checked_add`, `safe_shl`, `safe_div`, `safe_get`).
With an argument it triggers one class. Real output of the UBSan build
(`gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g -fsanitize=undefined ...`, then `./m09_ubsan_catalogue_ub all`):

```text
--- add
m09_ubsan_catalogue.c:34:56: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
    returned -2147483648
--- mul
m09_ubsan_catalogue.c:35:56: runtime error: signed integer overflow: 2147483647 * 2 cannot be represented in type 'int'
    returned -2
--- negate
m09_ubsan_catalogue.c:36:46: runtime error: negation of -2147483648 cannot be represented in type 'int'; cast to an unsigned type to negate this value to itself
    returned -2147483648
--- shift_wide
m09_ubsan_catalogue.c:37:53: runtime error: shift exponent 32 is too large for 32-bit type 'unsigned int'
    returned 1
--- shift_neg
m09_ubsan_catalogue.c:38:48: runtime error: shift exponent -1 is negative
    returned -2147483648
--- shift_lneg
m09_ubsan_catalogue.c:39:55: runtime error: left shift of negative value -1
    returned -2
--- bounds
m09_ubsan_catalogue.c:43:15: runtime error: index 4 out of bounds for type 'int [4]'
m09_ubsan_catalogue.c:43:15: runtime error: load of address 0x7fff75a10040 with insufficient space for an object of type 'int'
    returned 1973485664
--- misaligned
m09_ubsan_catalogue.c:49:12: runtime error: load of misaligned address 0x7fff75a10051 for type 'int', which requires 4 byte alignment
    returned 0
```

(The hex-dump lines UBSan prints under the bounds and alignment reports are left out; addresses
and the garbage value from `bounds` change from run to run.)

Look at `shift_wide`: `1u << 32` returned `1`, not `0`. The x86 `shl` instruction masks the count
to 5 bits, so the hardware computed `1 << 0`. Another CPU, or a constant the compiler folds, gives
`0`. "What the hardware does" is not a definition. The cases that trap after the report:

```text
$ ./m09_ubsan_catalogue_ub null
m09_ubsan_catalogue.c:54:12: runtime error: load of null pointer of type 'int'
Segmentation fault (core dumped)
$ ./m09_ubsan_catalogue_ub div0
m09_ubsan_catalogue.c:56:49: runtime error: division by zero
Floating point exception (core dumped)
```

Build with `-fno-sanitize-recover=all` and the first report ends the run with exit status 1,
which is what a CI job needs.

## 4. Under the hood (senior-level)

**Where in GCC each example happened.** The overflow fold is done by `fold-const.cc` while the
front end builds the expression tree, which is why it appears even at `-O0`. The null-check
deletion is value-range propagation (the `tree-vrp` and related passes) using "non-null" facts
derived from dereferences and from `__attribute__((nonnull))` on glibc's `memcpy` declaration.
GCC goes further with `-fisolate-erroneous-paths-dereference` (on at `-O2`): if a path is certain
to dereference null, it replaces that path with a trap instruction (`ud2`), so you get SIGILL
instead of SIGSEGV. The aliasing result comes from type-based alias analysis: every type gets an
alias set, and loads and stores from disjoint sets are allowed to be reordered or forwarded.
`char`, `signed char` and `unsigned char` have alias set 0, which conflicts with everything.

**Why `int` loop counters are faster.** On x86-64 an `int` index must be sign-extended to 64
bits before it can be used in an address. If `i` could wrap, the compiler would have to redo
that extension every iteration. Because signed overflow is UB, GCC can compute the index in a
64-bit register once ("induction variable widening") and vectorise the loop knowing its exact
trip count. With `-fwrapv` or an `unsigned` counter that can wrap, some of these transformations
are lost. This is the honest argument for why the language keeps signed overflow undefined.

**How UBSan works.** It is compile-time instrumentation, not a library trick. For `a + b` with
`-fsanitize=signed-integer-overflow`, GCC emits the add with an overflow-flag check and, on
overflow, calls `__ubsan_handle_add_overflow` with a static descriptor holding the file, line,
column and type. That is why the report is so precise, and why instrumented code is not folded
the same way: the instrumentation itself uses the result. `-fsanitize-trap=undefined` replaces
the handler calls with a trap instruction and needs no runtime library, which makes it usable in
constrained environments.

**LLVM's model, for completeness.** Clang represents "this value came from UB" as `poison`.
Poison propagates through arithmetic and only becomes true UB when it reaches something that
matters (a branch, a memory address, a call). That model explains why a compiler can delete
code *before* the bad operation: the branch that depends on poison may be resolved either way.

**The data-race rule is the same idea.** C11 5.1.2.4 makes a data race UB so the compiler may
keep a shared variable in a register inside a loop. A `while (!stop) {}` loop on a plain
`int stop` may legally become an infinite loop that never reloads `stop`. `volatile` stops that
particular reload problem but still leaves a race; `_Atomic` is the fix (module 14).

## 5. In production

- **MSVC to GCC ports expose UB that "worked for years".** MSVC does not perform type-based
  alias analysis, so code that casts `char` buffers to `struct` pointers or punns `int` and
  `float` usually behaves as written there. Built with GCC or Clang at `-O2` on Linux, the same
  code can read stale values. If you port parts of a Win32 server to Linux, either fix the
  punning with `memcpy` or build those files with `-fno-strict-aliasing` until you have.
- **Compiler upgrades are a risk event.** Each GCC release learns new facts to use. A length
  check like `if (offset + len < offset)` on signed values can pass review and testing for years
  and silently vanish after an upgrade. Run the test suite under UBSan before and after any
  compiler change, and keep `-Wall -Wextra -Werror` on.
- **"Only fails in the release build" playbook.** (1) Rebuild the release flags plus
  `-fsanitize=undefined,address` and rerun the failing case. (2) If that is clean, toggle the
  contract flags one at a time: if `-fno-strict-aliasing` makes it disappear, hunt for punning;
  if `-fwrapv` does, hunt for overflow; if `-fno-delete-null-pointer-checks` does, look for a
  check after a use. (3) Bisect the optimisation level per file (`#pragma GCC optimize` or
  per-object flags) to find which translation unit is affected. Record the finding in the RCA
  as "latent UB exposed by optimisation", not "compiler bug": it almost never is the compiler.
- **Untrusted lengths make UB a security problem.** In a banking server, lengths come from
  network messages and files. Signed overflow in a length check, or a size multiplication that
  wraps, is the first step of a remotely triggered memory corruption. Do size arithmetic in
  `size_t` with the `SIZE_MAX / size` check from 3.1, or with `__builtin_mul_overflow`.
- **Do not "fix" UB with `volatile` or by lowering the optimisation level.** That hides the
  symptom on today's compiler. Fix the operation, then add the UBSan run to CI so it stays fixed.

## 6. Try it

1. **Easy - classify.** For each, say implementation-defined, unspecified or undefined, with one
   line of reasoning: `sizeof(long)`; `f(g(), h())` call order; `-1 >> 1`; `INT_MAX + 1`;
   `(int)3000000000u`; `char *s = "hi"; s[0] = 'H';`. Done when all six are classified and you
   can name the clause of the contract each one breaks or relies on.
2. **Easy - reproduce.** Build `m09_overflow_check.c` four ways (`-O0`, `-O2`, `-O2 -fwrapv`,
   UBSan) and fill in the table from 3.1 yourself. Done when your numbers match and you can
   explain the `-O0` result.
3. **Medium - see the deletion.** Generate assembly for `m09_null_check_deleted.c` at `-O2` with
   and without `-fno-delete-null-pointer-checks`. Done when you can point at the instruction that
   holds the null check in one listing and say why it is missing from the other.
4. **Medium - aliasing in a parser.** Write `uint32_t read_be32(const unsigned char *p)` three
   ways: pointer cast plus `ntohl`, `memcpy` plus `ntohl`, and shifts. Call each on `buf + 1`.
   Build with `-O2 -fsanitize=undefined`. Done when you can say which version UBSan reports and
   why, and which one is portable without `ntohl`.
5. **Hard - the loop that never ends.** Write a loop `for (int i = 1; i > 0; i += i)` that counts
   iterations. Build at `-O0` and `-O2`, run each with `timeout 2`. Explain the difference using
   section 2.3. Done when you have the two outputs (or the timeout) and a three-sentence
   explanation.
6. **Hard - audit legacy code.** Take any C file you know from work (or `m15_legacy_report.c`)
   and build it with `-O2 -Wall -Wextra -Wstrict-aliasing=2 -fsanitize=undefined`. Run its
   tests. Done when you have a list of every warning and report with a one-line verdict each:
   real UB, false positive, or implementation-defined.

## 7. Interview questions

**Q: What is the difference between undefined, unspecified and implementation-defined behaviour?**
A: Implementation-defined behaviour is a choice the compiler must document, such as the size of
`int` or whether `char` is signed. Unspecified behaviour is one of several allowed outcomes
chosen without documentation, such as the order in which function arguments are evaluated.
Undefined behaviour has no requirements at all: the standard places no constraint on the whole
run, and the optimiser may assume it never happens.

**Q: Why might `if (a + b < a)` fail to detect overflow?**
A: For signed `int`, overflow is undefined, so the compiler may assume `a + b` does not overflow
and simplify `a + b < a` to `b < 0`. With `b > 0` the check becomes constant false; GCC 13 does
this even at `-O0`. Check before adding (`a > INT_MAX - b`) or use `__builtin_add_overflow` or
C23 `ckd_add`.

**Q: How can the compiler remove a null check?**
A: If the pointer was already dereferenced, or passed to a function whose parameter is declared
`nonnull` such as glibc's `memcpy`, then a null value would already have caused UB, so the
compiler infers the pointer is non-null and deletes later tests. This was the root of the 2009
Linux tun driver exploit. Check before use, or build legacy code with
`-fno-delete-null-pointer-checks`.

**Q: What is the strict aliasing rule and how do you type-pun legally?**
A: An object may only be accessed through an lvalue of its own (compatible) type, a signed or
unsigned variant, a qualified version, an aggregate containing it, or a character type. Writing
an `int` through a `float *` breaks it, and GCC at `-O2` may keep the old `int` in a register.
Pun with `memcpy` (portable, compiles to a register move), with a union in C (not in C++), or
read the bytes through `unsigned char *`.

**Q: Your service works in debug and fails in release. How do you investigate?**
A: Treat it as likely UB or a race exposed by optimisation. Rebuild release flags with UBSan and
ASan and rerun the failing input; for threads, run TSan. If the sanitizers are quiet, toggle
`-fno-strict-aliasing`, `-fwrapv` and `-fno-delete-null-pointer-checks` one at a time to find
the class, then narrow the translation unit by building files at different optimisation levels.

**Q: Is `volatile` a fix for undefined behaviour?**
A: No. `volatile` forces every access to happen in memory, which suppresses some optimisations,
but it does not make signed overflow, aliasing violations or data races defined. It is meant for
memory-mapped hardware and for `volatile sig_atomic_t` flags set by signal handlers. Fix the
operation instead.

**Q: What does `-fsanitize=undefined` do, and why add `-fno-sanitize-recover=all` in CI?**
A: It instruments operations such as signed arithmetic, shifts, array indexing and pointer
alignment with checks that call a runtime handler on failure and print the exact file, line and
values. By default it prints and continues, so a test can pass while hitting UB;
`-fno-sanitize-recover=all` makes the first report abort the run so CI fails.

## 8. Recap

- Implementation-defined = documented choice; unspecified = undocumented choice among allowed
  outcomes; undefined = no requirements for the whole run.
- The optimiser treats the rules as facts: no signed overflow, no null dereference, no
  out-of-bounds access, no aliasing across unrelated types, no data races.
- Real GCC 13 results: the naive overflow check folds to `false` (even at `-O0`); a null check
  after `memcpy(dst, NULL, 0)` is deleted at `-O2`; an `int` written through `float *` reads back
  stale at `-O2`.
- Correct overflow checks compare against the limit before the operation, or use
  `__builtin_add_overflow` / C23 `ckd_add`; size products use `count > SIZE_MAX / size`.
- Pun types with `memcpy`, a union (C only) or `unsigned char *`; never by casting pointers.
- `-fwrapv`, `-fno-strict-aliasing` and `-fno-delete-null-pointer-checks` change the contract;
  they are a safety net for legacy code, not a fix.
- Warnings catch local forms (`-Wsequence-point`, `-Wstrict-aliasing`,
  `-Wsizeof-array-argument`), but they are not proof; GCC 13 did not warn on an obvious
  uninitialised read.
- UBSan for arithmetic and pointer UB, ASan for memory, TSan for races, Valgrind or MSan for
  uninitialised reads; `-fno-sanitize-recover=all` in CI.
- A compiler upgrade or an MSVC-to-GCC port can turn latent UB into a production incident.
