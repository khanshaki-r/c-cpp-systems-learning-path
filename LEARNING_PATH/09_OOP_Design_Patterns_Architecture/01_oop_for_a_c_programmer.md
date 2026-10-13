# 01. OOP for a C Programmer

**Time:** ~2.5 hours   **Prerequisites:** none (C fluency; classes and references from 02_Modern_CPP)

## 1. In plain words

You already do object-oriented programming. Every time you write a `struct` that holds state,
a set of functions that take a pointer to that struct as the first argument, and a table of
function pointers so that callers can swap one implementation for another, you are writing
objects, methods and interfaces. C just does not give you a name or a compiler check for any
of it.

C++ takes those three habits and makes the compiler enforce them. A `class` is a `struct`
whose fields you can hide. A member function is a function whose first argument (`this`) is
passed for you. A `virtual` function is a slot in a function-pointer table that the compiler
builds, fills and calls on your behalf, with type checking at every step. That is the whole
mechanism. The rest of OOP is a set of design habits about *when* to use it.

The analogy that works for most C programmers: a class is a `.c` file with a `static` state
struct and a public `.h`. The `.h` is the public section, the `.c` internals are private, and
the compiler now refuses to let other files reach inside. The difference is that you can have
many instances of that `.c` file alive at once, each with its own state.

## 2. The real thing

### 2.1 From `struct` + functions to `class`

```c
/* C */
struct ring_buffer { char* buf; size_t cap, head, tail; };
int  rb_init(struct ring_buffer* rb, size_t cap);
int  rb_push(struct ring_buffer* rb, char c);
int  rb_pop(struct ring_buffer* rb, char* out);
void rb_destroy(struct ring_buffer* rb);
```

```cpp
// C++
class RingBuffer {
public:
    explicit RingBuffer(std::size_t cap);     // rb_init, but cannot be forgotten
    ~RingBuffer();                            // rb_destroy, called automatically
    bool push(char c);                        // rb_push; 'this' is the hidden first argument
    std::optional<char> pop();                // rb_pop; return value instead of out-parameter
private:
    char* buf_; std::size_t cap_, head_, tail_;
};
```

Three concrete gains:

1. **Construction and destruction cannot be skipped.** `rb_init` is a convention; a
   constructor is a rule. There is no window where a `RingBuffer` exists but is not
   initialised, and no path (including exceptions and early returns) where the destructor is
   not run. This is RAII: Resource Acquisition Is Initialisation. It is the single biggest
   reason C++ code has fewer leaks than C code of the same size.
2. **The invariant has a boundary.** "`head_ < cap_` and `tail_ < cap_`" is enforced by the
   handful of member functions. In C, any function in any file can write `rb->head = 99`.
3. **`const` is enforced for the whole object.** `std::optional<char> peek() const` promises
   not to mutate; the compiler checks the promise.

### 2.2 From function-pointer tables to virtual functions

In your RPC server you probably have something like:

```c
struct handler_ops {
    int  (*parse)(void* ctx, const char* buf, size_t len);
    int  (*execute)(void* ctx);
    void (*free)(void* ctx);
};
struct handler { const struct handler_ops* ops; void* ctx; };
```

That is a vtable and a vptr, hand-made. `ops` is the vtable: one shared table per
implementation. `ctx` is the object. Every implementation fills its own `handler_ops`.

In C++:

```cpp
class Handler {
public:
    virtual ~Handler() = default;
    virtual bool parse(std::string_view bytes) = 0;    // "= 0": pure virtual, must be overridden
    virtual int  execute() = 0;
};
class BalanceEnquiry : public Handler { /* overrides */ };
```

The compiler emits one `vtable` per class that has virtual functions and plants a hidden
`vptr` as the first member of every object. `h->execute()` compiles to roughly:

```
mov  rax, [rdi]        ; load vptr from object
call [rax + 16]        ; call slot 2 (after dtor slots) -- indirect call
```

Exactly what `h->ops->execute(h->ctx)` compiles to in C. The difference: you cannot forget to
fill a slot, cannot put the wrong signature in a slot, cannot pass the wrong `ctx`, and
`delete h` runs the right destructor.

Vocabulary you need to say correctly in interviews:

- **Polymorphism**: calling `execute()` on a `Handler&` and getting the derived behaviour.
  Runtime (dynamic) polymorphism uses virtual functions. Compile-time (static) polymorphism
  uses templates (Section 2.5 and Module 08).
- **Override** vs **overload**: `override` replaces a virtual function in a derived class;
  overloading is several functions with the same name and different parameters. Always write
  the `override` keyword; it turns a silent signature mismatch into a compile error.
- **Abstract class**: has at least one pure virtual function; cannot be instantiated.
- **Interface**: an abstract class with only pure virtual functions and a virtual destructor.
  C++ has no `interface` keyword; it is a convention.
- **`final`**: on a class, forbids deriving; on a function, forbids overriding further. Lets
  the compiler devirtualise calls when it knows the exact type.

### 2.3 Encapsulation and invariants

An **invariant** is a statement about an object's state that is true between operations.
"Balance equals the sum of the ledger." "`size_ <= capacity_`." "`fd_` is either -1 or an
open socket."

Encapsulation is not about hiding data for secrecy. It is about shrinking the amount of code
that can break an invariant to the member functions of one class, which you can read in one
sitting. When a `RingBuffer` is corrupt, you have 6 functions to read, not 600.

Rules of thumb:

- Make every data member private unless the type is a plain aggregate (a `struct Point {
  double x, y; }` with no invariant should stay a public struct).
- A getter and setter pair for every field is not encapsulation; it is a struct with extra
  steps. Expose *operations* (`withdraw`, `deposit`), not fields.
- `const` member functions are part of the interface contract. Mark everything that does not
  mutate.

### 2.4 What OOP buys you and what it costs

| Feature | Buys | Costs |
|---|---|---|
| Constructors / destructors (RAII) | No leaks, no uninitialised objects, exception safety | Almost nothing. Use everywhere. |
| Private members | Invariants are enforceable, code is locally reasonable | Slightly more typing. Use everywhere there is an invariant. |
| Virtual functions | Swap implementations at runtime; plug-in points; test doubles | 8 bytes per object (vptr), one indirect call (typically 1 to 5 ns when predicted, 20+ ns on a mispredict), no inlining across the call, heap allocation usually needed to hold "some derived type" |
| Deep inheritance hierarchies | Code reuse through base classes | Fragile base class problem: a change in the base silently changes every derived class. Hard to test. Prefer composition. |
| Everything-is-an-object style (Java-like) | Familiar to Java/C# developers | Heap churn, pointer chasing, poor cache behaviour, verbosity. Not idiomatic C++. |

The honest summary a senior engineer gives: **RAII and encapsulation are almost free and
almost always right. Virtual dispatch is a runtime decision mechanism; use it where you need
a decision at runtime. Inheritance for code reuse is usually a mistake; use composition.**

### 2.5 When procedural or generic code is better

Three cases where you should not reach for a class hierarchy:

1. **Pure computation over plain data.** Parsing an ISO 8583 message, computing an EMI,
   hashing a key. A free function over a struct is simpler, testable and inlinable. C++ is a
   multi-paradigm language; "free functions are first-class design tools" is an idiom.
2. **The set of types is fixed and known at compile time.** If a request is one of five
   message types and that list changes once a year, `std::variant<A, B, C, D, E>` with
   `std::visit` gives you exhaustiveness checking (the compiler tells you when you forgot a
   case) and no heap allocation. Module 06 and Module 08 cover this.
3. **Hot loops where the operation is known at the call site.** `std::sort(v.begin(),
   v.end(), cmp)` with a lambda inlines the comparator; a `virtual bool less(a, b)` would not.
   Templates win when performance matters and the type is known when you compile.

The decision question: **"Does the choice need to be made at runtime, by configuration or by
the client, and is the set of choices open-ended?"** Yes to both: virtual interface. No to
either: consider templates, `std::variant` or plain functions first.

## 3. Code

### `code/m01_struct_fnptr_vs_virtual.cpp`

The same `Store` abstraction three ways: C struct of function pointers with `void*` state,
C++ virtual interface, and template. Key parts:

```cpp
// C style: the table and the object are separate things you wire by hand.
struct store_ops {
    int (*put)(void* self, const char* key, const char* value);
    const char* (*get)(void* self, const char* key);
    void (*destroy)(void* self);
};
struct store { const store_ops* ops; void* state; };
```

```cpp
// C++: the compiler builds the table, plants the pointer, checks every slot.
class Store {
public:
    virtual ~Store() = default;
    virtual bool put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
};
class MapStore final : public Store { /* overrides */ };
```

```cpp
// Template: the "interface" is whatever exercise<T> calls. No table, no indirect call.
template <class StoreT>
void exercise(StoreT& s) { s.put("acct:1001", "balance=500"); /* ... */ }
```

Run it and compare `sizeof` the two `MapStore` classes: the virtual one is 8 bytes larger
on x86-64. That is the vptr.

### `code/m01_invariant_class.cpp`

An `Account` whose balance can never go negative and whose ledger always sums to the balance,
an `AccountRepository` interface, and a `transfer()` function written against the interface.
Note the pattern: the business function depends on an abstract repository, so it can be unit
tested with `InMemoryAccountRepository` and deployed with a database-backed one.

```cpp
bool transfer(AccountRepository& repo, const std::string& from, const std::string& to,
              Account::Paise amount) {
    Account* src = repo.find(from);     // observer pointer: the repo still owns the object
    Account* dst = repo.find(to);
    if (!src || !dst) return false;
    if (!src->withdraw(amount)) return false;   // invariant protected inside Account
    dst->deposit(amount);
    repo.save(*src); repo.save(*dst);
    return true;
}
```

One deliberate weakness to notice: `find()` returns a pointer into a `std::vector`, and
`save()` could `push_back`. If `save` of a new account ran between `find` and use, the pointer
would dangle. Module 03 is about designing so that this cannot happen.

## 4. Under the hood (senior-level)

### Object layout

```
class Base { virtual void f(); int a; };
class Derived : public Base { int b; void f() override; };

Derived object (x86-64, GCC):            vtable for Derived (read-only data segment):
+0   vptr  --------------------------->  [ offset_to_top = 0      ]
+8   a                                   [ typeinfo* for Derived  ]
+12  b                                   [ &Derived::~Derived (complete) ]
+16  (end; sizeof == 16)                 [ &Derived::~Derived (deleting) ]
                                         [ &Derived::f            ]   <- vptr points here
```

The vptr points *into* the table at the first function slot; the two words before it hold the
offset-to-top (used for multiple inheritance casts) and the RTTI pointer (used by
`dynamic_cast` and `typeid`). This is the Itanium C++ ABI, which GCC, Clang and most Unix
compilers follow. MSVC's layout differs in details but has the same shape.

A pure virtual slot holds the address of `__cxa_pure_virtual`, which aborts. That is the
"pure virtual function called" crash you can get if a virtual function is invoked from a base
class constructor or destructor while the object is still only partly constructed.

### Cost of a virtual call, precisely

1. Load `vptr` from the object (1 memory read, usually L1 hit if the object is hot).
2. Load the function address from the vtable slot (1 memory read, vtable is almost always in
   cache since it is shared by all instances).
3. Indirect `call`. Modern CPUs predict indirect branch targets well if the same call site
   keeps hitting the same target; a monomorphic call site is nearly free. A call site that
   alternates between many targets mispredicts, which costs a pipeline flush (typically
   15 to 25 cycles on current x86 cores).
4. **Lost optimisation** is the real cost: the compiler cannot inline the callee, so it cannot
   hoist loads, vectorise, or constant-fold across the call. A `virtual bool less(int, int)`
   inside `std::sort` can make the sort 3 to 10 times slower than a lambda; that is the
   inlining difference, not the indirect call itself.

GCC can **devirtualise** when it proves the dynamic type: a `final` class, a local object of
known type, or whole-program analysis under `-O2 -flto`. Speculative devirtualisation adds
`if (vptr == &Derived::vtable) inline_body(); else indirect_call();`.

### Why virtual destructors matter

`delete base_ptr` on a class without a virtual destructor is undefined behaviour if the
dynamic type differs. In practice the derived destructor is skipped (leaks) and `operator
delete` may be called with the wrong size (heap corruption on sized deallocation). Rule: any
class with a virtual function gets a virtual destructor. GCC warns under `-Wall`
(`-Wdelete-non-virtual-dtor`).

### Construction order

Base constructors run first, then members in declaration order (not initialiser-list order;
GCC warns with `-Wreorder`), then the constructor body. Destruction is the exact reverse. The
vptr is set to the Base vtable while the Base constructor runs and switched to Derived when
the Derived constructor starts. That is why a virtual call inside a constructor dispatches to
the class being constructed, not the final type.

### What a member function really is

`bool Account::withdraw(Paise p)` is compiled as a free function `withdraw(Account* this,
Paise p)`. Member access `balance_` is `this->balance_`. There is no runtime cost to a
non-virtual member function compared to a C function taking a struct pointer. The mangled
name encodes the class and parameter types, which is how overloading works at link time.

## 5. In production

**Where OOP earns its keep in your server.** The places where behaviour genuinely varies at
runtime: which storage backend (Berkeley DB vs YugabyteDB), which transport (TCP vs a test
harness in memory), which authentication scheme, which message codec. An interface at each
of those seams is what lets you unit test the business logic without a database, and swap
the database without touching the business logic. Your migration engine almost certainly had
an implicit "store interface" with two implementations; naming it and making it a class
would have let you test the migration logic against a fake.

**Where it hurts.** Teams coming from Java build `AbstractBaseRequestHandlerFactoryImpl`
hierarchies five levels deep. Every request becomes six heap allocations and a chain of
virtual calls. On a hot path handling tens of thousands of RPCs per second, that is
measurable. Flat designs win: a `struct Request` holding plain data, free functions that
process it, one interface at the storage boundary.

**War story shape: the partially constructed object.** A connection class calls
`start_reading()` from its constructor, which registers a callback with the event loop. The
event loop fires before the constructor finishes. The callback does a virtual call that lands
in the base class version, which is pure, and the process aborts with "pure virtual method
called". Fix: two-phase start (construct, then `start()`), or a factory function that
constructs fully and then registers.

**3 AM debugging tips.** In a crash dump, a corrupted vptr shows up as a jump to a nonsense
address or into the middle of unrelated code. Causes: use-after-free (the memory was reused),
writing past the end of the previous object in the heap, or a `memcpy` of a polymorphic
object. `-fsanitize=address` catches all three in test. In WinDbg/gdb, `x/4gx obj` and check
whether the first word points into the `.rodata` region of your binary; if not, the object is
garbage.

## 6. Try it

1. **(easy)** Take the C-style `store` in `m01_struct_fnptr_vs_virtual.cpp` and add a third
   function pointer `size`. Note every place you had to touch. Then add `virtual std::size_t
   size() const = 0;` to the C++ `Store` and see what the compiler forces you to do. Done
   when both compile and you can list the differences in failure modes (what happens when
   you forget to fill the slot in each version).
2. **(easy)** Convert a small C module you have written (a ring buffer, a string builder, a
   config parser) into a C++ class with a constructor, destructor and private state. Done
   when no caller can break its invariant without going through a member function, and
   `-Wall -Wextra -Werror` passes.
3. **(medium)** Write a `Shape` hierarchy (`Circle`, `Rect`) with `virtual double area()`,
   then the same with `std::variant<Circle, Rect>` and `std::visit`. Measure `sizeof` of
   each and count heap allocations needed to hold 1000 mixed shapes in a container. Done when
   you can explain which one you would choose for (a) a plug-in system and (b) a fixed set of
   geometry types.
4. **(medium)** In `m01_invariant_class.cpp`, `find()` returns a raw pointer that can dangle if
   `save()` reallocates. Fix it in two different ways: (a) make `save` never invalidate (hint:
   `std::map`, or `std::vector<std::unique_ptr<Account>>`), (b) change the interface so that
   `transfer` never holds two pointers across a call that may reallocate. Done when you can
   explain the trade-off of each.
5. **(hard)** Write a 20-line benchmark: a vector of 1 million objects, summed via (a) a
   virtual `value()` call where the vector holds `unique_ptr<Base>` with two derived types
   alternating, (b) the same with all objects the same derived type, (c) a `std::variant`,
   (d) a template. Run with `-O2`. Done when you can explain the ordering of the results in
   terms of branch prediction, inlining and cache misses (not just "virtual is slow").

## 7. Interview questions

**Q: What does the compiler generate for a virtual function call?**
A: It loads the hidden vptr from the object, loads the function address from the vtable slot
for that function, and performs an indirect call. The vtable is one read-only table per
class; the vptr is one pointer per object. It is the same mechanism as a C struct of function
pointers, generated and type-checked by the compiler.

**Q: Why must a base class with virtual functions have a virtual destructor?**
A: So that `delete` through a base pointer runs the derived destructor and frees the right
size. Without it the behaviour is undefined; in practice derived members leak and sized
deallocation can corrupt the heap.

**Q: What is the real cost of virtual dispatch?**
A: Two dependent loads and an indirect call, which is cheap if predicted. The larger cost is
that the compiler cannot inline across the call, so it loses constant folding, hoisting and
vectorisation. On hot paths with a known type, a template or `std::variant` lets the
optimiser see through.

**Q: When would you choose templates over virtual functions?**
A: When the set of types is known at compile time and the choice does not depend on runtime
input or configuration; when the call is on a hot path; when you need the callee inlined.
Virtual wins when the choice is a runtime decision, when the set of implementations is open
(plug-ins), or when you need to store heterogeneous objects behind one pointer type without
knowing all types in one translation unit.

**Q: What is an invariant and how does encapsulation help?**
A: An invariant is a condition that must hold between operations on an object. Encapsulation
shrinks the set of code that can violate it to the member functions of one class, so you can
verify it by reading one file instead of the whole program.

**Q: Why is "inheritance for code reuse" discouraged?**
A: It creates tight coupling: any change in the base class silently changes every derived
class, and derived classes depend on base implementation details. Composition (holding a
member of the reused type) gives the same reuse with a narrow, explicit interface.

**Q: What happens if you call a virtual function from a constructor?**
A: It dispatches to the version for the class currently being constructed, not the most
derived class, because the vptr is updated as each constructor runs. If that version is pure
virtual, the program aborts with "pure virtual method called".

## 8. Recap

- A class is a struct with an enforced boundary; a member function is a free function with a
  hidden `this`; a virtual function is a slot in a compiler-built function-pointer table.
- RAII (constructor acquires, destructor releases) is the main reason C++ leaks less than C.
  It cannot be skipped by early returns or exceptions.
- Encapsulation protects invariants by limiting who can break them to one class's members.
- Virtual dispatch: 8 bytes per object, two loads and an indirect call, and lost inlining.
  Use it for decisions made at runtime over an open set of implementations.
- Always `virtual ~Base() = default;` in a polymorphic base; always write `override`.
- Prefer composition over inheritance for reuse; inherit for interfaces.
- Plain functions over plain structs, `std::variant` over fixed type sets, and templates for
  hot known-type code are all legitimate alternatives to class hierarchies.
- A vptr pointing outside `.rodata` in a crash dump means the object is already destroyed or
  overwritten.
