# 02_Modern_CPP — Interview questions and model answers

38 questions, ordered from basic to senior. Answer out loud, then compare. A good answer is short,
precise, and ends with a consequence ("so in production you...") when there is one. The module that
covers each question is in brackets.

Levels: Q1 to Q10 basic, Q11 to Q24 intermediate, Q25 to Q38 senior.

---

## Basic

**Q1. You are a C programmer. What are the most important differences when you move to C++?** [01]

C++ adds constructors and destructors, which make RAII possible: cleanup is tied to scope instead of
to every error path. It adds references, overloading (and therefore name mangling), namespaces,
templates, and a stricter type system (no implicit `void*` to `T*` conversion). The standard library
gives you containers, strings and smart pointers so you rarely manage memory by hand. The compile and
link model, the stack, the heap and the ABI concepts are the same, so C debugging skills carry over directly.

**Q2. What is the difference between a pointer and a reference?** [01]

A reference is an alias that must be bound to an object at initialisation and cannot be re-seated or
null. A pointer is an object holding an address; it can be null, reassigned, and used in arithmetic.
Under the hood a reference is usually implemented as a pointer. Use a reference for "must exist,
will not change what it refers to" parameters, a pointer (or `std::optional<std::reference_wrapper>`)
when "nothing" is a valid value.

**Q3. What is name mangling and why does `extern "C"` exist?** [01]

Because C++ allows overloading, the compiler encodes the parameter types (and namespace) into the
symbol name, for example `_Z7depositR7Accountx`. C has no overloading and uses the plain name.
`extern "C"` tells the C++ compiler to use C linkage (no mangling) so a C++ program can call a C
library, or expose functions to C or to `dlsym`. Wrap C headers in `#ifdef __cplusplus extern "C" {`.

**Q4. In what order are members and bases constructed and destroyed?** [02]

Virtual bases first, then direct bases in declaration order, then members in declaration order, then
the constructor body. The order in the member-initialiser list does not matter; GCC warns with
`-Wreorder` if it differs. Destruction is the exact reverse. If a constructor throws, only the
already-constructed bases and members are destroyed, and the object's own destructor does not run.

**Q5. What does `explicit` do on a constructor?** [02]

It forbids the constructor from being used for implicit conversions and copy-initialisation, so
`Temperature t = 36.6;` and passing `36.6` to a `Temperature` parameter both fail to compile, while
`Temperature t{36.6};` works. Single-argument constructors should be `explicit` by default; it stops
silent conversions such as an `int` turning into a buffer of that size.

**Q6. What is RAII?** [03]

Resource Acquisition Is Initialisation: a resource (memory, file, socket, lock, transaction) is
acquired in a constructor and released in the destructor of an object with automatic lifetime.
Because destructors run on every exit from a scope, including `return`, `break` and exceptions,
cleanup cannot be forgotten. It replaces the C pattern of `goto cleanup` and makes exception-safe code
possible. `std::lock_guard`, `std::unique_ptr` and `std::fstream` are all RAII types.

**Q7. What are the special member functions, and what are the rules of zero, three and five?** [02, 04]

Default constructor, destructor, copy constructor, copy assignment, move constructor, move assignment.
Rule of five: if you write any of destructor, copy or move operations, you probably need to think
about all five, because you are managing a resource by hand. Rule of three is the pre-C++11 version.
Rule of zero: prefer composing classes from members that already manage themselves (`std::string`,
`std::vector`, `std::unique_ptr`) so you write none of them and the compiler-generated ones are correct.

**Q8. What does `const` on a member function mean?** [01, 11]

It means the function does not modify the observable state of the object; inside it, `this` is a
pointer to const. You can call only `const` member functions on a `const` object or through a `const&`.
`mutable` members (for example a cache or a mutex) are exempt. In production, const-correctness is
also a thread-safety signal: the standard library assumes `const` operations are safe to call
concurrently.

**Q9. `std::vector` vs `std::list`: which do you default to and why?** [06]

`std::vector`, almost always. It is contiguous, so iteration is cache-friendly and the hardware
prefetcher helps, while every `std::list` node is a separate heap allocation reached by a pointer
chase. Even middle insertions are often faster on a vector for small-to-medium sizes because moving
contiguous memory is cheap. Use `list` only when you need stable iterators across insertions and
erasures or O(1) `splice`, such as an LRU cache.

**Q10. What is a lambda, and what does it compile into?** [07]

A lambda is an unnamed function object. The compiler generates a unique class whose data members are
the captures and whose `operator()` is the body. Capture by value copies into members; capture by
reference stores references, which dangle if the lambda outlives the scope. Because each lambda has
its own type, passing it to a template lets the compiler inline the call, unlike a function pointer.

## Intermediate

**Q11. What are lvalues and rvalues, and what is an rvalue reference?** [04]

An lvalue has an identity you can take the address of (a named variable); an rvalue is a temporary
or a value about to expire. `T&&` is a reference that binds only to rvalues, which lets an overload
know "nobody else will look at this object again, so I may steal its resources". Precisely, C++17 has
lvalues, xvalues and prvalues; xvalues (such as the result of `std::move(x)`) and prvalues together are rvalues.

**Q12. What does `std::move` actually do?** [04]

Nothing at run time. It is a `static_cast<T&&>` that turns an lvalue into an xvalue so that overload
resolution picks the move constructor or move assignment. The move itself is done by those functions.
If the type has no move constructor, the copy constructor is chosen instead, silently. And
`std::move` on a `const` object yields `const T&&`, which binds to the copy constructor, so it copies.

**Q13. A function takes `std::string&& s` and does `v.push_back(s);`. What is wrong?** [04]

`s` has a name, so the expression `s` is an lvalue even though its type is an rvalue reference.
`push_back(const std::string&)` is chosen and the string is copied. Write `v.push_back(std::move(s));`.
In templates with forwarding references (`T&&` where `T` is deduced), use `std::forward<T>(s)` instead
so lvalue arguments are still copied.

**Q14. What state is a moved-from object in?** [04]

For standard library types, "valid but unspecified": you may destroy it, assign to it, and call
functions without preconditions such as `clear()` or `size()`, but you must not assume its value.
For your own types you choose the guarantee; for resource handles the convention is to leave the
source empty (fd = -1, pointer = null) so its destructor does nothing. Using a moved-from object's
value is a logic bug, and `clang-tidy`'s `bugprone-use-after-move` catches many cases.

**Q15. What is copy elision, and what changed in C++17?** [04]

Copy elision lets the compiler construct a returned object directly in the caller's storage, so no
copy or move happens. Since C++17 it is mandatory for prvalues: `T x = T{};` and `return T{};`
construct in place, and the type need not even be copyable or movable. NRVO (returning a named local)
is still optional but done by GCC and Clang in practice. Writing `return std::move(local);` defeats
NRVO and is a pessimisation.

**Q16. How should a function take a `std::string` parameter?** [04, 13]

Read-only and not stored: `std::string_view` (or `const std::string&` if you need a null-terminated
`c_str()`). Stored into a member ("sink"): take `std::string` by value and `std::move` it into place;
callers passing rvalues pay one move, lvalues pay one copy plus one move. The `const&` plus `&&`
overload pair saves that one move but doubles the code; use it only on hot paths.

**Q17. When do you use `unique_ptr`, `shared_ptr`, `weak_ptr`, or a raw pointer?** [05]

`unique_ptr` for single ownership; it is the default, and costs the same as a raw pointer with a
stateless deleter. `shared_ptr` only when there are genuinely several owners whose lifetimes you
cannot order. `weak_ptr` for observers that must not keep the object alive and to break cycles. A raw
pointer or reference for non-owning access when the owner provably outlives the user. In code review,
every `shared_ptr` should be able to name its second owner.

**Q18. How do you wrap a C API handle such as `FILE*` or `sqlite3*` in a smart pointer?** [05]

Use `std::unique_ptr<T, Deleter>` with a custom deleter, ideally a stateless struct:
`struct FileCloser { void operator()(FILE* f) const noexcept { std::fclose(f); } };` and
`using FilePtr = std::unique_ptr<FILE, FileCloser>;`. A stateless deleter adds no size thanks to the
empty-base optimisation; a function-pointer deleter doubles the size. For an `int` file descriptor,
which is not a pointer, write a small move-only class instead.

**Q19. What does `vector::push_back` cost, and when does it invalidate iterators?** [06]

Amortised O(1). When `size() == capacity()`, the vector allocates a larger buffer (libstdc++ doubles;
MSVC grows by 1.5x), moves or copies every element across, and frees the old buffer. That invalidates
every iterator, pointer and reference into the vector. Without reallocation only `end()` is
invalidated. If you know the size, `reserve()` first; and never hold a pointer into a vector across a
`push_back` unless capacity is guaranteed.

**Q20. Why does `std::vector` copy your elements during reallocation instead of moving them?** [04, 06]

Because your move constructor is not `noexcept`. `vector` gives the strong exception guarantee on
`push_back`: if reallocation fails half-way, the original contents must be intact. Moving
destructively and then throwing would lose data, so it uses `std::move_if_noexcept`, which copies
when the move might throw and a copy exists. Mark move operations `noexcept` (they almost never need
to throw) and check with `static_assert(std::is_nothrow_move_constructible_v<T>)`.

**Q21. Iterator invalidation rules for `map` and `unordered_map`?** [06]

`std::map` is a node-based red-black tree: insertion invalidates nothing, erase invalidates only
iterators to the erased element. `std::unordered_map` is a bucket array of node chains: erase
invalidates only the erased element, insertion invalidates iterators (not pointers or references to
elements) if it triggers a rehash. Use `reserve()` to prevent rehashes, and use the iterator returned
by `erase` when erasing inside a loop.

**Q22. `std::map` vs `std::unordered_map`: how do you choose?** [06]

`map` gives ordered iteration, range queries (`lower_bound`) and guaranteed O(log n), but each lookup
chases about log2(n) pointers through separate nodes. `unordered_map` gives average O(1) with a
single hash and usually one or two cache misses, but no ordering, worst-case O(n) with a bad hash,
and rehash spikes. For read-mostly data that is built once, a sorted `vector` with binary search
often beats both because it is contiguous.

**Q23. Why must templates usually be defined in headers?** [08]

A template is not code; it is a recipe. The compiler generates code only when it sees the template
used with concrete types, and it needs the full definition at that point in that translation unit.
If the definition lives in a `.cpp`, other translation units cannot instantiate it and you get
undefined-symbol link errors. The exception is explicit instantiation (`template class Foo<int>;`)
paired with `extern template` declarations, which also cuts build time.

**Q24. What is SFINAE, and what does `if constexpr` replace?** [08, 13]

Substitution Failure Is Not An Error: if substituting template arguments into a declaration
produces invalid code, that overload is silently removed from the candidate set rather than causing
an error. It is used with `std::enable_if` and `std::void_t` to select overloads by type properties.
C++17's `if constexpr` discards the untaken branch at compile time inside one function, which replaces
most SFINAE overload sets with readable code. C++20 concepts replace most of the rest.

## Senior

**Q25. Describe the memory layout of an object with virtual functions, and how a virtual call compiles.** [09]

Each polymorphic object carries a hidden vptr, typically at offset 0 under the Itanium ABI used by
GCC and Clang, pointing into its class's vtable in `.rodata`. The vtable holds offset-to-top, a
typeinfo pointer, then one function pointer per virtual function in declaration order, with derived
overrides replacing base entries. A call `p->area()` compiles to: load the vptr from `*p`, load the
slot at a fixed offset, indirect call. The constructor sets the vptr, which is why a virtual call
inside a base constructor dispatches to the base version.

**Q26. Why does a polymorphic base class need a virtual destructor?** [09]

`delete base_ptr;` on an object of derived type, through a base with a non-virtual destructor, is
undefined behaviour. In practice only the base destructor runs, so the derived class's members
(strings, buffers, sockets) leak, and the deleting destructor may free with the wrong size. With a
virtual destructor, the vtable's deleting-destructor slot dispatches to the most-derived destructor.
Rule: public and virtual, or protected and non-virtual. `shared_ptr` created with `make_shared<Derived>`
captures the right deleter, but do not rely on that.

**Q27. What is object slicing?** [09]

Copying a derived object into a base object by value copies only the base subobject; the derived
members are dropped and the vptr is the base's, so virtual calls go to base versions. It happens with
`Base b = derived;`, by-value parameters, and `std::vector<Base>`. Prevent it by passing polymorphic
types by reference or pointer, storing `std::vector<std::unique_ptr<Base>>`, and deleting or
protecting the base's copy operations.

**Q28. What does `final` buy you beyond documentation?** [09]

It lets the compiler devirtualise. When the static type is a `final` class, or a `final` virtual
function, the dynamic type is known exactly, so the indirect call becomes a direct call that can be
inlined. That removes the vtable load and the indirect branch, and, more importantly, enables
inlining and further optimisation of the caller. GCC's `-Wsuggest-final-types` points to candidates.

**Q29. What is in a `shared_ptr` control block, and what does `make_shared` change?** [05]

A `shared_ptr` is two pointers: one to the object, one to a control block holding the strong count,
the weak count, the deleter and the allocator (type-erased). Counts are atomic, so copying a
`shared_ptr` across threads costs a locked instruction on a shared cache line. `shared_ptr<T>(new T)`
makes two allocations; `make_shared<T>()` makes one block containing both, which is faster and more
cache-friendly. The catch: with `make_shared` the object's memory cannot be freed until the last
`weak_ptr` is gone, because it shares the allocation with the control block (the object is still destroyed when the strong count hits zero).

**Q30. Is `shared_ptr` thread-safe?** [05]

The control block is: different threads may copy and destroy different `shared_ptr` instances that
share one object, and the count stays correct. The pointee is not protected at all; concurrent access
to the object needs its own synchronisation. And one `shared_ptr` instance is not safe to read and
write from two threads at once; for that you need `std::atomic_load`/`atomic_store` on it in C++17 or
`std::atomic<std::shared_ptr<T>>` in C++20, or a mutex.

**Q31. How do you create a reference cycle with `shared_ptr`, and how do you fix it?** [05]

A parent holding `shared_ptr` to children while each child holds `shared_ptr` to its parent: when the
last external owner goes, every strong count is still at least 1, so nothing is freed. Valgrind
reports the blocks as definitely and indirectly lost. Fix it by making the back edge a `weak_ptr`
(lock it to use it) or a raw pointer when the parent provably outlives the children. The design rule
is that ownership must form a tree or a DAG, never a cycle.

**Q32. What are the exception-safety guarantees?** [10]

No-throw: the operation never fails (destructors, swaps, moves, `noexcept` functions). Strong: if it
throws, state is exactly as before, like a transaction. Basic: if it throws, no resources leak and
all invariants hold, but the state may have changed. No guarantee: anything can happen. The usual
route to the strong guarantee is "do all throwing work on a copy, then commit with a non-throwing
swap", which is copy-and-swap. Destructors must not throw; since C++11 they are `noexcept` by default
and a throw calls `std::terminate`.

**Q33. What happens at run time when an exception is thrown, and what does it cost?** [03, 10]

GCC and Clang use the zero-cost table-based model: the normal path has no checks, and per-function
unwind tables plus `.gcc_except_table` describe what to clean up. A `throw` allocates the exception
object (`__cxa_allocate_exception`), then the unwinder walks the stack twice, first to find a handler,
then to run landing pads that call destructors, and finally enters the `catch`. That costs on the
order of microseconds per throw (typical), so exceptions suit rare failures, not control flow or
per-request expected errors like "key not found".

**Q34. How do you choose between exceptions, `std::optional`, `std::error_code` and an expected type?** [10]

Exceptions for rare, cannot-continue failures far from where they can be handled (out of memory,
corrupt configuration at start-up); they keep the happy path clean. `std::optional` when absence is
normal and needs no explanation (`find`). `std::error_code`, or an expected-style `variant<T, Error>`,
when failure is a normal outcome the caller must handle and you need the reason, especially on hot
paths, across a C ABI, or in code built with `-fno-exceptions`. In a server, never let an exception
escape a thread function: catch at the request boundary, log, and return an error response.

**Q35. Name the main categories of undefined behaviour, and why the optimiser makes them dangerous.** [12]

Memory: out-of-bounds access, use-after-free, double free, null dereference, uninitialised reads.
Arithmetic: signed overflow, shift by negative or by at least the bit width, division by zero. Type:
strict-aliasing violations, calling through the wrong function type, misaligned access. Lifetime and
concurrency: dangling references, data races, modifying a `const` object. The compiler assumes UB
never happens and optimises on that assumption, so `if (x + 1 < x)` is deleted and a null check after
a dereference is removed. The bug therefore appears only at `-O2` and far from its cause; catch it
with `-fsanitize=address,undefined` and TSan.

**Q36. What is padding, and how does layout affect performance?** [12]

Each type has an alignment; the compiler inserts padding so every member is aligned and the struct
size is a multiple of its largest alignment. `{char; double; char;}` is 24 bytes, reordered
`{double; char; char;}` is 16. Smaller structs mean more objects per 64-byte cache line and fewer
misses. False sharing is the opposite trap: two threads writing different variables on the same
line make the line bounce between cores; separate hot per-thread data with `alignas(64)`.

**Q37. Which C++17 features do you actually use in production, and what is the trap in `string_view`?** [13]

Structured bindings (`for (auto& [k, v] : map)`), `if (auto it = m.find(k); it != m.end())`,
`std::optional`, `std::variant` with `std::visit`, `std::string_view`, `std::filesystem`,
`std::shared_mutex` for read-mostly data, `[[nodiscard]]`, `if constexpr`, fold expressions, inline
variables and CTAD. The `string_view` trap is lifetime: it does not own its characters, so returning
a view of a local string, or storing a view of a temporary, leaves it dangling. It is also not
null-terminated, so never pass `sv.data()` to a C API that expects a C string.

**Q38. Design the ownership for a TCP server: listener, connections, worker pool, shared store.** [05, 14]

The `Server` owns the listening socket as a move-only `SocketFd` by value, the thread pool by value,
and the store by value, so destruction order is declared order reversed: declare the store before the
pool so workers are joined before the store dies. Each accepted connection is a move-only
`Connection` holding a `SocketFd`, moved into the task that serves it, so exactly one owner closes
each fd. Workers get the store by reference; the store guards its map with a `std::shared_mutex`
(`shared_lock` for reads, `unique_lock` for writes). `shared_ptr` appears only if a connection
genuinely has two owners, such as async I/O plus a registry, with the registry holding `weak_ptr`.
This is exactly the tcpkv design in 10_Capstone_Projects/tcpkv.
