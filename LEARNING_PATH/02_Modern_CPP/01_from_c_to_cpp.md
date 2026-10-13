# 1. From C to C++: what actually changes

**Time:** ~2 hours   **Prerequisites:** none (solid C assumed)

## 1. In plain words

C++ started as "C with classes" and it still compiles with the same toolchain, links with the same
linker, and produces the same kind of ELF binary you already debug. You do not throw away anything
you know about the stack, pointers, `malloc`, or how `main` is reached. What changes is the set of
tools the language gives you for saying *what you mean*: a reference instead of a pointer that
"must not be null", a namespace instead of a `bank_` prefix on every function, a function name that
can be reused for different parameter types, and a compiler that enforces `const` instead of
politely ignoring it.

Think of it like moving from a plain text editor to one with a type checker. The file format is the
same, the keyboard is the same, but a whole category of mistakes is now caught before you run anything.

This module is a tour of the small-but-everywhere changes. Each later module takes one big idea
(RAII, move semantics, templates, virtual dispatch) and goes deep.

## 2. The real thing

### 2.1 References

A reference is an alias for an existing object. Under the hood it is almost always implemented as a
pointer, but the language rules are different:

| | Pointer `T*` | Reference `T&` |
|---|---|---|
| Can be null | yes | no (binding to null is undefined behaviour) |
| Can be re-seated to another object | yes | no, bound once at initialisation |
| Syntax at the call site | `f(&x)` | `f(x)` |
| Syntax inside the function | `p->field`, `*p` | `r.field`, `r` |
| Arithmetic | yes | no |

```cpp
void deposit(Account& a, long long paise);        // "I will modify the caller's Account"
long long balance(const Account& a);              // "I will only read it, and I will not copy it"
```

Rule of thumb: pass by `const T&` when the object is bigger than a pointer and you only read it;
pass by `T&` when you modify it; pass by value for small things (`int`, `double`, small structs);
use a pointer only when "no object" is a legal input.

### 2.2 Namespaces

```cpp
namespace tcpkv {
    struct Connection { /* ... */ };
    void serve(Connection&);
}
tcpkv::Connection c;
using tcpkv::serve;    // import one name
```

`using namespace std;` in a header is a well-known mistake: it dumps hundreds of names into every file
that includes the header and causes silent ambiguities later. In a `.cpp` file it is tolerable; in a
header it is a code-review rejection.

### 2.3 Function overloading

Several functions may share a name if their parameter lists differ. The compiler chooses at compile
time by matching argument types (overload resolution). The return type does *not* take part.

```cpp
int    twice(int);
double twice(double);
twice(21);     // int version
twice(1.5);    // double version
twice('a');    // int version: char promotes to int
```

Overload resolution has precise ranking rules (exact match > promotion > conversion > user-defined
conversion). You rarely need the details, but you must know that an ambiguous call is a compile error,
not a silent pick.

### 2.4 `const` correctness

In C, `const` is advisory. In C++ it is a type property the compiler enforces through member functions:

```cpp
class Account {
public:
    long long balance() const;      // promise: does not modify *this. Callable on a const Account.
    void deposit(long long);        // not const: cannot be called on a const Account.
};
```

A function taking `const Account&` can only call `const` member functions. This propagates: once you
accept `const&` in your API, callers can pass temporaries and read-only data, and you cannot
accidentally mutate.

### 2.5 `auto`, range-for, uniform initialisation

```cpp
auto it = container.begin();          // type deduced from the initialiser (compile time, no cost)
for (const auto& x : container) ...   // iterates begin() to end(); works for arrays, vectors, maps, your own types
int x{5};                             // brace init: refuses narrowing. int y{3.7}; is an error.
std::vector<int> v{1, 2, 3};          // initialiser list
Account a{"ACC-1", 100};              // aggregate / constructor call
```

`auto` deduces a *value* type by default (it drops `const` and `&`). Write `auto&` or `const auto&`
when you want a reference. `auto x = v[0];` copies.

### 2.6 `nullptr` and `enum class`

`NULL` in C is `((void*)0)` or plain `0`. In C++ `0` converts to any pointer, so `f(NULL)` with
overloads `f(int)` and `f(char*)` calls the wrong one. `nullptr` has type `std::nullptr_t` and only
converts to pointers.

`enum class` gives scoped, strongly typed enumerators: no implicit conversion to `int`, no name
clashes between `Status::Ok` and `Result::Ok`, and the underlying type can be fixed
(`enum class Op : std::uint8_t { ... }`), which matters for wire formats.

### 2.7 `iostream` vs `printf`

`std::cout << x` is type-safe (no `%d` vs `long` mismatch) and extensible (you overload `operator<<`
for your types). `printf` is terser, usually faster, and format strings are checked by GCC with
`-Wformat`. Both are fine. In production servers you will mostly use a logging library anyway.
One trap: `std::endl` flushes the stream every time. Use `'\n'` unless you actually need a flush.

### 2.8 How a C++ program is built

Exactly like C: preprocess -> compile each translation unit to an object file -> link.

```
g++ -std=c++17 -c a.cpp -o a.o
g++ -std=c++17 -c b.cpp -o b.o
g++ a.o b.o -o prog           # g++ as the driver adds -lstdc++ automatically; gcc would not
```

Differences you will notice:
- **Name mangling.** Because of overloading and namespaces, the symbol for `int area(int)` is
  `_Z4areai`, not `area`. `nm -C` or `c++filt` demangle.
- **`extern "C"`.** Disables mangling for a declaration so C code (or `dlsym`) can find the symbol.
  Every C header you include from C++ is wrapped in `#ifdef __cplusplus extern "C" { #endif`.
- **Templates and inline functions in headers** produce the same code in several object files; the
  linker is allowed to merge them (see 14_writing_production_cpp.md on the ODR).
- **The standard library** is `libstdc++` on GCC. `ldd ./prog` shows it.

## 3. Code

`code/m01_c_vs_cpp_basics.cpp` exercises every item above in one small program. The key parts:

```cpp
namespace bank {
    struct Account { std::string id; long long balance_paise; };
    void deposit(Account& a, long long paise) { a.balance_paise += paise; }   // reference: same object
    long long balance(const Account& a) { return a.balance_paise; }          // const&: read-only view
}

int         twice(int x)                { return 2 * x; }      // three overloads, chosen at compile time
double      twice(double x)             { return 2.0 * x; }
std::string twice(const std::string& s) { return s + s; }

enum class Status { Ok, Timeout, Refused };

int main() {
    bank::Account acc{"ACC-001", 0};
    bank::deposit(acc, 50000);
    std::vector<int> v{3, 1, 4, 1, 5};
    for (int& x : v) x *= 10;                      // by reference: modifies in place
    auto total = 0LL;
    for (auto x : v) total += x;
    int* p = nullptr;
    Status s = Status::Timeout;
    std::cout << static_cast<int>(s) << '\n';      // explicit, never implicit
}
```

`code/m01_extern_c_mangling.cpp` shows mangling and C linkage:

```cpp
int    area(int side)    { return side * side; }           // symbol: _Z4areai
double area(double side) { return side * side; }           // symbol: _Z4aread
extern "C" int c_area(int side) { return side * side; }    // symbol: c_area

#ifdef __cplusplus
extern "C" {
#endif
int legacy_checksum(const char* data, int len);            // how every C header does it
#ifdef __cplusplus
}
#endif
```

Build and inspect on WSL:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread m01_extern_c_mangling.cpp -o mangling
nm mangling | grep -E 'area|checksum'       # _Z4areai, _Z4aread, c_area, legacy_checksum
nm -C mangling | grep -E 'area|checksum'    # area(int), area(double), c_area, legacy_checksum
echo _Z4areai | c++filt                     # area(int)
```

## 4. Under the hood (senior-level)

**References are pointers without the syntax.** `void f(int& r)` compiles to the same machine code
as `void f(int* p)`. The compiler knows `r` is never null, so it can drop null checks and sometimes
keep the referenced value in a register. A reference member in a class takes `sizeof(void*)` bytes.
Binding a reference to a dangling object is undefined behaviour exactly like dereferencing a freed pointer.

**Overload resolution is compile-time only.** After it picks a candidate the call is a direct `call`
instruction, same as C. There is no runtime dispatch (that is what `virtual` is for, module 09).

**Mangled names encode the signature.** `_Z` prefix, then `4area` (length-prefixed name), then
parameter type codes (`i` = int, `d` = double, `PKc` = pointer to const char, `RSt6vectorIiE` =
reference to `std::vector<int>`). Namespaces appear as `N...E` nesting. This is the Itanium C++ ABI,
shared by GCC and Clang on Linux; MSVC uses a different scheme (`?area@@YAHH@Z`), which is why you
cannot link MSVC and MinGW C++ object files together, while plain C objects link fine.

**`extern "C"` changes linkage, not calling convention.** On x86-64 Linux both C and C++ use the
System V ABI, so a C function and a C++ function with the same signature are called identically.
`extern "C"` only suppresses mangling. You cannot overload an `extern "C"` function (two functions
would need the same symbol).

**`enum class` has a fixed size.** Default underlying type is `int`; with `: std::uint8_t` it is one
byte and can be `memcpy`'d into a wire header. Plain `enum` has an implementation-defined underlying
type unless you fix it.

**Brace initialisation and narrowing.** `int x{3.7}` is a compile error because the standard forbids
narrowing conversions in list-initialisation. This is the one place where C++ is stricter than C about
implicit conversions by default; everywhere else (`int x = 3.7;`) it still truncates silently.

**`auto` and `decltype` are compile-time.** `auto` never adds runtime cost. It can hide an expensive
copy though: `auto row = table[i];` copies the row; `const auto& row = table[i];` does not.

## 5. In production

- **Mixing C and C++ in one binary** is normal. Your existing C libraries (the Berkeley DB layer,
  the RPC marshalling code) can be linked into a C++ server as long as their headers carry
  `extern "C"`. Compile C files with `gcc` or `g++ -x c`, link with `g++`.
- **`extern "C"` and callbacks.** When a C API wants a function pointer (`qsort`, `pthread_create`,
  `signal`), pass an `extern "C"` function or a non-capturing lambda converted to a function pointer.
  Never pass a member function pointer.
- **Shared libraries and the ABI.** A `.so` built with GCC 13 exposes mangled names; a client built
  with the same compiler finds them. Mixing libstdc++ versions is usually fine on Linux because
  libstdc++ keeps backward ABI compatibility, but mixing `-D_GLIBCXX_USE_CXX11_ABI=0` and `=1`
  builds is not (you will see `std::__cxx11::basic_string` in the undefined-symbol error).
- **`const` as documentation.** In code review, a member function that could be `const` but is not is
  a smell: it tells readers "this might mutate" when it does not, and it blocks callers who hold a
  `const&`.
- **3 AM debugging.** A crash in `std::__cxx11::basic_string<char>::_M_assign` is not a bug in the
  standard library. It is almost always a dangling reference or a buffer overrun somewhere else that
  corrupted the string's internals. Build with `-fsanitize=address,undefined` and run again; this is
  the C++ equivalent of running your C code under Application Verifier.

## 6. Try it

1. **(easy)** Write `swap_values(int& a, int& b)` using references, then write the C version with
   pointers. Done when both compile and you can explain why the reference version cannot be called
   with `nullptr`.
2. **(easy)** Build `m01_extern_c_mangling.cpp`, run `nm` with and without `-C`, and write down the
   mangled name of a function `std::string greet(const std::string&, int)` you add. Done when you can
   read `PKc`, `i`, and `RK` fragments.
3. **(medium)** Take any 100-line C file you own, rename it `.cpp`, and compile it with
   `g++ -std=c++17 -Wall -Wextra -Werror`. Fix every error. Done when it compiles and you have a list
   of the three most common differences you hit (expect: implicit `void*` conversions, `new`/`class`
   as identifiers, missing casts on `malloc`).
4. **(medium)** Write a C header `legacy.h` with a function, compile its implementation with
   `gcc -c`, and call it from a C++ `main.cpp` with proper `extern "C"` guards. Done when `g++ main.cpp legacy.o`
   links and runs.
5. **(hard)** Create overloads `void log(int)`, `void log(long)`, `void log(double)`, `void log(const char*)`.
   Call `log(NULL)`, `log(0)`, `log('x')`, `log(3.0f)`, `log(nullptr)` and predict which one is
   chosen before compiling. Done when every prediction matches and you can explain the one that is
   ambiguous or does not compile.

## 7. Interview questions

**Q: What is the difference between a reference and a pointer?**
A reference is an alias bound once to an existing object; it cannot be null, cannot be re-seated,
and has no arithmetic. A pointer is a value that may be null and may be reassigned. Both usually
compile to the same machine code; the difference is in what the compiler will let you write.

**Q: Why does C++ mangle names and what does `extern "C"` do?**
Overloading and namespaces mean many functions can share a source-level name, so the linker symbol
must encode the full signature. `extern "C"` tells the compiler to emit the plain, unmangled symbol
so C code and `dlsym` can find it. It does not change the calling convention on Linux x86-64.

**Q: Can you overload a function on return type alone?**
No. Overload resolution only looks at the arguments. Two functions that differ only in return type
are a redefinition error.

**Q: What does `enum class` give you over a plain `enum`?**
Scoped names (no clashes), no implicit conversion to `int`, and a fixed underlying type you can
choose. The price is writing `static_cast<int>(e)` when you really want the number.

**Q: `auto x = vec[i];` versus `auto& x = vec[i];`: what is the difference?**
The first copies the element into `x`; the second makes `x` an alias for the element inside the
vector. For `int` the copy is free; for `std::string` or a large struct it is an allocation and a
memcpy. `auto` always deduces a value type unless you add `&`.

**Q: Why is `using namespace std;` in a header considered wrong?**
It injects every standard name into every translation unit that includes the header, so a later
addition to the standard library (or to the user's code) can silently change which function an
unqualified call resolves to, or make it ambiguous.

**Q: How would you call a C library compiled with gcc from a C++ program?**
Include its header, which must declare its functions inside `extern "C" { }` (guarded by
`#ifdef __cplusplus`). Compile the C sources with the C compiler, compile the C++ sources with g++,
and link everything with g++ so libstdc++ is pulled in.

## 8. Recap

- C++ uses the same compile-and-link pipeline as C; the differences are mangled symbols, `extern "C"`,
  and libstdc++ linked in by the `g++` driver.
- A reference is a non-null, non-reseatable alias; prefer `const T&` for read-only parameters.
- Overloading is resolved at compile time by argument types; return type does not participate.
- `const` member functions are enforced by the compiler and are part of your API contract.
- `auto` deduces a value type; add `&` or `const&` to avoid copies.
- Brace initialisation forbids narrowing; `nullptr` is a typed null; `enum class` is scoped and typed.
- Range-for works for any type with `begin()`/`end()`.
- `std::endl` flushes; use `'\n'`.
- Mangled names follow the Itanium ABI on Linux; `c++filt` and `nm -C` decode them.
