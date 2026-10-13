// m07_thread_pool.cpp
// THREAD POOL: N worker threads pulling tasks from one queue. Tasks return
// futures. The pool is BOUNDED in two ways: a fixed number of threads (no
// thread-per-request explosion) and an optional queue limit (back-pressure).
//
// Also shows the two classic mistakes:
//   - submitting a task that blocks waiting on another task in the same pool
//     (deadlock when all workers are blocked) -- described, not executed
//   - capturing a reference to a stack object that dies before the task runs
//
// C equivalent: pthread_create N times, a mutex/cond queue, void* (*fn)(void*).
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m07c m07_thread_pool.cpp

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads, std::size_t max_queue = 0) : max_queue_(max_queue) {
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this, i] { worker_loop(i); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();   // queued tasks finish; new submits are rejected
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // submit(f, args...) -> future<result>. Throws if the queue is full (caller
    // decides: retry, shed load, or reply "busy"). A blocking variant would
    // wait on a not_full condition like BoundedQueue.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        // Bind arguments now, by value: the task owns its inputs and nothing it
        // captures can go out of scope before it runs.
        auto task = std::make_shared<std::packaged_task<R()>>(
            [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(fn), std::move(tup));
            });
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) throw std::runtime_error("pool stopped");
            if (max_queue_ && queue_.size() >= max_queue_) throw std::runtime_error("pool queue full");
            queue_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t pending() {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }
    std::size_t executed() const { return executed_.load(); }

private:
    void worker_loop(std::size_t) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();   // exceptions inside are captured by packaged_task into the future
            executed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::size_t max_queue_;
    bool stop_ = false;
    std::atomic<std::size_t> executed_{0};
    std::vector<std::thread> workers_;   // last: joined first
};

// A CPU-ish task.
static long sum_to(long n) {
    long s = 0;
    for (long i = 1; i <= n; ++i) s += i;
    return s;
}

int main() {
    ThreadPool pool(4, /*max_queue=*/64);

    // 1. Fan out, collect results via futures.
    std::vector<std::future<long>> results;
    for (long n = 1; n <= 10; ++n) results.push_back(pool.submit(sum_to, n * 1000));
    long total = 0;
    for (auto& f : results) total += f.get();
    std::printf("sum of sums = %ld\n", total);

    // 2. Exceptions travel through the future.
    auto bad = pool.submit([] () -> int { throw std::runtime_error("boom"); });
    try {
        bad.get();
    } catch (const std::runtime_error& e) {
        std::printf("task threw: %s (caught on the caller's thread)\n", e.what());
    }

    // 3. Back-pressure: fill the queue past its bound.
    int rejected = 0;
    std::vector<std::future<void>> slow;
    for (int i = 0; i < 200; ++i) {
        try {
            slow.push_back(pool.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }));
        } catch (const std::runtime_error&) {
            ++rejected;
        }
    }
    for (auto& f : slow) f.get();
    std::printf("accepted %zu, rejected %d (queue bound = 64 + 4 running)\n", slow.size(), rejected);

    // 4. The lifetime rule, illustrated the SAFE way: pass data by value.
    {
        std::vector<int> local{1, 2, 3};
        // WRONG would be: pool.submit([&local] { ... }) and then leave this scope before it runs.
        auto f = pool.submit([v = local] { return v.size(); });   // copy captured: safe
        std::printf("captured by value: %zu elements\n", f.get());
    }

    // 5. The deadlock to know about (NOT run): with 4 workers, submitting 4 tasks that each
    //    submit a child task and .get() it blocks all 4 workers waiting for children that
    //    can never be scheduled. Rule: never block a pool thread on work from the same pool.

    std::printf("executed = %zu\n", pool.executed());
    return 0;   // ~ThreadPool joins workers
}
