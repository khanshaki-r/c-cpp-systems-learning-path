// m08_spsc_ring.cpp — single-producer single-consumer ring buffer: no CAS, two indices,
// release/acquire pairs, indices on separate cache lines.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m08_spsc_ring.cpp -o m08b
#include <atomic>
#include <thread>
#include <iostream>
#include <chrono>
#include <cstddef>
#include <cstdint>

template <class T, std::size_t N>   // N power of two
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    alignas(64) std::atomic<std::size_t> head_{0};   // written by consumer, read by producer
    alignas(64) std::atomic<std::size_t> tail_{0};   // written by producer, read by consumer
    alignas(64) T buf_[N]{};
public:
    bool push(const T& v) {
        std::size_t t = tail_.load(std::memory_order_relaxed);        // own index: relaxed
        std::size_t h = head_.load(std::memory_order_acquire);        // see consumer's pops
        if (t - h == N) return false;                                 // full
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);                // publish the slot
        return true;
    }
    bool pop(T& out) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        std::size_t t = tail_.load(std::memory_order_acquire);        // see producer's pushes
        if (h == t) return false;                                     // empty
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);                // free the slot
        return true;
    }
};

int main() {
    constexpr std::uint64_t M = 20'000'000;
    SpscRing<std::uint64_t, 1024> ring;

    auto t0 = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < M; ++i)
            while (!ring.push(i)) std::this_thread::yield();   // full: back off
    });

    std::uint64_t sum = 0, next_expected = 0, out_of_order = 0;
    for (std::uint64_t got = 0; got < M;) {
        std::uint64_t v;
        if (!ring.pop(v)) { std::this_thread::yield(); continue; }   // empty: back off
        if (v != next_expected) ++out_of_order;                      // WHY check: proves FIFO + no tearing
        next_expected = v + 1;
        sum += v;
        ++got;
    }
    producer.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    std::uint64_t expected = M * (M - 1) / 2;
    std::cout << "items=" << M << " sum=" << sum << (sum == expected ? " (ok)" : " (MISMATCH)")
              << " out_of_order=" << out_of_order << '\n';
    std::cout << "throughput: " << (static_cast<double>(M) / ms / 1000.0) << " M items/s\n";
    return (sum == expected && out_of_order == 0) ? 0 : 1;
}
