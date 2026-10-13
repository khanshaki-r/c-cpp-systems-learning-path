# 14. Multithreading in C: pthreads and C11

**Time:** ~3 hours   **Prerequisites:** modules 07 (function pointers), 09, 10; 04_Concurrency modules 01 to 03 (sections 1 and 2) for the concepts

## 1. In plain words

Threads in one process share everything: globals, the heap, open files. That is what makes them
fast to communicate and dangerous to combine. The one rule that matters: **if one thread writes a
variable and another thread reads or writes it, every access must be protected**, either by a
lock that both threads take, or by making the variable atomic. Break the rule and you have a data
race, which in C is undefined behaviour (module 09), not just "a wrong number sometimes".

Picture a bank branch with one shared ledger.

- A **mutex** is the single pen chained to the ledger. Whoever holds the pen may write; everyone
  else waits.
- A **condition variable** is the waiting room with a bell. A clerk who needs "a customer in the
  queue" sits down and releases the pen; whoever adds a customer rings the bell. The clerk
  wakes up, takes the pen back, and **checks the queue again** before acting, because someone
  else may have served that customer first.
- A **read-write lock** is the reading room: many people may read the rate sheet at once, but
  updating it needs the room empty.
- An **atomic** is a counter that clicks forward in one indivisible step, so two clerks
  pressing it together still count two.

You maintain a multithreaded Win32 server, so you have seen `CRITICAL_SECTION` and friends. This
module is the same ideas in the POSIX and C11 APIs that Linux systems interviews expect, with a
Win32 mapping table, and it ends with ThreadSanitizer, the tool that finds races for you.

## 2. The real thing

### 2.1 Creating and joining threads

```c
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
```

- **Arguments in**: pass a pointer to a struct that lives at least as long as the thread uses it.
  The classic bug is `for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, f, &i);`: all four
  threads read the same `i`, which keeps changing and dies when the loop ends. Give each thread
  its own struct (an array of job structs indexed by thread).
- **Results out**: write them into the caller-owned job struct, or return a heap pointer from the
  thread function, which `pthread_join` hands back and the joiner frees.
- **Join** waits for the thread and also makes everything it wrote visible to the joiner.
- **Detach** says "nobody will join this thread"; its resources are freed when it exits. A
  detached thread must signal completion some other way (a semaphore, a condition variable) and
  must not touch anything that the rest of the program may free first, which makes them a
  shutdown hazard.
- **Every** joinable thread must be joined or detached, or its stack and descriptor leak.
- pthread functions **return** an error number (0 on success) and do not set `errno`. Use
  `strerror(rc)`, not `perror`.

### 2.2 Mutexes

```c
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_lock(&lock);
balance += amount;            /* the critical section: as short as possible */
pthread_mutex_unlock(&lock);
```

- Default mutexes are not recursive: locking twice in one thread deadlocks. Win32's
  `CRITICAL_SECTION` **is** recursive, which hides design problems that appear when code is ported.
- `PTHREAD_MUTEX_ERRORCHECK` (set with `pthread_mutexattr_settype`) returns `EDEADLK` on a relock
  and `EPERM` on unlock by a non-owner. Use it in debug builds of code you are not sure about.
- **Lock ordering** prevents deadlock: if any code path holds A while taking B, then no path may
  hold B while taking A. Document the order next to the lock declarations.
- **Never hold a lock across blocking I/O** (a socket read, a database call, a log write to a
  slow disk). Copy what you need, unlock, then do the I/O.

### 2.3 Condition variables and the predicate loop

```c
pthread_mutex_lock(&q->mu);
while (q->count == 0 && !q->closed)               /* WHILE, never if */
    pthread_cond_wait(&q->not_empty, &q->mu);     /* atomically: unlock, sleep, relock */
/* here: the predicate is true and we hold the lock */
```

- `pthread_cond_wait` must be called with the mutex held. It releases the mutex and sleeps as one
  atomic step, so a signal sent between "check" and "sleep" cannot be lost, and it reacquires the
  mutex before returning.
- **Why `while`, not `if`.** POSIX allows *spurious wakeups* (returning with no signal), and even a
  real wakeup can be *stolen*: another thread takes the item between the signal and your wakeup.
  Re-checking the predicate handles both.
- **The predicate lives in shared state protected by the mutex.** A condition variable has no
  memory; signalling when nobody waits does nothing. A "flag only" design that waits without
  checking state loses wakeups.
- `pthread_cond_signal` wakes at least one waiter; `pthread_cond_broadcast` wakes all. Use
  broadcast when the state change may let several threads proceed, and always for shutdown.
- **Timed waits** take an absolute deadline. By default it is measured on `CLOCK_REALTIME`, which
  jumps when NTP or an administrator changes the clock; set `CLOCK_MONOTONIC` with
  `pthread_condattr_setclock` and compute the deadline from `clock_gettime(CLOCK_MONOTONIC)`.
- **Shutdown**: set a `closed` flag under the lock and broadcast; every wait loop checks the flag.

### 2.4 Read-write locks

`pthread_rwlock_rdlock` lets many readers in together; `pthread_rwlock_wrlock` waits until there
are none. Worth it only for read-mostly data with critical sections long enough to amortise the
more expensive lock; for tiny critical sections a plain mutex is often faster. Watch for writer
starvation: glibc's default rwlock prefers readers, so a steady stream of readers can keep a writer
waiting indefinitely (glibc offers `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` as an option).

### 2.5 One-time initialisation

`pthread_once(&once, init_fn)` runs `init_fn` exactly once even if many threads call it
together, and every caller returns only after it has finished, with its writes visible. It is the
correct form of "lazy global init", which written by hand with a flag is a data race (and the
classic broken double-checked locking).

### 2.6 C11 `<threads.h>` and `<stdatomic.h>`

| Purpose | pthreads | C11 | Win32 |
|---------|----------|-----|-------|
| create / wait | `pthread_create` / `pthread_join` | `thrd_create` / `thrd_join` | `_beginthreadex` / `WaitForSingleObject` + `CloseHandle` |
| thread function | `void *f(void *)` | `int f(void *)` | `unsigned __stdcall f(void *)` |
| mutex | `pthread_mutex_t` | `mtx_t` (`mtx_plain`, `mtx_recursive`, `mtx_timed`) | `CRITICAL_SECTION`, `SRWLOCK` |
| condition variable | `pthread_cond_t` | `cnd_t` | `CONDITION_VARIABLE` + `SleepConditionVariableCS/SRW` |
| read-write lock | `pthread_rwlock_t` | none | `SRWLOCK` (shared / exclusive) |
| once | `pthread_once` | `call_once` | `InitOnceExecuteOnce` |
| thread-local | `pthread_key_create` or `_Thread_local` | `tss_create`, `thread_local` | `TlsAlloc`, `__declspec(thread)` |
| atomic add | `_Atomic` / GCC builtins | `atomic_fetch_add` | `InterlockedIncrement`, `InterlockedAdd` |
| semaphore | `sem_t` (POSIX, separate header) | none | `CreateSemaphoreW` |

The full Win32 reference, with code, is 04_Concurrency/13_win32_concurrency_reference.md.

C11 threads are portable but thinner: no read-write lock, no thread attributes (stack size),
and timed waits only against `TIME_UTC` (wall clock). glibc has shipped them since 2.28 and MSVC
since Visual Studio 2022 17.8.

`<stdatomic.h>` is the part of C11 everyone uses:

```c
static atomic_long hits;
atomic_fetch_add_explicit(&hits, 1, memory_order_relaxed);        /* a pure counter */

g_msg.amount = 250000;                                             /* plain data ...       */
atomic_store_explicit(&ready, 1, memory_order_release);            /* ... then publish     */
while (!atomic_load_explicit(&ready, memory_order_acquire)) ;      /* reader: see the flag */
use(g_msg.amount);                                                  /* ... then the data    */
```

- `memory_order_relaxed`: the operation is atomic, nothing else is ordered. Right for counters.
- `release` on a store and `acquire` on the load that reads it: everything written before the
  release is visible after the acquire. This is how one thread hands data to another without a lock.
- `seq_cst` (the default of the non-`_explicit` functions): one global order of all such
  operations. Safest, sometimes slower.
- `atomic_flag` is the one type guaranteed lock-free: enough to build a spinlock.

The theory, with diagrams, is 04_Concurrency/07_atomics_and_the_memory_model.md. The practical
rule: use a mutex unless you can say exactly which memory order you need and why.

### 2.7 Thread-safe, reentrant, async-signal-safe

| Property | Meaning | Example |
|----------|---------|---------|
| Thread-safe | correct when called from several threads at once | `malloc`, `printf` (it locks the `FILE`) |
| Reentrant | correct when called again before the previous call finished in the **same** thread (nested, recursive, from a handler) | `strtok_r`, `localtime_r`, anything with only arguments and locals |
| Async-signal-safe | may be called from a signal handler | `write`, `_exit`, `sem_post`; **not** `printf`, `malloc` |

Functions with hidden static state are neither thread-safe nor reentrant, and POSIX gives most of
them an `_r` twin: `strtok` -> `strtok_r`, `localtime` / `gmtime` / `ctime` -> `localtime_r` /
`gmtime_r` / `ctime_r`, `rand` -> `rand_r`, `strerror` -> `strerror_r`, `getpwnam` ->
`getpwnam_r`; `gethostbyname` and `inet_ntoa` are replaced by `getaddrinfo` and `inet_ntop`.
Moving a static result buffer to `_Thread_local` makes a function thread-safe but **still not
reentrant**: two calls in one `printf` still overwrite each other (`m14_reentrant.c` proves both).
`errno` itself is thread-local, which is why it works at all in threaded code.

### 2.8 A thread pool in C

A fixed set of worker threads takes tasks (a function pointer plus an argument) from a queue:

```text
submit(fn, arg) -> [ lock | append task | pending++ | signal has_work | unlock ]
worker loop     -> [ lock | while (no task && !stopping) wait(has_work) | pop | unlock ]
                   run fn(arg) WITHOUT the lock
                   [ lock | pending-- | if 0: broadcast idle | unlock ]
wait_idle()     -> [ lock | while (pending > 0) wait(idle) | unlock ]
destroy()       -> [ lock | stopping = 1 | broadcast has_work | unlock ] then join every worker
```

The design decisions: the task runs outside the lock; `pending` counts queued plus running tasks
so `wait_idle` is exact; shutdown drains the queue before workers exit; and `pool_create` joins
the threads it already started if a later `pthread_create` fails. Sizing and queue policies are
in 04_Concurrency/09_thread_pools_and_task_queues.md.

### 2.9 ThreadSanitizer

Build with `-fsanitize=thread -g -O1` and run. TSan records which thread accessed each memory
location under which locks, and reports two accesses that are not ordered by any
synchronisation (a "happens-before" relation). This tiny racy program (`counter++` from two
threads, no lock) is the canonical example:

```c
static long counter;
static void *work(void *arg) { (void)arg; for (int i = 0; i < 100000; i++) counter++; return NULL; }
```

Built with plain `-O2` it printed `counter = 200000` on both runs: GCC turned the loop into a
single `counter += 100000`, so the lost updates you expect did not appear. The race is still
there and the next compiler may expose it. TSan reports it regardless (real output, paths
shortened):

```text
WARNING: ThreadSanitizer: data race (pid=1554)
  Read of size 8 at 0x555555558018 by thread T2:
    #0 work race.c:4 (race_t+0x1263)

  Previous write of size 8 at 0x555555558018 by thread T1:
    #0 work race.c:4 (race_t+0x128d)

  Location is global 'counter' of size 8 at 0x555555558018 (race_t+0x4018)
  ...
SUMMARY: ThreadSanitizer: data race race.c:4 in work
```

On this WSL kernel TSan binaries sometimes abort with "unexpected memory mapping"; run them as
`setarch $(uname -m) -R ./prog` (it disables address randomisation for that run).

**A verified TSan limitation.** With GCC 13.3 and clang 18.1 on glibc 2.39, a TSan build that uses
glibc's `<threads.h>` crashes at start-up:

```text
ThreadSanitizer:DEADLYSIGNAL
==1530==ERROR: ThreadSanitizer: SEGV on unknown address 0x000000000018 (pc 0x7ffff6eb3f90 ...)
```

glibc's `thrd_create`, `mtx_lock` and `cnd_wait` call glibc's internal pthread functions directly,
so TSan's interceptors (which hook the public `pthread_*` symbols) never see the new thread or the
locks. `m14_c11_threads_atomics.c` therefore has a small `-DUSE_PTHREADS` shim that maps the C11
names onto pthreads for the TSan build. That is worth knowing before you choose C11 threads for a
codebase you want to race-check.

## 3. Code

All five programs build with `-pthread`, pass their self-checks, and run clean under TSan using
the command in each file's `TSan:` header line.

### 3.1 `m14_pthreads_basics.c`

Arguments in through per-thread job structs, a result returned as a heap pointer, a mutex
counter, `pthread_once`, a read-write lock with an invariant check, and a detached thread that
reports through a semaphore. Real output:

```text
parallel sum over 4 threads: 2999997
mutex counter: 400000 (expected 400000)
rwlock: 60000 reads during 200 updates, torn reads = 0
all checks passed
```

The rwlock test is the useful pattern: the writer updates four fields that must stay equal, and
the readers count how often they see them unequal. Zero torn reads is the evidence that the lock
protects the invariant, not just the individual fields.

### 3.2 `m14_bounded_queue.c`

One mutex, two condition variables (`not_empty`, `not_full`), predicate loops, a timed pop on
`CLOCK_MONOTONIC`, and `bq_close` that broadcasts. Four producers push 20 000 values each through
a 16-slot queue to four consumers:

```text
4 producers -> 4 consumers: 80000 items, sum 800040000 (expected 800040000)
timed pop on empty queue: ETIMEDOUT, waited at least 45 ms
all checks passed
```

The deadline arithmetic is a classic bug site, so it is worth reading:

```c
struct timespec deadline;
clock_gettime(CLOCK_MONOTONIC, &deadline);     /* absolute deadline, computed once */
deadline.tv_sec += timeout_ms / 1000;
deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
if (deadline.tv_nsec >= 1000000000L) {         /* normalise, or timedwait fails with EINVAL */
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
}
```

Computing the deadline once, before the loop, means spurious wakeups do not extend the total wait.

### 3.3 `m14_thread_pool.c`

```c
static void *worker_main(void *arg)
{
    struct pool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->stopping)
            pthread_cond_wait(&p->has_work, &p->mu);
        if (!p->head) {                     /* stopping and nothing left */
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        struct task *t = p->head;
        p->head = t->next;
        if (!p->head)
            p->tail = NULL;
        pthread_mutex_unlock(&p->mu);

        t->fn(t->arg);                      /* run WITHOUT the lock: tasks may be slow */
        free(t);

        pthread_mutex_lock(&p->mu);
        if (--p->pending == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->mu);
    }
}
```

Real output:

```text
10000 square tasks done on 4 workers
50000 counter tasks drained at shutdown: hits = 50000
all checks passed
```

Compare it line by line with the C++ pool in `04_Concurrency/code/m09_thread_pool.hpp`: the
logic is the same, and most of the extra C lines are cleanup that destructors would do.

### 3.4 `m14_c11_threads_atomics.c`

`thrd_create`, `mtx_t`, `cnd_t` with a predicate loop and `call_once`; a relaxed atomic counter;
a spinlock from `atomic_flag`; release/acquire message passing; and `thread_local`:

```text
atomic counter = 400000, spinlock counter = 400000
message passed with release/acquire: account 1001 amount 250000
all checks passed
```

### 3.5 `m14_reentrant.c`

```c
/* Count "key=value" pairs in "a=1;b=2;c=3". The inner strtok call resets the hidden cursor,
 * so the outer loop loses its place after the first pair. */
for (char *item = strtok(s, ";"); item; item = strtok(NULL, ";")) {
    char *key = strtok(item, "=");
    char *val = strtok(NULL, "=");
    if (key && val)
        pairs++;
}
```

Real output:

```text
pairs counted with strtok: 1, with strtok_r: 3
errno per thread: 22 and 12 survived each other
all checks passed
```

The `strtok` bug needs no threads at all: a nested call in one thread is enough. That is the
difference between "not reentrant" and "not thread-safe". The file also shows that
`localtime()` returns the same pointer on every call and that a `_Thread_local` buffer gives
each thread its own copy.

## 4. Under the hood (senior-level)

**A mutex is a futex.** glibc's `pthread_mutex_t` (40 bytes on x86-64) holds an integer state.
An uncontended `lock` is one atomic compare-and-swap in user space: no system call. Only when the
CAS fails does the thread call `futex(FUTEX_WAIT)` to sleep in the kernel on that address, and
`unlock` calls `futex(FUTEX_WAKE)` only if someone is waiting. That is why uncontended locks cost
tens of nanoseconds and contended ones cost microseconds (typical figures): the expensive part is
contention, not locking. Condition variables are built on futexes too, with sequence counters so
that wakeups are not lost between the unlock and the sleep.

**What `pthread_create` does.** glibc `mmap`s a stack (8 MiB by default, from `ulimit -s`) with a
guard page below it, sets up the thread's TLS block and its thread control block, and calls
`clone` with flags that share the address space, files, filesystem info and signal handlers
(`CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | ...`). To the kernel a thread
is a task that happens to share memory. `_Thread_local` variables live in the TLS block, addressed
through the `%fs` segment register on x86-64; `errno` is one of them.

**Memory ordering on x86.** x86-64 is close to sequentially consistent ("total store order"):
the only reordering the hardware does is a later load passing an earlier store to a different
address. So acquire loads and release stores compile to plain `mov` instructions; the
`memory_order` still matters because it stops the **compiler** from reordering. A `seq_cst` store
needs `xchg` (or `mov` plus `mfence`). On ARM, which reorders much more, acquire and release
become `ldar` and `stlr`. Code that "works on x86" with the wrong order can fail on an ARM server.

**Why spinlocks are rarely right in user space.** A spinning thread burns its time slice; if the
lock holder was preempted, every waiter spins until the holder runs again. A futex mutex spins
briefly (glibc's adaptive mutex type) and then sleeps. Spinlocks fit kernels with preemption
disabled, not application servers. The `atomic_flag` spinlock in `m14_c11_threads_atomics.c` is
there to teach acquire and release, and it yields instead of spinning hard.

**How TSan works.** Every memory access is instrumented; for each 8-byte word TSan keeps shadow
cells recording the last few accesses with the thread id and a vector clock. Locks, atomics,
thread create and join update the clocks. Two accesses to one word, at least one a write, where
neither clock happens-before the other, form a race. The shadow memory is why TSan needs a fixed
address-space layout (hence the `setarch -R` workaround) and costs about 5 to 15 times in CPU
(typical).

## 5. In production

- **Locks held across I/O are the top latency bug.** A thread takes the session-table lock,
  then writes a log line to a slow disk or calls the database. Every other request queues
  behind it. In a dump, many threads are blocked in `EnterCriticalSection` or `futex_wait` on the
  same lock, and one thread holding it is in `WriteFile` or `recv`. Fix: copy what you need under
  the lock, release, then do I/O.
- **Lock-order inversion.** Path one locks the connection table, then a session; path two locks a
  session, then the connection table to remove itself. Under load they deadlock. In gdb:
  `thread apply all bt` shows two threads each waiting for the lock the other holds; in WinDbg,
  `!locks` and `~*kb`. The fix is a documented order (04_Concurrency/06_deadlocks_livelocks_starvation.md).
- **`if` instead of `while` around a wait.** Works for months, then a second consumer thread is
  added and the queue pop crashes on an empty queue once a week. Grep every
  `pthread_cond_wait` and `SleepConditionVariable*` call for a surrounding `while`.
- **Non-reentrant library calls in a threaded server.** `localtime`, `strtok`, `inet_ntoa` and
  `gethostbyname` used from worker threads give log lines with another request's timestamp or a
  corrupted token. They are easy to find with grep and easy to replace with the `_r` versions.
  On MSVC the CRT makes many of these per-thread, so code ported to Linux can break.
- **Shutdown races.** At exit, `main` frees globals while a detached thread still uses them, or a
  worker is still running a task when the pool's memory is freed. Make shutdown explicit: set the
  stop flag, wake everyone, join every thread, then free.
- **Run TSan on the test suite.** It finds races that never reproduced under a debugger. Add a
  TSan build to CI for any component with threads, and treat every report as a bug
  (04_Concurrency/12_debugging_concurrency_bugs.md).

## 6. Try it

1. **Easy - the loop-variable bug.** Write a program that passes `&i` from a `for` loop to four
   threads that print it. Run it a few times, then run it under TSan. Fix it with an array of
   ints. Done when you can explain the original output and TSan's report.
2. **Easy - error numbers.** Lock a default mutex twice from one thread with a 1-second
   `pthread_mutex_timedlock`, then repeat with an `ERRORCHECK` mutex and plain `lock`. Done when
   you can show `ETIMEDOUT` in the first case and `EDEADLK` in the second, printed with `strerror`.
3. **Medium - break the queue.** Change one `while` in `m14_bounded_queue.c` to `if` and run the
   test 200 times in a loop, then under TSan. Done when you either catch a failure or can explain
   why it is still wrong even though you did not.
4. **Medium - priority pool.** Give `m14_thread_pool.c` two priority levels (two queues; workers
   always drain the high queue first). Done when a test with a slow low-priority flood shows
   high-priority tasks still finish quickly, and TSan is clean.
5. **Hard - rwlock versus mutex.** Benchmark the rate table from `m14_pthreads_basics.c` with a
   read-write lock and with a mutex, for 1, 2, 4 and 8 reader threads and a critical section of
   10 ns and 1 microsecond (busy loop). Done when you have a table and can explain where the
   rwlock starts to win.
6. **Hard - Win32 port.** Port `m14_bounded_queue.c` to Win32 with `SRWLOCK` and
   `CONDITION_VARIABLE` (`SleepConditionVariableSRW`, `WakeConditionVariable`,
   `WakeAllConditionVariable`). Done when it passes the same test on Windows; compare with
   04_Concurrency/code/m13_win32_srwlock_condvar.c.

## 7. Interview questions

**Q: Why must `pthread_cond_wait` be called in a `while` loop?**
A: POSIX allows spurious wakeups, and even a real wakeup can find the condition false again
because another thread consumed the state between the signal and this thread reacquiring the
mutex. Re-checking the predicate under the mutex after every wakeup handles both cases. The
predicate must be shared state protected by the same mutex.

**Q: What does `pthread_cond_wait` do with the mutex?**
A: It must be called with the mutex held. It atomically releases the mutex and puts the thread to
sleep, so no signal can slip in between the check and the sleep, and it reacquires the mutex before
returning, so the caller can safely re-check the predicate.

**Q: What is the difference between thread-safe and reentrant?**
A: Thread-safe means correct when called concurrently from several threads, possibly using locks
or per-thread state. Reentrant means correct when re-entered before a previous call in the same
thread has finished, as with recursion, nested calls or signal handlers, which requires no hidden
shared state. `strtok` is neither; a version with a thread-local cursor would be thread-safe but
still not reentrant; `strtok_r` is both.

**Q: When do you use an atomic instead of a mutex?**
A: For a single variable updated with a simple operation, such as a counter, a flag or a pointer
swap, where an atomic read-modify-write is enough. As soon as an invariant spans several variables,
use a mutex. Atomics also require choosing a memory order; relaxed suffices for a pure counter,
release/acquire for publishing data.

**Q: Is `volatile` enough for a flag shared between threads?**
A: No. `volatile` forces the compiler to load and store the variable each time, but it gives no
atomicity and no ordering with respect to other memory, and a concurrent non-atomic access is still
a data race and therefore undefined. Use `_Atomic` (for example `atomic_bool` with release/acquire)
or a mutex.

**Q: How do you shut down a thread pool cleanly?**
A: Under the queue lock set a stopping flag and broadcast the condition variable so every idle
worker wakes. Workers finish (or drain) the queue, see the flag with an empty queue and return.
The owner joins every worker before freeing the pool, so no thread touches freed memory.

**Q: How does a futex-based mutex avoid system calls?**
A: The lock word lives in user memory; an uncontended lock or unlock is a single atomic
instruction. Only on contention does a thread call the `futex` system call to sleep on that address,
and unlock wakes a sleeper through `futex` only if the state shows there are waiters.

**Q: How do you find a data race?**
A: Build with ThreadSanitizer (`-fsanitize=thread -g -O1`) and run the tests or a load test; it
reports both conflicting accesses with stacks and the threads' creation points. Races rarely show up
in a debugger because breakpoints change timing, so TSan, Helgrind and careful review of which lock
protects each shared field are the main tools.

## 8. Recap

- A variable written by one thread and accessed by another needs a lock or an atomic; anything
  else is a data race and undefined behaviour.
- `pthread_create` takes a `void *(*)(void *)`; give each thread its own argument struct; join
  or detach every thread; pthread calls return error numbers, not `errno`.
- Mutex: short critical sections, a documented lock order, never across blocking I/O.
- Condition variable: wait in a `while` on a predicate protected by the mutex; broadcast for
  shutdown; absolute deadlines on `CLOCK_MONOTONIC`.
- rwlock for read-mostly data with non-trivial critical sections; watch writer starvation.
- `pthread_once` / `call_once` for one-time init; never a hand-rolled flag.
- C11: `thrd_t`, `mtx_t`, `cnd_t`, `call_once`, `thread_local`, and `<stdatomic.h>` with relaxed,
  release/acquire and seq_cst orders.
- Thread-safe is not reentrant: static buffers break both; `_Thread_local` fixes only the first;
  use the `_r` functions.
- A thread pool: queue under one mutex, tasks run unlocked, pending counter for wait-idle, drain
  and join on shutdown.
- TSan finds races that tests and debuggers miss; on this setup it needs `setarch -R`, and it
  cannot see glibc's `<threads.h>`, so race-check C11 threads code through pthreads.
