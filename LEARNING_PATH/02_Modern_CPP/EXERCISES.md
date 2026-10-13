# 02_Modern_CPP — All exercises

Every exercise from the "Try it" section of all 14 modules, in one place. The module file has the
full wording and the exact "done when" condition; this list gives the short version plus a hint.

Build everything in WSL with `g++ -std=c++17 -Wall -Wextra -Werror -pthread` unless the exercise
says otherwise. Do the easy ones the same day you read the module. Do at least one medium per
module. Pick the hard ones that match the job you want: the ownership, invalidation, vtable and
performance ones come up most in interviews.

Difficulty count: 28 easy, 29 medium, 24 hard (81 total).

---

## 01. From C to C++ (`01_from_c_to_cpp.md`)

1. **(easy)** `swap_values(int&, int&)` with references, plus the C pointer version.
   - Hint: a reference must bind to an object at initialisation, so there is no `nullptr` to pass.
     Try `swap_values(*p, *q)` with a null `p` and note that the UB moves to the caller's dereference.
2. **(easy)** Build `m01_extern_c_mangling.cpp`, run `nm` with and without `-C`, and decode the
   mangled name of `std::string greet(const std::string&, int)`.
   - Hint: `R` = reference, `K` = const, `P` = pointer, `i` = int, `c` = char. `std::string` shows up
     as a long `NSt7__cxx1112basic_string...` chunk; `c++filt` confirms your reading.
3. **(medium)** Rename a 100-line C file to `.cpp` and fix every error under `-Werror`.
   - Hint: `malloc` needs `static_cast<T*>`, identifiers named `new`/`class`/`this` must be renamed,
     and string literals are `const char*`, not `char*`.
4. **(medium)** C header + `gcc -c` object, called from C++ with `extern "C"` guards.
   - Hint: wrap the declarations in `#ifdef __cplusplus extern "C" { #endif` in the header itself,
     not in `main.cpp`, so every C++ includer gets it for free.
5. **(hard)** Predict which `log` overload is chosen for `NULL`, `0`, `'x'`, `3.0f`, `nullptr`.
   - Hint: GCC defines `NULL` as `__null`, an integer zero with the width of a pointer, so on 64-bit
     Linux `log(NULL)` picks `long`, not `const char*` (other compilers differ). `3.0f` is a
     float-to-double promotion; `'x'` promotes to `int`; only `nullptr` reaches `const char*`.

## 02. Classes, constructors and destructors (`02_classes_constructors_destructors.md`)

1. **(easy)** Predict the output of `m02_ctor_dtor_order.cpp` on paper, then run it.
   - Hint: bases first, then members in declaration order, then the constructor body. Destruction is
     the exact reverse.
2. **(easy)** Add `Tracer c` to `Derived` and list it first in the initialiser list; see `-Wreorder`.
   - Hint: members are initialised in declaration order, never in initialiser-list order.
3. **(medium)** `Temperature` with an `explicit` constructor and a `from_fahrenheit` factory.
   - Hint: `explicit` blocks copy-initialisation (`T t = 36.6;`) but not direct-initialisation
     (`T t{36.6};`). A named static factory documents the unit at the call site.
4. **(medium)** A class whose second member's constructor throws; observe which destructors run.
   - Hint: only fully constructed subobjects are destroyed. The outer object's lifetime never began,
     so its destructor does not run. This is why each resource needs its own RAII member.
5. **(hard)** Function-local static singleton; find `__cxa_guard_acquire` in `-O2 -S` output.
   - Hint: look for a byte-sized guard variable tested with a plain load before the call. Once it is
     set, later calls take the fast path and never reach the lock.

## 03. RAII (`03_raii_the_core_idea.md`)

1. **(easy)** Add an early-`return` path to `m03_file_handle_raii.cpp`; show `fclose` still runs.
   - Hint: put a `printf` in the destructor. Every path that leaves the scope runs it.
2. **(easy)** `DirHandle` wrapping `opendir`/`closedir`, move-only, listing `/tmp`.
   - Hint: copy `FileHandle`'s shape exactly: delete copy, write move ctor/assign that null the
     source, and check for null in the destructor.
3. **(medium)** Rewrite the C `process()` from section 2.1 with RAII; count lines before and after.
   - Hint: each `goto cleanup` label becomes one RAII local. Declare them in acquisition order so the
     reverse-order destruction matches what the C code did by hand.
4. **(medium)** Generic `ScopeGuard` with `dismiss()`, used to roll back `map.erase(key)`.
   - Hint: `template <class F> class ScopeGuard { F f_; bool active_ = true; ... }`. Make the
     destructor `noexcept` and call `f_()` only while `active_`. Call `dismiss()` after the last step succeeds.
5. **(hard)** `strace -e trace=socket,bind,listen,close` on `m03_socket_fd_raii.cpp`, then add a
   double-close bug and see `EBADF`.
   - Hint: the second `close` on the same number fails because the kernel already freed that fd slot.
     In a threaded server the slot may have been reused, so the "harmless" bug closes someone else's socket.
6. **(hard)** Find the landing pad for `write_report` in `-O2 -S` output.
   - Hint: search for `_Unwind_Resume`. The block just before it calls `fclose`; the normal path
     reaches `fclose` separately and has no flag checks. The `.gcc_except_table` maps call sites to pads.

## 04. Value semantics, copy and move (`04_value_semantics_copy_move.md`)

1. **(easy)** Build `m04_copy_elision_nrvo.cpp` with and without `-fno-elide-constructors`.
   - Hint: in C++17, prvalue elision is mandatory and the flag cannot disable it; only NRVO and
     the move from a named local appear when you pass the flag.
2. **(easy)** Drop `noexcept` from `UniqueFd`'s move constructor and `static_assert` it.
   - Hint: `vector` uses `std::move_if_noexcept` during reallocation. A move-only type is still moved
     (there is no copy to fall back on), but the strong guarantee is lost.
3. **(medium)** `Matrix` with rule of five and copy-and-swap; zero copies in `Matrix m = a * b;`.
   - Hint: `operator*` returns a prvalue, so `m` is constructed in place. Write
     `Matrix& operator=(Matrix other) noexcept { swap(other); return *this; }` to get both assignments at once.
4. **(medium)** Sink `set_name(std::string)` vs the `const&`/`&&` overload pair; 6-cell table.
   - Hint: the sink version costs one extra move in every case. The pair wins only when moves are
     expensive or the function is very hot; a `std::string` move is a few pointer stores.
5. **(hard)** Fixed-capacity container with `template <class... Args> T& emplace(Args&&...)`.
   - Hint: `::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...)`. Without `std::forward`
     every argument is an lvalue and you get a COPY where you expected a MOVE.
6. **(hard)** Fix `void f(std::string&& s) { names.push_back(s); }`.
   - Hint: `s` has a name, so it is an lvalue and `push_back(const T&)` is chosen. Use
     `std::move(s)`. The compiler is right not to warn: the code is valid, just slower.

## 05. Smart pointers and ownership (`05_smart_pointers_and_ownership.md`)

1. **(easy)** `unique_ptr` alias for `DIR*` with a stateless deleter; `static_assert` on `sizeof`.
   - Hint: `struct DirCloser { void operator()(DIR* d) const noexcept { closedir(d); } };`. An
     empty deleter takes no space thanks to the empty-base optimisation. A function pointer deleter doubles the size.
2. **(easy)** Make `Node::parent` a `shared_ptr` and run under `valgrind --leak-check=full`.
   - Hint: the report shows "definitely lost" for the root block and "indirectly lost" for the
     children. The cycle keeps every strong count at 1 or more after `main`'s pointer dies.
3. **(medium)** `Connection : enable_shared_from_this` with an async callback that keeps it alive.
   - Hint: capture `self = shared_from_this()` in the lambda. The object must already be owned by a
     `shared_ptr` (create it with `make_shared`), or `shared_from_this` throws `bad_weak_ptr`.
4. **(medium)** Toy `SharedPtr<T>` with a plain then an atomic count; benchmark 1 vs 4 threads.
   - Hint: the atomic increment is a `lock xadd`. From four threads the cache line holding the count
     bounces between cores, so it is much slower per copy than the single-thread case.
5. **(hard)** Ownership graph for a tcpkv-style server, with a type on every edge.
   - Hint: `Server` owns the listener by value, the pool by value, and the store by value. Workers
     receive the store by reference. A connection is a `unique_ptr` moved into the task. `shared_ptr`
     only appears if a connection truly has two owners (for example, async I/O plus a registry).

## 06. STL containers (`06_stl_containers_deep_dive.md`)

1. **(easy)** Element type with a non-`noexcept` move in `m06_vector_growth.cpp`; see copies.
   - Hint: the rule is "move if nothrow-movable or not copyable, else copy", so that a throw half-way
     through reallocation leaves the old buffer intact.
2. **(easy)** 1M ints in `std::set` vs a sorted `vector`; time inserts and lookups.
   - Hint: each set node is a separate allocation of roughly 40 bytes (typical on 64-bit libstdc++),
     so every step of a lookup is a likely cache miss. Binary search on a vector touches far fewer lines.
3. **(medium)** `AccountKey { branch, number }` as a key for `unordered_map` and `map`.
   - Hint: specialise `std::hash<AccountKey>` in namespace `std` and combine with
     `h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2))`. For `map`, `std::tie(a.branch, a.number) < std::tie(...)`.
4. **(medium)** LRU cache with `std::list` plus `unordered_map<K, list::iterator>`.
   - Hint: `list::splice` relinks the node without copying or destroying it, so every iterator to it
     stays valid. Evict from `back()` and erase the map entry using the key stored in the node.
5. **(hard)** Three deliberate invalidation bugs under `-D_GLIBCXX_DEBUG` and then ASan.
   - Hint: the debug STL reports "attempt to dereference a singular iterator" at the bad line. ASan
     only reports if freed memory is actually touched, so the vector case is the one it catches best.
6. **(hard)** `std::map` vs sorted `vector<pair>` + `lower_bound` at 1k, 100k, 10M.
   - Hint: at 1k both fit in L1/L2 and differ little. As size grows the map's pointer chasing costs a
     cache miss per level while the vector's top levels stay hot. Report what you measure, not what you expect.

## 07. STL algorithms, lambdas, iterators (`07_stl_algorithms_lambdas_iterators.md`)

1. **(easy)** Replace sum, find-first and remove-matching loops with algorithms.
   - Hint: `std::accumulate` needs `<numeric>` and an initial value of the right type (`0LL`, not `0`,
     for paise). Remove-matching is `v.erase(std::remove_if(...), v.end())`.
2. **(easy)** `<=` comparator on 1000 equal elements, under `-D_GLIBCXX_DEBUG` or ASan.
   - Hint: strict weak ordering requires `comp(a, a) == false`. With `<=` the unguarded inner loop
     of introsort can run past the end of the array.
3. **(medium)** `top_k` with `sort`, `partial_sort`, `nth_element` + sort of the prefix.
   - Hint: full sort is O(n log n); `partial_sort` is O(n log k) with a heap; `nth_element` is O(n)
     average and then sorting k items is trivial. Expect `nth_element` to win for k = 10.
4. **(medium)** `vector<std::function>` handler registry vs a `std::tuple` of lambdas.
   - Hint: `std::function` is an indirect call that the optimiser usually cannot inline. The tuple
     version, walked with `std::apply` and a fold, gives the compiler every concrete type to inline.
5. **(hard)** Zero-copy word-frequency counter with `string_view` keys and `partial_sort`.
   - Hint: keep the whole file in one `std::string` that outlives the map, or every key dangles.
     Call `reserve` on the map to avoid rehash churn.
6. **(hard)** Bidirectional-only algorithm that rejects forward iterators via `iterator_traits`.
   - Hint: `static_assert(std::is_base_of_v<std::bidirectional_iterator_tag,
     typename std::iterator_traits<It>::iterator_category>, "needs bidirectional")`. Random-access
     tags derive from bidirectional, so `vector` passes.

## 08. Templates (`08_templates_and_generic_programming.md`)

1. **(easy)** `print_all` with a type that has no `operator<<`; find "required from here".
   - Hint: read the error bottom-up. The first "required from here" line that names your own file is
     the call to fix.
2. **(easy)** `static_assert(std::is_arithmetic_v<T>, ...)` at the top of `max_of`.
   - Hint: the assertion fires before the body is instantiated, so the long cascade disappears.
3. **(medium)** `StaticVector<T, N>` with placement new into aligned storage.
   - Hint: `alignas(T) std::byte buf_[N * sizeof(T)];`, construct with placement new, destroy with
     `std::launder(reinterpret_cast<T*>(...))->~T()` only for indices below `size_`.
4. **(medium)** `has_size_v` with `std::void_t`, then the same check with `if constexpr`.
   - Hint: primary template `: std::false_type`; specialisation on
     `std::void_t<decltype(std::declval<T&>().size())>` `: std::true_type`.
5. **(hard)** Variadic `compose(fs...)` applying right-to-left; `compose()` is identity.
   - Hint: recurse: `compose(f, rest...)` returns `[=](auto x) { return f(compose(rest...)(x)); }`.
     The base case is a lambda that returns its argument unchanged.
6. **(hard)** `-ftime-report` on a heavy file, then `extern template` across five `.cpp` files.
   - Hint: put `extern template class Foo<int>;` in the header and one
     `template class Foo<int>;` in exactly one `.cpp`. Measure total wall time with a clean build each time.

## 09. OOP, inheritance, virtual (`09_oop_inheritance_virtual.md`)

1. **(easy)** `-fdump-lang-class` on `m09_vtable_demo.cpp`; read `Vtable for Circle`.
   - Hint: slot 0 is offset-to-top, slot 1 is the typeinfo pointer, then the function entries. The two
     destructor entries are the "complete object" destructor and the "deleting" destructor.
2. **(easy)** Remove `virtual` from `~Shape`; find `-Wdelete-non-virtual-dtor`.
   - Hint: `delete base_ptr` then calls only `~Shape`, so the derived part is never destroyed. It is UB
     formally, and in practice it leaks whatever the derived class owns.
3. **(medium)** C `struct ops` + `void* ctx` rewritten as an abstract `Device` interface.
   - Hint: the function pointer table becomes the vtable and `ctx` becomes the derived class's
     members. Tests inject a `FakeDevice` through a `Device&` parameter.
4. **(medium)** 100M `shape->area()` calls: uniform, alternating, and `std::variant` + `std::visit`.
   - Hint: uniform types let the indirect-branch predictor hit every time. Alternating may still
     predict well on modern CPUs; measure. `variant` keeps objects contiguous and lets the compiler inline.
5. **(hard)** Plugin `.so` with `extern "C" Plugin* create()`, loaded by `dlopen`/`dlsym`.
   - Hint: `dlsym` looks up a plain string, so the factory must not be mangled. Virtual calls need no
     symbol lookup at all: they go through the vptr stored in the object. Link the host with `-ldl` on older glibc.
6. **(hard)** `-O2 -S` on calls through a `final` class vs a non-final one.
   - Hint: with `final`, the compiler knows the dynamic type exactly and emits a direct `call` (or
     inlines it). Without it you see a load of the vptr then `call QWORD PTR [rax+N]`.

## 10. Error handling (`10_error_handling_exceptions_optional_expected.md`)

1. **(easy)** Swap the two lines in `add_strong` and throw on the 3rd allocation.
   - Hint: if the total is updated before the push that can throw, a failed push leaves the total
     wrong. Do the throwing step first, then the non-throwing commit.
2. **(easy)** `std::optional<Account> find_account(std::string_view)` with no sentinel values.
   - Hint: `if (auto a = find_account("X")) { use(a->balance); }`. `value_or` needs a full default
     `Account`, so it suits small types best.
3. **(medium)** `KvErr` enum, `KvCategory`, `make_error_code`, and `is_error_code_enum`.
   - Hint: specialise `std::is_error_code_enum<KvErr> : std::true_type` in namespace `std`, and put
     `make_error_code` in the same namespace as `KvErr` so argument-dependent lookup finds it.
4. **(medium)** `Expected<T, E>` on `std::variant<T, E>` with a short-circuiting `map`.
   - Hint: `map(f)` returns `Expected<decltype(f(value())), E>`; if `this` holds an error, return that
     error without calling `f`. Make sure `T` and `E` are distinct types, or the variant is ambiguous.
5. **(hard)** `optional<int>` vs throwing `parse_port` at 0%, 1%, 50% invalid input.
   - Hint: the throwing version is free on the happy path and costs on the order of a microsecond
     per throw (typical), because unwinding walks tables and allocates the exception object.
6. **(hard)** Thread-pool worker that captures task exceptions into `std::exception_ptr`.
   - Hint: `catch (...) { result.ex = std::current_exception(); }` in the worker, and
     `if (ex) std::rethrow_exception(ex);` in `get()`. The original dynamic type and its members survive.

## 11. const and constexpr (`11_const_constexpr_and_compile_time.md`)

1. **(easy)** Find `kCrcTable` in `.rodata` and `0xcbf43926` in `main`.
   - Hint: search the `.s` file for `.section .rodata`. GCC prints 32-bit immediates as signed
     decimals, so `0xcbf43926` appears as `$-873187034`, not in hex.
2. **(easy)** `constexpr days_in_month` with `static_assert` per month, then an out-of-bounds read.
   - Hint: UB is forbidden in constant evaluation, so the compiler must reject it; the message says
     something like "array subscript value is outside the bounds".
3. **(medium)** `constexpr std::array<std::string_view, 6>` enum-to-string table.
   - Hint: add a `Count` or `Last` enumerator and `static_assert(table.size() == size_t(E::Count))`.
     Adding a value without a name then breaks the build.
4. **(medium)** `constexpr` sieve producing 1000 primes; time the compile, then push to 100,000.
   - Hint: GCC caps constexpr loops (`-fconstexpr-loop-limit`, default 262144) and total operations
     (`-fconstexpr-ops-limit`). Compile-time work moves cost from every run to every build.
5. **(hard)** `constexpr` state-machine table and `constexpr bool accepts(std::string_view)`.
   - Hint: `constexpr State table[5][4] = {...};` indexed by `[state][input_class(c)]`. The same
     function used at runtime reads the same `.rodata` table.

## 12. Memory layout and performance (`12_memory_model_layout_and_performance.md`)

1. **(easy)** Reorder `Bad` by hand in `m12_padding_alignment.cpp`; predict offsets; add `static_assert`.
   - Hint: sort members from largest alignment to smallest. Size is rounded up to the largest alignment.
2. **(easy)** Change padding to 32 bytes in `m12_false_sharing_bench.cpp`.
   - Hint: two counters 32 bytes apart can still share a 64-byte line, so the slowdown returns.
     Note that adjacent-line prefetch on some Intel CPUs can make 64 behave worse than 128.
3. **(medium)** `Arena::destroy_all()` with a list of finalisers for non-trivial objects.
   - Hint: store `{ptr, [](void* p) { static_cast<T*>(p)->~T(); }}` when
     `!std::is_trivially_destructible_v<T>`. Run finalisers in reverse order of creation.
4. **(medium)** `m12_bench_basics.cpp` at `-O0`, `-O2`, `-O3 -march=native`, and under `perf stat`.
   - Hint: `perf stat -e cache-misses,instructions,cycles ./bench`. The list's cache-miss count
     explains its time much better than its instruction count does.
5. **(medium)** `SmallVector<T, N>` with inline storage and heap spill.
   - Hint: a move of the inline case must move each element (the buffer lives inside the object). A
     move of the heap case steals the pointer and resets the source to inline mode.
6. **(hard)** Signed overflow check, null check after dereference, float read through `int*`.
   - Hint: the optimiser assumes UB never happens: `x + 1 < x` becomes `false`, the null check is
     deleted because the earlier dereference "proved" non-null, and strict aliasing lets it reorder
     the load. `-fsanitize=undefined` reports the first; `-fsanitize=null` the second.
7. **(hard)** `std::pmr::unsynchronized_pool_resource` for an `unordered_map<int, std::string>`.
   - Hint: use `std::pmr::unordered_map<int, std::pmr::string>` so the strings use the pool too. Check
     peak RSS with `/usr/bin/time -v`: pools often trade lower time for higher peak memory.

## 13. C++17 features tour (`13_cpp17_features_tour.md`)

1. **(easy)** Remove `[[maybe_unused]]` and build with `-DNDEBUG`; drop the `try_reserve` check.
   - Hint: under `-DNDEBUG` the `assert` vanishes, the variable becomes unused, and `-Werror` fails.
     Ignoring a `[[nodiscard]]` result triggers `-Wunused-result`.
2. **(easy)** `const std::string&` to `std::string_view`; count allocations with a counting `operator new`.
   - Hint: replace global `operator new(std::size_t)` to increment a counter. Literals longer than the
     SSO buffer (15 chars in libstdc++) allocate when passed as `const std::string&`.
3. **(medium)** Crash-safe `config_reload` via `.tmp` + `fsync` + `fs::rename`.
   - Hint: `rename` is atomic on the same filesystem. `fs::` has no `fsync`, so open with `open()`,
     write, `fsync(fd)`, close, then rename. For full durability also `fsync` the directory.
4. **(medium)** `shared_mutex` vs `mutex` across reader:writer ratios and critical-section lengths.
   - Hint: with a 100 ns critical section the shared lock's extra atomic work often loses. With a
     10 us critical section and many readers, parallel reading wins. See 04_Concurrency module 04.
5. **(hard)** `std::sort` vs `std::sort(std::execution::par, ...)` with TBB at 1M, 10M, 100M.
   - Hint: link with `-ltbb`. At 1M the thread start-up and merge overhead eats most of the gain.
6. **(hard)** `TypeMap<Ts...>` on `std::tuple<std::optional<Ts>...>` with fold-based `visit_all`.
   - Hint: `std::get<std::optional<T>>(tuple_)` selects by type, so all `Ts` must be distinct.
     `visit_all`: `std::apply([&](auto&... o) { ((o ? f(*o) : void()), ...); }, tuple_);`.

## 14. Writing production C++ (`14_writing_production_cpp.md`)

1. **(easy)** Split `m14_library_api_demo.cpp` into `kv_store.cpp` + `main.cpp`; build with CMake; run clang-tidy.
   - Hint: set `CMAKE_EXPORT_COMPILE_COMMANDS=ON` so `clang-tidy -p build` can find the flags.
2. **(easy)** Two different `struct Config` definitions in two `.cpp` files.
   - Hint: this is an ODR violation, "ill-formed, no diagnostic required". The linker never sees types,
     so it links silently. `-flto -Wodr` compares type layouts at link time and catches it.
3. **(medium)** `SocketFd` header in the tcpkv layout, test via public header, `-fvisibility=hidden`.
   - Hint: `#define TCPKV_API __attribute__((visibility("default")))` on exported classes and
     functions only. Then `nm -D --defined-only libtcpkv.so | c++filt` should be short.
4. **(medium)** Run clang-tidy (`bugprone-*,performance-*,modernize-*,readability-*`) on a 200-line file.
   - Hint: expect `modernize-use-auto`, `performance-unnecessary-value-param`, and
     `readability-magic-numbers` to dominate. Fix the cause, never add `NOLINT`.
5. **(hard)** Public header for tcpkv's move-only `Connection` with documented contracts.
   - Hint: delete copy, `[[nodiscard]]` on every result, return `std::optional<Message>` plus an
     `std::error_code&` out-parameter or a small result struct. State in a comment which methods are
     safe to call from two threads (likely none).
6. **(hard)** CI pipeline: clang-format, `-Werror`, ASan+UBSan, TSan, clang-tidy on the diff.
   - Hint: TSan and ASan cannot be combined in one binary; build twice. `bugprone-use-after-move`
     catches the use-after-move statically; TSan catches the data race at run time.
