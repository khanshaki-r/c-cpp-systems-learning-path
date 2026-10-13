# 05. Structural Patterns

**Time:** ~2.5 hours   **Prerequisites:** 01, 02, 03

## 1. In plain words

Structural patterns are about *how objects are composed*: how you make two things with
different interfaces talk to each other, how you hide a messy subsystem behind one door, how
you stack behaviour in layers, and how you keep the inside of a module from leaking into its
users. If creational patterns are about birth, structural patterns are about plumbing.

Every one of them exists in C. An adapter is the `pq_wrapper.c` file you wrote so the rest
of the program does not see libpq. A facade is a `.h` with three functions in front of a
subsystem with thirty. An opaque `struct conn;` in a header is pimpl. The C++ versions add
type checking, RAII and the ability to stack them without re-declaring every slot by hand.

## 2. The real thing

### 2.1 Adapter: making a C API fit your interface

**What:** a class that implements *your* interface by calling *their* API. The rest of the
program depends on your interface only.

**Why it matters in your world:** libpq, Berkeley DB, Win32 sockets, ODBC, OpenSSL. Each has
opaque handles, manual `free` functions, status codes and `char*` results. Letting those leak
into business code means every function has to know the cleanup protocol of three libraries.

The two-step method:

1. **RAII wrapper per handle type.** `std::unique_ptr<PGconn, decltype(&PQfinish)>` or a
   custom deleter struct. From this point on, no path can leak the handle. This step alone
   removes a whole class of bugs and is worth doing even without step 2.
2. **The adapter class.** Implements your interface (`Store`), holds the RAII handle,
   translates each call: your types to their types, their status codes to your
   `optional`/exception/`expected`, their `char*` to your `std::string` *before* the result
   is freed.

```cpp
struct ResultDeleter { void operator()(PGresult* r) const { PQclear(r); } };
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;

std::optional<std::string> PqStore::get(const std::string& key) const {
    ResultPtr r(PQexecParams(conn_.get(), "SELECT v FROM kv WHERE k = $1", 1, ...));
    if (PQresultStatus(r.get()) != PGRES_TUPLES_OK) throw DbError(PQerrorMessage(conn_.get()));
    if (PQntuples(r.get()) == 0) return std::nullopt;
    return std::string(PQgetvalue(r.get(), 0, 0));   // copy before r goes out of scope
}
```

Rules: the constructor either produces a usable adapter or throws (no half-open state); the
adapter is move-only (it owns a handle with identity); nothing outside the adapter's `.cpp`
includes the C header.

**Class adapter vs object adapter:** the GoF book distinguishes inheriting from the adaptee
(class adapter) from holding it (object adapter). In C++, always hold it. Inheriting from a
library type couples you to its layout.

### 2.2 Facade: one door in front of a subsystem

**What:** a class (or a header of free functions) that exposes a small, task-oriented
interface over a group of classes that callers would otherwise have to orchestrate.

`LoanEligibilityFacade::decide(applicant_id)` loads the applicant, runs the policy, records
the decision and notifies. The RPC handler, the batch job and the test all call one function.
If tomorrow the decision also needs a fraud check, one place changes.

**C equivalent:** the public header of a module. `loan.h` with `loan_decide()`; `loan.c`
calling into five other files. Facades are the most C-like pattern; they are just good
modules.

**Facade vs adapter:** an adapter changes the *shape* of one interface to match another. A
facade *simplifies* access to many. An adapter is a translator; a facade is a receptionist.

**Pitfall:** a facade that grows into a god class with forty methods and all the business
logic inside it. The facade should orchestrate; the rules live in the components.

### 2.3 Decorator: stacking behaviour around an interface

**What:** a class that implements interface `I`, holds another `I`, and forwards each call
while adding something before or after. Decorators stack in any order.

```
service --> LoggingStore --> MetricsStore --> RetryingStore --> PqStore
             (same Store interface at every arrow)
```

**Why it beats inheritance:** with inheritance you would need `LoggingPqStore`,
`MetricsPqStore`, `LoggingMetricsPqStore`, and the same again for every backend. With
decorators you have one class per *concern* and compose at wiring time. Order is a
deployment decision: metrics outside retry counts attempts; metrics inside retry counts
successes.

**The forwarding base:** write one `StoreDecorator` that forwards every method, and have each
concrete decorator override only what it changes. Without it, every decorator re-types all
the forwarding, and adding a method to `Store` means editing every decorator (that is also a
reason to keep interfaces small, Module 02 ISP).

**Ownership:** the outer decorator owns the inner via `unique_ptr`. One pointer at the top
owns the whole stack; destroying it unwinds the layers.

**C equivalent:** a `store_ops` table whose functions call another `store_ops` held in the
context. Works, but you re-declare every slot per layer and the `void*` context chain is
hard to debug.

**Cost:** one virtual call per layer per operation. Four layers over a database call is
nothing; four layers over an in-memory hash lookup on a hot path is measurable. Module 08
shows the template (static) decorator for the hot case.

### 2.4 Composite: trees where leaf and group share an interface

**What:** `Rule` is the component; `MinAge` is a leaf; `AllOf` and `AnyOf` are composites
holding `vector<unique_ptr<Rule>>`. `passes(applicant)` works on a leaf or on a whole tree.

Use it when callers should not care whether they are holding one thing or a group: rule
engines, file systems (file vs directory), UI widget trees, expression trees (a parsed SQL
`WHERE` clause is a composite). Your loan-origination domain is full of these: eligibility
policies, fee schedules, document checklists.

**Ownership:** a `unique_ptr` tree (Module 03). Composites own children; nobody points
upward. If children need their parent, use a `Rule*` observer set by the parent on `add`.

**Modern alternative for closed sets:** `std::variant<MinAge, MinScore, AllOf, AnyOf>` with
recursive `std::visit`. Harder to write (recursive variants need indirection) but gives
exhaustiveness checking and no vtables. For an open set where rules are plug-ins, the virtual
composite wins.

### 2.5 Proxy: controlling access to an object

**What:** same structure as a decorator (wraps, same interface) with a different *intent*: a
proxy stands in for the real object and controls access. Kinds:

- **Caching proxy**: answers reads from memory, writes through, invalidates. The code file
  builds an LRU proxy in front of a slow store.
- **Lazy (virtual) proxy**: creates the expensive object on first use.
- **Remote proxy**: looks local, performs an RPC. Your client library's stub objects are
  remote proxies; the server-side skeleton is the other half.
- **Protection proxy**: checks permissions before forwarding.

**The caching proxy's contract:** after `put(k, v)` through the proxy, `get(k)` returns `v`
(write-through plus cache update). What it cannot promise: freshness if someone writes to the
backing store around it. State that in the class comment. Every cache has an invalidation
story and most cache bugs are about it.

**Decorator vs proxy, one sentence:** a decorator adds behaviour the caller wants to see; a
proxy tries to be invisible.

### 2.6 Bridge and pimpl: compile firewall and ABI stability

**Bridge (GoF):** separate an abstraction from its implementation so both can vary
independently, via a pointer from the abstraction to an implementation interface. In practice
in C++ this appears as **pimpl**: a public class holding `std::unique_ptr<Impl>` where `Impl`
is defined only in the `.cpp`.

```cpp
// connection.h -- what clients see
class Connection {
public:
    explicit Connection(std::string host, std::uint16_t port);
    ~Connection();                               // defined in .cpp
    Connection(Connection&&) noexcept;           // declared here, defined in .cpp
    Connection& operator=(Connection&&) noexcept;
    bool send(std::string_view bytes);
private:
    struct Impl;                                 // incomplete here
    std::unique_ptr<Impl> impl_;
};
```

What you get:

- **Compile firewall.** Adding a field to `Impl` recompiles `connection.cpp` only. Without
  pimpl, every file including `connection.h` recompiles. On a large codebase this is the
  difference between a 10-second and a 10-minute edit-compile loop.
- **Header hygiene.** `<winsock2.h>`, `<libpq-fe.h>`, `<db.h>` stay inside the `.cpp`. Clients
  do not inherit their macros and name pollution.
- **ABI stability.** `sizeof(Connection)` is one pointer forever. A shared library can change
  `Impl` without clients rebuilding. (Adding *virtual functions* or changing public signatures
  still breaks ABI; pimpl protects the *data layout* only.)

What it costs: one heap allocation per object, one pointer indirection per member access, and
the boilerplate of declaring special members in the header and defining them in the `.cpp`.
Do not pimpl small value types or anything on a hot path; pimpl the handful of classes at
module boundaries.

**C equivalent:** `struct conn;` in the header, `struct conn { ... }` in the `.c`,
`conn_create()` / `conn_destroy()`. That is exactly pimpl. The C++ version adds RAII so the
opaque pointer cannot leak, and move semantics so it can be returned by value.

**The one rule people get wrong:** the destructor (and move operations) must be *defined in
the `.cpp`*, after `Impl` is complete. If you write `~Connection() = default;` in the header,
the compiler instantiates `unique_ptr<Impl>::~unique_ptr` in client code where `Impl` is
incomplete, and you get "invalid application of sizeof to incomplete type".

### 2.7 Choosing

| You want to | Pattern |
|---|---|
| Use a C library through your own interface, with RAII | Adapter |
| Give callers one simple entry point to a multi-step operation | Facade |
| Add logging/metrics/retry around an interface without touching implementations | Decorator |
| Treat one item and a group of items the same way | Composite |
| Cache, lazy-load, remote-call or permission-check transparently | Proxy |
| Hide implementation details from a header; stabilise ABI; cut compile times | Pimpl (Bridge) |

## 3. Code

- `code/m05_adapter_c_api.cpp`: a fake libpq-shaped C library, RAII wrappers for its two
  handle types, and `PqStoreAdapter` implementing `Store`. Shows the "copy the `char*` before
  the result is cleared" rule and the "constructor throws on connect failure" rule.
- `code/m05_facade_composite.cpp`: a `Rule` composite (`MinAge`, `MinCreditScore`,
  `MaxDebtToIncome`, `NoExistingDefault`, `AllOf`, `AnyOf`) that prints itself as an indented
  tree, and `LoanEligibilityFacade::decide()` orchestrating repository, policy, log and
  notifier.
- `code/m05_decorator_store.cpp`: `StoreDecorator` forwarding base; `LoggingStore`,
  `MetricsStore`, `RetryingStore`; a `FlakyStore` to exercise retry; two stacking orders to
  show that order is a design decision.
- `code/m05_proxy_cache.cpp`: `CachingStoreProxy` with LRU eviction (`std::list` +
  `unordered_map` of iterators), write-through, hit/miss counters, compared against the
  direct store on round-trip count.
- `code/m05_pimpl.cpp`: `Connection` with `unique_ptr<Impl>`, laid out as header / source /
  client sections, move-only, with the destructor defined where `Impl` is complete.

Key lines from the decorator:

```cpp
class StoreDecorator : public Store {
public:
    explicit StoreDecorator(std::unique_ptr<Store> inner) : inner_(std::move(inner)) {}
    void put(const std::string& k, const std::string& v) override { inner_->put(k, v); }   // forward
    /* get, erase forward too */
protected:
    Store& inner() { return *inner_; }
private:
    std::unique_ptr<Store> inner_;   // outer owns inner; one unique_ptr at the top owns the stack
};

class MetricsStore final : public StoreDecorator {
    using StoreDecorator::StoreDecorator;
    void put(const std::string& k, const std::string& v) override {
        time_it([&] { inner().put(k, v); });  ++c_.puts;
    }
};
```

Key lines from the proxy:

```cpp
std::optional<std::string> get(const std::string& k) const override {
    if (auto it = index_.find(k); it != index_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);   // O(1) move-to-front; iterators stay valid
        return it->second->second;
    }
    auto v = backing_.get(k);
    if (v) insert_or_update(k, *v);
    return v;
}
```

## 4. Under the hood (senior-level)

**Decorator call cost.** Each layer is a virtual call: load vptr, load slot, indirect call,
and no inlining through it. Four layers add roughly four times 2 to 5 ns when predicted. GCC
cannot devirtualise because the inner pointer's dynamic type is a runtime fact. If you need
zero-cost layering on a hot path, the template version (`Logging<Metrics<MemoryStore>>`,
Module 08) is inlined completely, at the cost of a distinct type per stack and recompilation
when the stack changes.

**Why `std::list` plus `unordered_map` for LRU.** `std::list::splice` moves a node in O(1)
without invalidating any iterator, so the map can store iterators to list nodes and the
"move to front" operation touches three pointers. A `std::vector` would need O(n) shifting;
a `std::deque` invalidates iterators on push. The cost is one heap node per entry and poor
cache locality; an intrusive doubly-linked list over a slab of entries is the production
version (Module 11, LRU walkthrough).

**Pimpl and the ABI.** The Itanium ABI defines a class's layout by its members in order with
alignment padding. Clients compile `sizeof(Connection)` and member offsets into their code.
Changing a member breaks every client binary; with pimpl, clients only ever see one pointer.
Note what pimpl does *not* protect: adding a virtual function shifts vtable slots; changing a
public inline function changes code compiled into clients; changing an exception type thrown
changes the contract. Module 10 covers versioning.

**Pimpl and `const`.** `impl_->x = 1` compiles inside a `const` member function because the
pointer is const, not the pointee. You lose the compiler's help with `const`-correctness
unless you wrap access in `const Impl& impl() const` and `Impl& impl()` helpers. The
`std::experimental::propagate_const` wrapper fixes this; most codebases just write the two
accessors.

**Adapter and exception safety across the C boundary.** C code is not exception-aware. If
your adapter passes a C++ callback into a C library (`qsort` comparator, `PQnoticeReceiver`,
`DB->set_errcall`) and the callback throws, the exception unwinds through C frames compiled
without unwind tables, which is undefined behaviour and usually terminates. Catch everything
in the callback and translate to a status code or a stored error.

**Facade and link-time dependencies.** A facade in its own library pulls in every subsystem it
orchestrates. If a client only needs one small piece, linking against the facade library drags
in all of them. Keep the facade in the application layer, not in a shared foundation library.

## 5. In production

**Your Berkeley DB to YugabyteDB migration.** Two adapters implementing one `Store` (or
`CursorSource` and `RowSink`) interface would have let you write the migration loop once and
test it with two in-memory fakes. The adapter for Berkeley DB holds a `DB*` and a `DBC*`
with custom deleters; the adapter for YugabyteDB holds a `PGconn*`. The loop never sees
either. When telling this story, the phrase is: "I isolated both storage engines behind a
common interface so the transformation logic was independently testable."

**Decorators are how observability gets into a server without polluting it.** Logging,
metrics, tracing, retry, circuit breaking, timeout: each is one decorator over the storage
interface and one over the transport interface. The business logic has no `log(...)` calls
in it for I/O; they are in the decorators, configured per environment (verbose logging in
staging, metrics only in production).

**The caching proxy at 3 AM.** Symptom: a customer sees an old balance for 30 seconds after
a transfer. Cause: a second server instance wrote to the database; this instance's caching
proxy served the stale value. Fixes, in order of cost: short TTL; invalidation messages
between instances; move the cache to a shared tier (Redis) so there is one cache; or do not
cache data that must be read-your-writes across instances. A caching proxy's documentation
must say which of these it does.

**Pimpl in a shared library you ship to clients.** If your bank ships a client SDK (a `.so`
or `.dll`) to partner systems, every public class should be pimpl'd or behind a C ABI.
Otherwise a bug-fix release that adds a field forces every partner to rebuild, which they
will not do for months.

## 6. Try it

1. **(easy)** In `m05_adapter_c_api.cpp`, add `erase()` to `Store` and the adapter (the fake
   library needs a `DEL k` command). Done when the adapter compiles and `erase` on a missing
   key returns `false` without throwing.
2. **(easy)** Add a `TimeoutStore` decorator that measures each call and logs a warning if it
   exceeds a threshold. Insert it in both positions relative to `RetryingStore` and explain
   the different meaning. Done when both stacks compile and you can state which one you
   would deploy.
3. **(medium)** Extend `CachingStoreProxy` to cache *negative* results (key absent) with a
   short TTL, so repeated lookups of a missing key do not hit the database. Done when a
   test shows one round trip for five gets of a missing key, and a `put` of that key is
   visible immediately after.
4. **(medium)** Write a `Rule` for "applicant age between 21 and 60" two ways: as a new leaf
   class, and as an `AllOf` of `MinAge` and a new `MaxAge`. Then add a `Not` composite. Done
   when `Not(NoExistingDefault)` reads correctly in `describe()` and you can argue which of
   the two age solutions is better and why.
5. **(hard)** Convert `m05_pimpl.cpp` into three real files (`connection.h`, `connection.cpp`,
   `main.cpp`), compile with `-H` to see what headers each pulls in, then add a `std::vector`
   member to `Impl` and confirm `main.cpp` does not recompile (`make -n` or timestamps). Then
   move `~Connection() = default;` into the header and read the error message. Done when you
   can explain the error in terms of where `unique_ptr<Impl>`'s destructor is instantiated.

## 7. Interview questions

**Q: How would you wrap a C library like libpq in C++?**
A: First, an RAII wrapper per handle type using `unique_ptr` with a custom deleter, so no path
leaks. Then an adapter class implementing my own interface, translating status codes to
optionals or exceptions and copying `char*` results into `std::string` before freeing. Nothing
outside the adapter includes the C header.

**Q: What is the difference between a decorator and a proxy?**
A: Same structure: wrap an object behind the same interface. Different intent. A decorator
adds behaviour the caller wants (logging, metrics, retry) and stacks. A proxy controls access
and aims to be transparent (caching, lazy loading, remote access, permissions).

**Q: Why use decorators instead of subclasses for logging and metrics?**
A: Subclassing needs one class per combination of concern and backend. Decorators need one
class per concern and compose at wiring time in any order. Order itself becomes a design
decision, for example metrics outside or inside retry.

**Q: What does pimpl buy and what does it cost?**
A: It buys a compile firewall (private changes do not recompile clients), header hygiene
(platform headers stay in the `.cpp`), and data-layout ABI stability for shared libraries. It
costs a heap allocation, a pointer indirection, and boilerplate; and the destructor and move
operations must be defined in the `.cpp` where `Impl` is complete.

**Q: What can go wrong with a caching proxy?**
A: Staleness when another writer bypasses it, which needs a TTL, invalidation messages or a
shared cache tier. Unbounded growth without eviction. Caching negative results without a TTL
so a newly created key stays invisible. Serving a value after a failed write if you update the
cache before the backing store.

**Q: When would you use a composite, and how do you own the children?**
A: When callers should treat one item and a group uniformly: rule trees, expression trees,
widget trees. Children are owned by `vector<unique_ptr<Component>>` in the composite; parent
links, if needed, are raw observer pointers.

**Q: What is the facade pattern in C terms?**
A: A module's public header: a few task-oriented functions in front of many internal ones.
The point is to give callers one entry point and keep the choreography in one place.

## 8. Recap

- Adapter: RAII wrapper per C handle first, then a class implementing your interface over
  their API; copy results out before freeing.
- Facade: one simple entry point orchestrating several components; keep rules in the
  components, not the facade.
- Decorator: same interface, wraps and forwards, stacks in any order; one class per concern;
  a forwarding base avoids boilerplate.
- Composite: leaves and groups share an interface; `unique_ptr` tree ownership.
- Proxy: same structure as decorator, intent is access control; caching proxies need a stated
  invalidation story.
- Pimpl: `unique_ptr<Impl>` with special members defined in the `.cpp`; buys compile firewall
  and layout ABI stability; costs an allocation and an indirection.
- Every layer of virtual wrapping is a non-inlinable call; fine around I/O, measurable around
  in-memory hot paths.
- Exceptions must never cross into C frames: catch in callbacks and translate.
