# 02. SOLID in Plain Words

**Time:** ~2.5 hours   **Prerequisites:** 01

## 1. In plain words

SOLID is five habits that keep a codebase changeable after it has grown past the point where
one person holds it all in their head. Each one answers a question you have already asked
while maintaining a legacy server:

- **S**ingle Responsibility: "Why does changing the log format break the parser?"
- **O**pen/Closed: "Why do I have to edit the 2000-line `switch` every time we add a message type?"
- **L**iskov Substitution: "Why does this subclass crash when passed to code that worked with the base?"
- **I**nterface Segregation: "Why does my test fake have to implement 14 functions to test one?"
- **D**ependency Inversion: "Why can't I unit test this without a running database?"

They are not laws. They are the names of forces. A senior engineer knows each one, knows the
cost of applying it (more files, more indirection, more concepts), and applies it where
change is expected and skips it where it is not.

## 2. The real thing

### 2.1 S: Single Responsibility Principle

**Definition:** a class (or module) should have one reason to change.

"Reason to change" means a stakeholder or concern: wire format, business rule, storage
schema, logging policy. If the operations team asks for JSON logs and the product team asks
for a new fee rule, those two requests should touch different files.

Bad:

```cpp
class AccountHandler {
public:
    void handle(int fd) {
        char buf[4096]; read(fd, buf, sizeof buf);       // transport
        auto req = parse_iso8583(buf);                   // protocol
        if (req.amount > limits[req.tier]) reject();     // business rule
        PQexec(conn, build_sql(req).c_str());            // storage
        fprintf(logf, "%s %d\n", req.id, req.amount);    // logging
    }
};
```

Every concern in one function. A change to any of them risks all of them, and nothing here
can be tested without a socket and a database.

Good: the layering from Module 09. `Transport` reads bytes. `Codec` turns bytes into a
`Request` struct. `AccountService` applies rules. `AccountRepository` talks to storage. A
`Logger` is injected. Each is 50 to 200 lines and has one job.

The trap: SRP taken literally produces one class per function. The unit of "responsibility"
is a *cohesive* set of operations over shared state. `Account` with `deposit`, `withdraw`,
`balance` is one responsibility (account bookkeeping), not three.

### 2.2 O: Open/Closed Principle

**Definition:** open for extension, closed for modification. Adding a feature should mean
adding code, not editing working code.

Bad:

```cpp
std::string dispatch(const Request& r) {
    switch (r.type) {
        case MsgType::Balance:   return do_balance(r);
        case MsgType::Transfer:  return do_transfer(r);
        case MsgType::Statement: return do_statement(r);
        // every new type: edit here, re-test everything that goes through here
    }
}
```

Good:

```cpp
class Operation { public: virtual ~Operation() = default;
                  virtual std::string apply(Store&, const Request&) = 0; };
std::map<std::string, std::unique_ptr<Operation>> ops;   // register at startup
auto reply = ops.at(r.op)->apply(store, r);              // dispatch never changes
```

A new operation is a new class plus one `register_op` line in the composition root.

The honest counterpoint: a `switch` over an `enum` has one property the registry does not.
With `-Wswitch` (in `-Wall`), the compiler tells you every place you forgot to handle the new
enum value. For a *closed* set of types that rarely changes, the `switch` (or
`std::variant` + `std::visit`, Module 06) is the better design because exhaustiveness is
checked. OCP is for sets you expect to be *open*: plug-ins, customer-specific rules, message
handlers added by other teams.

### 2.3 L: Liskov Substitution Principle

**Definition:** if `D` derives from `B`, any code that works correctly with a `B&` must work
correctly with a `D&`. Subtypes may strengthen what they guarantee and weaken what they
require, never the reverse.

Bad (the classic):

```cpp
class Store { public: virtual void put(key, value) = 0; /* ... */ };
class ReadOnlyStore : public Store {
    void put(key, value) override { throw std::logic_error("read-only"); }  // breaks every caller
};
```

Every function that takes a `Store&` and calls `put` was correct until a `ReadOnlyStore` was
passed. The type system said yes and the runtime said no. Other violations: an override that
ignores a parameter, an override that requires a stricter precondition ("key must be
non-empty" when the base allowed empty), an override that leaves the object in a state the
base's other functions do not expect.

Good: model the difference in the type system. `class ReadableStore { get }` and `class Store
: public ReadableStore { put }`. Code that only reads takes a `ReadableStore&`, and a
read-only backend simply does not implement `Store`.

In C you had the same problem in a different costume: a function-pointer slot set to `NULL`
for "not supported", and every caller having to check. LSP says: do not put the slot in the
table if some implementations cannot fill it.

The `const` angle: a `const` member function in the base is a promise that overrides must
keep. An override that mutates through `const_cast` violates LSP.

### 2.4 I: Interface Segregation Principle

**Definition:** clients should not be forced to depend on functions they do not use. Prefer
several small interfaces to one large one.

Bad:

```cpp
class Connection {
public:
    virtual void send(bytes) = 0;      virtual bytes recv() = 0;
    virtual void set_tls(cert) = 0;    virtual Stats stats() = 0;
    virtual void set_keepalive(int) = 0; virtual void close() = 0;
    // ... 9 more
};
```

Your test fake must implement 15 functions to test a codec that only calls `send` and `recv`.
Every change to the big interface recompiles every implementor and every user.

Good: `class ByteSink { send }`, `class ByteSource { recv }`, `class Configurable { set_tls,
set_keepalive }`. A real socket implements all three; the codec depends on `ByteSink` and
`ByteSource`; the fake implements two functions.

The cost: more types to name and understand. Apply ISP where you have multiple *different
clients* of an interface or where you need test doubles. A single-user interface can be as
large as its single user needs.

### 2.5 D: Dependency Inversion Principle

**Definition:** high-level modules (business rules) must not depend on low-level modules
(database, sockets, files). Both depend on abstractions. The abstraction is *owned by the
high-level side*: the business layer declares what it needs (`AccountRepository`), and the
database layer implements it.

Bad:

```cpp
#include <libpq-fe.h>
class LoanService {
    PGconn* conn_;                                   // business logic knows Postgres
public:
    LoanService() { conn_ = PQconnectdb(getenv("DB")); }   // creates its own dependency
    bool approve(LoanId id) { PQexec(conn_, "..."); }
};
```

You cannot test `approve` without Postgres. You cannot run two `LoanService`s against two
databases. Changing the database means editing business code.

Good, with **constructor injection**:

```cpp
class LoanRepository {           // declared next to LoanService, in the business layer
public:
    virtual ~LoanRepository() = default;
    virtual std::optional<Loan> find(LoanId) = 0;
    virtual void save(const Loan&) = 0;
};

class LoanService {
public:
    explicit LoanService(LoanRepository& repo, Clock& clock) : repo_(repo), clock_(clock) {}
    bool approve(LoanId id);     // uses repo_ and clock_, knows nothing concrete
private:
    LoanRepository& repo_;
    Clock& clock_;
};
```

```cpp
// composition root (main or wire()): the only file that knows the concrete types
PgLoanRepository pg(conn_string);
SystemClock clock;
LoanService svc(pg, clock);
```

In tests: `FakeLoanRepository fake; FixedClock clock{2026-10-03}; LoanService svc(fake,
clock);`. Now `approve` is tested in microseconds with no I/O.

**Why a reference and not `shared_ptr`?** The composition root owns the concrete objects and
guarantees they outlive the service. A reference says exactly that. `shared_ptr` would say
"ownership is shared and unclear", which is false here (Module 03).

**Why `Clock`?** Anything that makes code non-deterministic (time, randomness, environment
variables, network) should be injected so tests can control it. Time is the most common one in
banking code: interest accrual, due dates, cut-off times.

### 2.6 How SOLID maps onto a server codebase

```
  +------------------+   depends on   +-------------------+
  |  transport/      | -------------> |  protocol/        |     (S: each dir = one concern)
  |  TcpListener     |                |  Codec, Request   |
  +------------------+                +-------------------+
                                               |
                                               v
                                      +-------------------+
                                      |  service/         |   declares the interfaces
                                      |  KvService        |   it needs (D):
                                      |  LoanService      |   Store, Clock, Logger
                                      +-------------------+
                                               ^  implements
                                               |
                                      +-------------------+
                                      |  storage/         |   (O: new backend = new file)
                                      |  PgStore, BdbStore|
                                      +-------------------+
```

Arrows point *inward* to the service layer. Nothing in `service/` includes anything from
`storage/` or `transport/`. Module 09 makes this a full directory layout.

### 2.7 The over-engineering warning

SOLID applied everywhere produces a codebase where every concrete class has an interface,
every object is created by a factory, and reading one request path means opening twelve
files. That is as unmaintainable as the 2000-line `switch`, just differently.

Rules for restraint:

- An interface with exactly one implementation and no test double is probably not needed yet.
  Add it when the second implementation or the first test arrives (the refactor is cheap).
- Inject the *boundaries* (storage, transport, time, external services). Do not inject a
  `StringFormatter`.
- A value type (`Money`, `AccountId`, `Request`) does not need an interface. Interfaces are
  for *behaviour with side effects* that varies.
- If you cannot name a second plausible implementation in ten seconds, do not abstract.

## 3. Code

### `code/m02_solid_di.cpp`

A minimal key-value service that respects all five principles, plus the test wiring that
shows why it matters.

```cpp
// D: the service receives its dependencies; it does not create them.
class KvService {
public:
    KvService(Store& store, Logger& log) : store_(store), log_(log) {}
    void register_op(std::string name, std::unique_ptr<Operation> op);  // O: extend by adding
    std::string handle(std::string_view line);
private:
    Store& store_;  Logger& log_;  RequestParser parser_;           // S: parsing lives elsewhere
    std::map<std::string, std::unique_ptr<Operation>> ops_;
};
```

```cpp
// I: Logger is one function. Flushable is a separate optional capability.
class Logger    { public: virtual ~Logger() = default; virtual void info(std::string_view) = 0; };
class Flushable { public: virtual ~Flushable() = default; virtual void flush() = 0; };
class StdoutLogger final : public Logger, public Flushable { /* ... */ };
```

```cpp
// L: WriteThroughCache IS-A Store and keeps the contract "after put(k,v), get(k)==v".
// The commented-out alternative, a ReadOnlyStore whose put() throws, would not.
```

```cpp
// Composition root: the only place that knows concrete types.
InMemoryStore backing;  WriteThroughCache cache(backing);  StdoutLogger logger;
KvService svc(cache, logger);
svc.register_op("GET", std::make_unique<GetOp>());
```

The test wiring at the end swaps in `RecordingLogger` and asserts on captured lines. No
socket, no database, no stdout.

## 4. Under the hood (senior-level)

**What dependency inversion costs at the machine level.** Each injected interface is a
virtual call at the boundary. For boundaries that are already expensive (a database round
trip is 100 microseconds or more; a syscall is at least a microsecond) the virtual call
(nanoseconds) is noise. That is exactly why DIP is applied *at I/O boundaries* and not
between two tight inner functions: the abstraction cost is proportionally invisible there.

**Why abstractions belong to the high-level side.** If `storage/` defined `Store` and
`service/` included `storage/store.h`, then `service/` depends on `storage/` at *compile
time*: a change to anything in that header recompiles the service. When `service/` owns the
header, `storage/` includes it and the dependency arrow points the right way. In build terms:
the service library has no link dependency on libpq; only the storage library and the
executable do. You can see this in a `CMakeLists.txt`: `target_link_libraries(service ...)`
should never list a database driver.

**OCP and link-time behaviour.** A registry of operations populated at startup means the
linker cannot know which `Operation` subclasses are used; it keeps all of them. A `switch`
lets the compiler and linker drop dead code and inline small cases. For embedded or
size-constrained builds this matters; for a server it does not.

**LSP and the compiler.** The compiler cannot check LSP; it is a semantic contract. Tools that
help: `override` (signature match), `final` (prevents further breaking), and *contract tests*:
a single test suite parameterised over every implementation of an interface, asserting the
interface's documented behaviour. If `WriteThroughCache` and `PgStore` both pass the same
`StoreContractTest`, they are substitutable in practice.

**ISP and recompilation.** Changing a virtual function signature changes the vtable layout,
which changes the offsets compiled into every call site. Every translation unit that calls
through the interface must be recompiled, and if the interface is in a shared library, every
client binary must be rebuilt (ABI break). Small interfaces change less often and break fewer
clients. Module 05 (pimpl) and Module 10 (versioning) continue this.

## 5. In production

**Your stored-procedure re-engineering project is a DIP story.** Business logic in PL/pgSQL
procedures *is* business logic that depends on the database. Pulling it into a C or C++
layer that talks to the database through a repository interface is dependency inversion:
the rules no longer know whether they run against Berkeley DB, YugabyteDB or an in-memory
fake. When you tell this story in an interview, use the words: "the business rules were
coupled to the storage engine; I introduced a repository boundary so the rules could be
tested independently and the backend could change." Module 09 Section 5 gives the full
narrative.

**The 3 AM version of each principle.**

- SRP violated: a change to log formatting shipped on Friday; on Monday the parser rejects
  every message with a comma in a field, because the two shared a buffer. You find it by
  diffing the one giant file.
- OCP violated: a new message type was added to the `switch` in four of the five places it
  needed to go. The fifth is the audit log. Regulators notice three months later.
- LSP violated: a `NullStore` used in a staging environment returns success on `put` and
  nothing on `get`. The reconciliation job happily reports zero discrepancies.
- ISP violated: nobody writes unit tests because every fake is 400 lines. Bugs reach
  integration test, where each cycle is 40 minutes.
- DIP violated: the only way to test the interest-calculation fix is against the shared
  staging database, which someone else is also using. Flaky tests, nobody trusts them.

**The anti-pattern to watch for in yourself.** Having learned SOLID, engineers go through a
phase of abstracting everything. The symptom: `IFoo` interfaces with one `FooImpl` each,
factories for everything, and no tests actually using the seams. If the seam is not used for a
test or a second implementation, it is cost without benefit. Remove it.

## 6. Try it

1. **(easy)** In `m02_solid_di.cpp`, add a `DEL` operation without touching `KvService`. Done
   when `handle("DEL k")` returns `OK` and a subsequent `GET k` returns `NOT_FOUND`.
2. **(easy)** Write a `ReadOnlyStore` that throws on `put`. Pass it to `KvService`. Observe
   the failure. Then fix the design: introduce `ReadableStore` with only `get`, make `Store`
   derive from it, and make `GetOp::apply` take `ReadableStore&`. Done when a read-only
   backend cannot be passed to `PutOp` at compile time.
3. **(medium)** Add a `Clock` interface with `virtual std::int64_t now_ms() = 0;` and a
   `TtlStore` decorator that expires keys. Write a `FixedClock` for tests and show a test
   that advances time without `sleep`. Done when the test runs in under a millisecond.
4. **(medium)** Take one function from your own C codebase that mixes parsing, business
   rule and storage. Split it on paper into three units and write the interface the business
   unit needs from storage. Done when you can write a test for the business unit that needs
   no I/O.
5. **(hard)** Write a `StoreContractTest` function template `template <class F> void
   contract(F make_store)` that creates a store via `make_store()` and asserts the full
   contract (put-then-get, overwrite, missing key). Run it against `InMemoryStore` and
   `WriteThroughCache`. Done when adding a buggy `Store` makes the contract test fail with a
   clear message.

## 7. Interview questions

**Q: State the five SOLID principles in one sentence each.**
A: S: one reason to change per class. O: add behaviour by adding code, not editing it.
L: a subtype must be usable wherever its base is, without surprises. I: do not force clients
to depend on functions they do not call. D: business logic depends on abstractions it owns,
and infrastructure implements them.

**Q: How do you implement dependency inversion in C++ without a DI framework?**
A: Declare an abstract interface in the high-level module, take it by reference or
`unique_ptr` in the constructor of the class that needs it, and wire the concrete object in
a composition root (`main` or a `wire()` function). No framework is needed; the constructor is
the injection point.

**Q: Give an example of a Liskov violation and how to fix it.**
A: A `ReadOnlyStore` deriving from `Store` whose `put` throws. Callers of `Store&` are now
broken by type. Fix: split the interface into `ReadableStore` and `Store`, so read-only
backends never claim to support `put`.

**Q: When is the Open/Closed principle the wrong choice?**
A: When the set of variants is closed and small. A `switch` over an enum or `std::visit` over a
`std::variant` gives compile-time exhaustiveness checking that a runtime registry cannot. OCP
is for open sets: plug-ins, handlers added by other teams.

**Q: Why should the interface be declared by the high-level module and not the low-level one?**
A: So the compile-time and link-time dependency points from infrastructure to business logic,
not the reverse. The service library then has no dependency on database drivers, which is
what makes it testable and portable.

**Q: What does over-applying SOLID look like and how do you avoid it?**
A: Interfaces with one implementation and no test using them, factories for value types,
twelve files per request path. Avoid it by abstracting only at boundaries where you have a
second implementation or a test double, and by keeping value types as plain structs.

**Q: How does Interface Segregation relate to ABI stability?**
A: Changing a virtual function changes vtable layout and forces every client to recompile.
Small, stable interfaces change less often. For shared libraries, pair ISP with pimpl or a C
ABI boundary.

## 8. Recap

- SOLID names five forces; apply each where change is expected, skip it where it is not.
- S: split by reason to change (transport, protocol, business, storage, logging).
- O: registries and interfaces for open sets; `switch` or `std::variant` for closed sets.
- L: subtypes keep the base's contract; model unsupported operations as different types, not
  as throwing overrides.
- I: small interfaces, several of them; a test fake should be a few lines.
- D: business layer declares interfaces, infrastructure implements them, constructor injects
  them, composition root wires them.
- Inject the boundaries: storage, transport, time, external services. Not helpers.
- An abstraction with one implementation and no test is cost without benefit.
- Your stored-procedure re-engineering is a dependency-inversion story; tell it that way.
