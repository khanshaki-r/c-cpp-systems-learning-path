// m06_deadlock_demo.cpp — a real deadlock (on demand), three fixes, and a hierarchical mutex.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m06_deadlock_demo.cpp -o m06
// Run:   ./m06          (fixes only)
//        ./m06 hang     (runs the broken version; attach gdb: gdb -p $(pgrep m06), thread apply all bt)
#include <mutex>
#include <thread>
#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <stdexcept>
#include <functional>
#include <utility>
#include <cstdlib>

struct Account {
    std::mutex m;
    long balance;
    std::string name;
    Account(std::string n, long b) : balance(b), name(std::move(n)) {}
};

// BROKEN: acquires in argument order. transfer(a,b) and transfer(b,a) in parallel will deadlock.
void transfer_broken(Account& from, Account& to, long amt) {
    std::lock_guard<std::mutex> l1(from.m);
    std::this_thread::sleep_for(std::chrono::microseconds(10));   // WHY: widen the window so it reliably hangs
    std::lock_guard<std::mutex> l2(to.m);
    from.balance -= amt; to.balance += amt;
}

// FIX 1: scoped_lock locks both with the std::lock algorithm; argument order is irrelevant.
void transfer_scoped(Account& from, Account& to, long amt) {
    std::scoped_lock lk(from.m, to.m);
    from.balance -= amt; to.balance += amt;
}

// FIX 2: a global order by address. Equivalent guarantee, shows the principle explicitly.
void transfer_ordered(Account& from, Account& to, long amt) {
    Account* first = &from; Account* second = &to;
    if (std::less<Account*>{}(second, first)) std::swap(first, second);   // WHY std::less: defined for unrelated pointers
    std::lock_guard<std::mutex> l1(first->m);
    std::lock_guard<std::mutex> l2(second->m);
    from.balance -= amt; to.balance += amt;
}

// FIX 3: try_lock with back-off. Breaks hold-and-wait; needs randomised sleep to avoid livelock.
void transfer_trylock(Account& from, Account& to, long amt) {
    for (int attempt = 0;; ++attempt) {
        std::unique_lock<std::mutex> l1(from.m);
        if (to.m.try_lock()) {
            std::lock_guard<std::mutex> l2(to.m, std::adopt_lock);
            from.balance -= amt; to.balance += amt;
            return;
        }
        l1.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(1 + (attempt * 7) % 13));   // crude jitter
    }
}

// A hierarchical mutex: debug-time enforcement of a lock order. Convention here: you may only
// acquire a mutex with a rank LOWER than the lowest you currently hold.
class hierarchical_mutex {
    std::mutex m_;
    const unsigned long rank_;
    unsigned long prev_ = 0;
    static thread_local unsigned long this_thread_lowest;   // ULONG_MAX = holding nothing
    void check() const {
        if (rank_ >= this_thread_lowest)
            throw std::logic_error("lock order violation: rank " + std::to_string(rank_) +
                                   " while holding " + std::to_string(this_thread_lowest));
    }
    void record() { prev_ = this_thread_lowest; this_thread_lowest = rank_; }
public:
    explicit hierarchical_mutex(unsigned long rank) : rank_(rank) {}
    void lock()   { check(); m_.lock(); record(); }
    void unlock() { this_thread_lowest = prev_; m_.unlock(); }
    bool try_lock() { check(); if (!m_.try_lock()) return false; record(); return true; }
};
thread_local unsigned long hierarchical_mutex::this_thread_lowest = ~0UL;

hierarchical_mutex high(10000), low(5000);

void correct_order() { std::lock_guard<hierarchical_mutex> a(high); std::lock_guard<hierarchical_mutex> b(low); }
void wrong_order()   { std::lock_guard<hierarchical_mutex> b(low);  std::lock_guard<hierarchical_mutex> a(high); }

template <class F>
void run_pair(const char* label, F transfer, bool expect_hang) {
    Account a("A", 1000), b("B", 1000);
    std::atomic<bool> done{false};
    std::thread t1([&] { for (int i = 0; i < 2000; ++i) transfer(a, b, 1); });
    std::thread t2([&] { for (int i = 0; i < 2000; ++i) transfer(b, a, 1); });
    std::thread watchdog([&] {
        for (int i = 0; i < 300 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done.load()) {
            std::cout << label << ": DEADLOCKED (watchdog fired after 3 s). Attach gdb now, or ctrl-C.\n";
            if (expect_hang) {
                std::this_thread::sleep_for(std::chrono::seconds(600));   // leave time to attach a debugger
                std::_Exit(0);   // WHY _Exit: joining the deadlocked threads is impossible
            }
        }
    });
    t1.join(); t2.join();
    done.store(true);
    watchdog.join();
    std::cout << label << ": ok, sum=" << a.balance + b.balance << '\n';
}

int main(int argc, char**) {
    run_pair("scoped_lock ", transfer_scoped, false);
    run_pair("ordered     ", transfer_ordered, false);
    run_pair("try_lock    ", transfer_trylock, false);

    correct_order();
    try { wrong_order(); } catch (const std::logic_error& e) { std::cout << "hierarchy caught: " << e.what() << '\n'; }

    if (argc > 1) {   // ./m06 hang  -> demonstrate the real deadlock for gdb practice
        std::cout << "running the broken version; this will hang...\n";
        run_pair("broken      ", transfer_broken, true);
    }
    return 0;
}
