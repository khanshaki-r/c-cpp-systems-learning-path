// m07_active_object.cpp
// ACTIVE OBJECT: an object that runs its methods on its OWN thread. Callers
// enqueue method calls (as closures) and get a std::future for the result.
// The object's state is touched by exactly one thread, so it needs no mutex of
// its own. The queue is the only synchronised structure.
//
// Compare with MONITOR OBJECT (m07_monitor_producer_consumer.cpp): a monitor
// runs methods on the CALLER's thread under a lock. Active object = one thread
// per object + queue; monitor = any thread + lock.
//
// Scenario: a Ledger that must apply postings strictly in order (banking
// requirement), accessed from many request threads.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m07a m07_active_object.cpp

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class ActiveLedger {
public:
    ActiveLedger() : worker_([this] { run(); }) {}

    ~ActiveLedger() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();   // structured lifetime: no task can run after we are gone
    }
    ActiveLedger(const ActiveLedger&) = delete;
    ActiveLedger& operator=(const ActiveLedger&) = delete;

    // Public API: each method packages a call and returns a future. The caller
    // decides whether to wait (synchronous semantics) or not (fire-and-forget).
    std::future<std::int64_t> post(std::string account, std::int64_t delta) {
        return enqueue([this, account = std::move(account), delta] { return do_post(account, delta); });
    }

    std::future<std::int64_t> balance(std::string account) {
        return enqueue([this, account = std::move(account)] { return do_balance(account); });
    }

    std::future<std::size_t> journal_size() {
        return enqueue([this] { return journal_.size(); });
    }

private:
    // --- the part that runs ONLY on worker_: no locks needed for these members ---
    std::int64_t do_post(const std::string& acct, std::int64_t delta) {
        balances_[acct] += delta;
        journal_.push_back(acct + ":" + std::to_string(delta));
        return balances_[acct];
    }
    std::int64_t do_balance(const std::string& acct) {
        auto it = balances_.find(acct);
        return it == balances_.end() ? 0 : it->second;
    }
    std::map<std::string, std::int64_t> balances_;
    std::vector<std::string> journal_;

    // --- the queue: the ONLY shared state ---
    template <class F>
    auto enqueue(F&& f) -> std::future<decltype(f())> {
        using R = decltype(f());
        // packaged_task wraps the callable and a promise; its future is returned.
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.emplace_back([task] { (*task)(); });   // type-erased: queue holds void() closures
        }
        cv_.notify_one();
        return fut;
    }

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;   // drain before exit
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();   // run OUTSIDE the lock so callers can keep enqueueing
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::thread worker_;   // declared LAST: constructed last (all members ready), destroyed first (joined in ~)
};

int main() {
    ActiveLedger ledger;

    // Many threads post concurrently. The ledger applies them one at a time, in
    // queue order, with no data race on balances_.
    std::vector<std::thread> clients;
    for (int t = 0; t < 4; ++t) {
        clients.emplace_back([&ledger, t] {
            for (int i = 0; i < 250; ++i) ledger.post("acct:" + std::to_string(t % 2), 10).wait();
        });
    }
    for (auto& c : clients) c.join();

    // Synchronous-looking calls: enqueue, then .get() blocks for the result.
    std::int64_t b0 = ledger.balance("acct:0").get();
    std::int64_t b1 = ledger.balance("acct:1").get();
    std::size_t n = ledger.journal_size().get();
    std::printf("acct:0 = %lld (expected 5000)\nacct:1 = %lld (expected 5000)\njournal = %zu entries\n",
                static_cast<long long>(b0), static_cast<long long>(b1), n);

    // Fire-and-forget: discard the future. The call still runs, in order.
    ledger.post("acct:0", -5000);
    std::printf("acct:0 after refund = %lld\n", static_cast<long long>(ledger.balance("acct:0").get()));
    return 0;   // ~ActiveLedger drains the queue and joins
}
