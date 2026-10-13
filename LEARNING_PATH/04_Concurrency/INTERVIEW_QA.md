# 04_Concurrency — interview questions and model answers

38 questions, ordered from basic to senior. Each answer is what you would say out loud in 30 to 60
seconds. The module that covers the topic in depth is given in brackets. Where your Win32 background
gives you a better answer, the answer uses it: interviewers like hearing that you have shipped this
in production, in any dialect.

Practise by covering the answer, saying yours aloud, then comparing. If yours is longer than the
model answer, cut it.

---

## Part A — Foundations

**1. What is the difference between a process and a thread? [01]**
A process owns an address space, handles and at least one thread; a thread is a schedulable
execution context (registers, stack, thread-local storage) inside a process. Threads in one process
share all memory, so communication is cheap and bugs are shared too: one thread's wild write or
crash takes down the whole process. On Linux both are `task_struct`s created by `clone` with
different sharing flags; on Windows they are distinct kernel objects (`CreateProcess` vs
`CreateThread`).

**2. What does `std::thread`'s destructor do if the thread is still joinable, and why? [01]**
It calls `std::terminate`. The alternatives were worse: an implicit `join` could hang silently in a
destructor during exception unwinding, and an implicit `detach` would leave a thread running with
references to stack variables that no longer exist. So you must `join()` or `detach()` explicitly,
or use an RAII wrapper (C++20 `std::jthread` joins in its destructor).

**3. What is a data race, and why is it undefined behaviour rather than just "a wrong value"? [02]**
A data race is two accesses to the same memory location from different threads, at least one a
write, at least one non-atomic, with no happens-before relation between them. The standard makes
it UB so the compiler may assume single-threaded semantics for ordinary variables: it can keep
values in registers, merge or delete loads and stores, and reorder them. So a racy `while (!flag)`
loop can become an infinite loop, and a racy struct copy can tear. "It is only a counter" is not a
defence; the compiler is allowed to break any code that races.

**4. Define happens-before in one or two sentences. [02, 07]**
A happens-before B means A's effects are guaranteed visible to B. It is built from sequenced-before
within one thread plus synchronises-with edges between threads (unlock then lock of the same mutex,
release store read by an acquire load, thread creation, join), closed transitively. If two
conflicting accesses are not ordered by happens-before, you have a data race.

**5. Why is `volatile` not a substitute for `std::atomic`? [02, 07]**
`volatile` only tells the compiler each access must actually happen, for memory-mapped I/O and
signal handlers. It does not make a read-modify-write atomic (`v++` is still load, add, store), it
creates no happens-before between threads, and it emits no CPU barriers. A `volatile` flag used to
publish data is still a data race in C++. MSVC's `/volatile:ms` gives volatile acquire/release
semantics as a vendor extension, which is why some Windows code gets away with it; portable code
cannot.

## Part B — Mutexes and locking

**6. Why use `std::lock_guard` or `std::unique_lock` instead of calling `lock()` and `unlock()`? [03]**
RAII guarantees the unlock on every exit path, including early returns and exceptions. A manual
`unlock()` that is skipped by a `throw` leaves the mutex locked forever and the next locker hangs,
which is the classic "worked for months, then deadlocked after a rare error" bug. It is the C++
answer to the `goto cleanup` pattern you write in C.

**7. When do you need `unique_lock` rather than `lock_guard`? [03, 05]**
When the lock must be released and reacquired, deferred, tried, timed or moved: condition variable
waits (which unlock and relock internally), `try_to_lock`, `defer_lock` with `std::lock`, and
unlocking early before a slow call. `lock_guard` is a zero-overhead scope lock and should be the
default; `scoped_lock` is the default when you need several mutexes at once.

**8. What does an uncontended `std::mutex` lock cost, and what happens when it is contended? [03]**
On Linux, glibc's mutex is futex-based: an uncontended lock is one atomic compare-and-swap in user
space and the unlock is one atomic store or exchange, typically tens of nanoseconds or less. Under
contention the loser may spin briefly, then calls `futex(FUTEX_WAIT)` and sleeps in the kernel;
the unlocker sees the "waiters" state and calls `futex(FUTEX_WAKE)`. So the cost of contention is
syscalls and context switches (microseconds), not the lock instruction itself. A CRITICAL_SECTION
works the same way with a spin count and a kernel event (or keyed event) as the slow path.

**9. Mutex vs spinlock vs futex: what is each, and when would you choose a spinlock? [03, 08]**
A spinlock busy-waits on an atomic flag; it never sleeps. A futex is the Linux kernel primitive
"sleep if this word still equals X / wake N sleepers on this word", which lets a mutex stay in user
space when uncontended and sleep when contended. A `std::mutex` is built on the futex (or on
`WaitOnAddress`/keyed events on Windows). Choose a spinlock only when critical sections are a few
dozen instructions, threads are not oversubscribed (ideally pinned), and you cannot afford a
syscall; for example in a kernel or a real-time core. In general server code a spinlock is a
mistake: if the holder is descheduled, every waiter burns its whole time slice.

**10. Why is `std::recursive_mutex` usually a design smell? [03]**
Recursion means a function that already holds the lock calls another that takes it again, so the
inner function may run while the outer one has the invariant half-updated. The recursive mutex hides
that by not deadlocking. The clean design is a public locking function plus a private `_locked`
helper that assumes the caller holds the lock. CRITICAL_SECTION is recursive by default, so this is
a real migration hazard when porting Win32 code to `std::mutex`: code that "worked" will now
self-deadlock.

**11. When does `std::shared_mutex` beat `std::mutex`, and when does it lose? [04]**
It wins when reads are frequent, long enough to overlap usefully, and writes are rare. It loses
when critical sections are tiny: every `lock_shared` still does an atomic RMW on one shared counter,
so readers bounce a cache line between cores and you pay more than a plain mutex while gaining no
parallelism. It can also starve writers depending on the implementation. Measure; and for
read-mostly data refreshed periodically, an immutable snapshot swap usually beats both.

## Part C — Condition variables and waiting

**12. Why must you wait on a condition variable in a loop with a predicate? [05]**
Three reasons. Spurious wakeups: the implementation may return from `wait` with no notify.
Stolen wakeups: between the notify and your reacquiring the mutex, another thread may consume the
item. Multi-purpose notifies: one condition variable may signal several state changes (item
available, or shutdown). So the wake only means "re-check", and `cv.wait(lk, pred)` is exactly
`while (!pred()) cv.wait(lk);`.

**13. What is a lost wakeup and how does the mutex prevent it? [05, 12]**
A lost wakeup is a notify that arrives when nobody is waiting yet, after the waiter has checked
the predicate (false) but before it has blocked. The waiter then sleeps forever. It is prevented by
modifying the shared state under the same mutex the waiter holds while it checks and blocks: then
the setter cannot run between "check" and "block", because `wait` releases the mutex atomically
with going to sleep. Setting the flag outside the mutex, even if the flag is atomic, reopens the
window.

**14. Should you call `notify_one` while holding the lock or after releasing it? [05]**
Both are correct as long as the state change was made under the lock. Notifying after unlock avoids
"hurry up and wait", where the woken thread immediately blocks on the mutex the notifier still
holds; glibc mitigates this but unlocking first is still a good default. Notifying under the lock
is required in one case: when the waiter might destroy the condition variable as soon as it sees
the state change.

**15. Map Win32 Events and Semaphores to the standard library. What has no direct equivalent? [05, 13]**
A Semaphore is a counting semaphore: C++20 `std::counting_semaphore`, or mutex + int + condition
variable in C++17. A manual-reset Event is a boolean plus `notify_all`. An auto-reset Event has no
direct equivalent: it is a binary semaphore that saturates at one, so two `SetEvent` calls with no
waiter in between merge into one wake. That coalescing is a common source of "stuck request" bugs;
in C++ you express the real condition as a predicate on the queue state, which cannot coalesce.

## Part D — Deadlock and liveness

**16. What are the four conditions for deadlock, and which one do you usually break? [06]**
Mutual exclusion, hold-and-wait, no preemption, and circular wait (the Coffman conditions). All four
must hold. In practice you break circular wait with a global lock order: every thread acquires
locks in the same ranked order, so no cycle can form. You break hold-and-wait with `std::scoped_lock`
over all needed mutexes at once, or with try-lock and back-off.

**17. How does `std::scoped_lock(a, b)` avoid deadlock? [06]**
It uses the `std::lock` algorithm: lock one mutex, try-lock the others, and if any try fails,
release everything and retry starting with the one that failed. It never holds one lock while
blocking on another, so it breaks hold-and-wait for that acquisition, regardless of argument order.
It only helps when you know all the locks up front; it cannot fix a lock taken inside a callback
later.

**18. What is livelock, and how is it different from deadlock? [06]**
In livelock threads are running and changing state but making no progress, typically two threads
that each back off and retry in lock-step forever. CPU is high, throughput is zero. In deadlock the
threads are blocked and CPU is zero. Livelock is fixed by randomised or exponential back-off, which
desynchronises the retries, or by deterministic ordering so retries are unnecessary.

**19. You get a hung server. Walk me through reading the deadlock from the thread stacks. [06, 12]**
First capture state without restarting: `gcore PID` on Linux, `procdump -ma` on Windows, and check
per-thread CPU (all zero means blocked, not spinning). Dump all stacks (`thread apply all bt`, or
`~*k` in WinDbg). Classify each thread by its top frames: in `__lll_lock_wait`/`futex_wait` or
`RtlpWaitOnCriticalSection` means waiting for a lock. For each waiter, find which lock it wants
(the frame's argument) and who owns it (`__owner` in a glibc mutex, `!cs` or `!locks` in WinDbg
gives `OwningThread`). Draw the wait-for graph; a cycle is a deadlock: "T7 holds cache, waits conn;
T12 holds conn, waits cache". No cycle with the owner in `recv` or a DB call means a lock held across
I/O.

**20. What is priority inversion and how is it handled? [06]**
A high-priority thread waits for a lock held by a low-priority thread, which in turn is preempted by
a medium-priority thread, so the high-priority thread effectively runs at low priority. Fixes are
priority inheritance (the holder temporarily inherits the waiter's priority, as in
`PTHREAD_PRIO_INHERIT` mutexes) or priority ceiling. Windows mitigates it by randomly boosting
starved threads. It matters most in real-time systems.

## Part E — Atomics and the memory model

**21. Explain acquire and release correctly. [07]**
A release store makes every write the thread did before it visible to any thread whose acquire load
reads that stored value (or a later value in the release sequence); that read creates a
synchronises-with edge. Acquire also prevents later reads and writes in the acquiring thread from
moving before the load; release prevents earlier ones from moving after the store. They are one-way
barriers that pair up: release alone, or acquire alone, guarantees nothing to another thread. The
classic use is publishing data: fill a buffer, release-store `ready = true`; the consumer
acquire-loads `ready` and then reads the buffer safely.

**22. What does `seq_cst` add over acquire/release, and what does it cost? [07]**
`seq_cst` adds a single total order over all `seq_cst` operations that every thread agrees on. That
forbids the store-then-load reordering that acquire/release allows, which algorithms like Peterson's
lock and Dekker-style flags need. On x86, loads are plain `mov` in every order, but a `seq_cst` store
compiles to `xchg` (or `mov` plus `mfence`), a full barrier that drains the store buffer and
typically costs tens of cycles. On ARM it costs barriers or `ldar`/`stlr` pairs on both sides. It
is the default because it is easiest to reason about; relax only on hot paths with a written proof.

**23. When is `memory_order_relaxed` correct? [07]**
When only the atomicity of that one variable matters and nothing else is published through it: a
statistics counter, a sequence-number generator, a "stop" flag where the thread join provides the
ordering for any data. It is wrong whenever another thread will use the value to decide that some
other memory is ready to read. Relaxed operations on one variable still have a single modification
order, so a counter never loses increments.

**24. Why does `shared_ptr`'s reference count decrement need `acq_rel`, while the increment can be relaxed? [07]**
The increment only needs atomicity: you already hold a reference, so the object cannot die. The
decrement that reaches zero runs the destructor, and the destructor must see every write that every
other owner made to the object before dropping its reference. Release on each decrement publishes
those writes; acquire on the decrement that hits zero receives them. Relaxed decrements could let
the deleter free memory while another thread's earlier writes to it are still unordered with the
delete.

**25. What does `compare_exchange_weak` do, and why prefer it in a loop over `_strong`? [07, 08]**
It atomically compares the variable with `expected` and, if equal, writes `desired`; if not, it
writes the current value into `expected` and returns false. The weak form may also fail spuriously
even when the values are equal, because on LL/SC architectures (ARM, POWER) a cache-line event can
break the reservation. Inside a retry loop that is harmless and the weak form compiles to tighter
code; use strong when a spurious failure would make you do expensive work or take a wrong branch.

## Part F — Lock-free

**26. What is the ABA problem? [08]**
A CAS-based algorithm reads A, gets delayed, and meanwhile another thread changes the value to B
and back to A; the CAS succeeds because the value matches, but the world has changed. In a Treiber
stack: thread 1 reads head = A and A->next = B, thread 2 pops A, pops B, pushes A back; thread 1's
CAS(head, A, B) succeeds and installs B, which is no longer in the stack. Fixes are a version tag
next to the pointer (double-width CAS), or safe memory reclamation so a node cannot be reused while
anyone might still hold it.

**27. Why is memory reclamation the hard part of lock-free data structures? [08]**
A thread that has read a node's address may dereference it at any later point, and there is no
lock telling the popper that it is safe to `delete`. Freeing too early is a use-after-free;
never freeing is a leak. Solutions: hazard pointers (each thread publishes the pointers it is using;
reclaimers skip those), epoch-based reclamation (free only after all threads have passed a
quiescent point), or reference counting. Garbage-collected languages get this for free, which is
why lock-free is so much easier in Java.

**28. Lock-free vs wait-free vs obstruction-free? [08]**
Lock-free: at any time, some thread makes progress, so the system never stalls even if one thread
is suspended, but an individual thread may retry forever. Wait-free: every thread completes in a
bounded number of its own steps. Obstruction-free: a thread makes progress if it runs alone. A
spinlock is none of these: if the holder is suspended, nobody progresses. Lock-free is not
automatically faster; under contention a CAS loop can be slower than a good mutex.

**29. Describe a single-producer single-consumer ring buffer and its memory orders. [08]**
A fixed array with a `head` written only by the consumer and a `tail` written only by the producer,
each on its own cache line. The producer writes the slot, then release-stores `tail`; the consumer
acquire-loads `tail`, reads the slot, then release-stores `head`; the producer acquire-loads `head`
to check for space. Each side reads its own index relaxed. No CAS is needed because each index has
exactly one writer. Caching the other side's index locally cuts cross-core traffic further.

## Part G — Thread pools and server design

**30. Design a thread pool. [09]**
A fixed vector of workers, a queue of type-erased tasks guarded by a mutex, a condition variable
that workers wait on with the predicate "closed or queue not empty", and a `closed` flag. `submit`
wraps the callable in a `std::packaged_task`, pushes it, notifies one worker and returns the
future; exceptions travel through the future instead of killing the worker. Workers pop under the
lock and run the task outside the lock. A bounded queue with a second condition variable gives
backpressure so a burst cannot exhaust memory.

**31. How do you shut a thread pool down without losing tasks or hanging? [09]**
Set `closed = true` under the queue mutex, then `notify_all`. Workers exit only when the queue is
closed AND empty, so everything already submitted is still run (graceful drain); `submit` after
close throws or returns an error. Then join every worker. The common bug is checking only `closed`
in the worker, which drops queued tasks; the other is setting the flag without the lock, which can
lose the wake-up and hang the join. If you need a fast stop, provide a separate `shutdown_now()`
that swaps out the queue and lets the abandoned futures report `broken_promise`.

**32. Why should a task never block on a future from its own pool? [09]**
If every worker runs a task that waits for a sub-task, and the sub-tasks are queued behind them,
no worker is free to run the sub-tasks: the pool deadlocks with all threads in `future::get`. Fixes:
run the sub-task inline when the caller is already a worker, use a separate pool for the dependent
work, or use continuations instead of blocking waits.

**33. What is wrong with building a server on `std::async`? [09]**
With `launch::async`, libstdc++ creates a new thread per call, so there is no bound and no queue.
The returned future's destructor blocks until the task finishes, so a discarded result makes the
call synchronous. With the default policy the task may be deferred and never run until `get()`. There
is no cancellation or priority. It is fine for a one-off background computation in a tool, not for
request handling.

**34. How do you serve a read-mostly table (rates, configuration) to many threads with near-zero reader cost? [11]**
Make the table immutable and publish it through an atomic `shared_ptr` (RCU-style snapshot). A
reader atomically loads the pointer, uses its own snapshot for as long as it likes, and drops it.
The refresher builds a complete new table off to the side and atomically swaps the pointer with
release; old snapshots die when their last reader finishes. Readers never wait for a rebuild and
never see a half-built table. In libstdc++ the atomic `shared_ptr` operations use a small internal
lock pool, so it is "short lock, never held across anything slow", not truly lock-free; true RCU or
epochs get the read side to zero shared writes.

**35. What is the safe way to do lazy one-time initialisation? [11]**
First choice: a function-local static; since C++11 the compiler emits a thread-safe guard with an
acquire-load fast path. Second: `std::call_once` with a `std::once_flag` when initialisation needs
arguments or the storage is a member. Hand-written double-checked locking is correct only with an
atomic pointer, acquire on the fast-path load and release on the publishing store; with a plain
pointer it is a data race. If the initialiser throws, both standard forms leave the flag unset and
the next caller retries. Win32's equivalent is `InitOnceExecuteOnce`.

## Part H — Performance and debugging

**36. What is false sharing and how do you find and fix it? [10]**
Two threads write different variables that sit in the same 64-byte cache line. The coherence
protocol (MESI) must give the writing core exclusive ownership of the whole line, so the line
ping-pongs between cores and each write costs a cross-core transfer instead of an L1 hit. Symptom:
per-thread counters that scale negatively with thread count. Find it with `perf c2c` on Linux or
VTune; fix with `alignas(64)` per-thread slots, padding, or accumulating locally and publishing
rarely. True sharing (everyone updating one counter) needs a different fix: striping or batching.

**37. How do you find a data race you cannot reproduce? [12]**
Build with ThreadSanitizer (`-fsanitize=thread -g -O1`) and run the tests and a stress test: TSan
tracks happens-before with vector clocks and reports unordered conflicting accesses even if the bad
interleaving did not occur on this run, with both stacks. Run it in CI and treat reports as
failures. For deterministic reproduction, add race points (sleeps at named windows enabled by an
environment variable) to widen the suspected window. A debugger is weak for races because stopping
threads changes the timing.

**38. Your Windows server uses CRITICAL_SECTION, SRWLOCK, Events and Semaphores. How would you explain it, and what changes moving to C++17? [13]**
In standard terms: a user-mode mutex with a spin phase, a reader-writer lock, condition signalling
and a counting semaphore. Mapping: CRITICAL_SECTION to `std::mutex` (but it is recursive, `std::mutex`
is not), SRWLOCK to `std::shared_mutex`, CONDITION_VARIABLE to `std::condition_variable`, Semaphore
to C++20 `std::counting_semaphore` or mutex+int+cv, `InitOnceExecuteOnce` to `std::call_once`,
`Interlocked*` to `std::atomic` (Interlocked ops are full barriers, like `seq_cst`). Three things do
not map directly: auto-reset Event coalescing, `WaitForMultipleObjects` (replace with one composite
predicate or one queue), and named cross-process kernel objects. Debugging also changes:
`!locks`/`!cs` in WinDbg become `__owner` in gdb, and an SRWLOCK records no owner at all, so a
SRWLOCK-heavy server needs its own owner tracking in debug builds.
