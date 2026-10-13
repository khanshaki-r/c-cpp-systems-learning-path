# 03. GCC and Clang flags that matter

**Time:** ~2 hours   **Prerequisites:** 02

## 1. In plain words

A compiler flag is an instruction to the compiler about **how** to build, not **what** to build.
"Optimise for speed", "keep debug information", "treat this warning as an error", "insert memory
checks into every load and store". In Visual Studio you set these through project property pages
and MSBuild writes `/O2 /Zi /W4` into the command line for you. On Linux you write the flags
yourself, or CMake writes them from a few properties. Either way, a senior engineer knows what each
flag does and what it costs.

There are roughly fifteen flags you will use every day and another ten you reach for when something
is wrong. This module is that list, with what each does under the hood and the MSVC name next to it.

## 2. The real thing

### 2.1 Language standard: `-std=`

```bash
g++ -std=c++17 ...     # the standard this curriculum targets
g++ -std=c++20 ...     # GCC 13 supports most of C++20; modules partially
g++ -std=gnu++17 ...   # GCC default if you say nothing: C++17 plus GNU extensions
gcc -std=c11 ...       # for C
```

Always set it explicitly. The default changes between compiler versions (GCC 6 defaulted to
gnu++14, GCC 11+ to gnu++17), and `gnu++` enables extensions like variable-length arrays and
statement expressions that MSVC will reject. `-pedantic` or `-Wpedantic` warns on extensions.
MSVC: `/std:c++17`, plus `/permissive-` for standards conformance and `/Zc:__cplusplus` so the
`__cplusplus` macro reports the truth.

### 2.2 Optimisation: `-O0` to `-O3`, `-Os`, `-Og`

| Flag | Meaning | Use |
|---|---|---|
| `-O0` | no optimisation; every variable lives in memory; straightforward code | default; debugging |
| `-O1` | basic: constant folding, dead code, simple register allocation | rarely |
| `-O2` | most optimisations that do not trade size for speed: inlining of small functions, loop invariant motion, vectorisation (GCC 12+), scheduling | **release default** |
| `-O3` | `-O2` plus aggressive inlining, loop unrolling, more vectorisation | hot numerical code; measure |
| `-Os` | optimise for size | embedded, cold code |
| `-Og` | optimise, but keep debugging experience good | debug builds that are too slow at -O0 |
| `-Ofast` | `-O3` plus `-ffast-math`: breaks IEEE semantics | avoid unless you know why |

What `-O2` changes about your view of the program: variables are in registers or gone entirely,
functions are inlined so stack traces skip frames, loops are reordered, and reads of a non-volatile
variable can be hoisted out of a loop. The classic "my busy-wait loop never sees the flag change" is
the compiler legitimately caching a load because you did not use `std::atomic` (04_Concurrency).
`-O2` can also expose undefined behaviour that `-O0` hid: signed overflow, uninitialised reads,
strict aliasing.

MSVC: `/Od` (none), `/O1` (size), `/O2` (speed), `/Ox`. Visual Studio Debug config is `/Od`,
Release is `/O2`.

### 2.3 Debug information: `-g`

```bash
g++ -g ...            # DWARF debug info in the object file (default level 2)
g++ -g3 ...           # plus macro definitions
g++ -O2 -g ...        # optimised AND debuggable: what production builds should be
g++ -ggdb ...         # GDB-specific extensions
```

`-g` does **not** change the generated machine code. It adds `.debug_*` sections that map addresses
to source lines, describe variable locations, and encode types. The sections make the file larger
(often 5-10x, typical) but they are not loaded into memory at run time. Production rule: always build
with `-O2 -g`, then `strip --only-keep-debug` into a separate file (or `objcopy`) so you can
symbolise core dumps from the field. This is the Linux equivalent of generating `.pdb` with `/Zi`
and archiving it: MSVC puts debug info in a separate `.pdb`; GCC puts it inside the ELF until you
split it out.

`-fno-omit-frame-pointer`: keep `rbp` as a frame pointer. `-O2` omits it to free a register,
which makes stack unwinding depend on DWARF `.eh_frame` data. Frame pointers make `perf` and
in-process backtraces cheap and reliable. Cost is around 1% (typical); Fedora and Ubuntu now build
their distributions with it. Use it in any build you will profile.

### 2.4 Warnings

```bash
-Wall           # the "common" set: unused variables, uninitialised, sign-compare, return-type...
-Wextra         # more: unused parameters, missing field initialisers, implicit fallthrough
-Wpedantic      # ISO compliance: warns on GNU extensions
-Werror         # every warning is an error: the build fails
-Wshadow        # a local hides another local or a member; classic source of wrong-variable bugs
-Wconversion    # implicit conversions that may lose value (int -> short, double -> int)
-Wsign-conversion  # signed <-> unsigned implicit conversions (part of -Wconversion in C++)
-Wold-style-cast   # C-style casts in C++
-Wnon-virtual-dtor # class with virtual functions but non-virtual destructor
-Woverloaded-virtual  # a derived function hides a base virtual overload
-Wnull-dereference
-Wdouble-promotion    # float promoted to double silently
-Wformat=2            # printf format string checks, including non-literal formats
-Wimplicit-fallthrough  # switch case falls through without [[fallthrough]]
-Wduplicated-cond -Wduplicated-branches -Wlogical-op   # GCC only
-Wuseless-cast        # GCC only
-Wno-unused-parameter # -Wno-X disables X. Use sparingly and with a comment
-Werror=return-type   # make one specific warning an error
```

A good baseline for a new project:

```bash
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
-Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
-Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror
```

Why `-Werror` on a project you own: a warning that is ignored on Monday is 400 warnings by Friday
and the one real bug is invisible among them. Why **not** `-Werror` for code you distribute as
source: a new compiler version adds new warnings and breaks every downstream build. CMake convention:
turn `-Werror` on for CI and developers, off for packagers (an option like `TCPKV_WERROR`).

What `-Wall` does **not** include that you might expect: `-Wshadow`, `-Wconversion`,
`-Wold-style-cast`. `-Wall` is "warnings that are almost always right"; the rest need judgement.

Suppressing one warning at one site:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void legacy_callback(int unused_handle) { }
#pragma GCC diagnostic pop
```

Or `[[maybe_unused]]`, `(void)param;`, or omitting the parameter name. Prefer those.

### 2.5 Sanitizers: `-fsanitize=`

Sanitizers are compiler-inserted run-time checks. The compiler instruments every memory access,
every arithmetic operation, or every synchronisation, and links a run-time library that reports
violations with a stack trace. They are the single most effective bug-finding tool you have.
Full treatment in 05_Memory_Debugging_RCA; the flags:

```bash
-fsanitize=address              # ASan: heap/stack/global overflow, use-after-free, use-after-return,
                                #       double free, leaks (LSan included on Linux). ~2x slowdown, ~3x memory
-fsanitize=undefined            # UBSan: signed overflow, misaligned access, null deref, bad shifts,
                                #       out-of-bounds on known arrays, invalid enum/bool. ~1.2x slowdown
-fsanitize=address,undefined    # the usual pair for tests
-fsanitize=thread               # TSan: data races, lock-order inversions. ~5-15x slowdown. NOT combinable with ASan
-fsanitize=leak                 # LSan alone
-fsanitize=memory               # MSan: uninitialised reads. Clang only, needs all libs instrumented
-fno-sanitize-recover=all       # abort on first UBSan report instead of continuing
-fsanitize-address-use-after-scope
-fno-omit-frame-pointer -g      # ALWAYS pair with sanitizers for readable stack traces
```

Run-time options via environment: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`,
`UBSAN_OPTIONS=print_stacktrace=1`, `TSAN_OPTIONS=second_deadlock_stack=1`.

Rules: the sanitizer flag must be given at **both** compile and link time. Do not mix sanitized and
non-sanitized objects of the same library. Build type for sanitizers: `-O1 -g` is the sweet spot
(fast enough, traces accurate). CMake: a separate preset or build directory (module 04).

MSVC: `/fsanitize=address` since VS 2019 16.9; no UBSan or TSan. Say that in interviews.

### 2.6 Target architecture: `-march`, `-mtune`

```bash
-march=x86-64        # baseline: SSE2 only (default on x86-64 Linux)
-march=x86-64-v2     # +SSE4.2, POPCNT  (every x86 since ~2009)
-march=x86-64-v3     # +AVX2, BMI, FMA  (Haswell 2013+; RHEL 10 baseline)
-march=native        # whatever THIS machine supports. Binary may crash with SIGILL elsewhere
-mtune=generic       # scheduling tuning only, no new instructions
```

The trap: `-march=native` in CI produces a binary that uses AVX-512 on the build runner and dies
with "Illegal instruction" on the customer's older server. For distributed binaries use a named
level. For Docker images that run on your own fleet, `-march=x86-64-v3` is a reasonable choice.
MSVC: `/arch:AVX2`.

### 2.7 Preprocessor, include and link paths

```bash
-DNDEBUG                  # define NDEBUG: disables assert(). CMake Release adds this automatically
-DTCPKV_VERSION=\"1.2.0\" # define with a value
-DLOG_LEVEL=2
-UNDEBUG                  # undefine
-I include/               # add header search dir (for #include "x.h" and <x.h>)
-isystem /opt/foo/include # like -I but warnings in those headers are suppressed (third-party)
-iquote dir               # only for #include "x.h"
-L lib/                   # add library search dir for the linker
-lfoo                     # link libfoo.so or libfoo.a (searched -L dirs then system dirs)
-l:libfoo.a               # link exactly this file name
-pthread                  # compile: define _REENTRANT; link: -lpthread. Use this, not -lpthread
-static                   # fully static executable (needs static libc; glibc static has caveats)
-static-libstdc++ -static-libgcc   # static C++ runtime only; glibc stays dynamic. Good for deploy
-shared                   # produce a shared library
-fPIC                     # position independent code (module 02)
-fvisibility=hidden       # export nothing from a .so unless marked
-Wl,<opt>                 # pass <opt> to the linker: -Wl,-rpath,'$ORIGIN' -Wl,--as-needed -Wl,-z,now
-fuse-ld=lld              # use LLVM's linker (faster than bfd); or -fuse-ld=mold
-rdynamic                 # export all symbols from an executable (for backtrace() symbol names, dlopen plugins)
```

`-I` vs `-isystem` matters once `-Werror` is on: you do not want a warning inside Boost or
GoogleTest headers to fail your build. CMake's `SYSTEM` keyword on `target_include_directories`
emits `-isystem`.

Why `-pthread` and not `-lpthread`: on old glibc `-pthread` also set preprocessor macros that
selected thread-safe variants; on glibc 2.34+ (Ubuntu 22.04 and newer) libpthread is merged into
libc and both spellings work, but `-pthread` remains the portable, correct one. CMake:
`find_package(Threads REQUIRED)` and link `Threads::Threads`.

### 2.8 Other flags you will see

```bash
-fno-exceptions -fno-rtti    # embedded / game code; breaks much of the standard library
-fstack-protector-strong     # canaries against stack smashing (Ubuntu default)
-D_FORTIFY_SOURCE=2          # checked versions of memcpy/sprintf (needs -O1+; Ubuntu default)
-fPIE -pie                   # position-independent executable for ASLR (Ubuntu default)
-fstack-usage                # emit .su files with per-function stack sizes (embedded, deep recursion)
-ftime-report                # where compile time goes
-ftime-trace                 # Clang: Chrome-trace JSON of compile time per header/function
-H                           # print the include tree as it is processed
-M / -MM / -MMD -MF x.d      # emit Makefile dependency rules (how build systems know header deps)
-fdiagnostics-color=always   # colour in CI logs
-fmax-errors=5               # stop after 5 errors
-x c++                       # treat input as C++ regardless of extension
-###                         # print the commands the driver would run, without running
-v                           # verbose: show include paths and subprocess commands
-print-search-dirs           # where the driver looks for libs
-Q --help=warnings           # list warnings and whether enabled
```

### 2.9 Clang differences

Clang accepts almost all GCC flags. Differences that matter:

- `-Weverything` (Clang only) enables literally every warning; use it once to discover warnings,
  never in a build.
- Error messages are often clearer, with fix-it hints.
- `-ftime-trace` and `-fsanitize=memory` are Clang only.
- `clang-tidy` and `clang-format` come from the same front end and read `compile_commands.json`.
- Default C++ standard library on Linux is still libstdc++ (GCC's); `-stdlib=libc++` switches.

CI best practice: build with both. Each catches warnings the other misses (module 09).

### 2.10 MSVC equivalents

| Purpose | GCC / Clang | MSVC |
|---|---|---|
| Standard | `-std=c++17` | `/std:c++17 /permissive- /Zc:__cplusplus` |
| No optimisation | `-O0` | `/Od` |
| Optimise speed | `-O2` | `/O2` |
| Optimise size | `-Os` | `/O1` |
| Debug info | `-g` (inside ELF) | `/Zi` (separate .pdb), `/Z7` (in .obj) |
| Warnings | `-Wall -Wextra` | `/W4` (`/Wall` is MSVC's "everything", noisy) |
| Warnings as errors | `-Werror` | `/WX` |
| Specific warning off | `-Wno-unused-parameter` | `/wd4100` |
| Pedantic | `-Wpedantic` | `/permissive-` |
| Define | `-DNAME=1` | `/DNAME=1` |
| Include path | `-I dir` | `/I dir` |
| System include | `-isystem dir` | `/external:I dir /external:W0` |
| Library path | `-L dir` | `/LIBPATH:dir` (linker) |
| Link library | `-lfoo` | `foo.lib` on command line, or `#pragma comment(lib,"foo")` |
| Compile only | `-c` | `/c` |
| Output | `-o x` | `/Fe:x.exe`, `/Fo:x.obj` |
| Preprocess only | `-E` | `/E`, `/P` |
| Assembly listing | `-S` | `/FA` |
| Position independent | `-fPIC` | not needed; PE relocations are always present |
| Shared library | `-shared` | `/LD` (`/DLL` to linker) |
| Static C++ runtime | `-static-libstdc++` | `/MT` (debug `/MTd`) |
| Dynamic C++ runtime | default | `/MD` (debug `/MDd`) |
| Threads | `-pthread` | default (CRT is always multithreaded now) |
| Exceptions | default on | `/EHsc` (must be explicit) |
| RTTI off | `-fno-rtti` | `/GR-` |
| LTO | `-flto` | `/GL` + linker `/LTCG` |
| ASan | `-fsanitize=address` | `/fsanitize=address` |
| UBSan, TSan | `-fsanitize=undefined,thread` | none |
| Stack protector | `-fstack-protector-strong` | `/GS` (default on) |
| Frame pointer | `-fno-omit-frame-pointer` | `/Oy-` |
| Target ISA | `-march=x86-64-v3` | `/arch:AVX2` |
| Parallel build | build system (`make -j`, Ninja) | `/MP` |
| Precompiled header | `-include pch.h` with `.gch` | `/Yc /Yu` |
| Show commands | `-v`, `-###` | `/showIncludes`, verbose MSBuild output |

The `/MD` vs `/MT` choice has no exact Linux analogue because glibc is almost always dynamic; the
closest is `-static-libstdc++` (statically link the C++ standard library only). The Windows
"mismatched runtime library" link error (LNK2038) has no Linux equivalent because there is one glibc.

## 3. Code

Keep a flags file and use it in every shell experiment:

```bash
# ~/.cxxflags  - source it or paste into scripts
export CXXFLAGS_DEBUG="-std=c++17 -O0 -g3 -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"
export CXXFLAGS_RELEASE="-std=c++17 -O2 -g -DNDEBUG -Wall -Wextra -Werror"
export CXXFLAGS_ASAN="-std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
```

Watch optimisation levels change code. `code/m03_flags_demo.cpp`:

```cpp
#include <cstdint>
#include <cstdio>

// A loop the optimiser can turn into a closed-form expression at -O2.
std::uint64_t sum_to(std::uint64_t n) {
    std::uint64_t s = 0;
    for (std::uint64_t i = 1; i <= n; ++i) s += i;
    return s;
}

// Shadowing bug that -Wshadow catches: the inner 'total' hides the outer one.
int shadow_demo(int x) {
    int total = 0;
    if (x > 0) {
        int total = x * 2;     // -Wshadow: declaration shadows a previous local
        (void)total;
    }
    return total;              // always 0: the bug
}

// Narrowing that -Wconversion catches.
short narrow(int v) { return static_cast<short>(v); }   // explicit cast: no warning

int main() {
    std::printf("%llu\n", static_cast<unsigned long long>(sum_to(100000)));
    std::printf("%d %d\n", shadow_demo(5), narrow(70000));
    return 0;
}
```

```bash
g++ -std=c++17 -O0 -S -o - code/m03_flags_demo.cpp | grep -A20 '_Z6sum_tom:' | head -30   # a loop
g++ -std=c++17 -O2 -S -o - code/m03_flags_demo.cpp | grep -A20 '_Z6sum_tom:' | head -30   # multiply, no loop
g++ -std=c++17 -Wall -Wextra -c code/m03_flags_demo.cpp          # compiles: -Wall misses -Wshadow
g++ -std=c++17 -Wall -Wextra -Wshadow -c code/m03_flags_demo.cpp # warning: declaration of 'total' shadows
g++ -std=c++17 -Wall -Wextra -Wshadow -Werror -c code/m03_flags_demo.cpp; echo "status=$?"  # 1
```

Sanitizer in action, `code/m03_asan_demo.cpp`:

```cpp
#include <vector>
int main(int argc, char**) {
    std::vector<int> v(4);
    int idx = argc + 3;        // 4 when run with no args: one past the end
    v[idx] = 1;                // heap-buffer-overflow: ASan reports with line numbers
    return v[0];
}
```

```bash
g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined code/m03_asan_demo.cpp -o asan_demo
./asan_demo
# ==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
#   WRITE of size 4 at ... thread T0
#     #0 0x... in main code/m03_asan_demo.cpp:5
#   0x... is located 0 bytes after 16-byte region [...]
#   allocated by thread T0 here: ... operator new ... std::vector ...
```

## 4. Under the hood (senior-level)

**What `-O2` actually does to your code.** GCC runs around 200 passes; the ones you notice:
inlining (small functions disappear, call overhead gone, more optimisation across the former
boundary), SROA (structs split into scalars that live in registers), GVN/PRE (duplicate
computations removed), loop-invariant code motion, strength reduction (`i*4` becomes `p+=4`),
induction-variable elimination (the `sum_to` loop becomes `n*(n+1)/2`), dead-store elimination,
tail-call optimisation, and auto-vectorisation (GCC 12+ at `-O2`, with a cost model that is more
aggressive at `-O3`). Every one of these is legal only because the standard says what is observable
behaviour: values in non-volatile variables between sequence points are not. If your program relies
on something that is not observable behaviour, `-O2` will "break" it.

**Why UB matters more at `-O2`.** The optimiser assumes UB does not happen and uses that as a
fact. `if (p == nullptr) return; *p = 1;` after an earlier `*p` dereference: the check is removed
because `p` was already dereferenced, so it "cannot" be null. Signed overflow: `for (int i = 0; i
<= n; ++i)` with `n == INT_MAX` becomes an infinite loop at `-O2` because `i <= n` is folded to
`true`. UBSan exists to catch exactly the cases the optimiser exploits.

**How `-g` coexists with `-O2`.** DWARF location lists describe where a variable lives at each
instruction range: in `rax` from 0x401000 to 0x401020, in `rbx` after, "optimized out" where it has
no home. `-Og` and `-fvar-tracking-assignments` improve the coverage. GDB printing
`<optimized out>` is this mechanism being honest, not a bug.

**How ASan works.** Every 8 bytes of application memory has one shadow byte at
`(addr >> 3) + offset`. The compiler inserts a shadow check before each load/store; the run time
poisons redzones around allocations and quarantines freed memory so use-after-free is detected
rather than recycled. Cost: 2x CPU, 3x memory. Interceptors replace `malloc`, `memcpy`, `strcpy`
etc. via symbol interposition (module 02). TSan uses a different shadow: a per-8-byte history of
the last four accesses (thread, epoch, size, write-bit) plus vector clocks per thread to detect
unordered access. That is why it is so much slower and why it finds real races deterministically.

**What `-D_FORTIFY_SOURCE=2` does.** Where the compiler knows the destination size at compile
time (`char buf[64]; strcpy(buf, src)`), glibc's `__builtin___strcpy_chk` replaces the call and
aborts on overflow. Ubuntu enables it by default at `-O1+`. It is a cheap partial ASan for production.

**Why `-march=native` is a deployment bug.** The compiler emits `vpaddd ymm...` (AVX2) or AVX-512
instructions. A CPU without the feature raises `#UD`, the kernel delivers SIGILL, and the process
dies with exit 132 before `main` if a constructor used the instruction. Function multi-versioning
(`__attribute__((target_clones("avx2","default")))`) is the correct way to get native speed on a
portable binary.

## 5. In production

- **The build that CI runs must be the build you debug.** `-O2 -g -fno-omit-frame-pointer`
  everywhere, debug info split and archived with the build ID (`readelf -n prog | grep Build.ID`).
  When a core comes in from the field you match it by build ID. Same discipline as archiving
  `.pdb` files per release.
- **Warnings as errors in CI, not in distributed source.** An option in CMake (module 04) that is
  ON in presets and in `ci.yml`. New GCC releases add warnings; you want to know, not to break
  downstream packagers.
- **Sanitizer builds are a separate pipeline job**, not a developer afterthought (module 09). An
  ASan run of the test suite on every PR catches most memory bugs before review.
- **A 3 AM story:** release build works, debug build works, release build with one more log line
  crashes. Root cause: a stack buffer overflow of exactly one byte, invisible at `-O0` because
  padding absorbed it, invisible with the old layout because the overwritten byte was unused;
  the log line changed stack layout. ASan found it in 30 seconds. Lesson: run sanitizers before
  you theorise.
- **Flag hygiene for your banking server:** `-Wconversion` on a codebase that mixes `int`,
  `size_t`, `uint32_t` from network protocols will produce hundreds of warnings. Enable it on new
  files first (CMake can set per-target compile options), fix as you touch code.
- **MSVC-isms to drop when porting to GCC:** `#pragma once` is fine (both support it), `__int64`
  becomes `std::int64_t`, `_stricmp` becomes `strcasecmp`, `__declspec(dllexport)` becomes
  `__attribute__((visibility("default")))` behind a macro, `/EHsc` is default, `#pragma comment(lib)`
  does not exist: libraries go in the build system.

## 6. Try it

1. **(easy)** Compile `code/m03_flags_demo.cpp` at `-O0` and `-O2` with `-S` and diff the assembly
   for `sum_to`. Done when you can state in one sentence what the optimiser did.
2. **(easy)** Build a hello world with `-g` and without; compare sizes with `ls -l` and `size`.
   Run `readelf -S` and list the sections that appeared. Done when you can explain why `size`
   shows no difference but `ls -l` does.
3. **(medium)** Take one of your existing C files, compile it with
   `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`. Count warnings per category with
   `grep -o '\[-W[a-z-]*\]' | sort | uniq -c`. Fix the `-Wshadow` ones. Done when the shadow count
   is zero and you can justify each `-Wconversion` warning as either a real bug or a benign cast.
4. **(medium)** Build `code/m03_asan_demo.cpp` with and without ASan. Without, run it ten times and
   note the behaviour. With, read the report and identify the allocation site line. Done when you
   can explain why the non-ASan version did not crash.
5. **(hard)** Write a program that triggers signed-integer overflow in a loop condition such that
   `-O0` terminates and `-O2` loops forever. Confirm with `timeout 2 ./prog; echo $?`. Then build
   with `-fsanitize=undefined` and read the report. Done when you can explain which assumption the
   optimiser made.
6. **(hard)** Build hello world with `-static-libstdc++ -static-libgcc`, then fully `-static`.
   Compare `ldd` output and sizes. Copy the fully static binary to a `scratch` Docker container
   (`docker run --rm -v $PWD:/w scratch /w/hello` will not work because scratch has no shell;
   use `FROM scratch` + `COPY` + `ENTRYPOINT`). Done when it runs in an image with zero other files.

## 7. Interview questions

**Q: What is the difference between `-O2` and `-O3`, and which do you ship?**
`-O3` adds aggressive inlining, loop unrolling and more vectorisation on top of `-O2`. It often
grows code size and sometimes slows down branchy code through cache pressure. Ship `-O2 -g` by
default and move specific hot translation units to `-O3` only after measuring.

**Q: Does `-g` slow down the program?**
No. It adds DWARF sections that describe the code; the machine code is identical. It increases
file size and link time. Production builds should always carry debug info, split into a separate
file if size matters.

**Q: Why `-Wall -Wextra -Werror`? What does `-Wall` miss?**
`-Wall` covers the warnings that are nearly always bugs; `-Wextra` adds more; `-Werror` keeps the
count at zero so new warnings are visible. `-Wall` omits `-Wshadow`, `-Wconversion`,
`-Wold-style-cast` and others that need judgement, so I add them explicitly.

**Q: Explain how AddressSanitizer works and what it costs.**
The compiler instruments each memory access to check a shadow byte that records whether the 8-byte
granule is addressable; the run time surrounds allocations with poisoned redzones and quarantines
freed blocks. Catches overflow, use-after-free, double free and leaks. Roughly 2x CPU and 3x
memory, so it runs in tests and CI, not production.

**Q: What does `-fPIC` have to do with `-shared`?**
Shared libraries are mapped at arbitrary addresses, so their code must not embed absolute
addresses. `-fPIC` compiles with GOT/PLT indirection and RIP-relative access so the linker can
produce a `.so` without text relocations.

**Q: What is the MSVC equivalent of `-Wall -Wextra -Werror -O2 -g`?**
`/W4 /WX /O2 /Zi`. Note `/Wall` on MSVC is far noisier than GCC's `-Wall`; `/W4` is the
practical maximum. `/Zi` writes a separate `.pdb` rather than embedding debug info.

**Q: Why should you not use `-march=native` in CI builds?**
It targets the build machine's CPU features; the binary can contain AVX-512 instructions that
raise SIGILL on an older production CPU. Use a named baseline like `x86-64-v3`, or function
multi-versioning.

## 8. Recap

- Always set `-std=c++17` explicitly; defaults change between compiler versions.
- `-O0` debug, `-O2 -g` release, `-O3` after measuring, `-Og` when debug is too slow.
- `-g` costs nothing at run time; always ship with it and split the debug info.
- `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` is a sane baseline; `-Werror` on in CI, optional for packagers.
- `-fsanitize=address,undefined` at `-O1 -g -fno-omit-frame-pointer` on every PR; TSan separately.
- `-march=native` is for your own machine only.
- `-D`, `-I`, `-isystem`, `-L`, `-l`, `-pthread`, `-Wl,` cover defines, paths, libraries and linker options.
- `-static-libstdc++` for self-contained deploys; full `-static` for scratch containers.
- MSVC: `/W4 /WX /O2 /Zi /MD /std:c++17 /permissive- /EHsc`; `/fsanitize=address` only.
- Sanitizer flags go at both compile and link; never mix sanitized and unsanitized builds of one library.
