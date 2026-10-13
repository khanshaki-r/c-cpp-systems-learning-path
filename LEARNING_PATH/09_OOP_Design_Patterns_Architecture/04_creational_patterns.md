# 04. Creational Patterns

**Time:** ~2 hours   **Prerequisites:** 01, 02, 03

## 1. In plain words

Creational patterns answer one question: *how does code get an object without knowing, or
caring, exactly which class it is and how it was set up?* In C you solve this with
`thing_create(kind, args...)` functions that return an opaque pointer, with `static` globals
for "the one logger", and with structs you fill in and then validate. Each of those is a
creational pattern with a name.

The reason to name them is the reason to name anything: you can talk about the design in one
word instead of a paragraph, and you can recognise when a codebase is using one badly. The
most important lesson in this module is about the singleton, which is the pattern C and C++
programmers over-use most, and which you should learn to replace with explicit wiring.

## 2. The real thing

### 2.1 Factory function

**What:** a function that returns a new object through an interface, hiding the concrete
type. `std::unique_ptr<Store> make_store(std::string_view kind, ...)`.

**C equivalent:** `store* store_create(const char* kind, ...)` with a `switch` inside, or
`store_ops` tables selected by name.

**When:** the concrete type depends on runtime data (config file, command-line flag, message
header) and the caller should not name it. This is 90% of all factory use. It also centralises
construction logic that is easy to get wrong (two-phase init, validation).

**Cost:** one heap allocation per object (you return by `unique_ptr` because the size is
unknown to the caller). Negligible unless called per request on a hot path.

Two refinements:

- **Registry factory**: a `std::map<std::string, std::function<std::unique_ptr<Store>(...)>>`
  that backends register into at startup. Adding a backend does not edit `make_store` (Open/
  Closed, Module 02). The cost is that the set of backends is invisible to the compiler and
  you lose dead-code elimination.
- **Static factory function on the class**: `static std::optional<Date> Date::parse(std::string_view)`
  for constructors that can fail without throwing, or when you need two constructors with the
  same parameter types (`Temperature::from_celsius(double)` vs `from_kelvin(double)`).

### 2.2 Abstract factory

**What:** an interface whose methods create a *family* of related objects that must be
consistent with each other. `StorageFactory { make_store(); make_locks(); make_wal(); }` with
`FileStorageFactory` and `MemoryStorageFactory`.

**C equivalent:** a struct of function pointers where every slot is a constructor.

**When:** you have 2 or more product types that must come from the same "backend" and you
switch the whole backend at once (test vs production, Berkeley DB vs Postgres). Mixing a
`PgStore` with `BdbLocks` must be impossible; one factory object guarantees it.

**When not:** one product type. Then it is just a factory function with extra ceremony.

### 2.3 Builder

**What:** an object that collects the pieces of a complex value step by step and produces the
finished, validated value at the end. Usually with a fluent interface (each setter returns
`*this`).

```cpp
ServerConfig cfg = ServerConfigBuilder{}
    .bind("127.0.0.1", 7000).workers(8).store("file", "/var/lib/tcpkv/data.db").build();
```

**C equivalent:** fill a `struct config`, then call `config_validate(&cfg)`. The difference:
with a builder, `build()` is the *only* way to obtain a `ServerConfig`, so every
`ServerConfig` in the program is valid by construction. In C, nothing stops a caller from
skipping `config_validate`.

**When:** many optional parameters (more than four or so), defaults that must live in one
place, validation that spans several fields ("if TLS is on, cert path must be set"), or a
value that must be immutable once created.

**When not:** a struct with three fields and no cross-field constraints. Use aggregate
initialisation or designated initialisers (C++20) and skip the builder.

**Idiom notes:** return `Builder&` (not `Builder`) from setters to avoid copying; make
`build()` `const` so a builder can produce several configs; make the product's fields
`const` or give it only getters if immutability matters.

### 2.4 Singleton: why mostly avoid

**What:** a class with exactly one instance, reached through `Class::instance()`.

**C equivalent:** a `static struct logger g_logger;` with `logger_get()`.

The one-instance *property* is often legitimate (one process-wide metrics registry, one
connection to a hardware device). The singleton *pattern* (global access through
`instance()`) is almost always the wrong way to get it, for four reasons:

1. **Hidden dependency.** A function that calls `Logger::instance()` depends on the logger,
   but its signature does not say so. Readers and tests cannot see the coupling.
2. **Untestable in isolation.** Every test shares the same instance. State leaks between
   tests; tests cannot run in parallel; you cannot substitute a fake without a reset hook.
3. **Lifetime at exit.** Function-local statics are destroyed in reverse construction order
   at `exit()`. Another static's destructor (or a still-running thread) that uses the
   singleton after its destruction is a crash in `atexit` handling, which is miserable to
   debug. Many codebases "solve" this by leaking the singleton on purpose.
4. **Initialisation order across translation units** is unspecified for namespace-scope
   statics (the "static initialisation order fiasco"). Function-local statics avoid it but
   introduce point 3.

**What to do instead:** dependency injection (Module 02). Create the single instance in
`main` (or a `wire()` function), pass it by reference to everything that needs it. The
"single" property is now a fact about your wiring, enforced by the fact that nobody else
constructs one, and tests can construct as many as they want.

**When a singleton-like thing is acceptable:**

- A process-wide *initialisation action* (initialise OpenSSL, `PQinitSSL`, set up a signal
  handler): use `std::call_once` with a `std::once_flag`. It is an action, not an object.
- Pure, stateless, trivially destructible helpers (a lookup table). No lifetime issue.
- Deep inside a library where threading an argument through 40 layers is unreasonable and
  the thing is genuinely process-global (an allocator, a logging sink). Even then, provide a
  `set_instance()` hook for tests.

**Thread-safe singleton in C++11 and later:** the Meyers singleton (`static T inst;` inside
`instance()`) is guaranteed thread-safe by the standard. The compiler emits a guard variable;
the first thread takes a lock (`__cxa_guard_acquire`), others wait, subsequent calls are a
single byte load. You do not need double-checked locking in C++; the compiler does it.

### 2.5 Prototype

**What:** create a new object by cloning an existing configured one, via `virtual
std::unique_ptr<Base> clone() const`.

**C equivalent:** `memcpy` a template struct, then fix up pointers (deep copy) by hand.

**When:** construction is expensive (parsing a template, loading a schema) and many similar
instances are needed; or you hold objects through a base pointer and need copies without
knowing the concrete type (copy constructors are not virtual, so you need `clone()`).

**Modern note:** for non-polymorphic types, copying a value *is* the prototype pattern.
`ConnectionParams replica = base; replica.host = "...";` There is nothing to name.

### 2.6 Object pool

Covered in Module 03 Section 2.8 from the ownership side. As a creational pattern: a pool
*creates* objects up front and *recycles* them, which the caller sees as "acquire gives me a
ready object". Use for expensive-to-construct objects with a warm-up cost (database
connections, large buffers, threads). The `unique_ptr` custom deleter is the C++ trick that
makes the return automatic.

### 2.7 Choosing

| Need | Use |
|---|---|
| Hide concrete type chosen at runtime | Factory function |
| Backends register themselves | Registry factory |
| Constructor that can fail, or two constructors with the same parameter types | Static named factory function returning `optional`/`expected` |
| Several products that must match | Abstract factory |
| Many optional fields, cross-field validation, immutable result | Builder |
| Exactly one instance | Construct once in `main`, inject by reference |
| One-time initialisation action | `std::call_once` |
| Copy through base pointer | Prototype (`clone()`) |
| Expensive objects, bounded count | Object pool |

## 3. Code

### `code/m04_factory_builder.cpp`

Factory function, registry factory, abstract factory and builder on a storage backend
example.

```cpp
std::unique_ptr<Store> make_store(std::string_view kind, std::string_view arg) {
    if (kind == "memory") return std::make_unique<MemoryStore>();
    if (kind == "file")   return std::make_unique<FileStore>(std::string(arg));
    throw std::invalid_argument("unknown store kind: " + std::string(kind));
}
```

```cpp
class ServerConfigBuilder {
public:
    ServerConfigBuilder& bind(std::string addr, std::uint16_t port);   // returns *this
    ServerConfigBuilder& workers(int n);
    ServerConfig build() const;         // the ONLY way to get a ServerConfig; validates
private:
    ServerConfig cfg_{"0.0.0.0", 0, 4, std::chrono::milliseconds(30000), 1024, "", ""};  // defaults
};
```

### `code/m04_singleton_vs_di.cpp`

A Meyers singleton `MetricsRegistry`, a `std::call_once` initialiser for a legacy C library,
and the DI alternative: a plain `Metrics` class constructed once in `main` and passed by
reference. The test section shows the isolation you get from DI that the singleton cannot
give.

```cpp
static MetricsRegistry& instance() {
    static MetricsRegistry inst;   // thread-safe init guaranteed by C++11
    return inst;
}
```

```cpp
std::call_once(flag_, [] { /* init legacy C library exactly once */ });
```

### `code/m04_prototype_pool.cpp`

`Iso8583Message::clone()` as the classic prototype, a value-type copy as the modern form,
and a `ConnectionPool` with a `Lease` type that returns connections automatically.

## 4. Under the hood (senior-level)

**Factory functions and allocation.** Returning `std::unique_ptr<Base>` forces a heap
allocation because the caller does not know the derived size. If that matters (hot path),
alternatives are: return a `std::variant<A, B>` by value (fixed set, no heap), use a small
buffer optimisation type-erasure wrapper (Module 08), or construct in-place into
caller-provided storage. Most server code should not care: a request that touches a database
costs 10,000 times more than one `malloc`.

**`std::function` in a registry.** Each `std::function` is typically 32 bytes on GCC
(libstdc++), holds small callables inline (up to 16 bytes) and heap-allocates larger ones.
Invocation is an indirect call through a stored pointer, same cost class as a virtual call.
A capture-less lambda stored in it fits inline.

**How the Meyers singleton is compiled.** GCC emits a 64-bit guard variable next to the
static. The fast path is `if ((guard & 1) == 0) { slow path }`, one load and one branch,
which is cheaper than a mutex. The slow path calls `__cxa_guard_acquire`, constructs,
registers the destructor with `__cxa_atexit`, then `__cxa_guard_release`. If the
constructor throws, the guard is reset and the next caller retries. `__cxa_atexit` is why
destruction order at exit is reverse construction order, across all translation units.

**`std::call_once` implementation.** libstdc++ builds it on `pthread_once` (older) or on a
futex-based state machine (GCC 11 and later). Semantics: if the callable throws, the flag is
not set and another caller may retry. Exactly-once applies to successful completion.

**Builders and copies.** A fluent setter that returns `Builder` by value copies the whole
builder at every step: for a builder holding a few strings that is several allocations per
call. Return `Builder&`. If you want rvalue chaining on a temporary builder to move the
contents into `build()`, you can add `&&`-qualified overloads; that is an optimisation to
apply when measured, not by default.

**Prototype and slicing.** `Base b = derived;` copies only the `Base` subobject and the
vptr becomes `Base`'s. This is "slicing" and it is why copying polymorphic objects needs a
virtual `clone()`. A common defence: make the base class's copy constructor `protected` so
only derived classes (in their `clone()`) can invoke it.

## 5. In production

**Config from the environment.** A server reads a file, environment variables and command
line, in that priority. A builder is the natural shape: three "sources" each call setters,
`build()` validates once. The alternative, a struct that every source mutates and a
`validate()` somebody calls at the end, is how servers end up starting with port 0.

**The singleton that bit everyone.** A logger singleton with a background flushing thread.
At `exit()`, the static logger is destroyed while the thread is still running; the thread
touches the destroyed mutex; the process crashes *after* `main` returns, with a stack trace
inside `exit`. Fix: construct the logger in `main`, join its thread in its destructor, which
now runs before `main` returns. This is also why "declare the thread pool last" from Module
03 matters.

**Factories and plug-ins.** If backends live in shared libraries (`dlopen`), each library's
initialiser registers its creator into the registry factory. The core never names a backend.
This is the real reason to prefer a registry over a `switch`: the switch cannot name a type
that does not exist yet at compile time.

**Pools and connection health.** A pooled database connection can die while idle (server
restart, idle timeout at a firewall). A production pool validates on acquire (cheap ping or
`PQstatus`) or on error, and replaces dead connections. Without this, the first request after
a network blip fails with a stale connection, and the second succeeds, which looks like a
flaky bug for weeks.

## 6. Try it

1. **(easy)** Add a `"cached"` kind to `make_store` that wraps a `MemoryStore` in the
   `WriteThroughCache` from Module 02. Done when `make_store("cached", "")->name()` reports
   it is a cache over memory.
2. **(easy)** Give `ServerConfigBuilder` a cross-field rule: if `store_kind == "file"`,
   `store_arg` must be non-empty. Done when the builder rejects `store("file")` with a clear
   message.
3. **(medium)** Replace the `MetricsRegistry` singleton with DI in a small program that has
   three "modules" (parser, handler, store) all incrementing metrics. Then write a test that
   runs two scenarios in sequence and asserts counts, and show that the singleton version
   fails the second assertion (state leaks) while the DI version passes. Done when both
   programs compile and the behaviour difference is demonstrated.
4. **(medium)** Add health-checking to `ConnectionPool`: a `bool DbConnection::alive()` that
   a test can flip to false, and an `acquire()` that discards dead connections and constructs
   replacements. Done when a test shows a dead connection replaced transparently.
5. **(hard)** Implement a registry factory where backends self-register via a static
   initialiser object in their own translation unit (`static Registrar r{"file", &make_file};`).
   Explain what happens if that translation unit is in a static library and nothing
   references it (hint: the linker drops it). Done when you can state the fix
   (`--whole-archive`, or explicit registration) and why a plain `switch` does not have this
   problem.

## 7. Interview questions

**Q: What is the difference between a factory function and an abstract factory?**
A: A factory function creates one product of a type chosen at runtime. An abstract factory
is an interface that creates a family of related products, guaranteeing they come from the
same backend. Use the abstract factory only when there are several products that must match.

**Q: Why is the singleton pattern discouraged, and what replaces it?**
A: It hides dependencies, makes tests share state, and has destruction-order problems at
exit. Replace it with one instance created in `main` and injected by reference; for one-time
initialisation actions use `std::call_once`.

**Q: Is a function-local static thread-safe in C++?**
A: Yes, since C++11. The compiler emits a guard and ensures exactly one thread constructs
it while others wait. You do not need double-checked locking.

**Q: When do you reach for a builder?**
A: When a value has many optional fields, defaults that should live in one place, or
validation across fields, and when you want every instance to be valid by construction
because `build()` is the only way to make one.

**Q: Why does the prototype pattern need a virtual `clone()`?**
A: Copy constructors are not virtual; copying through a base pointer slices the object to
the base type. `clone()` dispatches to the derived class, which copies itself fully and
returns a base pointer.

**Q: What is the cost of returning `unique_ptr<Base>` from a factory and when does it matter?**
A: One heap allocation per object because the caller does not know the derived size. It
matters on hot paths creating many small objects; alternatives are returning a
`std::variant` by value for a closed set or a small-buffer type-erasure wrapper.

## 8. Recap

- Factory function: hide the concrete type chosen at runtime; the most used creational pattern.
- Registry factory: open for extension, but the compiler loses sight of the set.
- Abstract factory: a family of products that must match; skip it for one product.
- Builder: validated, immutable values with many optional fields; `build()` is the only exit.
- Singleton: the one-instance property is fine, the global `instance()` is not. Construct in
  `main`, inject by reference. `std::call_once` for one-time actions.
- Meyers singleton is thread-safe by the standard; its problem is exit-time destruction order.
- Prototype: `clone()` for polymorphic copies; plain copy for value types.
- Object pool: bounded, recycled, returned automatically via a custom deleter.
