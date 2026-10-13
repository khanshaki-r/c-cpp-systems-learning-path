# 09. Thread pools and task queues

**Time:** ~4 hours   **Prerequisites:** 03, 05

## 1. In plain words

A thread pool is a fixed set of worker threads that sit waiting on a queue of tasks. Anyone who has
work to do puts a task in the queue and, optionally, keeps a ticket (a future) to collect the
result later. A worker takes the next task, runs it, and goes back to the queue. Threads are
created once at startup, not per request, so a burst of ten thousand requests becomes ten thousand
queue entries handled by thirty-two threads, not ten thousand threads fighting for eight cores.

This is the "pooled request model" of your application server, described in portable terms. The
Win32 version uses `CreateThread` for the workers, a `CRITICAL_SECTION` around a linked list of
requests, and an Event or Semaphore to wake workers. The C++17 version uses `std::thread`,
`std::deque`, `std::mutex` and `std::condition_variable`, and adds two things C makes painful:
a typed result that travels back to the submitter (`std::future<T>`) and exceptions that cross the
thread boundary intact instead of becoming a return code that someone forgets to check.

The design question interviewers ask is always the same: **how do you shut it down without losing
a task and without hanging?** This module answers it with a protocol you can prove correct.

## 2. The real thing

### 2.1 Futures and promises

A `std::promise<T>` is the writing end; its `std::future<T>` is the reading end. The producer calls
`promise.set_value(v)` or `promise.set_exception(ptr)`; the consumer calls `future.get()`, which
blocks until one of those has happened, then returns `v` or rethrows the exception. `get()` may be
called once. `future.wait_for(d)` returns `std::future_status::ready`, `timeout`, or `deferred`.

```cpp
std::promise<int> p;
std::future<int> f = p.get_future();
std::thread t([&p] { p.set_value(compute()); });   // or p.set_exception(std::current_exception())
int result = f.get();                                // blocks; rethrows if set_exception
t.join();
```

A `std::packaged_task<R(Args...)>` wraps a callable and a promise: calling the task runs the
callable and stores its return value, or the exception it threw, into the promise. That is exactly
the wrapper a thread pool needs: the worker calls `task()`, and whatever happens is delivered to the
submitter's future. A worker never sees the exception; it cannot be killed by one.

`std::shared_future<T>` is a copyable future; many threads may `get()` it (each gets a copy of the
value). Use it to broadcast a one-shot result (a "config loaded" signal, for instance).

Mapping to Win32: a future is a manual-reset Event plus a result slot plus an exception slot. You
have built this by hand with a `struct Request { HANDLE done; int rc; ... }`. `std::future` is that
struct with the lifetime problems solved.

### 2.2 `std::async` and why not to build a server on it

```cpp
auto f = std::async(std::launch::async, work, arg);   // runs on a new thread, now
auto g = std::async(std::launch::deferred, work, arg);  // runs on the calling thread at g.get()
auto h = std::async(work, arg);                          // implementation chooses; usually async
```

Pitfalls:

1. **The future's destructor blocks.** A future returned by `std::async` with `launch::async`
   waits for the thread in its destructor. `std::async(std::launch::async, f);` with the result
   discarded is therefore *synchronous*: the temporary future is destroyed at the semicolon and
   blocks until `f` finishes. This surprises everyone once.
2. **No pool.** Every `launch::async` call creates a new thread (libstdc++) or uses a system pool
   (MSVC). Ten thousand calls is ten thousand threads on Linux. There is no bound and no queue.
3. **Default policy is unspecified.** Without a policy, the call may be deferred and never run
   until `get()`, which with a `wait_for` loop means you poll forever on a `deferred` status.
4. **No cancellation, no priorities, no affinity.**

`std::async` is fine for a one-off "do this in the background and give me the answer" in a tool.
For a server, write or use a pool.

### 2.3 The fixed pool: design

```
                      submit(f, args...)
                            |
                            v   (lock m_; push; unlock; not_empty_.notify_one)
   +----------------------------------------------------------+
   |  deque<function<void()>>  tasks_     GUARDED_BY(m_)       |   bounded by max_queue_
   +----------------------------------------------------------+
        ^           ^           ^           ^
        |           |           |           |     (wait not_empty_ with predicate
    worker 0    worker 1    worker 2    worker 3       closed_ || !tasks_.empty())
```

Components:

- `std::vector<std::thread> workers_`: created in the constructor, joined in `shutdown()`.
- `std::deque<std::function<void()>> tasks_`: type-erased tasks. `std::function` requires a
  copyable callable, and `std::packaged_task` is move-only, so each task is wrapped in a
  `std::shared_ptr<std::packaged_task<R()>>` captured by a copyable lambda.
- `std::mutex m_`, `std::condition_variable not_empty_` (workers wait), `std::condition_variable
  not_full_` (submitters wait when the queue is at its bound).
- `bool closed_` under `m_`: no more submissions; workers drain and exit.

`submit`:

```cpp
template <class F, class... Args>
auto submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
{
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<R> fut = task->get_future();
    {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return closed_ || max_queue_ == 0 || tasks_.size() < max_queue_; });
        if (closed_) throw std::runtime_error("ThreadPool: submit after shutdown");
        tasks_.emplace_back([task] { (*task)(); });
    }
    not_empty_.notify_one();
    return fut;
}
```

Worker loop:

```cpp
void worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_);
            not_empty_.wait(lk, [&] { return closed_ || !tasks_.empty(); });
            if (tasks_.empty()) return;            // closed AND drained: the only exit
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        not_full_.notify_one();                    // a submitter may be waiting on the bound
        task();                                    // packaged_task: exceptions go to the future
    }
}
```

### 2.4 Shutdown without lost tasks: the proof

```cpp
void shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_) return;
        closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& w : workers_) if (w.joinable()) w.join();
}
```

Claim: every task that was successfully submitted before `shutdown()` returned is executed.

1. A successful `submit` pushes the task into `tasks_` under `m_` while `closed_ == false`.
   `closed_` is set under the same mutex, so the push and the close are totally ordered: every
   submitted task is in the deque at the moment `closed_` becomes true.
2. A worker exits only when it observes, under `m_`, `tasks_.empty()`. Since tasks are only
   removed by workers executing them, a worker observing an empty deque means every task has been
   taken by some worker.
3. A worker that has taken a task executes it before returning to the loop; `join()` waits for the
   worker to return from `worker_loop`; therefore `join()` returning means that worker's task ran.
4. `notify_all` after setting `closed_` under the mutex wakes every waiting worker (no lost wakeup,
   module 05). A worker that is running a task will finish it and re-enter the loop, see `closed_`
   or more tasks, and either take one or exit.
5. Therefore after all joins, the deque is empty and every task that was in it has been executed.

The subtle part is the order in the predicate's consumer: check `tasks_.empty()` **first**, exit
only if empty. The broken version `if (closed_) return;` before draining loses every task that was
queued when shutdown began. Interviewers probe exactly this.

`shutdown_now()`, for the "abandon everything" case: set `closed_`, swap `tasks_` with an empty
deque under the mutex (the abandoned `packaged_task`s are destroyed, which sets a
`std::future_error` with `broken_promise` on every outstanding future so submitters do not hang),
`notify_all`, join.

The destructor calls `shutdown()`. A pool destroyed with tasks still queued drains them. If you do
not want that, call `shutdown_now()` first.

### 2.5 Exceptions crossing threads

`std::packaged_task` catches whatever the callable throws and stores it via
`promise.set_exception(std::current_exception())`. The submitter's `future.get()` rethrows it on the
submitter's thread with the original type. The worker thread is unaffected. Without this (a raw
`std::function<void()>` that throws), the exception propagates out of `worker_loop`, out of the
thread's entry function, and `std::terminate()` kills the whole process. A `try/catch (...)` in the
worker loop as a last line of defence is reasonable; logging and continuing is the right policy
for a server, because one bad request must not take down the other 63 workers.

`std::exception_ptr` is the type-erased handle: `std::current_exception()` inside a catch gives you
one; `std::rethrow_exception(p)` throws it again anywhere. It is how you move an exception from a
Win32 thread's `DWORD rc` world into a typed C++ error on the caller.

### 2.6 Backpressure

An unbounded queue turns a burst into unbounded memory growth and unbounded latency: the 10,000th
request waits for 9,999 others. Bounding the queue forces a decision at submit time:

- **Block the submitter** (the code above): the accept thread stops reading from sockets, TCP
  receive buffers fill, clients see slow connects. Simple, self-regulating, and what a C server
  with a fixed-size request array does implicitly.
- **Reject**: `try_submit` returns `false` or throws; the caller returns "503 busy" immediately.
  Better latency for everyone who does get in. Required when the submitter is itself an I/O loop
  that must not block (module 11).
- **Shed load by priority**: drop old items, or low-priority items. Needs a priority queue and a
  policy; rarely worth it below very high scale.

Whatever you choose, expose `pending()` as a metric. Queue depth over time is the single best
health signal a pooled server has: rising depth means you are falling behind before latency
alarms fire.

### 2.7 Sizing and variations

- Thread count from module 01: cores for CPU-bound, more for I/O-bound, from config.
- One global queue is simplest and balances load perfectly. Per-thread queues with work stealing
  (each worker pops from its own deque, steals from others when empty) cut contention on the
  queue mutex at high task rates (millions per second) at the cost of complexity. A request server
  doing thousands of requests per second does not need it.
- Task size: a task should be tens of microseconds or more; below that the queue overhead
  (lock, notify, context switch) dominates. Batch tiny tasks.
- Never block a worker waiting on a future from the *same* pool: with all workers blocked waiting
  for tasks that are stuck behind them in the queue, the pool deadlocks. Rule: a task may submit
  to its own pool but must not wait on the result. Use continuations, a separate pool, or run the
  dependent work inline.

## 3. Code

Files: `code/m09_thread_pool.hpp` (the pool), `code/m09_thread_pool_demo.cpp` (results,
exceptions, backpressure, shutdown drain count), `code/m09_async_pitfall.cpp` (the blocking
destructor and the deferred policy).

The header's public interface:

```cpp
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads, std::size_t max_queue = 0);   // 0 = unbounded
    ~ThreadPool();                                   // calls shutdown(): drains, then joins

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    void shutdown();        // graceful: refuse new work, run everything queued, join
    void shutdown_now();    // abandon queued work (futures get broken_promise), join
    std::size_t pending() const;
    long completed() const;
    std::size_t size() const { return workers_.size(); }
private:
    void worker_loop(std::size_t index);
    ...
};
```

Demo excerpt:

```cpp
ThreadPool pool(4, /*max_queue=*/16);

// 1. Results come back typed.
std::vector<std::future<long>> futs;
for (long i = 0; i < 100; ++i) futs.push_back(pool.submit([i] { return i * i; }));
long sum = 0; for (auto& f : futs) sum += f.get();           // 328350

// 2. Exceptions cross the thread boundary; the worker survives.
auto bad = pool.submit([] () -> int { throw std::runtime_error("bad request"); });
try { bad.get(); } catch (const std::runtime_error& e) { std::cout << "caught: " << e.what() << '\n'; }
auto good = pool.submit([] { return 7; });                   // same workers, still alive
assert(good.get() == 7);

// 3. Backpressure: with max_queue 16 and 4 slow workers, the 21st submit blocks.
// 4. Shutdown drains: submit 1000 quick tasks then destroy the pool; completed() == 1000 + earlier.
```

## 4. Under the hood (senior-level)

### What a `std::future` is

`std::future<T>` holds a `shared_ptr` to a heap-allocated **shared state**: a storage slot for
`T` or an `exception_ptr`, a ready flag, and a wait mechanism (libstdc++ uses an atomic plus a
futex via `__atomic_futex_unsigned`; before that, a mutex and condition variable). `set_value`
constructs the result, sets the flag with release semantics, and futex-wakes waiters. `get()`
acquire-loads the flag, futex-waits if not ready, then moves the result out. Allocation: one per
promise/future pair. That is cheap compared with a request but not free, which is why
latency-critical pools sometimes use a custom task type without futures.

### `packaged_task` and type erasure

`std::packaged_task<R()>` stores the callable in a type-erased buffer (small-buffer optimised,
heap otherwise) plus a shared state. Our pool then wraps *that* in a `std::function<void()>`,
which is a second type-erasure layer and possibly a second allocation. A production pool with a
hot path typically defines its own `UniqueFunction` (move-only `std::function`) to remove one
layer. C++23 adds `std::move_only_function` for this reason.

### Why the task runs outside the lock

`task()` is called after `lk` is released. If it were called under `m_`, the pool would be
single-threaded (one task at a time) and any task that submits to the pool would self-deadlock.
This is the module 03 rule, "never hold a lock across a call-out", in its purest form.

### Wakeup cost and batching

Each `submit` does lock, push, unlock, `notify_one`. If a worker is sleeping in `futex_wait`,
`notify_one` is a `FUTEX_WAKE` syscall (about 1 µs) plus the woken thread's context switch and
cache warm-up (several µs). At 100,000 submits per second that is a measurable fraction of a core.
Optimisation when it matters: keep a count of idle workers and skip `notify_one` when zero are
idle (they will find the task on their next loop), or submit in batches with one notify.

### Thread exit during shutdown

`join()` on each worker in sequence is fine because every worker has already been told to exit;
joining them serially just waits for the slowest. A worker still executing a 30-second task delays
shutdown by 30 seconds. If that is unacceptable, tasks must be cancellable: check a
`std::atomic<bool>` stop token periodically (C++20 formalises this as `std::stop_token`). You
cannot kill a thread safely from outside; `TerminateThread` and `pthread_cancel` leave locks held
and heap corrupted. Never use them in a server.

## 5. In production

- **Your pooled request model, described portably.** "Fixed pool of N worker threads created at
  startup. Acceptor thread reads a request from the socket and pushes it onto a bounded queue
  protected by a mutex; workers block on a condition variable. Each request carries a completion
  handle so the acceptor can reply. Shutdown closes the queue, drains in-flight work, then joins."
  This sentence is what the interviewer wants, and every element maps to a line in
  `m09_thread_pool.hpp`.
- **The blocking `std::async` destructor.** `std::async(std::launch::async, send_audit_log, rec);`
  in a request handler, result discarded. It looks asynchronous. It is a synchronous call with the
  overhead of a thread creation. Found in code review by anyone who has read this module; found
  in production by a latency graph.
- **Worker self-deadlock.** A task calls `pool.submit(sub_task).get()`. Under light load it works
  (a free worker runs `sub_task`). Under full load all N workers are inside tasks that are each
  waiting for a `sub_task` that is queued behind them. The server hangs with an empty CPU and a
  full queue. Rule: never `get()` inside a task on the same pool.
- **Lost tasks at shutdown.** The C version checked a `g_shutdown` flag at the top of the worker
  loop and returned. Requests that arrived in the last 50 ms before shutdown were in the queue and
  never processed; the client got no response and retried against the new instance, double
  posting a loan disbursement. This is why the drain order matters.
- **Exceptions killing workers.** A `std::function<void()>` task that throws in a pool without
  `packaged_task` or a `catch (...)` calls `std::terminate`. One malformed request takes down the
  server. Always wrap.
- **3 AM symptom:** queue depth climbing, CPU low, all workers in `recv` or a database call.
  The pool is sized for compute and the work is I/O. Raise the thread count or (module 11) move
  the I/O waiting onto an event loop.

## 6. Try it

1. **(easy)** Build `m09_thread_pool_demo.cpp` with the header and run it. Confirm the sum, the
   caught exception, and that `completed()` equals the number submitted after the pool is
   destroyed. Done when all three match.
2. **(easy)** Build and run `m09_async_pitfall.cpp`. Measure the elapsed time around the
   discarded-future `std::async` call. Done when you have shown it is sequential.
3. **(medium)** Break the shutdown: change the worker's exit test to `if (closed_) return;` before
   taking a task. Submit 1,000 tasks that each sleep 1 ms, then destroy the pool immediately.
   Print `completed()`. Done when you have observed lost tasks and restored the correct order.
4. **(medium)** Add `bool try_submit(F&&)` that returns `false` instead of blocking when the queue
   is full, and a `pending()` sampler thread that prints queue depth every 100 ms during a burst
   of 10,000 submits. Done when the depth graph (just numbers) shows the bound being hit and the
   rejection count is non-zero.
5. **(hard)** Reproduce the self-deadlock: 4 workers, each task submits a sub-task and calls
   `get()` on it; submit 8 outer tasks. Observe the hang, dump stacks with gdb, and explain why
   every worker is in `futex_wait` inside `future::get`. Then fix it two ways: (a) run the
   sub-task inline if the caller is a pool worker (`thread_local bool in_pool`), (b) use a second
   pool. Done when both fixes complete and you can state the rule.
6. **(hard)** Implement a cooperative cancellation: `ThreadPool` holds a `std::atomic<bool>
   stop_requested_`; tasks receive a `const std::atomic<bool>&` and check it. `shutdown_now()`
   sets it before swapping out the queue. A long-running task (loop of 100 x 10 ms sleeps) must
   exit within 20 ms of `shutdown_now()`. Done when the measured shutdown time is under 50 ms.

## 7. Interview questions

**Q: Design a thread pool. What are the components?**
A fixed vector of worker threads, a queue of type-erased tasks guarded by a mutex, a condition
variable workers wait on with the predicate "closed or queue not empty", and a closed flag for
shutdown. `submit` wraps the callable in a `packaged_task`, pushes a copyable lambda holding it,
notifies one worker, and returns the future. Workers pop under the lock, run the task outside the
lock, and loop. Optionally a bound on the queue with a second condition variable for backpressure.

**Q: How do you shut a thread pool down without losing tasks or hanging?**
Set `closed` under the queue mutex, `notify_all`, join every worker. Workers exit only when the
queue is empty *and* closed, checked under the mutex, so every task pushed before close is popped
and run. The notify under the mutex prevents a lost wakeup. The join waits for in-flight tasks.
Checking `closed` before draining is the classic bug; it abandons queued work.

**Q: How do exceptions cross from a worker to the submitter?**
`std::packaged_task` catches anything the callable throws and stores an `exception_ptr` in the
shared state. The submitter's `future.get()` rethrows it on the calling thread with the original
type. The worker thread never sees it. Without this wrapper an exception escapes the thread entry
function and `std::terminate` kills the process.

**Q: What is wrong with `std::async` in a server?**
The future's destructor blocks for `launch::async`, so a discarded result makes the call
synchronous. Each call creates a new thread with no bound or queue. The default policy may be
`deferred`, which never runs until `get()`. No cancellation. It is for one-off background work,
not for request handling.

**Q: What is backpressure and how do you implement it in a pool?**
Limiting queued work so a burst does not become unbounded memory and latency. Bound the deque;
`submit` either blocks on a "not full" condition variable (self-regulating, pushes back to TCP) or
returns failure so the caller can reply "busy" immediately. Expose queue depth as a metric; it is
the earliest signal of overload.

**Q: Why must a task never wait on a future from the same pool?**
If all workers are inside tasks that are blocked waiting for sub-tasks queued behind them, no
worker is free to run those sub-tasks and the pool deadlocks with an idle CPU. Run dependent work
inline, use continuations, or a separate pool.

**Q: Why does the worker release the lock before running the task?**
Holding the lock would serialise all tasks and make any task that submits to the pool
self-deadlock. The lock protects only the queue; the task is a call-out and must run unlocked.

## 8. Recap

- A pool: fixed threads, mutex-guarded task deque, condition variable, closed flag.
- `packaged_task` + `future`: typed results and exceptions cross threads; workers survive throws.
- `submit` wraps in `shared_ptr<packaged_task>` because `std::function` needs copyable.
- Shutdown: close under the lock, `notify_all`, workers drain until empty-and-closed, join.
  Check empty before closed.
- `shutdown_now` swaps the queue out; abandoned futures get `broken_promise`.
- Bound the queue; block or reject; publish depth.
- `std::async`: blocking destructor, no pool, unspecified default policy. Not for servers.
- Never `get()` on the same pool inside a task; never hold the queue lock while running a task.
- Cancellation is cooperative; never terminate a thread from outside.
- Your Win32 pooled request model is this design with Events for futures and a CS for the queue.
