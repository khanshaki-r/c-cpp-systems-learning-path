// m07_atomic_counter_orders.cpp — correctness of fetch_add under every order, and the cost of
// seq_cst stores vs release stores on x86.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m07_atomic_counter_orders.cpp -o m07a
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <initializer_list>

template <class F>
double time_ms(F f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::atomic<long> counter{0};

void bump(std::memory_order mo, int n) {
    for (int i = 0; i < n; ++i) counter.fetch_add(1, mo);   // WHY: on x86 all orders emit lock xadd
}

std::atomic<long> seq_store_target{0};
void seq_stores(std::memory_order mo, int n) {
    for (int i = 0; i < n; ++i) seq_store_target.store(i, mo);   // WHY: here x86 differs: mov vs xchg
}

const char* order_name(std::memory_order mo) {
    switch (mo) {
    case std::memory_order_relaxed: return "relaxed";
    case std::memory_order_acquire: return "acquire";
    case std::memory_order_release: return "release";
    case std::memory_order_acq_rel: return "acq_rel";
    case std::memory_order_seq_cst: return "seq_cst";
    default: return "other";
    }
}

int main() {
    const int threads = 4, per = 2'000'000;
    for (auto mo : {std::memory_order_relaxed, std::memory_order_acq_rel, std::memory_order_seq_cst}) {
        counter.store(0);
        double ms = time_ms([&] {
            std::vector<std::thread> ts;
            for (int t = 0; t < threads; ++t) ts.emplace_back(bump, mo, per);
            for (auto& t : ts) t.join();
        });
        std::cout << "fetch_add " << order_name(mo) << ": counter=" << counter.load()
                  << " (expected " << static_cast<long>(threads) * per << ") " << ms << " ms\n";
    }
    for (auto mo : {std::memory_order_relaxed, std::memory_order_release, std::memory_order_seq_cst}) {
        double ms = time_ms([&] { seq_stores(mo, 50'000'000); });
        std::cout << "single-thread store " << order_name(mo) << ": " << ms << " ms\n";
    }
    static_assert(std::atomic<long>::is_always_lock_free, "need lock-free long");
    struct Big { long a, b, c; };
    std::cout << "atomic<8-byte long> always lock free: " << (std::atomic<long>::is_always_lock_free ? "yes" : "no") << '\n';
    std::cout << "atomic<24-byte struct> always lock free: " << (std::atomic<Big>::is_always_lock_free ? "yes" : "no")
              << "  (no: it would use a hidden lock in libatomic)\n";
    return 0;
}
