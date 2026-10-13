# 06. Behavioural Patterns

**Time:** ~3 hours   **Prerequisites:** 01, 02, 03, 05

## 1. In plain words

Behavioural patterns are about *who does what, in which order, and how they find out*. How
does a service swap one algorithm for another at runtime? How does one object tell five others
that something happened without knowing who they are? How do you turn "do this later" into a
thing you can queue? How do you keep a protocol parser from becoming a 600-line `switch`?

In C you have answered each of these with a function pointer, a callback list, a struct on a
queue, and an enum with a `switch`. Those are the right answers in C, and most of them are
still right in C++. This module gives each one its name, shows the class-hierarchy version
you will see in older C++ codebases, and then the modern version (`std::function`,
`std::variant`, lambdas) which is usually shorter and faster.

The single most important idea in this module is in Section 2.2: the observer pattern's
lifetime bug. It is the most common crash in event-driven C++ servers and most engineers
cannot explain why it happens or how to make it impossible.

## 2. The real thing

### 2.1 Strategy: `std::function` vs virtual vs template

**What:** make an algorithm swappable. A transfer service whose fee rule varies by deployment.

| Form | When | Cost |
|---|---|---|
| Virtual interface `FeePolicy { fee(); name(); }` | Strategy has several operations, or state with identity, or must be stored by name in a registry | 1 heap object, vptr, indirect call |
| `std::function<Paise(const Transfer&)>` | Strategy is one operation; lambdas capture their parameters | 32 bytes, indirect call, heap for captures over 16 bytes |
| Template parameter `TransferService<Fee>` | Choice known at compile time; hot path | Zero runtime cost, one type per strategy, cannot swap at runtime |
| C: `Paise (*fn)(const void* ctx, ...)` plus `void* ctx` | C | Indirect call, no type safety on ctx |

The decision: *how many operations* and *when is the choice made*. One operation, runtime
choice: `std::function`. Several operations: interface. Compile-time choice on a hot path:
template.

A property of `std::function` strategies that interfaces lack: they compose trivially.
`max_of(percent_fee(25, cap), crossbank_fee(300))` is a strategy built from strategies in
three lines (see the code file).

### 2.2 Observer and its lifetime bugs

**What:** a subject keeps a list of listeners and calls them when an event occurs. Listeners
register and (should) deregister.

**The bug:**

```cpp
class Subject { std::vector<Observer*> obs_; void notify() { for (auto* o : obs_) o->on_event(); } };
```

An `Observer` is destroyed without unsubscribing. The pointer stays in `obs_`. The next
`notify()` calls a virtual function through a freed object: the vptr is garbage, the jump
goes anywhere. Under AddressSanitizer this is caught immediately; in production it is a
crash hours later in an unrelated place.

A second bug: a callback subscribes or unsubscribes *during* `notify()`, mutating `obs_`
while it is being iterated. Iterator invalidation; undefined behaviour.

**Three fixes, in order of preference:**

1. **Subscription token (RAII).** `subscribe()` returns a move-only object whose destructor
   unsubscribes. The observer stores the token as its *last* member, so when the observer is
   destroyed the token goes first and the subject forgets it before any other member dies.
   Mark `subscribe()` `[[nodiscard]]` so dropping the token is a warning. Constraint: the
   subject must outlive all tokens (it is owned higher in the tree, Module 03), and the
   subject's `notify()` iterates over a snapshot of ids so callbacks may mutate the list.
2. **`weak_ptr` observers.** The subject stores `std::weak_ptr<Observer>` and calls
   `lock()` before each call, pruning expired entries. Correct and simple, but forces every
   observer to be `shared_ptr`-managed and adds atomics on every notify. Use it when the
   observers are already shared for other reasons.
3. **Documented manual unsubscribe.** The C way. It works until someone forgets, and in a
   large codebase someone forgets.

**Threads make it worse.** If `notify()` runs on thread A while the observer is destroyed on
thread B, even the token fix has a window: B's destructor unsubscribes while A is already
inside the callback. Full solutions: notify under a lock that the destructor also takes
(risk: deadlock if the callback re-enters); or post events to the observer's own thread as
values (actor model, Module 07); or the `weak_ptr` form, where `lock()` keeps the object
alive for the duration of the call. Pick based on whether callbacks may block.

**Modern form:** the observer list is `std::vector<std::function<void(const Event&)>>`; no
`Observer` interface. The token mechanism applies identically.

### 2.3 Command: undo and request queues

**What:** an operation as an object: what to do and everything needed to do it. Then it can be
queued, logged, replayed, undone, or run on another thread.

Two shapes:

- **Undoable commands** need an interface with `execute()` and `undo()` and state captured *at
  execute time* (the previous value), not at construction. A `CompositeCommand` undoes its
  parts in reverse order: that is a transaction. A `History` owns executed commands in a
  `vector<unique_ptr<Command>>` and a redo stack.
- **Fire-and-forget commands** are just closures: `std::function<void()>` on a queue. This is
  what a thread pool's task queue is (Module 07). The closure *is* the command object; the
  captures *are* its arguments.

**C equivalent:** `struct { int type; union { ... } args; }` on a queue, with a `switch` to
execute. The C++ form keeps `execute` and `undo` next to each other, so they cannot drift
apart when someone edits one.

**Banking relevance:** a reversal is the undo of a posting. Designing postings as commands
with a stored inverse is how reversals stay correct when the posting logic changes.

### 2.4 State machine: connection states, protocol parser

**What:** behaviour depends on which of a finite set of states the object is in; events
cause transitions.

Three implementations:

1. **Enum + `switch` in each event handler.** The C form, and often the best for small
   machines: the compiler's `-Wswitch` tells you about a missing case; everything is in one
   place; no allocations. Grows badly when states carry different data or when there are many
   events times many states.
2. **State classes (GoF).** `class State { virtual unique_ptr<State> on_bytes(...) }`, one
   class per state; the context swaps its `unique_ptr<State>` on transition; `on_enter`/
   `on_exit` hooks. Each state's data lives in that state object and is destroyed on exit,
   which is excellent for "buffer being accumulated during this state". Cost: the transition
   table is spread across classes; one allocation per transition (avoidable with a pool).
3. **`std::variant<Connecting, Authenticating, Ready, Closing, Closed>` with `std::visit`.**
   Each state is a plain struct with its own data; transitions return a new variant value;
   no heap, no vtable, and a missing (state, event) handler is a compile error. This is the
   form to prefer in new C++17 code (Module 08 has the full example).

The protocol parser is the state machine you already own. A length-prefixed or
delimiter-based framing layer is `WaitingForHeader -> ReadingBody -> Complete`, with the
partial buffer as state data. Writing it as an explicit machine (any of the three forms)
instead of nested `if`s over `bytes_so_far` is what makes it testable one byte at a time.

### 2.5 Template method vs NVI

**What:** a base class fixes the skeleton of an algorithm; subclasses fill in steps.

**NVI (Non-Virtual Interface)** is the C++ idiom for doing it safely: the public function is
non-virtual and calls private virtual hooks.

```cpp
class Handler {
public:
    std::string handle(const Request& r) {        // public, NON-virtual: the skeleton
        if (!validate(r)) return "ERR";
        log("start"); auto reply = run(r); log("end"); return reply;
    }
private:
    virtual bool validate(const Request&) { return true; }   // hook with default
    virtual std::string run(const Request&) = 0;              // required hook
};
```

Why private virtual: a derived class can override a private virtual (access and virtuality
are independent in C++), but cannot *call* it, so nobody can invoke `run()` without the
validation and logging around it. The invariant parts (locking, metrics, exception
translation) are in one place and cannot be skipped.

**Modern alternative:** when the varying part is one step, pass a callable to a free function
template (`handle_with(request, log, [](auto& r) { ... })`). No inheritance. Prefer it unless
you need several hooks or per-handler state.

### 2.6 Visitor: `std::variant` + `std::visit` as the modern form

**The problem visitor solves:** you have a closed set of *types* and an open set of
*operations*. With virtual functions, adding an operation means editing every type. Visitor
flips it: adding an operation is one new visitor; adding a type is the expensive change.

**Classic form:** `Message::accept(Visitor&)` calls `visitor.visit(*this)`, which overloads
on the concrete type. Double dispatch: first on the message's dynamic type (virtual
`accept`), then on the visitor's overload set. Boilerplate: every element type needs
`accept`; every visitor must implement every `visit`.

**Modern form:**

```cpp
using Message = std::variant<Deposit, Withdrawal, Transfer>;   // a type-safe tagged union

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

std::string serialise(const Message& m) {
    return std::visit(overloaded{
        [](const Deposit& d)    { /* ... */ },
        [](const Withdrawal& w) { /* ... */ },
        [](const Transfer& t)   { /* ... */ },
    }, m);
}
```

No base class, no vptr, no heap, no `accept`. Forget an alternative and it does not compile.
A generic lambda `[](const auto& x) { return x.amount; }` handles "same code for every
alternative". A struct with `operator()` overloads is a stateful visitor.

**C equivalent:** `struct { enum tag; union { ... }; }` plus one `switch` per operation.
`std::variant` is that with the tag maintained for you, the union typed, and the `switch`
checked for exhaustiveness.

### 2.7 Chain of responsibility: request middleware

**What:** a request passes through a sequence of handlers; each can answer it, modify it,
or pass it on.

**Classic:** linked handlers with `set_next()` and a virtual `try_handle()` returning
`optional<Response>`. Each link owns the next (`unique_ptr`), so the chain is a list.

**Middleware form** (what every web framework and most RPC servers use):

```cpp
using Handler    = std::function<Response(const Request&)>;
using Middleware = std::function<Handler(Handler next)>;   // wraps the rest of the chain
Handler app = compose({logging(log), auth(), rate_limit(2)}, business_handler);
```

A middleware receives the rest of the chain as `next` and returns a new handler that does
its work before and/or after calling `next`. Composition is function application from the
inside out. Authentication, rate limiting, logging, tracing, timeouts and retries are each
15 lines and independent of each other and of the business handler.

Note the `shared_ptr` in `rate_limit`: the counter table must outlive every copy of the
closure, and `std::function` copies closures. That is a legitimate shared-ownership case
(Module 03).

### 2.8 Iterator

In C++ the iterator pattern is not a virtual interface; it is a protocol the language speaks.
Provide `begin()`/`end()` returning a type with `operator*`, `operator++`, `operator==`, and
the five `iterator_traits` typedefs, and your container works with range-`for` and every
standard algorithm. The code file has a ring buffer with a forward iterator and a paginated
database-cursor-style input iterator that fetches pages lazily, so the consumer sees rows
and never pages.

Iterator categories you should be able to name: input (single pass), forward, bidirectional,
random access. The category tells algorithms what they may do; `std::sort` requires random
access, `std::find` only input.

### 2.9 Choosing

| Problem | First choice in C++17 | Classic form |
|---|---|---|
| Swap one operation at runtime | `std::function` | Strategy interface |
| Swap several related operations | Interface | Strategy interface |
| Notify many listeners | `vector<function>` + RAII token | Observer interface |
| Queue work / undo | Closures / `Command` with `undo` | Command interface |
| Finite states with per-state data | `std::variant` + `visit` | State classes |
| Fixed skeleton, varying steps | NVI, or callable parameter | Template method |
| Closed types, open operations | `std::variant` + `visit` | Visitor |
| Request pipeline | Middleware functions | Chain of handlers |
| Walk a structure | `begin()`/`end()` protocol | Iterator interface |

## 3. Code

- `code/m06_strategy_function_vs_virtual.cpp`: fee policy as C function pointer, virtual
  interface, `std::function` (with composition), and template parameter; prints `sizeof` of
  each so the costs are visible.
- `code/m06_observer_lifetime.cpp`: the naive observer (bug explained), the RAII
  `Subscription` token with snapshot iteration in `notify`, an `AuditLog` that is safe by
  construction (token as last member), and the `weak_ptr` variant that prunes dead observers.
- `code/m06_command_undo_queue.cpp`: `SetCommand`/`EraseCommand` capturing previous state at
  execute time, `CompositeCommand` as a transaction, `History` with undo/redo, and a
  `RequestQueue` of closures.
- `code/m06_state_machine_connection.cpp`: GoF state classes for a connection protocol
  (`Connecting -> Authenticating -> Ready -> Closing -> Closed`), transitions by returning
  the next state, `on_enter`/`on_exit` hooks, attempt counter kept in the context.
- `code/m06_template_method_nvi.cpp`: `Handler::handle` as the fixed skeleton with private
  virtual `validate` and `run`, plus the callable-parameter alternative.
- `code/m06_visitor_variant.cpp`: classic accept/visit and the `std::variant` form with
  `overloaded`, a stateful `Totals` visitor and a generic lambda.
- `code/m06_chain_middleware.cpp`: linked handlers with NVI, and `compose()` for
  `std::function` middleware with auth, rate limit and access log.
- `code/m06_iterator.cpp`: `RingBuffer<T, N>::iterator` (forward) and `RowIterator` (input,
  paginated), used with range-`for`, `std::accumulate`, `std::max_element`, `std::find`.

The token-based observer, which is the piece to memorise:

```cpp
class Subject {
public:
    class Subscription {           // move-only RAII token
    public:
        ~Subscription() { reset(); }
        void reset() { if (subject_) { subject_->unsubscribe(id_); subject_ = nullptr; } }
        /* move ctor/assign transfer subject_ and id_ */
    private:
        Subject* subject_ = nullptr;  std::uint64_t id_ = 0;
    };
    [[nodiscard]] Subscription subscribe(Callback cb);   // dropping the token unsubscribes at once
    void notify(const Event& e);                         // iterates a snapshot of ids
};

class AuditLog {
    std::vector<std::string> lines_;
    Subject::Subscription sub_;   // LAST member: destroyed first, so no callback sees dead lines_
};
```

## 4. Under the hood (senior-level)

**`std::function` internals (libstdc++).** 32 bytes: a 16-byte local buffer, a pointer to a
manager function (copy/destroy/type query) and a pointer to an invoker. Callables that fit in
16 bytes and are nothrow-movable are stored inline (a function pointer, a lambda with one or
two pointer captures); larger ones are heap-allocated. Invocation: load the invoker pointer
and call it indirectly, which then calls the target. Two indirect calls for the heap case,
effectively one for the inline case after inlining of the invoker. Copying a `std::function`
copies the callable (and heap-allocates again if it was on the heap). Hence `const
std::function&` parameters, or templates, on hot paths.

**`std::variant` internals.** Storage is a union of the alternatives plus an index
(`unsigned char` for small variants, so `sizeof(variant<A,B>)` is `max(sizeof A, sizeof B)`
rounded up to alignment, plus one byte, padded). `std::visit` with one variant compiles to a
jump table indexed by the index, or to a `switch`, with each case calling the visitor
overload inline. For `n` variants it builds an n-dimensional table. No heap, no vptr, fully
inlinable. The valueless-by-exception state exists if a converting assignment throws
mid-way; `std::visit` throws `bad_variant_access` then.

**Why a private virtual function can be overridden.** Access control applies to *names* at
the point of use; virtual dispatch applies to the vtable slot. `Derived::run` overrides
`Base::run` regardless of either's access specifier. The call inside `Base::handle` is
`this->run()`, which is legal inside `Base`. This is the whole trick behind NVI.

**Double dispatch cost.** Classic visitor: one virtual call (`accept`), then one more
(`visit`), each a vptr load plus indirect call. Variant visitor: one bounds-checked index
load plus one indirect jump through a table, or a direct `switch`. Both are cheap; the
variant form additionally inlines the visitor bodies, which the classic form cannot.

**Observer notify and iterator invalidation.** `std::vector::erase` during range-`for` over
the same vector is undefined behaviour; it may appear to work until the vector reallocates.
The snapshot-of-ids approach costs an allocation per `notify`; an alternative is a
"notifying" flag plus a deferred removal list, which is what Qt's signals and most GUI
toolkits do internally.

**State classes and allocation.** Each transition `make_unique<NextState>()` is a `malloc`
and a `free`. For a connection handling thousands of messages a second this is noise; for a
parser running per byte it is not. Fixes: states as `std::variant` (no allocation), or a
per-context pool of state objects.

## 5. In production

**The observer crash, as seen in a dump.** Stack: `Subject::notify` -> `??? ()`. The frame
above is at an address that is not in any module, or in the middle of a function. `obs_[i]`
points into freed memory; the first word (vptr) is whatever the allocator wrote there (often
a free-list pointer). The fix is architectural (tokens), not a null check.

**Middleware in your RPC server.** Authentication, per-client throttling, request logging
with correlation ids, latency histograms, and panic-to-error-reply translation are each one
middleware. The business handlers contain none of it. When the operations team asks for
"log every request over 500 ms", that is a ten-line middleware and a config flag, not a
change to forty handlers.

**State machines and protocol bugs.** A connection stuck in `Authenticating` forever because
the timeout event was only handled in `Connecting`. With state classes, the missing
`on_timeout` override falls back to the base (if you gave it a default), silently. With the
variant form and no default in `overloaded`, the compiler refuses the missing combination.
This is the strongest argument for `std::variant` state machines in protocol code: every
(state, event) pair is reviewed because it must be written down.

**Commands for audit and replay.** If every mutation of the ledger is a `Command` with a
serialisable form, the audit log is the command log, replay into a fresh store is
reconstruction, and reversal is `undo`. This is event sourcing in one sentence, and it falls
out of the pattern for free.

**Strategy via configuration.** A fee schedule that varies per product and per customer tier,
loaded from a table at startup and compiled into `std::function` objects by a factory, lets
the product team change pricing without a deploy. The trade-off is that the set of
*mechanisms* (flat, percent, tiered) is still code; only parameters move to data.

## 6. Try it

1. **(easy)** In the strategy file, add a `TieredFee` as a `std::function` built from a
   `std::vector<std::pair<Paise, int>>` of thresholds and basis points. Done when `t2` is
   charged at the right tier and the lambda captures the table by value.
2. **(easy)** In the observer file, make `AuditLog` unsubscribe itself from *inside* its own
   callback (on the third event). Done when `notify()` survives the mid-iteration
   unsubscribe and you can explain why the snapshot made that safe.
3. **(medium)** Convert `m06_state_machine_connection.cpp` to an enum + `switch` form in one
   function `State step(State, Event, Connection&)`. Compare line counts and list what each
   form made easier. Then deliberately remove one case and observe `-Wswitch`. Done when
   both compile and you can argue for one in a code review.
4. **(medium)** Add a `Refund` alternative to the modern `Message` variant. Observe every
   place the compiler stops you. Then add a fourth message type to the *classic* hierarchy
   and list what the compiler did and did not catch. Done when you can state the trade-off
   "open types vs open operations" from the experience.
5. **(hard)** Write a `timeout(std::chrono::milliseconds)` middleware that runs `next` on a
   worker thread and returns `504` if it does not finish in time. Then explain the lifetime
   problem: what does the worker thread touch after the timeout fired and the `Request`
   went out of scope? Fix it by passing the request by value into the worker. Done when
   `-fsanitize=thread,address` is clean.
6. **(hard)** Make `RingBuffer::iterator` bidirectional (add `operator--`) and verify
   `std::reverse_copy` works. Then explain why it cannot be random-access without changing
   the representation (hint: `operator+` must be O(1) and comparisons must be consistent
   across the wrap-around).

## 7. Interview questions

**Q: When do you use `std::function` instead of a strategy interface?**
A: When the strategy is a single operation and lambdas can capture its parameters.
`std::function` composes easily and avoids a class per strategy. When the strategy has
several related operations or identity, an interface is clearer. On a hot path with a
compile-time known strategy, a template parameter removes the indirection entirely.

**Q: Explain the observer pattern's lifetime bug and how you prevent it.**
A: The subject holds pointers to observers; an observer destroyed without unsubscribing
leaves a dangling pointer that the next notify dereferences. Prevent it with an RAII
subscription token that unsubscribes in its destructor, stored as the observer's last member;
or with `weak_ptr` observers that the subject locks and prunes. Also iterate over a snapshot
so callbacks may subscribe or unsubscribe during notify.

**Q: What is NVI and why are the hooks private?**
A: Non-Virtual Interface: a public non-virtual function implements the fixed algorithm and
calls private virtual hooks. Private virtuals can be overridden but not called by derived
classes or outsiders, so the skeleton (validation, locking, logging) cannot be bypassed.

**Q: Why is `std::variant` with `std::visit` considered the modern visitor?**
A: It gives the visitor's key property, adding operations without editing types, with no
`accept` boilerplate, no base class, no heap, and compile-time exhaustiveness: a missing
alternative fails to compile. The trade-off is the same as classic visitor: the set of types
is closed.

**Q: Compare three ways to implement a state machine in C++.**
A: Enum plus `switch`: simplest, exhaustiveness via `-Wswitch`, but states cannot carry
distinct data cleanly. State classes: per-state data and hooks, but the transition table is
scattered and each transition allocates. `std::variant` of state structs with `std::visit`:
per-state data, no allocation, and every (state, event) pair must be written, which the
compiler enforces.

**Q: What is middleware and how is it chain of responsibility?**
A: Each middleware is a function that takes the rest of the chain and returns a handler that
does work before or after calling it. Composed inside-out, the first listed is outermost.
Each link decides whether to answer, modify or pass on the request, which is exactly chain of
responsibility expressed with closures instead of linked objects.

**Q: What does a type need to work with range-`for` and standard algorithms?**
A: `begin()` and `end()` returning an iterator type with `operator*`, `operator++`,
`operator==`/`!=`, and the `iterator_traits` typedefs (category, value_type, difference_type,
pointer, reference). The category tells algorithms what operations are allowed.

## 8. Recap

- Strategy: `std::function` for one operation, interface for several, template for hot and
  compile-time known.
- Observer: the lifetime bug is real; fix it with RAII tokens (last member) or `weak_ptr`,
  and iterate a snapshot.
- Command: capture undo state at execute time; a closure on a queue is a command with no undo.
- State machine: enum+switch, state classes, or `std::variant`; prefer variant in new code.
- Template method: public non-virtual skeleton, private virtual hooks (NVI); or a callable
  parameter.
- Visitor: closed types, open operations; `std::variant` + `overloaded` + `std::visit`.
- Chain of responsibility: middleware as `Handler(Handler next)`, composed inside-out.
- Iterator: a protocol, not an interface; provide `begin`/`end` and the typedefs.
- `std::function` is 32 bytes with a 16-byte inline buffer; `std::variant` is a typed union
  with an index and a jump table.
