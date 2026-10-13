# 2. Classes, constructors and destructors: object lifetime

**Time:** ~2.5 hours   **Prerequisites:** 01_from_c_to_cpp.md

## 1. In plain words

In C a `struct` is a bag of bytes. You `memset` it, fill it, and remember to clean up whatever it
points to. In C++ a class is a bag of bytes **plus two guaranteed hooks**: a constructor that runs
exactly once when the object comes into existence, and a destructor that runs exactly once when it
goes out of existence. Everything in modern C++ (RAII, smart pointers, lock guards, move semantics)
is built on the promise that those two hooks always run, in a defined order, no matter how the
scope is left.

Analogy: a hotel room. Check-in (constructor) always happens before you can use the room; check-out
(destructor) always happens when you leave, even if you leave through the fire exit (an exception).
The hotel, not the guest, guarantees the room is cleaned.

## 2. The real thing

### 2.1 Lifetime

An object's lifetime begins when its constructor finishes and ends when its destructor starts.
Where the object lives decides *when* that happens:

| Storage | Constructed | Destroyed |
|---|---|---|
| Local (automatic) | when the declaration is reached | when the enclosing block exits (any way) |
| Global / namespace-scope | before `main` starts (within one TU, in declaration order) | after `main` returns, reverse order |
| `static` local | first time the declaration is reached (thread-safe since C++11) | after `main`, reverse order of construction |
| Member | as part of the enclosing object's construction | as part of its destruction |
| Heap (`new`) | at the `new` expression | at the matching `delete` (or never, if you forget) |
| Temporary | at the expression | at the end of the full expression (the `;`) |

### 2.2 Construction order (fixed by the standard)

For `Derived d;`:
1. Base class constructors (in the order bases are listed).
2. Members of `Derived`, **in declaration order** (not initialiser-list order).
3. The body of `Derived::Derived()`.

Destruction is the exact reverse: body of `~Derived`, members in reverse declaration order, then
bases. GCC's `-Wreorder` warns when your initialiser list is written in a different order than the
declarations, because the written order is misleading.

### 2.3 Member initialiser lists

```cpp
class Account {
    const std::string id_;   // const member: must be initialised, cannot be assigned
    long long balance_;
public:
    Account(std::string id, long long b) : id_(std::move(id)), balance_(b) {}   // initialise directly
};
```

Assigning in the body (`id_ = id;`) would first default-construct `id_` and then assign: two
operations instead of one, and impossible for `const` members, reference members, and types without
a default constructor. Always use the initialiser list. Default member initialisers
(`long long balance_ = 0;`) are the second tool; the initialiser list overrides them.

### 2.4 The special member functions

Six functions the compiler can write for you:

```cpp
struct T {
    T();                        // default constructor
    ~T();                       // destructor
    T(const T&);                // copy constructor
    T& operator=(const T&);     // copy assignment
    T(T&&);                     // move constructor        (module 04)
    T& operator=(T&&);          // move assignment         (module 04)
};
```

The generated versions are memberwise: copy each member, move each member, destroy each member.
That is exactly right when the class owns no raw resource, and exactly wrong when it does
(two objects would both `free` the same pointer).

### 2.5 `explicit`, `= default`, `= delete`

- `explicit T(int)` stops `T t = 5;` and `f(5)` where `f` takes a `T`. Single-argument constructors
  should be `explicit` unless the conversion is genuinely intended (like `std::string` from `const char*`).
- `T() = default;` asks for the compiler-generated version. Needed when another constructor you wrote
  suppressed it. Also documents intent.
- `T(const T&) = delete;` removes a function. Calling it is a compile error with a clear message.
  This is how you make a type non-copyable.

### 2.6 Rule of zero / three / five (preview)

- **Rule of zero:** if your class owns no raw resource (it holds `std::string`, `std::vector`,
  `std::unique_ptr`, ints), write **no** special members. The generated ones are correct.
- **Rule of three (C++98):** if you write any of destructor / copy ctor / copy assignment, you almost
  certainly need all three, because you are managing a resource.
- **Rule of five (C++11):** add move ctor and move assignment to the three, or copies will be used
  where moves were possible. Module 04 does this in full.

Aim for rule of zero. Write rule-of-five classes only in thin wrappers around raw resources
(file descriptors, `FILE*`, C library handles), then compose everything else out of those.

## 3. Code

`code/m02_ctor_dtor_order.cpp` prints the order of every construction and destruction:

```cpp
struct Tracer {
    std::string name;
    explicit Tracer(std::string n) : name(std::move(n)) { std::cout << "  construct " << name << '\n'; }
    ~Tracer() { std::cout << "  destroy   " << name << '\n'; }
};

struct Base {
    Tracer t{"Base::t"};
    Base()  { std::cout << "  Base() body\n"; }      // body runs after all members exist
    ~Base() { std::cout << "  ~Base() body\n"; }     // body runs before members die
};

struct Derived : Base {
    Tracer a{"Derived::a"};   // constructed in declaration order ...
    Tracer b{"Derived::b"};   // ... regardless of initialiser-list order
    Derived()  { std::cout << "  Derived() body\n"; }
    ~Derived() { std::cout << "  ~Derived() body\n"; }
};
```

Expected output for the `Derived d;` block:

```
construct Base::t
Base() body
construct Derived::a
construct Derived::b
Derived() body
d alive, sizeof=...
~Derived() body
destroy   Derived::b
destroy   Derived::a
~Base() body
destroy   Base::t
```

`code/m02_explicit_default_delete.cpp` shows the keywords:

```cpp
class Meters {
public:
    explicit Meters(double v) : value_(v) {}   // print(3.5) will not compile; print(Meters{3.5}) will
    double value() const { return value_; }
private:
    double value_;
};

class Connection {                              // rule of five: owns an fd
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection() { reset(); }
    Connection(const Connection&) = delete;             // copying = double close: make it impossible
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Connection& operator=(Connection&& o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
private:
    void reset() noexcept { if (fd_ != -1) { /* ::close(fd_) */ fd_ = -1; } }
    int fd_;
};

struct Point { int x = 0; int y = 0; };         // rule of zero: nothing to write
```

## 4. Under the hood (senior-level)

**A constructor is an ordinary function with a hidden `this` parameter.** `Account a("X", 5);`
compiles to: reserve `sizeof(Account)` bytes on the stack, then `call Account::Account(&a, "X", 5)`.
There is no allocation unless a member allocates. The destructor is called by the compiler at the
closing brace: it emits `call Account::~Account(&a)` on *every* exit path, including the hidden paths
created by exceptions (landing pads in the `.gcc_except_table` section, see 03_raii_the_core_idea.md).

**Members are constructed by the constructor itself.** The member initialiser list is compiled into
the constructor's prologue: constructor calls for each member in declaration order, then the body.
If a member's constructor throws, the members already constructed are destroyed in reverse order and
the exception continues; the containing object's destructor does **not** run because the object never
finished being born. This is why a constructor that acquired two resources manually (two raw `fopen`
calls) leaks the first when the second fails, and why wrapping each resource in its own object
(rule of zero on the outside) fixes it.

**Trivial types.** If a class has no user-provided constructors/destructor, no virtual functions,
and all members are trivial, the compiler-generated special members are *trivial*: copy is a
`memcpy`, destruction is a no-op, and `std::vector<T>` can `memmove` elements when it grows.
`std::is_trivially_copyable_v<T>` tells you. Adding an empty user-written destructor `~T() {}`
makes the type non-trivial and silently disables those optimisations; `~T() = default;` does not.

**Static local initialisation is thread-safe.** `static Logger& get() { static Logger l; return l; }`
compiles to a guard variable plus `__cxa_guard_acquire`/`__cxa_guard_release`; the first thread
constructs, others block. This is the modern singleton. Static destruction order across translation
units is unspecified, which is the "static initialisation order fiasco": do not let one global's
constructor use another global from a different file.

**`sizeof` an object.** It is the sum of members plus padding (module 12) plus 8 bytes for a vptr if
the class has any virtual function (module 09). Member functions, constructors and destructors add
nothing to the object: they are code, not data. An empty class has `sizeof == 1` so that two
objects always have distinct addresses.

## 5. In production

- **The leak you keep fixing in C** (a function that `return`s early and forgets `free`) is
  structurally impossible for a member of a class with a correct destructor. Module 03 turns this
  into a design principle.
- **Two-phase init is a smell.** A class with `init()` that must be called after the constructor
  invites the bug of using it before `init()`. Do the acquisition in the constructor; if it can fail,
  throw or use a factory function returning `std::optional<T>` (module 10).
- **Never call virtual functions from a constructor or destructor** expecting the derived override:
  during `Base::Base()` the object *is* a `Base`, the vptr points to `Base`'s table. The call goes to
  the base version (or `std::terminate` on a pure virtual).
- **Static objects in a shared library** run their constructors at `dlopen` time and destructors at
  `dlclose` / process exit. A banking server that `exit()`s from a worker thread while another thread
  still uses a static object will crash in the destructor. Prefer `std::quick_exit` or structured
  shutdown.
- **`-Wreorder`, `-Wuninitialized`, `-Weffc++`** catch init-order and missing-init bugs at compile
  time. The first two are in `-Wall`.
- **3 AM debugging.** A crash dump showing a destructor called twice on the same address means a
  class with a raw pointer member was copied (rule of three violated). Grep for classes that define a
  destructor but not a copy constructor; `-Wdeprecated-copy-dtor` (not in `-Wall`) finds them.

## 6. Try it

1. **(easy)** Compile and run `m02_ctor_dtor_order.cpp`. Before running, write the expected output
   on paper. Done when your prediction matches line for line.
2. **(easy)** Add a third `Tracer c` to `Derived` and initialise it in the initialiser list *before*
   `a`. Observe the `-Wreorder` error, then fix it. Done when you can state the rule in one sentence.
3. **(medium)** Write `class Temperature` with an `explicit` constructor from `double` in Celsius and
   a `static Temperature from_fahrenheit(double)` factory. Show that `Temperature t = 36.6;` fails
   and `Temperature t{36.6};` works. Done when both facts are demonstrated by a compile error and a run.
4. **(medium)** Write a class holding two `Tracer` members where the second member's constructor
   throws (add a `bool fail` parameter). Observe which destructors run. Done when you can explain
   why the outer object's destructor is *not* called.
5. **(hard)** Implement a `Singleton` using a function-local static. Compile with
   `g++ -S -O2 -std=c++17` and find the `__cxa_guard_acquire` call in the assembly. Done when you can
   point at the guard variable and explain why a second call skips the lock.

## 7. Interview questions

**Q: In what order are base classes, members and the constructor body executed?**
Bases first (in declaration order), then members in declaration order, then the body. Destruction
is the exact reverse. The initialiser-list order has no effect, which is why `-Wreorder` exists.

**Q: Why use a member initialiser list instead of assigning in the constructor body?**
Initialisation constructs the member directly with the right value; assignment default-constructs
first and then assigns, doing two operations. `const` members, references and types without a default
constructor can *only* be initialised in the list.

**Q: What does `explicit` prevent?**
Implicit conversions through that constructor: `T t = 5;`, `f(5)` where `f(T)`, and `return 5;` from
a function returning `T`. Direct initialisation `T t(5)` and `T{5}` still work.

**Q: What is the rule of zero?**
If a class owns no raw resource, declare none of the special member functions: the compiler-generated
copy, move and destructor are correct. Put resource management in small dedicated wrapper types and
compose.

**Q: What happens if a constructor throws halfway through?**
Members already constructed are destroyed in reverse order, the exception propagates, and the
object's destructor does not run because the object never existed. Any resource acquired manually
in the body before the throw leaks unless it is held by a member with its own destructor.

**Q: Why does adding `~T() {}` slow down `std::vector<T>`?**
A user-provided destructor makes `T` non-trivially-destructible and (for the implicit copy) prevents
the vector from treating elements as raw bytes. Element relocation becomes a loop of copy/move
constructor plus destructor calls instead of one `memmove`. `~T() = default;` keeps the type trivial.

**Q: Is a function-local `static` thread-safe to initialise?**
Yes since C++11. The compiler emits a guard and uses `__cxa_guard_acquire`; concurrent first callers
block until the one constructing finishes. It is the standard way to write a lazy singleton.

## 8. Recap

- Constructors and destructors run exactly once per object, on every exit path; this is the
  foundation of RAII.
- Order: bases, then members in declaration order, then body; destruction reverses it.
- Use member initialiser lists; `const` and reference members require them.
- Six special members; the compiler generates memberwise versions.
- Rule of zero by default; rule of five only in thin resource wrappers.
- `explicit` on single-argument constructors; `= default` to restore; `= delete` to forbid.
- A user-provided destructor makes a type non-trivial; prefer `= default`.
- Function-local statics are thread-safe lazy singletons; cross-TU global init order is unspecified.
- An object that throws from its constructor never gets a destructor call.
