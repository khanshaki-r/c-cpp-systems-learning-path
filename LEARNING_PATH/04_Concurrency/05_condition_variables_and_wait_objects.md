# 05. Condition variables and wait objects

**Time:** ~3 hours   **Prerequisites:** 03

## 1. In plain words

A mutex lets you protect data. It does not let you *wait for the data to change*. A worker thread
that wants the next request from a queue must either check the queue in a loop (burning CPU) or
sleep until someone tells it there is work. A condition variable is the "tell it" mechanism: a
thread goes to sleep on the condition variable, releasing the mutex while it sleeps; another
thread changes the data and signals the condition variable; the sleeper wakes up, re-takes the
mutex, and checks the data.

Analogy: a restaurant pass. Cooks (producers) put plates on the pass and ring a bell. Waiters
(consumers) wait near the bell. A waiter who hears the bell walks over, but *checks that there is
actually a plate* before picking one up, because another waiter may have been faster, or the bell
may have been rung by mistake. That check-after-wake is the predicate loop, and it is the single
most important rule in this module.

You have done this on Win32 with an Event handle and `WaitForSingleObject`. Events and condition
variables solve the same problem differently, and the differences explain a class of bugs (lost
wakeups) that your Win32 code may have hit without anyone knowing why.

## 2. The real thing

### 2.1 The API

```cpp
#include <condition_variable>

std::mutex m;
std::condition_variable cv;
std::deque<Request> queue;   // GUARDED_BY(m)
bool shutting_down = false;  // GUARDED_BY(m)

// consumer
std::optional<Request> pop() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return !queue.empty() || shutting_down; });   // predicate loop
    if (queue.empty()) return std::nullopt;                           // shutting down, drained
    Request r = std::move(queue.front());
    queue.pop_front();
    return r;
}

// producer
void push(Request r) {
    {
        std::lock_guard<std::mutex> lk(m);
        queue.push_back(std::move(r));
    }                      // unlock before notify: the woken thread can take the lock immediately
    cv.notify_one();
}
```

Members of `std::condition_variable`:

- `wait(unique_lock&)`: atomically release the mutex and block; on wake, re-acquire the mutex
  before returning. The mutex **must** be held when calling.
- `wait(unique_lock&, Predicate p)`: equivalent to `while (!p()) wait(lk);`. Always use this
  form.
- `wait_for(lk, duration[, pred])`, `wait_until(lk, time_point[, pred])`: timed variants. Return
  `std::cv_status::timeout` or `no_timeout`, or with a predicate, the predicate's value.
- `notify_one()`: wake at least one waiter (if any). `notify_all()`: wake all waiters.

`std::condition_variable` works only with `std::unique_lock<std::mutex>`. If you need a different
lock type, `std::condition_variable_any` works with anything that has `lock()`/`unlock()` at some
cost in size and speed.

### 2.2 Why the predicate loop: three derivations

You must call `wait` in a loop that re-checks the condition. Here is why, three independent times.
Any one of them is sufficient.

**Reason 1: spurious wakeups.** The standard permits `wait` to return without any `notify`. This
is not laziness; it is because the underlying futex wait can be interrupted (a signal delivered to
the thread, `EINTR`) and because efficient implementations on some platforms cannot distinguish
"woken by notify" from "woken by the kernel for its own reasons" without an extra round trip. The
POSIX standard permits the same for `pthread_cond_wait`. Windows `SleepConditionVariableCS` also
documents spurious wakeups. So a thread may wake with the queue still empty.

**Reason 2: stolen wakeups.** Producer pushes one item and calls `notify_one`. Waiter A is woken.
Before A re-acquires the mutex, thread C (which was never waiting) calls `pop`, takes the mutex,
finds the item, and takes it. A now acquires the mutex and finds the queue empty. Without the
loop, A pops from an empty queue.

**Reason 3: notify_all with a condition that is true for one.** Producer pushes one item and calls
`notify_all` (perhaps it does not know how many it pushed). Five waiters wake. One gets the item.
Four must go back to sleep. Without the loop, four threads pop from an empty queue.

The predicate form `cv.wait(lk, pred)` encodes the loop so you cannot forget it. The condition
must be checked under the mutex, which is why `wait` re-acquires before returning and why the
predicate runs with the lock held.

### 2.3 Lost wakeups

The opposite failure: a notify that nobody hears. This happens when the waiter checks the
condition, finds it false, and *then* goes to sleep, with a gap in between during which the
producer sets the condition and notifies. The notify finds no waiters and is dropped (condition
variables have no memory). The waiter then sleeps forever.

```
consumer                              producer
lock m
check queue: empty
unlock m             <- gap
                                      lock m; push; unlock m; notify_one()  -- nobody waiting
wait on cv           <- sleeps forever
```

The condition variable API prevents this by making "release the mutex and start waiting" one
atomic step inside `wait`. Because the consumer holds the mutex from the check until the moment
the wait registers it, the producer cannot push in the gap: it is blocked on the mutex. When it
does push and notify, the consumer is already registered as a waiter.

This is the rule: **the condition must be modified under the same mutex that the waiter holds
when checking it**. If a producer sets `shutting_down = true` without the lock and then notifies,
a lost wakeup is possible. Even if the variable is `std::atomic<bool>`, you still need to take the
mutex (and release it) around the write to close the gap. "Atomic flag plus condition variable"
is a classic bug.

### 2.4 notify_one vs notify_all

- `notify_one` when exactly one waiter can make progress from this change (one item pushed, one
  consumer should take it). Cheaper: wakes one thread.
- `notify_all` when the change may let several waiters proceed (shutdown flag, "queue is no
  longer full" with several producers blocked, a state transition that different waiters care
  about for different reasons). Also the safe default when waiters have *different* predicates on
  the same condition variable: `notify_one` might wake a waiter whose predicate is still false
  while the one whose predicate is now true stays asleep. That waiter re-checks, sleeps again,
  and the intended waiter never wakes: a lost wakeup in disguise.

Rule: one condition variable per *condition*. A bounded queue has "not empty" (consumers wait) and
"not full" (producers wait); give each its own condition variable so `notify_one` is always
correct.

### 2.5 Notify with or without the lock held

Both are correct. Notifying after unlocking lets the woken thread acquire the mutex immediately
instead of waking, failing to acquire, and sleeping again (a wasted context switch). glibc has an
optimisation (wait morphing was removed; current glibc simply requeues) that makes the difference
small. Notify-after-unlock is the convention; notify-while-locked is required only when the
condition variable itself may be destroyed by the woken thread (rare: a one-shot "done" signal
where the waiter deletes the object).

### 2.6 Mapping to Win32

| Concept | Win32 | C++17 |
|---|---|---|
| condition variable | `CONDITION_VARIABLE` (Vista+) with `SleepConditionVariableCS` / `SleepConditionVariableSRW`, `WakeConditionVariable`, `WakeAllConditionVariable` | `std::condition_variable` |
| manual-reset event | `CreateEvent(NULL, TRUE, FALSE, NULL)`; `SetEvent` leaves it set until `ResetEvent` | no direct equivalent; `mutex + bool + condition_variable` or `std::future<void>` for one-shot |
| auto-reset event | `CreateEvent(NULL, FALSE, ...)`; `SetEvent` wakes exactly one waiter and resets | roughly a binary semaphore: `mutex + int count + condition_variable` |
| wait for any of N | `WaitForMultipleObjects(n, handles, FALSE, timeout)` | none; one condition variable with a composite predicate, or a single queue of events |
| wait for all of N | `WaitForMultipleObjects(n, handles, TRUE, ...)` | none; a counter of completed tasks under a mutex plus a condition variable, or N futures joined in sequence |

The crucial difference: **an Event has state (set or not set); a condition variable does not.**
`SetEvent` on a manual-reset event with no waiter is remembered; a later `WaitForSingleObject`
returns immediately. `notify_one` with no waiter is forgotten. This is why Event-based code does
not need the "modify under the mutex" rule for simple one-shot signals, and why condition variable
code does. But it is also why Event-based code has its own classic bug: an auto-reset Event that is
set twice before the waiter runs wakes it only once; the second signal is lost. A condition
variable plus a counter does not have this problem because the state lives in the counter you
control.

Since Vista, Windows has `CONDITION_VARIABLE`, which is the same thing as `std::condition_variable`
(same spurious wakeups, same predicate-loop requirement, pairs with `CRITICAL_SECTION` or
`SRWLOCK`). If your server still uses Events for producer-consumer, `CONDITION_VARIABLE` with
`SRWLOCK` is the modern Win32 replacement, and module 13 shows it.

`WaitForMultipleObjects` has no standard C++ equivalent. The portable designs are: (a) one queue
that all event sources push into, with one condition variable (what an actor's mailbox is, module
11); (b) a `select`/`epoll`/`poll` loop for I/O events (08_Networking); (c) `std::future` and
`when_any` from the Concurrency TS, not standard in C++17.

### 2.7 Timed waits and the deadline pattern

Use `wait_until` with an absolute deadline, not `wait_for` with a relative duration, when looping:
a `wait_for(100ms)` inside a predicate loop that wakes spuriously at 99 ms waits another 100 ms,
so the total timeout stretches. `wait_until(deadline, pred)` handles this correctly and returns
`false` if the deadline passed with the predicate still false.

## 3. Code

File: `code/m05_producer_consumer.cpp`

```cpp
#include <condition_variable>
#include <mutex>
#include <deque>
#include <optional>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <atomic>

// Bounded blocking queue. Two condition variables: one per condition.
template <class T>
class BoundedQueue {
    std::mutex m_;
    std::condition_variable not_empty_;   // consumers wait here
    std::condition_variable not_full_;    // producers wait here
    std::deque<T> q_;                     // GUARDED_BY(m_)
    const std::size_t cap_;
    bool closed_ = false;                 // GUARDED_BY(m_)
public:
    explicit BoundedQueue(std::size_t cap) : cap_(cap) {}

    // Blocks while full. Returns false if the queue was closed (item not enqueued).
    bool push(T item) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });   // WHY loop: spurious/stolen
        if (closed_) return false;
        q_.push_back(std::move(item));
        lk.unlock();                 // WHY unlock first: the woken consumer can lock without a bounce
        not_empty_.notify_one();     // WHY one: exactly one consumer can proceed per item
        return true;
    }

    // Blocks while empty. Returns nullopt only when closed AND drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;        // closed and nothing left
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    // Timed pop: returns nullopt on timeout or on closed-and-drained.
    template <class Rep, class Period>
    std::optional<T> pop_for(std::chrono::duration<Rep, Period> d) {
        std::unique_lock<std::mutex> lk(m_);
        auto deadline = std::chrono::steady_clock::now() + d;   // WHY absolute: loop does not stretch
        if (!not_empty_.wait_until(lk, deadline, [&] { return !q_.empty() || closed_; })) return std::nullopt;
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    // Close: no more pushes accepted; consumers drain what remains and then get nullopt.
    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);   // WHY under the lock: closes the lost-wakeup gap
            closed_ = true;
        }
        not_empty_.notify_all();   // WHY all: every blocked consumer must re-check and exit
        not_full_.notify_all();    // every blocked producer must learn it will fail
    }
};

int main() {
    BoundedQueue<int> q(8);
    std::atomic<long> consumed_sum{0};
    std::atomic<int> consumed_count{0};

    const int producers = 3, consumers = 4, per_producer = 10000;

    std::vector<std::thread> cs;
    for (int c = 0; c < consumers; ++c)
        cs.emplace_back([&] {
            while (auto v = q.pop()) {               // loop ends on nullopt: closed and drained
                consumed_sum.fetch_add(*v, std::memory_order_relaxed);
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::vector<std::thread> ps;
    for (int p = 0; p < producers; ++p)
        ps.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) q.push(p * per_producer + i);
        });

    for (auto& t : ps) t.join();   // all items are in the queue or already consumed
    q.close();                     // consumers drain and exit
    for (auto& t : cs) t.join();

    long n = static_cast<long>(producers) * per_producer;
    long expected = n * (n - 1) / 2;   // sum of 0..n-1
    std::cout << "consumed " << consumed_count.load() << " items, sum " << consumed_sum.load()
              << " (expected " << n << ", " << expected << ")\n";

    // Timed pop on an empty, open queue
    BoundedQueue<int> empty(1);
    auto t0 = std::chrono::steady_clock::now();
    auto r = empty.pop_for(std::chrono::milliseconds(50));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "timed pop: " << (r ? "got value" : "timeout") << " after ~" << ms << " ms\n";
    return 0;
}
```

The shutdown protocol here is the one you will reuse in the thread pool (module 09): producers
finish, `close()` flips a flag under the mutex and wakes everyone, consumers drain until the queue
is empty *and* closed. No task is lost because `pop` returns `nullopt` only when both are true.

## 4. Under the hood (senior-level)

### `wait` as an atomic release-and-sleep

glibc's `pthread_cond_wait`:

1. Read the condition variable's sequence counter (`__wseq`), which counts signals issued.
2. Unlock the mutex.
3. `futex(&cond->__g_signals, FUTEX_WAIT, expected_value)`. The kernel compares the word against
   `expected_value` atomically with enqueuing the thread. If a `notify` happened between step 1
   and step 3, the word has changed, the futex call returns immediately, and no wakeup is lost.
   This is the mechanism that makes "check then sleep" atomic from the producer's point of view.
4. On return (notified, spurious, or `EINTR`), re-lock the mutex with the normal mutex path
   (which may itself block if the notifier still holds it).
5. Return to the caller, who re-checks the predicate.

glibc's implementation since 2.25 uses two "groups" of waiters to guarantee that a signal wakes a
thread that was waiting *before* the signal, not one that arrived after (this was a correctness
fix; the previous design could effectively steal a signal). The details are intricate; what you
need is: `notify` increments a counter and does `FUTEX_WAKE`; `wait` sleeps on a futex only if
the counter has not changed since it checked.

### Why `wait` needs `unique_lock` and not `lock_guard`

`wait` must call `unlock()` and `lock()` on the lock object. `lock_guard` has neither member. This
is a compile-time enforcement that the mutex is the kind that can be released mid-scope.

### Spurious wakeups are not a bug in your platform

On Linux, `futex` returns `EINTR` when a signal is delivered; glibc returns from `wait` rather
than restarting the futex, because restarting would require re-reading state under the lock
anyway. Requiring the caller to loop is cheaper than requiring the library to guarantee no false
returns. Windows `SleepConditionVariableCS` is documented the same way. Any design that requires
"exactly one wake per notify" is fighting the primitive and will lose.

### Thundering herd

`notify_all` on a condition variable with 64 waiters wakes 64 threads that then all try to take
one mutex. 63 of them block again on the mutex. That is 64 context switches to deliver one item.
glibc used to "morph" the condvar wait into a mutex wait inside the kernel (`FUTEX_CMP_REQUEUE`)
to avoid this; current versions still use requeue in `pthread_cond_broadcast`. Even so, prefer
`notify_one` when one waiter suffices, and prefer per-condition condition variables.

### Condition variable vs semaphore

A counting semaphore (C++20 `std::counting_semaphore`, Win32 `CreateSemaphore`, POSIX `sem_t`)
has state: a count. `release` increments; `acquire` blocks until the count is positive then
decrements. It does not need a mutex or a predicate loop for the simple "N items available" case,
and a release with no waiter is remembered. In C++17 you build one from mutex, condition variable
and an int (exercise 3). A condition variable is the more general primitive because the predicate
can be anything.

## 5. In production

- **The pooled request model.** Your server's "worker waits for request" is exactly `pop()`
  above. If it is built on an auto-reset Event, it has the two-signals-one-wake hazard: two
  requests enqueued in quick succession, one `SetEvent` coalesced, one worker woken, one request
  sits in the queue until the next signal. It is invisible under load (the next request arrives
  within milliseconds) and a mystery under light load (the last request of the day waits until
  morning). The fix on Win32 is a semaphore (count = items) or `CONDITION_VARIABLE` with a
  predicate loop; the fix in C++ is the queue above.
- **Shutdown that hangs.** Workers blocked in `wait` and `main` joined them without ever
  notifying. Rule: every `wait` must have a shutdown predicate (`|| closed_`) and shutdown must
  set the flag under the mutex and `notify_all`.
- **Shutdown that loses work.** `closed_` checked *before* the queue in `pop`:
  `if (closed_) return nullopt;` ahead of `if (!q_.empty())`. Items pushed just before close are
  abandoned. The order in the code above (queue first, then closed) is deliberate. Interviewers
  ask about this; module 09 makes it the central design point of the thread pool.
- **Missing the lock on the flag.** `std::atomic<bool> stop; ... stop = true; cv.notify_all();`
  with a waiter checking `stop` in its predicate. The waiter reads `stop == false`, the setter
  sets it and notifies, the waiter then calls the internal wait: lost wakeup. Taking and releasing
  the mutex around `stop = true` (even for an atomic) is required.
- **3 AM symptom:** one worker's stack shows `pthread_cond_wait` while the queue has items. Either
  someone pushed without notifying (missing `notify_one` on a rarely used code path) or the
  predicate is wrong. Dump the queue size and the waiter count; if size > 0 and waiters > 0, the
  notify is missing.

## 6. Try it

1. **(easy)** Build and run `m05_producer_consumer.cpp`. Replace `not_empty_.wait(lk, pred)` with
   the non-predicate `not_empty_.wait(lk)` and `if (q_.empty()) return nullopt;` after it.
   Run with 4 consumers; observe lost items or early exits. Done when you can name which of the
   three derivations caused the failure you observed.
2. **(easy)** Move `closed_ = true` outside the `lock_guard` in `close()`. Run 50 times with a
   tiny queue and many consumers. It may or may not hang (lost wakeup). Add a 1 ms sleep between
   the predicate check and the futex wait if you want to force it (you cannot easily from
   outside; instead reason it through and write the interleaving). Done when the interleaving is
   written down with line numbers.
3. **(medium)** Implement `class Semaphore { void acquire(); void release(); }` with mutex,
   condition variable and an int. Then implement a Win32-style auto-reset event on top of it
   (release only if count is 0, so repeated signals coalesce) and a manual-reset event (a bool
   plus `notify_all`). Done when a test shows the auto-reset version losing the second of two
   back-to-back signals while the semaphore does not.
4. **(medium)** Add a `size()` and a `waiting_consumers` counter to `BoundedQueue` and a
   `dump()` that prints both. Deliberately remove the `notify_one` from `push` and show that
   `dump()` after the producers finish prints `size > 0, waiters > 0`. Done when you have the
   diagnostic output.
5. **(hard)** Implement a `WaitForMultipleObjects`-style "wait for any" in C++17: a class
   `EventSet` where N sources each call `signal(i)` and one waiter calls `int wait_any()` which
   returns the index of a signalled source and clears it. One mutex, one condition variable, a
   `std::bitset`. Then explain why this design does not need `notify_all` and under what change it
   would. Done when a test with 8 sources and 1 waiter receives every signal exactly once.

## 7. Interview questions

**Q: Why must `condition_variable::wait` be called in a loop that re-checks the condition?**
Three reasons: the standard permits spurious wakeups (the futex can return on a signal), a
non-waiting thread can take the mutex and consume the state between the notify and the waiter's
re-acquire (stolen wakeup), and `notify_all` wakes threads whose condition may be true for only one
of them. The predicate overload encodes the loop.

**Q: What is a lost wakeup and how does the condition variable API prevent it?**
A notify issued while no thread is waiting is dropped; if a waiter checked the condition just
before and sleeps just after, it sleeps forever. The API prevents it by making "release the mutex
and register as a waiter" atomic inside `wait`, so the producer cannot modify the condition in the
gap as long as it modifies the condition under the same mutex.

**Q: Why does the shared flag need to be modified under the mutex even if it is `std::atomic`?**
Because atomicity of the flag is not the problem; the problem is the window between the waiter's
predicate check and its entry into the kernel wait. Holding the mutex while setting the flag means
the setter cannot run inside that window. Without it the notify can fire before the waiter is
registered.

**Q: When do you use `notify_all` instead of `notify_one`?**
When more than one waiter may be able to proceed (shutdown, a batch insert, "not full" with
several producers), or when waiters on the same condition variable have different predicates so a
single wakeup might go to the wrong one. Prefer one condition variable per condition so
`notify_one` is usually correct.

**Q: How does a Win32 Event differ from a condition variable?**
An Event has state: a set that nobody is waiting for is remembered, and a manual-reset Event stays
set. A condition variable has no state; a notify with no waiter is lost, so the state must live in
your own variables under a mutex. An auto-reset Event coalesces two sets into one wake, which a
counter-based condition variable design does not.

**Q: What is the C++17 equivalent of `WaitForMultipleObjects`?**
There is none in the standard library. Portable designs route all events into one queue with one
condition variable (an actor mailbox), use a composite predicate over several flags under one
mutex, or use the OS I/O multiplexer (`epoll`, `poll`) for file-descriptor events.

**Q: Describe a correct shutdown for a blocking queue.**
Set a `closed` flag under the queue's mutex, then `notify_all` on every condition variable.
Consumers' predicate is "not empty or closed"; after waking they take an item if one exists and
return "no more" only when the queue is empty and closed. Producers see closed and fail their push.
This drains every item that was enqueued before close.

## 8. Recap

- A condition variable lets a thread sleep until shared state changes; the mutex protects the
  state, the condition variable signals the change.
- `wait` atomically releases the mutex and sleeps; it re-acquires before returning.
- Always wait in a predicate loop: spurious wakeups, stolen wakeups, and `notify_all` all
  require it. Use `cv.wait(lk, pred)`.
- Modify the condition under the mutex, even if it is atomic, or you risk a lost wakeup.
- One condition variable per condition; `notify_one` for one-item changes, `notify_all` for
  shutdown and batch changes.
- Unlock before notify (convention), use `wait_until` with a deadline in loops.
- Events have state and coalesce signals; condition variables have no state. Win32 has
  `CONDITION_VARIABLE` since Vista; use it.
- `WaitForMultipleObjects` has no std equivalent; use a single queue or epoll.
- Shutdown: flag under the lock, `notify_all`, consumers drain then exit.
