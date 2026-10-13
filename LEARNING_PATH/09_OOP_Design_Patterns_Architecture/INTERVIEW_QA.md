# Interview Questions: OOP, Design Patterns and Architecture in C++

38 questions with model answers, ordered from basic to senior. Answer out loud first, then
compare. The module in brackets is where the topic is covered in depth.

## Basics

**1. What does a class give you that a C struct plus functions does not? [01]**
Access control that enforces invariants (only member functions can change private state),
constructors and destructors that run automatically (RAII), and virtual functions for
runtime polymorphism without hand-written function-pointer tables. The generated code for a
non-virtual member function is the same as a C function taking a struct pointer; the gain
is in what the compiler checks for you.

**2. How does a virtual call work, and what does it cost? [01]**
Each object of a polymorphic class holds a vptr to its class's vtable, an array of function
pointers. A virtual call loads the vptr, loads the slot, and makes an indirect call: a few
nanoseconds when predicted. The bigger cost is that the call cannot be inlined, so the
optimiser loses constant folding and vectorisation across it, plus 8 bytes per object.

**3. Why must a base class with virtual functions have a virtual destructor? [01]**
Deleting a derived object through a base pointer calls the base destructor only, unless the
destructor is virtual; the derived part is never destroyed, which is undefined behaviour and
in practice a leak or corruption. Either make it `public virtual` or make it `protected` and
non-virtual so deletion through the base is impossible.

**4. State the SOLID principles in one sentence each. [02]**
Single responsibility: a module has one reason to change. Open/closed: add behaviour by adding
code, not editing working code. Liskov substitution: a derived type must honour every promise
of its base. Interface segregation: clients depend only on the operations they use.
Dependency inversion: high-level policy and low-level detail both depend on an abstraction
owned by the policy side.

**5. Give a Liskov violation and its fix. [02]**
A `ReadOnlyStore` deriving from `Store` and throwing from `put()`: code written against
`Store` breaks when handed it. Fix by splitting interfaces, a `ReadableStore` with `get`
and a `Store` that adds `put`, so the read-only type never promises what it cannot do.

**6. Composition or inheritance? [01, 02]**
Prefer composition: it couples to a public interface, can be changed at runtime, and does not
expose the base's internals. Use public inheritance for a genuine "is-a" with substitutability,
mostly to implement an interface. Implementation inheritance for code reuse tends to produce
fragile hierarchies where a base change breaks subclasses.

**7. What is dependency injection, and why not just use a singleton? [02, 04]**
Dependency injection means a component receives its collaborators (store, clock, logger)
from its creator, usually through the constructor, instead of finding them itself. It makes
dependencies visible in the signature and replaceable in tests. A singleton is a hidden
global: tests share its state, initialisation and destruction order are hard to control,
and every user is coupled to one concrete class.

**8. What problems does a singleton cause in C++ specifically? [04]**
Static initialisation order across translation units is unspecified, so one global may use
another before it is constructed; destruction order at exit can leave a logger destroyed
before an object that logs in its destructor. Function-local statics fix construction
(thread-safe since C++11) but not destruction or test isolation. If one instance is
required, create it in `main` and pass it down.

## Ownership and lifetime

**9. How do you design ownership in a server? [03]**
Draw a tree: each object has exactly one owner, expressed as a value member or `unique_ptr`;
everything else holds references or observer pointers to things owned by an ancestor, so
they outlive the observer. The server owns the listener, pool and store; connections own
their sockets and buffers. `shared_ptr` is reserved for genuinely shared lifetimes, such as
an immutable config snapshot read by many requests.

**10. Why is `shared_ptr` everywhere a design smell? [03]**
It hides who is responsible for an object's lifetime, so destruction happens at an
unpredictable time on an unpredictable thread. It costs an atomic increment and decrement
per copy and a control block. Cycles leak. Most objects have one clear owner, and saying so
with `unique_ptr` makes the design reviewable.

**11. What is the observer lifetime bug and how do you prevent it? [06]**
The subject keeps raw pointers to observers; an observer is destroyed without unsubscribing,
and the next notify calls through a dangling pointer. A second form is a callback that
subscribes or unsubscribes during iteration. Fix with an RAII subscription token stored as
the observer's last member, and iterate over a snapshot of ids; or store `weak_ptr`s and
`lock()` before each call. Across threads, the `weak_ptr` lock or posting events to the
observer's own thread is needed, because a token can unsubscribe while a callback runs.

**12. When is a raw pointer acceptable in modern C++? [03]**
As a non-owning, nullable observer of an object whose lifetime is guaranteed by someone else,
for example a child pointing to its owner, or a function parameter that may be null. Never as
an owner. If it cannot be null, a reference is clearer.

## Patterns

**13. Strategy: `std::function`, virtual interface, or template parameter? [06]**
One operation chosen at runtime: `std::function`, which also accepts lambdas with captures.
Several related operations or a strategy with identity: an interface. Choice fixed at
compile time on a hot path: a template parameter, which inlines. The question is how many
operations and when the choice is made.

**14. How does `std::variant` with `std::visit` replace the visitor pattern? [06, 08]**
The variant holds one of a closed set of types with a tag maintained for you; `std::visit`
with an overload set calls the right lambda for the active type. No base class, no `accept`,
no heap, and a missing alternative is a compile error unless you add a generic fallback. It
suits closed types with open operations; a virtual hierarchy suits open types with a fixed
set of operations.

**15. What is NVI and why use it? [06]**
Non-virtual interface: the public function is non-virtual and calls private virtual hooks.
The base class keeps control of the skeleton (validation, locking, logging, exception
translation) so derived classes can customise steps but cannot skip the invariant parts, and
nobody can call a hook directly.

**16. How do you implement undo? [06]**
The command pattern with `execute()` and `undo()`, capturing the previous state at execute
time rather than at construction. A history stack owns executed commands; a composite command
undoes its parts in reverse, which is a transaction. In banking terms, a reversal is the undo
of a posting.

**17. How would you implement request middleware? [06]**
As chain of responsibility in function form: a handler is `function<Response(const Request&)>`
and a middleware is a function taking the next handler and returning a new one. Auth, rate
limiting, logging and timeouts each wrap the rest of the chain and are composed in order.
State shared across copies of a closure (a counter table) lives in a `shared_ptr`.

**18. What is type erasure? [08]**
Runtime polymorphism without requiring a common base class: a wrapper holds an internal
abstract concept, a template model implements it by forwarding to the stored object, and the
wrapper has value semantics. `std::function` and `std::any` are examples. It lets unrelated
types, including ones you cannot modify, be stored and used through one interface.

**19. What is CRTP and when do you use it? [08]**
The curiously recurring template pattern: `class Derived : public Base<Derived>`, so the base
can call derived functions through a `static_cast` with no virtual dispatch. Use it for
compile-time template methods and mixins (adding operators or counters) on hot paths. You
lose a common base type, so different derived types cannot share one container.

**20. What is policy-based design? [08]**
A host class takes its behaviours (storage, locking, logging) as template parameters and
composes them at compile time. Disabled policies cost nothing thanks to the empty base
optimisation and inlining. Costs: one instantiation per combination, configuration fixed at
build time, longer compile times and error messages.

**21. Virtual functions vs templates: what is the real cost model? [08]**
Virtual: an indirect call, no inlining, a vptr per object, one copy of the code, decisions at
runtime. Templates: direct inlined calls and full optimisation, but one copy of the code per
instantiation (binary size, instruction cache, compile time) and the type must be known at
compile time. Ask whether the decision is made at runtime and whether the call is hot.

## Concurrency patterns

**22. Active object vs monitor object? [07]**
A monitor object protects its state with a lock and runs methods on the caller's thread;
simple, but callers block on contention. An active object owns a thread and a queue; calls
become messages and return futures, so callers never block on its lock and its state is
touched by one thread only, at the cost of a queue hop and asynchronous results.

**23. Reactor vs proactor? [07]**
A reactor waits for readiness (epoll says a socket is readable) and the application then
performs the non-blocking read itself. A proactor starts the operation and is notified on
completion with the data already transferred (Windows IOCP, io_uring). Linux servers are
traditionally reactors; your Win32 background maps to the proactor model.

**24. Why should a thread pool's queue be bounded? [07]**
An unbounded queue turns overload into unbounded memory growth and unbounded latency: every
request waits behind everything already queued, and the server dies of memory instead of
refusing work. A bounded queue forces a decision at the edge: block the producer, reject with
"busy", or shed load, which keeps latency for accepted requests predictable.

## Architecture

**25. Describe a layered architecture for a TCP server. [09]**
Transport (sockets, framing, threads), protocol (bytes to typed requests and back, pure),
service (business rules), storage (persistence), plus common value types and a composition
root in `main`. Dependencies point toward the service; the service owns the storage
interface, and storage implements it. One CMake target per layer enforces the graph.

**26. What is your error-handling policy in a C++ server? [09]**
Programmer bugs assert or abort; expected failures are values (`Result`, `optional`);
transient environmental failures carry a retryable code. Each boundary translates errors into
the next layer's vocabulary without changing their meaning, wire errors are produced in one
function, exceptions never cross threads, C callbacks or destructors, and each error is
logged once where it is handled.

**27. How would you refactor legacy C business logic into modules? [09, 10]**
Inventory entry points and rules, write characterization tests that pin current behaviour,
create seams by extracting data access behind interfaces and rules into pure functions, then
route slices to the new implementation behind a switch (strangler), shadow-compare, switch
over and delete the old code. In my stored-procedure project, data-heavy validation,
authorisation and workflow rules moved into procedures that return all failures in one call,
which cut round trips per transaction.

**28. Business rules in stored procedures or in the application server? [09]**
Data-heavy checks and invariants every caller must respect are better near the data: one
round trip and impossible to bypass. Orchestration, integrations and complex computation are
better in the server: testable with fakes, debuggable, and scalable by adding servers.
Procedures cost database CPU, which is the hardest tier to scale, and need disciplined schema
versioning.

**29. What is pimpl and how does it relate to ABI stability? [05, 10]**
Pointer to implementation: the public class holds only a `unique_ptr<Impl>` and declares its
functions; all data and private helpers live in the `.cpp`. The class's size and layout never
change when the implementation does, so old binaries keep working and clients do not
recompile. Costs: one heap allocation and one indirection per call, and the destructor must
be defined where `Impl` is complete.

**30. What breaks ABI compatibility? [10]**
Changing the size or layout of a class used across the boundary (adding or reordering
members), adding or reordering virtual functions, changing parameter types (the mangled name
changes), changing inline functions or templates compiled into clients, and changing an
enum's underlying type. The linker matches names only, so most of these fail silently at
run time.

**31. How do you make an API hard to misuse? [10]**
Strong types for ids and money, enums instead of boolean flags, named-field request structs,
factory functions that validate (parse, don't validate), RAII for paired calls,
`[[nodiscard]]` on results, `explicit` constructors, `= delete` on dangerous overloads, and
ownership stated in the signature. The aim is that a wrong call does not compile.

**32. Fakes vs mocks? [10]**
A fake is a working simplified implementation (an in-memory ledger), and tests assert on the
resulting state; such tests survive refactoring. A mock records calls and verifies
interactions; it is right when the interaction is the only observable effect (an SMS, an
audit event). Over-mocking ties tests to the implementation and blocks refactoring.

## Low-level design

**33. How do you approach a low-level design interview? [11]**
Clarify functional requirements, scale, concurrency and scope, and write them down. Identify
entities and the parts likely to change, which become strategies. Draw a class diagram with
ownership. Code the core data structures and central methods so they compile. Walk an example,
handle edge cases, then discuss thread safety, scale and trade-offs, talking throughout.

**34. Design an LRU cache with TTL. Key points? [11]**
A doubly linked list in recency order plus a hash map from key to list iterator; `splice`
moves a node to the front in O(1) without invalidating other iterators. Each entry stores its
expiry from an injected steady clock; expiry is lazy on `get` plus a periodic sweep. One
mutex for thread safety (`get` mutates, so a reader-writer lock does not help), sharding by
key hash to scale.

**35. Design a rate limiter. Which algorithm and why? [11]**
Token bucket by default: two numbers of state per key, lazy refill on each call, bursts up
to capacity, exact long-run rate. Sliding window log when an exact count in any window is
required, at O(limit) memory per key. Wrap single-key algorithms in a keyed, lock-striped map
with idle-key eviction, and inject time so tests do not sleep.

**36. How do you design a workflow engine for loan applications? [11]**
A table of (state, event) to transition, each with allowed roles, guard rules and actions.
The generic engine runs every guard and reports all failures, writes an audit entry for every
attempt, changes state, then runs actions. Persist state, audit and outbox rows in one
transaction, use optimistic locking for concurrent edits, and load the table from
configuration so new products need no new code.

**37. How would you design and test a job scheduler? [11]**
A pure core holding a min-heap of (due time, sequence, id) and a job map, with `run_due(now)`
returning due jobs, lazy cancellation and retry with back-off; then a thin threaded shell
whose dispatcher sleeps with `wait_until` on the next due time and hands jobs to workers. All
ordering logic is tested by calling the core with chosen times; the threads get a couple of
short smoke tests.

**38. How do you present a design in ten minutes? [12]**
One minute of problem and requirements, a big-picture diagram with each box's responsibility,
the core flow or data structure, two or three decisions with the rejected alternatives in a
trade-off table, failure and scale behaviour, then open questions. Lead with the answer and
name the trade-offs before the audience asks.
