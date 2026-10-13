// m07_monitor_producer_consumer.cpp
// MONITOR OBJECT: an object whose every public method takes the object's own
// mutex, so callers never lock explicitly and the invariant is protected at the
// class boundary. Condition variables inside let methods WAIT for a state
// ("not empty", "not full") without busy looping.
//
// The canonical monitor is a bounded blocking queue, which is also the
// PRODUCER-CONSUMER pattern: producers push, consumers pop, the bound gives
// back-pressure (producers block when consumers fall behind).
//
// C equivalent: pthread_mutex_t + pthread_cond_t in a struct, with every
// function doing lock/unlock by hand. Same thing; the C++ version makes the
// unlock impossible to forget (lock_guard) and the predicate re-check explicit.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m07b m07_monitor_producer_consumer.cpp

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : cap_(capacity) {}

    // Blocks while full. Returns false if the queue was closed.
    bool push(T item) {
        std::unique_lock<std::mutex> lk(mu_);
        not_full_.wait(lk, [this] { return closed_ || q_.size() < cap_; });   // predicate form: no lost wakeups
        if (closed_) return false;
        q_.push_back(std::move(item));
        lk.unlock();              // release BEFORE notify: the woken consumer can take the lock immediately
        not_empty_.notify_one();
        return true;
    }

    // Blocks while empty. Returns nullopt when closed AND drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(mu_);
        not_empty_.wait(lk, [this] { return closed_ || !q_.empty(); });
        if (q_.empty()) return std::nullopt;   // closed and empty
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    // Bounded wait: for shutdown paths and health checks.
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!not_empty_.wait_for(lk, timeout, [this] { return closed_ || !q_.empty(); })) return std::nullopt;
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    // Shutdown: wake everyone. Producers get false; consumers drain then get nullopt.
    void close() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

private:
    mutable std::mutex mu_;               // mutable: size() is logically const
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> q_;
    std::size_t cap_;
    bool closed_ = false;
};

struct Job {
    int producer;
    int seq;
};

int main() {
    BoundedQueue<Job> queue(8);   // small bound so back-pressure actually happens
    constexpr int kProducers = 3, kConsumers = 2, kPerProducer = 50;

    std::mutex print_mu;
    std::vector<int> consumed_per_consumer(kConsumers, 0);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, p] {
            for (int i = 0; i < kPerProducer; ++i) queue.push(Job{p, i});   // blocks when 8 are waiting
        });
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&queue, &consumed_per_consumer, &print_mu, c] {
            while (auto job = queue.pop()) {          // loop ends when closed and drained
                std::this_thread::sleep_for(std::chrono::microseconds(50));   // "work"
                ++consumed_per_consumer[static_cast<std::size_t>(c)];
                if (job->seq == 0) {
                    std::lock_guard<std::mutex> lk(print_mu);
                    std::printf("consumer %d got first job of producer %d\n", c, job->producer);
                }
            }
        });
    }

    for (auto& t : producers) t.join();   // all items pushed (consumers may still be working)
    queue.close();                         // no more producers: let consumers drain and exit
    for (auto& t : consumers) t.join();

    int total = 0;
    for (int n : consumed_per_consumer) total += n;
    std::printf("consumed %d jobs (expected %d), split:", total, kProducers * kPerProducer);
    for (int n : consumed_per_consumer) std::printf(" %d", n);
    std::printf("\nqueue size at end = %zu\n", queue.size());
    return 0;
}
