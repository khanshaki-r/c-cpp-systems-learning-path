# Step 02. RAII FileDescriptor and Socket wrappers (move-only), with unit tests

**Time:** ~7 hours   **Week:** 1 (Oct 6 – Oct 12)
**Read first:** 02_Modern_CPP: RAII, destructors, the rule of five, move semantics and
`std::move`, `noexcept`, `explicit`, deleted functions, exceptions and `std::system_error`.
08_Networking_Distributed: the BSD socket API (`socket`, `bind`, `listen`, `accept`,
`connect`, `read`, `send`, `poll`), `getaddrinfo`, `EINTR`, partial writes, `SO_REUSEADDR`,
`TCP_NODELAY`. 06_Linux_Toolchain_DevOps: file descriptors, `O_CLOEXEC`, `strace`.

## 1. Goal

Two classes that make it impossible to leak a file descriptor or to forget the three things
every C network programmer has forgotten at least once: retrying on `EINTR`, looping on
partial writes, and closing on every exit path.

In plain words: in a C server you write `close(fd)` in every error branch and
hoped you did not miss one. In C++ you write the close once, in a destructor, and the
compiler calls it for you on every path including exceptions. That is RAII (Resource
Acquisition Is Initialization): a resource is owned by an object, and the object's lifetime
is the resource's lifetime.

## 2. Design

```
FileDescriptor                      Socket
  int fd_ = -1                        FileDescriptor fd_
  explicit FileDescriptor(int)        static listen_tcp(host, port, backlog)
  ~FileDescriptor() -> close          static connect_tcp(host, port)
  copy: deleted                       accept() -> Socket
  move: transfers, source = -1        read_some(buf, len) -> 0 on EOF
  get(), valid(), release(), reset()  write_all(data, len)   loops partial sends
                                      wait_readable(timeout) poll()
                                      shutdown_both(), local_port(), peer_address()
```

Decisions worth remembering:

* **Move-only.** Two objects owning the same descriptor would close it twice; the second
  `close` might hit a descriptor the kernel has just handed to another thread. Deleting the
  copy constructor makes that a compile error.
* **`close()` is not retried on EINTR.** On Linux, if `close` returns `EINTR` the descriptor
  is already gone. Retrying could close somebody else's new descriptor. (This is the opposite
  of `read`/`write`, and it is a classic interview question.)
* **`write_all` uses `send(..., MSG_NOSIGNAL)`.** Writing to a socket whose peer has gone
  away raises `SIGPIPE`, which kills the process by default. `MSG_NOSIGNAL` turns that into
  `EPIPE`, which we can handle per connection.
* **Errors are exceptions** (`std::system_error` carrying `errno`). Inside a constructor you
  cannot return an error code, and a `Socket` that failed to connect must not exist.
* **`accept4`/`SOCK_CLOEXEC`/`pipe2(O_CLOEXEC)`**: descriptors are created close-on-exec
  atomically, so a `fork()` in another thread can never leak them to a child.

## 3. Files to create

### `include/tcpkv/fd.hpp`

Identical to `code/include/tcpkv/fd.hpp`:

```cpp
#pragma once

#include <utility>

namespace tcpkv {

// Throws std::system_error built from an errno value.
// Every wrapper that talks to the kernel uses this so error reporting is uniform.
[[noreturn]] void throw_system_error(int err, const char* what);

// Owning handle for one POSIX file descriptor.
//
// Rules:
//   * exactly one owner: copying is deleted, moving transfers ownership
//   * the destructor closes the descriptor exactly once; a moved-from handle holds -1
//   * close() is NOT retried on EINTR: on Linux the descriptor is already released
//     when close() returns EINTR, and retrying could close a descriptor that another
//     thread has just been handed by the kernel
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    // Gives up ownership without closing. The caller now owns the raw descriptor.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // Closes the current descriptor (if any) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

    friend void swap(FileDescriptor& a, FileDescriptor& b) noexcept { std::swap(a.fd_, b.fd_); }

private:
    int fd_ = -1;
};

// Creates a pipe with O_CLOEXEC on both ends. Returns {read_end, write_end}.
// Throws std::system_error on failure.
std::pair<FileDescriptor, FileDescriptor> make_pipe();

// Sets or clears O_NONBLOCK on a descriptor. Throws std::system_error on failure.
void set_nonblocking(int fd, bool enabled);

}  // namespace tcpkv
```

Line-by-line, the parts that matter:

* `explicit FileDescriptor(int)`: without `explicit`, `FileDescriptor fd = 3;` would compile
  and silently adopt descriptor 3. Adopting a descriptor is a deliberate act.
* The move constructor calls `other.release()`, so the source is left holding `-1` and its
  destructor does nothing. That is what "transfer ownership" means mechanically.
* Move assignment guards `this != &other`. Self-move-assignment is rare but legal, and
  `reset(release())` on the same object would close the descriptor and then adopt the closed
  value.
* Everything is `noexcept`. Containers (`std::vector<Socket>`) only move elements during
  reallocation if the move constructor is `noexcept`; otherwise they copy, and here copying
  is deleted, so without `noexcept` the vector would not compile.

### `src/fd.cpp`

```cpp
#include "tcpkv/fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace tcpkv {

void throw_system_error(int err, const char* what) {
    // generic_category() is the category for POSIX errno values; its messages come from strerror.
    throw std::system_error(err, std::generic_category(), what);
}

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
        // The return value is deliberately ignored. There is nothing useful to do with an
        // error from close() in a destructor, and on Linux the descriptor is gone either way.
        ::close(fd_);
    }
    fd_ = fd;
}

std::pair<FileDescriptor, FileDescriptor> make_pipe() {
    int fds[2] = {-1, -1};
    // pipe2 with O_CLOEXEC is atomic: no window in which a fork()ed child inherits the ends.
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw_system_error(errno, "pipe2");
    }
    return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

void set_nonblocking(int fd, bool enabled) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        throw_system_error(errno, "fcntl(F_GETFL)");
    }
    const int wanted = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (wanted != flags && ::fcntl(fd, F_SETFL, wanted) < 0) {
        throw_system_error(errno, "fcntl(F_SETFL)");
    }
}

}  // namespace tcpkv
```

### `include/tcpkv/socket.hpp`

Identical to `code/include/tcpkv/socket.hpp`.

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "tcpkv/fd.hpp"

namespace tcpkv {

// A TCP socket built on FileDescriptor. Move-only, like the descriptor it owns.
//
// Every blocking call retries on EINTR. write_all() loops until every byte is
// handed to the kernel, because send() on a stream socket may accept fewer bytes
// than asked. All failures are reported as std::system_error carrying errno.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    ~Socket() = default;

    // Resolves `host` (empty means "any address"), binds, listens. `port` 0 picks a free port.
    static Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog);

    // Resolves and connects. Tries every address returned by getaddrinfo in order.
    static Socket connect_tcp(const std::string& host, std::uint16_t port);

    // Blocks until a client connects (unless the listener is non-blocking, in which case
    // it throws std::system_error with EAGAIN when nothing is pending).
    Socket accept();

    // Returns the number of bytes read; 0 means the peer closed its side.
    std::size_t read_some(char* buf, std::size_t len);

    // Writes all bytes or throws. Uses MSG_NOSIGNAL so a dead peer gives EPIPE, not SIGPIPE.
    void write_all(const char* data, std::size_t len);
    void write_all(std::string_view data) { write_all(data.data(), data.size()); }

    // Waits up to `timeout` for the socket to become readable (data, EOF or error).
    // Returns false on timeout. EINTR is reported as a timeout so callers re-check their state.
    bool wait_readable(std::chrono::milliseconds timeout);

    // shutdown(SHUT_RDWR). Errors (e.g. ENOTCONN) are ignored: this is a courtesy to the peer.
    void shutdown_both() noexcept;

    std::uint16_t local_port() const;
    std::string peer_address() const noexcept;  // "ip:port", or "?" if it cannot be determined

    void set_nodelay(bool enabled);
    void set_nonblocking(bool enabled);

    int fd() const noexcept { return fd_.get(); }
    bool valid() const noexcept { return fd_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

private:
    FileDescriptor fd_;
};

}  // namespace tcpkv
```

Notice that `Socket` has *no* user-written destructor, move constructor or move assignment.
It contains one `FileDescriptor`, and the compiler-generated versions do the right thing by
composing it. This is the rule of zero: if every member manages itself, the class needs no
special member functions. `Socket` only deletes copying so the intent is explicit.

### `src/socket.cpp`

Identical to `code/src/socket.cpp` (about 230 lines). Type it in from there. The parts to
understand deeply:

```cpp
// RAII for the linked list returned by getaddrinfo(). Move-only: a copy would free twice.
struct AddrInfoList {
    addrinfo* head = nullptr;

    AddrInfoList() = default;
    AddrInfoList(const AddrInfoList&) = delete;
    AddrInfoList& operator=(const AddrInfoList&) = delete;
    AddrInfoList(AddrInfoList&& other) noexcept : head(other.head) { other.head = nullptr; }
    AddrInfoList& operator=(AddrInfoList&&) = delete;
    ~AddrInfoList() {
        if (head != nullptr) {
            ::freeaddrinfo(head);
        }
    }
};
```

Even a helper that lives for ten lines gets a destructor. The `getaddrinfo` result is a heap
list; any `throw` between the call and `freeaddrinfo` would leak it. With this struct it cannot.

```cpp
std::size_t Socket::read_some(char* buf, std::size_t len) {
    for (;;) {
        const ssize_t n = ::read(fd_.get(), buf, len);
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        throw_system_error(errno, "read");
    }
}

void Socket::write_all(const char* data, std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ::send(fd_.get(), data + offset, len - offset, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error(errno, "send");
        }
        offset += static_cast<std::size_t>(n);
    }
}
```

`read` returns 0 only at EOF; `send` on a blocking socket can return less than `len` when the
socket buffer is nearly full. The 4 MiB test below exists to force that path.

```cpp
std::uint16_t Socket::local_port() const {
    sockaddr_storage storage{};
    socklen_t len = sizeof storage;
    if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
        throw_system_error(errno, "getsockname");
    }
    // memcpy instead of a pointer cast: sockaddr_storage and sockaddr_in are different types
    // and reading one through a pointer to the other is undefined behaviour.
    if (storage.ss_family == AF_INET) {
        sockaddr_in in4{};
        std::memcpy(&in4, &storage, sizeof in4);
        return ntohs(in4.sin_port);
    }
    ...
```

The `reinterpret_cast<sockaddr*>` into the syscall is the API's sanctioned use; reading the
port back through `((sockaddr_in*)&storage)->sin_port` is a strict-aliasing violation that
UBSan and optimising compilers will eventually punish. `memcpy` is free after optimisation.

### `tests/fd_test.cpp`

Identical to `code/tests/fd_test.cpp`. The core idea: ask the kernel whether a descriptor is
open.

```cpp
// fcntl(F_GETFD) is the cheapest way to ask the kernel "is this descriptor still open?".
bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(FileDescriptor, ClosesOnDestruction) {
    int raw = -1;
    {
        const std::pair<FileDescriptor, FileDescriptor> ends = tcpkv::make_pipe();
        raw = ends.first.get();
        ASSERT_TRUE(fd_is_open(raw));
    }
    EXPECT_FALSE(fd_is_open(raw));
}
```

Other tests: move constructor leaves the source invalid and the descriptor open; move
assignment closes the old descriptor; `release` does not close; `reset` closes and adopts;
pipe ends have `FD_CLOEXEC`; `set_nonblocking` toggles `O_NONBLOCK`; listen on port 0 gives a
real port; echo over loopback; `read_some` returns 0 after peer shutdown; `wait_readable`
times out and then wakes; a 4 MiB write arrives intact (with a reader thread, or both sides
would block); connecting to a closed port throws; non-blocking `accept` with no client throws
`EAGAIN`.

### `CMakeLists.txt` changes

Replace the `hello_test` block with the library and the fd test. Delete `tests/hello_test.cpp`.

```cmake
# ---------------------------------------------------------------------------------------------
# The library: everything except main()
# ---------------------------------------------------------------------------------------------
add_library(tcpkv STATIC
    src/fd.cpp
    src/socket.cpp)
target_include_directories(tcpkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tcpkv
    PUBLIC Threads::Threads
    PRIVATE tcpkv_warnings)
```

and in the tests block:

```cmake
    foreach(name IN ITEMS fd)
        add_executable(${name}_test tests/${name}_test.cpp)
        target_link_libraries(${name}_test PRIVATE tcpkv GTest::gtest_main tcpkv_warnings)
        gtest_discover_tests(${name}_test
            DISCOVERY_TIMEOUT 120
            PROPERTIES TIMEOUT 300)
    endforeach()
```

Each later step adds a name to the `foreach` list.

## 4. Build and run

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
```

Expected: 19 tests, all pass, under both presets. ASan with `detect_leaks=1` would report a
leaked `addrinfo` list or a leaked descriptor's heap bookkeeping; it reports nothing.

## 5. Test it by hand

Watch the syscalls to see RAII in action:

```bash
strace -f -e trace=socket,bind,listen,accept4,connect,close,pipe2 ./build/debug/fd_test \
    --gtest_filter='Socket.AcceptedConnectionEchoesBytes' 2>&1 | grep -v '^+++'
```

You should see exactly three `socket`/`accept4` calls and exactly three `close` calls for
that test, in destructor order (reverse of declaration). No `close` is written anywhere in
the test file; the destructors did it.

Also try `ulimit -n 64` then run `fd_test` in a loop; if any test leaked descriptors the later
tests would fail with `EMFILE`.

## 6. Commit

```bash
git rm tests/hello_test.cpp
git add -A
git commit -m "Add RAII FileDescriptor and Socket wrappers with tests

- FileDescriptor: move-only owner of one fd; closes exactly once; no EINTR retry on close
- Socket: listen/connect via getaddrinfo, accept4(SOCK_CLOEXEC), EINTR-safe read,
  partial-write-safe send with MSG_NOSIGNAL, poll-based wait_readable
- make_pipe (pipe2 O_CLOEXEC) and set_nonblocking helpers
- 19 tests incl. kernel-verified close, 4 MiB write, timeout, EAGAIN on non-blocking accept"
git push
```

## 7. What you learned, and what to say in an interview

* **RAII** turns "remember to close on every path" into "cannot forget". The destructor runs
  on return, on `break`, on `throw`, on scope exit after a moved-from object.
* **Move-only types** encode "exactly one owner" in the type system. The compiler, not a
  code review, catches the double close.
* **Rule of zero**: `Socket` has no special members because `FileDescriptor` already behaves.
* **`noexcept` on moves** is not decoration; standard containers check it.
* **The three syscall gotchas**: `EINTR` on read/accept/poll (retry), partial `send` (loop),
  `close` after `EINTR` (do not retry). Plus `SIGPIPE` (`MSG_NOSIGNAL`), `SO_REUSEADDR`
  (restart during `TIME_WAIT`), and `CLOEXEC` (atomic, not a separate `fcntl`).
* **Strict aliasing**: `memcpy` into the right `sockaddr_*` type instead of casting pointers.

Interview sentence: "Every descriptor in tcpkv is owned by a `FileDescriptor`; there is one
`close()` call in the whole codebase, in its `reset()`. The test for it asks the kernel via
`fcntl(F_GETFD)` whether the descriptor is really gone after the destructor runs."
