# 01. Processes, threads and the OS

**Time:** ~2.5 hours   **Prerequisites:** none

## 1. In plain words

A process is a running program with its own private memory. A thread is one line of execution
inside that process. All threads in a process see the same memory, the same open files, the same
heap. Each thread has only three things of its own: a stack, a set of CPU registers (including the
program counter), and a small block of bookkeeping the kernel keeps for scheduling it.

Analogy: a process is a kitchen. Threads are cooks. Every cook can reach every shelf and every pan
(shared memory). Each cook has their own hands and their own notepad (registers and stack). The
restaurant manager (the OS scheduler) decides which cook is standing at the stove at any moment,
and when the manager swaps one cook for another, the outgoing cook has to write down exactly what
they were doing (save registers) and the incoming cook has to read their own notes back (restore
registers). That swap is a context switch, and it is not free.

You already create threads with `CreateThread` on Windows. This module shows you what the kernel
does when you call it, what the POSIX and C++17 versions look like, and how to decide how many
cooks the kitchen should have.

## 2. The real thing

### 2.1 What the kernel keeps per thread

On both Linux and Windows the kernel has a structure per thread. Windows calls it the ETHREAD /
KTHREAD pair (the user-mode shadow is the TEB, Thread Environment Block); Linux calls it
`task_struct` (Linux does not distinguish processes from threads at this level; a thread is a task
that shares its address space with other tasks). Textbooks call it the TCB, Thread Control Block.

It contains, at minimum:

```
+---------------------------------------------+
| thread id                                   |
| state: running / ready / blocked / ...      |
| saved registers (when not running)          |
|   - program counter (RIP)                   |
|   - stack pointer (RSP)                     |
|   - general purpose regs, flags, FP/SIMD    |
| pointer to kernel stack (for syscalls)      |
| pointer to user stack base and size         |
| scheduling info: priority, time slice used, |
|   which CPU it last ran on (affinity, cache)|
| pending signals / APCs                      |
| pointer to the owning process (mm / EPROCESS)|
| thread-local storage block pointer          |
+---------------------------------------------+
```

The process holds what is shared: the page tables (address space), the file descriptor / handle
table, the heap, environment, signal handlers, and the list of threads.

### 2.2 Two stacks per thread

Each thread has a **user stack** (typical default: 8 MB on Linux, from `ulimit -s`; 1 MB on
Windows, from the PE header or `CreateThread` parameter) and a **kernel stack** (small, typically
16 KB on x86-64 Linux). When the thread makes a system call or takes an interrupt, the CPU
switches to the kernel stack. The user stack is only reserved virtual address space at first;
pages are committed as the thread actually touches them.

This matters for "how many threads": 10,000 threads with 8 MB stacks reserve 80 GB of virtual
address space. That is fine on 64-bit for reservation, but each thread touches at least one page,
and the kernel structures per thread are not free either.

### 2.3 Scheduling and the context switch

The scheduler picks which runnable thread gets each CPU. Linux uses CFS (Completely Fair
Scheduler) by default, which tracks virtual runtime per task and picks the one that has had the
least. Windows uses a priority-based preemptive scheduler with 32 priority levels and dynamic
boosts (e.g. a thread woken from a wait gets a temporary boost).

A **context switch** happens when the CPU moves from one thread to another. The steps:

1. Something triggers it: the timer interrupt says the time slice is over, the thread blocks on
   I/O or a lock, or a higher-priority thread becomes runnable.
2. Kernel saves the current thread's registers into its TCB.
3. Scheduler picks the next thread.
4. If the next thread belongs to a different process, the kernel loads a new page-table root
   (CR3 on x86). This flushes or tags the TLB, which is the expensive part. Switching between
   threads of the same process skips this.
5. Kernel restores the next thread's registers and returns to user mode.

Cost: the direct cost is on the order of 1 to 2 microseconds on modern x86 Linux (typical,
measured with tools such as `lmbench lat_ctx`; your numbers will differ). The indirect cost is
larger and harder to see: the new thread's working set is not in L1/L2 cache, so it runs slower
for a while. This is why a thread that blocks on a lock for 200 ns is worse off than one that
spins for 200 ns: blocking means two context switches plus cold caches.

### 2.4 User threads vs kernel threads

- **Kernel threads (1:1):** every thread you create is known to the kernel and scheduled by it.
  This is what pthreads on Linux (NPTL), Win32 threads, and `std::thread` all use. Blocking one
  thread in a syscall does not block the others.
- **User threads (M:N or N:1):** a library multiplexes many user-level "threads" onto fewer kernel
  threads. Go goroutines, Windows fibers, and most coroutine schedulers are examples. Switching
  is cheap (no kernel entry) but if one blocks in a syscall, the kernel thread under it blocks too
  unless the runtime intercepts the call.

For this course, "thread" means kernel thread. Coroutines come up in 08_Networking.

### 2.5 The three APIs side by side

| Operation | Win32 | POSIX (pthread) | C++17 `std::thread` |
|---|---|---|---|
| create | `CreateThread(NULL, stack, fn, arg, 0, &id)` | `pthread_create(&t, NULL, fn, arg)` | `std::thread t(fn, args...)` |
| entry signature | `DWORD WINAPI fn(LPVOID)` | `void* fn(void*)` | any callable, any args |
| wait for finish | `WaitForSingleObject(h, INFINITE)` then `CloseHandle(h)` | `pthread_join(t, &ret)` | `t.join()` |
| fire and forget | `CloseHandle(h)` immediately | `pthread_detach(t)` | `t.detach()` |
| current id | `GetCurrentThreadId()` | `pthread_self()` | `std::this_thread::get_id()` |
| sleep | `Sleep(ms)` | `nanosleep` / `usleep` | `std::this_thread::sleep_for(10ms)` |
| yield | `SwitchToThread()` | `sched_yield()` | `std::this_thread::yield()` |
| return value | thread exit code via `GetExitCodeThread` | `void*` from fn | use `std::future` (module 09) |
| hardware threads | `GetSystemInfo` | `sysconf(_SC_NPROCESSORS_ONLN)` | `std::thread::hardware_concurrency()` |

Two things that will feel different coming from Win32:

1. **A `std::thread` must be joined or detached before it is destroyed.** If a `std::thread`
   object that is still joinable goes out of scope, the destructor calls `std::terminate()`.
   There is no silent "handle leak" equivalent of forgetting `CloseHandle`. This is deliberate:
   the committee decided a crash is better than a thread you forgot about writing to a destroyed
   object. C++20 adds `std::jthread` which joins in its destructor; in C++17 you write a small
   RAII wrapper (shown below).
2. **Arguments are copied into the thread** by value unless you wrap them with `std::ref`. The
   C habit of passing `&local_struct` and hoping the thread reads it before the function returns
   is still possible (pass a pointer) but no longer the default.

On Linux, `std::thread` is a thin wrapper over `pthread_create`, which is a thin wrapper over the
`clone()` syscall with flags `CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD`
and more: "make a new task that shares my memory, files and signal handlers". On Windows, MSVC's
`std::thread` calls `_beginthreadex`, which calls `CreateThread` after setting up the CRT's
per-thread state.

### 2.6 Thread-local storage

Sometimes you want a global that is separate per thread: an error code (`errno` is exactly
this), a per-thread RNG, a per-thread scratch buffer, a per-thread log prefix.

| Win32 | POSIX | C++11 |
|---|---|---|
| `__declspec(thread) int x;` | `__thread int x;` (GCC) | `thread_local int x;` |
| `TlsAlloc` / `TlsSetValue` / `TlsGetValue` | `pthread_key_create` / `pthread_setspecific` | (not needed; `thread_local` covers it) |

`thread_local` works on any type, including types with constructors and destructors. The object is
constructed the first time that thread touches it (or at thread start for trivially constructible
types) and destroyed when the thread exits. Under the hood each thread's TLS block is reached
through a segment register (`%fs` on x86-64 Linux, `%gs` TEB on Windows) so access costs one extra
indirection, not a lock.

### 2.7 How many threads?

Rules that hold up in practice:

- **CPU-bound work:** as many threads as hardware threads (`hardware_concurrency()`), sometimes
  one fewer to leave room for the OS and the main thread. More than that only adds context
  switches and cache thrashing.
- **Blocking I/O-bound work (your RPC server waiting on the database):** more threads than cores,
  because most of them are asleep most of the time. A starting estimate is
  `cores * (1 + wait_time / compute_time)`; for a request that spends 20 ms waiting on YugabyteDB
  and 1 ms computing, that is 21 threads per core. In practice you cap it (64, 128, 256) and
  measure, because the database has its own connection limit and every extra thread is a stack and
  a scheduler entry.
- **Never one thread per request without a cap.** A burst of 10,000 requests must not become
  10,000 threads. That is the whole reason your server has a *pooled* request model; module 09
  builds one.
- Measure. Thread count is a tuning knob, so read it from config, not a `#define`.

## 3. Code

File: `code/m01_threads_basics.cpp`

```cpp
#include <thread>
#include <vector>
#include <iostream>
#include <mutex>
#include <chrono>

// A tiny RAII wrapper so a thread can never be destroyed while joinable.
// C++20 has std::jthread; in C++17 we write this ourselves.
class joining_thread {
    std::thread t_;
public:
    template <class F, class... Args>
    explicit joining_thread(F&& f, Args&&... args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...) {}
    ~joining_thread() { if (t_.joinable()) t_.join(); }   // WHY: never terminate() by accident
    joining_thread(const joining_thread&) = delete;        // threads are move-only
    joining_thread& operator=(const joining_thread&) = delete;
    joining_thread(joining_thread&&) = default;
    joining_thread& operator=(joining_thread&&) = default;
    std::thread& get() { return t_; }
};

// thread_local: one instance per thread, constructed on first use in that thread.
thread_local int call_count = 0;

std::mutex cout_mtx;   // std::cout itself will not interleave characters, but lines can; serialize.

void worker(int id, int iterations) {
    for (int i = 0; i < iterations; ++i) ++call_count;   // no race: call_count is per-thread
    std::lock_guard<std::mutex> lk(cout_mtx);
    std::cout << "worker " << id << " os-tid " << std::this_thread::get_id()
              << " local count " << call_count << '\n';
}

int main() {
    unsigned hw = std::thread::hardware_concurrency();   // 0 means "unknown"
    std::cout << "hardware threads: " << hw << '\n';

    // 1. Create and join. Arguments are copied into the new thread.
    std::vector<joining_thread> pool;
    for (int i = 0; i < 4; ++i) pool.emplace_back(worker, i, 1000 * (i + 1));
    pool.clear();   // joins all four; destructors run here

    // 2. Pass by reference on purpose with std::ref. Without it, the thread gets a copy of
    //    'total' and the main thread never sees the result.
    long total = 0;
    std::thread t([](long& out) {
        for (int i = 1; i <= 100; ++i) out += i;
    }, std::ref(total));
    t.join();
    std::cout << "sum via ref: " << total << '\n';   // 5050

    // 3. Detach: fire and forget. The thread must not touch anything that main might destroy.
    //    Here it only uses its own stack and a copy of a value, which is the only safe pattern.
    std::thread bg([](int v) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)v;   // pretend to do work
    }, 42);
    bg.detach();
    // main could exit before bg finishes; the process ends and bg is killed. Acceptable only
    // because bg owns nothing.

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout << "main thread local count: " << call_count << '\n';   // 0: never incremented here
    return 0;
}
```

Build and run on WSL:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m01_threads_basics.cpp -o m01 && ./m01
```

The equivalent in C with Win32 and in C with pthreads is in module 13 and in
`code/m13_win32_threads.c`.

## 4. Under the hood (senior-level)

### What `std::thread t(f, a, b)` does on Linux with libstdc++

1. Packs `f`, `a`, `b` (decay-copied) into a heap-allocated state object.
2. Calls `pthread_create(&handle, nullptr, execute_native_thread_routine, state)`.
3. glibc's `pthread_create` allocates the stack with `mmap` (anonymous, with a guard page at
   the bottom marked PROT_NONE so overflow faults instead of corrupting the neighbour), sets up
   the TLS block and the `struct pthread` descriptor at the top of that stack, then calls
   `clone()` with the CLONE_THREAD family of flags.
4. The kernel creates a new `task_struct`, copies the parent's register state, points the new
   task's stack pointer at the mmap'd region, marks it runnable and returns the new TID.
5. The new task is picked by the scheduler, lands in `start_thread`, which calls your `f(a, b)`.
   When `f` returns, the wrapper runs TLS destructors and calls the `exit` syscall for that
   thread (`exit`, not `exit_group`; `exit_group` would kill the whole process).

`join()` calls `pthread_join`, which does a `futex` wait on the thread's TID field until the kernel
clears it on thread exit (the `CLONE_CHILD_CLEARTID` flag arranges this). Module 03 explains futex.

### Stack guard pages and overflow

The guard page explains a crash signature you will recognise: a thread with deep recursion or a
large local array dies with SIGSEGV and the faulting address is just below the thread's stack. On
Windows the same event produces `STATUS_STACK_OVERFLOW` (0xC00000FD). Fix: smaller frames, heap
allocation, or a larger stack (`pthread_attr_setstacksize`; `CreateThread` stack parameter;
`std::thread` has no portable way, which is one of its real gaps).

### Why too many threads hurt even when idle

Each thread costs: the TCB (several KB), kernel stack (16 KB), at least one committed user-stack
page (4 KB) plus the TLS block, an entry in scheduler run-queues or wait-queues, and a slot in
every lock's wait list it touches. Idle threads cost nothing in CPU but they still pay the
cold-cache penalty when they wake, and 5,000 of them make `thread apply all bt` in gdb an
afternoon's reading.

### Thread exit and resources

On Linux, a thread's resources (kernel stack, task_struct) are released when it is joined or if it
was detached. A thread that exits and is never joined is a "zombie thread" until the process ends.
On Windows, the thread object persists until every handle to it is closed, which is why you must
`CloseHandle` even after `WaitForSingleObject` returns. `std::thread::join()` does both the wait
and the release in one call.

## 5. In production

- **Your server's startup.** Your Win32 server creates N worker threads at startup, each blocking
  on a wait object for work. In C++17 terms that is a `std::vector<std::thread>` plus a
  `std::condition_variable`. Module 09 writes it. The portable description of your design is
  "fixed-size pool of kernel threads, blocking work queue".
- **Thread naming.** Give threads names: `pthread_setname_np(pthread_self(), "worker-3")` on
  Linux, `SetThreadDescription` on Windows 10+. Then `thread apply all bt` and Process Explorer
  show "worker-3" instead of a TID. This is the single cheapest debugging investment in a
  multithreaded server.
- **Detached threads and shutdown.** A detached thread that touches a global `std::map` while
  `main` returns and static destructors run is a use-after-free that happens only at shutdown
  and only sometimes. Rule: in a server, never detach. Keep every thread's handle and join all of
  them in a known order at shutdown.
- **Thread count from config.** You will be asked "how did you size the pool?" The senior answer:
  started at 2x cores because requests are mostly database-bound, measured throughput and p99
  latency under load, found the knee, and set it with headroom. The junior answer is "32, it
  seemed fine".
- **3 AM symptom:** CPU at 100% with throughput falling. Often too many runnable threads
  thrashing caches, or a spin-wait that should be a blocking wait. `top -H` (Linux) or Process
  Explorer threads tab (Windows) shows which threads are burning CPU.

## 6. Try it

1. **(easy)** Compile and run `m01_threads_basics.cpp`. Change the number of workers to 100 and
   observe that output lines never interleave mid-line because of `cout_mtx`. Remove the lock
   and run several times; find one interleaved line. Done when you have seen the interleaving.
2. **(easy)** Write a program that prints `std::thread::hardware_concurrency()` and, on Linux,
   also reads `/proc/self/status` after creating 50 threads and reports the `Threads:` line.
   Done when the number is 51.
3. **(medium)** Remove the `joining_thread` wrapper and let a joinable `std::thread` go out of
   scope. Run it; observe `terminate called without an active exception`. Then write a one-line
   explanation of why the committee chose terminate over detach. Done when the explanation
   mentions dangling references.
4. **(medium)** Measure thread creation cost: create and join 10,000 threads one after another
   with `std::chrono::steady_clock`. Report microseconds per create+join. Then compare with the
   time to push 10,000 items through a single reused thread via a queue (you may use the queue
   from module 05 after you reach it). Done when you have both numbers and a sentence on what
   they imply for a request server.
5. **(hard)** Write a `thread_local` struct with a constructor and destructor that print the
   thread id. Create three threads that each touch it. Verify construction happens once per
   thread and destruction happens at thread exit, not at program exit. Done when the output shows
   three constructions and three destructions before `main` prints "exiting".

## 7. Interview questions

**Q: What is the difference between a process and a thread?**
A process owns an address space, file handles, and at least one thread. Threads inside a process
share all of that and own only their stack, registers and scheduling state. Creating a thread is
cheaper than a process and communication is through shared memory instead of IPC, but one thread's
bug can corrupt every other thread's data.

**Q: What does a context switch cost, and why?**
Directly, saving and restoring registers plus the scheduler's decision, on the order of a
microsecond on modern x86 (typical). Indirectly and usually larger, the incoming thread's data and
code are not in L1/L2 cache and, if it is a different process, the TLB has been flushed. That is
why spinning briefly can beat blocking for very short critical sections.

**Q: What happens if a `std::thread` is destroyed while still joinable?**
`std::terminate()` is called. The standard chose this over implicit detach because a detached
thread holding references to the destroyed scope is a use-after-free waiting to happen. You must
call `join()` or `detach()` first; in practice wrap the thread in an RAII type that joins.

**Q: How is `std::thread` implemented on Linux?**
It calls `pthread_create`, which `mmap`s a stack with a guard page, sets up TLS, and issues the
`clone()` syscall with flags that share the address space, file table and signal handlers. The
kernel makes a new `task_struct`; Linux does not have a separate "thread" object, just tasks that
share resources.

**Q: How many threads should a server use?**
CPU-bound: one per hardware thread. Blocking-I/O-bound: more, roughly cores times
(1 + wait/compute), capped and measured, because beyond the knee you only add context switches,
memory and lock contention. Never unbounded, never one per request without a cap.

**Q: What is `thread_local` and how is it accessed?**
A storage duration where each thread gets its own instance, constructed on first use in that
thread and destroyed at thread exit. Access goes through a per-thread base register (`%fs` on
x86-64 Linux) plus an offset, so it is a memory load with one extra indirection, not a lock or a
hash lookup. `errno` is the classic example.

## 8. Recap

- A thread owns a stack, registers and a kernel control block; everything else is shared with the
  process.
- Context switch cost is roughly a microsecond directly and more indirectly through cold caches.
- pthreads, Win32 threads and `std::thread` are all 1:1 kernel threads; `std::thread` wraps
  `pthread_create` on Linux and `_beginthreadex` on Windows.
- A joinable `std::thread` going out of scope calls `std::terminate`; wrap it in an RAII joiner.
- `std::thread` copies its arguments; use `std::ref` or pointers to share deliberately.
- Each thread has a user stack with a guard page and a small kernel stack; overflow gives
  SIGSEGV just below the stack or `STATUS_STACK_OVERFLOW` on Windows.
- `thread_local` replaces `__declspec(thread)` and `TlsAlloc`, works with constructors, and costs
  one segment-relative load.
- Size thread pools by measurement; CPU-bound means one per core, I/O-bound means more but
  always capped.
- Name your threads. Never detach in a server.
