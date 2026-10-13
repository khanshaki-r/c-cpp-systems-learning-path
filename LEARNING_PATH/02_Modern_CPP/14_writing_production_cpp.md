# 14. Writing production C++

**Time:** ~2.5 hours   **Prerequisites:** all previous modules

## 1. In plain words

Knowing the language is half the job. The other half is the set of habits that make a C++ codebase
survive five years and twenty engineers: how files are split, how the build is configured so the
compiler finds bugs for you, how formatting and static analysis are automated so review time goes to
design, what a reviewer looks for, and how an API is shaped so it is hard to misuse. This module
collects those habits and ends with the API-design rules the tcpkv project will apply.

Analogy: a kitchen. The recipes are the language; the mise en place, labelled containers, sharp
knives and a cleaning routine are what let the kitchen serve 300 covers a night without anyone
getting hurt.

## 2. The real thing

### 2.1 Project structure

```
tcpkv/
  CMakeLists.txt
  include/tcpkv/        public headers, one per public class or concept: kv_store.hpp, socket_fd.hpp
  src/                  implementation .cpp files, private headers
  tests/                one test file per header; a tiny main or Catch2/GoogleTest
  bench/                micro-benchmarks (Google Benchmark)
  tools/                clang-format, clang-tidy configs, scripts
  third_party/          vendored or FetchContent dependencies
```

Public headers live under a namespace-named directory so includes read `#include <tcpkv/kv_store.hpp>`
and cannot collide with other libraries. Everything in a public header is API; everything else is free
to change.

### 2.2 Headers vs sources

- A header declares; a source defines. Exceptions: templates, `inline`/`constexpr` functions, and
  class definitions (which are declarations of the type, with inline member function bodies allowed).
- Every header must be **self-contained**: it includes what it uses and compiles on its own
  (`g++ -fsyntax-only header.hpp` or a per-header test). Never rely on the includer having included
  something first.
- Include order in a `.cpp`: its own header first (proves self-containment), then project headers,
  then third-party, then standard library. Alphabetical within groups; clang-format does it.
- Forward-declare (`class Connection;`) instead of including when you only need a pointer or
  reference. It cuts compile time and coupling.
- **Include guards.** `#pragma once` is supported by every compiler you will use and is simpler than
  `#ifndef TCPKV_KV_STORE_HPP`. Either is fine; pick one for the project.
- Minimise what a header exposes: private implementation details go in the `.cpp` or behind a
  **pimpl** (`std::unique_ptr<Impl> impl_;`) when ABI stability or compile-time isolation matters.

### 2.3 The One Definition Rule (ODR)

- A non-inline function or variable must be defined exactly once in the whole program.
  Two definitions = linker error ("multiple definition").
- A class, inline function, template, or `inline` variable may be defined in several translation
  units, but **every definition must be token-for-token identical** with the same meaning. If they
  differ (same class name, different members, in two `.cpp` files; a header whose content depends on a
  macro that differs between TUs), the program is ill-formed with **no diagnostic required**: the linker
  picks one at random and the other TU reads garbage.
- Anonymous namespaces (`namespace { ... }`) or `static` give internal linkage: safe to define the same
  name differently in different `.cpp` files.
- Practical ODR hygiene: no `#define` that changes a header's layout (`NDEBUG` affecting `struct`
  members is the classic), no `using namespace` in headers, unique names for helper types in `.cpp`
  files (anonymous namespace), same compiler flags for every TU in a binary (`-std`, `-D_GLIBCXX_DEBUG`,
  `-fno-rtti`).

### 2.4 Build flags that find bugs

```
-std=c++17 -Wall -Wextra -Werror -pedantic
-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
-Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
Debug:   -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer   (and -D_GLIBCXX_DEBUG for container checks)
Release: -O2 -g -DNDEBUG   (symbols split out with objcopy)
Thread tests: -fsanitize=thread (separate build; incompatible with ASan)
```

`-Werror` from day one. Warnings that are tolerated become warnings that are ignored.

### 2.5 clang-format and clang-tidy

- **clang-format** fixes layout mechanically. Commit a `.clang-format` (start from
  `BasedOnStyle: Google` or `LLVM`, set `ColumnLimit`, `IndentWidth`), run it in a pre-commit hook or
  CI check. Formatting never appears in review again.
- **clang-tidy** is the static analyser. A reasonable `.clang-tidy` enables
  `bugprone-*, performance-*, modernize-*, readability-*, cppcoreguidelines-*` minus a few noisy ones.
  Key checks: `bugprone-use-after-move`, `bugprone-dangling-handle` (string_view), `performance-unnecessary-copy-initialization`,
  `performance-for-range-copy`, `modernize-use-nullptr`, `cppcoreguidelines-owning-memory`,
  `misc-non-private-member-variables-in-classes`. Run on changed files in CI
  (`clang-tidy -p build/ file.cpp` with a `compile_commands.json` from CMake).
- **cppcheck** and **include-what-you-use** are useful extras; sanitisers (module 12) cover the
  runtime side.

### 2.6 C++ Core Guidelines highlights

The Core Guidelines (Stroustrup, Sutter; isocpp.github.io/CppCoreGuidelines) are long; these are the
rules that matter most for your kind of code:

- **P.4** Ideally, a program should be statically type safe. **ES.46** Avoid narrowing conversions.
- **R.1** Manage resources automatically using RAII. **R.3** A raw pointer is non-owning.
  **R.11** Avoid calling `new` and `delete` explicitly. **R.20/21** Prefer `unique_ptr` over
  `shared_ptr` unless ownership is shared.
- **C.20** If you can avoid defining default operations, do (rule of zero). **C.21** If you define any,
  define all (rule of five). **C.35** A base class destructor should be public and virtual, or protected
  and non-virtual. **C.46** `explicit` for single-argument constructors. **C.128** Virtual functions
  specify exactly one of `virtual`, `override`, `final`.
- **F.15/F.16** Pass cheap types by value, others by `const&`. **F.20** Return outputs by value.
  **F.21** Return multiple values as a struct or tuple. **F.43** Never return a pointer/reference to a local.
- **E.2/E.3** Throw to signal that a function cannot perform its task; use exceptions for errors only.
  **E.6** Use RAII to prevent leaks. **E.12** `noexcept` when throwing is impossible or unacceptable.
- **ES.20** Always initialise an object. **ES.23** Prefer `{}` initialisation. **ES.50** Do not cast
  away `const`. **ES.100/101** Do not mix signed and unsigned arithmetic.
- **CP.1** Assume your code will run as part of a multithreaded program. **CP.20** Use RAII, never
  plain `lock()`/`unlock()`. **CP.21** Use `std::lock` or `std::scoped_lock` to acquire multiple mutexes.
- **SF.7** Do not write `using namespace` at global scope in a header. **SF.8** Use include guards.
  **SF.11** Header files should be self-contained.
- **Con.1/Con.4** By default, make objects immutable; use `const` to define objects whose values do not change.

### 2.7 Code review checklist

Ownership and lifetime
- Every raw pointer: non-owning and the owner outlives it? Every `new`: inside a smart pointer? Every
  `release()`: documented hand-off?
- References/views (`string_view`, `span`, iterators) stored in members or captured in lambdas: does
  the source outlive them?
- Rule of zero or full rule of five; moves `noexcept`; moved-from state valid.

Correctness
- `override` on every override; virtual destructor on polymorphic bases.
- Comparators are strict weak orderings; `std::tie` for multi-field.
- Containers: iterator invalidation across the loop body? `operator[]` on a map in a read path?
- Signed/unsigned mixing, narrowing, uninitialised members, order of member initialisers.
- Exceptions: what guarantee does this function give? `noexcept` only where true. Destructors do not throw.
- Thread safety: which mutex protects this member? Is it stated in a comment next to the member?

API
- `explicit` single-arg constructors; `[[nodiscard]]` on results that must be checked; `const` wherever possible.
- Parameters: `string_view`/`const&` to read, by value to sink, pointer only when nullable.
- No `bool` parameters that make call sites unreadable (`send(data, true, false)`): use an `enum class`.

Hygiene
- Header self-contained, no `using namespace` in headers, no macros where `constexpr`/`inline` works.
- Names say what things are; comments say why, not what.
- Tests exist for the new behaviour and for the failure path.

### 2.8 Reading cppreference

cppreference.com is the reference, not a tutorial. For any function: read the **signature** (which
overloads, `const`, `noexcept`), the **parameters** and **return value**, then the **complexity**, the
**exceptions** section (what guarantee), the **iterator invalidation** notes for containers, and the
**"since C++NN"** markers so you do not use a C++20 feature in a C++17 build. The example at the bottom is
compilable; paste it to verify behaviour. For library headers, the top-level page per header lists what
it provides. For the language, the pages on value categories, lifetime, and undefined behaviour are
worth reading once in full.

### 2.9 API design for a library (what tcpkv needs)

1. **Make ownership visible in signatures.** Factory returns `std::unique_ptr<Connection>` or a
   move-only value; functions borrowing take `Connection&`; nothing takes or returns an owning raw pointer.
2. **Make misuse a compile error.** `explicit` constructors; deleted copy on identity types;
   `[[nodiscard]]` on error codes and handles; `enum class` for options; strong typedefs
   (`struct Port { std::uint16_t value; }`) instead of bare integers where they could be swapped.
3. **Make the common case the default.** Sensible defaults in constructors; the simplest call does the
   right thing; advanced knobs behind an options struct, not a 9-parameter function.
4. **Choose the error-reporting style per layer** (module 10) and state it in the header comment.
   tcpkv: constructors throw `std::system_error`; `get`/`put`/`erase` return values; nothing on the request
   path throws for client errors.
5. **Return copies or values from shared state,** never references into a container another thread may
   mutate (the `KvStore::get` rule).
6. **Keep the ABI surface small.** Public headers expose the minimum; pimpl for anything that may change
   its members; no `std::` types across a C ABI (if tcpkv ever exposes a C API, it is `extern "C"` with
   opaque handles and plain types).
7. **Thread-safety contract in the header.** "All public member functions are safe to call concurrently"
   or "externally synchronised" per class, next to the class comment.
8. **Document complexity and allocation.** `get` is O(1) average and allocates one `std::string` for
   the returned copy; `put` may rehash.
9. **Test the API from the outside.** A test that uses only the public header catches leaks of
   implementation details and verifies the header is self-contained.

## 3. Code

`code/kv_store.hpp` is a public header written to these rules, and `code/m14_library_api_demo.cpp`
plays the role of both `kv_store.cpp` (implementation) and `main.cpp` (a user) in one file so the
example builds with one command:

```cpp
// kv_store.hpp
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tcpkv {
class KvStore {
public:
    KvStore() = default;
    explicit KvStore(std::size_t expected_keys);          // explicit: 'KvStore s = 100;' must not compile

    KvStore(const KvStore&) = delete;                     // identity type: copying is a bug, so it is an error
    KvStore& operator=(const KvStore&) = delete;
    KvStore(KvStore&&) noexcept = default;                // but moving is fine and cheap
    KvStore& operator=(KvStore&&) noexcept = default;
    ~KvStore() = default;

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;   // copy out, never a reference in
    bool put(std::string_view key, std::string value);                          // sink parameter for the value
    [[nodiscard]] bool erase(std::string_view key);                             // the caller must look at the answer
    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    void clear() noexcept { map_.clear(); }
private:
    std::unordered_map<std::string, std::string> map_;
};
}  // namespace tcpkv
```

```cpp
// what would be kv_store.cpp
std::optional<std::string> KvStore::get(std::string_view key) const {
    auto it = map_.find(std::string(key));    // C++17 cost: materialise a key; C++20 heterogeneous lookup removes it
    if (it == map_.end()) return std::nullopt;
    return it->second;
}
bool KvStore::put(std::string_view key, std::string value) {
    auto [it, inserted] = map_.insert_or_assign(std::string(key), std::move(value));
    return inserted;
}

// what would be main.cpp
// store.erase("acct:1");                  // error under -Werror: [[nodiscard]] value ignored
bool erased = store.erase("acct:1");
tcpkv::KvStore moved = std::move(store);   // allowed
// tcpkv::KvStore copy = moved;            // error: deleted
static_assert(!std::is_copy_constructible_v<tcpkv::KvStore>);
static_assert(std::is_nothrow_move_constructible_v<tcpkv::KvStore>);
```

Try to misuse the API (copy it, ignore `erase`'s result, construct from a bare integer) and read the
three compile errors. That is the API doing the reviewer's job.

A minimal `CMakeLists.txt` for the layout in 2.1:

```cmake
cmake_minimum_required(VERSION 3.16)
project(tcpkv CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)            # compile_commands.json for clang-tidy

add_library(tcpkv src/kv_store.cpp src/socket_fd.cpp)
target_include_directories(tcpkv PUBLIC include)
target_compile_options(tcpkv PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wnon-virtual-dtor)
find_package(Threads REQUIRED)
target_link_libraries(tcpkv PUBLIC Threads::Threads)

add_executable(tcpkv_server src/main.cpp)
target_link_libraries(tcpkv_server PRIVATE tcpkv)

enable_testing()
add_executable(kv_store_test tests/kv_store_test.cpp)
target_link_libraries(kv_store_test PRIVATE tcpkv)
add_test(NAME kv_store COMMAND kv_store_test)
```

## 4. Under the hood (senior-level)

**What the linker does with inline and template definitions.** Each TU emits inline functions,
template instantiations, inline variables and vtables as weak symbols in COMDAT groups (sections named
`.text._Z...`, with `.section ... ,"axG",@progbits,<name>,comdat`). The linker keeps the first group
with a given name and discards the rest. It does **not** compare contents: if two TUs produced
different code for the same symbol (an ODR violation), one TU's callers silently run the other's
code. `-Wodr` with `-flto` is the only compile-time check; otherwise it shows as "impossible" behaviour.

**Why a header-only change rebuilds everything.** Each `.cpp` is preprocessed into one giant TU
containing every included header, and `make`/`ninja` rebuild a TU when any of its dependencies'
timestamps change (dependencies are tracked with `-MMD`). Touching a header included by 300 files
costs 300 compilations. Forward declarations, pimpl, and smaller headers are compile-time
engineering. Precompiled headers and C++20 modules attack the same problem.

**`#pragma once` vs guards.** Guards are pure preprocessor; the compiler still opens and tokenises the
file to find the `#endif` on later includes (GCC optimises this away when it recognises the guard
pattern). `#pragma once` lets the compiler skip the file by identity (inode/path), which is why it is
not in the standard: file identity is not portable in theory. In practice both are fine.

**Symbol visibility.** By default every non-static symbol in a shared library is exported, making the
`.so` large and every type part of the ABI. `-fvisibility=hidden` plus `__attribute__((visibility("default")))`
(or a `TCPKV_API` macro) on the public API shrinks the export table, speeds up loading, and lets the
compiler optimise internal calls as if they were static. It is the Linux equivalent of
`__declspec(dllexport)` on Windows.

**`-Werror` and compiler upgrades.** New GCC versions add warnings to `-Wall`; a `-Werror` build can
break on a compiler upgrade without any code change. Pin the compiler version in CI and bump it
deliberately, fixing the new warnings in the same change.

**clang-tidy's model.** It runs the Clang front end on each TU using the exact flags from
`compile_commands.json`, builds the AST, and matches checks against it. It therefore sees templates
only as instantiated in that TU and sees macros expanded. It is slow (seconds per file); run it on the
diff in CI and on the whole tree nightly.

**Sanitiser interaction with the ODR.** ASan has an ODR-violation detector for global variables with
the same name in different TUs (`detect_odr_violation=2`); it fires at start-up and is one of the few
runtime checks for the problem.

**Debug symbols and `-g`.** DWARF debug info can be several times the size of the code. `-g` does not
change the generated machine code at any `-O` level; build release with `-O2 -g`, then
`objcopy --only-keep-debug server server.debug; strip --strip-debug server; objcopy --add-gnu-debuglink=server.debug server`.
gdb finds the `.debug` file by the link (and by build-id in `/usr/lib/debug`). You get symbolised
core dumps from production without shipping symbols.

## 5. In production

- **Day one checklist for tcpkv (or any new C++ project):** `-std=c++17 -Wall -Wextra -Werror`,
  a `.clang-format`, a `.clang-tidy`, a sanitiser CI job, a test target, `compile_commands.json`,
  and a `README` section titled "Error handling policy" and "Thread-safety conventions". It takes an
  hour and saves weeks.
- **The header is the contract.** When reviewing, read the header first and ask: can I misuse this? If
  a reviewer needs to read the `.cpp` to know who owns a pointer or whether a function throws, the
  header is incomplete.
- **Review for lifetime first.** Most severe C++ bugs are lifetime bugs (dangling reference, use after
  move, iterator invalidation, view outliving its string). Logic bugs are caught by tests; lifetime bugs
  often are not, so reviewers should spend their attention there.
- **Fix warnings at the source, never with casts.** A `-Wsign-conversion` warning on
  `v[i]` with `int i` is fixed by making `i` a `std::size_t`, not by `static_cast<size_t>(i)` at the use site.
- **Treat clang-tidy findings as bugs until proven otherwise.** `NOLINT` with a reason is acceptable;
  bare `NOLINT` is a review finding.
- **Keep the build reproducible.** Same compiler, same flags, same standard library for every TU;
  pinned third-party versions; no "works on my machine" `-D` flags. ODR violations and ABI mismatches
  come from inconsistency, not from the language.
- **3 AM debugging.** "Impossible" crash in code that has not changed: check whether all objects were
  built with the same flags after a header changed (stale object files, two definitions of a struct with
  different `#ifdef`s). `nm -C` the objects and compare the symbol list; `-Wodr -flto` on a clean build
  usually names the culprit.

## 6. Try it

1. **(easy)** Split `m14_library_api_demo.cpp` into `kv_store.cpp` and `main.cpp`, build with the
   `CMakeLists.txt` above, and run `clang-tidy -p build src/kv_store.cpp`. Done when the build is
   warning-free and you have read every tidy finding.
2. **(easy)** Create an ODR violation on purpose: define `struct Config { int a; };` in one `.cpp`
   and `struct Config { int a; int b; };` in another, each with a function that constructs and prints
   it, both non-inline. Link and run. Done when you can explain the (lack of) diagnostic and have seen
   `-flto -Wodr` catch it.
3. **(medium)** Add a `SocketFd` (module 03) header to the tcpkv layout, write a test that uses only the
   public header, and add `-fvisibility=hidden` with a `TCPKV_API` macro. Done when `nm -D libtcpkv.so`
   lists only the intended symbols.
4. **(medium)** Take any 200-line C++ file you have written during this course, run clang-tidy with
   `bugprone-*,performance-*,modernize-*,readability-*`, and fix every finding without `NOLINT`. Done
   when it is clean and you can name the three most common findings.
5. **(hard)** Write the public header for tcpkv's `Connection` class (move-only, owns a `SocketFd`,
   `read_message()` returning `std::optional<Message>` or an `error_code`, `write(std::string_view)`
   returning `error_code`), with the thread-safety contract and error policy in comments. Have someone
   (or a later you) try to misuse it from a test. Done when every misuse you can think of is a compile
   error or a documented behaviour.
6. **(hard)** Set up a GitHub Actions (or local script) pipeline: clang-format check, build with
   `-Werror`, tests under ASan+UBSan, tests under TSan, clang-tidy on the diff. Done when a deliberate
   use-after-move and a deliberate data race each fail the pipeline with a readable report.

## 7. Interview questions

**Q: What is the One Definition Rule?**
Every non-inline function and variable must be defined exactly once in the program (the linker
enforces this). Classes, templates, inline functions and inline variables may be defined in many
translation units but every definition must be identical; if not, the program is ill-formed with no
diagnostic required, and the linker silently picks one definition.

**Q: What makes a header self-contained and why does it matter?**
It includes everything it uses and compiles on its own. Otherwise it works only when included after
some other header, which breaks when include order changes and hides the real dependency graph. The
"own header first" include order in `.cpp` files tests it on every build.

**Q: `#pragma once` or include guards?**
Both prevent double inclusion. `#pragma once` is non-standard but universally supported and uses file
identity; guards are standard but need a unique macro per file. Pick one per project; the choice is
not worth a debate.

**Q: How do you keep a shared library's ABI stable?**
Hide implementation behind a pimpl or opaque handle, do not expose standard-library types or
templates across the boundary, only append virtual functions (or avoid virtual in the ABI), use
`-fvisibility=hidden` with an explicit export macro, and never change the layout of exported structs
without a version bump.

**Q: What do you look for first in a C++ code review?**
Lifetime and ownership: who owns each pointer, do references/views outlive their source, use after
move, iterator invalidation, thread-safety of shared members. Then the API shape (can it be misused?),
then exception guarantees, then style, which should already be automated.

**Q: What is the role of clang-tidy versus the compiler's warnings?**
Compiler warnings catch local, syntactic issues quickly. clang-tidy does deeper, slower semantic
analysis on the AST: use after move, dangling `string_view`, unnecessary copies, modernisation,
Core Guidelines rules. Run warnings on every build and tidy on the diff in CI.

**Q: How do you ship optimised binaries but still debug core dumps?**
Build with `-O2 -g`, split the DWARF info into a separate file with `objcopy --only-keep-debug`,
strip the binary, add a `.gnu_debuglink`, and keep the debug file by build-id. gdb loads it when
analysing the core.

**Q: Name five Core Guidelines rules you apply every day.**
Manage resources with RAII (R.1); a raw pointer is non-owning (R.3); rule of zero or rule of five
(C.20/C.21); `explicit` single-argument constructors (C.46); virtual functions specify exactly one of
`virtual`/`override`/`final` (C.128); return outputs by value (F.20); never return a reference to a
local (F.43); use RAII for locks (CP.20).

## 8. Recap

- Layout: `include/<project>/` public headers, `src/`, `tests/`, `bench/`; headers declare, sources define.
- Headers self-contained, own header first, forward-declare where possible, `#pragma once` or guards.
- ODR: one definition for non-inline entities; identical definitions for inline/templates/classes; violations
  are undiagnosed. Same flags for every TU.
- `-std=c++17 -Wall -Wextra -Werror` plus `-Wshadow -Wconversion -Wnon-virtual-dtor`; sanitisers in CI;
  `-O2 -g` with split debug info in release.
- clang-format removes style from review; clang-tidy finds use-after-move, dangling views, copies.
- Review lifetime first, then API misuse, then guarantees; style is automated.
- Core Guidelines: RAII, non-owning raw pointers, rule of zero/five, `explicit`, `override`, return by value.
- cppreference: read signature, complexity, exceptions, invalidation, "since C++NN".
- API design: ownership in signatures, misuse as compile error, defaults for the common case, stated
  error policy and thread-safety contract, copies out of shared state, small ABI surface.
