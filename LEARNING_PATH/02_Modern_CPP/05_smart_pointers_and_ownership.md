# 5. Smart pointers and ownership design

**Time:** ~3 hours   **Prerequisites:** 03, 04

## 1. In plain words

A raw pointer answers "where is the object?" but not "who deletes it?". In your C code that second
question is answered by a comment, a naming convention, or the arena allocator's contract. A smart
pointer answers it in the type: `std::unique_ptr<T>` means "I am the single owner and I will delete
it"; `std::shared_ptr<T>` means "we jointly own it; the last one out deletes it"; a raw `T*` or `T&`
then means, by convention, "I am only looking, someone else owns it".

Analogy: a car. `unique_ptr` is the title deed: exactly one person holds it, and it can be handed
over. `shared_ptr` is a car-share where the car is scrapped when the last member leaves.
`weak_ptr` is knowing the car's number plate: you can check whether it still exists and borrow it
if so, but you do not keep it alive. A raw pointer is a passenger: enjoys the ride, never owns anything.

## 2. The real thing

### 2.1 `std::unique_ptr<T, Deleter>`

- Owns exactly one object (or array with `unique_ptr<T[]>`). Move-only. Destructor calls the deleter.
- `std::make_unique<T>(args...)` constructs the object and the pointer in one step. Prefer it over
  `unique_ptr<T>(new T(...))`: exception-safe in expressions with several allocations, and no `new`
  in your code.
- `get()` gives the raw pointer for non-owning use (passing to a C API). `release()` gives up
  ownership and returns the raw pointer. `reset(p)` deletes the current object and takes `p`.
- Custom deleters make it the universal wrapper for C APIs:

```cpp
struct FileCloser { void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); } };
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;          // sizeof == sizeof(FILE*)

using ZctxPtr = std::unique_ptr<zctx, void (*)(zctx*)>;          // sizeof == 2 pointers
ZctxPtr z(zctx_new(6), &zctx_free);
```

A stateless functor deleter costs nothing (empty base optimisation). A function-pointer deleter
costs one extra pointer in the object.

### 2.2 `std::shared_ptr<T>`

- Shared ownership through a **control block** holding a strong count, a weak count, the deleter,
  and (with `make_shared`) the object itself.
- Copying increments the strong count atomically; destroying decrements; when it hits zero the
  object is destroyed; when the weak count also hits zero the control block is freed.
- `std::make_shared<T>(args...)` does **one** allocation for control block and object together.
  `shared_ptr<T>(new T)` does two, and leaks `T` if the control block allocation throws.
- `sizeof(shared_ptr) == 2 * sizeof(void*)`: a pointer to the object and a pointer to the control
  block. The object pointer can differ from what the control block manages (aliasing constructor,
  `shared_ptr<Base>` from `shared_ptr<Derived>`).
- Thread safety: the reference count is atomic, so copying `shared_ptr`s to the same object from
  several threads is safe. Access to the *pointed-to object* is not synchronised, and concurrent
  modification of the *same* `shared_ptr` variable is a data race.

### 2.3 `std::weak_ptr<T>`

A non-owning observer of a `shared_ptr`-managed object. `lock()` returns a `shared_ptr` (null if the
object is gone). `expired()` is a cheaper check but racy: by the time you act, it may have died.
Two uses:
- **Breaking cycles.** Parent owns child (`shared_ptr`), child refers back to parent (`weak_ptr`).
  With two `shared_ptr`s neither count ever reaches zero and both leak.
- **Caches and observers.** A cache holds `weak_ptr`s so it never keeps an object alive on its own.

### 2.4 Ownership design

Decide, for every object in the system, exactly one of:

| Ownership | Type to use | Typical case |
|---|---|---|
| Single owner, lifetime = scope | plain member / local object (no pointer at all) | most objects |
| Single owner, polymorphic or optional or heap-sized | `std::unique_ptr<T>` | a `Logger` behind an interface, a 1 MB buffer |
| Single owner, transferable across threads/queues | `std::unique_ptr<T>` moved into the queue | request objects in a worker pool |
| Several owners with unrelated lifetimes | `std::shared_ptr<T>` | a config snapshot read by many threads while being replaced |
| Observer that must not extend lifetime | `std::weak_ptr<T>` | caches, back-pointers, callbacks that might outlive the target |
| Non-owning access, owner guaranteed alive | `T&` or `T*` (nullable) | function parameters, iteration |

`shared_ptr` is **not** the default. It is for genuinely shared, non-hierarchical lifetime. Most
C++ codebases that overuse it end up with unclear ownership, cycles, and atomic traffic in hot loops.
Start with values and `unique_ptr`; introduce `shared_ptr` only when you can name the second owner.

### 2.5 Mapping to your arena allocator

Your C arena with "explicit ownership contracts" is exactly this discipline done by hand: the arena
is the single owner of every block, callers get non-owning pointers, and the lifetime ends when the
arena resets. In C++ that design still exists (module 12, `std::pmr`), and the vocabulary types make
the contract checkable: a function taking `T*` is documented as non-owning; one taking
`std::unique_ptr<T>` takes ownership; one returning `std::unique_ptr<T>` gives it.

## 3. Code

`code/m05_unique_ptr_c_api.cpp` wraps three kinds of C resources:

```cpp
if (FilePtr f = open_file("/tmp/m05_demo.txt", "w")) {   // contextual bool: non-null?
    std::fputs("hello\n", f.get());                       // raw pointer to the C API
}                                                          // fclose here

MallocPtr<char> buf(static_cast<char*>(std::malloc(64)));  // deleter calls free

ZctxPtr z(zctx_new(6), &zctx_free);
ZctxPtr z2 = std::move(z);        // z is null now
zctx* raw = z2.release();         // hand ownership back to C; z2 will not free
zctx_free(raw);

auto arr = std::make_unique<int[]>(8);   // delete[] on destruction
```

`code/m05_shared_weak_ptr.cpp` shows the counts changing, a cache that does not keep objects alive,
and a parent/child cycle broken with `weak_ptr`:

```cpp
auto s1 = std::make_shared<Session>("shakir");   // one allocation: control block + Session
{ std::shared_ptr<Session> s2 = s1; }            // use_count 1 -> 2 -> 1 (two atomic ops)

class SessionCache {
    std::unordered_map<std::string, std::weak_ptr<Session>> cache_;
public:
    std::shared_ptr<Session> get(const std::string& id) {
        auto it = cache_.find(id);
        return it == cache_.end() ? nullptr : it->second.lock();   // null if the session died
    }
};

struct Node {
    std::shared_ptr<Node> child;    // owning edge
    std::weak_ptr<Node> parent;     // back edge: weak, or both nodes leak
};
```

Expected: `~Session(shakir)` prints as soon as `s1.reset()` runs even though the cache still holds a
`weak_ptr`, and both `~Node` lines print at the end of the block.

## 4. Under the hood (senior-level)

**`unique_ptr` is a zero-overhead abstraction.** `sizeof(std::unique_ptr<T>) == sizeof(T*)` with a
stateless deleter; `operator->` and `operator*` inline to a load; the destructor inlines to
`if (p) delete p;`. The one real cost is at the ABI boundary: on x86-64, a `unique_ptr` is passed to
a function in memory (because it has a non-trivial destructor), not in a register like a raw pointer.
Measurable only in very hot code.

**The `shared_ptr` control block** (libstdc++, `_Sp_counted_base`) contains a vptr (the control block
is polymorphic so it can hold different deleters and allocators), an `int` strong count (`_M_use_count`),
and an `int` weak count (`_M_weak_count`). With `make_shared`, the `T` object is embedded directly
after it in the same allocation (`_Sp_counted_ptr_inplace`). Typical size: 16 bytes of header plus
the object. `shared_ptr<T>(new T)` instead allocates `T` separately and a smaller control block that
stores the pointer: two allocations, two cache lines.

**Atomic reference counting cost.** Every copy and destroy of a `shared_ptr` is a `lock xadd` (or
`lock dec`) on the control block's count. On x86 that is a full barrier, roughly 20 cycles
uncontended and far more if another core recently touched the same line (the count is a shared
write hotspot). libstdc++ skips the atomic instructions when the program has not linked
`libpthread` (`__gthread_active_p()`), which is why a single-threaded benchmark can look faster than
the server. Passing `shared_ptr` by value through five layers of calls does five increments and five
decrements for no reason; pass `const shared_ptr<T>&` or, better, `T&` when the callee does not need
to own.

**Why `make_shared` can delay freeing memory.** Because object and control block share one allocation,
the memory cannot be returned until the weak count also reaches zero. A long-lived `weak_ptr` to a
large object keeps the object's *bytes* allocated (though destroyed) until the `weak_ptr` dies.
For large objects observed by long-lived weak pointers, `shared_ptr<T>(new T)` is the better choice.

**`weak_ptr::lock()` is a CAS loop.** It must increment the strong count only if it is non-zero, so
it reads the count, checks, and does a compare-and-swap; retries on contention. `expired()` is a plain
load and is only a hint.

**`enable_shared_from_this`.** A class that must produce a `shared_ptr` to itself from inside a
member function (typical in async callbacks: `self = shared_from_this()` captured in the lambda)
derives from `std::enable_shared_from_this<T>`, which stores a `weak_ptr` to the control block that
`make_shared` fills in. Calling `shared_from_this()` on an object not owned by a `shared_ptr`
throws `std::bad_weak_ptr`.

**Custom deleter type erasure.** `shared_ptr<T>` has one type regardless of deleter (the deleter
lives in the control block); `unique_ptr<T, D>` has the deleter in its type. That is why
`shared_ptr` is convenient for plugin-style APIs and `unique_ptr` is the tool for zero-overhead wrappers.

## 5. In production

- **Hot paths and `shared_ptr`.** A request handler that copies a `shared_ptr<Config>` per request is
  one atomic RMW per request; per *field access* it would be a disaster. Pattern: take a copy once at
  the top of the request (or hold a `const Config&` if the owner is guaranteed to outlive the call),
  then use references.
- **Config hot-reload** is the textbook `shared_ptr` use: readers hold `shared_ptr<const Config>`
  for the duration of a request; the reloader builds a new `Config` and atomically swaps the pointer
  (`std::atomic<std::shared_ptr<T>>` in C++20, or a mutex-protected pointer in C++17). Old readers
  finish on the old snapshot; it is freed when the last one lets go.
- **Cycles are silent leaks.** A `Session` holding a `shared_ptr<Connection>` whose callback captures
  `shared_ptr<Session>` never frees. Valgrind reports "still reachable", not "lost". Rule: callbacks
  capture `weak_ptr` and `lock()` when invoked, or capture a raw pointer with a documented lifetime.
- **Never `delete` what a smart pointer owns; never wrap the same raw pointer twice.**
  `shared_ptr<T> a(p); shared_ptr<T> b(p);` creates two control blocks and a double free. Always
  copy the `shared_ptr`, never the raw pointer, to add an owner.
- **Interop with C callbacks.** `void* user_data` in a C API is where ownership gets lost. Pass
  `handle.get()` and keep the owner alive in a known scope, or pass `handle.release()` and document
  which C callback frees it.
- **3 AM debugging.** ASan reports "heap-use-after-free" on an object held through a raw pointer: some
  `unique_ptr` owner was destroyed while an observer kept the raw pointer. Fix the design (the observer
  should hold a `weak_ptr`, or the owner must outlive the observer), not the symptom.

## 6. Try it

1. **(easy)** Write a `unique_ptr` alias for `DIR*` (`opendir`/`closedir`) with a stateless deleter and
   assert `sizeof` equals one pointer. Done when it lists `/tmp` and the static_assert holds.
2. **(easy)** In `m05_shared_weak_ptr.cpp`, change `Node::parent` to `shared_ptr` and run under
   `valgrind --leak-check=full`. Done when you can read the report and identify the cycle.
3. **(medium)** Implement `class Connection : std::enable_shared_from_this<Connection>` with an
   `async_read(std::function<void()> on_done)` that stores a lambda capturing `shared_from_this()`.
   Show that the connection survives until the callback runs. Done when the destructor prints after the
   callback, not before.
4. **(medium)** Write a tiny `SharedPtr<T>` with an `int` strong count (non-atomic), copy/move/destroy,
   and `use_count()`. Then make the count `std::atomic<int>` and benchmark copying in a loop from one
   thread vs four threads sharing one object. Done when you can report the ratio and explain it.
5. **(hard)** Design the ownership for a tcpkv-style server: `Server` owns listening `SocketFd`,
   accepts into `Connection` objects, each handled by a worker from a pool, with a shared
   `KvStore`. Draw the ownership graph and assign a type (value / `unique_ptr` / `shared_ptr` /
   `weak_ptr` / reference) to every edge. Done when no edge is `shared_ptr` without a named second
   owner and no cycle exists.

## 7. Interview questions

**Q: When would you choose `unique_ptr` over `shared_ptr`?**
By default. `unique_ptr` expresses single ownership, costs the same as a raw pointer, and can be
moved to transfer ownership. `shared_ptr` is for genuinely shared lifetime where no single owner can
be named, and costs two pointers plus atomic reference counting on every copy.

**Q: What is in a `shared_ptr` control block, and why does `make_shared` matter?**
A strong count, a weak count, the deleter (and allocator), and with `make_shared` the object itself.
`make_shared` does one allocation instead of two and cannot leak the object if the second allocation
fails. The downside: the object's memory is held until all `weak_ptr`s are gone too.

**Q: Is `shared_ptr` thread-safe?**
The reference count is atomic, so copying and destroying different `shared_ptr` instances pointing to
the same object from several threads is safe. The managed object itself is not protected, and two
threads writing the same `shared_ptr` variable is a data race.

**Q: What problem does `weak_ptr` solve?**
Observing a shared object without extending its lifetime: it breaks ownership cycles (parent/child)
and lets caches or callbacks check whether the object still exists via `lock()`, which returns a
`shared_ptr` or null.

**Q: How do you wrap a C API handle in a smart pointer?**
`std::unique_ptr<Handle, Deleter>` where `Deleter` is a stateless functor calling the C free function.
The result is the size of a raw pointer and inlines to the same code. A function-pointer deleter also
works but adds a pointer of storage.

**Q: What does a raw pointer mean in a modern C++ codebase?**
Non-owning, nullable access to an object someone else owns. References mean non-owning, non-null.
Owning raw pointers (`new` without a smart pointer) should not appear outside of allocator internals.

**Q: Why is passing `shared_ptr` by value through many layers a performance problem?**
Each copy and destroy is an atomic read-modify-write on the control block, a full memory barrier on
x86 and a contended cache line if several threads share the object. Pass `const shared_ptr&` or a
plain `T&` when the callee does not need to participate in ownership.

**Q: What happens if you construct two `shared_ptr`s from the same raw pointer?**
Two independent control blocks each believe they own the object; the second to die double-frees it.
Always copy an existing `shared_ptr` to add an owner, or use `shared_from_this()`.

## 8. Recap

- Ownership belongs in the type: `unique_ptr` = single owner, `shared_ptr` = shared, `weak_ptr` =
  observer, raw pointer/reference = non-owning.
- `unique_ptr` costs nothing over a raw pointer with a stateless deleter; use custom deleters for C APIs.
- `make_unique` / `make_shared` instead of `new`; `make_shared` does one allocation.
- `shared_ptr` copies are atomic RMWs; pass by `const&` or `T&` in hot paths.
- `weak_ptr` breaks cycles and backs caches; `lock()` is a CAS loop, `expired()` is only a hint.
- Two `shared_ptr`s from one raw pointer is a double free; cycles are silent leaks.
- `enable_shared_from_this` for async callbacks that must keep the object alive.
- Start with values and `unique_ptr`; justify every `shared_ptr` with a named second owner.
- Your C arena "ownership contracts" are the same discipline; C++ makes them checkable types.
