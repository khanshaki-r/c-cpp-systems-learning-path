# 02_Modern_CPP — Modern C++17 for a C programmer, to senior depth

## What this course covers

You have written production C for 2.5 years: a Berkeley DB to YugabyteDB migration engine, a
multithreaded RPC-over-TCP application server, crash-dump analysis. You know what the stack is, what
`malloc` does, and what happens when someone forgets a `free` on the third error path. What you do
not have yet is fluency in the language that most C++ systems teams write every day: C++17 with
RAII, value semantics, move semantics, smart pointers, the STL, templates and virtual dispatch.

This course takes you from "C with classes" to "I can design an ownership model for a server, write
it in idiomatic C++17, and explain to an interviewer what the compiler generates for every line."

Every module starts from something you already do in C and shows the C++ way of saying the same
thing more safely. Then it goes under the hood: object layout, vtables, the `shared_ptr` control
block, vector reallocation, exception landing pads, name mangling, what `-O2` does to undefined
behaviour. That second half is what separates a mid-level answer from a senior one.

The thread running through the course is ownership. Who owns this resource? When is it released?
Can it be copied, moved, shared, or only borrowed? If you can answer those four questions for every
object in a design, you can write C++ that does not leak, does not double-free, and does not crash
at 3 AM.

## Why it is on the resume

This course backs the language half of your resume. These exact phrases depend on it:

- "C++17"
- "STL, RAII, smart pointers, templates, OOP, memory & ownership design, performance optimisation"
- tcpkv: "RAII socket and file-descriptor wrappers and move-only connection handles"
- tcpkv: "shared_mutex" (the C++17 type itself is introduced in module 13; the full
  readers-writers treatment is in 04_Concurrency/04_shared_mutex_and_readers_writers.md)

Interviewers for C++ systems roles open almost every loop with a language round. The questions are
predictable: what a vtable looks like, what `std::move` actually does, why `vector` copies instead
of moves when your move constructor is not `noexcept`, what lives in a `shared_ptr` control block,
which iterators a `push_back` invalidates, what the exception-safety guarantees are, and what the
difference is between C and C++ for a programmer like you. Every one of those is answered here to
the depth where you can derive the answer instead of reciting it.

## Prerequisites

- C at a professional level: pointers, structs, `malloc`/`free`, function pointers, header/source
  split, the build and link model. You have this.
- 01_Environment_Setup done: WSL Ubuntu 24.04 with `g++ --version` reporting GCC 13, and VS Code
  connected to WSL.
- Nothing else. No prior C++ beyond the arrays and binary search you are doing now.

## Modules

| #  | File | Topic | Hours |
|----|------|-------|-------|
| 01 | [`01_from_c_to_cpp.md`](01_from_c_to_cpp.md) | References, namespaces, overloading and name mangling, `const`, `auto`, `nullptr`, `enum class`, `extern "C"`, the build model | 2 |
| 02 | [`02_classes_constructors_destructors.md`](02_classes_constructors_destructors.md) | Object lifetime, construction/destruction order, initialiser lists, special member functions, `explicit`, `= default`, `= delete` | 2.5 |
| 03 | [`03_raii_the_core_idea.md`](03_raii_the_core_idea.md) | RAII, file and socket-fd wrappers, scope guards, stack unwinding, landing pads | 2.5 |
| 04 | [`04_value_semantics_copy_move.md`](04_value_semantics_copy_move.md) | Value categories, rvalue references, `std::move`, rule of five, copy elision and NRVO, parameter-passing choices, move-only handles | 3 |
| 05 | [`05_smart_pointers_and_ownership.md`](05_smart_pointers_and_ownership.md) | `unique_ptr` with custom deleters, `shared_ptr` control block, `weak_ptr`, cycles, `enable_shared_from_this`, ownership graphs | 3 |
| 06 | [`06_stl_containers_deep_dive.md`](06_stl_containers_deep_dive.md) | `vector` growth, `map` vs `unordered_map`, node vs contiguous, iterator invalidation, custom keys and hashes | 3 |
| 07 | [`07_stl_algorithms_lambdas_iterators.md`](07_stl_algorithms_lambdas_iterators.md) | Algorithms, erase-remove, strict weak ordering, lambdas and closures, `std::function` cost, iterator categories | 2.5 |
| 08 | [`08_templates_and_generic_programming.md`](08_templates_and_generic_programming.md) | Function and class templates, deduction, specialisation, traits, SFINAE, variadic templates, `if constexpr` | 3 |
| 09 | [`09_oop_inheritance_virtual.md`](09_oop_inheritance_virtual.md) | Inheritance, virtual functions, vtable and vptr layout, virtual destructors, `override`/`final`, interfaces vs composition | 3 |
| 10 | [`10_error_handling_exceptions_optional_expected.md`](10_error_handling_exceptions_optional_expected.md) | Exceptions, exception-safety guarantees, `noexcept`, `optional`, `variant`, `error_code`, expected-style results | 3 |
| 11 | [`11_const_constexpr_and_compile_time.md`](11_const_constexpr_and_compile_time.md) | `const` correctness, `constexpr` functions and tables, `static_assert`, compile-time UB detection | 2 |
| 12 | [`12_memory_model_layout_and_performance.md`](12_memory_model_layout_and_performance.md) | Layout, padding, alignment, cache lines, false sharing, stack vs heap, arenas and `pmr`, benchmarking, what `-O2` does, UB | 3.5 |
| 13 | [`13_cpp17_features_tour.md`](13_cpp17_features_tour.md) | Structured bindings, `if` with initialiser, `string_view`, `optional`/`variant`/`any`, `filesystem`, parallel algorithms, folds, CTAD, attributes, `shared_mutex` | 2 |
| 14 | [`14_writing_production_cpp.md`](14_writing_production_cpp.md) | Project layout, headers, ODR, warning flags, sanitizers, clang-format/tidy, Core Guidelines, code review, library API design | 2.5 |

Total: about 37.5 hours. At 4 to 5 hours a week alongside the other courses, this is 8 to 9 weeks.

Order: 01 to 05 are the core and must be done in order; everything else in C++ builds on RAII,
move semantics and ownership. 06 to 08 come next in order. 09, 10 and 11 can be done in any order
after that. 12 is the hardest and the most "senior" module; budget a second pass. 13 is a short tour
best done after 06 to 11. 14 ties everything together and leads straight into the tcpkv capstone.

## Supporting files

- `code/` — 28 complete programs plus `kv_store.hpp`. Each module names the files it uses.
  Files are named `mNN_short_name.cpp`, where `NN` is the module number.
- `EXERCISES.md` — every exercise from every module's "Try it" section, with difficulty and a hint.
- `INTERVIEW_QA.md` — 38 questions from basic to senior, with model answers.

## How to build the code

Build inside WSL, from a copy in the Linux filesystem (not under `/mnt/d`; see
01_Environment_Setup for why). Every C++ file compiles cleanly with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m04_move_only_handle.cpp -o m04_move_only_handle
./m04_move_only_handle
```

To build everything at once:

```sh
cd code
for f in m*.cpp; do
    g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 "$f" -o "${f%.cpp}" || echo "FAILED: $f"
done
```

Useful variants while studying:

```sh
# Catch memory and UB bugs (modules 03 to 06, 12)
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=address,undefined file.cpp -o file

# Checked STL: aborts on invalid iterators and bad comparators (modules 06, 07)
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -D_GLIBCXX_DEBUG file.cpp -o file

# See what the compiler generated (modules 03, 09, 11, 12)
g++ -std=c++17 -O2 -S -masm=intel file.cpp -o file.s

# Show copies and moves that elision would hide (module 04)
g++ -std=c++17 -Wall -Wextra -Werror -fno-elide-constructors m04_copy_elision_nrvo.cpp -o elide
```

`m12_*` benchmarks only mean something at `-O2` or higher. `m14_library_api_demo.cpp` includes
`kv_store.hpp`, so build it from inside `code/`.

## Suggested order with the other courses

- Do modules 01 to 05 here before 04_Concurrency/03_mutexes_lock_guard_unique_lock.md: a
  `lock_guard` is just RAII, and you should recognise it as such.
- Module 06 pairs with 03_Data_Structures_Algorithms: learn the STL container first, then implement
  its data structure from scratch there.
- Module 09 is the language half; 09_OOP_Design_Patterns_Architecture is the design half.
- Module 12 pairs with 05_Memory_Debugging_RCA (heap internals, sanitizers) and
  04_Concurrency/10_cache_lines_false_sharing_performance.md.
- Module 14 leads into 10_Capstone_Projects/tcpkv, steps 01 and 02.

## You are done when you can...

- [ ] Explain, without notes, the four things that change when a C programmer moves to C++:
      references, overloading and mangling, constructors/destructors, and the type system being
      stricter (no implicit `void*` conversion). Call a C library from C++ with `extern "C"`.
- [ ] State the construction and destruction order of a class with bases and members, and explain
      why a destructor is not called for an object whose constructor threw.
- [ ] Write a move-only RAII wrapper for a file descriptor (or `FILE*`, or a socket) from memory,
      with the move constructor, move assignment and destructor all correct and `noexcept`.
- [ ] Say what `std::move` really is (a cast to an rvalue reference, nothing more), when a named
      rvalue reference is an lvalue, and when the compiler elides a copy entirely.
- [ ] Apply the rule of zero, and when that is not possible, the rule of five, and justify which one
      a given class needs.
- [ ] Draw the memory layout of `make_shared<T>()` vs `shared_ptr<T>(new T)`, name what lives in
      the control block, and explain why `weak_ptr` keeps the block alive but not the object.
- [ ] Design the ownership graph for a small server (listener, connections, worker pool, shared
      store) and assign value, `unique_ptr`, `shared_ptr`, `weak_ptr` or reference to every edge.
- [ ] Explain `vector` growth, why it copies elements whose move constructor is not `noexcept`, and
      list which operations invalidate iterators for `vector`, `map` and `unordered_map`.
- [ ] Pick between `map`, `unordered_map` and a sorted `vector` for a given workload and explain
      the choice in terms of cache lines and node allocation.
- [ ] Replace raw loops with STL algorithms and lambdas, and explain what a lambda compiles into
      and what `std::function` costs.
- [ ] Write a class template, a variadic function template, and a type trait, and read a long
      template error down to the "required from here" line that points at your code.
- [ ] Draw an object with a vptr and its vtable, explain how a virtual call compiles, why a base
      class needs a virtual destructor, and when `final` lets the compiler devirtualise.
- [ ] Name the three exception-safety guarantees, show the copy-and-swap route to the strong one,
      and choose between exceptions, `optional`, `error_code` and an expected-style type for a given API.
- [ ] Explain padding and alignment, reorder a struct to shrink it, show false sharing in a
      benchmark, and name four kinds of undefined behaviour the optimiser exploits.
- [ ] Use the C++17 features that matter in production (`string_view`, `optional`, `variant`,
      structured bindings, `if` initialisers, `filesystem`, `shared_mutex`, `[[nodiscard]]`) and
      name the lifetime trap of `string_view`.
- [ ] Lay out a C++ library with a clean public header, build it warning-free with sanitizers, and
      explain what the One Definition Rule forbids.

## Resume lines this course backs up

- "C++17"
- "STL, RAII, smart pointers, templates, OOP, memory & ownership design, performance optimisation"
- tcpkv: "RAII socket and file-descriptor wrappers and move-only connection handles"
  (modules 03, 04, 05 and 14; built for real in 10_Capstone_Projects/tcpkv/02_step_raii_fd_and_socket.md)
- tcpkv: "shared_mutex" (module 13 introduces it; 04_Concurrency module 04 goes deep)
