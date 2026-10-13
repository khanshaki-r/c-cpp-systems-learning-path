# 10. Error handling: exceptions, optional, variant, error_code

**Time:** ~3 hours   **Prerequisites:** 03, 04

## 1. In plain words

In C you return an `int`, set `errno`, or hand back `NULL`, and every caller must check. Forgetting a
check is silent. C++ gives you four tools and you will use all of them: **exceptions** for failures
that the immediate caller cannot handle and that should unwind to a recovery point; **`std::optional`**
for "there might be no value" (a lookup miss); **`std::variant`** / a result type for "either a value
or a structured error" that the caller must handle right here; and **`std::error_code`** for carrying
C-style numeric errors (errno, library codes) without allocation or unwinding.

The skill is not learning the syntax. It is deciding, per function, *which kind of failure* it has
and picking the matching tool, then making the chosen behaviour a policy for the whole server rather
than a per-function mood.

Analogy: `optional` is an empty mailbox (normal, check it). `error_code` is a form letter saying why
(cheap, routine). An exception is the fire alarm: everybody drops what they are doing and goes to the
designated exit. You do not pull the fire alarm because the mailbox is empty.

## 2. The real thing

### 2.1 Exceptions

```cpp
struct ParseError : std::runtime_error {
    int line;
    ParseError(const std::string& what, int l) : std::runtime_error(what), line(l) {}
};

try {
    parse(input);
} catch (const ParseError& e) {      // catch by const reference: no slicing, no copy
    log(e.line, e.what());
} catch (const std::exception& e) {  // most general LAST
    log(e.what());
}
```

- Throw by value, catch by `const&`. Derive from `std::exception` (usually `std::runtime_error` or
  `std::logic_error`) so generic handlers can log `what()`.
- `throw;` inside a handler rethrows the current exception without copying.
- An exception escaping `main` or a thread function calls `std::terminate`. Wrap every thread body in
  a `try`/`catch` that logs and decides.
- Exceptions are for errors the caller cannot reasonably handle at the call site: resource exhaustion,
  broken invariants, I/O failures deep in a call chain, constructor failures. Not for expected
  outcomes (key not found, EOF, invalid user input in a parser's normal operation).

### 2.2 Exception safety guarantees

Every function you write provides one of these, and your API documentation should say which:

| Guarantee | Meaning | How to achieve |
|---|---|---|
| **Nothrow** (`noexcept`) | never throws | only call nothrow operations; swap, move, destructors, integer math |
| **Strong** | if it throws, state is exactly as before (transactional) | do all throwing work on the side, then commit with nothrow operations (copy-and-swap) |
| **Basic** | if it throws, state is valid but unspecified; no leaks | RAII for every resource |
| **None** | may leak or corrupt | unacceptable |

RAII (module 03) gives the basic guarantee almost for free. The strong guarantee costs a copy or a
careful ordering: "throwing operations first, committing operations last". The standard library
documents its guarantees: `vector::push_back` is strong; `vector::insert` of a range is basic
(unless the element type's move is `noexcept`, when it becomes strong).

### 2.3 `noexcept`

A promise that the function does not throw; if it does, `std::terminate` is called immediately (no
unwinding). Mark: destructors (implicit), move constructors and move assignment (module 04: needed
for `vector` to move), `swap`, simple getters, and anything you want callers to be able to rely on.
Do not mark functions that allocate or call unknown code. `noexcept(expr)` queries whether an
expression can throw, so templates can propagate the property.

### 2.4 `std::optional<T>`

A `T` or nothing, stored inline (no heap). `has_value()`, `*opt`, `opt->member`, `value()` (throws
`bad_optional_access` if empty), `value_or(default)`. Use it for lookups and parsers where "absent" is
a normal result. `sizeof(optional<T>)` is `sizeof(T)` plus a `bool` plus padding.

### 2.5 `std::variant<Ts...>` and `std::visit`

A type-safe tagged union: holds exactly one of the alternatives, knows which (`index()`), and
`std::visit(visitor, v)` calls the visitor's overload for the held type, checked at compile time
(missing an alternative is an error). A `Result = std::variant<Value, Error>` type forces the caller
to handle both. C++23 adds `std::expected<T, E>` for exactly this; in C++17 either write a small
`Expected` class or use `variant`.

### 2.6 `std::error_code` and `std::error_condition`

A `(int value, const error_category&)` pair, 16 bytes, trivially copyable, no allocation. The category
supplies `message()` and maps codes to portable `std::errc` conditions (so `ec == std::errc::timed_out`
works whether the code came from errno or from your own category). `std::system_error` is the
exception that carries an `error_code`, bridging the two worlds: throw it from a constructor when
`socket()` fails, return an `error_code` from a hot-path `send()`.

Define your own category for a library (`class KvCategory : public std::error_category`) so callers
can compare against your enum without string matching.

### 2.7 An error-handling policy for a server

1. **Programming errors** (null where non-null promised, index out of range, broken invariant):
   `assert` in debug, and in release either `std::terminate` via a checked helper or a logged exception.
   Do not try to continue; the state is unknown.
2. **Resource failures** (out of memory, cannot open listening socket, config file missing):
   exceptions. Caught at the top of the component (connection handler, startup), logged, and the
   component is torn down and restarted or the process exits cleanly.
3. **Expected operational errors** (client sent garbage, key not found, peer closed connection,
   `EAGAIN`): return values. `optional` for absent, `error_code` or a result variant for "why".
   These are the normal paths of a server and must not unwind.
4. **Boundaries**: C callbacks, thread entry points, destructors, `noexcept` functions must never let
   an exception escape. `catch (...)` at those boundaries, log, convert.
5. **Logging**: log once, at the point where the error is handled, with context. Do not log at every
   level of the stack; that is what the exception's message and the error_code carry up for you.

## 3. Code

`code/m10_exceptions_safety.cpp` demonstrates the guarantees and unwinding:

```cpp
void add_basic(long long amount)  { total_ += amount; entries_.push_back(amount); }  // basic: inconsistent on throw
void add_strong(long long amount) { entries_.push_back(amount); total_ += amount; }  // strong: throw-first, commit-last

void replace_all(const std::vector<long long>& fresh) {
    std::vector<long long> copy(fresh);   // may throw; *this untouched
    long long sum = 0; for (long long v : copy) sum += v;
    entries_.swap(copy);                  // noexcept commit
    total_ = sum;
}

long long parse_amount(const std::string& s, int line) {
    try { /* std::stoll ... */ }
    catch (const std::invalid_argument&) { throw ParseError("not a number: '" + s + "'", line); }   // translate
    catch (const std::out_of_range&)     { throw ParseError("out of range: '" + s + "'", line); }
}

struct Guard { const char* name; ~Guard() { std::cout << "  unwinding through " << name << '\n'; } };
void level3(const std::string& s) { Guard g{"level3"}; parse_amount(s, 3); }   // prints as the stack unwinds
```

`code/m10_optional_variant_error_code.cpp` shows the non-throwing tools:

```cpp
std::optional<int> parse_port(std::string_view s);             // nullopt for "", "80a", "99999"
std::error_code open_device(std::string_view name);            // {} on success; errno wrapped on failure
if (ec == std::errc::device_or_resource_busy) retry();         // portable comparison

using Result = std::variant<long long, Error>;
template <class... Fs> struct overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> overloaded(Fs...) -> overloaded<Fs...>;

std::visit(overloaded{
    [](long long bal)   { std::cout << "balance=" << bal << '\n'; },
    [](const Error& e)  { std::cout << "error: " << e.code.message() << " (" << e.context << ")\n"; },
}, lookup_balance(db, acct));

using Message = std::variant<Get, Put, Del>;                   // a protocol message without a hand-written tag
if (auto* g = std::get_if<Get>(&msg)) use(g->key);             // null if the wrong alternative
```

## 4. Under the hood (senior-level)

**Throw cost, measured in mechanism.** `throw X{}` calls `__cxa_allocate_exception` (heap, with an
emergency pool for `bad_alloc`), copy-constructs `X` there, then `__cxa_throw`, which runs the two-phase
unwinder from libgcc (module 03 section 4). Phase 1 walks frames reading `.eh_frame` and calling the
personality routine to find a handler; phase 2 walks again running landing pads. Each frame costs a
lookup in the unwind tables (binary search over FDEs, cached by `dl_iterate_phdr`, which takes a
lock in older glibc, a real contention point in multithreaded servers that throw a lot). Typical: 1 to
10 microseconds per throw, proportional to stack depth. A `return std::error_code` is a register move.
At 1% throw rate on a 100k req/s server that is still fine; at 50% it is not.

**No cost when not thrown.** The normal path has no checks. `try` blocks are free. The tables take
space in the binary (`.eh_frame`, `.gcc_except_table`), typically 5 to 15% of text size.

**`noexcept` and code generation.** A `noexcept` function needs no landing pads for its own cleanup
(the compiler may still emit a call to `std::terminate` guard), and callers can skip generating
cleanup paths around it. In `std::vector`, `std::is_nothrow_move_constructible` picks move over copy.
Violating `noexcept` calls `std::terminate` directly; the stack is *not* unwound (destructors do not
run), which matters for anything that flushes in a destructor.

**`std::optional` layout.** A union of `T` and nothing, plus a `bool engaged`; `sizeof(optional<int>) == 8`,
`sizeof(optional<std::string>) == 40`. No dynamic allocation ever. Returning `optional<T>` by value
is subject to the same elision rules as `T`.

**`std::variant` layout and `visit`.** Storage for the largest alternative plus an index
(`unsigned char` for up to 255 alternatives, aligned up). `std::visit` with one variant generates a
table of function pointers indexed by `index()` (or a switch for small counts in newer libstdc++), so
dispatch is a bounds check and an indirect jump: comparable to a virtual call but with no heap and no
vptr. A variant can become `valueless_by_exception` if a type-changing assignment throws midway; check
`valueless_by_exception()` only if your alternatives have throwing move constructors.

**`std::error_code` is a C-compatible value.** Two words, trivially copyable, returned in registers.
The category is a pointer to a singleton with virtual functions (`message()`, `equivalent()`), so
comparison with an `errc` calls through the category: cheap, but not free; do it on the error path only.

**Exception objects and `what()`.** `std::runtime_error` stores its message in a reference-counted
string (libstdc++ `__cow_string`) so copying the exception does not allocate and `what()` cannot
throw. Your own exception types should avoid members whose copy constructor can throw; the copy
performed by `throw` would then throw inside a throw, terminating the program.

**Thread boundaries.** An exception does not cross threads. `std::thread` with an escaping exception
terminates the whole process. `std::async`/`std::promise` capture it (`std::exception_ptr`) and rethrow
in `get()`; this is how you move an error from a worker to the thread that cares.

## 5. In production

- **Pick per layer, not per mood.** Constructors and startup: throw. Request-handling hot path:
  return `optional`/`error_code`/result. Component tops (connection loop, thread entry): `catch`,
  log with context, recover or exit. Write this down in the project's README and enforce in review.
- **The `catch (...) {}` that swallows** turns an out-of-memory into a silent data loss. If you catch
  broadly, log the `what()` (via `catch (const std::exception&)` first) and re-throw or terminate
  anything you cannot identify.
- **Destructors that can fail** (flush, close with pending writes, commit) need an explicit
  `close()`/`commit()` that can throw, with the destructor as a best-effort fallback that logs.
  `std::fstream` works this way: the destructor closes silently; call `flush()` and check if you care.
- **Error codes and `[[nodiscard]]`.** A function returning `std::error_code` or `bool` should be
  `[[nodiscard]]` (module 13) so the compiler enforces the check your C callers kept forgetting.
- **Map C errors at the boundary.** Wrap `errno` into `std::error_code(errno, std::generic_category())`
  the moment it is produced; downstream code then compares against `std::errc` names instead of
  magic numbers, and `message()` gives the `strerror` text for logs.
- **Banking semantics.** A multi-step balance update needs the strong guarantee: compute everything,
  then commit with nothrow operations, or hold a transaction object whose destructor rolls back unless
  `commit()` was called (the ScopeGuard from module 03). Your PL/pgSQL `BEGIN ... EXCEPTION WHEN` block
  is the same shape.
- **3 AM debugging.** `terminate called after throwing an instance of 'std::system_error' what(): Resource temporarily unavailable`:
  a `noexcept` function or thread body let an exception escape. Core dump shows `__cxa_throw` at the
  top; walk down to the first frame of your code. `catch throw` in gdb breaks at every throw.

## 6. Try it

1. **(easy)** Build `m10_exceptions_safety.cpp`. Swap the two lines in `add_strong` and write a test
   with a custom allocator that throws on the 3rd allocation. Done when you can show `total()`
   disagreeing with the entries after the throw.
2. **(easy)** Write `std::optional<Account> find_account(std::string_view id)` over a `std::vector`
   and use it with `if (auto a = find_account("X"))`, `value_or`, and `->`. Done when there is no
   sentinel value anywhere.
3. **(medium)** Define `enum class KvErr { KeyNotFound = 1, TooLarge, Readonly }`, a
   `KvCategory : std::error_category`, `make_error_code(KvErr)`, and the `is_error_code_enum`
   specialisation. Done when `std::error_code ec = KvErr::TooLarge;` compiles and `ec.message()` prints
   your text.
4. **(medium)** Implement `template <class T, class E> class Expected` with `has_value()`, `value()`,
   `error()`, and a `map(f)` method, built on `std::variant<T, E>`. Done when a chain of three
   `map` calls short-circuits on the first error in a test.
5. **(hard)** Benchmark `parse_port` returning `optional<int>` vs a version that throws on invalid
   input, over 10M inputs where 0%, 1%, 50% are invalid. Done when you have a table and can state where
   the crossover is and why.
6. **(hard)** Write a thread pool worker loop that catches exceptions from tasks, stores them in a
   `std::exception_ptr` inside a per-task result, and rethrows them in the caller's `get()`. Done when
   a task throwing `ParseError` is caught as `ParseError` in the submitting thread with its `line` intact.

## 7. Interview questions

**Q: Exceptions or error codes?**
Exceptions for failures the immediate caller cannot handle and that should unwind to a recovery
point (resource exhaustion, constructor failure, broken invariants); return values (`optional`,
`error_code`, result types) for expected outcomes on normal paths (lookup miss, bad input, EAGAIN).
The decision should be a project-wide policy per layer, not per function.

**Q: Name the exception safety guarantees.**
Nothrow (never throws, `noexcept`), strong (on throw, state unchanged, transactional), basic (on throw,
state valid and nothing leaked), and none. RAII gives basic; "throwing work first, nothrow commit last"
or copy-and-swap gives strong.

**Q: What does `noexcept` do and when should you use it?**
It promises the function will not throw; if one escapes, `std::terminate` is called without unwinding.
Use it on destructors, move operations, `swap`, and simple non-allocating functions. It lets
`std::vector` move instead of copy and lets the compiler drop cleanup paths.

**Q: Why catch by `const&`?**
Catching by value copies the exception (possibly throwing again) and slices derived exceptions down to
the base type, losing information. A const reference binds to the exception object the runtime
allocated, with its full dynamic type.

**Q: What happens if an exception leaves a destructor during unwinding?**
Two exceptions would be in flight; the runtime calls `std::terminate`. Destructors are implicitly
`noexcept`, so the terminate happens at the destructor even outside unwinding. Provide an explicit
`close()` for failures that matter.

**Q: What is `std::optional` for, and what does it cost?**
Representing "a value or nothing" without sentinels or heap allocation: a union of `T` and a `bool`
flag, stored inline. `value()` throws on empty; `*`, `->` and `value_or` do not check.

**Q: How does `std::visit` on a `std::variant` work?**
The variant stores the active alternative's index; `visit` dispatches on it through a generated table
or switch to the visitor's matching overload. The compiler checks that the visitor handles every
alternative, so adding a type to the variant breaks every visitor that forgot it: a feature.

**Q: What is `std::error_code` and when is it better than an exception?**
A 16-byte value: an integer plus a pointer to a category that gives it meaning and a message. No
allocation, no unwinding, trivially returned in registers. Right for hot paths, C API boundaries
(wrap errno immediately), and libraries that must not throw. `std::system_error` carries one when you
do need to throw.

## 8. Recap

- Four tools: exceptions (unwind to a recovery point), `optional` (absent value), `variant`/result
  (value or structured error, handled here), `error_code` (cheap numeric error with a category).
- Throw by value, catch by `const&`, derive from `std::exception`, most specific handler first.
- Guarantees: nothrow, strong, basic, none; RAII gives basic, "throw first, commit last" gives strong.
- `noexcept` on destructors, moves, swap; violating it terminates without unwinding.
- Throwing costs microseconds (two stack walks); not throwing costs nothing.
- `optional` and `variant` are inline, no heap; `visit` is a checked jump table.
- Wrap `errno` into `error_code` at the boundary; compare with `std::errc` names.
- Policy per layer: constructors/startup throw, hot paths return, component tops catch and log once.
- Exceptions never cross thread or C boundaries; use `exception_ptr` or convert.
