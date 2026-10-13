# 09. Designing a Server Codebase

**Time:** ~3.5 hours   **Prerequisites:** 02, 03, 05, 06, 07

## 1. In plain words

Think of a bank branch. The person at the counter takes your form and hands back a receipt.
They do not decide whether you get the loan. The form has a fixed layout, so anyone can read
it. The loan officer reads the form and applies the bank's rules. The records room stores
the files. The counter clerk never walks into the records room, and the loan officer never
cares whether the form came in by hand or by post.

A server codebase works the same way. **Transport** is the counter: it moves bytes in and out
over sockets. **Protocol** is the form layout: it turns bytes into a request value and a
response value back into bytes. **Service** is the loan officer: business rules. **Storage**
is the records room: a database or an in-memory map. Each layer talks only to the one next
to it, and the rules never depend on how the bytes arrived or where the files are kept.

You already work in a server shaped like this, even if nobody drew it. This module gives you
the vocabulary and the rules, so you can explain the design in an interview, lay out a new
repository (tcpkv) properly, and describe how you moved legacy C business logic into a
cleaner layer without breaking a bank's lending flow.

## 2. The real thing

### 2.1 The four layers

| Layer | Owns | Must not know about | Typical types |
|---|---|---|---|
| Transport | sockets, framing at byte level, timeouts, thread pool, TLS | commands, keys, loans | `Socket`, `Listener`, `Connection`, `ThreadPool` |
| Protocol | wire grammar, parsing, encoding, protocol versions, limits on message size | sockets, databases, business rules | `Parser`, `Request` (a `std::variant`), `encode_reply()` |
| Service | business rules, validation, authorisation, workflow, transactions, metrics | sockets, wire format, SQL text | `KvService`, `LoanService`, `WorkflowEngine`, `Rule` |
| Storage | persistence, SQL, connection pools, caches of tables | wire format, sessions | `MemoryStorage`, `PgApplicationRepository` |

Plus two things that are not layers:

- **Common / domain types.** Small value types every layer may use: `Error`, `Result<T>`,
  `AccountId`, `Paise`, `Application`. No I/O, no dependencies.
- **The composition root.** The one place (usually `main`) that creates every concrete
  object and wires them together. It is the only code that knows *all* the concrete types.

### 2.2 Dependency direction

**Dependency** here means "includes the header of" and "links against". The rule:

```
   transport ---> protocol ---> service <--- storage
                                   ^
                                   |
                         common / domain types (everyone may use)

   app/main (composition root) depends on all of them, nothing depends on app.
```

Read the arrow from storage carefully: storage depends **up** on the service, not the other
way round. The service declares the interface it needs (a **port**):

```cpp
namespace service {
class Storage {                       // declared in service/storage_port.hpp
public:
    virtual ~Storage() = default;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual void put(const std::string& key, std::string value) = 0;
    virtual bool erase(const std::string& key) = 0;
};
}
```

and the storage library implements it (an **adapter**). This is the Dependency Inversion
Principle from Module 02 applied at the scale of libraries. The name for the whole shape is
**ports and adapters**, also called hexagonal architecture: business logic in the middle,
I/O on the outside, interfaces between them owned by the middle.

Why it matters:

- You can test every rule with an in-memory fake, no database, in milliseconds.
- You can replace Berkeley DB with YugabyteDB by writing one new adapter. You have done this
  migration; in a ports-and-adapters design it is an adapter swap rather than a rewrite.
- A change in the wire protocol never forces a recompile of storage.

**Enforce it with the build, not with discipline.** In CMake each layer is a library target,
and `target_link_libraries` lists only what the arrow allows:

```cmake
add_library(kv_common   INTERFACE)                       # header-only vocabulary types
add_library(kv_service  src/service/kv_service.cpp)
target_link_libraries(kv_service  PUBLIC kv_common)      # no storage, no transport
add_library(kv_storage  src/storage/memory_storage.cpp)
target_link_libraries(kv_storage  PUBLIC kv_service)     # implements service's port
add_library(kv_protocol src/protocol/parser.cpp)
target_link_libraries(kv_protocol PUBLIC kv_common)
add_library(kv_transport src/transport/socket.cpp src/transport/connection.cpp)
target_link_libraries(kv_transport PUBLIC kv_protocol kv_service)
add_executable(tcpkv_server apps/server_main.cpp)        # the composition root
target_link_libraries(tcpkv_server PRIVATE kv_transport kv_storage)
```

With per-target include directories, `#include "storage/..."` from inside `src/service/`
fails to compile. That is the point.

### 2.3 Interfaces at boundaries, not everywhere

Put a virtual interface (or a template parameter, Module 08) where one of these is true:

1. **It is I/O.** Socket, database, file, clock, random numbers, environment. Anything that
   makes a test slow, flaky or dependent on the machine.
2. **It is a layer boundary.** The service's view of storage. The transport's view of the
   request handler.
3. **There really are two implementations** in production (memory store and RocksDB store),
   or a third is planned.

Do *not* put an interface in front of every class "for testability". A `Parser` with an
`IParser` and one implementation is noise: test the real parser, it is pure. Interfaces are
not free in reading time: every one is a place where "go to definition" stops working.

Runtime cost is rarely the argument. A virtual call costs a few nanoseconds (typical, when
predicted); a `recv()` syscall costs on the order of a microsecond and a database round trip
hundreds of microseconds to milliseconds. One or two virtual calls per request are invisible.
Inside a hot loop over millions of keys, the template form is the right tool.

### 2.4 Directory layout

One directory per layer, one namespace per directory, one CMake target per directory:

```
tcpkv/
  CMakeLists.txt
  include/tcpkv/            public headers (what the client library exposes)
    client.hpp
  src/
    common/   result.hpp error.hpp log.hpp           namespace tcpkv::common
    config/   config.hpp config.cpp                  namespace tcpkv::config
    protocol/ parser.hpp parser.cpp reply.hpp        namespace tcpkv::protocol
    service/  kv_service.hpp kv_service.cpp          namespace tcpkv::service
              storage_port.hpp                       (the interface storage implements)
    storage/  memory_storage.hpp memory_storage.cpp  namespace tcpkv::storage
    transport/fd.hpp socket.hpp connection.hpp server.hpp thread_pool.hpp
  apps/       server_main.cpp  cli_main.cpp          composition roots
  tests/      one test binary per layer, plus an end-to-end test over a real socket
```

Private headers live next to their `.cpp`. Only `include/` is installed. Tests link the
layer they test plus fakes, never the whole server.

### 2.5 Configuration

Rules that save you at 3 AM:

- **Read once, at start-up, into a plain struct.** Validate everything (ranges, unknown keys,
  file paths exist). Fail start-up with a clear message. A server that starts with a typo'd
  key silently ignored is worse than one that refuses to start.
- **Precedence**, lowest to highest: compiled defaults, config file, environment variables,
  command-line flags. Log the effective configuration (minus secrets) at start-up.
- **Immutable after start.** Pass `const Config&` (or the specific sub-struct a component
  needs, so the dependency is visible). Never call `getenv()` in the middle of a request:
  it is a hidden input that a test cannot control, and it is not thread-safe against
  `setenv()`.
- **Hot reload, if needed,** is an RCU-style snapshot: build a new `Config`, validate it,
  publish it with an atomic `shared_ptr` swap; requests take a snapshot at their start
  (`code/m07_rcu_snapshot.cpp`). Only reload what is safe to change live (log level, limits),
  never the port or the thread count.
- **Secrets** (DB passwords) come from a secret store or a file with tight permissions,
  never the command line (visible in `ps`) and never the log.

### 2.6 Logging

- **Inject a logger interface** (or a small set of free functions over a configured sink) so
  tests can capture output. A global `printf` to a file cannot be tested and cannot be
  redirected.
- **Correlation id.** Each request gets an id at the transport layer; every log line for
  that request carries it. Without it, a log from 64 worker threads is unreadable.
- **Levels with meaning.** ERROR: someone must act. WARN: unexpected but handled. INFO: one
  line per significant event (start, stop, config, a connection opened). DEBUG: per request,
  off in production.
- **Structured fields** (`req=7f3a op=SET key_len=12 dur_us=85 result=OK`) can be parsed by
  log tools; prose cannot.
- **Never log** passwords, full account numbers, Aadhaar or PAN numbers, card data, session
  tokens. In banking this is an audit finding, not a style issue. Mask at the logging call.
- **Do not log under a lock you care about,** and do not let a slow disk block request
  threads: use an asynchronous logger with a bounded queue that drops and counts
  (`code/m11_logger.cpp`).

### 2.7 Error policy

Decide once, write it down, apply everywhere. A workable policy for a C++ server:

| Kind of failure | Example | Mechanism |
|---|---|---|
| Programmer bug, invariant broken | index out of range, null where impossible | `assert` in debug; in release log and abort the process (a core dump beats corrupt data) |
| Expected, input-related | bad key, unknown command, rule failed | return a value: `Result<T>` / `std::optional` / error enum |
| Environmental, maybe transient | DB timeout, connection reset | `Result` with a retryable code, or an exception caught at the service boundary |
| Start-up failure | bad config, port in use | exception or error return in `main`, exit non-zero with a message |

And these rules:

- **Translate at every boundary.** Storage turns `PGRES_FATAL_ERROR` into
  `Errc::Unavailable` or `Errc::Internal`; the protocol layer turns `Errc` into wire text in
  **one** function (`encode_error` in the code file). SQL text and file paths never reach a
  client.
- **Never translate to a different meaning.** A timeout is not "not found". If you map it to
  not-found, a client will create a second copy of something that exists.
- **Exceptions never cross** a thread boundary (catch in the worker loop), a C API boundary
  (callbacks registered with a C library), or a destructor (destructors are `noexcept`).
- **Log once,** where the error is handled, not at every layer it passes through.

### 2.8 What makes a good module

- **Small interface, large implementation** ("deep module", the term is from John
  Ousterhout's *A Philosophy of Software Design*). `KvService` has three functions and hides
  validation, limits, metrics and storage. A module whose interface is as complicated as its
  body is not hiding anything.
- **One reason to change** (Module 02, single responsibility). The protocol module changes
  when the wire format changes, nothing else.
- **Owns its data.** Nobody else reaches into its structs. In C you enforced this with
  opaque pointers in headers; in C++ with `private` and, across a library boundary, pimpl
  (`code/m05_pimpl.cpp`).
- **No cycles.** If A needs B and B needs A, one of them contains a third module trying to
  get out, or one direction should be an interface owned by the other.
- **No `utils` dumping ground.** A `utils` directory with string helpers, time helpers and
  half of the business logic becomes the module everything depends on and nothing can change.

### 2.9 Full architecture: tcpkv

The spec is `10_Capstone_Projects/tcpkv/00_spec.md`. Its components map onto the layers:

```
 apps/server_main.cpp  (composition root: parse flags -> Config, build Store, Metrics,
         |              Server; install signal handlers; run)
         v
 +------------------------------ transport ------------------------------------+
 |  FileDescriptor  Socket  Server(acceptor loop, wake pipe)  ThreadPool       |
 |  Connection::serve(): poll -> read -> parser.feed -> execute -> write_all   |
 +-------------------------------|---------------------------------------------+
                                 | Request values (std::variant), Reply values
 +------------------------------ protocol -------------------------------------+
 |  Parser (incremental state machine, limits)   reply encoders                |
 +-------------------------------|---------------------------------------------+
                                 | execute_command(Request) -> Reply
 +------------------------------ service --------------------------------------+
 |  command execution: key validation, TTL semantics, STATS                    |
 |  Metrics (atomics, histogram)          port: Store interface (optional)     |
 +-------------------------------|---------------------------------------------+
                                 |
 +------------------------------ storage --------------------------------------+
 |  Store: unordered_map + shared_mutex, lazy expiry + periodic purge          |
 +-----------------------------------------------------------------------------+
 common: log.hpp, limits, error types
```

Honest note for the interview: tcpkv is small enough that the service layer is thin
(`execute_command` plus metrics), and the spec's `Store` is concrete rather than behind an
interface. That is a defensible choice: there is one store implementation and the store is
already fast to test directly. Say so, and say what you would add if a second storage engine
arrived (extract the port, keep `Store` as the first adapter).

### 2.10 Full architecture: a loan-origination application server

This is the shape of the server you maintain, drawn cleanly. Requests are RPC calls over TCP
from branch clients; the backend is a distributed SQL database.

```
 Branch client (desktop app)         RPC over TCP, length-prefixed binary frames
        |
 +------v--------------------------- transport ---------------------------------+
 | Listener -> Connection (one per client socket)                               |
 | Frame reader (length prefix, max frame size, read timeout)                   |
 | Request pool: bounded queue + worker threads (the "pooled request model")    |
 +------|-----------------------------------------------------------------------+
        | raw frame
 +------v--------------------------- protocol ----------------------------------+
 | Decode header (msg type, version, session token, request id) and body        |
 | Dispatch table: msg type -> handler (versioned: v1 and v2 decoders co-exist) |
 +------|-----------------------------------------------------------------------+
        | typed request: CreateApplication, UpdateApplicant, SubmitApplication,
        | ApproveApplication, Disburse, FetchMaster ...
 +------v------------------- cross-cutting middleware --------------------------+
 | authenticate session -> authorise (role x action x branch) -> rate limit     |
 | -> audit "request received" -> start unit of work (DB transaction)           |
 +------|-----------------------------------------------------------------------+
 +------v--------------------------- service -----------------------------------+
 | ApplicationService  KycService  EligibilityService  SanctionService           |
 | WorkflowEngine (state machine + rules, code/m11_workflow_engine.cpp)          |
 | Rules: FOIR, age, product limits, maker-checker, deviation approvals          |
 | Ports: ApplicationRepository, CustomerRepository, MasterData, Outbox, Clock   |
 +------|-----------------------------------------------------------------------+
        |
 +------v--------------------------- storage (adapters) ------------------------+
 | Pg/Yugabyte repositories (libpq, prepared statements, stored procedures)     |
 | Connection pool (one connection per worker for the unit of work)             |
 | MasterDataCache: in-memory copy of master tables, refreshed by snapshot swap  |
 | Outbox table: side effects (SMS, CBS posting) committed with the state change |
 +-----------------------------------------------------------------------------+
```

Points to be able to defend:

- **Unit of work per request.** One DB transaction per request: state change, audit row and
  outbox rows commit together or not at all. Side effects (SMS, posting to core banking)
  happen afterwards from the outbox, retried until done, so a crash between "approved" and
  "SMS sent" never loses the SMS or sends it for a rolled-back approval.
- **Where rules live.** Validation that needs many rows (limits across a customer's
  existing loans) is cheaper close to the data (a stored procedure, one round trip).
  Validation of a single request's fields is cheaper and more testable in the service.
  Section 2.11 has the trade-off table.
- **Master-data cache.** Product, branch and rate tables change rarely and are read on every
  request. An immutable snapshot (`shared_ptr<const MasterData>`) refreshed every few
  minutes removes repeated joins from the hot path; readers never lock. Stale-by-minutes is
  acceptable for a product list and not for a balance, which is why balances are never cached.
- **Threading.** The transport's worker count bounds concurrent DB transactions and so must
  be no larger than the connection pool. A request holding a DB connection while waiting for
  another lock is how pools deadlock.
- **Versioning.** Branch clients upgrade slowly. The protocol layer keeps old message
  versions decodable and converts them into the current typed request, so the service has
  one version of every operation.

### 2.11 Refactoring legacy C business logic into modules

Your resume line: "Re-engineered legacy C business logic into a modular PL/pgSQL
stored-procedure layer that centralises validation, authorisation and workflow rules,
cutting application-to-database round trips per transaction." This section gives you the
method and the vocabulary to describe it.

**The starting point** in most legacy servers: one handler function per message type, each
hundreds of lines, mixing parsing, validation, several SQL calls, business rules, and
response formatting, with globals for shared state. Rules are duplicated across handlers and
drift apart. Every rule check that needs data is a separate round trip.

**The method (works whether the target is C++ modules or stored procedures):**

1. **Inventory.** List every entry point (message types, batch jobs) and, for each, the rules
   it applies and the tables it touches. A spreadsheet is fine. You will find the same rule
   implemented three slightly different ways; decide which one is correct with the business.
2. **Characterization tests.** Before changing anything, record what the old code does:
   feed it real (masked) inputs and capture outputs, including the wrong ones. These tests
   pin current behaviour, not desired behaviour. `code/m09_layered_server.cpp` part 2 does
   this: old and new must answer byte for byte the same.
3. **Create seams.** Extract the SQL calls behind a function or interface
   (`CustomerRepository`) so the rules can run without the database. Extract each rule into a
   pure function (`decide(request, customer)`). Each extraction is small and covered by step 2.
4. **Move the logic to its new home.** In your case, rules that need data moved into
   procedures: `fn_validate_application(app_id)` performs all checks in one call and returns
   every failure, instead of the C code issuing a query per check. Authorisation (role,
   branch, amount band) and workflow transitions (allowed state changes) became table-driven
   in the database, so every caller enforces the same rule.
5. **Strangle.** Route one message type (or one product) to the new path behind a switch;
   run old and new side by side in shadow mode where possible; compare; then switch over and
   delete the old code. Module 10 section 2.10 covers the strangler pattern in detail.
6. **Delete.** Refactoring that leaves both versions alive forever doubles maintenance.

**Business logic in the application server vs in stored procedures:**

| Concern | In the C, C++ application server | In PL/pgSQL procedures |
|---|---|---|
| Round trips for data-heavy checks | one per query unless batched | one call for the whole check |
| Unit testing | easy with fakes, fast | needs a database (pgTAP or tests in a container) |
| Consistency across callers | only if every caller uses the same module | enforced: every client calls the same procedure |
| Scaling | add app servers (stateless) | uses database CPU, the hardest tier to scale |
| Deployment and versioning | binary release | schema migration; must stay compatible with old app versions |
| Debugging | debugger, core dumps | `RAISE NOTICE`, logs, `EXPLAIN` |
| Language | full C++, types, libraries | SQL-centric; weak for complex algorithms |

The honest senior answer: data-intensive validation and invariants that every caller must
respect belong close to the data; orchestration, integration with other systems and complex
computation belong in the application server. Your project moved the first kind into
procedures, which is why it cut round trips.

**How to tell it in an interview** (situation, task, action, result): the situation (rules
spread across C handlers, duplicated, several round trips per transaction), the task
(centralise without breaking a live lending flow), the actions (inventory, characterization
tests, procedure per business operation returning all failures, table-driven authorisation
and workflow, switch-over per message type), the result (fewer round trips per transaction,
one place to change a rule). Use your real numbers for round trips before and after; do not
invent them.

## 3. Code

`code/m09_layered_server.cpp` puts a complete layered server in one file so the shape is
visible at once: `common` (Result, Error, Logger interface), `config` (validated, immutable),
`service` (KvService and the `Storage` port it owns), `storage` (MemoryStorage implementing
the port), `protocol` (parse to a `std::variant`, one error-mapping function), `transport`
(a scripted fake connection), and `app::serve` gluing them with `std::visit`. `main` is the
composition root. Part 2 is a legacy C loan handler and its modular replacement, checked by
a characterization test.

The composition root, which is the only code that knows every concrete type:

```cpp
int main() {
    auto cfg = config::parse({{"port", "7400"}, {"max_value_bytes", "8"}});
    assert(cfg.ok());
    storage::MemoryStorage store;                       // concrete: only main sees it
    app::CapturingLogger log;
    service::KvService svc(store, log, cfg.value());    // depends on the PORT, not on MemoryStorage
    transport::ScriptedConnection conn({"PING", "SET a hello", "GET a"});
    app::serve(conn, svc);
}
```

The service validates and delegates; it never sees sockets or SQL:

```cpp
common::Result<common::Unit> set(const std::string& key, std::string value) {
    if (auto e = check_key(key)) return *e;
    if (value.size() > cfg_.max_value_bytes)
        return common::Error{common::Errc::TooLarge, "value too large"};
    store_.put(key, std::move(value));
    return common::Unit{};
}
```

The characterization test (the most useful ten lines in any legacy refactor):

```cpp
for (const char* in : inputs) {
    char buf[64];
    legacy::process_loan_request(in, buf, sizeof buf);   // old code, untouched
    assert(svc2.handle(in) == std::string(buf));         // new code must agree exactly
}
```

## 4. Under the hood (senior-level)

**What a layer boundary costs at runtime.** A virtual call through a port is a load of the
vptr, a load of the slot and an indirect call; when the branch predictor has seen it before,
a few nanoseconds (typical). It blocks inlining across the boundary, which matters in a
tight loop and not at all once per request. Marking adapter classes `final` lets the
compiler devirtualise calls made through the concrete type, and with LTO (`-flto`) GCC can
sometimes devirtualise through the interface when it sees only one implementation in the
whole program.

**What layers cost at build time.** Headers are the real coupling in C++. A service header
that includes `<libpq-fe.h>` makes every file that includes the service recompile when libpq
changes, and leaks C symbols into every translation unit. Ports keep heavy includes inside
`.cpp` files of the adapter. Forward declarations (`class Socket;`) and pimpl cut the
include graph further. Tools: `-H` prints the include tree; `include-what-you-use` finds
unnecessary includes; `ninja -d stats` and `-ftime-trace` (Clang) show where compile time
goes.

**Static libraries and link order.** With GNU ld, a static library is searched once, at its
position on the command line, for symbols needed so far. If `kv_transport` uses `kv_service`,
then `kv_service` must come after it. CMake computes the order from
`target_link_libraries`, which is another reason to declare the graph accurately. A cycle
between static libraries needs `--start-group`; treat needing it as a design bug.

**ODR across modules.** Two modules defining `struct Config` in the global namespace with
different members is a One Definition Rule violation: no diagnostic required, and in
practice one module reads the other's layout and gets garbage. Namespaces per module
(`tcpkv::config::Config`) make this impossible. The same holds for two inline functions or
templates with the same name and different bodies.

**Symbol visibility.** On Linux, every non-static function in a shared library is exported
by default. Building with `-fvisibility=hidden` and exporting only the public API (an
`EXPORT` macro on the classes in `include/`) shrinks the dynamic symbol table, speeds up
loading, lets the compiler optimise internal calls, and makes the ABI surface explicit.
This is the Linux equivalent of `__declspec(dllexport)` that you know from MSVC.

**Thread ownership by layer.** In the loan server, transport threads own connections,
workers own one request each, and the storage pool owns connections to the database. A
layer should not create threads that call upward into another layer without a documented
contract, because then the caller's locks are held on a thread it did not expect. The
outbox worker and the cache refresher are explicit, named threads owned by the composition
root.

## 5. In production

**Layer skipping.** Under deadline pressure someone calls the database from the protocol
decoder "just to look up the branch code". Now the decoder cannot be tested without a
database and a slow query stalls frame decoding. The build-enforced dependency graph (2.2)
turns this into a compile error instead of a code-review argument.

**Pass-through layers.** The opposite failure: a service layer where every function is
`return repo_.same_function(args);`. That is a layer with no responsibility. Either it gains
rules, or delete it and let transport talk to a repository interface directly. Layers are
for separating reasons to change, not for symmetry on a diagram.

**Error meaning lost in translation.** A war story shape common to lending systems: a read
from the application table times out under load, the storage layer returns "not found",
the service decides the application does not exist and the client is told to create it. The
branch user clicks again and now there are two applications for one customer. The fix is an
error policy with a distinct `Unavailable` code and a rule that only a definite answer from
the database becomes `NotFound`.

**Config read lazily.** A limit read with `getenv` inside a request handler worked for years
until a deployment script changed the environment of only some processes. Half the servers
used the old limit. Reading config once and logging the effective values at start-up makes
this visible in the first log line.

**Logging that takes the server down.** A synchronous logger writing to a network share;
the share slows down; every worker blocks in `fwrite` while holding a request; the thread
pool is exhausted; the load balancer marks the server dead. Async logging with a bounded,
dropping queue (and a counter for drops) turns a disk problem into a gap in the logs instead
of an outage.

**What to check first at 3 AM.** Find the correlation id of one failing request. Grep it
across all logs. The layer where the request's log lines stop is the layer to investigate.
This only works if every layer logs with that id, which is a design decision made long
before the incident.

## 6. Try it

1. **(easy)** In `m09_layered_server.cpp`, add an `EXISTS key` command. Touch only the
   protocol (`parse`, a new `Exists` struct) and `app::serve`; the service gets one new
   function. Done when the scripted test covers it and no storage code changed.
2. **(easy)** Add a `RequestIdLogger` decorator that prefixes each line with a request id passed
   through `serve`. Done when every log line of one request shares an id.
3. **(medium)** Replace `MemoryStorage` with a `FlakyStorage` decorator (Module 05) that
   fails every third call with an `Unavailable` result. Change the port so storage can
   report failure, and make sure the wire reply is `-ERR try again`, never `$-1`. Done when a
   test proves a storage failure never looks like "not found".
4. **(medium)** Draw the CMake target graph for tcpkv (one target per layer) and write the
   `CMakeLists.txt` snippet. Done when a deliberate `#include "storage/..."` from a service
   file fails to compile.
5. **(hard)** Take one real handler from your work (or invent an 80-line one in the legacy
   style), write characterization tests for it with at least 15 inputs including bad ones,
   then split it into parse, repository, pure rules and service. Done when the tests pass
   unchanged before and after.
6. **(hard)** Write a one-page architecture for the loan-origination server (2.10) in the
   design-doc template from Module 12, including the unit-of-work and outbox decisions and a
   trade-off table for "rules in procedures vs in the server". Done when you can present it
   in 10 minutes.

## 7. Interview questions

**Q: How would you structure the codebase of a TCP server?**
A: Four layers: transport (sockets, framing, threads), protocol (bytes to typed requests and
back, pure), service (business rules), storage (persistence), plus common value types and a
composition root in `main`. Dependencies point inward toward the service; the service owns
the storage interface, and storage implements it. Each layer is a separate library target so
the build enforces the graph.

**Q: What is dependency inversion at the architecture level?**
A: High-level policy (the service) does not depend on low-level detail (the database); both
depend on an abstraction, and the abstraction is owned by the high-level side. Concretely the
`Storage` interface lives in the service's directory and the database adapter links against
it. That is why rules can be tested with an in-memory fake and why replacing the database is
an adapter change.

**Q: Where would you put interfaces?**
A: At I/O (sockets, DB, files, clock), at layer boundaries, and where two real
implementations exist. Not in front of pure logic like a parser, which is easier to test
directly. The runtime cost of a virtual call per request is negligible next to a syscall;
the cost that matters is readability.

**Q: What is your error-handling policy in a server?**
A: Bugs assert or abort; expected failures are values (`Result`/`optional`); transient
environmental failures are values with a retryable code or exceptions caught at the service
boundary. Translate at each boundary into the next layer's vocabulary without changing
meaning, map to wire errors in exactly one function, never let exceptions cross threads, C
callbacks or destructors, and log once where the error is handled.

**Q: How do you handle configuration?**
A: Load once at start-up with defined precedence (defaults, file, env, flags), validate
fully, fail fast on unknown keys or bad values, log the effective config without secrets,
and pass it immutable by `const&`. Hot-reload only safe fields through an atomic snapshot
swap.

**Q: How would you refactor a 1000-line legacy C handler safely?**
A: Characterization tests first, capturing current outputs for real inputs. Then create seams
by extracting data access behind an interface and rules into pure functions, in small steps
each covered by the tests. Route traffic gradually to the new path (strangler), compare in
shadow mode, switch over, and delete the old code.

**Q: Business logic in stored procedures or in the application server?**
A: Data-heavy checks and invariants every caller must respect are better near the data: one
round trip and impossible to bypass. Orchestration, integrations and complex computation are
better in the server: testable, debuggable, scalable horizontally. The cost of procedures is
database CPU, harder testing and schema-versioning discipline.

**Q: How do you guarantee a side effect (SMS, posting) happens exactly when a state change commits?**
A: The transactional outbox: write the state change and an outbox row in the same database
transaction; a separate worker reads the outbox, performs the side effect with an idempotency
key, and marks it done. The side effect may be retried, so it must be idempotent, but it is
never lost and never sent for a rolled-back change.

## 8. Recap

- Transport moves bytes, protocol converts bytes to typed values, service applies rules,
  storage persists. Common types are shared; `main` is the composition root.
- Dependencies point toward the service. The service owns the storage port; storage
  implements it. Enforce the graph with one CMake target per layer.
- Interfaces at I/O and layer boundaries, not everywhere. A virtual call per request is noise
  next to a syscall.
- Config: once, validated, immutable, logged; never `getenv` mid-request.
- Logging: injected, levelled, structured, correlated by request id, asynchronous with a
  bounded dropping queue, never logs secrets or personal identifiers.
- Errors: bugs abort, expected failures are values, translate at boundaries without changing
  meaning, map to wire errors in one place.
- Good modules are deep, own their data, have one reason to change, and form no cycles.
- Loan server: unit of work per request, outbox for side effects, immutable master-data
  snapshot, worker count no larger than the DB pool.
- Legacy refactor: inventory, characterization tests, seams, move, strangle, delete.

