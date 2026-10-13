# 10. API Design and Code Quality

**Time:** ~2.5 hours   **Prerequisites:** 01, 02, 03, 09

## 1. In plain words

An API (application programming interface) is the set of functions and types other code is
allowed to call: a class's public members, a library's headers, a server's wire protocol.
A good API is like a good ATM. There are few buttons, each does one obvious thing, and it is
hard to do something dangerous by accident: you cannot take the cash and leave the card in
the machine, because the machine returns the card first.

Bad APIs are the source of a large share of production bugs. Not because the implementation
is wrong, but because callers use it in ways the author did not expect: arguments swapped,
return values ignored, objects used after they are freed, functions called from two threads.
In C you defended against this with comments and code review. C++ lets you push much of it
into the type system, so the compiler refuses the mistake.

This module is about that, and about the habits that keep a codebase healthy over years:
tests that make change safe, code review that catches design problems early, and replacing
legacy code gradually instead of in one risky rewrite.

## 2. The real thing

### 2.1 Easy to use correctly, hard to use incorrectly

Scott Meyers put the whole of API design in one sentence: make interfaces easy to use
correctly and hard to use incorrectly. Concretely:

- **Minimal.** Every public function is a promise you must keep. If a caller can build it
  from two existing functions, it probably should not be a member. Fewer functions also
  means fewer ways to leave the object in a half-updated state.
- **Consistent.** Same naming, same argument order, same error style across the whole API.
  If `get()` returns `optional` in one class, do not return a sentinel in another.
- **Obvious at the call site.** Readers see calls far more often than declarations.
  `post(txn, true, false)` says nothing; `post(txn, Posting::DryRun, Notify::No)` says it all.
- **Ownership visible in the signature** (Module 03):

| Parameter | Means |
|---|---|
| `const T&` / `T` (cheap) | borrow for the call / copy |
| `T&` | borrow and modify (prefer returning a value instead) |
| `T*` | borrow, may be null |
| `std::unique_ptr<T>` | the function takes ownership (a sink) |
| `std::shared_ptr<T>` | the function will share ownership (rarely right) |
| `std::string_view`, pointer plus size | read-only view of characters / elements, valid only during the call |

### 2.2 Techniques that make misuse a compile error

1. **Strong types** instead of raw integers and strings: `AccountId`, `Paise`, `BranchCode`.
   Section 2.5.
2. **Enums instead of booleans** for options. A `bool` parameter is a puzzle at the call site
   and silently accepts an `int`.
3. **Named-field request structs** for functions with several same-typed parameters.
   `transfer({.from = a, .to = b, .amount = x})` (C++20 designated initialisers; in C++17 a
   struct with named members) stops positional swaps.
4. **Make invalid states unrepresentable.** A connection that is either `Connecting` with a
   retry count or `Ready` with a session is a `std::variant`, not a struct with a bool and
   two fields of which one is meaningless (Module 08).
5. **Parse, don't validate.** (The phrase is from an essay by Alexis King.) A type whose only
   constructor is a `parse()` that returns `optional<T>` guarantees that every value of that
   type in the program is valid. `Ifsc::parse("HDFC0001234")` in the code file. After the
   boundary, nobody re-checks.
6. **RAII for paired calls.** Lock/unlock, begin/commit, subscribe/unsubscribe: return an
   object whose destructor does the second half (Module 06's subscription token).
7. **Return values, not out-parameters.** `std::optional<Customer> find(id)` cannot be
   misused the way `int find(int id, Customer* out)` can (ignored return, uninitialised out).
8. **Delete what must not happen.** `= delete` the copy constructor of a socket; `= delete`
   an overload that would accept a dangerous conversion
   (`void set_timeout(double) = delete;` next to `set_timeout(std::chrono::milliseconds)`).

### 2.3 const-correctness

- A `const` member function promises not to change the object's observable state. Mark every
  member function that does not modify `const`; the compiler then lets callers holding a
  `const&` use it, and stops you from accidentally mutating.
- **Logical vs bitwise constness.** A cache inside a `const get()` or a mutex locked inside a
  `const size()` is fine: mark those members `mutable`. The rule is that observable state
  does not change.
- **The standard library's convention:** `const` member functions are safe to call
  concurrently from several threads on the same object. Your classes should keep that
  promise, which means a `mutable` cache inside a `const` function needs a lock or an atomic.
- Take parameters by `const T&` (or by value for cheap types). Do not return `const T` by
  value: it disables moving from the result.
- Prefer `const` local variables. A value that never changes after initialisation is one
  less thing to track while reading.

### 2.4 noexcept

`noexcept` is a promise that the function does not throw. If it throws anyway,
`std::terminate` is called immediately. Use it:

- **On move constructors and move assignment.** `std::vector` moves elements during
  reallocation only if the move constructor is `noexcept` (otherwise it copies, to keep the
  strong exception guarantee). A missing `noexcept` on a move turns every `push_back`
  reallocation into a copy.
- **On swap, destructors** (implicitly `noexcept` already), and simple getters.
- **On functions where throwing would be a bug** and you prefer a crash with a clear stack
  to a half-handled exception.

Do not put it on a function that may reasonably throw later: removing `noexcept` from a
public API is a breaking change for callers that relied on it (for example with
`std::is_nothrow_move_constructible`).

### 2.5 Strong types

```cpp
template <class Tag, class Rep>
class Strong {
public:
    constexpr explicit Strong(Rep v) noexcept : v_(v) {}   // explicit: no silent conversion
    constexpr Rep get() const noexcept { return v_; }
    friend constexpr bool operator==(Strong a, Strong b) noexcept { return a.v_ == b.v_; }
private:
    Rep v_;
};
using AccountId = Strong<struct AccountIdTag, std::uint64_t>;
using BranchId  = Strong<struct BranchIdTag,  std::uint32_t>;
```

`AccountId` and `BranchId` are different types, so passing one where the other is expected
does not compile. A tag declared inline (`struct AccountIdTag`) is enough; it is never
defined. Give each strong type only the operations that make sense: money adds to money and
multiplies by an integer; an id has no arithmetic at all. Money is an integer count of the
smallest unit (paise), never `double`.

### 2.6 [[nodiscard]]

`[[nodiscard]]` on a function makes ignoring its return value a warning (an error with
`-Werror`). On a *type*, it applies to every function returning that type by value.

Put it on:
- error-returning types (`Result`, `Status`): ignoring them is the classic C bug;
- functions whose only purpose is the return value (`parse`, `empty()`, `try_lock`);
- factories that return owning handles (`subscribe()` returning a token that unsubscribes
  when dropped).

Where you really mean to ignore: `(void)f();` or `std::ignore = f();` makes it visible.

### 2.7 Errors in an API

Pick one style per API and say it in the header: exceptions, `Result<T>` (C++23
`std::expected`), `std::optional<T>` for "no value" that needs no reason, or error codes with
out-parameters (only at C boundaries). Mixing them inside one API forces every caller to
handle both. Module 09 section 2.7 has a server-wide policy.

### 2.8 Documentation that earns its place

The code already says *what* it does. The header comment should say what the code cannot:

- **Preconditions** (`key` non-empty, at most 256 bytes) and what happens if violated.
- **Postconditions and guarantees** (after `commit()` returns true, the change is durable).
- **Ownership and lifetime** (the returned view is valid until the next `put`).
- **Thread safety** (safe to call concurrently / must be externally synchronised).
- **Complexity** when it is not obvious (O(n) sweep; amortised O(1)).
- **Errors** (which codes, which exceptions).
- **One usage example** for anything non-trivial.

### 2.9 Versioning, API and ABI

- **API compatibility:** old *source* still compiles. **ABI compatibility:** old *binaries*
  still link and run against the new library.
- **ABI breaks** (old binaries crash or misbehave): adding or reordering data members (size
  and offsets change), adding a virtual function in the middle (vtable slots shift), changing
  a function's parameter types (mangled name changes), changing an inline function or
  template that old binaries compiled into themselves, changing an enum's underlying type.
- **Tools:** pimpl keeps data members out of the header, so the class's size never changes
  (`code/m05_pimpl.cpp`). Inline namespaces version symbols: `namespace api { inline
  namespace v2 { ... } }` lets code compiled against `v1` keep its old symbols while new
  code gets `v2` by default (`code/m10_api_quality.cpp`). `[[deprecated("use X")]]` warns
  callers a release before removal.
- **Semantic versioning:** MAJOR for breaking changes, MINOR for compatible additions, PATCH
  for fixes. Applies to wire protocols too: add fields that old readers ignore; never change
  the meaning of an existing field.

### 2.10 Seams, fakes and mocks

A **seam** (term from Michael Feathers, *Working Effectively with Legacy Code*) is a place
where you can change behaviour without editing the code there. In C++:

- **Object seam:** an interface parameter (`Ledger&`, `Clock&`). The most common.
- **Template seam:** a template parameter (`Cache<Clock>`). Zero cost, compile-time.
- **Link seam:** link a different `.o` that defines the same function (common for C code;
  swap `db_query()` with a test version at link time).

Test doubles, named precisely:

| Double | What it is | Assert on |
|---|---|---|
| Stub | returns canned answers | the code under test's output |
| Fake | a working, simplified implementation (in-memory ledger) | resulting state |
| Mock | records calls and checks them against expectations | the interactions |
| Spy | a real or fake object that also records calls | both |

**Prefer fakes.** A test that asserts "the balance moved from A to B" survives refactoring;
a test that asserts "`debit` was called before `credit` exactly once" breaks when you merge
them into one `post` call, even though behaviour is unchanged. Use mocks for effects that
have no state to inspect: an SMS sent, an audit event emitted, a metric incremented.

### 2.11 Code review checklist

Read the change twice: once for design, once for detail.

**Design**
- Does it belong in this layer and module? Does it add a dependency in the wrong direction?
- Is the public API minimal? Could a caller misuse it (swapped args, ignored result, bool flags)?
- Who owns every new object? Any `shared_ptr` that could be `unique_ptr`?
- Is there a simpler design (a function instead of a class, a value instead of a pointer)?

**Correctness**
- Error paths: is every failure handled or propagated, with its meaning intact?
- Resource cleanup on every path: RAII or a leak on early return?
- Lifetimes: references or views stored beyond the life of what they refer to?
- Concurrency: shared data protected? Locks held across callbacks or I/O? Lock order?
- Integer issues: overflow, signed/unsigned comparison, money in floating point?
- Input from outside: lengths checked before copying, limits enforced?

**Quality**
- Tests for the new behaviour, including a failure case? Do they test behaviour, not
  implementation?
- `const`, `noexcept` on moves, `[[nodiscard]]` on results, `explicit` on single-arg constructors?
- Names say what, comments say why. Dead code and commented-out code removed.
- Logs: correct level, correlation id, no personal data.

### 2.12 Refactoring safely

**Refactoring** is changing structure without changing behaviour. The safety rules:

1. **Tests first.** If there are none, write characterization tests (Module 09, 2.11).
2. **Small steps,** each compiling and passing. Rename, extract function, introduce
   parameter, move function, extract interface: each one alone.
3. **Separate commits for refactoring and for behaviour change.** A reviewer can verify a
   pure refactor mechanically; mixing in a fix hides the fix.
4. **Let the compiler find the call sites.** Change a parameter from `int` to `AccountId`
   and fix every error the compiler reports; that is a complete list, unlike a text search.
5. **Run sanitizers** (`-fsanitize=address,undefined`, and `thread` for concurrent code) on
   the test suite after each structural change; refactors often change lifetimes.
6. **Keep commits bisectable.** If each commit builds and passes, `git bisect` finds a
   regression in minutes.

### 2.13 The strangler fig pattern

Martin Fowler named it after a vine that grows around a tree until it replaces it. Instead
of rewriting a legacy system in one big release, you:

1. **Put a router in front** of the legacy code: a facade through which every request goes.
   At first it sends everything to the legacy path.
2. **Build the new implementation** for one slice (one message type, one product).
3. **Shadow:** send the slice to both, return the legacy answer, compare and count
   mismatches. Only safe for reads or against a scratch database.
4. **Switch** the slice to the new path behind a flag that can be flipped back in seconds.
5. **Repeat** slice by slice. **Delete** the legacy code for each slice once stable.

**Branch by abstraction** is the same idea inside one codebase: introduce an interface over
the old implementation, write the new implementation behind the same interface, switch
callers by configuration, delete the old one.

Why it beats the big rewrite: every step is small and reversible, the business keeps
running, and you learn the hidden behaviour of the old system one slice at a time instead of
all at once on go-live night.

## 3. Code

`code/m10_api_quality.cpp` builds a small funds-transfer service that uses every technique in
this module: `Strong<Tag, Rep>` and a `Paise` type with only meaningful arithmetic,
`static_assert`s proving no implicit conversions, an `Ifsc` type that can only be created by
`parse()`, a `[[nodiscard]]` `TransferResult`, a named-field `TransferRequest`, an enum
`Posting::DryRun` instead of a bool, three seams (`Clock`, `Ledger`, `Notifier`), a fake
ledger and a mock notifier, inline-namespace versioning, and a `StranglerRouter` with legacy,
new and shadow routes.

The test shows the difference between a fake and a mock in two lines:

```cpp
TransferResult r = svc.transfer({a, b, Paise(2'500), "rent"});
assert(*ledger.balance(a) == Paise(7'500));            // FAKE: assert resulting state
assert(notes.calls.size() == 1);                       // MOCK: assert the interaction
```

The strangler router's shadow mode:

```cpp
std::string old_answer = legacy_(product, payload);
if (modern_(product, payload) != old_answer) ++mismatches_;   // compare, never serve
return old_answer;
```

## 4. Under the hood (senior-level)

**Strong types cost nothing.** A class holding one `uint64_t`, with a trivial copy
constructor and trivial destructor, is passed in a register under the System V x86-64 ABI,
exactly like the raw integer. The `static_assert(sizeof(AccountId) == sizeof(uint64_t))` in
the code file checks the size; compile with `-O2 -S` and the generated code for a function
taking `AccountId` is identical to one taking `uint64_t`. A non-trivial destructor or copy
constructor changes this: the object is then passed by hidden reference in memory.

**`const` rarely helps the optimiser.** `const_cast` away from a reference to a non-const
object is legal, so the compiler cannot assume memory behind a `const T&` does not change
across a call to unknown code. Its value is for humans and for the type checker. The one
place it does help: a `const` object defined as `const` (not just viewed through a const
reference) may be placed in read-only memory and folded.

**`noexcept` and code generation.** GCC uses table-based (zero-cost) exceptions: no
instructions on the normal path, unwind tables consulted only when something throws. A call
to a `noexcept` function lets the *caller* omit cleanup landing pads for that call; a
`noexcept` function that does throw goes straight to `std::terminate` without unwinding
further. The big real-world effect is not in the function itself but in library decisions:
`std::vector` uses `std::move_if_noexcept` during reallocation, and `std::is_nothrow_*`
traits switch algorithms.

**`[[nodiscard]]` is purely a diagnostic.** It changes no code generation and no ABI. It can
be added to an existing API without breaking binaries, but may break builds that use
`-Werror`, so add it in a minor release with a note.

**What an inline namespace does to symbols.** `api::v2::fee(Paise)` mangles as something like
`_ZN3api2v23feeE5Paise`. A binary compiled against the old header that called `api::fee`
when `v1` was the inline namespace recorded `api::v1::fee`, which the new library still
exports. Two incompatible versions can therefore be linked into one program without an ODR
collision. libstdc++ uses exactly this (`std::__cxx11::basic_string`) for its C++11 ABI
change.

**Why ABI breaks are silent.** The linker matches names, not layouts. If you add a member to
a class whose objects are allocated by an old binary, the old binary allocates the old size
and the new library writes past the end. No link error, just heap corruption hours later.
This is why shared libraries with C++ classes in their public headers need pimpl or a strict
"never change layout" rule, and why C APIs over C++ internals remain popular for plugins.

## 5. In production

**The ignored error.** `db_exec(conn, "UPDATE ...");` with the return value ignored, in C,
for years, until the table was locked by a long batch and every update silently failed for
an hour. `[[nodiscard]]` on the C++ wrapper's result type turns this whole class of bug into
a build failure. When you wrap a legacy C API (Module 05 adapter), put `[[nodiscard]]` on
everything that can fail.

**Boolean traps in banking code.** `calculate_interest(amount, rate, true, false)`; the
third argument was "compound", the fourth "leap-year aware", and a refactor swapped them in
one caller. Enums would have made the swap a compile error.

**Mocks that froze the design.** A service with 300 tests, each mocking the repository with
exact call sequences. Every attempt to batch two queries into one broke 80 tests that were
not testing anything the business cared about. The team rewrote them against an in-memory
fake repository; the next refactor broke none.

**The rewrite that never shipped.** The common legacy story: a two-year "rewrite in modern
C++" runs in parallel with the old system, which keeps getting features, so the rewrite
never catches up. The strangler approach ships value per slice and makes the remaining
legacy smaller every month. In an interview, being able to explain why you would *not* do a
big-bang rewrite is a senior signal.

**Shadow traffic with side effects.** Shadowing a write path against the production database
executes the write twice. Shadow only reads, or point the new path at a copy, or make the
new path compute a decision without committing it and compare decisions.

## 6. Try it

1. **(easy)** Add a `BranchId` strong type to `m10_api_quality.cpp` and a function that takes
   `(AccountId, BranchId)`. Write the call with the arguments swapped and confirm it does not
   compile. Done when you can paste the compiler error.
2. **(easy)** Remove `[[nodiscard]]` from `TransferResult`, call `svc.transfer(...)` without
   using the result, rebuild, then restore it. Done when you have seen the warning become an
   error under `-Werror`.
3. **(medium)** Write a `ChaosLedger` fake that fails `post` on demand, and test that
   `TransferService` returns an error and does not notify. Done when the test passes and the
   mock shows zero calls.
4. **(medium)** Give `Strong` a `std::hash` specialisation and use `AccountId` as an
   `unordered_map` key. Done when it compiles without exposing `get()` at the call site.
5. **(hard)** Implement a `ShadowAsync` route in `StranglerRouter` that runs the new handler
   on a separate thread with a timeout, so a slow new implementation never delays the
   response. Done when a test with a sleeping new handler still returns within 5 ms.
6. **(hard)** Pick a function from your work codebase (or `legacy::process_loan_request` in
   `m09_layered_server.cpp`) and write a review using the checklist in 2.11. Done when you
   have at least one finding in each of the three groups.

## 7. Interview questions

**Q: What makes an API hard to misuse?**
A: Types that encode the rules: strong types for ids and money, enums instead of booleans,
named-field structs for many parameters, factory functions that validate, RAII for paired
operations, `[[nodiscard]]` on results, and `= delete` for dangerous conversions. The goal is
that the wrong call does not compile, instead of being caught in review.

**Q: Why should move constructors be noexcept?**
A: `std::vector` reallocates using `std::move_if_noexcept`: if the move constructor might
throw, it copies instead to keep the strong exception guarantee. A missing `noexcept` turns
every reallocation into a full copy, which is a silent performance bug.

**Q: What does const on a member function promise, and how do mutexes fit in?**
A: That observable state does not change, and by standard-library convention that it is safe
to call concurrently. A mutex or cache inside it is marked `mutable` because locking does not
change observable state; the cache must itself be thread-safe to keep the concurrency promise.

**Q: What is the difference between API and ABI compatibility?**
A: API compatibility means old source compiles; ABI compatibility means old binaries still
work with the new library. Adding a data member or a virtual function keeps the API but
breaks the ABI because sizes, offsets and vtable slots change. Pimpl and inline namespaces
are the main C++ tools for keeping ABI stable.

**Q: Fake or mock: which do you prefer and why?**
A: Fakes, for most dependencies: tests assert on resulting state, so they survive
refactoring. Mocks verify interactions and are right only when the interaction is the
observable effect (a notification, an audit event). Over-mocking freezes the implementation.

**Q: How do you refactor code that has no tests?**
A: Write characterization tests that capture current behaviour, then make the smallest
structural changes to create seams, extracting functions and interfaces, each step covered by
those tests. Separate refactoring commits from behaviour changes so each can be reviewed and
reverted independently.

**Q: Explain the strangler fig pattern.**
A: Put a routing facade in front of the legacy system, build the replacement slice by slice
behind it, shadow-compare where safe, switch each slice with a reversible flag, and delete
legacy code as slices move. It avoids the big-bang rewrite, whose risk is that it never
catches up with the moving old system.

## 8. Recap

- Easy to use correctly, hard to use incorrectly. Minimal, consistent, obvious at the call site.
- Ownership in the signature: reference/pointer borrow, `unique_ptr` sink, `shared_ptr` rarely.
- Strong types, enums for options, named-field requests, parse-don't-validate, RAII pairs.
- `const` everywhere it is true; `mutable` for locks and caches; const means concurrently callable.
- `noexcept` on moves and swaps, or vectors copy.
- `[[nodiscard]]` on result types and value-only functions.
- Document preconditions, ownership, thread safety, complexity, errors.
- ABI breaks are silent; pimpl and inline namespaces protect binaries.
- Seams: object, template, link. Prefer fakes; mock only effects.
- Refactor in small, tested, separate commits. Replace legacy with a strangler, not a rewrite.

