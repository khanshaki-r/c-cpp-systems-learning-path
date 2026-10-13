# 8. Templates and generic programming

**Time:** ~3 hours   **Prerequisites:** 06, 07

## 1. In plain words

In C, a generic container means `void*` and casts (losing type safety), or a macro that stamps out
code per type (losing debuggability). A **template** is a recipe the compiler follows to generate a
real, fully typed function or class for each set of types you use it with. `std::vector<int>` and
`std::vector<std::string>` are two different classes, both generated from one recipe, each as fast
as if you had written it by hand for that type.

The cost is that templates are compiled where they are *used*, not where they are *written*, so the
recipe must be visible (in a header) and errors show up at the use site, often with long messages.
Learning to read those messages, and to constrain templates so the messages get shorter, is most of
the skill.

Analogy: a template is a cookie cutter. `vector<int>` and `vector<string>` are two different cookies
from the same cutter. The cutter itself is not a cookie; you cannot eat `std::vector`.

## 2. The real thing

### 2.1 Function templates and deduction

```cpp
template <class T>
T max_of(T a, T b) { return (b < a) ? a : b; }

max_of(3, 7);          // T deduced as int
max_of(2.5, 1.5);      // T deduced as double
max_of(3, 2.5);        // error: conflicting deductions (int vs double)
max_of<double>(3, 2.5);// explicit: fine
```

Deduction happens from the arguments only (never from the return type). Deduction does not perform
conversions except trivial ones (array-to-pointer, adding `const`), which is why `3` and `2.5` conflict.

### 2.2 Class templates and non-type parameters

```cpp
template <class T, std::size_t N>
class RingBuffer { T buf_[N]{}; /* ... */ };

RingBuffer<int, 4> a;    // a distinct type from RingBuffer<int, 8>
```

Non-type parameters (integers, pointers, enums; also `auto` in C++17) make the value part of the type,
so the compiler can unroll and allocate statically. `std::array<T, N>` is the canonical example.

C++17 **class template argument deduction** (CTAD) lets you write `std::pair p{1, 2.5}` and
`std::lock_guard g(mutex)` without the angle brackets (module 13).

### 2.3 Instantiation and the header-only rule

A template is not compiled until it is **instantiated** with concrete arguments. At that point the
compiler needs the full definition, in the same translation unit. Hence templates live in headers.
Each translation unit that uses `vector<int>::push_back` generates its own copy; the linker merges
identical copies (they are "weak"/COMDAT symbols, module 14 on the ODR). Only the member functions
actually *used* are instantiated, so `std::vector<T>` for a `T` without `operator<` compiles fine as
long as you never call `sort` on it.

**Explicit instantiation** (`template class RingBuffer<int, 4>;` in one `.cpp`, with
`extern template` in the header) can move the code into one object file to speed up builds.

### 2.4 Two-phase lookup

A template is checked twice. At **definition** the compiler parses it and resolves every name that
does *not* depend on a template parameter. At **instantiation** it resolves the **dependent** names
(`item.size()` where `item`'s type depends on `T`). Consequences:
- Typos in non-dependent code are caught even if the template is never used.
- Calling a member of a dependent base class requires `this->member` or `Base<T>::member`, because
  the compiler cannot look into a base whose definition depends on `T` during phase one.
- `typename` is required before dependent type names: `typename Container::value_type x;`.

### 2.5 Specialisation

**Full specialisation** replaces the template for one exact set of arguments:

```cpp
template <> const char* max_of<const char*>(const char* a, const char* b) { return std::strcmp(b, a) < 0 ? a : b; }
```

**Partial specialisation** (class templates only) replaces it for a pattern:

```cpp
template <class T> struct Describe     { static const char* kind() { return "value"; } };
template <class T> struct Describe<T*> { static const char* kind() { return "pointer"; } };
```

Function templates cannot be partially specialised; overload them instead.

### 2.6 Variadic templates and fold expressions

```cpp
template <class... Ts>
void log_line(const Ts&... parts) { (std::cout << ... << parts) << '\n'; }   // C++17 fold

template <class... Ts> auto sum_all(Ts... xs) { return (xs + ... + 0); }
```

`Ts...` is a parameter pack; `sizeof...(Ts)` counts it; `parts...` expands it. Before C++17 you
peeled one argument per recursive call with a base-case overload; a fold expression applies an
operator across the pack in one line. `std::make_unique`, `emplace_back`, `std::tuple` and
`std::variant` are all variadic templates.

### 2.7 SFINAE, `if constexpr`, type traits

**Type traits** (`<type_traits>`) are compile-time predicates and transformers:
`std::is_integral_v<T>`, `std::is_same_v<A, B>`, `std::is_trivially_copyable_v<T>`,
`std::remove_reference_t<T>`, `std::decay_t<T>`, `std::conditional_t<cond, A, B>`.

**SFINAE** ("Substitution Failure Is Not An Error"): when substituting deduced types into a
function template's signature produces an invalid type, that candidate is silently dropped from
overload resolution instead of causing an error. `std::enable_if_t<cond, int> = 0` as a dummy
template parameter is the classic way to enable a template only for some types; `std::void_t<decltype(expr)>`
detects whether an expression is valid.

**`if constexpr`** (C++17) is the readable replacement for most SFINAE tricks inside a function body:
the branch not taken is discarded and never instantiated, so it may contain code that would not
compile for the current `T`.

**Concepts (C++20 preview).** `template <std::integral T> T twice(T x);` states the requirement
directly, gives readable errors ("constraints not satisfied: T does not satisfy integral"), and
replaces most `enable_if`. Know the syntax for interviews; use `enable_if` or `static_assert` in C++17.

### 2.8 Compile-time cost

Every distinct instantiation is parsed, checked and code-generated. Heavy template libraries
(Boost.Spirit, Eigen) make a `.cpp` take seconds to minutes. Mitigations: include less, use
explicit instantiation for hot templates, keep template bodies small and delegate to non-template
helpers, use `-ftime-report` or Clang's `-ftime-trace` to find the cost.

## 3. Code

`code/m08_templates_basics.cpp` covers deduction, specialisation, two-phase lookup, and SFINAE:

```cpp
template <class T> T max_of(T a, T b) { return (b < a) ? a : b; }
template <> const char* max_of<const char*>(const char* a, const char* b) { return (std::strcmp(b, a) < 0) ? a : b; }

template <class T, std::size_t N> class RingBuffer { /* push/pop; T buf_[N]{}; */ };
static_assert(std::is_trivially_copyable_v<RingBuffer<int, 4>>);   // no owned resources: memcpy-able

template <class Container>
std::size_t total_size(const Container& c) {
    std::size_t n = 0;
    for (const auto& item : c) n += item.size();   // dependent: checked only at instantiation
    return n;
}

// SFINAE: drops out when T has no .c_str(); the long long overload is then the only candidate
template <class T, class = std::void_t<decltype(std::declval<const T&>().c_str())>>
std::size_t text_len(const T& t) { return std::strlen(t.c_str()); }
std::size_t text_len(long long v) { return std::to_string(v).size(); }
```

`code/m08_variadic_if_constexpr.cpp` covers packs, folds, `if constexpr`, traits and `enable_if`:

```cpp
template <class T>
std::string to_text(const T& v) {
    if constexpr (std::is_same_v<T, std::string>)   return v;
    else if constexpr (std::is_arithmetic_v<T>)     return std::to_string(v);
    else if constexpr (std::is_pointer_v<T>)        return v ? to_text(*v) : std::string("null");
    else static_assert(sizeof(T) == 0, "to_text: unsupported type");   // only fires if reached
}

template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
T twice(T x) { return 2 * x; }      // twice(1.5) -> "no matching function"
```

Try `to_text(std::vector<int>{})` and read the error: it names the `static_assert` message and the
instantiation chain. Then try `twice(1.5)` and read that error: "no matching function... candidate
template ignored: requirement ... was not satisfied" (wording varies by GCC version).

## 4. Under the hood (senior-level)

**Instantiation produces real functions with mangled names.** `max_of<int>` becomes
`_Z6max_ofIiET_S0_S0_`. `nm -C` shows `int max_of<int>(int, int)`. Each instantiation is a weak
symbol placed in its own COMDAT section (`.text._Z6max_ofIiET_S0_S0_`); the linker keeps one copy and
discards the rest. This is why identical template code in 50 object files does not bloat the final
binary, but does bloat compile time and object file size.

**Why templates cannot be "compiled once" like C functions.** The compiler needs the concrete type
to know the size of `T`, which `operator<` to call, whether the copy is a `memcpy` or a constructor
call. Without the type there is no code to emit. C++20 modules reduce *parsing* cost by storing a
pre-parsed form, but instantiation still happens per use.

**Template code bloat is real and bounded.** `std::vector<int*>` and `std::vector<char*>` generate
separate code even though the machine code would be identical. libstdc++ mitigates in places by
having `vector<T*>` forward to a `vector<void*>` implementation (the "hoisting" technique). In your
own templates, move type-independent work into a non-template base or helper and keep the template
layer thin.

**Dependent names and `this->`.** In `template <class T> struct Derived : Base<T> { void f() { helper(); } }`,
`helper` is looked up in phase one, cannot be found (Base<T> is not examined because a
specialisation might remove it), and the compiler assumes a free function; the error says
"there are no arguments to 'helper' that depend on a template parameter". Writing `this->helper()`
defers lookup to phase two.

**SFINAE only applies in the immediate context.** A substitution failure inside the function *body*
is a hard error, not a dropped candidate. That is why `enable_if` goes in the signature (return type
or a template parameter), and why `if constexpr` inside the body is a different tool: it discards a
branch, it does not remove an overload.

**`if constexpr` and discarded statements.** Inside a template, a discarded branch is not
instantiated, so it may reference members `T` does not have. Outside a template (or in a
non-dependent branch), both branches are still checked, and `static_assert(false)` fires
unconditionally; hence the `sizeof(T) == 0` idiom to make it dependent.

**Type traits are implemented with specialisation and compiler intrinsics.** `std::is_pointer<T>`
is `false_type` with a partial specialisation `is_pointer<T*> : true_type`. Traits like
`is_trivially_copyable` need information only the compiler has and map to builtins
(`__is_trivially_copyable(T)`).

**Templates vs virtual functions.** Both achieve polymorphism. Templates resolve at compile time:
zero call overhead, inlining, but a separate instantiation per type and a header-only interface.
Virtual functions resolve at run time: one implementation, binary-compatible interface, an indirect
call. Module 09 covers the second; use templates when the type set is known at compile time and
performance matters, virtuals when the set is open (plugins) or when compile time and binary
boundaries matter.

## 5. In production

- **Error messages.** Read from the top: the first "required from here" line is your code; everything
  below is the instantiation stack. `static_assert` with a message at the top of a template turns a
  200-line error into one line. Clang's errors are shorter than GCC's; using both compilers in CI helps.
- **Header hygiene.** A template in a header that includes `<regex>` or `<iostream>` adds seconds to
  every TU that includes it. Keep template headers minimal, forward-declare where you can, and move
  non-template code into `.cpp` files.
- **Explicit instantiation for build speed.** If `Serializer<Message>` is used in 80 files, declare
  `extern template class Serializer<Message>;` in the header and `template class Serializer<Message>;`
  in one `.cpp`. The 80 files now only need the declaration.
- **Templates in library ABIs.** A template in a public header is compiled into every client; changing
  its body is an ABI break for them. Libraries that promise ABI stability keep templates thin and
  forward to non-template, exported functions.
- **When not to template.** If only one type is ever used, a template is slower to compile, harder to
  debug (template frames in gdb are long), and gives nothing. Write the concrete type. Generalise when
  the second type arrives.
- **3 AM debugging.** gdb shows frames like `std::__detail::_Map_base<...>::operator[]` 400 characters
  wide. `set print demangle on`, `set print asm-demangle on`, and `frame function <short name>` help;
  so does building with `-Og -g` for a debuggable optimised binary.

## 6. Try it

1. **(easy)** Write `template <class T> void print_all(const std::vector<T>&)` and call it with
   `int`, `std::string`, and a type without `operator<<`. Read the full error for the last and find
   the "required from here" line. Done when you can point at your own line in the error.
2. **(easy)** Add `static_assert(std::is_arithmetic_v<T>, "T must be a number")` at the top of
   `max_of` and re-run the failing call. Done when the error is one readable line.
3. **(medium)** Implement `template <class T, std::size_t N> class StaticVector` (fixed capacity,
   no heap, `push_back`/`pop_back`/`operator[]`/`size`) with placement new into an aligned byte array
   and correct destruction of only the live elements. Done when it holds `std::string`s without
   leaking under Valgrind and `sizeof(StaticVector<int, 8>)` is 8*4 + sizeof(size_t).
4. **(medium)** Write a `template <class T> constexpr bool has_size_v` trait using `std::void_t` that is
   true for `std::vector` and `std::string` and false for `int`. Then write the same with
   `if constexpr` inside a function. Done when both pass identical `static_assert`s.
5. **(hard)** Write a variadic `template <class... Fs> auto compose(Fs... fs)` that returns a lambda
   applying the functions right-to-left. Done when `compose(f, g, h)(x) == f(g(h(x)))` for lambdas
   of mixed types, and `compose()` returns identity.
6. **(hard)** Compile a file that uses `std::map<std::string, std::vector<int>>` with
   `g++ -std=c++17 -ftime-report` and read the time per phase. Then add `extern template` for a
   template of your own used across five `.cpp` files and measure total build time before/after.
   Done when you can quantify the saving.

## 7. Interview questions

**Q: Why do templates have to be in header files?**
The compiler generates code for a template only when it sees a use with concrete types, and it needs
the full definition at that point in the same translation unit. Splitting declaration and definition
across files works only with explicit instantiation of every type you will ever use.

**Q: What is two-phase lookup?**
Templates are checked at definition for everything not depending on a template parameter, and at
instantiation for dependent names. It catches typos early, and it is why you write `this->member`
for members of a dependent base class and `typename` before dependent type names.

**Q: What is SFINAE?**
If substituting deduced template arguments into a function template's signature produces an invalid
type or expression, the template is removed from overload resolution instead of causing a compile
error. It enables `enable_if`-style constraints and detection idioms. C++20 concepts replace most uses
with clearer syntax.

**Q: Difference between `if constexpr` and a normal `if` inside a template?**
A normal `if` compiles both branches for every instantiation; `if constexpr` discards the untaken
branch at compile time so it can contain code invalid for the current type. The condition must be a
constant expression.

**Q: What is a fold expression?**
A C++17 way to apply a binary operator across a parameter pack: `(xs + ... + 0)` or
`(std::cout << ... << args)`. It replaces recursive variadic templates with a base-case overload.

**Q: How do templates affect binary size and compile time?**
Each distinct instantiation is compiled separately in every translation unit that uses it; the linker
deduplicates identical weak symbols, so binary size grows with the number of distinct instantiations
while compile time grows with the number of uses. Explicit instantiation and thin template layers over
non-template code mitigate both.

**Q: Templates or virtual functions for polymorphism?**
Templates when the set of types is known at compile time and you want inlining and zero call
overhead; virtual functions when the set is open (plugins, runtime configuration) or when you need
a stable binary interface. Templates trade compile time and header exposure for speed.

**Q: Can you partially specialise a function template?**
No. Only class templates support partial specialisation. For functions, write overloads; the more
specialised overload wins by the normal rules.

## 8. Recap

- A template is a recipe; each set of arguments instantiates a separate, fully typed function or class.
- Deduction works from arguments only and does not convert; conflicting deductions are errors.
- Templates live in headers because instantiation needs the definition; the linker merges duplicates.
- Two-phase lookup: non-dependent names checked at definition, dependent at instantiation;
  `this->` and `typename` for dependent names.
- Full specialisation for exact types; partial specialisation for patterns (classes only).
- Variadic templates + fold expressions handle any number of arguments.
- SFINAE removes candidates in the signature; `if constexpr` discards branches in the body;
  `static_assert` gives readable errors; C++20 concepts replace `enable_if`.
- Type traits answer compile-time questions about types.
- Compile time and code size scale with instantiations; keep templates thin and use explicit
  instantiation for hot ones.
