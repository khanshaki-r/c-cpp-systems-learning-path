# 4. Value semantics: copy, move, and the rule of five

**Time:** ~3 hours   **Prerequisites:** 02, 03

## 1. In plain words

In C, `a = b` for structs copies the bytes. If the struct holds a pointer, both copies now point at
the same memory and you have to decide by convention who frees it. C++ lets a type define what
copying *means*: `std::string a = b;` makes an independent deep copy. That is **value semantics**:
objects behave like integers, independent after copy.

Deep copying is expensive when the source is about to die anyway, like a function's return value or a
temporary. **Move semantics** (C++11) adds a second operation: instead of copying the contents,
*steal* them, leaving the source empty but valid. Moving a `std::vector` of a million elements
copies three pointers.

Analogy: copying a house means building an identical second house. Moving a house means handing over
the keys: the first owner is left with nothing, but nothing was rebuilt.

## 2. The real thing

### 2.1 Value categories: lvalue and rvalue

An **lvalue** is an expression that names an object that persists: a variable, `*p`, `arr[i]`,
a function returning `T&`. An **rvalue** is a temporary or something about to expire: `f()` returning
by value, `T{}`, `a + b`, or `std::move(x)`.

The compiler uses the category to pick between copy and move:

```cpp
std::string a = b;              // b is an lvalue  -> copy constructor
std::string c = make_name();    // rvalue          -> move constructor (or elided entirely)
std::string d = std::move(a);   // std::move casts a to an rvalue -> move constructor; a is now moved-from
```

`std::move` does **not** move anything. It is a cast: `static_cast<T&&>(x)`. It says "you may treat
this as a temporary". The move happens in whatever constructor or assignment receives it.

### 2.2 Rvalue references

`T&&` binds to rvalues only. It is how you write an overload that receives temporaries:

```cpp
void push(const std::string& s);   // called for lvalues: must copy
void push(std::string&& s);        // called for rvalues: may steal from s
```

Inside the second function, `s` itself is an lvalue (it has a name), so to actually move its contents
you write `std::move(s)` again. Forgetting that is the most common move-semantics bug: a `T&&`
parameter that is copied.

### 2.3 The rule of five, written correctly

```cpp
class Buffer {
    char* data_ = nullptr;
    std::size_t size_ = 0;
public:
    Buffer() noexcept = default;
    explicit Buffer(std::size_t n);
    ~Buffer() { std::free(data_); }                                  // 1: release

    Buffer(const Buffer& o);                                         // 2: deep copy (allocate + memcpy)
    Buffer(Buffer&& o) noexcept                                      // 3: steal
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

    Buffer& operator=(Buffer o) noexcept { swap(*this, o); return *this; }   // 4 and 5 together: copy-and-swap
    friend void swap(Buffer& a, Buffer& b) noexcept { std::swap(a.data_, b.data_); std::swap(a.size_, b.size_); }
};
```

Key points:
- **Move operations must be `noexcept`.** `std::vector` only moves elements during reallocation if
  the move constructor is `noexcept`; otherwise it copies, to preserve the strong exception
  guarantee. A non-noexcept move constructor silently makes your type slow in containers.
- **Moved-from state must be valid.** The destructor will run on the source. `free(nullptr)` is fine;
  `fclose(nullptr)` is not. Reset handles to their empty value.
- **Copy-and-swap** gives one assignment operator for both lvalues and rvalues (the parameter is
  copy-constructed or move-constructed depending on the argument), handles self-assignment, and is
  strongly exception-safe: if the copy throws, `*this` is untouched.
- Declaring *any* of the five suppresses the implicit move operations. Declaring a move suppresses
  implicit copies. Write all five or none (rule of zero).

### 2.4 Copy elision and NRVO

C++17 **guarantees** that `T x = T(...)` and `return T(...)` (a prvalue) construct the object directly
in its final location: zero copies, zero moves, even if the type is not movable. For a *named* local
(`T t; ...; return t;`) the compiler is *allowed* to elide (NRVO) and GCC does it at every
optimisation level when there is a single return object; otherwise it falls back to a move. Writing
`return std::move(t);` is wrong: it disables NRVO and forces a move. GCC warns with `-Wpessimizing-move`.

### 2.5 Choosing parameter types

| You want to | Take | Why |
|---|---|---|
| read, not copy | `const T&` | no copy, binds to everything |
| modify the caller's object | `T&` | |
| keep a copy ("sink": store into a member, push into a container) | `T` by value, then `std::move` into place | 1 copy for lvalues, 0 for rvalues, one overload |
| steal specifically from temporaries | `T&&` plus a `const T&` overload | only when the by-value version's extra move matters |
| small trivially copyable (`int`, `double`, `string_view`, pointers) | `T` by value | cheaper than a reference |
| forward anything to another function | `T&&` in a template + `std::forward<T>(x)` | perfect forwarding |

Return by value. Returning `const T&` to a local is a dangling reference; returning `T&&` almost never
makes sense.

## 3. Code

`code/m04_move_only_handle.cpp` contains `UniqueFd` (move-only) and `Buffer` (full rule of five, as
above). The part that shows the noexcept rule and moved-from state:

```cpp
std::vector<UniqueFd> fds;
fds.push_back(UniqueFd(3));         // rvalue: moved in
UniqueFd f(4);
fds.push_back(std::move(f));        // f is now moved-from: valid, holds -1
// fds.push_back(f);                // error: copy constructor is deleted
fds.emplace_back(5);                // constructed in place
// vector grows by MOVING elements only because UniqueFd(UniqueFd&&) is noexcept
std::vector<UniqueFd> moved = std::move(fds);   // O(1): three pointers change hands; fds is empty
```

`code/m04_copy_elision_nrvo.cpp` uses a `Loud` type whose special members print, so you can see
exactly what happens. Build it twice, once with `-fno-elide-constructors`, and compare:

```cpp
Loud make_prvalue() { return Loud("prvalue"); }   // C++17: guaranteed elision, prints only "ctor"
Loud make_nrvo()    { Loud l("nrvo"); l.tag += "!"; return l; }   // NRVO: "ctor" only on GCC; MOVE if disabled
Loud make_no_nrvo(bool flag) { Loud a("a"), b("b"); if (flag) return a; return b; }   // two candidates: MOVE

void by_value(Loud l);         // COPY for lvalue arguments, MOVE for rvalues
void by_cref(const Loud& l);   // nothing
void by_rref(Loud&& l);        // nothing at the call; the move happens inside when you std::move(l)

struct Person {
    std::string name;
    explicit Person(std::string n) : name(std::move(n)) {}   // sink parameter
};
Person p1(n);              // 1 copy into the parameter, 1 cheap move into the member
Person p2(std::move(n));   // 0 copies
```

Expected highlights of the output:

```
-- prvalue (guaranteed elision)
ctor  prvalue
-- nrvo
ctor  nrvo
-- no nrvo possible
ctor  a
ctor  b
MOVE  a
-- by_value(lvalue): COPY
COPY  prvalue
```

## 4. Under the hood (senior-level)

**A move is a shallow copy plus a reset.** For `std::vector`, the move constructor copies three
pointers (begin, end, capacity end) and nulls the source's. For `std::string` with libstdc++'s
small-string optimisation, a short string (15 chars or fewer) lives inside the object, so moving it
*is* copying 32 bytes; only long strings move by pointer steal. Moving is not free, it is just cheap.

**Why `noexcept` matters to `std::vector`.** On reallocation the vector must relocate N elements into
the new buffer. If relocation can throw halfway, some elements have been moved out of the old buffer
and the vector cannot restore the old state: the strong exception guarantee is lost. So
`std::vector` uses `std::move_if_noexcept`: it moves when the move constructor is `noexcept`, and
copies otherwise. Check with `static_assert(std::is_nothrow_move_constructible_v<T>)`.

**Moved-from state in the standard library.** The standard says "valid but unspecified" for most
types. Practically, libstdc++ leaves a moved-from `std::string` empty and a moved-from
`std::vector` empty, but code relying on that is non-portable. `std::unique_ptr` is guaranteed null
after a move. The safe operations on any moved-from object: assign to it, destroy it, or call
functions with no preconditions (`size()`, `empty()`, `clear()`).

**Copy elision mechanics.** Without elision, `Loud x = make_prvalue();` would construct a temporary
in the callee's frame and then move it to `x`. With elision, the caller passes the address of `x`
as a hidden first argument (the return slot) and the callee constructs directly there. This is why
returning large objects by value is free in modern C++ and why "return by output parameter" is a
C-era habit to drop.

**Implicit moves on return.** `return local;` treats `local` as an rvalue first (so a move is tried),
then as an lvalue. Writing `std::move` there is redundant at best (GCC: `-Wredundant-move`) and
harmful when it blocks NRVO (`-Wpessimizing-move`, in `-Wall`).

**Self-move.** `x = std::move(x)` is legal and must leave `x` valid. Copy-and-swap handles it
(result: `x` ends up empty). Hand-written move assignment needs `if (this != &o)` or must be written
so the order of operations is safe. GCC 13's `-Wself-move` flags the obvious cases.

**Perfect forwarding.** In `template <class T> void f(T&& x)`, `T&&` is a *forwarding reference*:
`T` deduces to `U&` for lvalues and `U` for rvalues, and reference collapsing (`U& &&` -> `U&`)
makes `x` an lvalue or rvalue reference accordingly. `std::forward<T>(x)` restores the original
category when passing `x` on. This is how `emplace_back` and `make_unique` avoid copies.

## 5. In production

- **Move-only handle types** make ownership transfer explicit in function signatures. In tcpkv, a
  `Connection` is move-only: passing it to a worker thread by value *is* the hand-off, and the
  compiler stops two threads from both owning it.
- **Hidden copies are the most common performance bug in C++ written by C programmers.**
  `for (auto conn : connections)` copies every element. `std::string s = view;` allocates. Passing
  `std::vector` by value to a function that only reads it copies the whole thing. Build with
  `-Wall -Wextra` and watch for `-Wrange-loop-construct` (in `-Wall` since GCC 11).
- **`std::move` on a `const` object silently copies.** `const T&&` cannot bind to a move constructor
  taking `T&&`, so the copy constructor is chosen. No warning. Grep for `std::move(` on `const` locals
  and on members accessed through `const` methods.
- **Returning `std::move(member)` from a getter** destroys the object's state for the next caller.
  A move-out should be a named, documented operation (`take_result()`), never a getter.
- **Benchmark before adding `T&&` overloads.** The by-value sink costs one extra move over the
  `const&`/`&&` pair. For a `std::string` that is 32 bytes of copying. Add the overloads only when
  profiling shows it matters.
- **3 AM debugging.** A use-after-move: an object is moved into a container inside a loop and then
  used after the loop. Symptoms: empty strings, null pointers, "impossible" states. clang-tidy's
  `bugprone-use-after-move` catches most cases statically; enable it in CI (module 14).

## 6. Try it

1. **(easy)** Build `m04_copy_elision_nrvo.cpp` with and without `-fno-elide-constructors`. Count
   the MOVE and COPY lines in each. Done when you can explain every difference.
2. **(easy)** Change `UniqueFd`'s move constructor to drop `noexcept`, then add a
   `static_assert(std::is_nothrow_move_constructible_v<UniqueFd>)`. Done when you see it fail and
   can explain what `std::vector` would do differently.
3. **(medium)** Write `class Matrix` (owning `double*`, rows, cols) with the full rule of five using
   copy-and-swap. Verify with a `Loud`-style print that `Matrix m = a * b;` performs zero copies.
   Done when the output shows only constructions and moves.
4. **(medium)** Write `void set_name(std::string)` (sink) and the pair
   `set_name(const std::string&)` / `set_name(std::string&&)`. Call each with an lvalue, an rvalue, and
   a string literal, and count copies and moves. Done when you have a table of 6 cells and can say
   when the two-overload version is worth it.
5. **(hard)** Write a `template <class... Args> T& emplace(Args&&... args)` for a tiny fixed-capacity
   container using perfect forwarding and placement new. Verify with `Loud` that `emplace("x")`
   constructs exactly once. Done when `emplace(std::move(loud))` prints one MOVE and `emplace(loud)`
   prints one COPY.
6. **(hard)** Find the bug: a function takes `std::string&& s` and does `names.push_back(s);`. Fix it,
   then explain why the original compiles without warning. Done when you can state the rule "a named
   rvalue reference is an lvalue".

## 7. Interview questions

**Q: What does `std::move` do?**
Nothing at run time. It is a cast to an rvalue reference (`static_cast<T&&>`). The move actually
happens in the move constructor or move assignment that receives the result. Applied to a `const`
object it silently results in a copy.

**Q: What state is an object in after being moved from?**
Valid but unspecified for most standard types: it can be destroyed or assigned to, and functions
without preconditions (`size`, `clear`) are safe. `std::unique_ptr` is guaranteed null. Your own
types should reset handles so the destructor is a no-op.

**Q: Why must move constructors be `noexcept`?**
Containers like `std::vector` relocate elements during reallocation. If the move could throw midway,
the old buffer would be partially destroyed and the strong exception guarantee lost, so the vector
falls back to copying unless the move is `noexcept`. A non-noexcept move makes your type slower
without any warning.

**Q: Explain copy-and-swap.**
Write assignment as `T& operator=(T other) { swap(*this, other); return *this; }`. The parameter is
constructed by copy or move depending on the argument, so one function serves both. The swap is
`noexcept`, so if the copy throws nothing has changed (strong guarantee), and self-assignment is
handled because the copy is made before anything is modified.

**Q: What is guaranteed copy elision in C++17?**
Initialising an object from a prvalue (`T x = T(...)`, `return T(...)`) constructs it directly in
place with no copy or move, and it works even for non-movable types. NRVO (returning a named local)
remains optional but is done by GCC when there is a single return object.

**Q: Should you write `return std::move(local);`?**
No. `return local;` already tries a move, and `std::move` prevents NRVO, so it is strictly worse.
GCC flags it with `-Wpessimizing-move` / `-Wredundant-move`.

**Q: How do you pass a parameter you intend to store?**
Take it by value and `std::move` it into place. Lvalue callers pay one copy (unavoidable), rvalue
callers pay one cheap move, and there is a single overload to maintain. Add `const&` / `&&` overloads
only if profiling shows the extra move matters.

**Q: What is a forwarding reference?**
`T&&` where `T` is a deduced template parameter. It binds to both lvalues and rvalues, deducing `T`
as `U&` or `U`, and `std::forward<T>(x)` passes the argument on with its original value category.
This is how `emplace_back` constructs elements in place with zero copies.

## 8. Recap

- Lvalues name persistent objects; rvalues are temporaries or `std::move`d objects.
- `std::move` is a cast; the move happens in the receiving constructor/assignment.
- Move = steal the guts and reset the source; the source must remain destructible.
- Move operations must be `noexcept`, or `std::vector` will copy instead.
- Rule of five: destructor, copy ctor, copy assign, move ctor, move assign; copy-and-swap merges the
  last two safely.
- C++17 guarantees elision for prvalues; NRVO is optional but GCC does it; never `return std::move(x)`.
- Parameter rules: `const T&` to read, `T` by value to sink, `T&&` + `std::forward` to forward.
- A named `T&&` parameter is an lvalue inside the function; `std::move` it to actually move.
- `std::move` on `const` copies silently; use-after-move is caught by clang-tidy, not the compiler.
