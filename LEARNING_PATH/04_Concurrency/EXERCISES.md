# 04_Concurrency — all exercises

Every exercise from section 6 ("Try it") of modules 01 to 13, in one place, with a difficulty and a
hint. The full "done when" condition is in the module; it is repeated here in short form so you can
tick exercises off without opening the module.

How to use this file:
- Do every **easy** exercise as you finish each module. They take 10 to 30 minutes.
- Do at least two **medium** exercises per module. They are where the understanding sticks.
- Pick the **hard** exercises that match your resume story (thread pool, deadlock RCA, lock-free,
  Win32 mapping). Each one is a talking point in an interview.
- Build everything with `code/build.sh` on WSL Ubuntu 24.04. TSan builds land in `code/build/tsan/`.

TSan note for WSL Ubuntu 24.04: if a TSan binary dies immediately with
`FATAL: ThreadSanitizer: unexpected memory mapping`, the kernel's ASLR entropy is higher than
TSan expects. Run it as `setarch $(uname -m) -R ./build/tsan/prog`, or once per boot
`sudo sysctl vm.mmap_rnd_bits=28`. This is an environment issue, not a bug in your code.

Legend: **E** = easy, **M** = medium, **H** = hard.

---

## Module 01 — Processes, threads and the OS (`01_processes_threads_and_the_os.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 1.1 | E | Run `m01_threads_basics.cpp` with 100 workers; remove the `cout_mtx` lock and find an interleaved line. | `std::cout << a << b` is several separate calls; another thread can write between any two of them. Run 5 to 10 times. |
| 1.2 | E | Print `hardware_concurrency()`; after creating 50 threads read `Threads:` from `/proc/self/status`. Done when it says 51. | Keep the 50 threads blocked (on a latch or a sleep) while you read the file, or they may have exited already. |
| 1.3 | M | Let a joinable `std::thread` go out of scope; observe `terminate`. Explain why the committee chose terminate over detach. | Think about a lambda capturing a local by reference: implicit detach would leave a thread reading a dead stack frame. |
| 1.4 | M | Time 10,000 create+join cycles vs pushing 10,000 items through one reused thread via a queue. | Use `steady_clock`, divide by 10,000. Creation costs a `clone` syscall, a stack `mmap` and scheduler work; a queue push costs a lock and a notify. |
| 1.5 | H | `thread_local` struct with printing ctor and dtor; three threads; show 3 constructions and 3 destructions before `main` prints "exiting". | Construction happens on first use in each thread, not at thread start. Destruction runs during thread exit, so join before printing "exiting". |

## Module 02 — Data races and why they are UB (`02_data_races_and_why_they_are_ub.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 2.1 | E | Build `m02_race_demo.cpp` at `-O0` and `-O2`, run 5 times each, explain why `-O2` sometimes prints exactly 1,000,000. | At `-O2` the compiler may hoist the counter into a register and store once at the end, so the threads' updates do not interleave; one thread's final store overwrites the other's. |
| 2.2 | E | Build with `-fsanitize=thread`; find "previous write", "current read" and thread creation sites. | Read the report top-down: two stacks, then "Location is global ...", then "Thread T1 created at". Look for your file name, skip libstdc++ frames. |
| 2.3 | M | `while (!ready) {}` with plain `bool` at `-O2` hangs; find the non-reloading loop in `g++ -O2 -S`; fix with `std::atomic<bool>`. | Look for a `jmp` to itself (`.L3: jmp .L3`). The compiler may assume no other thread writes a non-atomic, so it loads `ready` once. |
| 2.4 | M | Store-buffering litmus test, 1,000,000 runs with a start barrier; count `r1 == 0 && r2 == 0`. | Use an atomic counter both threads increment then spin on until it reaches 2. The reorder comes from the store buffer; only `seq_cst` (an `mfence` or `xchg` on x86) forbids it. |
| 2.5 | H | Audit a stats struct updated by workers with no lock; mark every conflicting pair without happens-before; rewrite minimally; TSan quiet. | Per-field: is it written by more than one thread? Read by a reporter thread? Pure counters become relaxed atomics; fields that must be consistent together need one mutex or a snapshot. |

## Module 03 — Mutexes, lock_guard, unique_lock (`03_mutexes_lock_guard_unique_lock.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 3.1 | E | Remove the `lock_guard` in `m03_mutex_counter.cpp`'s `increment`; see wrong totals; restore. | Same lost-update mechanism as module 02. Run several times; the loss varies. |
| 3.2 | E | Replace `scoped_lock(from.m, to.m)` with two sequential `lock_guard`s; hang; `gdb -p PID`, `thread apply all bt`. | Look for two threads in `__lll_lock_wait` (or `futex_wait`). Go up to your `transfer` frame in each and print which account it locked first. |
| 3.3 | M | `foo()` locks `m` and calls `bar()` which also locks `m`; fix with `bar_locked()`; paragraph on why `recursive_mutex` is wrong. | When `bar` runs inside `foo`, `foo`'s invariant may be half-updated. A recursive mutex lets `bar` see that broken state silently. |
| 3.4 | M | Uncontended vs contended lock cost: 10 million `lock_guard` on one thread vs two threads. | Uncontended is one atomic CAS plus one atomic store (typically tens of ns or less). Contended adds `futex` syscalls and context switches; count them with `strace -c -f`. |
| 3.5 | H | `ConfigStore`: version A reads the file under the lock, version B reads into a local map then swaps. Measure reader p99. | Store each `get()` duration in a per-thread vector, merge, sort, take index `0.99 * n`. B's critical section is a pointer swap; A's is file I/O. |

## Module 04 — shared_mutex and readers-writers (`04_shared_mutex_and_readers_writers.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 4.1 | E | Run `m04_shared_mutex_map.cpp`; note the three throughputs; sharded beats single with writers. | Sharding divides contention by the shard count because unrelated keys stop sharing a lock. |
| 4.2 | E | Make `get` return `const std::string*` into the map; ASan with 8 readers and 2 writers doing `del` and `set`. | The pointer outlives the `shared_lock`; a writer erases the node; the reader dereferences freed memory. Return by value or by `shared_ptr`. |
| 4.3 | M | Replace `shared_mutex` with plain `mutex`; rerun 8 readers, 0 writers. | `shared_lock` still does an atomic RMW on one shared counter, so readers bounce one cache line. With tiny critical sections a plain mutex can win. |
| 4.4 | M | Pre-build keys so the read section has no allocation; find where plain mutex matches or beats `shared_mutex`. | The shorter the critical section, the more the lock's own cost dominates. `shared_mutex` only pays off when readers hold it long enough to overlap. |
| 4.5 | H | Show writer starvation with 8 tight readers; build a fair rwlock (writer-waiting flag blocks new readers); compare max writer wait. | Fields: `readers`, `writer_active`, `writers_waiting`, one mutex, two condition variables. Readers wait while `writer_active || writers_waiting > 0`. Verify with TSan. |

## Module 05 — Condition variables and wait objects (`05_condition_variables_and_wait_objects.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 5.1 | E | Replace `wait(lk, pred)` with bare `wait(lk)` plus `if (q_.empty()) return nullopt;`; 4 consumers; observe failures. | Name the cause: spurious wakeup, stolen wakeup (another consumer took the item first) or wake for close. Each makes an empty queue look like "closed". |
| 5.2 | E | Move `closed_ = true` outside the lock in `close()`; write the lost-wakeup interleaving with line numbers. | Consumer checks predicate (false) -> producer sets flag and notifies -> consumer blocks. The notify hit nobody. The lock makes "check and block" atomic relative to the setter. |
| 5.3 | M | `Semaphore` from mutex + cv + int; auto-reset event on top (coalescing) and manual-reset event (bool + `notify_all`). Show the auto-reset event losing the second of two back-to-back signals. | Auto-reset: `if (count == 0) count = 1;` in signal. Two signals with no wait in between leave count at 1. Compare with `code/m13_win32_threads.c`. |
| 5.4 | M | Add `size()`, `waiting_consumers` and `dump()`; remove `notify_one` from `push`; show `size > 0, waiters > 0`. | Increment `waiting_consumers` before the wait and decrement after, both under the lock. That pair of numbers is the signature of a lost notify in a production dump. |
| 5.5 | H | `EventSet` wait-for-any with one mutex, one cv, a `std::bitset`; 8 sources, 1 waiter; every signal received once. Explain why `notify_one` suffices. | `wait_any` waits for `bits.any()`, finds the lowest set bit, clears it. With one waiter `notify_one` is enough; with several waiters waiting for different subsets you need `notify_all`. |

## Module 06 — Deadlocks, livelocks, starvation (`06_deadlocks_livelocks_starvation.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 6.1 | E | `./m06_deadlock_demo hang`; gdb `thread apply all bt`; write "T1 holds A waits B; T2 holds B waits A". | In each `transfer_broken` frame, `print from.name` and `print to.name`. The thread waits on `to`; it holds `from`. |
| 6.2 | E | In gdb, print the owner of the mutex each thread waits on and match to `info threads`. | `frame N` into the lock call, then `print to.m._M_mutex.__data.__owner`: that is the LWP id shown in `info threads`. |
| 6.3 | M | Three-thread cycle A->B, B->C, C->A; dump stacks; draw the cycle; fix with ranks; 10,000 iterations clean. | Give A=30, B=20, C=10 and require descending acquisition. T3 then takes A before C, which breaks the cycle. |
| 6.4 | M | Replace the try-lock jitter with a bare `yield`; pin to 2 cores; compare throughput. | Without randomness both threads retry in lock-step and keep colliding (livelock). Random back-off desynchronises them, like Ethernet's exponential back-off. |
| 6.5 | H | Write a one-page lock order document for a real or stand-in server; enforce with `hierarchical_mutex` in debug; test that a violation is caught. | List every lock, give ranks with gaps (10, 20, 30), mark leaf locks (logger, metrics). The test should take two locks in the wrong order and expect the throw or abort. |

## Module 07 — Atomics and the memory model (`07_atomics_and_the_memory_model.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 7.1 | E | Run `m07_atomic_counter_orders.cpp`; compare store timings; find the `xchg` in the `-S` output. | A `seq_cst` store compiles to `xchg` (implicitly locked, a full barrier) on x86; relaxed and release stores are a plain `mov`. |
| 7.2 | E | Make `m07_message_passing.cpp` relaxed on both sides; TSan reports a race on `payload`; restore acquire/release. | TSan checks the C++ model, not x86. Relaxed creates no happens-before, so the payload accesses are unordered by the language even if the hardware happens to keep them in order. |
| 7.3 | M | Long-lived store-buffering harness with a round counter, 10 million rounds; both-zero count per order. | acq/rel does not forbid store-load reordering; only `seq_cst` does. Expect relaxed and acq/rel non-zero, `seq_cst` exactly zero. |
| 7.4 | M | Refcount with relaxed increment; choose and justify the decrement order. | Every thread's writes to `p` must happen-before the `delete`. Release on each decrement publishes them; acquire on the one that hits zero receives them. Hence `acq_rel`, or release plus an acquire fence. |
| 7.5 | H | Peterson's lock with atomics; `seq_cst` gives exactly 2,000,000; release/acquire breaks it. Name the reorder. | Each thread stores `flag[me]` then loads `flag[other]`: store-then-load to different addresses, which is the store-buffering pattern. Both can read false and both enter. |

## Module 08 — Lock-free basics (`08_lock_free_basics.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 8.1 | E | `m08_spinlock.cpp` vs mutex at 1, 2, 4, 8 threads; then `taskset -c 0`. | On one core a spinner burns its whole time slice while the holder is descheduled; a mutex sleeps and lets the holder run. |
| 8.2 | E | `m08_spsc_ring.cpp`: change `tail_` store to relaxed; TSan reports the race on `buf_`. | The release store on `tail_` is what publishes the slot write. Without it the consumer's read of `buf_` has no happens-before. |
| 8.3 | M | `m08_treiber_stack.cpp` with `-DBROKEN -fsanitize=address`, 4 threads; write the use-after-free interleaving. | T1 loads `head` and reads `head->next`; meanwhile T2 pops the same node and deletes it. T1's read touches freed memory. That gap is what hazard pointers close. |
| 8.4 | M | Cache the other side's index in `SpscRing`; measure before and after. | The producer only reloads `head_` (acquire) when its cached copy says "full". A stale cached value is always conservative, so correctness holds. |
| 8.5 | H | Hazard pointers for the Treiber stack; 8 threads, 10 million ops; ASan and TSan silent; state the memory bound. | Publish the hazard with `seq_cst` (or a fence) then re-check `head` before using the node. Bound: at most `H + R` unreclaimed nodes, where `H` is the hazard count and `R` is the scan threshold. |

## Module 09 — Thread pools and task queues (`09_thread_pools_and_task_queues.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 9.1 | E | Run `m09_thread_pool_demo.cpp`; confirm sum, caught exception, and `completed()` equals submitted. | The exception is delivered through the `packaged_task`'s future. `get()` rethrows it on the caller's thread. |
| 9.2 | E | Run `m09_async_pitfall.cpp`; show the discarded-future `std::async` call is sequential. | Three 200 ms jobs take about 600 ms because each temporary future's destructor joins. Keeping the futures in a vector gives about 200 ms. |
| 9.3 | M | Break shutdown with `if (closed_) return;` before taking a task; 1,000 tasks of 1 ms; destroy immediately; see lost tasks; restore. | The correct exit is "closed AND queue empty". Checking only `closed` exits with work still queued. |
| 9.4 | M | `try_submit` returning `false` when full, plus a sampler printing queue depth every 100 ms during a 10,000-submit burst. | `try_submit` is `submit` with the `not_full_` wait replaced by `if (q_.size() >= max_queue_) return false;` under the same lock. |
| 9.5 | H | Self-deadlock: 4 workers, each task submits a sub-task and calls `get()`; 8 outer tasks. Fix with inline execution and with a second pool. | All 4 workers block in `future::get`; the sub-tasks sit in the queue with no free worker. `thread_local bool in_pool` set in `worker_loop` lets `submit` run inline. |
| 9.6 | H | Cooperative cancellation via `std::atomic<bool> stop_requested_`; a 100 x 10 ms task exits within 20 ms of `shutdown_now()`. | Check the flag between each 10 ms step with a relaxed load (the join after shutdown gives the ordering you need). Measure from `shutdown_now()` to the end of join. |

## Module 10 — Cache lines, false sharing, performance (`10_cache_lines_false_sharing_performance.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 10.1 | E | Run `m10_false_sharing.cpp`; record packed, padded, true-shared; padded/packed ratio at max threads. | Close other programs. On WSL the Windows host shares the CPU, so run 3 times and take the median. |
| 10.2 | E | Try `alignas(32)` and `alignas(128)`; explain 32 and whether 128 differs. | 32 puts two slots per 64-byte line, so pairs of threads still fight. 128 can help on Intel because the adjacent-line prefetcher pulls lines in pairs. |
| 10.3 | M | `StripedCounter` vs one `std::atomic<long>` at 1, 4, 8 threads; then measure `read()` cost. | Writes go to the thread's own padded stripe (index by a hashed thread id). `read()` sums all stripes, so reads get slower as writes get faster. |
| 10.4 | M | Pin threads with `pthread_setaffinity_np`; variance across 5 runs pinned vs unpinned. | Use `cpu_set_t`, `CPU_ZERO`, `CPU_SET(t % ncpu)`. Report min, median and max of each set of 5 runs. |
| 10.5 | H | Per-worker stats in the module 09 pool, packed vs padded; 1,000,000 tiny tasks; solve Amdahl for `s`. | `speedup = 1 / (s + (1 - s) / N)`; solve `s = (N / speedup - 1) / (N - 1)`. Expect the queue mutex, not the stats, to dominate `s`. |

## Module 11 — Concurrency patterns for servers (`11_concurrency_patterns_for_servers.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 11.1 | E | Run `m11_call_once_singleton.cpp` ("constructed: 1" for all three forms); write the broken plain-pointer DCLP and catch it with TSan. | Copy form 1 and change `std::atomic<Table*>` to `Table*`. TSan reports the unlocked first read racing with the write under the mutex. |
| 11.2 | E | Run `m11_rcu_snapshot_cache.cpp`; zero inconsistent reads; note the publish time. | If you have fewer than 9 hardware threads, also run `./m11_rcu_snapshot_cache 3`. The median publish is the honest number; the tail includes scheduling. |
| 11.3 | M | Replace the snapshot with a `shared_mutex` table rebuilt in place under the exclusive lock; compare reader p99 with and without the refresher. | Rebuilding in place makes every reader wait for the whole build. The snapshot version's p99 should barely move when the refresher runs. |
| 11.4 | M | `ConnectionPool` with fake 1 ms `Conn`: 16 workers, pool of 4, 50 ms acquire timeout; count successes and timeouts; leak a lease. | Lease = RAII object whose destructor returns the connection. Leak by `new`-ing a lease and never deleting it; the in-use metric stays one higher forever. |
| 11.5 | H | `Parser` and `Accountant` actors with bounded mailboxes, replies via `std::promise`; then mailboxes of 2 to build a mailbox deadlock. | Deadlock: Parser blocks sending to a full Accountant mailbox while Accountant blocks replying into a full Parser mailbox. Each holds its own thread (the resource) and waits for the other. |

## Module 12 — Debugging concurrency bugs (`12_debugging_concurrency_bugs.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 12.1 | E | `m12_tsan_demo` under TSan: `race`, `order`, `wakeup`. One-line fix for each. | Fixes: lock (or atomic) on `hits`; one global order for `m_cache`/`m_conn`; set `ready` under `wm`. If TSan dies with "unexpected memory mapping", use `setarch $(uname -m) -R`. |
| 12.2 | E | ASan build, `uaf` mode; find the freeing stack and the using stack; state the ownership fix. | ASan prints three stacks: the bad access, "freed by thread T0 here", "previously allocated by". Fix with `shared_ptr` or by joining the worker before `delete`. |
| 12.3 | M | Run `m06_deadlock_demo hang` under `valgrind --tool=helgrind`; compare with TSan's lock-order-inversion. | Both report the inversion from the lock graph before the actual hang. helgrind is slower but needs no rebuild. Kill the program once the report appears. |
| 12.4 | M | Add `RACE_POINT` to `m05_producer_consumer.cpp` between predicate check and wait; remove the lock around `closed_ = true`; hang every run. | Inline `while (!pred) { RACE_POINT("gap"); cv.wait(lk); }`. `m12_tsan_demo.cpp`'s `wakeup` mode is a worked example; build with `-DRACE_POINTS`. |
| 12.5 | H | `gcore` the hung `m06_deadlock_demo`; from the core alone find both threads, each mutex's `__owner`, and write the wait-for graph; repeat for a 3-lock cycle. | `gdb ./m06_deadlock_demo core.PID`, `info threads`, `thread N`, `bt`, `frame K`, `print *mutex_ptr`. Owner is an LWP id; map it with `info threads`. |
| 12.6 | H | Per-thread ring-buffer event log plus a `sigaction` crash handler that dumps every thread's ring. | Register each ring in a fixed-size global array at thread start (no allocation in the handler). In the handler use `write(2, ...)`, not `printf`; only async-signal-safe calls. |

## Module 13 — Win32 concurrency reference (`13_win32_concurrency_reference.md`)

| # | Diff | Exercise | Hint |
|---|------|----------|------|
| 13.1 | E | Build and run `m13_win32_threads.c`; see the auto-reset Event lose a wake and the Semaphore not. | An Event is one bit: signalled or not. A Semaphore is a count. Two `SetEvent` calls with no waiter in between set the same bit twice. |
| 13.2 | E | Run `m13_win32_srwlock_condvar.c`; annotate it line by line against `m05_producer_consumer.cpp`. | `AcquireSRWLockExclusive` = `unique_lock`, `SleepConditionVariableSRW` in a `while` = `cv.wait(lk, pred)`, `WakeAllConditionVariable` = `notify_all`. |
| 13.3 | M | Inventory every primitive in your real server; give the C++17 equivalent and whether conversion changes behaviour. | Watch for three behaviour changes: recursion (CRITICAL_SECTION allows it, `std::mutex` does not), coalescing (Events), and cross-process (named kernel objects). |
| 13.4 | M | `WaitOnAddress`-based spin-then-block mutex; compare uncontended cost with CRITICAL_SECTION. | Use three states: 0 unlocked, 1 locked, 2 locked with waiters. Unlock only calls `WakeByAddressSingle` if the old state was 2. This is exactly the glibc futex mutex design. |
| 13.5 | H | Module 09 pool on the Vista thread pool (`CreateThreadpoolWork`) with your own completion signalling; compare with `m09_thread_pool.hpp`. | The OS pool gives you thread management and sizing. It does not give futures, exception transport or a bounded queue; you build those with a per-task struct plus SRWLOCK and CONDITION_VARIABLE. |

---

## Suggested order if you are short of time

1. All E exercises in modules 02, 03, 05, 06, 07 (the core of every concurrency interview).
2. 5.3 (semaphore and events: maps directly to your Win32 work), 6.3, 7.4, 9.3, 9.5.
3. One H per interview story: 6.5 (lock order document), 9.5 (pool self-deadlock),
   12.5 (deadlock from a core), 13.5 (Win32 pool vs std pool).
