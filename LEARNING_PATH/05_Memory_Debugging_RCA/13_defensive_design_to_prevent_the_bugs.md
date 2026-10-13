# 13. Defensive design to prevent the bugs

**Time:** ~3 hours   **Prerequisites:** 04, 08, 12

## 1. In plain words

Modules 01 to 12 taught you to find memory bugs after they have hurt someone. This module is about
making them impossible, or at least making them fail loudly in your test lab instead of quietly at
a bank. It is the "preventive actions" column of every RCA, turned into habits.

The analogy is a building. You can hire excellent firefighters (debuggers, dumps, RCAs). You can
also build with fire-resistant materials (RAII, bounded types), install smoke detectors that go off
early (assertions, sanitizers in CI), run fire drills with random fires (fuzzing), have an inspector
check the plans (static analysis, code review), and fit fire doors that stop a fire spreading
(hardening flags that turn an exploit into a clean crash). Each layer catches what the others miss.
None of them is enough alone.

The order matters for cost: a bug made impossible by the type system costs nothing; a bug caught
by a compiler warning costs a minute; by a sanitizer in CI, an hour; by a fuzzer, a day; in
production, an incident, a dump, an RCA, and the client's trust.

## 2. The real thing

### 2.1 RAII: make ownership impossible to get wrong

RAII ("Resource Acquisition Is Initialisation") means: every resource (memory, file, socket, lock,
DB connection) is owned by an object whose destructor releases it. When the object goes out of
scope - by normal return, early return or exception - the resource is released exactly once.

What it eliminates, from module 04's catalogue:

| Bug class | RAII tool | Why it cannot happen any more |
|-----------|-----------|-------------------------------|
| Leak on early return / exception | `std::unique_ptr`, containers, `std::lock_guard` | Destructors run on every exit path |
| Double free | `std::unique_ptr` (move-only) | Exactly one owner; moving transfers it |
| Use after free (single-thread) | Ownership in one place; others hold ids or `weak_ptr` | Nobody keeps a raw pointer past the owner's lifetime |
| Lifetime races (multi-thread) | `std::shared_ptr` captured by each user, or owner joins users before destroying | The object lives while anyone uses it |
| Lock not released | `std::lock_guard`, `std::scoped_lock`, `std::unique_lock` | Unlock in the destructor |
| Deadlock from inconsistent order of two locks | `std::scoped_lock(a, b)` | Acquires both with a deadlock-avoidance algorithm |

Rules that make RAII work in practice:

- **No naked `new`/`delete` or `malloc`/`free` in application code.** Use `std::make_unique`,
  `std::make_shared`, `std::vector`, `std::string`. A raw `new` is a code-review finding.
- **One owner.** Decide who owns every object. Everyone else gets a reference valid for one call, or
  an id they look up again (`code/m13_raii_fix.cpp`, `SessionTable`).
- **Wrap C resources once.** For a C API (`FILE*`, a socket fd, a DB handle from a C library) write a
  small wrapper or use `unique_ptr` with a custom deleter:

```cpp
struct FileCloser { void operator()(std::FILE *f) const noexcept { if (f) std::fclose(f); } };
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
FilePtr f{std::fopen("ledger.csv", "r")};      // closed on every exit path
```

- **`shared_ptr` is for shared ownership, not for "I could not decide".** It costs an atomic
  refcount and makes lifetime harder to reason about. Cycles leak (module 09); break them with
  `weak_ptr`.
- **For C code (much of your existing code)**: the equivalent discipline is a single cleanup path
  (`goto cleanup;` with every resource initialised to NULL at the top and released once at the
  bottom), and ownership comments on every pointer parameter (`/* borrowed */`, `/* takes
  ownership */`).

### 2.2 Bounds-checked access

Out-of-bounds writes are the most dangerous bug class: they corrupt the heap (module 03) and are
the basis of most memory-safety exploits.

- **Use types that know their size.** `std::vector`, `std::array`, `std::string`, `std::span`
  (C++20) or `gsl::span`, `std::string_view`. A `char*` plus a separate length is two things that
  can disagree.
- **`.at()` where an index comes from input.** Throws `std::out_of_range` instead of corrupting.
  Use `[]` in hot loops where the bound is the container's own `size()`.
- **Hardened standard library in debug and test (and often in production).**
  `-D_GLIBCXX_ASSERTIONS` (libstdc++) makes `operator[]`, `front()`, `back()`, iterator
  dereferences and more check their preconditions and abort. MSVC: `_ITERATOR_DEBUG_LEVEL=2`
  (debug default) or `_CONTAINER_DEBUG_LEVEL=1`. The cost of `_GLIBCXX_ASSERTIONS` is usually small
  (a compare and branch per access); measure before deciding to drop it from production.
- **Range-for and algorithms over index arithmetic.** `for (auto &x : v)` cannot go out of bounds;
  `for (int i = 0; i <= n; ++i)` can.
- **Validate lengths at the boundary.** Every length field from the wire is checked against the
  protocol maximum *and* the destination size before any copy. The parser in RCA 1 of module 12
  and `code/m13_fuzz_target.cpp` both trusted the wire.
- **C string functions**: never `strcpy`, `strcat`, `sprintf`, `gets`. Use `snprintf` and check
  the result; prefer `std::string`. `strncpy` does not always terminate; it is not the safe
  version people think.
- **Integer sizes**: sizes are `size_t`; check multiplications before allocating
  (`if (count > SIZE_MAX / elem) fail;`) - `m04_integer_overflow_size.cpp` and
  `allocate_table` in `m13_raii_fix.cpp`.

### 2.3 Assertions and contracts

An assertion states something that must be true if the program is correct. If it is false, the
program has a bug, and the safest thing is to stop *now*, close to the cause, rather than continue
and crash far away (module 03's "found, not done" problem).

- **Preconditions** at function entry (arguments in range, pointer non-null, lock held),
  **postconditions** before return, **invariants** of a class at the end of each public method
  (`balance == sum(entries)`, `size <= capacity`).
- `assert()` is compiled out with `-DNDEBUG`, which most release builds define. Keep it for
  checks that are expensive or purely for development.
- For checks that guard money or memory safety, use an **always-on** assertion that logs and
  aborts in production too (so you get a core and a clear message instead of corrupted data):

```cpp
// Always-on check: stays in release builds. Aborting with a core beats posting a wrong balance.
#define ALWAYS_ASSERT(cond, msg)                                                   \
    do {                                                                           \
        if (!(cond)) [[unlikely]] {                                                \
            std::fprintf(stderr, "ASSERT FAILED %s:%d %s: %s\n",                   \
                         __FILE__, __LINE__, #cond, msg);                          \
            std::abort();                                                          \
        }                                                                          \
    } while (0)
```

(Macro line continuations are backslashes on preprocessor lines, which is fine; the rule in this
course is only that a `//` comment line must never end with one, because the next line would
silently become part of the comment.)

- **Assertions are not error handling.** Bad *input* is expected and gets an error response.
  A broken *invariant* is a bug and gets an abort. Asserting on user input turns a malformed
  message into a denial of service.
- **Contracts in C++**: C++26 adds contract assertions (`pre`, `post`, `contract_assert`). Until
  your compiler and codebase use them, the macros above and `[[nodiscard]]` on functions whose
  return value must be checked (error codes, `try_lock`) give most of the benefit.
- **Debug-only structural checks** are a form of assertion too: the lock-rank checker from module
  10 (abort on out-of-order acquisition), canary fields in structs (`uint32_t magic = 0xC0FFEE11;`
  checked on every access, set to `0xDEADDEAD` in the destructor) that catch use-after-free and
  overwrites close to the cause.

### 2.4 Fuzzing with libFuzzer

Fuzzing feeds a function millions of automatically generated inputs and watches for crashes,
hangs and sanitizer reports. Coverage-guided fuzzers (libFuzzer, AFL++) keep inputs that reach new
code and mutate them further, so they find their way through parsers that random input never
gets past.

A libFuzzer target is one function:

```cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Request r{};
    parse_request(data, size, r);      // must not crash, leak, hang or trigger UB for ANY input
    return 0;
}
```

`code/m13_fuzz_target.cpp` contains a request parser with a deliberate off-by-one and this target.
Built with `clang++ -fsanitize=fuzzer,address,undefined`, it finds the bug in seconds (section 3).

What to fuzz in a banking server: every parser of external input (ISO 8583, ISO 20022 XML, fixed-
width files from partners, your RPC wire format, config files), decoders (base64, compression),
and anything with length fields. What makes a good target:

- **Deterministic and fast**: no network, no disk, no global state carried between calls (or reset
  it). Thousands of executions per second.
- **Sanitizers on**: a fuzzer without ASan finds only crashes; with ASan and UBSan it finds the
  silent overflow that would have corrupted the heap.
- **A seed corpus**: a handful of real (anonymised) valid messages in `corpus/` gives the fuzzer a
  head start.
- **Dictionaries** (`-dict=`) of protocol tokens help with text formats.
- **Regression**: every crash input found becomes a unit test. CI runs the target for a few minutes
  on every change and for longer nightly; tools like OSS-Fuzz / ClusterFuzzLite run them
  continuously.

GCC has no libFuzzer; on Linux use clang for fuzz builds. On Windows, MSVC supports
`/fsanitize=fuzzer` alongside `/fsanitize=address` (VS 2022).

### 2.5 Static analysis

Static analysers read the source without running it and report likely bugs: null dereferences on
some path, leaks on an error path, use after move, uninitialised variables, buffer overruns with
known sizes, lock misuse.

| Tool | Platform | Run as | Strengths |
|------|----------|--------|-----------|
| Compiler warnings | all | `-Wall -Wextra ...`, `/W4` | Cheapest, fastest, always on |
| clang-tidy | Linux, Windows | `clang-tidy -p build/ file.cpp` with `compile_commands.json` | Modernisation (`modernize-*`), C++ Core Guidelines (`cppcoreguidelines-*`), bug patterns (`bugprone-*`), Clang Static Analyzer path checks (`clang-analyzer-*`) |
| cppcheck | Linux, Windows | `cppcheck --enable=warning,performance,portability --inconclusive --project=compile_commands.json` | Works on C and C++ without a full compile; good at buffer, leak and null checks in C code |
| MSVC `/analyze` | Windows | `cl /analyze` or "Run Code Analysis" in Visual Studio | Path-sensitive checks; with SAL annotations (`_In_reads_(n)`, `_Out_writes_(n)`) it checks buffer sizes across calls - very relevant for C and Win32 code |
| Clang Static Analyzer | Linux | `scan-build make` | Deep inter-procedural path analysis |

Practical adoption on an existing codebase:

- Generate `compile_commands.json` (CMake: `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; make: `bear --
  make`).
- Start with a small, high-value check set (`bugprone-*`, `clang-analyzer-*`,
  `cppcoreguidelines-owning-memory` is noisy on legacy code - add later).
- **Baseline**: record today's findings and fail CI only on *new* findings in changed files. A
  wall of 4,000 old warnings gets ignored; zero new warnings per change is enforceable.
- Suppressions are local and justified (`// NOLINT(bugprone-xyz): reason`), never global.

### 2.6 Warnings as errors

Compiler warnings are free static analysis. Many memory bugs of module 04 are warnings first:
uninitialised variables (`-Wmaybe-uninitialized`), sign/size confusion (`-Wsign-compare`,
`-Wconversion`), format string mismatches (`-Wformat`), returning a reference to a local
(`-Wreturn-local-addr`), missing `return`, fall-through in `switch`.

A recommended GCC/Clang set for new code (the one `m13_raii_fix.cpp` builds clean with):

```
-Wall -Wextra -Wpedantic -Werror
-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast
-Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
```

MSVC: `/W4 /WX /permissive-` and consider `/w14640` (thread-unsafe static init) and `/we4715`
(not all paths return).

Rules: `-Werror` in CI and developer builds always; for third-party headers use `-isystem` so their
warnings do not fail your build; on a legacy codebase, turn on warnings one at a time and fix them
module by module. Compiler upgrades add warnings; budget for them.

### 2.7 Hardening flags

Hardening flags do not remove bugs. They turn a silent corruption or an exploitable overflow into
an immediate, diagnosable crash, and make exploitation harder. For a bank that is a security
requirement as much as a reliability one.

| Flag | Compiler | What it does | Cost |
|------|----------|--------------|------|
| `-D_FORTIFY_SOURCE=2` (or `=3`, GCC 12+) | GCC/Clang, needs `-O1`+ | Replaces `memcpy`, `strcpy`, `sprintf`, `read`... with checked versions when the destination size is known at compile time (`=3` also when known at runtime via `__builtin_dynamic_object_size`); aborts with `*** buffer overflow detected ***` | Very low |
| `-fstack-protector-strong` | GCC/Clang | Canary between locals and the return address in functions with arrays or address-taken locals; checked before return: `*** stack smashing detected ***` | Low |
| `/GS` | MSVC (default on) | Same idea; failure is `0xC0000409` (module 07) | Low |
| `-fstack-clash-protection` | GCC/Clang | Probes each page when growing the stack by large amounts so a huge `alloca`/VLA cannot jump over the guard page | Low |
| `-D_GLIBCXX_ASSERTIONS` | libstdc++ | Bounds and precondition checks in containers (2.2) | Low to moderate |
| `-fPIE -pie`, `-Wl,-z,relro,-z,now`, `-Wl,-z,noexecstack` | GCC/Clang + ld | Full ASLR for the executable; read-only GOT after startup; non-executable stack | Very low |
| `-fcf-protection=full` | GCC/Clang x86 | Intel CET: indirect branch tracking and shadow stack (when CPU and kernel support it) | Low |
| `/guard:cf` (CFG) | MSVC + linker | Control Flow Guard: indirect calls checked against a bitmap of valid targets; a corrupted function pointer or vtable becomes a fail-fast `0xC0000409` instead of a jump to attacker data | Low |
| `/DYNAMICBASE`, `/HIGHENTROPYVA`, `/NXCOMPAT` | MSVC linker (defaults) | ASLR, 64-bit high-entropy ASLR, DEP | None |
| `/CETCOMPAT` | MSVC linker | Hardware shadow stack on supported Windows and CPUs | Low |

The Ubuntu 24.04 GCC enables several of these by default (`_FORTIFY_SOURCE=3` at `-O1`+,
`-fstack-protector-strong`, `-fstack-clash-protection`, PIE, `-fcf-protection`). Check with
`gcc -Q --help=common` or `checksec --file=binary`; do not assume your production build
environment matches your laptop. Pass them explicitly in the build system.

Relationship to dumps: a hardening abort produces a *better* core than the corruption it prevented.
`*** buffer overflow detected ***` from `__chk_fail` has the guilty `memcpy` caller on the stack;
the unhardened version would have crashed in `malloc` an hour later.

### 2.8 A memory-safety code review checklist

Print this and use it for every change to C or C++ code in a server. Each line is a bug class from
this course.

**Ownership and lifetime**
- [ ] Every allocation has exactly one owner; no naked `new`/`delete`/`malloc`/`free` outside a
      wrapper.
- [ ] No raw pointer or reference is stored beyond the call that received it (ids or `weak_ptr`
      instead).
- [ ] Objects used by other threads, timers or callbacks outlive them (or are `shared_ptr`
      captured); shutdown stops and joins users before destroying.
- [ ] No reference, pointer or iterator into a container is used after an insert, erase or
      `push_back` that may invalidate it (`m04_invalid_iterator`).
- [ ] No `std::string_view` / `std::span` outlives the data it views.

**Bounds and sizes**
- [ ] Every copy is bounded by the destination size; every wire length is validated against the
      spec maximum and the destination.
- [ ] Sizes are `size_t`; multiplication and addition of sizes checked for overflow before
      allocation.
- [ ] No `strcpy`/`strcat`/`sprintf`/`gets`; `snprintf` results checked; `strncpy` termination
      handled.
- [ ] Loops use `<` with the container's own size, or range-for.

**Initialisation**
- [ ] Every variable and member initialised at declaration (`int x = 0;`, default member
      initialisers, `T obj{};`).
- [ ] Struct copied to the wire or disk is zero-initialised (no padding leaks).

**Errors and early exits**
- [ ] Every error path releases what the success path releases (RAII makes this automatic).
- [ ] Return values of `[[nodiscard]]` functions, system calls and `snprintf` are checked.
- [ ] Exceptions cannot escape destructors, thread functions or C callbacks.

**Concurrency**
- [ ] Every shared mutable variable is protected by a named lock or is `std::atomic`; the comment
      says which lock.
- [ ] New nested lock acquisitions follow the documented global lock order; multiple locks taken
      with `std::scoped_lock`.
- [ ] No blocking call (I/O, DB, RPC, `sleep`) while holding a lock that request threads need.
- [ ] Condition variable waits use a predicate loop.
- [ ] Signal handlers call only async-signal-safe functions (module 11).

**Observability**
- [ ] New error paths log with request id and a stable event name.
- [ ] Invariants that protect money or memory safety use an always-on assertion.

**Tests**
- [ ] New parsing code has a fuzz target; new concurrent code runs under TSan in CI; the change
      passes the ASan+UBSan suite.

## 3. Code

### 3.1 The fixed versions: `code/m13_raii_fix.cpp`

This file rewrites the bugs of `m04_use_after_free`, `m04_heap_overflow`, `m09_leak` and
`m06_crash_example` the defensive way. It builds clean with the strict warning set and the
hardening flags, and every sanitizer run is clean:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion -pthread \
    -g -O2 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -o build/m13_raii_fix m13_raii_fix.cpp
./build/m13_raii_fix
# audit: session 1 no longer exists
# audit: 2 other 100
# sum 4500
# accepted 750/1000, nothing leaked
# rejected: table too large
# fee applied: 975
./build_debug.sh asan && ./build/m13_raii_fix_asan     # no report
valgrind --leak-check=full ./build/m13_raii_fix_debug   # "All heap blocks were freed"
```

The key patterns, one per bug class:

```cpp
// Use-after-free -> one owner; callers keep ids, not pointers.
class SessionTable {
    std::unordered_map<long, std::unique_ptr<Session>> sessions_;
public:
    void close(long id) { sessions_.erase(id); }            // unique_ptr deletes; nothing dangles
    const Session *find(long id) const;                     // valid for this call only; may be null
};

// Heap overflow -> the container is the bound; .at() throws instead of corrupting.
for (std::size_t i = 0; i < totals.size(); ++i) totals.at(i) = static_cast<int>(i) * 100;

// Leak on early return -> the resource dies with the scope.
auto r = std::make_unique<Request>(Request{id, std::string(64, 'x')});
if (r->id % 4 == 0) return false;                           // freed here too

// Size overflow -> policy limit checked before the multiplication.
if (count > kMax) throw std::length_error("table too large");
```

Exercise 1 below asks you to break each one back and watch which layer (warning, hardened library,
sanitizer) catches it.

### 3.2 Fuzzing: `code/m13_fuzz_target.cpp`

The parser copies an account id into `char account[12]` with a wrong bound check
(`j > sizeof out.account` instead of `j + 1 >= sizeof out.account`). For an account id of 12
characters it writes the terminator to `account[12]`, one past the end of the array; for a longer
one it writes a character to `account[12]` before the check rejects the input. The plain build
runs four hand-written inputs and looks fine.

There is a lesson hidden in this file. `account` is a member of `struct Request { char kind;
char account[12]; long amount; }`. Offset 13 (`account[12]`) is **padding inside the same
struct**, before `amount` at offset 16. AddressSanitizer works at the granularity of whole
allocations: the write is still inside the 24-byte `Request` object, so ASan sees nothing wrong.
Most people expect an ASan `heap-buffer-overflow` here; predicting that it will *not* appear, and
saying why, is a good interview answer. What catches it is UBSan's
`-fsanitize=bounds` check (included in clang's `-fsanitize=undefined`), which knows the array
type is `char[12]` and checks the index. UBSan only *prints* by default and carries on, so make it
fatal for the fuzzer:

```bash
sudo apt install clang          # once; libFuzzer ships with clang
clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -DFUZZING \
    -o build/m13_fuzz m13_fuzz_target.cpp
mkdir -p corpus && printf 'D:ACC-1:100\n' > corpus/seed1
./build/m13_fuzz corpus -max_len=256 -timeout=5
# INFO: Seed: 2344653046
# INFO:        1 files found in corpus
# m13_fuzz_target.cpp:35:9: runtime error: index 12 out of bounds for type 'char[12]'
# SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior m13_fuzz_target.cpp:35:9
# MS: 1 InsertRepeatedBytes-; base unit: ...
# artifact_prefix='./'; Test unit written to ./crash-661bf4dbc4c7659ddff3685176cb9050b56159d0
./build/m13_fuzz crash-<sha1>   # reproduces deterministically: this file becomes a unit test
```

That is real output from clang 18 on Ubuntu 24.04: one mutation of the seed (`MS: 1`) was enough.

A second lesson from the same file. Build the *plain* program with `g++ -O0` and with `g++ -O2`
and run the four samples. At `-O0` the over-long `D:ACCOUNT-TOO-LONG-1:1` is rejected as `bad`.
At `-O2` it is accepted as `ok` with `account=ACCOUNT-TOO-LON` - 15 characters in a 12-byte array.
The write `account[j]` with `j == 12` is undefined behaviour, so GCC may assume it never happens,
conclude that `j > 12` is always false, and delete the check. The program now overflows into
`amount` and beyond, silently, and only in the optimised build - which is the one you ship. This
is why "it works in debug" proves nothing about memory bugs, and why UBSan belongs in CI.

If the same field were the last member of a heap
object - as in RCA 1 of module 12 - the write would leave the allocation and ASan would report
`heap-buffer-overflow`. If it were followed directly by another member with no padding, it would
silently overwrite that member and only UBSan bounds checking, `_FORTIFY_SOURCE` (for library
copies) or a review would catch it. Defence in layers, in one struct.

Fix the bound to `j + 1 >= sizeof out.account`, rebuild, and run the fuzzer for 10 minutes: no
report.

### 3.3 Static analysis on the course code

```bash
sudo apt install clang-tidy cppcheck
cd code
clang-tidy m04_use_after_free.cpp m04_double_free.cpp m09_leak.cpp \
    -checks='-*,clang-analyzer-*,bugprone-*,cppcoreguidelines-owning-memory' -- -std=c++17
# Typical findings (exact wording and which ones fire depend on the clang-tidy version):
#   [clang-analyzer-cplusplus.NewDelete]       use of memory after it is freed
#   [clang-analyzer-unix.Malloc]               attempt to free released memory
#   [clang-analyzer-cplusplus.NewDeleteLeaks]  potential leak of memory
#   [cppcoreguidelines-owning-memory]          raw new/delete outside an owner type
# Whether the analyzer follows the pointer across functions decides whether it finds the bug.
cppcheck --enable=warning --inconclusive m04_*.cpp m09_leak.cpp
clang-tidy m13_raii_fix.cpp -checks='-*,clang-analyzer-*,bugprone-*' -- -std=c++17   # clean
```

Note what static analysis finds (single-function, single-thread ownership errors) and what it
does not (races, overflows whose size depends on runtime input across many functions). That gap is
why you also need sanitizers and fuzzing.

### 3.4 The CI build matrix

This is the build matrix the preventive actions of module 12 ask for, as a shell sketch (module
08's exercise 6 builds it out):

```bash
STRICT="-std=c++17 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -pthread"
HARDEN="-O2 -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong \
        -fstack-clash-protection -fcf-protection=full -fPIE -pie -Wl,-z,relro,-z,now"
g++ $STRICT $HARDEN                                   -o out/release   src.cpp   # what ships
g++ $STRICT -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -o out/asan src.cpp
g++ $STRICT -g -O1 -fsanitize=thread                  -o out/tsan     src.cpp
clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -DFUZZING -o out/fuzz fuzz.cpp
# test stage: run the suite on release, asan, tsan with halt_on_error=1; fuzz for 5 min;
# clang-tidy on changed files; any finding fails the merge request.
```

## 4. Under the hood (senior-level)

**How `_FORTIFY_SOURCE` works.** glibc's headers, when `_FORTIFY_SOURCE > 0` and optimisation is
on, redirect `memcpy(d, s, n)` to `__memcpy_chk(d, s, n, __builtin_object_size(d, 0))`. The
compiler substitutes the size of `d` if it can see it (a local array, a struct member, a `malloc`
with a constant size); if it cannot, the size is `(size_t)-1` and the check is a no-op. `=3` uses
`__builtin_dynamic_object_size`, which can also track sizes known only at runtime (e.g.
`malloc(n)` with `n` a variable), catching more. On failure `__chk_fail` prints
`*** buffer overflow detected ***` and aborts. That is why it needs `-O1` or higher: object-size
tracking happens in the optimiser.

**How the stack protector works.** At function entry the compiler loads a random canary from
thread-local storage (`fs:0x28` on x86-64 Linux) and stores it between the local arrays and the
saved registers/return address; before `ret` it compares and calls `__stack_chk_fail` on mismatch.
A linear overflow of a local array must overwrite the canary before it reaches the return address.
`-strong` instruments every function with a local array or an address-taken local (a middle ground
between `-fstack-protector`, which only covers char arrays of 8+ bytes, and `-all`). It does not
catch overflows that skip the canary (an indexed write `buf[i]` with a wild `i`) or overflows of
heap objects.

**How CFG works.** The linker emits a bitmap of valid indirect-call targets (function entry
points). Every indirect call is preceded by a call to `_guard_check_icall` /
`__guard_dispatch_icall_fptr`, which looks the target up in the bitmap, provided by the OS loader.
A corrupted function pointer or vtable entry that points into the middle of a function or into
data fails the check and the process fail-fasts with `0xC0000409` (subcode `FAST_FAIL_GUARD_ICALL_
CHECK_FAILURE`). In a dump this looks different from the access violation you would otherwise get
- module 07 shows how to decode the subcode. Intel CET's indirect branch tracking (`endbr64`
instructions at valid targets) is the hardware version on Linux.

**How libFuzzer finds the bug so fast.** `-fsanitize=fuzzer` adds SanitizerCoverage: a callback
or counter at every edge of the control-flow graph, plus comparison tracing (`__sanitizer_cov_
trace_cmp`) that records operands of `==` and `switch`. When an input reaches a new edge, it is
kept in the corpus. Comparison tracing lets the fuzzer see "the code compared byte 1 with `':'`"
and insert `':'` there, so it gets through the format checks in a handful of iterations instead of
random luck. Then it mutates the account field length upward until ASan reports.

**Why static analysis has false positives and negatives.** Path-sensitive analysers explore paths
symbolically and must cut off (loop unrolling bounds, inter-procedural depth, unknown library
behaviour). Cutting off loses real bugs (false negatives); approximating unknown values as "anything"
reports paths that cannot happen (false positives). SAL annotations on Windows and
`[[gnu::nonnull]]`, `[[nodiscard]]`, ownership types (`unique_ptr`, `gsl::owner<T*>`) in C++ give
the analyser facts it cannot infer, which reduces both.

**Assertions and the optimiser.** `assert(p != nullptr)` before `*p` documents the precondition; in
release (`NDEBUG`) it disappears. An always-on check followed by `abort()` (a `noreturn` function)
lets the optimiser assume the condition after the check, which can make later code *faster*. The
dangerous variant is `__builtin_unreachable()` / `[[assume]]` used as an assertion: if the
condition is false, behaviour is undefined, and the optimiser may delete your safety checks. Never
use assume-style hints for conditions that depend on input.

## 5. In production

- **Legacy C code is where your bank servers live.** You will not rewrite it in C++ with RAII.
  The realistic path: warnings on and fixed module by module; `-D_FORTIFY_SOURCE` and stack
  protector in the release build (often a zero-code-change win); ASan in the test environment;
  fuzz targets for the parsers that take external input; clang-tidy/cppcheck/`/analyze` with a
  baseline so new code is clean; RAII-style wrappers for every *new* C++ component.
- **Hardening changes the crash signature.** After enabling `_FORTIFY_SOURCE` or CFG, some old
  "random" crashes turn into `*** buffer overflow detected ***` or `0xC0000409`. That is the flag
  working: it has turned a heap-corruption investigation into a one-line fix. Tell operations to
  expect it, and treat the first such crash as a gift.
- **ASan in a staging environment.** An ASan build of the whole server in a staging environment
  that replays production traffic (anonymised) finds bugs no unit test reaches. Memory cost is
  about 2-3x and CPU about 2x (typical figures from the ASan documentation); a staging box can
  afford it.
- **Release builds keep some checks on.** `_GLIBCXX_ASSERTIONS`, always-on asserts for money
  invariants, and the stack protector cost little. Turning them off for "performance" without a
  profile showing they matter trades a cheap abort-with-core for an expensive corruption incident.
- **Review checklists rot if they are long and generic.** Keep the checklist in the merge-request
  template, and add a line every time an RCA finds a class of bug the checklist did not cover. The
  checklist then becomes the memory of every incident.
- **Measure the layers.** In RCAs, record which layer *should* have caught the bug and why it did
  not. After a year, the pattern tells you where to invest (e.g. "6 of 9 incidents were parsing
  bugs: fuzzing first").

## 6. Try it

1. **Easy - break it back.** In a copy of `m13_raii_fix.cpp`, reintroduce one bug at a time: `[]`
   with `i <= size()` instead of `.at()`; a raw `new` with an early return; a raw `Session*` kept
   after `close`. For each, record which layer catches it: strict warnings, `_GLIBCXX_ASSERTIONS`,
   ASan, LSan, or none. Done when you have a 3-row table with an observed (not guessed) answer per
   cell.
2. **Easy - checksec.** Build `m04_heap_overflow.cpp` twice: with no flags and with the `HARDEN`
   set from 3.4. Run `checksec --file=` (from the `checksec` package) or `readelf -lW`/`-d` on both.
   Done when you can point at PIE, RELRO, canary, NX and FORTIFY in the output and say which flag
   produced each.
3. **Medium - fuzz and fix.** Build and run the fuzzer from 3.2, save the crash input, turn it into
   a unit test in the plain build, fix the bound, re-run the fuzzer for 10 minutes. Done when the
   crash input passes, the fuzzer runs clean, and you can explain why the plain build's four
   samples did not show the bug.
4. **Medium - FORTIFY in action.** Write a 10-line program that `strcpy`s `argv[1]` into the
   `char buf[16]` member of a struct allocated with `new` (taking it from `argv` avoids a
   compile-time `-Wstringop-overflow` error). Run it with a 20-character argument. Build with `-O2`
   with `-U_FORTIFY_SOURCE` and again with `-D_FORTIFY_SOURCE=2`. Done when one build prints `*** buffer overflow detected ***` and you can
   show `__strcpy_chk` in `objdump -d` and `bt` of the core.
5. **Hard - static analysis baseline.** Run clang-tidy and cppcheck on every file in `code/`. Make a
   table of which tool found which deliberate bug and which bugs no static tool found. Then write a
   script that fails only if the finding count increases relative to a saved baseline. Done when
   adding a new `strcpy` to any file makes the script fail and nothing else does.
6. **Hard - fuzz a real parser.** Write a fuzz target for a parser you have written or used (an RPC
   message decoder, a fixed-width record reader, a config parser), with a seed corpus of three valid
   inputs. Run it for an hour with ASan and UBSan. Done when you have either a found bug with a
   reproduction test, or the coverage report (`-print_final_stats=1`, or `-fprofile-instr-generate`
   with `llvm-cov`) showing which branches the fuzzer reached and which it did not.

## 7. Interview questions

**Q: How does RAII prevent memory bugs?**
A: It ties every resource to an object whose destructor releases it, so release happens exactly
once on every exit path, including early returns and exceptions. That eliminates leaks on error
paths, double frees (one owner via `unique_ptr`), unreleased locks (`lock_guard`), and with clear
ownership and ids instead of stored raw pointers, most single-threaded use-after-free.

**Q: What does `_FORTIFY_SOURCE` do and what are its limits?**
A: It redirects functions like `memcpy`, `strcpy` and `sprintf` to checked variants that know the
destination size from `__builtin_object_size` and abort on overflow. It only works with
optimisation on and only when the compiler can determine the destination size (`=3` extends this to
some runtime sizes); it does not check hand-written loops or indexed writes.

**Q: What is fuzzing and why does it find bugs unit tests miss?**
A: Fuzzing runs a function on millions of generated inputs and reports crashes, hangs and sanitizer
errors. Coverage-guided fuzzers keep inputs that reach new code and mutate them, and comparison
tracing lets them satisfy format checks, so they explore edge cases (over-length fields, empty
values, odd encodings) that people do not think to write tests for. With ASan on, they find silent
overflows, not just crashes.

**Q: Assertion versus error handling - when do you use which?**
A: Error handling is for conditions that can legitimately happen: bad input, network failure, a full
disk; they get an error response and a log line. Assertions are for conditions that can only be
false if the program has a bug - a broken invariant; the safest response is to stop with a core
close to the cause. Asserting on input turns a malformed message into a denial of service.

**Q: What hardening flags do you put on a production C++ build on Linux and on Windows?**
A: Linux: `-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection
-fcf-protection=full -fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack`, often
`-D_GLIBCXX_ASSERTIONS`. Windows: `/GS`, `/guard:cf`, `/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT`, and
`/CETCOMPAT` where supported. They turn overflows and control-flow hijacks into immediate, clean
aborts.

**Q: How do you introduce static analysis on a large legacy codebase?**
A: Generate a compilation database, start with a small set of high-signal checks, record a baseline
of existing findings, and fail CI only on new findings in changed code. Fix old findings
opportunistically, module by module, and make suppressions local with a reason. This keeps the
signal high and avoids a wall of warnings everyone ignores.

**Q: What is on your memory-safety code review checklist?**
A: Single ownership and no naked new/delete; no stored raw pointers or iterators across
invalidating operations; every copy bounded by destination size and every wire length validated;
size arithmetic overflow-checked; everything initialised; error paths release resources; shared
state protected by a named lock with a global lock order; signal handlers async-signal-safe; new
parsers fuzzed and new concurrent code run under TSan.

## 8. Recap

- Defence in layers: types that make bugs impossible, warnings, assertions, sanitizers, fuzzing,
  static analysis, hardening, review. Earlier layers are cheaper.
- RAII: one owner, destructors release, no naked new/delete; ids or `weak_ptr` instead of stored
  raw pointers; `scoped_lock` for multiple locks.
- Bounds: sized types, `.at()` for input-derived indexes, `_GLIBCXX_ASSERTIONS`, validate lengths at
  the boundary, never `strcpy`/`sprintf`, overflow-check size arithmetic.
- Assertions are for bugs, error handling for input; always-on asserts for money and memory
  invariants; never assume-style hints on input-dependent conditions.
- libFuzzer + ASan + UBSan on every parser of external input; crash inputs become unit tests.
- Static analysis (clang-tidy, cppcheck, MSVC `/analyze` with SAL) with a baseline; warnings as
  errors with a strict set.
- Hardening: `_FORTIFY_SOURCE`, stack protector, stack clash protection, PIE/RELRO/NX, CET; MSVC
  `/GS`, CFG, ASLR, DEP, CET. They turn silent corruption into clean, diagnosable aborts.
- The review checklist is the memory of every incident: add a line after every RCA.
