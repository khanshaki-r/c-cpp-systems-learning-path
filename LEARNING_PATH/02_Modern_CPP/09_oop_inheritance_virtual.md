# 9. OOP: inheritance, virtual functions and what a vtable really is

**Time:** ~3 hours   **Prerequisites:** 02, 05

## 1. In plain words

In your C server you probably have a struct of function pointers somewhere: `struct handler_ops {
int (*open)(void*); int (*read)(void*, ...); void (*close)(void*); }`, and every "driver" fills in
its own functions. That *is* object-oriented programming, done by hand. C++ `virtual` functions are
the same idea with the compiler maintaining the table of function pointers for you, guaranteeing it is
filled in correctly, and giving you a syntax (`obj->read()`) that hides the indirection.

Inheritance lets one class extend another. Virtual functions let the caller work with the base type
while the derived type's code runs. That combination is **runtime polymorphism**: one interface, many
implementations, chosen when the program runs. The cost is one pointer per object and one indirect
call per virtual invocation, and the benefit is that new implementations can be added without
touching the callers.

Analogy: a power socket (the base-class interface). Any appliance (derived class) that fits the
socket works, and the wall does not care which appliance it is.

## 2. The real thing

### 2.1 Inheritance basics

```cpp
class Shape {                       // base
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;          // pure virtual: no body here, Shape is abstract
    virtual const char* name() const { return "shape"; }   // virtual with a default
    int id = 0;
};
class Circle final : public Shape { // derived; 'final' forbids further derivation
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159 * r_ * r_; }   // override: compiler checks it really overrides
    const char* name() const override { return "circle"; }
private:
    double r_;
};
```

- `public` inheritance means "is-a": a `Circle` can be used wherever a `Shape&` or `Shape*` is expected.
- A **pure virtual** function (`= 0`) makes the class abstract: it cannot be instantiated, only derived
  from. A class with only pure virtuals and a virtual destructor is an **interface**.
- `override` turns a silent mistake (wrong signature, so you *hid* instead of overrode) into a compile
  error. Always write it.
- `final` on a class or function stops further overriding and lets the compiler devirtualise calls.

### 2.2 Dispatch: static vs dynamic

```cpp
Circle c(1.0);
Shape& s = c;
s.area();         // dynamic dispatch: Circle::area, chosen at run time via vtable
s.Shape::name();  // qualified call: static, skips the vtable
c.area();         // the compiler knows c is a Circle (final): direct call, inlinable
```

The static type is what the compiler sees (`Shape&`); the dynamic type is what the object actually is
(`Circle`). Non-virtual functions dispatch on the static type; virtual functions on the dynamic type.
Dispatch only happens through a reference or pointer; a `Shape` object *by value* is just a `Shape`.

### 2.3 Virtual destructors

```cpp
Shape* p = new Circle(1.0);
delete p;      // calls ~Circle only if ~Shape is virtual. Otherwise: undefined behaviour (usually a leak of r_ ... and worse)
```

Rule: a class with any virtual function needs a virtual destructor (or a protected non-virtual one if
deletion through the base is forbidden). `std::unique_ptr<Shape>` and `std::shared_ptr<Shape>` call
`delete` on a `Shape*`, so the same rule applies when they hold derived objects (`shared_ptr` is the
exception: it captures the deleter at construction, so it works even without a virtual destructor,
but do not rely on that).

### 2.4 Object slicing

```cpp
void f(Shape s);     // takes by VALUE (does not even compile for an abstract Shape)
void g(Animal a);    // compiles if Animal is concrete
g(dog);              // copies ONLY the Animal part: the Dog's data is cut off, and a.sound() is Animal's
```

Passing or assigning a derived object *by value* into a base type copies only the base subobject.
The copy's vptr is the base's. Always pass polymorphic objects by reference or pointer, and consider
deleting the copy operations of base classes to make slicing impossible.

### 2.5 Composition over inheritance

Inheritance is the tightest coupling in the language: the derived class depends on the base's
implementation details and cannot be changed independently. Prefer **composition**: `Server` *has a*
`std::unique_ptr<Logger>` injected from outside, rather than *is a* `ConsoleLogger`. Inherit only from
abstract interfaces, and only when you need runtime substitution.

The **Non-Virtual Interface** (NVI) idiom: public non-virtual `run()` enforces the protocol (lock,
log, time) and calls a private virtual `do_run()` that derived classes implement. Callers cannot bypass
the protocol; implementers cannot break it.

### 2.6 When NOT to use virtual

- The set of types is closed and known at compile time: use templates (module 08) or `std::variant`
  + `std::visit` (module 10). Zero indirection, inlinable.
- The call is on a hot inner loop with a tiny body (`get_x()`): the indirect call and lost inlining
  cost more than the work. Hoist the dispatch out of the loop ("one virtual call per batch").
- Value types (`Point`, `Money`, `Timestamp`): no virtual functions, no inheritance. They should be
  trivially copyable.
- You are tempted to inherit for code reuse only: a free function or a member object is simpler.

### 2.7 Multiple inheritance and RTTI (briefly)

Multiple inheritance is used mainly to implement several interfaces. Avoid inheriting from more than
one class with data. `dynamic_cast<Derived*>(base_ptr)` checks the dynamic type at run time (null if
wrong) using RTTI; `typeid(*p)` names it. Frequent `dynamic_cast` means the design is missing a virtual
function. `-fno-rtti` removes both and shrinks binaries; `dynamic_cast` then fails to compile.

## 3. Code

`code/m09_vtable_demo.cpp` makes the hidden machinery visible:

```cpp
struct Plain { int a; int b; };                 // sizeof 8: no vptr

class Shape { public: virtual ~Shape() = default; virtual double area() const = 0;
              virtual const char* name() const { return "shape"; } int id = 0; };   // sizeof 16: vptr + int + padding

std::uintptr_t read_vptr(const Shape& s) {      // the vptr is the first 8 bytes on the Itanium ABI
    std::uintptr_t v = 0;
    std::memcpy(&v, &s, sizeof v);              // memcpy: no strict-aliasing violation
    return v;
}
// c1 and c2 (both Circle) print the same vptr; sq (Square) prints a different one.

std::vector<std::unique_ptr<Shape>> shapes;     // dynamic dispatch through the base pointer
for (const auto& s : shapes) std::cout << s->name() << " area=" << s->area() << '\n';

Shape& ref = c1;
ref.name();          // "circle" via vtable
ref.Shape::name();   // "shape": qualified call, no vtable

void slice(Animal a);          // by value: prints "..." for a Dog
void no_slice(const Animal& a);// by reference: prints "woof"
```

Run `g++ -std=c++17 -fdump-lang-class m09_vtable_demo.cpp` and open the generated `.class` file:
it prints every class layout and vtable (`Vtable for Circle`, slots with offsets).

`code/m09_interfaces_composition.cpp` shows a pure interface, injection, a template alternative and NVI:

```cpp
class Logger { public: virtual ~Logger() = default; virtual void log(const std::string&) = 0; };

class Server {
public:
    explicit Server(std::unique_ptr<Logger> logger) : logger_(std::move(logger)) {}   // injected, owned
    void handle(const std::string& req) { logger_->log("handled " + req); ++handled_; }
private:
    std::unique_ptr<Logger> logger_;
    int handled_ = 0;
};

template <class Codec> std::string encode_with(const Codec& codec, const std::string& payload) {
    return codec.encode(payload);      // static polymorphism: no vtable, inlinable
}

class Task {                            // NVI
public:
    virtual ~Task() = default;
    void run() { std::cout << "begin\n"; do_run(); std::cout << "end\n"; }
private:
    virtual void do_run() = 0;
};
```

## 4. Under the hood (senior-level)

**The vtable.** For every class with at least one virtual function the compiler emits one read-only
table per class (in `.rodata`, symbol `_ZTV5Shape` = "vtable for Shape"). On the Itanium ABI it looks
like:

```
_ZTV6Circle:
    [-2]  offset-to-top        (0 for the primary base; non-zero for secondary bases in multiple inheritance)
    [-1]  pointer to typeinfo  (_ZTI6Circle, used by dynamic_cast and typeid)
    [ 0]  Circle::~Circle()           (complete object destructor)
    [ 1]  Circle::~Circle()           (deleting destructor: runs ~Circle then operator delete)
    [ 2]  Circle::area()
    [ 3]  Circle::name()
```

Slots are in declaration order of the base's virtual functions; a derived class's new virtuals are
appended. `Square` does not override `name()`, so `_ZTV6Square[3]` holds the address of
`Shape::name`. The two destructor entries exist because `delete p` needs a version that also frees
memory, while a stack object or a base subobject needs one that does not.

**The vptr.** Each polymorphic object starts (on this ABI) with a hidden pointer to its class's
vtable, pointing at slot 0 (so negative indices reach the typeinfo). Hence `sizeof(Shape) == 16`:
8 for the vptr, 4 for `id`, 4 padding. The constructor writes the vptr: `Shape::Shape` stores
`&_ZTV5Shape[2]`, then `Circle::Circle` overwrites it with `&_ZTV6Circle[2]`. That is why a virtual
call *inside* `Shape`'s constructor goes to `Shape`'s version: at that moment the vptr still points
at `Shape`'s table.

**A virtual call compiles to three instructions:**

```asm
mov   rax, [rdi]          ; load vptr from the object (rdi = this)
call  [rax + 16]          ; load slot 2 (area) and call it  (8 bytes per slot)
```

versus one `call` for a non-virtual function. The real cost is not the two loads (the vtable is hot in
L1); it is that the compiler cannot inline across the indirect call, and the branch predictor must
guess the target. Modern CPUs predict a stable target well; a loop alternating between two
implementations mispredicts. **Devirtualisation**: when the compiler can prove the dynamic type
(`final` class, object constructed in the same function, `-fdevirtualize` with LTO, or speculative
devirtualisation guarded by a vptr comparison), it emits a direct call and can inline.

**Why pointer adjustments happen in multiple inheritance.** With `class D : public A, public B`,
a `B*` to a `D` points into the *middle* of the object (at the `B` subobject, which has its own vptr).
Converting `D*` to `B*` adds an offset; a virtual function of `B` overridden in `D` is reached via a
"thunk" that subtracts the offset before jumping to `D`'s code. The `offset-to-top` slot lets
`dynamic_cast<void*>` find the start of the complete object.

**Virtual destructors in the vtable are why `delete base_ptr` works.** `delete p` with a virtual
destructor calls slot 1 (the deleting destructor), which knows the complete object's size and runs
`operator delete` with the right pointer. Without `virtual`, `delete p` calls `Shape::~Shape`
directly and `operator delete(p)` with the size of `Shape`; the derived members are never destroyed and
the allocator may be told the wrong size (undefined behaviour, often silently "works").

**Pure virtual slots** hold `__cxa_pure_virtual`, which prints "pure virtual method called" and
aborts. You see that message when a destructor or constructor calls a pure virtual function, or when
an object is used after its destructor has started.

**RTTI** uses the typeinfo pointer at slot -1: `typeid(*p)` loads it; `dynamic_cast` walks the
typeinfo graph (`__dynamic_cast` in libstdc++), which for deep hierarchies costs tens to hundreds of
nanoseconds, far more than a virtual call.

**Virtual functions and `std::function`/`std::variant`.** All three are runtime dispatch. Virtual:
one pointer per object, open set of types, binary-compatible. `std::variant` + `visit`: closed set
of types known at compile time, no heap, no vptr, a jump table over the index. `std::function`:
dispatch on the callable rather than the object. Pick by whether the type set is open or closed.

## 5. In production

- **Your `handler_ops` struct is a hand-rolled vtable.** The difference in C++ is that the compiler
  checks every slot is filled (a missing override on an abstract class is a compile error), that the
  right destructor runs, and that nobody mutates the table at run time.
- **Interfaces at module boundaries, templates inside.** A pure virtual `Storage` interface lets you
  swap a real Berkeley DB backend for an in-memory fake in tests; inside the backend, use templates and
  values for speed. One virtual call per request is noise next to a syscall.
- **ABI stability.** Adding a virtual function to a class in a shared library changes the vtable
  layout; every client compiled against the old header calls the wrong slot. Append new virtuals at the
  end and only in major versions, or keep the hierarchy out of the public ABI (pimpl).
- **Never call virtuals from constructors/destructors** expecting the override. During construction
  the object's dynamic type is the class whose constructor is running.
- **Object slicing in containers.** `std::vector<Shape>` cannot even hold derived objects (and does not
  compile for an abstract `Shape`); `std::vector<std::unique_ptr<Shape>>` is the pattern. A
  `std::vector<Animal>` filled from `Dog`s compiles and silently slices.
- **3 AM debugging.** "pure virtual method called / terminate called without an active exception":
  a virtual call during construction/destruction, or a use-after-free where the vptr was overwritten by
  the allocator's freelist pointer. In gdb, `p *obj` shows `_vptr.Shape = 0x...`; `info symbol` on that
  value tells you which class's vtable it is (or that it is garbage). A vptr that points into the heap
  rather than `.rodata` means the object is dead.

## 6. Try it

1. **(easy)** Build `m09_vtable_demo.cpp` with `-fdump-lang-class` and read the `Vtable for Circle`
   section. Done when you can name what is in each slot and explain why there are two destructor entries.
2. **(easy)** Remove `virtual` from `~Shape` and compile with `-Wall -Wextra`. Find the
   `-Wdelete-non-virtual-dtor` warning (make `delete` explicit if needed). Done when you can explain
   what would go wrong at run time.
3. **(medium)** Rewrite a C-style `struct ops { int (*open)(void*); ... }` plus `void* ctx` pair as an
   abstract `Device` interface with two implementations. Done when the C++ version has no `void*` and
   the test can inject a fake.
4. **(medium)** Write a tight loop calling `shape->area()` 100M times on a vector containing (a) only
   `Circle`s, (b) alternating `Circle`/`Square`. Time both with `-O2`. Then replace the vector of
   pointers with a `std::variant<Circle, Square>` and `std::visit`. Done when you can report three
   numbers and explain the branch predictor's role.
5. **(hard)** Implement a tiny plugin system: `extern "C" Plugin* create()` in a `.so` returning a class
   derived from an abstract `Plugin`, loaded with `dlopen`/`dlsym`. Done when the main program calls
   virtual functions on an object whose class it never saw at compile time, and you can explain why
   `extern "C"` is needed only for `create`.
6. **(hard)** Using `g++ -O2 -S`, compile a function that calls a virtual function on a `final` class
   object and on a non-final one. Done when you can show the direct `call` in the first and the
   `mov rax,[rdi]; call [rax+N]` sequence in the second.

## 7. Interview questions

**Q: How are virtual functions implemented?**
Each class with virtual functions has a static table of function pointers (the vtable); each object
holds a hidden pointer to its class's table (the vptr), written by the constructor. A virtual call
loads the vptr, loads the slot for that function, and calls through it. Overriding replaces the slot
in the derived class's table.

**Q: Why must a base class destructor be virtual?**
So that `delete` through a base pointer (including inside `unique_ptr<Base>`) dispatches to the
derived destructor. Without it, only the base destructor runs and derived members are never destroyed;
the behaviour is undefined.

**Q: What is object slicing?**
Copying a derived object into a base-type object (by-value parameter, assignment, container of base)
copies only the base subobject. The derived data and the derived vptr are lost. Pass polymorphic types
by reference or pointer.

**Q: What does `override` do and why use it?**
It makes the compiler verify that the function really overrides a base virtual with the same
signature. Without it, a mismatched signature silently creates a new, unrelated function and the base
version keeps being called.

**Q: What is the runtime cost of a virtual call?**
Two dependent loads and an indirect call, usually a few cycles when predicted. The larger cost is
lost inlining and optimisation across the call, and branch mispredictions when the target alternates.
Devirtualisation (`final`, LTO) removes it where the type can be proven.

**Q: What happens if you call a virtual function in a constructor?**
It dispatches to the version for the class whose constructor is currently running, because the vptr
points to that class's table at that moment. Calling a pure virtual there aborts with "pure virtual
method called".

**Q: When would you prefer `std::variant` to a class hierarchy?**
When the set of alternatives is closed and known at compile time: `variant` stores values inline
(no heap, no vptr), `std::visit` dispatches with a jump table and the compiler checks every case is
handled. Hierarchies win when the set is open (plugins) or when binary compatibility matters.

**Q: What is the NVI idiom?**
A public non-virtual function that implements the fixed protocol (pre/post conditions, locking,
logging) and calls a private virtual hook that derived classes override. It separates "what the
interface promises" from "what the implementation customises".

## 8. Recap

- Virtual = runtime dispatch via a per-class vtable and a per-object vptr; cost is one pointer per
  object and an indirect call.
- Vtable slots: offset-to-top, typeinfo, two destructors, then virtuals in declaration order.
- Always `override`; use `final` to enable devirtualisation; always a virtual destructor on a
  polymorphic base.
- Dispatch happens through references and pointers only; by-value copies slice.
- Virtual calls in constructors/destructors go to the class being constructed/destroyed.
- Inherit from interfaces, compose everything else; NVI keeps the protocol in the base.
- Use templates or `std::variant` for closed type sets and hot loops; virtuals for open sets and
  binary boundaries.
- Adding a virtual function changes the vtable layout: an ABI break for shared libraries.
- "pure virtual method called" and a vptr pointing into the heap are the signatures of lifetime bugs.
