# 3. RAII: the core idea of C++

**Time:** ~2.5 hours   **Prerequisites:** 02_classes_constructors_destructors.md

## 1. In plain words

RAII stands for "Resource Acquisition Is Initialisation", which is a terrible name for a simple
idea: **tie every resource to the lifetime of an object**. Acquire the resource in the constructor.
Release it in the destructor. Because the destructor runs automatically when the object goes out of
scope (module 02), the resource is released automatically, on every path, including the paths you
forgot about.

A resource is anything you must give back: heap memory, a file descriptor, a socket, a mutex lock,
a database cursor, a reference count, a temporary directory. In C you track each one by hand and
write `goto cleanup` ladders. In C++ you wrap each one once, in a small class, and never think about
it again.

Analogy: a library book with an automatic return date. You do not remember to return it; it returns
itself when you leave the building. You cannot forget, and you cannot return it twice.

## 2. The real thing

### 2.1 The C problem

```c
int process(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char* buf = malloc(4096);
    if (!buf) { fclose(f); return -1; }            /* path 1 */
    if (read_header(f, buf) != 0) { free(buf); fclose(f); return -1; }   /* path 2 */
    if (validate(buf) != 0)        { free(buf); fclose(f); return -1; }   /* path 3 */
    /* ... every new early return needs the same two lines ... */
    free(buf);
    fclose(f);
    return 0;
}
```

Every exit path must release everything acquired so far, in reverse order. Add a third resource and
the number of cleanup lines grows with the number of exits. Miss one and you leak; release twice and
you crash. Your Berkeley DB migration engine almost certainly has functions shaped like this.

### 2.2 The C++ solution

```cpp
int process(const char* path) {
    FileHandle f(path, "r");            // throws if fopen fails
    std::vector<char> buf(4096);        // owns its memory
    if (read_header(f.get(), buf.data()) != 0) return -1;   // f and buf released automatically
    if (validate(buf.data()) != 0) return -1;              // same
    return 0;
}                                       // same
```

Three exits, zero cleanup lines. Add a fourth resource: still zero cleanup lines. The compiler
generates the release calls, in the right order (reverse of acquisition), on every path.

### 2.3 The three rules of an RAII class

1. **Acquire in the constructor.** If acquisition fails, throw (or make the object represent "empty"
   and give it an `explicit operator bool`). An object that exists must be usable.
2. **Release in the destructor.** The destructor must not throw and must handle the "moved-from" or
   "empty" state (releasing nothing).
3. **Decide copy semantics.** Either delete copy (one owner: `unique_ptr`-like), implement deep copy
   (value-like: `std::string`), or share with reference counting (`shared_ptr`-like). The default
   memberwise copy is wrong for raw handles. Always make the choice explicit.

### 2.4 Exceptions and stack unwinding

When an exception is thrown, the runtime walks back up the call stack ("unwinds") looking for a
matching `catch`. For every stack frame it passes, it runs the destructors of all fully constructed
local objects in reverse order. That is the mechanism that makes RAII complete: a `return`, a
`break`, falling off the end, and a `throw` all lead to the same destructor calls.

```
main()
 └ handle_request()         Guard g1;   <- ~g1 runs third
    └ parse()               Buffer b;   <- ~b  runs second
       └ read_field()       Lock  l;    <- ~l  runs first
             throw ParseError{...};
```

Without RAII, a thrown exception would skip your manual cleanup code entirely. This is why
"exceptions vs error codes" (module 10) is not really a choice in C++: once you use any library that
throws (and the standard library does, on allocation failure), only RAII keeps the program correct.

### 2.5 RAII beyond memory and files

| Resource | Standard RAII type |
|---|---|
| heap object | `std::unique_ptr<T>`, `std::shared_ptr<T>` (module 05) |
| array of T | `std::vector<T>` |
| string | `std::string` |
| mutex lock | `std::lock_guard`, `std::unique_lock`, `std::shared_lock` (04_Concurrency) |
| thread that must be joined | `std::jthread` (C++20) or your own wrapper |
| file stream | `std::ifstream` / `std::ofstream` |
| C `FILE*`, fd, socket, library handle | write a 20-line wrapper, or `unique_ptr` with a custom deleter |
| "undo this on exit" | a scope guard (exercise 4) |

## 3. Code

`code/m03_file_handle_raii.cpp` is `fopen`/`fclose` done properly:

```cpp
class FileHandle {
public:
    FileHandle(const char* path, const char* mode) : f_(std::fopen(path, mode)) {
        if (!f_) throw std::system_error(errno, std::generic_category(), std::string("fopen ") + path);
    }
    ~FileHandle() { if (f_) std::fclose(f_); }                  // runs on every exit path
    FileHandle(const FileHandle&) = delete;                      // one owner
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }   // ownership transfer
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) { if (f_) std::fclose(f_); f_ = o.f_; o.f_ = nullptr; }
        return *this;
    }
    std::FILE* get() const noexcept { return f_; }              // hand the raw handle to C APIs
private:
    std::FILE* f_;
};

void write_report(const char* path, bool fail_midway) {
    FileHandle out(path, "w");
    std::fputs("header\n", out.get());
    if (fail_midway) throw std::runtime_error("disk quota exceeded (simulated)");
    std::fputs("body\n", out.get());
}   // fclose happens here on both paths
```

`code/m03_socket_fd_raii.cpp` is the pattern tcpkv uses for every socket, a move-only fd owner:

```cpp
class SocketFd {
public:
    SocketFd() noexcept = default;
    explicit SocketFd(int fd) noexcept : fd_(fd) {}
    ~SocketFd() { reset(); }
    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;
    SocketFd(SocketFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    SocketFd& operator=(SocketFd&& o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    int  get() const noexcept   { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int  release() noexcept     { return std::exchange(fd_, -1); }  // opt out: give the fd back to manual code
    void reset(int fd = -1) noexcept { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
private:
    int fd_ = -1;
};

SocketFd make_listener(std::uint16_t port) {
    SocketFd s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) throw std::system_error(errno, std::generic_category(), "socket");
    // ... setsockopt, bind, listen; each failure throws and s closes itself ...
    return s;     // ownership travels to the caller
}
```

Note the `std::exchange(o.fd_, -1)` idiom: read the old value and store the new one in one line.
It is the standard way to write "steal and empty the source".

## 4. Under the hood (senior-level)

**Zero-cost exceptions.** GCC implements exceptions with tables, not with code on the normal path.
For each function, the compiler emits an entry in `.eh_frame` (how to restore registers and find the
caller's frame) and `.gcc_except_table` (which destructors to call for which range of instructions,
and which `catch` clauses apply). On the happy path there is **no** check, no flag, no extra
instruction: a function with RAII objects runs as fast as the C version without cleanup code.
When `throw` executes:

1. `__cxa_allocate_exception` gets memory for the exception object (heap, with a small emergency
   pool for `bad_alloc`), the object is copied/moved there.
2. `__cxa_throw` calls `_Unwind_RaiseException` (libgcc). Phase 1 ("search"): walk frames using
   `.eh_frame`, ask each frame's personality routine (`__gxx_personality_v0`) whether it has a
   matching handler. If none, `std::terminate`.
3. Phase 2 ("cleanup"): walk again; for each frame, jump to its *landing pad*, a block of code the
   compiler generated that runs the destructors of live locals and then calls `_Unwind_Resume`.
4. At the handler frame, control enters the `catch` block. `__cxa_begin_catch` / `__cxa_end_catch`
   manage the exception object's lifetime.

Costs: a throw is expensive (microseconds: two stack walks, table lookups, often a lock inside the
unwinder for `dl_iterate_phdr`). Exceptions are for exceptional paths, not for control flow in a hot loop.

**Landing pads are why destructors matter even without `try`.** A function with no `try` block still
has landing pads if it has objects with non-trivial destructors; the pads run the destructors and
resume unwinding. A function whose locals are all trivial has no landing pad and the unwinder skips
straight through it.

**`noexcept` on destructors.** Destructors are implicitly `noexcept`. If one throws during unwinding
(two exceptions in flight), `std::terminate` is called. This is why `~FileHandle` ignores `fclose`'s
return value: there is no good way to report it. If flush errors matter, offer an explicit
`close()` method that can throw, and let the destructor be the fallback.

**Why `-fno-exceptions` is not a free lunch.** Some embedded teams disable exceptions. Then
`new` failing calls `std::abort`, constructors cannot report failure, and you are back to two-phase
init. For a Linux server, keep exceptions on and use them for genuinely exceptional failures.

**Static vs dynamic cost of RAII.** The destructor call at scope end is a direct `call` (or inlined).
A `std::lock_guard` compiles to the same `pthread_mutex_lock`/`pthread_mutex_unlock` pair you would
write by hand; look at `g++ -O2 -S` output to confirm there is nothing else.

## 5. In production

- **Your C leak bugs, classified.** Early-return leaks, error-path double-frees, "forgot to close the
  cursor on the exception path", lock not released when a function bails out: all are the *same* bug
  class, and all vanish when each resource lives in an object. In code review, a raw `fopen`, `malloc`,
  `socket`, `pthread_mutex_lock` outside a wrapper's constructor is a finding.
- **Ownership transfer across layers.** `make_listener` returns a `SocketFd` by value: the caller
  now owns it, and the function cannot "forget" to hand it over. In C the same function returns an
  `int` and the documentation has to say who closes it.
- **Interfacing with C APIs that take ownership.** Use `release()` to hand the raw handle to code
  that will close it itself (e.g. passing an fd to `epoll` plus a struct that owns it). Document every
  `release()` call site; they are the only places the compiler stops protecting you.
- **The destructor-order guarantee is a tool.** Declare the lock guard *after* the object it protects,
  and a logger *before* the things that log, so they die in the right order.
- **3 AM debugging.** `lsof -p <pid>` shows thousands of open fds climbing over hours: some code path
  gets an fd from `accept()` and stores it as `int`. Fix: `SocketFd conn(::accept(...))` at the one
  place the fd is born; the leak is then impossible rather than merely fixed.
- **Valgrind / ASan after RAII.** Leaks found by `valgrind --leak-check=full` in RAII code almost
  always point to a `release()` without a matching manual close, or to a `shared_ptr` cycle (module 05).

## 6. Try it

1. **(easy)** Build `m03_file_handle_raii.cpp`. Add a third exit path (`return` early when the path
   contains "skip"). Done when you can show that `fclose` still runs without touching the function body.
2. **(easy)** Write `class DirHandle` wrapping `opendir`/`closedir` with the same shape as `FileHandle`.
   Done when it is move-only, non-copyable, and lists the entries of `/tmp`.
3. **(medium)** Rewrite the C `process()` function from section 2.1 in C++ with RAII. Count the
   lines before and after. Done when the C++ version has zero explicit cleanup calls.
4. **(medium)** Write a generic `ScopeGuard` that takes a lambda in its constructor and calls it in
   its destructor, with a `dismiss()` method. Use it to roll back a partially applied change
   (`map.erase(key)` if a later step fails). Done when the rollback runs on `throw` and is skipped
   after `dismiss()`.
5. **(hard)** Run `m03_socket_fd_raii.cpp` under `strace -e trace=socket,bind,listen,close` and
   confirm exactly one `close` per `socket`. Then add a bug (a copy of the fd into a second `int` that
   is also closed) and observe `EBADF`. Done when you can explain which line the kernel rejected and why
   `SocketFd` makes the bug unwritable.
6. **(hard)** Compile `m03_file_handle_raii.cpp` with `g++ -std=c++17 -O2 -S` and locate the landing
   pad for `write_report` (look for `.LLSDA`, `_Unwind_Resume`, and the `fclose` call reached only from
   the pad). Done when you can point at the normal-path code and show it contains no exception checks.

## 7. Interview questions

**Q: What is RAII in one sentence?**
Bind each resource to the lifetime of an object: acquire in the constructor, release in the
destructor, so the compiler releases it on every exit path, including exceptions.

**Q: Why is RAII more than "remember to call cleanup"?**
Because the destructor call is generated by the compiler at every scope exit, including the hidden
exits created by exceptions. Manual cleanup code is skipped by a `throw`; destructors are not.

**Q: What happens to local objects when an exception propagates through a function?**
Stack unwinding runs the destructors of all fully constructed locals in reverse construction order,
via compiler-generated landing pads, then continues unwinding to the next frame.

**Q: Why should a destructor never throw?**
If an exception is already propagating and a destructor throws a second one, the runtime calls
`std::terminate`. Destructors are implicitly `noexcept`; report cleanup failures through an explicit
`close()` method instead.

**Q: How much does exception support cost on the non-throwing path with GCC?**
Essentially nothing: GCC uses table-based ("zero-cost") unwinding. The tables live in `.eh_frame`
and `.gcc_except_table` and are consulted only when a throw happens. The throw itself costs
microseconds (two stack walks), so exceptions should not be used for routine control flow.

**Q: What are the three decisions every RAII class must make?**
How to acquire (constructor, and what to do on failure), how to release (destructor, handling the
empty state), and what copying means (deleted, deep copy, or shared). Leaving copy as the default is
the classic double-free bug.

**Q: You get an `int fd` from `accept()`. How do you prevent fd leaks in a server?**
Wrap it immediately in a move-only owner (`SocketFd conn(::accept(...))`) at the point of creation.
Pass the owner by value when transferring ownership, by reference when lending it, and call
`release()` only where a C API takes over ownership.

## 8. Recap

- RAII: constructor acquires, destructor releases; the compiler calls the destructor on every path.
- The destructor runs for `return`, `break`, end of scope, and during exception unwinding.
- An RAII class must define copy semantics explicitly: delete, deep copy, or share.
- Destructors must not throw and must tolerate an empty/moved-from state.
- `std::exchange(member, empty_value)` is the idiom for "steal and empty".
- GCC exceptions are zero-cost on the happy path, expensive when thrown; use them for exceptional cases.
- Landing pads in `.gcc_except_table` run destructors during unwinding.
- Standard RAII types already exist for memory, strings, locks and streams; write wrappers only for
  raw C handles.
- In review, any raw resource acquisition outside a wrapper's constructor is a finding.
