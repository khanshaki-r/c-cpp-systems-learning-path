// m05_producer_consumer.cpp — bounded blocking queue with two condition variables and a
// close-and-drain shutdown protocol. This is the core of the module 09 thread pool.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m05_producer_consumer.cpp -o m05
#include <condition_variable>
#include <mutex>
#include <deque>
#include <optional>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <utility>

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

    std::size_t size() {
        std::lock_guard<std::mutex> lk(m_);
        return q_.size();
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
    std::cout << "left in queue after drain: " << q.size() << " (expected 0)\n";

    // Timed pop on an empty, open queue
    BoundedQueue<int> empty(1);
    auto t0 = std::chrono::steady_clock::now();
    auto r = empty.pop_for(std::chrono::milliseconds(50));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "timed pop: " << (r ? "got value" : "timeout") << " after ~" << ms << " ms\n";

    // Push after close fails cleanly
    empty.close();
    std::cout << "push after close accepted: " << (empty.push(1) ? "yes" : "no") << " (expected no)\n";
    return 0;
}
