# 03. Ownership and Object Lifetime Design

**Time:** ~3 hours   **Prerequisites:** 01; 02_Modern_CPP module 05 (unique_ptr, shared_ptr basics)

## 1. In plain words

Every object in a running program has exactly one question that must have a clear answer:
*who is responsible for destroying it, and when?* In C you answer this with discipline and
comments ("caller frees", "borrowed, do not free"). You already know how that goes at 3 AM:
a double free in one place, a leak in another, a use-after-free that only shows up under load.

Ownership design is the practice of answering that question *in the type system* so the
compiler enforces it. `std::unique_ptr<T>` means "I own this and nobody else does". A plain
`T&` or `T*` means "I am looking at this, someone else owns it, and they promise it outlives
my use". `std::shared_ptr<T>` means "several of us own it and the last one out turns off the
lights", and that is a design statement you should be reluctant to make.

The analogy: in a well-run office every physical key has one person responsible for it. You
can lend the key (a reference), but there is never confusion about who returns it at the end
of the day. Shared ownership is the key on a hook that everyone uses; eventually nobody knows
whether the door was locked.

## 2. The real thing

### 2.1 The vocabulary

| Term | Meaning | C++ spelling | C spelling |
|---|---|---|---|
| Owner | Responsible for destruction | value member, `std::unique_ptr<T>`, container of values | the struct that `free()`s it |
| Unique ownership | Exactly one owner at a time; transferable | `std::unique_ptr<T>`, move semantics | "caller takes ownership" comment |
| Shared ownership | Several owners; destroyed when last releases | `std::shared_ptr<T>` | manual refcount field |
| Observer / borrow | Non-owning view, must not outlive owner | `T&`, `T*`, `std::string_view`, `std::span` | plain pointer + "do not free" |
| Weak observer | Non-owning, can detect that the owner is gone | `std::weak_ptr<T>` | generation-checked handle |
| Handle | Integer or opaque id resolved by the owner | `struct ConnId { uint64_t v; }` | `int fd`, `HANDLE` |

A **lifetime** is the span from construction to destruction. A **dangling** pointer or
reference is one whose target's lifetime has ended. Every memory-safety bug in C is a
lifetime bug: use before start (uninitialised), use after end (use-after-free), two ends
(double free), no end (leak).

### 2.2 Ownership diagrams

Before writing a class, draw who owns whom. Solid arrows are ownership (`unique_ptr` or
value member), dashed arrows are observation (reference or raw pointer).

```
   Server
   |-- owns --> Metrics                         (value member, declared first)
   |-- owns --> Listener                        (value member)
   |-- owns --> map<ConnId, unique_ptr<Connection>>
                     |
                     Connection
                     |-- owns --> unique_ptr<Session>     (null until authenticated)
                     |-- owns --> vector<Request>         (values)
                     |-- observes - - -> Metrics&         (Server guarantees lifetime)
                     |-- observes - - -> Store&           (Server guarantees lifetime)
```

Rules the diagram must satisfy:

1. **It is a tree** (or a forest). Every object has exactly one owning path from a root. The
   roots are `main`'s locals or, rarely, statics.
2. **Every observation arrow points to something whose owner is an ancestor or an
   earlier-declared sibling.** `Connection` observes `Metrics`; `Metrics` is owned by
   `Server`, which also owns `Connection`, and `Metrics` is declared before the connections
   map so it is destroyed after. If an observation arrow points to a *later* sibling or to a
   *descendant* of another branch, you have a lifetime risk.
3. **No cycles.** If A owns B and B needs to reach A, B gets an observer `A&` or `A*`, never a
   `shared_ptr<A>`.

Draw this for any class with more than two members holding pointers. It takes two minutes and
it finds most lifetime bugs before they are written.

### 2.3 `unique_ptr` trees

The default ownership structure for a server, a parser, a UI, anything: a tree of
`unique_ptr`s rooted in one object, usually created in `main`.

```cpp
class Server {
    Metrics metrics_;                                            // value: simplest ownership
    std::unique_ptr<Store> store_;                               // unique_ptr because polymorphic
    std::unordered_map<ConnId, std::unique_ptr<Connection>> conns_;  // unique_ptr because
                                                                 // Connection is non-movable
};
```

When to use a value member vs `unique_ptr`:

- **Value member** when the type is concrete, movable (or never moved), and always present.
  No heap allocation, no indirection, destroyed automatically.
- **`unique_ptr`** when the member is polymorphic (held through a base class), optional (may
  be null), non-movable but the container needs to move (a `std::vector<Connection>` would
  need `Connection` to be movable; `std::vector<std::unique_ptr<Connection>>` does not), or
  large and rarely present.

Transfer of ownership is a move: `std::unique_ptr<Connection> c = acceptor.take_next();
conns_.emplace(id, std::move(c));`. The compiler refuses to copy a `unique_ptr`, so there is
no accidental double ownership.

Passing ownership *into* a function: `void adopt(std::unique_ptr<T> t)`. Caller writes
`adopt(std::move(t))` and is left with a null pointer; the intent is in the signature.

Passing an object for *use* without transferring: `void process(T& t)` or `void process(const
T& t)`. Never `void process(std::unique_ptr<T>& t)` unless the function may reseat the pointer
(rare), and never `void process(std::shared_ptr<T> t)` just to "be safe": it forces every
caller into shared ownership and adds an atomic increment per call.

### 2.4 Shared ownership as a smell

`std::shared_ptr` is correct when the lifetime of an object is genuinely determined by the
*last* of several independent users, and you cannot know statically which one that will be.
Real examples: a cached, immutable configuration snapshot that in-flight requests on several
threads are reading while a new one is being installed (Module 07, RCU); an asynchronous
operation's completion handler that must keep the connection alive until the callback fires.

It is a smell when it is used because the author could not decide who the owner is. Costs:

- Two allocations per object (object + control block) unless `make_shared`; the control block
  holds two atomic counters.
- Every copy of the pointer is an atomic increment and decrement, which is a full memory
  barrier on x86 (`lock xadd`), roughly 20 cycles uncontended and hundreds under contention
  from several cores.
- Cycles leak. `A` holds `shared_ptr<B>`, `B` holds `shared_ptr<A>`: neither count reaches
  zero. You need `weak_ptr` on one side, and now you have to think about which.
- Destruction happens "somewhere": whichever thread drops the last reference runs the
  destructor, possibly a thread that should never touch that object's resources.
- The ownership diagram is no longer a tree; nobody can draw it.

Rule: reach for `shared_ptr` only after you have tried to draw the tree and *proved* that no
single owner can be identified. In a server, that is usually only the config snapshot and the
"keep me alive until my async op completes" case.

### 2.5 Observer pointers and references

A raw pointer in modern C++ is not a bug. It is the correct spelling for "non-owning, may be
null". A reference is "non-owning, never null, cannot be reseated". Use them freely for
observation, with one discipline: **the observer must not be stored in anything that could
outlive the owner.**

Safe: a function parameter `Store&`; a member `Metrics&` in a `Connection` owned by the object
that owns the `Metrics`; a local pointer returned by `find()` used before the next call that
could invalidate.

Unsafe: storing a `Connection*` in a global list of "connections waiting for a reply"; a
lambda capturing `this` and posted to a thread pool; a `std::string_view` member pointing at a
`std::string` parameter (the string dies at the end of the call).

The invalidation question for every observer: *what operations on the owner invalidate this?*
For a `std::vector<T>` element pointer: any `push_back` that reallocates, any `erase` before
it. For a `std::map` node pointer: only erasing that node. For `std::unordered_map`: rehash
invalidates iterators but *not* pointers or references to elements (the nodes are stable).
`std::deque`: `push_back`/`push_front` keep element references valid but invalidate
iterators. Know these for the containers you use.

### 2.6 Handles vs pointers

A **handle** is an id that the owner resolves to an object on demand. It is the right tool
when an observer must survive across operations that may destroy the target.

```cpp
struct ConnId { std::uint64_t value; };
Connection* Server::find(ConnId id);   // nullptr if closed
```

Advantages over a pointer: resolution fails cleanly (`nullptr`) instead of dereferencing freed
memory; handles can cross thread, process and network boundaries; the owner can relocate the
object (compact a pool) without updating observers; a handle fits in a log line.

Make the handle hard to reuse by accident: a 64-bit counter never repeats in practice. If you
must use a small index (pool slot), pair it with a **generation** counter that increments
every time the slot is reused; `find` compares generations and rejects stale handles. This is
how game engines, kernels (file descriptors plus inode generation) and `weak_ptr` (the
control block's weak count) solve the same problem.

```cpp
struct SlotHandle { std::uint32_t index; std::uint32_t generation; };
```

### 2.7 Lifetimes across threads

A thread is another observer with no synchronised view of your destructor. The three safe
patterns:

1. **Structured lifetime**: the owner starts the thread and joins it in the owner's
   destructor, so the thread cannot outlive the objects it uses. `std::thread` members
   declared *last* in the class so they are destroyed (joined) *first*.
2. **Message passing with values**: the thread receives copies or moved values (a `Request`
   by value, a `std::string`, a `std::shared_ptr<const Config>` snapshot), never a reference
   to something owned by another thread.
3. **Keep-alive**: an async operation holds a `shared_ptr` to its object until it completes
   (`enable_shared_from_this`, the Asio idiom). This is one of the legitimate `shared_ptr`
   uses. The cost is that the object's destruction time becomes non-deterministic.

The anti-pattern: `thread_pool.post([this] { this->on_timer(); })` followed by `delete this`
(or the owner destroying the object) before the task runs. Module 06 (observer) and 07 cover
this bug in detail because it is the most common crash in event-driven servers.

### 2.8 Object pools and arenas

Allocation is not free: `malloc` for small sizes is around 20 to 50 ns on glibc with a warm
thread cache, more with contention or fragmentation, and every allocation is a potential
cache miss on access. Two patterns to control it:

**Object pool**: pre-construct N objects, lend them out, take them back. Bounded memory,
no fragmentation, warm cache lines. Use a `unique_ptr` with a custom deleter that returns
the object, so a forgotten return is impossible. Exhaustion becomes explicit back-pressure
(return `nullopt`, or block with a timeout), which is almost always what you want under
overload instead of unbounded heap growth followed by the OOM killer.

**Arena** (region, bump allocator): one large block; allocation is "round up, bump a
pointer"; freeing is "reset the pointer". Perfect for per-request scratch data: parse the
message, allocate every field in the arena, process, reset. Hundreds of allocations become
one. The constraint: objects in the arena are not individually destroyed, so they must be
trivially destructible or you run destructors yourself before reset.

Both exist in C (you have probably written a free-list). The C++ gain is that the return path
is enforced by a destructor, and the arena can refuse non-trivial types at compile time with
`static_assert(std::is_trivially_destructible_v<T>)`.

### 2.9 Designing ownership in a server: connections, requests, sessions

The concrete design for a TCP application server (the kind you maintain):

```
main()
  owns Config (value)                               lifetime: whole process
  owns Server (value)
        owns Metrics, Logger (values, declared first)
        owns Store (unique_ptr<Store>, polymorphic)
        owns Acceptor (value, holds listening fd)
        owns WorkerPool (value, declared LAST so threads are joined first)
        owns map<ConnId, unique_ptr<Connection>>
              Connection
                owns fd (RAII wrapper, value)
                owns read/write buffers (values)
                owns unique_ptr<Session>             lifetime: auth .. close
                owns deque<Request>                  lifetime: parse .. reply sent
                observes Store&, Metrics&, Logger&
```

Decisions and reasons:

- **A `Request` is a value.** It is created by the parser, moved into the connection's queue,
  moved to a worker, processed, and the reply is moved back. Nothing holds a pointer to it
  across a thread boundary; the request *itself* crosses the boundary by move. No
  `shared_ptr<Request>`.
- **A `Session` is owned by its `Connection`.** When the socket closes, the session dies. If
  sessions must survive reconnects (resume tokens), the `Server` owns a `SessionStore` and the
  connection holds a `SessionId` handle instead.
- **Workers never hold `Connection*`.** They receive `(ConnId, Request)` by value and return
  `(ConnId, Reply)`. The I/O thread resolves `ConnId`; if the client disconnected meanwhile,
  the reply is dropped. This single rule eliminates the "client disconnected mid-request"
  crash class.
- **Store, Metrics, Logger are observed by reference** because `Server` owns them and
  declares them before the connections and worker pool, so they are destroyed after.
- **Shutdown order is destruction order reversed**: workers joined first (no more tasks),
  then connections closed (fds released), then store flushed, then metrics and logger. This
  falls out of member declaration order; you do not write a `shutdown()` function unless you
  need to run it before destruction.

## 3. Code

### `code/m03_ownership_tree.cpp`

The server above, single-threaded, with handles. Key parts:

```cpp
class Server {
    // Declaration order IS destruction order (reversed). metrics_ before conns_,
    // so every ~Connection can still call metrics_.dec_connections().
    Metrics metrics_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<ConnId, std::unique_ptr<Connection>, ConnIdHash> conns_;
public:
    ConnId accept();                 // hands out a handle, never a pointer
    Connection* find(ConnId id);     // observer pointer, valid until the next close()
    void close(ConnId id) { conns_.erase(id); }   // ~Connection runs here
};
```

`Connection` deletes its copy operations because its address is handed out as an observer
and because it owns a resource with identity (an fd in real code). The `main` function closes
a connection and shows `find()` returning `nullptr` where a stored pointer would have dangled.

### `code/m03_object_pool_arena.cpp`

A `BufferPool` whose `acquire()` returns `std::unique_ptr<RequestBuffer, Returner>`, so the
buffer goes back to the pool at scope exit even on exceptions, and an `Arena` with bump
allocation and a `static_assert` that rejects non-trivially-destructible types.

```cpp
struct Returner {
    BufferPool* pool;
    void operator()(RequestBuffer* b) const { pool->release(b); }   // deleter returns, not frees
};
using Handle = std::unique_ptr<RequestBuffer, Returner>;
```

```cpp
template <class T, class... Args>
T* Arena::make(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>, "nothing will call destructors");
    void* p = allocate(sizeof(T), alignof(T));
    return new (p) T(std::forward<Args>(args)...);     // placement new into the arena
}
```

## 4. Under the hood (senior-level)

**`unique_ptr` is free.** `sizeof(std::unique_ptr<T>) == sizeof(T*)` with the default
deleter (the deleter is an empty class and the empty base optimisation removes it). The
destructor is `if (p) delete p;`, inlined. Moving is two pointer stores. There is no
runtime cost over a raw pointer with a manual `delete`; the only cost is the one you want,
which is that the compiler refuses to copy it. With a stateful deleter (the pool `Returner`
holding a `BufferPool*`) it is 16 bytes.

**`shared_ptr` is not free.** `sizeof == 2 * sizeof(void*)`: object pointer plus control
block pointer. The control block holds the strong count, the weak count, the deleter and
the allocator. `make_shared` puts the object and the control block in one allocation
(faster, one cache line), but then the memory for the object is not released until the
weak count also hits zero. Copying does `lock xadd` on the strong count; in a loop on a
shared object across cores, that line bounces between caches and dominates run time.

**Why declaration order is destruction order.** The compiler generates the destructor body
as: user-written body, then destroy members in reverse declaration order, then base
classes. That is why "declare what others depend on first" is not a style preference; it is
how you guarantee a member is still alive when later members' destructors run.

**`std::thread` in a destructor.** A joinable `std::thread` whose destructor runs calls
`std::terminate`. That is deliberate: it forces you to decide. Declaring the thread (or
pool) last and joining in the destructor is the structured pattern. C++20's `std::jthread`
joins automatically; in C++17 you write the join.

**Handles and the kernel.** A file descriptor is a handle into the process's fd table; the
kernel owns the `struct file`. `close(fd)` drops the table entry; the `struct file` is
refcounted because `dup` and `fork` can share it. The fd number *is* reused immediately,
which is exactly the stale-handle problem Section 2.6 describes, and the cause of the
classic "I wrote to the wrong socket" bug when a close races with a write. Generation
counters are how you avoid that in your own handle tables.

**Arenas and the hardware.** A bump allocator hands out *consecutive* addresses, so objects
allocated together are adjacent in memory and prefetch well. `malloc` returns whatever block
fits, scattering related objects across the heap; walking a linked structure then costs one
cache miss (60 to 100 ns typical, main memory) per node. This locality, not the saved
`free()` calls, is usually the bigger win.

## 5. In production

**Your migration engine.** A Berkeley DB cursor, a YugabyteDB connection, a batch of rows
in flight, a transformation context. Who owned the row buffer while it was being written?
If the answer was "the loop variable, and I free it at the bottom", every `continue` and
every error path was a leak risk. The C++ shape: `struct Batch { std::vector<Row> rows; }`
owned by the loop iteration, a `PgConnection` RAII wrapper owned by the engine, a `BdbCursor`
RAII wrapper owned by the scan. Zero explicit frees, zero leaks on error paths.

**The 3 AM bugs and how ownership design prevents them.**

- *Client disconnects mid-request; worker writes reply to freed connection.* Prevented by
  workers holding `ConnId`, never `Connection*`.
- *Shutdown crash: logger destroyed before the last worker logs.* Prevented by declaring the
  logger first and the worker pool last.
- *Memory grows until OOM under a burst.* Prevented by a bounded pool with explicit
  back-pressure instead of `new` per request.
- *Session state leaks after disconnect.* Prevented by `Connection` owning `unique_ptr<Session>`.
- *Double free of a buffer on an error path.* Prevented by the pool's deleter: the compiler
  will not let you return twice because you cannot copy the `unique_ptr`.

**Debugging lifetime bugs.** `-fsanitize=address` reports use-after-free with the
allocation and deallocation stacks; run your test suite under it always. For production
crash dumps: a vptr pointing to freed memory, or a string whose length field is a pointer
value, means use-after-free. Valgrind's `--track-origins=yes` for uninitialised reads.
`gdb` with `watch -l *ptr` to find who writes a location.

**Interview framing.** When asked "how do you manage memory in a large C++ codebase", the
senior answer is not "smart pointers". It is: "I draw the ownership tree first; values and
`unique_ptr` for ownership, references and raw pointers for observation, handles when an
observer must outlive its target, `shared_ptr` only for the two cases where multiple
independent lifetimes genuinely decide destruction. Declaration order encodes shutdown
order."

## 6. Try it

1. **(easy)** Draw the ownership diagram (solid = owns, dashed = observes) for the RPC server
   you maintain at work: listener, worker threads, connections, request buffers, database
   handles, config. Mark every dashed arrow that points to something not owned by an
   ancestor. Done when you have a list of the lifetime risks in your real codebase.
2. **(easy)** In `m03_ownership_tree.cpp`, swap the declaration order of `metrics_` and
   `conns_` in `Server`. Predict what happens at shutdown; then add a print in
   `~Metrics` and `~Connection` to confirm. Done when you can explain the output.
3. **(medium)** Add a generation counter to `BufferPool` so that `Handle`s are indices plus
   generation, and `pool.get(handle)` returns `nullptr` for a stale handle after the buffer
   was recycled. Done when a test shows a stale handle rejected.
4. **(medium)** Make `Arena::make` support non-trivially-destructible types by recording
   `(void* obj, void (*dtor)(void*))` pairs and running them in `reset()` in reverse order.
   Done when a `std::string` allocated in the arena is destroyed exactly once (verify with
   AddressSanitizer or a counting type).
5. **(hard)** Build a two-thread version of `m03_ownership_tree.cpp`: an I/O thread owns
   connections, a worker thread receives `(ConnId, Request)` through a queue and returns
   `(ConnId, std::string)`. Close a connection while its request is in flight. Done when the
   program never touches a freed `Connection` (run under `-fsanitize=address,thread`), and
   you can explain which rule made that impossible.

## 7. Interview questions

**Q: When do you use `unique_ptr`, `shared_ptr`, a raw pointer and a reference?**
A: `unique_ptr` for sole ownership of a heap object (polymorphic, optional or non-movable
member). `shared_ptr` only when several independent lifetimes genuinely decide destruction,
such as a config snapshot read by in-flight requests or an async keep-alive. Raw pointer for
a non-owning, possibly null observer; reference for a non-owning, never-null one. Ownership
should form a tree.

**Q: Why is `shared_ptr` a smell?**
A: It usually means the author did not identify the owner. Costs are atomic refcount traffic,
a control block, cycle leaks needing `weak_ptr`, non-deterministic destruction on whichever
thread drops the last reference, and an ownership graph nobody can draw.

**Q: How do you design ownership of connections and requests in a server?**
A: Server owns connections in a map keyed by handle; connection owns its fd, buffers, session
and queued requests; requests move by value between threads; workers hold `ConnId`, not
`Connection*`, and the I/O thread resolves the handle when the reply comes back so a
disconnected client's reply is simply dropped.

**Q: What is the difference between a handle and a pointer?**
A: A pointer is an address that is valid only while the object lives; dereferencing a stale
one is undefined behaviour. A handle is an id resolved by the owner, so a stale handle fails
cleanly. Pair small-index handles with a generation counter to detect slot reuse.

**Q: Why does member declaration order matter?**
A: Members are constructed in declaration order and destroyed in reverse. If member B holds a
reference to member A, A must be declared first so it outlives B. Threads and pools go last
so they are joined before the resources they use are destroyed.

**Q: What does an object pool buy over `new`/`delete` per request?**
A: Bounded memory with explicit back-pressure under overload, no fragmentation, warm cache
lines, and no allocator lock contention. With a `unique_ptr` custom deleter the return path
is enforced by the compiler.

**Q: When is an arena appropriate and what is its constraint?**
A: For many short-lived allocations with a common end point, such as everything created while
parsing and processing one request. Allocation is a pointer bump and freeing is a reset. The
constraint is that objects are not individually destroyed, so they must be trivially
destructible or you must run destructors yourself.

## 8. Recap

- Every object has one owner; draw the tree before writing the class.
- Value members and `unique_ptr` for ownership; `T&` and `T*` for observation; handles when
  observers must outlive their targets.
- `shared_ptr` only when several independent lifetimes genuinely decide destruction; it costs
  atomics, a control block and clarity.
- Observation arrows must point to objects owned by an ancestor or an earlier-declared
  sibling.
- Declaration order is reverse destruction order; it encodes shutdown.
- Requests cross thread boundaries by value; workers hold handles, not pointers.
- Pools give bounded memory and enforced return via custom deleters; arenas give locality and
  one-shot freeing.
- `-fsanitize=address` on every test run; a wild vptr in a dump means use-after-free.
