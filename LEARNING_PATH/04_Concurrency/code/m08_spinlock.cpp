// m08_spinlock.cpp — test-and-test-and-set spinlock with pause and yield back-off, vs std::mutex.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m08_spinlock.cpp -o m08a
// Try:   ./m08a            then           taskset -c 0 ./m08a    (single core: spinlock collapses)
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <mutex>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void cpu_relax() { _mm_pause(); }
#else
static inline void cpu_relax() { std::this_thread::yield(); }
#endif

class Spinlock {
    std::atomic<bool> locked_{false};
public:
    void lock() {
        for (int spins = 0;; ++spins) {
            if (!locked_.exchange(true, std::memory_order_acquire)) return;
            // Test-and-test-and-set: spin on a read so the line stays Shared across spinners.
            while (locked_.load(std::memory_order_relaxed)) {
                if (spins < 100) cpu_relax();                 // WHY: short back-off, holder is likely active
                else std::this_thread::yield();               // WHY: let a descheduled holder run
            }
        }
    }
    void unlock() { locked_.store(false, std::memory_order_release); }
    bool try_lock() { return !locked_.exchange(true, std::memory_order_acquire); }
};

template <class Lock>
double bench(const char* name, int threads, int iters) {
    Lock lk;
    long shared = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < iters; ++i) { std::lock_guard<Lock> g(lk); ++shared; }
        });
    for (auto& t : ts) t.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << name << " threads=" << threads << " total=" << shared
              << " (expected " << static_cast<long>(threads) * iters << ") " << ms << " ms\n";
    return ms;
}

int main() {
    for (int t : {1, 2, 4, 8}) {
        bench<Spinlock>("spinlock  ", t, 1'000'000);
        bench<std::mutex>("std::mutex", t, 1'000'000);
    }
    return 0;
}
