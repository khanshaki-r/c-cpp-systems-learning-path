// m12_false_sharing_bench.cpp
// Two threads, two counters. Adjacent counters share a cache line and fight over it; padded ones do not.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m12_false_sharing_bench.cpp
// Full treatment: see 04_Concurrency (memory model, atomics, false sharing).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

constexpr std::size_t kCacheLine = 64;   // typical x86-64 line size. C++17 has std::hardware_destructive_interference_size,
                                         // but GCC warns that its value is ABI-fragile, so we spell the number.

struct Adjacent {   // a and b sit in the same 64-byte line
    std::atomic<std::uint64_t> a{0};
    std::atomic<std::uint64_t> b{0};
};
struct Padded {     // each counter gets its own line
    alignas(kCacheLine) std::atomic<std::uint64_t> a{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> b{0};
};

template <class Counters>
double run_ms(Counters& c, std::uint64_t iters) {
    auto t0 = std::chrono::steady_clock::now();
    std::thread t1([&] { for (std::uint64_t i = 0; i < iters; ++i) c.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread t2([&] { for (std::uint64_t i = 0; i < iters; ++i) c.b.fetch_add(1, std::memory_order_relaxed); });
    t1.join();
    t2.join();
    auto t1_end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1_end - t0).count();
}

int main() {
    const std::uint64_t iters = 20'000'000;
    static_assert(sizeof(Padded) == 2 * kCacheLine, "two full cache lines");
    std::cout << "sizeof(Adjacent)=" << sizeof(Adjacent) << " sizeof(Padded)=" << sizeof(Padded) << '\n';

    Adjacent adj;
    Padded pad;
    for (int rep = 0; rep < 3; ++rep) {   // repeat: the first run includes thread start-up and cold caches
        double ms_adj = run_ms(adj, iters);
        double ms_pad = run_ms(pad, iters);
        std::cout << "rep " << rep << ": adjacent " << ms_adj << " ms, padded " << ms_pad
                  << " ms, ratio " << ms_adj / ms_pad << "x\n";
    }
    std::cout << "checksums " << adj.a.load() + adj.b.load() << ' ' << pad.a.load() + pad.b.load() << '\n';
    return 0;
}
