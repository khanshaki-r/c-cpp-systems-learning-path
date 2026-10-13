// m09_thread_pool.hpp — a fixed-size thread pool: std::thread + deque + mutex + two condition
// variables + packaged_task futures. Graceful shutdown drains every submitted task.
// Header-only. Include from a .cpp and build with -pthread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#endif

class ThreadPool {
public:
    // threads: number of workers (0 is treated as 1). max_queue: 0 = unbounded; otherwise submit()
    // blocks while the queue holds max_queue tasks (backpressure).
    explicit ThreadPool(std::size_t threads, std::size_t max_queue = 0)
        : max_queue_(max_queue) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this, i] { worker_loop(i); });
    }

    ~ThreadPool() { shutdown(); }   // WHY: destroying a pool must never leave threads running

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit any callable with arguments. Returns a future for its result. Exceptions thrown by
    // the callable are captured into the future; the worker thread is unaffected.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
        // WHY shared_ptr: packaged_task is move-only, std::function needs a copyable callable.
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();
        {
            std::unique_lock<std::mutex> lk(m_);
            not_full_.wait(lk, [this] { return closed_ || max_queue_ == 0 || tasks_.size() < max_queue_; });
            if (closed_) throw std::runtime_error("ThreadPool: submit after shutdown");
            tasks_.emplace_back([task] { (*task)(); });
        }
        not_empty_.notify_one();   // WHY after unlock: the woken worker can take the lock at once
        return fut;
    }

    // Graceful: refuse new work, run everything already queued, then join. Idempotent.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m_);   // WHY under the lock: no lost wakeup (module 05)
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
    }

    // Abandon queued work. Outstanding futures for abandoned tasks receive broken_promise.
    // Tasks already running finish. Then join.
    void shutdown_now() {
        std::deque<std::function<void()>> abandoned;
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
            abandoned.swap(tasks_);   // WHY swap: destroy the packaged_tasks outside the lock
        }
        not_empty_.notify_all();
        not_full_.notify_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
        // 'abandoned' is destroyed here; each packaged_task's promise is broken -> future_error.
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lk(m_);
        return tasks_.size();
    }
    long completed() const { return completed_.load(std::memory_order_relaxed); }
    std::size_t size() const { return workers_.size(); }

private:
    void worker_loop(std::size_t index) {
#ifdef __linux__
        std::string name = "pool-" + std::to_string(index);       // visible in gdb / top -H
        pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
        (void)index;
#endif
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                not_empty_.wait(lk, [this] { return closed_ || !tasks_.empty(); });
                if (tasks_.empty()) return;   // WHY this order: exit only when closed AND drained
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            not_full_.notify_one();           // a blocked submitter may now proceed
            try {
                task();                       // WHY outside the lock: tasks may be slow or re-submit
            } catch (...) {
                // packaged_task already routed the exception into the future; this catch only
                // guards against a task type that is not a packaged_task. Never let a throw
                // escape the thread: that would call std::terminate.
            }
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;   // GUARDED_BY(m_)
    mutable std::mutex m_;
    std::condition_variable not_empty_;         // workers wait: closed_ || !tasks_.empty()
    std::condition_variable not_full_;          // submitters wait: closed_ || size < max_queue_
    std::size_t max_queue_;
    bool closed_ = false;                       // GUARDED_BY(m_)
    std::atomic<long> completed_{0};
};
