# 11. const, constexpr and compile-time programming

**Time:** ~2 hours   **Prerequisites:** 08

## 1. In plain words

C has two ways to make a constant: `#define SIZE 256` (textual, untyped, invisible to the debugger)
and `const int size = 256;` (typed, but in C not usable as an array bound at file scope and
sometimes not folded). C++ fixes this with `constexpr`: a typed constant that the compiler *must* be
able to evaluate at compile time, and which you can use anywhere a literal can go. More importantly,
`constexpr` applies to **functions**: the same code that runs at run time can be executed by the
compiler to build lookup tables, validate configuration, or check invariants before the program ever
starts.

Analogy: `const` is a sign that says "do not touch". `constexpr` is a vending machine built into the
compiler: feed it constants and it hands you the answer baked into the binary.

## 2. The real thing

### 2.1 `const`: immutability

`const` means "cannot be modified through this name". It is about *access*, not about when the value is
known: `const int n = std::rand();` is fine. Uses:

- Variables: `const int max_retries = 5;` (internal linkage by default at namespace scope in C++).
- Pointers and references: `const T*` (pointee const), `T* const` (pointer const), `const T&`.
- Member functions: `long long balance() const;` promises not to modify `*this` (module 01).
- `mutable` members may be modified inside `const` member functions (caches, mutexes).
- `const_cast` removes const; modifying an object that was *defined* const through it is undefined
  behaviour.

### 2.2 `constexpr` variables

```cpp
constexpr int seconds_per_day = 60 * 60 * 24;     // must be a constant expression; implies const
constexpr double pi = 3.141592653589793;
int table[seconds_per_day / 3600];                 // usable as an array bound, template argument, case label
```

The initialiser is evaluated at compile time or the program does not compile. `constexpr` on a
variable also implies `const` and (since C++17) `inline` for `static` data members.

### 2.3 `constexpr` functions

```cpp
constexpr std::uint32_t crc32_entry(std::uint32_t i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}
static_assert(crc32_entry(1) == 0x77073096u);     // evaluated by the compiler
std::uint32_t x = crc32_entry(user_input);         // same function, evaluated at run time
```

A `constexpr` function *can* be evaluated at compile time when all its arguments are constants and
the context requires a constant (array bound, `static_assert`, `constexpr` variable, template
argument). Otherwise it is an ordinary function. Since C++14 the body may contain loops, local
variables, `if`, multiple statements. Not allowed (C++17): `try`, `goto`, `static` locals, uninitialised
variables, calls to non-`constexpr` functions, dynamic allocation (C++20 relaxes some of these),
`reinterpret_cast`, and undefined behaviour (the compiler must diagnose UB during constant evaluation,
which makes `constexpr` a UB detector).

### 2.4 Literal types

A type usable in constant expressions: scalars, references, arrays of literal types, and classes with a
`constexpr` constructor (or an aggregate), a trivial destructor, and literal-type members.
`std::array`, `std::pair`, `std::string_view`, `std::optional` of a literal type are literal types;
`std::string` and `std::vector` are not in C++17 (they allocate).

```cpp
struct Version {
    int major, minor, patch;
    constexpr bool operator<(const Version& o) const { /* ... */ }
    constexpr int packed() const { return major * 10000 + minor * 100 + patch; }
};
constexpr Version kMinSupported{1, 4, 0};
static_assert(kMinSupported < Version{2, 0, 0});
```

### 2.5 `static_assert`

A compile-time assertion with a message. Use it to pin down ABI assumptions (`sizeof(Header) == 12`),
type properties (`std::is_trivially_copyable_v<T>`), and the results of `constexpr` computations
(unit tests that run inside the compiler). A failing `static_assert` is a build error, which is the
cheapest possible time to find a bug.

### 2.6 Inline variables (C++17)

```cpp
inline constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();   // in a header
```

Before C++17, a non-`const` or class-type variable defined in a header produced "multiple definition"
link errors, so people wrote `extern` declarations plus one `.cpp` definition, or function-local
statics. `inline` on a variable means "every translation unit may define this; the linker keeps one".
`static constexpr` data members are implicitly inline, so they no longer need an out-of-class definition.

### 2.7 Compile-time tables

The pattern: a `constexpr` function returns a `std::array` filled by a loop; assign it to an
`inline constexpr` variable; use it. The compiler emits the finished table into `.rodata`. Zero
start-up cost, no "init on first use" race, and `static_assert` can verify entries. CRC tables, sine
tables, enum-to-string tables, state-machine transition tables, protocol opcode tables all fit.

### 2.8 `if constexpr` and `constexpr` lambdas

`if constexpr` (module 08) selects code at compile time. Lambdas are implicitly `constexpr` in C++17
when they can be, so `constexpr auto sq = [](int x) { return x * x; }; static_assert(sq(3) == 9);` works.

## 3. Code

`code/m11_constexpr_tables.cpp` builds a CRC-32 table at compile time and verifies it:

```cpp
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) t[i] = crc32_table_entry(i);
    return t;
}
inline constexpr auto kCrcTable = make_crc_table();              // 1 KiB in .rodata, computed by g++

constexpr std::uint32_t crc32(std::string_view data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) c = kCrcTable[(c ^ ch) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static_assert(crc32("123456789") == 0xCBF43926u);                 // the standard CRC-32 check value

constexpr Version kMinSupported{1, 4, 0};                          // literal type
static_assert(Version{1, 5, 2}.packed() == 10502);

const int     runtime_const = std::rand();      // const: read-only, value known at run time
constexpr int compile_const = 60 * 60 * 24;     // constexpr: value known at compile time

constexpr std::string_view op_name(Op op) { switch (op) { /* ... */ } }
static_assert(op_name(Op::Put) == "PUT");
```

Verify the table is really precomputed: `g++ -std=c++17 -O2 -S m11_constexpr_tables.cpp` and look
for `kCrcTable` in `.rodata` as 256 `.long` values, with no loop that fills it at start-up. Then
`objdump -d` the binary and confirm `crc32("123456789")` in `main` became a single constant load
(`mov esi, 0xcbf43926`).

## 4. Under the hood (senior-level)

**Constant evaluation is an interpreter inside the compiler.** GCC's `constexpr` evaluator walks the
function's tree representation, tracking the values of every variable, following loops and branches.
It has limits (`-fconstexpr-loop-limit`, default 262144 iterations; `-fconstexpr-ops-limit`,
33 million operations) to stop infinite compile times. The evaluation must be deterministic: no
reading uninitialised memory, no out-of-bounds, no signed overflow, no `reinterpret_cast`. Any UB
is a hard error with a message pointing at the operation, which is why pushing code into
`constexpr` tests is a cheap way to find UB.

**When is a `constexpr` function evaluated at compile time?** Only when the result is *required* to
be a constant: `constexpr` variable initialisers, `static_assert`, array bounds, template non-type
arguments, `case` labels, `if constexpr` conditions, enumerator values. In `int x = f(3);` the
compiler *may* fold it (and at `-O2` it will for simple cases), but it is not required to. To force
evaluation use `constexpr auto v = f(3);` and then use `v`.

**`.rodata` and relocations.** A `constexpr` table of integers lands in `.rodata`, shared between
processes (the page is file-backed and read-only). A table of pointers (`const char*` names) needs
load-time relocations unless the binary is non-PIE or the strings are placed relative; `std::string_view`
tables have the same issue because they contain a pointer. Still constant-initialised, but the
dynamic loader touches those pages at start-up (`.data.rel.ro`).

**Constant initialisation vs dynamic initialisation.** A global whose initialiser is a constant
expression is *constant-initialised*: the bytes are in the binary, no code runs at start-up, and it is
usable from any other global's constructor without ordering problems (no "static initialisation order
fiasco"). A global initialised by a non-`constexpr` call runs code at start-up in an unspecified order
across translation units. `constexpr` globals are always safe; `const` globals might not be.
C++20 adds `constinit` to demand constant initialisation without implying `const`.

**`inline` variables and the linker.** An `inline` variable is emitted as a weak symbol in a COMDAT
section in every translation unit that uses it (exactly like inline functions and template
instantiations); the linker keeps one copy. Address identity is preserved: `&kCrcTable` is the same in
every TU. Before C++17, `static const` in a header gave each TU its own copy with different addresses.

**`const` and the optimiser.** `const int n = 5;` at local scope lets the compiler fold `n` because
it sees the initialiser. `const int& n` or `const int* p` gives the optimiser *nothing*: the object
behind the reference may be modified by someone else (it is "const through this view", not
"immutable"). Only `constexpr` or a `const` object with a visible constant initialiser is truly
foldable. `const` on a by-value parameter (`void f(const int x)`) is purely local documentation.

**`constexpr` and ODR.** A `constexpr` function is implicitly `inline`, so defining it in a header
is fine and expected.

## 5. In production

- **Replace `#define` constants.** `constexpr int kMaxConnections = 1024;` is typed, scoped,
  visible to the debugger, and still folds. `#define` only remains for conditional compilation and
  include guards.
- **Protocol constants and wire formats.** `static_assert(sizeof(WireHeader) == 7)` next to the
  `#pragma pack` struct (module 12) makes a silent layout change a build error. The same for enum
  values that are part of the protocol: `static_assert(static_cast<int>(Op::Del) == 2)`.
- **Lookup tables at zero start-up cost.** A banking server that spends 200 ms at start-up building
  tables (character classes, checksum tables, holiday calendars for the next decade) can have most of
  them computed by the compiler instead, which also removes the "was it initialised yet?" race.
- **`constexpr` as unit tests.** `static_assert(parse_amount("12.50") == 1250)` runs on every build,
  on every developer machine, with no test harness and no flakiness. Keep the heavy tests in a real
  framework; use `static_assert` for invariants of pure functions.
- **Compile-time cost.** A `constexpr` loop of 10 million iterations takes seconds per translation unit
  on every build. Keep compile-time work small (tables of a few KB), or precompute offline and check it in.
- **3 AM debugging.** A value that "cannot change" changed: someone `const_cast`ed a `const` object or
  wrote through a pointer. If the object was `constexpr`/`const`-defined, the compiler may have folded
  reads of it, so the code reads the old value in one place and the new one in another. Modifying a
  defined-const object is UB; find the cast.

## 6. Try it

1. **(easy)** Build `m11_constexpr_tables.cpp` with `-O2 -S`. Find `kCrcTable` in `.rodata` and the
   constant `0xcbf43926` in `main`. Done when you can show both in the assembly.
2. **(easy)** Write `constexpr int days_in_month(int month, bool leap)` and `static_assert` every month.
   Then deliberately introduce an out-of-bounds array read and observe the compile-time UB diagnostic.
   Done when you can quote the error.
3. **(medium)** Write a `constexpr` enum-to-string table for a 6-value `enum class` as
   `std::array<std::string_view, 6>` with `static_assert`s that every entry is non-empty and that
   `table[static_cast<int>(E::Last)]` is correct. Done when adding a 7th enumerator breaks the build.
4. **(medium)** Implement `constexpr std::array<int, 1000> primes()` using a sieve, and time the
   compile with and without it (`time g++ ...`). Then raise to 100,000 and read the
   `-fconstexpr-loop-limit` / ops-limit error. Done when you can explain the trade-off.
5. **(hard)** Write a `constexpr` state-machine transition table for a tiny protocol parser
   (states: Start, Key, Sep, Value, Done; inputs: letter, colon, newline, other) and a
   `constexpr bool accepts(std::string_view)` that walks it. `static_assert` five inputs. Done when the
   same function parses runtime input and the table is in `.rodata`.

## 7. Interview questions

**Q: What is the difference between `const` and `constexpr`?**
`const` means the object cannot be modified through this name; the value may be computed at run time.
`constexpr` means the value must be a compile-time constant, usable as array bounds, template
arguments and in `static_assert`. `constexpr` implies `const`; the reverse is false.

**Q: When is a `constexpr` function evaluated at compile time?**
When it is called with constant arguments in a context that requires a constant expression
(`constexpr` variable, `static_assert`, array bound, template argument, `case` label). Elsewhere it is
a normal function, though the optimiser may still fold it.

**Q: What can a `constexpr` function not do in C++17?**
Allocate, use `static` or thread-local locals, `try`/`catch`, `goto`, `reinterpret_cast`, call
non-`constexpr` functions, leave variables uninitialised, or perform undefined behaviour (which is
diagnosed as an error during constant evaluation).

**Q: What is a literal type?**
A type whose values can exist in constant expressions: scalars, references, arrays of literal types,
and classes with a `constexpr` constructor or aggregate initialisation, a trivial destructor, and
literal-type members. `std::array` qualifies; `std::string` does not (in C++17).

**Q: What problem do `inline` variables solve?**
Defining a variable in a header without "multiple definition" link errors: each translation unit
emits a weak definition and the linker keeps one, with a single address program-wide. It replaces
`extern` declarations plus a `.cpp` definition for constants and makes `static constexpr` members
definition-free.

**Q: Why are `constexpr` globals immune to the static initialisation order fiasco?**
They are constant-initialised: their bytes are in the binary and no constructor runs at start-up, so
any other global's constructor can read them regardless of translation-unit order.

**Q: Does `const int& x` help the optimiser?**
No. `const` on a reference or pointer only restricts what you can do through it; the referenced
object may change through other paths, so the compiler must reload it. Only a `const` or `constexpr`
object with a visible constant initialiser is foldable.

## 8. Recap

- `const` = read-only access; `constexpr` = compile-time constant (implies `const`).
- `constexpr` functions run at compile time when the context requires a constant, else at run time;
  same source for both.
- Constant evaluation diagnoses undefined behaviour: `constexpr` tests find UB.
- Literal types: `constexpr` constructor, trivial destructor, literal members; `std::array` yes,
  `std::string` no (C++17).
- `static_assert` is a free, always-on unit test for invariants and ABI assumptions.
- `inline` variables (C++17) allow header-defined constants with one address; `static constexpr`
  members are implicitly inline.
- Compile-time tables: `constexpr` function returning `std::array`, assigned to an `inline constexpr`
  variable, landing in `.rodata`.
- Constant initialisation is free and order-safe; dynamic initialisation of globals is not.
- `const` through references and pointers gives the optimiser nothing.
