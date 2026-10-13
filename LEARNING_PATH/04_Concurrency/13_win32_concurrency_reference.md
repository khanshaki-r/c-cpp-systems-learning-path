# 13. Win32 concurrency reference

**Time:** ~2 hours   **Prerequisites:** 03, 05, 07, 09 (this module is a reference; read it once, return to it)

## 1. In plain words

This is the module that lets you describe your existing work in portable terms. Every primitive
your server uses has a standard C++17 equivalent, or a documented reason why it does not. When an
interviewer who has never touched Windows asks "what synchronisation did you use?", you answer
"a user-mode mutex with a spin phase for the connection table, a reader-writer lock for the master
tables, condition variables for the request queue, and a counting semaphore to bound in-flight
requests" rather than "CRITICAL_SECTION, SRWLOCK, Events and a Semaphore". Same facts, understood
by everyone.

Analogy: you speak one dialect fluently. This module is the phrasebook that maps each word to the
standard language, with notes on the words that have no exact translation.

The Win32 `.c` examples in `code/` are Windows-only. Build them with MSVC (`cl /W4 /O2 file.c`) or
MinGW GCC. They are not part of the WSL build.

## 2. The real thing

### 2.1 The side-by-side table

| Concept | Win32 | C++17 standard | POSIX | Notes |
|---|---|---|---|---|
| create thread | `CreateThread`, `_beginthreadex` | `std::thread` | `pthread_create` | use `_beginthreadex` from C code so the CRT's per-thread state is initialised |
| wait for thread | `WaitForSingleObject(h, INFINITE)` + `CloseHandle(h)` | `thread::join()` | `pthread_join` | Windows thread objects live until the last handle closes |
| detach | `CloseHandle(h)` | `thread::detach()` | `pthread_detach` | |
| thread id | `GetCurrentThreadId()` | `std::this_thread::get_id()` | `pthread_self()` / `gettid()` | |
| thread name | `SetThreadDescription` (Win10+) | none | `pthread_setname_np` | shows in debuggers; do it |
| stack size | `CreateThread` param | none (gap) | `pthread_attr_setstacksize` | |
| TLS | `__declspec(thread)`, `TlsAlloc/TlsGetValue` | `thread_local` | `__thread`, `pthread_key_create` | |
| sleep | `Sleep(ms)` | `std::this_thread::sleep_for` | `nanosleep` | `Sleep(0)` yields; `Sleep(1)` is ~1 ms or the timer resolution |
| yield | `SwitchToThread()` | `std::this_thread::yield()` | `sched_yield` | |
| intra-process mutex | `CRITICAL_SECTION` | `std::mutex` / `std::recursive_mutex` | `pthread_mutex_t` | CS is recursive and can spin; `std::mutex` is neither |
| mutex try | `TryEnterCriticalSection` | `try_lock()` | `pthread_mutex_trylock` | |
| cross-process mutex | `CreateMutex` (kernel object) | none | named `sem_open` or `pthread_mutex` in shared memory with `PTHREAD_PROCESS_SHARED` | kernel object: syscall per acquire, abandonment detection, waitable with WFMO |
| timed mutex | `WaitForSingleObject(hMutex, ms)` | `std::timed_mutex::try_lock_for` | `pthread_mutex_timedlock` | |
| reader-writer lock | `SRWLOCK` | `std::shared_mutex` | `pthread_rwlock_t` | SRWLOCK: pointer-sized, static init, no recursion, no upgrade, does not record owner |
| condition variable | `CONDITION_VARIABLE` + `SleepConditionVariableCS/SRW` | `std::condition_variable` | `pthread_cond_t` | same spurious-wakeup and predicate-loop rules |
| notify one / all | `WakeConditionVariable` / `WakeAllConditionVariable` | `notify_one` / `notify_all` | `pthread_cond_signal` / `broadcast` | |
| manual-reset event | `CreateEvent(.., TRUE, ..)` | none; `mutex + bool + condition_variable`, or `std::promise<void>` / `shared_future<void>` for one-shot | none | stateful: a set with no waiter is remembered |
| auto-reset event | `CreateEvent(.., FALSE, ..)` | none; binary semaphore from `mutex + int + condition_variable` | none | coalesces back-to-back sets: two `SetEvent`s may wake one waiter |
| counting semaphore | `CreateSemaphore` / `ReleaseSemaphore` | `std::counting_semaphore` (C++20); build from mutex + cv in C++17 | `sem_t` | |
| wait for any / all of N | `WaitForMultipleObjects(n, h, bWaitAll, ms)` | none | none (`select`/`poll`/`epoll` for fds only) | single queue or composite predicate instead |
| alertable wait / APC | `WaitForSingleObjectEx(.., TRUE)`, `QueueUserAPC` | none | signals (loosely) | |
| one-time init | `InitOnceExecuteOnce` + `INIT_ONCE` | `std::call_once`, function-local static | `pthread_once` | |
| atomic add | `InterlockedIncrement`, `InterlockedExchangeAdd` | `atomic::fetch_add` (seq_cst) | `__atomic_fetch_add` | un-suffixed Interlocked = full barrier |
| atomic exchange | `InterlockedExchange` | `atomic::exchange` | | |
| CAS | `InterlockedCompareExchange(&v, desired, expected)` returns old | `compare_exchange_strong(expected&, desired)` returns bool, updates `expected` | | note argument order and return convention differ |
| 128-bit CAS | `InterlockedCompareExchange128` | `atomic<16-byte struct>` with `-mcx16` | | |
| acquire/release variants | `InterlockedIncrementAcquire/Release`, `*NoFence` | `memory_order_acquire/release/relaxed` | | matter on ARM64 Windows |
| full fence | `MemoryBarrier()` | `atomic_thread_fence(seq_cst)` | `__sync_synchronize` | |
| compiler-only fence | `_ReadWriteBarrier()` (deprecated) | `atomic_signal_fence` | `asm volatile("":::"memory")` | |
| `volatile` semantics | acquire/release under `/volatile:ms` (x86 default) | none; plain volatile is not atomic | none | portable code must not rely on it |
| futex | `WaitOnAddress` / `WakeByAddressSingle/All` (Win8+) | none directly; underlies `std::mutex` on Linux | `futex(2)` | |
| thread pool | `CreateThreadpool`, `CreateThreadpoolWork`, `SubmitThreadpoolWork`, `TrySubmitThreadpoolCallback` | none (module 09 builds one) | none | Vista+ pool is a good default on Windows |
| timers | `CreateThreadpoolTimer`, `CreateWaitableTimer` | none; a timer thread + cv | `timer_create`, `timerfd` | |
| async I/O completion | I/O completion port: `CreateIoCompletionPort`, `GetQueuedCompletionStatus` | none | `epoll` (readiness), `io_uring` (completion) | IOCP is the Windows event loop; module 11 / 08_Networking |
| process-wide exit | `ExitProcess` | `std::exit`, `std::quick_exit` | `exit`, `_exit` | |
| kill a thread | `TerminateThread` | none (good) | `pthread_cancel` | never in production; leaves locks held |

### 2.2 Threads

```c
#include <windows.h>
#include <process.h>

unsigned __stdcall worker(void* arg) {
    int id = *(int*)arg;
    /* ... */
    return 0;
}

HANDLE h = (HANDLE)_beginthreadex(NULL, 0, worker, &id, 0, NULL);
WaitForSingleObject(h, INFINITE);
CloseHandle(h);
```

`_beginthreadex` versus `CreateThread`: the CRT (`errno`, `strtok` state, locale, C++ exception
state) is per-thread and `_beginthreadex` initialises it; `CreateThread` from C code that uses the
CRT historically leaked that state. MSVC's `std::thread` uses `_beginthreadex`.

`GetExitCodeThread` retrieves the return value after the thread ends. In C++ terms that is the
future (module 09).

### 2.3 CRITICAL_SECTION

```c
CRITICAL_SECTION cs;
InitializeCriticalSectionAndSpinCount(&cs, 4000);   /* spin ~4000 iterations before blocking */
EnterCriticalSection(&cs);
/* ... */
LeaveCriticalSection(&cs);
DeleteCriticalSection(&cs);
```

Structure (`RTL_CRITICAL_SECTION`): `LockCount` (encoded: locked bit, waiters count, wake flag),
`RecursionCount`, `OwningThread`, `LockSemaphore` (the kernel wait object, allocated lazily on
first contention), `SpinCount`. Fast path: `InterlockedCompareExchange` on `LockCount`. Contended:
spin `SpinCount` times, then wait on the lazily created event / keyed event. Recursive: the owner
re-entering increments `RecursionCount`.

Portable description: "a recursive user-mode mutex with an adaptive spin before blocking". C++
equivalent for most uses: `std::mutex` after you have removed the recursive entries (module 03).
`!locks` and `!cs` in WinDbg resolve owners because `OwningThread` is recorded; this is the one
diagnostic advantage CS has over SRWLOCK.

### 2.4 SRWLOCK

```c
SRWLOCK lock = SRWLOCK_INIT;
AcquireSRWLockShared(&lock);    ReleaseSRWLockShared(&lock);
AcquireSRWLockExclusive(&lock); ReleaseSRWLockExclusive(&lock);
TryAcquireSRWLockExclusive(&lock);   /* Win7+ */
```

One pointer-sized word. No recursion (re-acquiring exclusive from the owner deadlocks; acquiring
shared while holding shared is permitted but undefined if a writer is waiting, so do not). No
upgrade. Faster than CS even for exclusive-only use because there is no recursion check and no
lazily allocated kernel object. Portable description: `std::shared_mutex`. If your server has
CRITICAL_SECTIONs around read-mostly data, converting to SRWLOCK is the single easiest
measurable win.

### 2.5 CONDITION_VARIABLE

```c
CONDITION_VARIABLE cv;   InitializeConditionVariable(&cv);
SRWLOCK lock = SRWLOCK_INIT;

/* consumer */
AcquireSRWLockExclusive(&lock);
while (queue_empty(&q) && !closed)                             /* predicate loop, same as C++ */
    SleepConditionVariableSRW(&cv, &lock, INFINITE, 0);        /* releases lock, waits, re-acquires */
/* ... */
ReleaseSRWLockExclusive(&lock);

/* producer */
AcquireSRWLockExclusive(&lock);
push(&q, item);
ReleaseSRWLockExclusive(&lock);
WakeConditionVariable(&cv);
```

`SleepConditionVariableCS` pairs with a CRITICAL_SECTION; `SleepConditionVariableSRW` with an
SRWLOCK (the last argument `CONDITION_VARIABLE_LOCKMODE_SHARED` lets a shared holder wait). The
documentation states that spurious wakeups occur and the condition must be re-checked. Everything
in module 05 applies unchanged. If you still use Events for producer-consumer, this is the
replacement.

### 2.6 Events and Semaphores

```c
HANDLE evManual = CreateEvent(NULL, TRUE,  FALSE, NULL);   /* manual-reset, initially not set */
HANDLE evAuto   = CreateEvent(NULL, FALSE, FALSE, NULL);   /* auto-reset */
SetEvent(ev); ResetEvent(ev); PulseEvent(ev) /* deprecated; unreliable */;
WaitForSingleObject(ev, timeout_ms);                       /* WAIT_OBJECT_0, WAIT_TIMEOUT, WAIT_FAILED */

HANDLE sem = CreateSemaphore(NULL, initial, maximum, NULL);
ReleaseSemaphore(sem, 1, NULL);                            /* count += 1 */
WaitForSingleObject(sem, INFINITE);                        /* count -= 1 when > 0 */
```

Events are kernel objects: every `SetEvent` and every wait is a syscall (~1 µs typical), which is
why a request queue signalled per item with an Event is slower than a CONDITION_VARIABLE that only
enters the kernel when a thread is actually asleep. They are also cross-process and waitable in
groups, which condition variables are not.

Semantics to describe portably:

- Manual-reset event = a latch. C++: `std::promise<void>` / `std::shared_future<void>` for one
  shot; `mutex + bool + condition_variable` with `notify_all` for resettable.
- Auto-reset event = a binary semaphore with coalescing. C++: `mutex + bool + condition_variable`
  with `notify_one`. Note the coalescing hazard (module 05): two `SetEvent`s before a wait is one
  wake.
- Semaphore = a counting semaphore. C++20: `std::counting_semaphore`. C++17: `mutex + int +
  condition_variable`.

### 2.7 WaitForMultipleObjects

```c
HANDLE h[3] = { hShutdownEvent, hWorkSemaphore, hTimer };
DWORD r = WaitForMultipleObjects(3, h, FALSE /* any */, INFINITE);
switch (r) {
case WAIT_OBJECT_0 + 0: /* shutdown */ break;
case WAIT_OBJECT_0 + 1: /* work */     break;
case WAIT_OBJECT_0 + 2: /* timer */    break;
case WAIT_FAILED: /* GetLastError() */ break;
}
```

Limit `MAXIMUM_WAIT_OBJECTS` = 64. With `bWaitAll = FALSE` and several objects signalled, the
*lowest index* is returned (so put the shutdown event first; this is why your shutdown is
responsive). `WAIT_ABANDONED_0 + i` means a mutex handle whose owner died.

No standard C++ equivalent. Portable rewrites:

- "Wait for shutdown or work" = one condition variable with predicate `closed || !queue.empty()`
  (module 05). This covers 90% of WFMO use in request servers.
- "Wait for any of N heterogeneous events" = an actor mailbox: each source pushes a tagged message
  into one queue (module 11).
- "Wait for all of N tasks" = N futures joined, or a counter plus condition variable (a latch;
  `std::latch` in C++20).
- "Wait for socket or event" = `epoll` with an `eventfd` for the non-socket signal (08_Networking).

### 2.8 Interlocked

```c
LONG v = 0;
InterlockedIncrement(&v);                                    /* returns new value */
LONG old = InterlockedCompareExchange(&v, 10 /*desired*/, 5 /*expected*/);   /* returns old */
if (old == 5) { /* swapped */ }
InterlockedExchangePointer(&p, newp);
MemoryBarrier();
```

All un-suffixed forms are full barriers (seq_cst) on every architecture. `Acquire` / `Release` /
`NoFence` suffixed variants exist for ARM64; on x86 they compile to the same `lock` instruction.
`volatile LONG` with `/volatile:ms` gives acquire loads and release stores, which is why many Win32
flags "work"; under `/volatile:iso` (the default on ARM64) they do not. Portable: `std::atomic<long>`.

Mapping the CAS convention: Win32 returns the *old value* and you compare it yourself; C++ returns
`bool` and writes the current value into your `expected` variable on failure. Both express the
same loop.

### 2.9 Thread pool API (Vista+)

```c
PTP_POOL pool = CreateThreadpool(NULL);
SetThreadpoolThreadMaximum(pool, 8);
SetThreadpoolThreadMinimum(pool, 2);
TP_CALLBACK_ENVIRON env; InitializeThreadpoolEnvironment(&env); SetThreadpoolCallbackPool(&env, pool);

VOID CALLBACK work_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work) { /* ... */ }
PTP_WORK work = CreateThreadpoolWork(work_cb, ctx, &env);
SubmitThreadpoolWork(work);                        /* may be submitted many times */
WaitForThreadpoolWorkCallbacks(work, FALSE);       /* wait for outstanding callbacks */
CloseThreadpoolWork(work);
CloseThreadpool(pool);
```

Also `CreateThreadpoolTimer`, `CreateThreadpoolWait` (wait on a handle and run a callback: this is
how you get "wait for N > 64 objects"), `CreateThreadpoolIo` (IOCP integration), and cleanup groups.
There is no result/future; you build completion signalling yourself. It is a solid, well-tested
pool and the right default for new Windows code; module 09's pool is what it looks like inside.
The legacy `QueueUserWorkItem` is the older API with less control.

### 2.10 I/O completion ports in one paragraph

An IOCP is a kernel queue of completed asynchronous I/O operations. You associate handles (sockets,
files) with the port, issue overlapped operations (`WSARecv` with an `OVERLAPPED`), and a small
number of threads (usually one per core) block in `GetQueuedCompletionStatus`, which returns one
completion at a time. The kernel also limits how many threads it wakes concurrently to the
port's concurrency value, so you get an event loop that is automatically multi-threaded. This is
the Windows equivalent of an `epoll` + pool design, and the natural shape for a Windows server that
must handle more connections than threads. `PostQueuedCompletionStatus` lets you inject your own
messages, which is how you integrate a request queue with the same loop. 08_Networking covers the
Linux side; the concepts are identical, the readiness-vs-completion distinction is the main
difference (epoll tells you a socket *can* be read; IOCP tells you a read *has completed*).

## 3. Code

File: `code/m13_win32_threads.c` (Windows-only) — `_beginthreadex`, a CRITICAL_SECTION-guarded
counter, an auto-reset Event based request queue showing the coalescing hazard, and a Semaphore
based version that does not have it.

File: `code/m13_win32_srwlock_condvar.c` (Windows-only) — the module 05 producer-consumer queue in
C with `SRWLOCK` and `CONDITION_VARIABLE`, with the same close-and-drain shutdown protocol, plus a
`WaitForMultipleObjects` shutdown-or-work example and `InitOnceExecuteOnce`.

Key fragment of the condition-variable queue in C:

```c
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
    int buf[64]; size_t head, tail, count;
    BOOL closed;
} Queue;

BOOL queue_pop(Queue* q, int* out) {
    AcquireSRWLockExclusive(&q->lock);
    while (q->count == 0 && !q->closed)                       /* predicate loop */
        SleepConditionVariableSRW(&q->not_empty, &q->lock, INFINITE, 0);
    if (q->count == 0) { ReleaseSRWLockExclusive(&q->lock); return FALSE; }   /* closed and drained */
    *out = q->buf[q->head]; q->head = (q->head + 1) % 64; q->count--;
    ReleaseSRWLockExclusive(&q->lock);
    WakeConditionVariable(&q->not_full);
    return TRUE;
}
```

Compare line by line with `BoundedQueue::pop` in `m05_producer_consumer.cpp`. They are the same
program.

## 4. Under the hood (senior-level)

### Kernel objects vs user-mode primitives

Win32 has two families. **Kernel objects** (Mutex, Event, Semaphore, WaitableTimer, Thread,
Process, File) are referenced by `HANDLE`, live in the kernel's object manager, are securable and
nameable, can be shared across processes, and are waited on through `NtWaitForSingleObject` /
`NtWaitForMultipleObjects`. Every operation is a syscall and a kernel dispatcher lock. **User-mode
primitives** (CRITICAL_SECTION, SRWLOCK, CONDITION_VARIABLE, INIT_ONCE, Interlocked) do their fast
path with an interlocked instruction in user space and enter the kernel only to block, via keyed
events (historically) or `NtWaitForAlertByThreadId` / `WaitOnAddress` (Windows 8+). This is the
same split as glibc's futex-based primitives vs System V semaphores on Linux. The rule "use
CRITICAL_SECTION unless you need cross-process or WFMO" is the rule "use the user-mode primitive
unless you need a kernel feature".

### Keyed events and `WaitOnAddress`

Before Windows 8, user-mode primitives blocked on a single global **keyed event** object: a wait
is keyed by an address, and a wake releases the waiter with that key. That is a futex without the
value check. Windows 8 added `WaitOnAddress(addr, compare, size, timeout)`: block only if `*addr`
still equals `*compare`, which is exactly `FUTEX_WAIT`. `WakeByAddressSingle/All` are
`FUTEX_WAKE`. Modern CRITICAL_SECTION and SRWLOCK use these. If you ever write your own primitive
on Windows, `WaitOnAddress` is the building block, just as `futex` is on Linux.

### Dispatcher objects and the wait block

When a thread waits on a kernel object, the kernel allocates a wait block linking the thread to the
object's wait list, marks the thread `Waiting`, and the scheduler picks another thread. `SetEvent`
walks the wait list, satisfies the waits, and makes the threads ready, with a priority boost
(`EVENT_INCREMENT`, +1) so that a thread woken for I/O or an event gets scheduled promptly. WFMO
with `bWaitAll = TRUE` allocates one wait block per object and the thread becomes ready only when
all are signalled. The 64-object limit is because the wait blocks are allocated in a fixed array in
the thread's kernel structure.

### Priority boosts and your server

Windows boosts a thread's priority when it wakes from a wait (so a worker just handed a request runs
soon) and decays it over a few quanta. This mostly helps a request server. It also means a thread
that *never* waits (a spin loop) never gets boosted and can be starved by threads that do. On
Linux CFS the analogous effect is that a sleeping task accumulates less virtual runtime and is
favoured when it wakes. Either way: block, do not spin, in a user-space server.

### Why SRWLOCK does not record its owner

Recording the owner costs a store on every acquire and makes the structure larger than a pointer.
SRWLOCK's design goal was a single word for both modes. The cost shows up in debugging: WinDbg's
`!locks` cannot tell you who holds an SRWLOCK. If your server is SRWLOCK-heavy, keep a debug-build
owner field next to each lock, written after acquire and cleared before release, so a dump can
resolve it.

## 5. In production

- **Describing the synchronisation layer.** "Per-connection state is behind a user-mode mutex
  with a spin phase (CRITICAL_SECTION). Master tables are behind a reader-writer lock (SRWLOCK).
  The request queue is a bounded buffer with a condition variable (or, if it still uses Events,
  say: signalled with an auto-reset event, which we bounded with a semaphore to avoid coalesced
  wakeups). Shutdown is a manual-reset event placed first in every WaitForMultipleObjects so
  workers exit promptly. Counters use Interlocked operations, which are sequentially consistent on
  x86." Every phrase is now precise and portable.
- **Converting Events to CONDITION_VARIABLE.** The migration is mechanical: the Event becomes a
  CONDITION_VARIABLE plus the predicate it stood for (queue non-empty, shutdown requested), the
  `WaitForSingleObject` becomes the predicate loop with `SleepConditionVariableSRW`, the
  `SetEvent` becomes "modify state under the lock, then Wake". The coalescing bug disappears. The
  syscall per item disappears.
- **CS to SRWLOCK.** Find the CRITICAL_SECTIONs around read-mostly data, confirm no recursive
  entries (debug: assert `RecursionCount == 1` after each Enter, or temporarily switch to a
  checker), convert. Measure p99 under concurrent load before and after.
- **TerminateThread.** If the codebase has it, it is a latent heap corruption. Replace with a
  cooperative stop flag checked by the thread. This is a good "what would you fix first" answer.
- **Dumps and SRWLOCK.** Because `!locks` cannot resolve SRWLOCK owners, a hang analysis on an
  SRWLOCK-heavy server needs your own owner tracking. Add it in debug builds now.
- **3 AM on Windows.** `procdump -ma PID`, open in WinDbg, `!analyze -v -hang`, `~*k`, `!locks`.
  Same procedure as module 12, same wait-for graph. `WAIT_ABANDONED` in a log means a thread died
  holding a kernel Mutex; find the crash that killed it.

## 6. Try it

1. **(easy)** Build `m13_win32_threads.c` with MSVC or MinGW and run it. Observe the auto-reset
   Event version losing a wake when two `SetEvent`s coalesce and the Semaphore version not. Done
   when you can explain the difference in terms of state.
2. **(easy)** Build and run `m13_win32_srwlock_condvar.c`. Put it side by side with
   `m05_producer_consumer.cpp` and annotate each line with its equivalent. Done when every line has
   a partner.
3. **(medium)** Write a table of every synchronisation primitive your real server uses (count
   them: how many CRITICAL_SECTIONs, Events, Semaphores, Mutex handles, SRWLOCKs) and, for each,
   the C++17 equivalent and whether a conversion would change behaviour (recursion, coalescing,
   cross-process). Done when the table is complete and you have identified the one conversion with
   the best payoff.
4. **(medium)** Implement `WaitOnAddress`-based spinlock-then-block mutex in C on Windows (fast
   path interlocked, slow path `WaitOnAddress`, unlock `WakeByAddressSingle`). Compare its
   uncontended cost to CRITICAL_SECTION. Done when both numbers are recorded and the design maps to
   glibc's futex mutex in your explanation.
5. **(hard)** Rewrite the module 09 thread pool using the Vista thread pool API
   (`CreateThreadpoolWork`), adding your own completion signalling with a per-task manual-reset
   Event or a counter plus CONDITION_VARIABLE so that a submitter can wait for a result. Then
   write a paragraph comparing it with `m09_thread_pool.hpp`: what the OS pool does for you and
   what it does not (futures, exceptions, bounded queue). Done when both run the same workload
   and the paragraph is written.

## 7. Interview questions

**Q: Describe the synchronisation in your Windows server in portable terms.**
A user-mode recursive mutex with an adaptive spin phase (CRITICAL_SECTION) for per-connection state;
a reader-writer lock (SRWLOCK) for read-mostly master tables; a bounded request queue with a
condition variable (or event plus semaphore) for the pooled workers; a manual-reset shutdown latch
checked first in every multi-object wait; sequentially consistent atomic counters (Interlocked). The
C++17 names are `std::mutex`, `std::shared_mutex`, `std::condition_variable`, a promise/future or
flag-under-mutex, and `std::atomic`.

**Q: What is the difference between a CRITICAL_SECTION and a Win32 Mutex handle?**
A CRITICAL_SECTION is a user-mode lock: interlocked fast path, optional spin, kernel wait only on
contention, intra-process only, recursive, records its owner. A Mutex handle is a kernel object:
every acquire is a syscall, works across processes, can be waited on with other handles via
`WaitForMultipleObjects`, has a timeout on acquire, and reports abandonment if the owner dies.

**Q: How does a Win32 Event differ from a condition variable, and which is the C++ equivalent?**
An Event is a kernel object with state: a set with no waiter persists, and an auto-reset Event
coalesces multiple sets into one wake. A condition variable is stateless and must be paired with a
predicate under a mutex. C++ has only the condition variable; an Event is modelled as a flag or
count under a mutex with a condition variable, or `std::promise<void>` for a one-shot latch.
Windows also has `CONDITION_VARIABLE`, which is the same as the C++ one.

**Q: What does `WaitForMultipleObjects` do and what is the portable replacement?**
Blocks until any (or all) of up to 64 kernel objects are signalled, returning the lowest signalled
index. No standard C++ equivalent. Portable designs: one condition variable with a composite
predicate (shutdown or work), a single tagged message queue for heterogeneous events, a latch or
joined futures for wait-all, and epoll with an eventfd for sockets plus signals.

**Q: Are Interlocked operations the same as `std::atomic`?**
Un-suffixed Interlocked functions are atomic read-modify-writes with full barrier (seq_cst)
semantics. `std::atomic` defaults to seq_cst and offers weaker orders; Win32 added `Acquire`,
`Release` and `NoFence` suffixes for ARM64 to match. The CAS convention differs: Win32 returns the
old value, C++ returns a bool and updates `expected`.

**Q: What is `WaitOnAddress`?**
Windows 8's futex: block the calling thread only if the value at an address still equals a
comparand, and wake with `WakeByAddressSingle/All`. It is the building block under modern
CRITICAL_SECTION, SRWLOCK and CONDITION_VARIABLE, exactly as `futex(2)` is under glibc's mutex and
condition variable.

**Q: What is an I/O completion port?**
A kernel queue of completed overlapped I/O operations on associated handles, drained by a bounded
set of threads blocked in `GetQueuedCompletionStatus`, with the kernel limiting concurrent active
threads to a configured value. It is the Windows event loop and the counterpart of epoll plus a
pool on Linux, with completion semantics rather than readiness semantics.

## 8. Recap

- Two families: kernel objects (handles, syscalls, cross-process, WFMO) and user-mode primitives
  (interlocked fast path, block only on contention). Same split as System V vs futex on Linux.
- CRITICAL_SECTION = recursive spinning `std::mutex`; SRWLOCK = `std::shared_mutex`;
  CONDITION_VARIABLE = `std::condition_variable`; INIT_ONCE = `std::call_once`.
- Events have state and coalesce; condition variables do not. Model an Event as flag + mutex + cv.
- Semaphore = counting semaphore; C++20 has it, C++17 builds it from mutex + int + cv.
- WaitForMultipleObjects has no standard equivalent; use composite predicates, one queue, latches,
  or epoll.
- Interlocked = seq_cst atomics; CAS return convention differs from C++.
- `WaitOnAddress` is the Windows futex.
- Vista thread pool is a fine default on Windows; it lacks futures and bounded queues.
- IOCP = completion-based event loop, the Windows epoll-plus-pool.
- SRWLOCK does not record owners; add debug owner tracking for dump analysis.
- Never `TerminateThread`.
