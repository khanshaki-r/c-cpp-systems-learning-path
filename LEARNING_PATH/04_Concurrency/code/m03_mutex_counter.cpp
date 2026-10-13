// m03_mutex_counter.cpp — lock_guard, scoped_lock (two mutexes), unique_lock (early release).
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m03_mutex_counter.cpp -o m03
#include <mutex>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <map>
#include <string>

// A thread-safe counter map: the smallest realistic "store guarded by a mutex".
class CounterMap {
    mutable std::mutex m_;              // mutable: const readers must still lock
    std::map<std::string, long> data_;  // GUARDED_BY(m_)
public:
    void increment(const std::string& key, long by = 1) {
        std::lock_guard<std::mutex> lk(m_);      // RAII: unlocked on every exit path
        data_[key] += by;
    }
    long get(const std::string& key) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = data_.find(key);
        return it == data_.end() ? 0 : it->second;
    }
    // Snapshot copy: hold the lock only for the copy, never while the caller iterates.
    std::map<std::string, long> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return data_;
    }
};

// Two accounts, two mutexes, lock both without deadlock regardless of argument order.
struct Account {
    std::mutex m;
    long balance = 0;
    explicit Account(long b) : balance(b) {}
};

void transfer(Account& from, Account& to, long amount) {
    std::scoped_lock lk(from.m, to.m);   // WHY: std::lock algorithm; transfer(a,b) and
    from.balance -= amount;              // transfer(b,a) in parallel cannot deadlock
    to.balance   += amount;
}

// unique_lock: release early so the slow part runs unlocked.
std::mutex log_mtx;
std::vector<std::string> log_lines;

void log_and_do_slow_work(const std::string& msg) {
    std::unique_lock<std::mutex> lk(log_mtx);
    log_lines.push_back(msg);
    lk.unlock();                                   // WHY: the sleep below must not hold the lock
    std::this_thread::sleep_for(std::chrono::microseconds(50));
}

int main() {
    CounterMap counters;
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&counters, t] {
            for (int i = 0; i < 100000; ++i) counters.increment(t % 2 ? "odd" : "even");
        });
    }
    for (auto& t : ts) t.join();
    std::cout << "odd=" << counters.get("odd") << " even=" << counters.get("even") << '\n';
    // 400000 and 400000, every time.

    Account a(1000), b(1000);
    std::thread t1([&] { for (int i = 0; i < 10000; ++i) transfer(a, b, 1); });
    std::thread t2([&] { for (int i = 0; i < 10000; ++i) transfer(b, a, 1); });
    t1.join(); t2.join();
    std::cout << "a=" << a.balance << " b=" << b.balance << " sum=" << a.balance + b.balance << '\n';

    std::vector<std::thread> loggers;
    for (int i = 0; i < 4; ++i)
        loggers.emplace_back([i] {
            for (int k = 0; k < 100; ++k) log_and_do_slow_work("t" + std::to_string(i));
        });
    for (auto& t : loggers) t.join();
    std::cout << "log lines: " << log_lines.size() << '\n';   // 400

    auto snap = counters.snapshot();   // iterate a copy, no lock held
    for (const auto& kv : snap) std::cout << kv.first << " -> " << kv.second << '\n';
    return 0;
}
