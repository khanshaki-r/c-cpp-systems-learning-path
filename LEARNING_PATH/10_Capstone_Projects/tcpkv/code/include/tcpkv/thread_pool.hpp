#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tcpkv {

// std::function requires its callable to be copyable, which rules out lambdas that capture a
// move-only Connection. This is the smallest type-erased wrapper that only requires movability
// (C++23 has std::move_only_function; we are on C++17).
class MoveOnlyFunction {
public:
    MoveOnlyFunction() noexcept = default;

    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
                                                std::is_invocable_r_v<void, std::decay_t<F>&>>>
    MoveOnlyFunction(F&& f)  // NOLINT: implicit by design, like std::function
        : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    void operator()() { impl_->call(); }
    explicit operator bool() const noexcept { return impl_ != nullptr; }

private:
    struct Base {
        virtual ~Base() = default;
        virtual void call() = 0;
    };
    template <class F>
    struct Impl final : Base {
        template <class G>
        explicit Impl(G&& g) : f(std::forward<G>(g)) {}
        void call() override { f(); }
        F f;
    };
    std::unique_ptr<Base> impl_;
};

// Fixed-size pool of worker threads consuming one FIFO queue.
//
// Lifecycle: submit() while running; stop() rejects new tasks, drops queued ones and lets
// workers finish the task they are executing; the destructor calls stop() and joins.
// A task that throws is logged and the worker continues (an exception must never kill a worker).
class ThreadPool {
public:
    using Task = MoveOnlyFunction;

    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Returns false if the pool has been stopped (the task is destroyed, not run).
    bool submit(Task task);

    // Blocks until the queue is empty and no task is running (or the pool is stopped).
    void drain();

    // Idempotent. Queued tasks are destroyed; running tasks complete; workers then exit.
    void stop() noexcept;

    std::size_t size() const noexcept { return workers_.size(); }
    std::size_t pending() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::deque<Task> queue_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;  // workers wait here for tasks
    std::condition_variable idle_cv_;  // drain() waits here
    std::size_t active_ = 0;           // tasks currently executing
    bool stopped_ = false;
};

}  // namespace tcpkv
