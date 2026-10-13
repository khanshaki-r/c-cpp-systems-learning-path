#include "tcpkv/thread_pool.hpp"

#include <exception>

#include "tcpkv/log.hpp"

namespace tcpkv {

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        thread_count = 1;
    }
    workers_.reserve(thread_count);
    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        // std::thread's constructor can throw (EAGAIN). If we let the exception escape with
        // threads already running, our destructor would not run and std::terminate would be
        // called from the joinable std::thread destructors. Clean up first, then rethrow.
        stop();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    stop();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool ThreadPool::submit(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    work_cv_.notify_one();
    return true;
}

void ThreadPool::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return stopped_ || (queue_.empty() && active_ == 0); });
}

void ThreadPool::stop() noexcept {
    std::deque<Task> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        dropped.swap(queue_);  // destroy the queued tasks outside the lock, below
    }
    work_cv_.notify_all();
    idle_cv_.notify_all();
    // `dropped` is destroyed here. Each dropped task may own a Connection whose destructor
    // closes a socket; doing that while holding mutex_ would stall every worker.
}

std::size_t ThreadPool::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void ThreadPool::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        try {
            task();
        } catch (const std::exception& e) {
            log::error("task threw: ", e.what());
        } catch (...) {
            log::error("task threw a non-std exception");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (queue_.empty() && active_ == 0) {
                idle_cv_.notify_all();
            }
        }
    }
}

}  // namespace tcpkv
