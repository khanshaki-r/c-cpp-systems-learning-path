# 01. How C really works: from source files to a running program

**Time:** ~2.5 hours   **Prerequisites:** none (you have written and fixed C before)

## 1. In plain words

A C program is a set of text files. The compiler never sees "the program". It sees **one `.c`
file at a time**, after the preprocessor has pasted every `#include`d header into it. It turns
that one file into an **object file**: machine code with holes in it wherever the code mentions
something defined in another file. The **linker** then takes all the object files plus the
libraries, fills every hole with a real address, and writes the executable.

Analogy: a book written by several authors. Each author (a `.c` file) writes one chapter alone,
with only a shared glossary (the headers) to know what the other chapters contain. A translator
(the compiler) translates each chapter separately and leaves notes like "see chapter 7, page ?"
wherever a chapter refers to another. The editor (the linker) binds the chapters together and
fills in every "page ?". If chapter 3 refers to a section that nobody wrote, the editor, not the
translator, is the one who complains: that is your "unresolved external symbol".

The C standard does not describe your CPU. It describes an imaginary **abstract machine** and
says which results your program must produce. The compiler may generate any machine code that
gives the same observable results. That freedom is why optimised code is fast, and why code
that breaks the rules (undefined behaviour) can "work in Debug and crash in Release". Your
maintenance work on the Win32 server has almost certainly hit bugs of exactly these kinds:
link errors after a header change, a global defined twice, a crash that only appears in the
optimised build. This module gives you the model that explains all of them.

## 2. The real thing

### 2.1 The abstract machine and the as-if rule

The standard (ISO/IEC 9899) defines the meaning of a program in terms of an abstract machine
that executes every expression exactly as written. A real compiler must only preserve the
**observable behaviour** of that machine (C17 5.1.2.3):

- accesses to `volatile` objects happen exactly as written, in order;
- data written to files is what the abstract machine would have written when the program ends;
- input and output on interactive devices happens in the right order.

Everything else is fair game: the compiler may keep variables in registers, delete stores nobody
reads, reorder independent computations, inline functions, or compute a loop's result at compile
time. This is the **as-if rule**. It is why a debugger shows "optimised out" for a variable at
`-O2`, and why stepping through Release code jumps around.

The standard sorts "not fully specified" behaviour into four buckets. Interviewers ask for the
difference, with examples:

| Kind | Meaning | Examples |
|---|---|---|
| Implementation-defined | The compiler picks, and **must document** its choice | `sizeof(int)`, whether plain `char` is signed, result of `-8 >> 1`, converting an out-of-range value to a signed type |
| Unspecified | The compiler picks among allowed options, need not document, may differ call to call | order of evaluation of function arguments and of the operands of `+` |
| Undefined (UB) | The standard places **no requirement** at all | signed overflow, out-of-bounds access, null dereference, use after free, data race, `i = i++ + 1` |
| Locale-specific | Depends on the runtime locale | what `isalpha(0xE9)` returns |

Undefined behaviour is not "random". It means the compiler is allowed to **assume it never
happens** and optimise on that assumption. Module 02 shows GCC deleting an overflow check
because signed overflow "cannot happen". Module 09 of this course,
09_undefined_behaviour_and_the_optimizer.md, is dedicated to UB.

### 2.2 Sequencing: when side effects happen

A **side effect** is a change to an object (assignment, `++`, writing a file). C11 replaced the
old term "sequence point" with **sequenced before**: if one evaluation is sequenced before
another, its side effects are complete before the other starts. The guarantees you rely on:

- `&&`, `||`, `?:` and the comma operator: the left operand is fully evaluated (side effects
  included) before the right one. `&&` and `||` skip the right side when the result is known.
- A full expression (a statement's expression, an `if` condition, each part of a `for`) is
  complete before the next one starts.
- All arguments are evaluated before the function body runs. Their order among themselves is
  **unspecified**. Two function calls never interleave.

If two side effects on the same object are **unsequenced** (or a side effect and a read of the
same object), the behaviour is undefined. GCC catches the obvious cases with `-Wall`:

```
w5.c:1:25: warning: operation on 'i' may be undefined [-Wsequence-point]
    1 | int k(int i) { return i = i++ + 1; }
```

### 2.3 The four stages of a build

```
 foo.c ──► [preprocessor] ──► foo.i ──► [compiler] ──► foo.s ──► [assembler] ──► foo.o ─┐
 bar.c ──► ...                                                          bar.o ─┤
                                         libc.so / libm.a / your .a ───────────┤
                                                                               ▼
                                                                         [linker] ──► prog
```

| Stage | What it does | GCC | MSVC |
|---|---|---|---|
| Preprocess | runs `#include`, `#define`, `#if`; strips comments; output is one big C file | `gcc -E foo.c` (or `-save-temps` keeps `foo.i`) | `cl /P foo.c` (writes `foo.i`) |
| Compile | parses C, type-checks, optimises, emits assembly | `gcc -S foo.c` | `cl /FA foo.c` (listing) |
| Assemble | assembly to an object file with a symbol table and relocations | `gcc -c foo.c` | `cl /c foo.c` (gives `foo.obj`) |
| Link | joins objects and libraries, resolves symbols, writes the executable | `gcc foo.o bar.o -o prog` | `link foo.obj bar.obj` |

`gcc -save-temps -c m01_eval_order.c` leaves all the intermediate files, so you can look at each:

```
m01_eval_order.c
m01_eval_order.i
m01_eval_order.o
m01_eval_order.s
```

### 2.4 Translation units

A **translation unit** (TU) is one `.c` file after preprocessing: the file itself plus every
header it includes, recursively. The compiler handles exactly one TU at a time and knows nothing
about the others. Everything it needs to know about a function or global defined elsewhere must
come from a **declaration** in this TU, usually from a header.

`m01_eval_order.c` is 63 lines. After preprocessing it is 788 lines (317 without the line
markers), almost all of it from `<assert.h>`, `<stdio.h>` and `<string.h>`. `gcc -H` prints
the include tree, one dot per nesting level:

```
. /usr/include/assert.h
.. /usr/include/features.h
... /usr/include/features-time64.h
. /usr/include/stdio.h
.. /usr/include/x86_64-linux-gnu/bits/libc-header-start.h
.. /usr/lib/gcc/x86_64-linux-gnu/13/include/stddef.h
```

This is why a change to a widely included header rebuilds the whole project, and why MSVC
projects use precompiled headers (`stdafx.h`, `pch.h`).

### 2.5 Declarations and definitions

- A **declaration** introduces a name and its type: "there is an `int` called `g_requests`
  somewhere", "there is a function `next_id` taking nothing and returning `int`".
- A **definition** is a declaration that also creates the thing: it reserves storage for an
  object, or provides the body of a function.

```c
extern int g_requests;     /* declaration only: no storage */
int next_id(void);         /* declaration (a prototype): no body */

int g_requests = 0;        /* definition: storage reserved here */
int next_id(void) { ... }  /* definition: the body */
```

The rule (C17 6.9p5): every object or function with external linkage that the program uses must
have **exactly one** definition in the whole program. Declarations can repeat as often as you
like, as long as they agree.

**Tentative definitions.** At file scope, `int counter;` with no initializer and no `extern` is a
*tentative* definition. Within one TU it becomes a real definition (zero-initialised) if no other
definition appears. Across TUs, two files that both say `int counter;` used to be merged silently
by the linker as "common" symbols. GCC 10 and later (and Clang 11 and later) default to
`-fno-common`, so the same code now fails:

```
/usr/bin/ld: /tmp/ccsbNaen.o:(.bss+0x0): multiple definition of `counter'; /tmp/ccauhZNV.o:(.bss+0x0): first defined here
collect2: error: ld returned 1 exit status
```

With `-fcommon` it links again. Legacy C written for older compilers (and for MSVC, which still
accepts the pattern for C) often contains this. The fix is the proper pattern: `extern int
counter;` in the header and `int counter;` in exactly one `.c` file.

### 2.6 Headers versus source files

A header is a contract that several TUs include. Put in it only things that may appear in many
TUs without creating duplicate definitions:

| In a header | In a `.c` file |
|---|---|
| function prototypes | function definitions |
| `extern` declarations of globals | the one definition of each global |
| `struct`, `union`, `enum`, `typedef` definitions | `static` helpers and `static` file-scope data |
| macros | anything that is private to this file |
| `static inline` functions (each TU gets its own copy) | |

Two rules that prevent most header pain:

1. **Every header is self-contained**: it includes what it uses, so it compiles when it is the
   first and only include. Check it by making `foo.h` the first `#include` in `foo.c`.
2. **Every header has an include guard** (module 08), so including it twice in one TU is harmless.

### 2.7 Linkage

Linkage decides whether the same name in different places refers to the same thing.

| Linkage | Meaning | How you get it |
|---|---|---|
| external | the name refers to the same object or function across all TUs | file-scope declaration without `static`; `extern` |
| internal | the name is private to its TU; another TU can reuse the name for something else | `static` at file scope |
| none | each declaration is a separate entity | locals, function parameters, `typedef` names |

`static` means two unrelated things depending on where it is (module 04 covers both in full):

- at **file scope**: internal linkage (private to the file);
- at **block scope**: static storage duration (one object that lives for the whole program).

Make every file-scope function and global `static` unless another file really needs it. Fewer
external names means fewer clashes, faster links, and better optimisation (the compiler knows
every caller of a `static` function, so it can inline it or drop it).

### 2.8 Object files and symbols

An object file contains machine code with **relocations**: placeholders that say "put the address
of symbol X here". It also has a **symbol table** listing what it defines and what it needs.
`nm` prints the table (`dumpbin /symbols` on Windows). From the split build in section 3:

```
unit_a.o:
0000000000000000 t bump
0000000000000000 B g_requests
000000000000001d T next_id
0000000000000000 d s_next
0000000000000045 T unit_a_name

unit_b.o:
0000000000000065 r __PRETTY_FUNCTION__.0
                 U __assert_fail
                 U g_requests
0000000000000000 T main
                 U next_id
                 U printf
                 U unit_a_name
```

| Letter | Meaning |
|---|---|
| `T` / `t` | defined in `.text` (code). Upper case = external linkage, lower case = internal (`static`) |
| `D` / `d` | defined in `.data` (initialised data) |
| `B` / `b` | defined in `.bss` (zero-initialised data: takes no space in the file) |
| `R` / `r` | defined in `.rodata` (read-only data) |
| `U` | **undefined here**: used by this object, must be defined by another object or a library |

Notice `g_requests = 0` is in `.bss`, not `.data`: zero-initialised data costs nothing in the
file. Notice also that `bump` and `s_next` are lower case: `static` turned them into internal
symbols that the linker will never match against another file.

Two link errors you will meet forever, in both toolchains:

| Problem | GNU ld | MSVC link.exe |
|---|---|---|
| a name is used but never defined | `undefined reference to 'next_id'` | `LNK2019: unresolved external symbol` |
| a name is defined in two objects | `multiple definition of 'counter'` | `LNK2005: already defined in foo.obj` |

The linker works on names only. In C a symbol carries **no type information**, so if one file
declares `extern long g_requests;` and another defines `int g_requests;`, nothing complains; the
first file simply reads 8 bytes from a 4-byte object. (C++ mangles function parameter types into
the symbol name, which catches the function case; see
02_Modern_CPP/01_from_c_to_cpp.md.) For libraries, shared objects, link order and the dynamic
loader, see 06_Linux_Toolchain_DevOps/02_how_a_cpp_program_is_built.md: the mechanics are
identical for C.

### 2.9 The C standards

| Standard | `__STDC_VERSION__` | What it added that you use daily |
|---|---|---|
| C89 / C90 | (not defined) | prototypes, `void`, `const`, `volatile`, the standard library |
| C95 | `199409L` | wide-character functions, digraphs |
| C99 | `199901L` | `//` comments, declarations anywhere, `for (int i...)`, `long long`, `<stdint.h>`, `<stdbool.h>`, `inline`, `restrict`, designated initializers, compound literals, flexible array members, variadic macros, `snprintf`, `__func__`, VLAs |
| C11 | `201112L` | `_Generic`, `_Static_assert`, `_Alignas`/`_Alignof`, `_Noreturn`, `_Thread_local`, `<threads.h>`, `<stdatomic.h>` and a memory model, anonymous structs and unions, `aligned_alloc`; VLAs became optional; `gets` removed |
| C17 | `201710L` | bug fixes only, no new features. **This course targets C17.** |
| C23 | `202311L` | `nullptr`, `bool`/`true`/`false`/`static_assert` as keywords, `typeof`, `constexpr` objects, `[[attributes]]`, `#embed`, binary literals, `<stdckdint.h>` (checked arithmetic), `strdup` in the standard, two's complement required, `()` now means "no parameters" |

GCC 13 defaults to `-std=gnu17` (C17 plus GNU extensions). Use `-std=c17 -Wpedantic` to stay
inside the standard; that is what every program in this course is built with.

### 2.10 MSVC C versus GCC C: the differences you will hit

You have spent 2.5 years in MSVC. These are the differences that bite when the same C code moves
to GCC on Linux, or when you explain your Windows experience in an interview:

| Topic | MSVC | GCC on Linux |
|---|---|---|
| Standard support | historically C89 plus selected C99 library pieces. `/std:c11` and `/std:c17` since VS 2019 16.8; recent VS 2022 releases added `<threads.h>` and experimental atomics | full C17, most of C23 by GCC 14 |
| `__STDC_VERSION__` | defined only with `/std:c11` or later | always (C99 and later) |
| VLAs | never supported (`__STDC_NO_VLA__`) | supported |
| `long` | 32 bits (LLP64) | 64 bits (LP64). Module 02. |
| `snprintf` | real C99 `snprintf` only since VS 2015. Before that, `_snprintf`, which does **not** NUL-terminate on truncation and returns -1 | standard |
| `printf("%zu")` | supported since VS 2015 (UCRT); older code uses `%Iu` | standard |
| "Safe" functions | `strcpy_s`, `strtok_s`, `localtime_s`, and warning C4996 for `strcpy` unless `_CRT_SECURE_NO_WARNINGS` | glibc does not provide them |
| Same name, different signature | MSVC `localtime_s(struct tm *, const time_t *)` vs C11 Annex K `localtime_s(const time_t *, struct tm *)`; MSVC `strtok_s` has 3 parameters, Annex K has 4 | POSIX `localtime_r`, `strtok_r` |
| POSIX names | `strdup` deprecated in favour of `_strdup`, `fileno` as `_fileno` | standard POSIX names |
| `.c` vs `.cpp` | `.c` compiled as C unless `/TP`. Many "C" Windows code bases are really compiled as C++ | by extension, same idea |
| Debug fill patterns | Debug CRT fills new heap blocks with `0xCD`, freed blocks with `0xDD`, guard bytes with `0xFD`; `/RTC1` fills uninitialised stack with `0xCC` | no fill unless you ask (`-ftrivial-auto-var-init=pattern`, GCC 12+) |
| Missing prototype | warning C4013 | GCC 13: warning (`-Wimplicit-function-declaration`); GCC 14: error |
| Extensions | `__int64`, `__declspec(...)`, `__stdcall`, `#pragma comment(lib, ...)` | `__attribute__((...))`, `__builtin_*`, statement expressions |

The debug fill patterns matter a lot for "works in Debug, fails in Release" (section 5).

## 3. Code

### 3.1 One file, two translation units: `code/m01_two_units.c`

The file contains a "header" part (declarations), a unit A part (definitions) and a unit B part
(`main`). Built normally, both halves are switched on and it is one TU. Built with `-DUNIT_A` and
`-DUNIT_B` separately, it becomes two TUs that only meet in the linker.

```c
#if !defined(UNIT_A) && !defined(UNIT_B)
#define UNIT_A
#define UNIT_B
#endif

/* ---- What a header would contain: DECLARATIONS both units agree on. No storage here. ---- */
extern int g_requests;          /* declaration: "an int called g_requests exists somewhere" */
int next_id(void);              /* declaration (prototype) of a function defined in unit A */
const char *unit_a_name(void);

#ifdef UNIT_A
int g_requests = 0;             /* definition: storage is reserved here, external linkage */
static int s_next = 1000;       /* internal linkage: unit B cannot name this object */

static int bump(int *p)         /* internal linkage function: invisible outside this unit */
{
    return (*p)++;
}

int next_id(void)
{
    ++g_requests;
    return bump(&s_next);
}

const char *unit_a_name(void) { return "unit A"; }
#endif

#ifdef UNIT_B
#include <assert.h>
#include <stdio.h>

int main(void)
{
    int a = next_id();
    int b = next_id();
    assert(a == 1000 && b == 1001);
    assert(g_requests == 2);
    printf("ids %d and %d came from %s; g_requests = %d\n", a, b, unit_a_name(), g_requests);
    return 0;
}
#endif
```

Split build:

```bash
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -DUNIT_A -c m01_two_units.c -o unit_a.o
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -DUNIT_B -c m01_two_units.c -o unit_b.o
gcc unit_a.o unit_b.o -o m01_split && ./m01_split
```

```
ids 1000 and 1001 came from unit A; g_requests = 2
```

`nm unit_a.o unit_b.o` gives the table shown in section 2.8. Now link `unit_b.o` alone
(`gcc unit_b.o -o broken`). The compiler was perfectly happy with unit B; the **linker** is the
one that fails (two linker warnings about text relocations trimmed):

```
/usr/bin/ld: unit_b.o: in function `main':
/home/shakir/dev/c12.v9Oc/m01_two_units.c:55:(.text+0xe): undefined reference to `next_id'
/usr/bin/ld: /home/shakir/dev/c12.v9Oc/m01_two_units.c:56:(.text+0x16): undefined reference to `next_id'
/usr/bin/ld: /home/shakir/dev/c12.v9Oc/m01_two_units.c:58:(.text+0x59): undefined reference to `g_requests'
/usr/bin/ld: /home/shakir/dev/c12.v9Oc/m01_two_units.c:59:(.text+0x8c): undefined reference to `g_requests'
/usr/bin/ld: /home/shakir/dev/c12.v9Oc/m01_two_units.c:59:(.text+0x91): undefined reference to `unit_a_name'
collect2: error: ld returned 1 exit status
```

The `.text+0xe` style offsets are the positions of the holes (relocations) the linker could not fill.

### 3.2 Which standard am I really compiling? `code/m01_compiler_and_standard.c`

```c
#if defined(__STDC_VERSION__)
    long v = __STDC_VERSION__;
#else
    long v = 0L;                 /* C90 did not define the macro at all */
#endif
    printf("__STDC_VERSION__ = %ldL -> %s\n", v, std_name(v));

#if defined(_MSC_VER)
    printf("compiler: MSVC, _MSC_VER = %d\n", _MSC_VER);
#elif defined(__clang__)
    printf("compiler: clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    printf("compiler: gcc %d.%d\n", __GNUC__, __GNUC_MINOR__);
#endif

#ifdef __STDC_NO_VLA__
    puts("variable length arrays: NOT supported");
#else
    puts("variable length arrays: supported (optional since C11; MSVC never had them)");
#endif
```

Output with the course flags:

```
__STDC_VERSION__ = 201710L -> C17
__STDC_HOSTED__  = 1 (1 = full standard library available)
compiler: gcc 13.3
variable length arrays: supported (optional since C11; MSVC never had them)
<threads.h>: available
<stdatomic.h>: available
double is IEEE 754 binary64 (__STDC_IEC_559__ defined)
```

First line with other `-std=` values (the C99 and C11 builds stop at the final `assert` on purpose):

```
-std=gnu17:  __STDC_VERSION__ = 201710L -> C17
-std=c2x:    __STDC_VERSION__ = 202000L -> C2x draft (what GCC 13 reports for -std=c2x)
-std=c11:    m01_compiler_and_standard.c:65: main: Assertion `v >= 201710L' failed.
```

Note `__clang__` is tested before `__GNUC__`: Clang also defines `__GNUC__` for compatibility, so
the order of the tests matters.

### 3.3 What order is guaranteed: `code/m01_eval_order.c`

`note()` appends a letter to a trace each time it is called, so the program can print the order
in which the compiler chose to evaluate things:

```c
    int sum = note('a', 1) + note('b', 2);              /* order of the two calls: unspecified */
    int packed = three(note('x', 1), note('y', 2), note('z', 3));   /* argument order: unspecified */
    int r = note('p', 0) && note('q', 1);               /* &&: left first; right only if needed */
    r = (note('s', 1), note('t', 2));                   /* comma operator: s, then t; value of t */
```

```
note(a) + note(b)     called in order "ab", value 3
three(x, y, z)        called in order "zyx", value 123
p && q                called "p", value 0
m || n                called "m", value 1
(s, t)                called "st", value 2
c ? d : e             called "cd", value 10
all guaranteed orderings held
```

GCC 13 on x86-64 evaluated the three arguments **right to left** ("zyx") but the operands of `+`
left to right. Neither is promised. Code that depends on argument order (for example
`printf("%d %d", next(), next())`) is a latent bug that appears when the compiler, the flags or
the platform change.

## 4. Under the hood (senior-level)

### What the preprocessor hands the compiler

`gcc -E` output starts with **line markers** so that error messages can point back to your file:

```
# 0 "m01_eval_order.c"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3 4
# 0 "<command-line>" 2
# 1 "m01_eval_order.c"
# 11 "m01_eval_order.c"
# 1 "/usr/include/assert.h" 1 3 4
```

`# 1 "/usr/include/assert.h" 1 3 4` means "now at line 1 of assert.h; flag 1 = entering a new
file, 3 = system header (warnings suppressed), 4 = treat as `extern "C"`". When a macro or a
header does something surprising, `gcc -E` (MSVC: `cl /P`) shows you exactly what the compiler
saw. This is the first tool to reach for when a build behaves differently from what the source
appears to say.

### What the compiler does with a TU

GCC parses the TU into a tree, lowers it to GIMPLE (three-address code in SSA form, where every
variable is assigned once), runs hundreds of optimisation passes on it, lowers to RTL (close to
machine instructions), allocates registers and emits assembly. The UB-based optimisations you will
see in this course happen in those passes: GCC treats "`x + 1 > x` for signed `x`" as always true
because overflow is undefined. Here is GCC 13 `-O2` on two small functions (the `endbr64`
instructions that `-fcf-protection` adds are removed for readability):

```c
int naive_increment_overflows(int x) { return x + 1 < x; }
int checked(int x) { return x == 2147483647; }
```

```asm
naive_increment_overflows:
	xorl	%eax, %eax          # return 0: the comparison was deleted
	ret
checked:
	xorl	%eax, %eax
	cmpl	$2147483647, %edi   # the correct check survives
	sete	%al
	ret
```

### Relocations: the holes in an object file

`objdump -dr unit_b.o` shows the holes the linker has to fill:

```
   d:	e8 00 00 00 00       	call   12 <main+0x12>
			e: R_X86_64_PLT32	next_id-0x4
  57:	8b 05 00 00 00 00    	mov    0x0(%rip),%eax        # 5d <main+0x5d>
			59: R_X86_64_PC32	g_requests-0x4
```

The call instruction's target is `00 00 00 00`. The relocation record says: "at offset `0xe`,
write the distance from here to `next_id`, minus 4". The linker computes every such value once it
has placed all sections in memory. `R_X86_64_PC32` is a PC-relative data reference; that is how
x86-64 code reaches globals without absolute addresses, which is what makes position-independent
executables (and ASLR) cheap.

### Sections

`objdump -h unit_a.o` (trimmed):

```
  0 .text         00000056   ...  2**0
  1 .data         00000004   ...  2**2
  2 .bss          00000004   ...  2**2
  3 .rodata       00000007   ...  2**0
```

`.data` holds `s_next = 1000` (4 bytes), `.bss` holds `g_requests` (4 bytes of zeros that are not
stored in the file), `.rodata` holds the string `"unit A"` plus its NUL (7 bytes). At load time
the OS maps `.text` and `.rodata` read-only, `.data` read-write from the file, and `.bss` as fresh
zero pages. That is why writing to a string literal crashes: it lives in a read-only page. The
full process layout is in 05_Memory_Debugging_RCA/01_process_memory_layout.md.

### Static initialisation needs no code

C requires initialisers of static-duration objects to be **constant expressions**. The compiler
writes the bytes straight into `.data`, so nothing runs before `main` to set them up. (C++ allows
dynamic initialisation of globals, and therefore has the "static initialisation order fiasco";
C does not.) Before `main`, the C runtime's `_start` sets up the stack, `argc`/`argv`, stdio and
`atexit` handlers, and calls `main`; when `main` returns, `exit` flushes stdio buffers and runs
the handlers.

### Why missing prototypes crash only on 64-bit

In C89, calling a function with no declaration in scope made the compiler assume it returns
`int`. GCC 13 still accepts this with a warning:

```
imp.c:1:28: warning: implicit declaration of function 'make' [-Wimplicit-function-declaration]
imp.c:1:28: warning: initialization of 'char *' from 'int' makes pointer from integer without a cast [-Wint-conversion]
```

On a 32-bit target `int` and pointers have the same size and the code works by accident. On
x86-64 the caller reads only the low 32 bits of the returned 64-bit pointer (`eax` instead of
`rax`) and sign-extends it. Heap addresses on 64-bit Linux are above 4 GiB, so the truncated
pointer is garbage and the first dereference crashes. The classic case is calling `malloc`
without `#include <stdlib.h>`. Treat `-Wimplicit-function-declaration` as an error (GCC 14 does).

## 5. In production

- **"Works in Debug, crashes in Release."** The usual causes, in order of frequency: an
  uninitialised variable (MSVC Debug fills stack with `0xCC` and heap with `0xCD`, so a pointer is
  never accidentally NULL or valid in Debug; Release gets whatever was there); undefined behaviour
  that the optimiser exploits (deleted checks, reordered stores); and timing changes that expose a
  data race. When a crash moves or vanishes with optimisation level, think "UB" first, and build
  with sanitizers (05_Memory_Debugging_RCA/08_valgrind_and_sanitizers.md).
- **LNK2019 after a refactor.** A function was declared in a header but its `.c` file is not in
  the project, or it was made `static` in its file, or the C function is being called from a C++
  file without `extern "C"` (the C++ side looks for a mangled name the C object does not have).
  `dumpbin /symbols foo.obj | findstr name` on Windows, `nm -A *.o | grep name` on Linux: look for
  `U` on one side and the missing `T` on the other.
- **LNK2005 / multiple definition.** Someone put `int g_debug_level = 0;` in a header. Every TU
  that includes it defines it. Fix: `extern` in the header, definition in one `.c` file.
- **A header that changes layout depending on who includes it.** `#pragma pack(1)` without a
  matching `pop` at the end of a header leaks into every struct declared after it in each TU that
  includes it. Two TUs can then disagree about the size and offsets of the same `struct`, and
  neither compiler nor linker notices. Symptoms are fields that read as garbage only when the
  struct crosses a module boundary. Module 06 shows the safe `push`/`pop` form.
- **Mismatched declarations across files.** `extern int g_count;` in one file and `long g_count;`
  in another, or a prototype in a header that no longer matches the definition. Prevent it by
  making every `.c` file include its own header, so the compiler sees declaration and definition
  together.
- **3 AM tools.** `gcc -E` / `cl /P` for "what did the compiler actually see"; `gcc -H` /
  `cl /showIncludes` for "which header got pulled in from where"; `nm` / `dumpbin /symbols` for
  "who defines this symbol"; `objdump -dr` / `dumpbin /disasm /relocations` for "what is this
  instruction really calling".

## 6. Try it

1. **(easy)** Do the split build of `m01_two_units.c` and run `nm` on both objects. Done when you
   can say, for every line of the `nm` output, which line of source created it and why its letter
   is upper or lower case.
2. **(easy)** Remove `static` from `s_next` in unit A and add a definition `int s_next = 5;` to
   unit B. Rebuild split. Done when you have the `multiple definition` error, can explain it, and
   can fix it two ways (make either one `static`).
3. **(medium)** In the unit B part only, change the declaration to `extern long g_requests;`
   (wrap it in `#ifdef UNIT_B`). Build split. Done when it links with no error and you can explain
   why the linker cannot notice, and what `main` now reads.
4. **(medium)** Run `gcc -std=c17 -save-temps -c m01_eval_order.c`. Open the `.i` file and find
   your own code at the bottom; open the `.s` file and find the call order of `note('x'...)`,
   `note('y'...)`, `note('z'...)`. Done when you can point at the instructions that prove the
   right-to-left order.
5. **(hard)** Write `make.c` with `char *make(void) { return malloc(16); }` and `main.c` that calls
   `make()` with **no prototype**. Build with `gcc -std=c89 -w` and run. Done when you see it crash
   (or print a truncated pointer) and can explain the `eax` versus `rax` mechanism; then add the
   prototype and see it work.
6. **(hard)** If you have Visual Studio, build `m01_compiler_and_standard.c` with `cl /std:c17`
   and with plain `cl` (no `/std`). Done when you have both outputs side by side and can explain
   every difference from the GCC output above.

## 7. Interview questions

**Q: What happens between `gcc main.c util.c` and a running program?**
The preprocessor expands includes and macros for each file, producing one translation unit per
`.c`. The compiler turns each TU into assembly and the assembler into an object file with a
symbol table and relocations. The linker resolves every undefined symbol against the other
objects and libraries, lays out sections and patches the relocations. At run time the loader maps
the sections, the C runtime sets up and calls `main`.

**Q: What is the difference between a declaration and a definition?**
A declaration introduces a name and its type; a definition also creates the entity, reserving
storage for an object or supplying a function body. You can declare many times but must define an
externally linked entity exactly once in the program. `extern int x;` is a declaration;
`int x = 0;` at file scope is a definition.

**Q: What does `static` mean at file scope and inside a function?**
At file scope it gives internal linkage: the name is private to that translation unit. Inside a
function it gives static storage duration: one object, initialised once before `main`, that keeps
its value between calls. They are two different features that share a keyword.

**Q: Explain undefined, unspecified and implementation-defined behaviour with an example of each.**
Implementation-defined: the compiler chooses and documents it, such as `sizeof(long)` or the
signedness of `char`. Unspecified: the compiler chooses among valid options without documenting
it, such as the order in which function arguments are evaluated. Undefined: no requirements at
all, such as signed overflow or reading past an array; the optimiser may assume it never happens.

**Q: "undefined reference" versus "multiple definition": which tool reports them and why?**
Both come from the linker, because only the linker sees all object files together. "Undefined
reference" means some object uses a symbol that no object or library defines; "multiple
definition" means two objects define the same external symbol. The compiler cannot report either
because it sees one translation unit at a time.

**Q: Why can a type mismatch between `extern` declarations in two C files go undetected?**
C symbols are just names; the object file records no type. Each TU is compiled against its own
declaration, and the linker matches names only. The defence is to include a module's own header
in its `.c` file so the compiler checks the declaration against the definition.

**Q: What does the as-if rule allow?**
Any transformation that preserves observable behaviour: volatile accesses, file output and
interactive I/O. The compiler may keep values in registers, remove dead stores, reorder and
inline code. It is why debugging optimised code is hard and why UB can make an apparently
unrelated check disappear.

**Q: Which differences between MSVC and GCC have you had to deal with in C code?**
`long` is 32 bits on Windows and 64 on Linux; MSVC has no VLAs; old MSVC `_snprintf` does not
terminate on truncation; MSVC `_s` functions are not available in glibc and some differ from
Annex K in argument order; POSIX names like `strdup` and `strtok_r` have `_strdup` and `strtok_s`
equivalents. Also MSVC Debug builds fill memory with `0xCC`/`0xCD`, which hides uninitialised
variable bugs until Release.

## 8. Recap

- The compiler sees one translation unit at a time; the linker joins them by symbol name.
- Four stages: preprocess (`-E`), compile (`-S`), assemble (`-c`), link. You can stop after each.
- Declaration = name and type; definition = also creates it. Exactly one definition per external
  name; GCC 10+ rejects duplicate tentative definitions (`-fno-common`).
- Headers hold declarations, types, macros and `static inline` functions; never definitions of
  globals or non-inline functions.
- Linkage: external (shared), internal (`static` at file scope), none (locals).
- `nm`: `T`/`D`/`B` defined, `U` needed from elsewhere, lower case = `static`.
- "Undefined reference" and "multiple definition" are linker errors; C symbols carry no types.
- The as-if rule lets the optimiser do anything that keeps observable behaviour; UB breaks the
  contract and the optimiser exploits it.
- Order of evaluation of `+` operands and function arguments is unspecified; `&&`, `||`, `?:` and
  comma are ordered.
- MSVC C differs from GCC C in `long` size, VLAs, `_snprintf`, `_s` functions and Debug fill
  patterns; know them before you port Windows code.
