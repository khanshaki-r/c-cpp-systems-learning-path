# 13. C++17 features tour

**Time:** ~2 hours   **Prerequisites:** 06 to 11

## 1. In plain words

C++11 was the big change (move semantics, lambdas, smart pointers, `auto`, threads). C++14 polished
it. C++17 is a collection of medium-sized conveniences that make daily code shorter and safer:
unpacking a pair into two names, declaring a variable inside an `if`, passing strings without copying,
saying "this function's result must not be ignored", deducing template arguments from constructor
calls. Plus a few new library types (`optional`, `variant`, `any`, `string_view`, `filesystem`) and
`std::shared_mutex`, the reader-writer lock your tcpkv store uses.

This module is the checklist: each feature, what it replaces, the one trap. You have met several
already; here they are collected so you can say "C++17" on a resume and mean every item.

## 2. The real thing

### 2.1 Structured bindings

```cpp
auto [it, inserted] = map.insert({key, value});      // replaces std::pair<iterator,bool> r = ...; r.first; r.second
for (const auto& [name, age] : ages) ...             // unpack map entries
auto [count, mean] = compute(v);                     // any struct with public members, std::tuple, std::array, C array
```

The names are aliases into a hidden copy (or reference, with `auto&`/`const auto&`) of the whole
object. You cannot skip a field or nest them; unused names do not warn.

### 2.2 `if` / `switch` with initialiser

```cpp
if (auto it = m.find(k); it != m.end()) use(*it); else miss();    // 'it' scoped to the if/else
if (std::lock_guard lock(mu); !queue.empty()) pop();              // lock scoped to the statement
switch (auto op = next_op(); op) { ... }
```

Replaces a variable declared on the line before whose scope leaks to the end of the block.

### 2.3 `std::string_view`

A `(const char*, size_t)` pair. Take it as a parameter wherever you used to take `const std::string&`
and only read: callers with literals, `char*`, `std::string`, or substrings pay nothing.
`remove_prefix`, `substr` (O(1), no copy), `find`, comparison operators all work.
Traps: not null-terminated (do not pass `.data()` to C APIs expecting a C string); does not own
(never store a view to a temporary or to a buffer that will be reused); `std::string s = sv;` is an
explicit copy by design.

### 2.4 `std::optional`, `std::variant`, `std::any`

Covered in module 10. `optional<T>`: maybe a `T`, inline. `variant<Ts...>`: exactly one of several
types, inline, `std::visit`. `any`: any copyable type, type-erased, heap for large payloads,
`any_cast` checks at run time. Use `variant` when the set of types is known; `any` is for genuinely
open-ended plugin data and is rare in good code.

### 2.5 `std::filesystem`

Portable paths and file operations: `fs::path` with `/` joining, `exists`, `file_size`,
`create_directories`, `remove_all`, `directory_iterator`, `recursive_directory_iterator`,
`temp_directory_path`, `rename`, `last_write_time`. Every operation has a throwing overload and an
`std::error_code&` overload. GCC 9+ needs no extra library; GCC 8 needed `-lstdc++fs`.

### 2.6 Parallel algorithms

`std::sort(std::execution::par, b, e)`, also `par_unseq`, `seq`, for most algorithms plus
`std::reduce`, `std::transform_reduce`, `std::exclusive_scan`. On GCC the implementation is Intel TBB:
`#include <execution>` and link with `-ltbb` (`apt install libtbb-dev`). Your callable must be
thread-safe and must not throw (an exception in a parallel algorithm calls `std::terminate`).
Speed-ups appear for large inputs with real work per element; a 10k-element sort is faster serial.

### 2.7 Fold expressions and variadic improvements

`(args + ...)`, `(std::cout << ... << args)`, `((xs > 0) && ...)` (module 08). Also
`if constexpr`, and `auto` as a non-type template parameter.

### 2.8 Inline variables

`inline constexpr int kVersion = 3;` in a header: one object across all translation units
(module 11). `static constexpr` members no longer need an out-of-class definition.

### 2.9 Attributes: `[[nodiscard]]`, `[[maybe_unused]]`, `[[fallthrough]]`

- `[[nodiscard]]` on a function (or a type): discarding the return value is a warning, an error with
  `-Werror`. Put it on anything returning an error code, a `bool` success flag, a handle, or a value
  whose computation is the only effect (`empty()`, `size()`).
- `[[maybe_unused]]` silences unused-variable/parameter warnings for things used only under `#ifdef`
  or in `assert`.
- `[[fallthrough]];` documents an intentional `case` fall-through so `-Wimplicit-fallthrough` stays quiet.

### 2.10 Class template argument deduction (CTAD)

`std::pair p{1, 2.5};`, `std::vector v{1, 2, 3};`, `std::lock_guard lock(mu);`,
`std::shared_lock lock(rw_mutex);`, `std::tuple t{1, 'c', "s"};`. Deduction follows constructor
parameters or explicit deduction guides (module 10's `overloaded` uses one). Trap:
`std::vector v{other_vector};` deduces `vector<int>` (copy), not `vector<vector<int>>`.

### 2.11 `std::byte`

An `enum class` over `unsigned char` with only bitwise operators: raw memory that cannot be
accidentally added or printed as a character. `std::to_integer<int>(b)` converts explicitly.
Use it for buffers and arena storage (`std::byte buf[4096]`); it aliases anything like `char`.

### 2.12 `std::shared_mutex` and `std::scoped_lock`

`std::shared_mutex` (C++17; `shared_timed_mutex` was C++14): many threads may hold it in shared
mode (`std::shared_lock`), or one in exclusive mode (`std::unique_lock`). Right for read-mostly
data (configuration, a key-value cache) where readers outnumber writers heavily. `std::scoped_lock`
locks several mutexes at once in a deadlock-free order. Both are covered in depth in
04_Concurrency/03_mutexes_lock_guard_unique_lock.md; here you get the shape of the tcpkv store.

### 2.13 Smaller items

Guaranteed copy elision (module 04), `std::size`/`std::data`/`std::empty` free functions,
`std::clamp`, `std::gcd`/`std::lcm`, `std::apply`, `std::invoke`, `std::not_fn`, `std::as_const`,
`std::uncaught_exceptions`, `__has_include`, hexadecimal floating literals, `u8` character literals,
`static_assert` without a message, nested namespaces `namespace a::b::c {}`, `std::map::try_emplace`
and `insert_or_assign`, `extract`/`merge` of map nodes, `std::sample`, `std::launder`,
`std::aligned_alloc`, `std::hardware_destructive_interference_size`, removal of `auto_ptr`,
`random_shuffle`, trigraphs, `register` and dynamic exception specifications.

## 3. Code

`code/m13_cpp17_tour.cpp` touches every language-level item:

```cpp
inline constexpr int kProtocolVersion = 3;

auto [count, mean] = compute({1.0, 2.0, 6.0});
for (const auto& [name, age] : ages) ...
if (auto [it, inserted] = ages.insert({"ann", 99}); !inserted) ...
if (auto it = ages.find("bob"); it != ages.end()) ... else ...

std::string_view trim(std::string_view s) {              // O(1) slicing, no allocation
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

std::pair p{1, 2.5};  std::vector words{std::string("a"), std::string("b")};  std::tuple t{1, 'c', "txt"};
template <class... Ts> constexpr bool all_positive(Ts... xs) { return ((xs > 0) && ...); }

[[nodiscard]] bool try_reserve(std::vector<int>& v, std::size_t n);
[[maybe_unused]] bool ok = try_reserve(buf, 2'000'000);  assert(!ok);

std::any a = 42;  a = std::string("now a string");  std::any_cast<std::string>(&a);
std::byte b{0x0F};  b <<= 4;  std::to_integer<int>(b);

fs::path dir = fs::temp_directory_path() / "m13_demo";
fs::create_directories(dir);
for (const auto& entry : fs::directory_iterator(dir)) entry.path().filename(), entry.file_size();
fs::remove_all(dir);
```

`code/m13_shared_mutex_cache.cpp` is the read-mostly store:

```cpp
class KvStore {
public:
    std::optional<std::string> get(const std::string& key) const {
        std::shared_lock lock(mu_);              // readers run concurrently
        auto it = map_.find(key);
        return it == map_.end() ? std::nullopt : std::optional<std::string>(it->second);   // copy out under the lock
    }
    void put(const std::string& key, std::string value) {
        std::unique_lock lock(mu_);              // exclusive
        map_[key] = std::move(value);
    }
private:
    mutable std::shared_mutex mu_;               // mutable: const get() may lock
    std::unordered_map<std::string, std::string> map_;
};
```

Four reader threads and one writer run against it; the program prints the hit count and the final
state. Note `mutable` on the mutex and that `get` returns a copy, never a reference into the map.

## 4. Under the hood (senior-level)

**Structured bindings are not variables.** `auto [a, b] = f();` creates a hidden object `e` of the
returned type and makes `a` and `b` *names for* `e.first`/`e.second` (or `get<0>(e)`, `get<1>(e)` for
tuple-like types, or the members in order for aggregates). There is no copy of `a` and `b`
individually; `decltype(a)` is the member's declared type, even though `a` behaves like a reference.
With `auto&` the hidden object is a reference and no copy happens at all.

**`std::string_view` ABI.** Two words, passed in registers (`rdi`, `rsi`) on x86-64, trivially
copyable; `const std::string&` is one pointer but requires a `std::string` object to exist (a literal
argument creates a temporary with a heap allocation if longer than 15 bytes). `string_view::substr` is
pointer arithmetic; `std::string::substr` allocates.

**`std::variant` vs `union` vs inheritance.** `variant` is a union with an index byte and
compiler-generated copy/move/destroy that switch on the index. Storage is `max(sizeof(Ts...))`
rounded to `max(alignof(Ts...))` plus the index. No heap, no vptr.

**`std::any` storage.** libstdc++ stores types that fit in a pointer-sized buffer and are
nothrow-movable inline; everything else is heap-allocated. Each `any` holds a pointer to a static
manager function per stored type (effectively a hand-rolled vtable), which is how `any_cast` checks
the type via `typeid` comparison.

**`std::shared_mutex` on Linux** is `pthread_rwlock_t` (56 bytes, vs 40 for `std::mutex`). Taking a
shared lock is an atomic increment of a reader count when no writer holds or waits; a writer waits
for readers to drain. glibc's rwlock prefers readers by default (`PTHREAD_RWLOCK_PREFER_READER_NP`),
which can starve writers under constant read load; the writer-preferring variant exists but
`std::shared_mutex` does not expose it. Uncontended shared lock/unlock is slower than a plain mutex
(more atomic operations), so a shared mutex only wins when critical sections are long enough or
readers numerous enough that parallel readers pay for the extra cost. Measure. For very short reads a
plain `std::mutex` or a lock-free snapshot (`shared_ptr<const Config>` swap) is often faster.

**CTAD mechanics.** The compiler synthesises a set of function templates from the class's
constructors (and from explicit deduction guides), performs overload resolution on them with the
constructor arguments, and uses the deduced class template arguments. That is why
`std::vector v{other};` picks the copy constructor's deduction (vector<int>) rather than the
initializer-list constructor.

**`[[nodiscard]]` codegen.** None; it is purely a compile-time diagnostic. The same is true for all
attributes here. `[[fallthrough]]` suppresses a warning; `[[maybe_unused]]` suppresses another.

**`std::filesystem` and errors.** The throwing overloads throw `std::filesystem::filesystem_error`
(derived from `std::system_error`) carrying both paths and the `error_code`. The `error_code&`
overloads never throw and are the right choice in servers (`fs::remove(path, ec)`). Path operations
are string manipulation and never touch the disk; `exists`, `file_size` etc. are `stat` calls.

**Parallel algorithms and TBB.** libstdc++ delegates to Intel's PSTL on top of TBB's task scheduler
(work-stealing thread pool, one worker per hardware thread). `par_unseq` additionally allows
vectorisation within a thread, which forbids anything that could deadlock across lanes (no locks in
the callable). Without `-ltbb` the link fails with undefined `tbb::` symbols.

## 5. In production

- **`[[nodiscard]]` everywhere a C programmer forgets to check.** On `error_code`-returning
  functions, `bool try_*`, `release()`, factory functions returning handles. With `-Werror` the
  compiler enforces the check that code review used to miss.
- **`string_view` parameters, `std::string` members.** Accept views at the API, store strings. A member
  `string_view` is a lifetime bug waiting for a reused buffer. In a parser, views into the request
  buffer are fine *within* the request's processing; anything that outlives it must copy.
- **Structured bindings in map loops** remove the `.first`/`.second` noise that hides bugs
  (`it->second.second`). Name the parts.
- **`shared_mutex` for config and caches, not for everything.** In tcpkv the store is read-mostly, so
  readers in parallel is a real win; a mostly-written structure under a `shared_mutex` is slower than
  under a `std::mutex`. Profile with `perf` and look at `pthread_rwlock_*` time.
- **`std::filesystem` in servers.** Use the `error_code` overloads; a missing directory is a normal
  condition, not an exception. `fs::rename` is atomic within a filesystem (the write-temp-then-rename
  pattern for crash-safe config updates).
- **Parallel algorithms are a tool for batch jobs,** not for request paths: the TBB pool competes with
  your own thread pool for cores and adds tail latency. Use them in offline tooling (report generation,
  migration jobs) where throughput is all that matters.
- **3 AM debugging.** A `std::string_view` printing garbage or crashing in `operator<<`: dangling view.
  gdb shows a `_M_str` pointer into freed heap. Search for views stored in members or captured by
  lambdas that outlive the string. ASan reports it as `heap-use-after-free` with the allocation and
  free stacks.

## 6. Try it

1. **(easy)** Build `m13_cpp17_tour.cpp`. Remove the `[[maybe_unused]]` and build with `-DNDEBUG`.
   Then remove the `if (!try_reserve(...))` check. Done when you have seen both warnings become errors
   and can explain each.
2. **(easy)** Rewrite a function of yours taking `const std::string&` to take `std::string_view`, and
   call it with a literal, a `std::string`, and a substring. Count allocations with a counting
   `operator new`. Done when the view version allocates zero times.
3. **(medium)** Write a `config_reload(const fs::path&)` that writes to `path + ".tmp"`, `fsync`s,
   and `fs::rename`s over the original, using `error_code` overloads throughout. Done when killing
   the process at any point leaves either the old or the new file intact, never a partial one.
4. **(medium)** Benchmark `m13_shared_mutex_cache.cpp` with `std::shared_mutex` vs `std::mutex`
   for reader:writer ratios 100:1, 10:1, 1:1 and critical sections of 100 ns and 10 us (add a spin).
   Done when you have a table and can say where `shared_mutex` wins on your machine.
5. **(hard)** Install `libtbb-dev` and sort a `std::vector<std::uint64_t>` of 1M, 10M and 100M
   elements with `std::sort` vs `std::sort(std::execution::par, ...)`. Done when you have the speed-ups
   and `perf stat` shows the number of threads used; explain the 1M result.
6. **(hard)** Implement `template <class... Ts> class TypeMap` using a `std::tuple<std::optional<Ts>...>`
   with `get<T>()`/`set<T>()` via fold expressions and `if constexpr`, and a `visit_all(f)` that calls
   `f` on each present value. Done when `static_assert`s verify it is constexpr-constructible and a test
   covers an empty map.

## 7. Interview questions

**Q: What are structured bindings and what can you bind?**
A declaration `auto [a, b] = expr;` that names the parts of a pair, tuple, `std::array`, C array, or
struct with all-public members. The names alias the members of a hidden object (copy, or reference
with `auto&`); you cannot skip or nest.

**Q: `std::string_view` versus `const std::string&`: when and why?**
Use `string_view` for read-only string parameters: it is two words passed in registers, accepts
literals and `char*` without constructing a `std::string`, and slices in O(1). Keep `const std::string&`
(or `std::string`) when you need a null-terminated C string or will store the value. Never store a
view that may outlive its source.

**Q: What does `[[nodiscard]]` do?**
Marks a function (or type) whose return value must not be silently discarded; ignoring it produces a
compiler warning, an error with `-Werror`. It has no runtime effect. Use it on error codes, success
flags, handles and pure computations.

**Q: What is CTAD?**
Class template argument deduction: the compiler deduces a class template's arguments from the
constructor arguments, so `std::pair p{1, 2.5}` and `std::lock_guard lock(mu)` need no angle
brackets. Deduction uses the constructors and any user-written deduction guides.

**Q: How does `std::shared_mutex` differ from `std::mutex`, and when does it help?**
It allows many concurrent shared (read) holders or one exclusive (write) holder. On Linux it is a
`pthread_rwlock_t`. It helps when reads greatly outnumber writes and critical sections are long enough
to amortise its higher per-lock cost; for very short sections a plain mutex is often faster. Readers
can starve writers with glibc's default policy.

**Q: Why does `std::vector v{other_vector};` not create a vector of vectors?**
CTAD prefers the copy-constructor deduction over the initializer-list constructor when the braced list
holds a single element of the same template. Write `std::vector<std::vector<int>> v{other};` or
`std::vector v{other, other2};` to get the nested type.

**Q: What do you need to use parallel algorithms on GCC?**
`#include <execution>`, an execution policy argument (`std::execution::par`), and linking against
Intel TBB (`-ltbb`). The callable must be thread-safe and must not throw.

## 8. Recap

- Structured bindings and `if`-initialisers tighten scope and remove `.first`/`.second` noise.
- `string_view` for read-only string parameters; never store a view beyond its source's lifetime.
- `optional`, `variant`, `any`: inline, inline, type-erased; prefer `variant` over `any`.
- `filesystem`: use the `error_code` overloads in servers; `rename` is atomic.
- Parallel algorithms need `-ltbb` on GCC and are for batch work.
- Fold expressions, `if constexpr`, inline variables, CTAD, `std::byte`.
- `[[nodiscard]]` turns forgotten checks into compile errors; `[[maybe_unused]]`, `[[fallthrough]]`
  document intent.
- `std::shared_mutex` = `pthread_rwlock_t`; wins only for read-mostly, long-enough critical
  sections; see 04_Concurrency for the full treatment.
- Guaranteed copy elision, `try_emplace`, `insert_or_assign`, node `extract`/`merge`, `std::clamp`,
  `std::apply`, nested namespaces.
