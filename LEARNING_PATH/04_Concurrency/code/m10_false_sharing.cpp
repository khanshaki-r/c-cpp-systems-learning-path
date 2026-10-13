// m10_false_sharing.cpp — false sharing vs padded slots vs true sharing, measured.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m10_false_sharing.cpp -o m10
// Run:   ./m10              (optionally: taskset -c 0-7 ./m10 to pin the CPU set)
// Read the numbers as ratios, not absolutes. Absolute rates depend on the CPU and on WSL.
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <new>
#include <cstddef>

// WHY 64 rather than std::hardware_destructive_interference_size: GCC 12+ warns
// (-Winterference-size) that the constant can change with -mtune, which breaks ABI if it leaks
// into a header. 64 bytes is the line size on every current x86-64 and most ARM server cores.
// Apple M-series uses 128. Exercise 2 asks you to try 32 and 128.
constexpr std::size_t kLine = 64;

struct Packed  { std::atomic<long> v{0}; };                       // 8 bytes: 8 per line
struct alignas(kLine) Padded { std::atomic<long> v{0}; };         // one per line

static_assert(sizeof(Padded) == kLine, "padded slot must fill exactly one line");

template <class Slot>
double run(const char* label, int threads, std::chrono::milliseconds dur) {
    std::vector<Slot> slots(static_cast<std::size_t>(threads));   // C++17 aligned new honours alignas
    std::atomic<bool> stop{false};
    std::vector<long> local(static_cast<std::size_t>(threads));
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            long n = 0;
            auto& slot = slots[static_cast<std::size_t>(t)];
            while (!stop.load(std::memory_order_relaxed)) {
                slot.v.fetch_add(1, std::memory_order_relaxed);   // WHY relaxed: pure counter
                ++n;
            }
            local[static_cast<std::size_t>(t)] = n;   // written once at the end: no false sharing here
        });
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    long total = 0; for (long n : local) total += n;
    double mops = static_cast<double>(total) / static_cast<double>(dur.count()) / 1000.0;
    std::cout << label << " threads=" << threads << " sizeof(slot)=" << sizeof(Slot)
              << " : " << mops << " M increments/s\n";
    return mops;
}

// True sharing: all threads hit one counter. The line ping-pongs AND the RMWs serialise.
double run_shared(int threads, std::chrono::milliseconds dur) {
    alignas(kLine) std::atomic<long> one{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) one.fetch_add(1, std::memory_order_relaxed);
        });
    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    double mops = static_cast<double>(one.load()) / static_cast<double>(dur.count()) / 1000.0;
    std::cout << "true-shared            threads=" << threads << " : " << mops << " M increments/s\n";
    return mops;
}

int main() {
    using namespace std::chrono_literals;
    std::cout << "cache line assumed: " << kLine << " bytes, hardware threads: "
              << std::thread::hardware_concurrency() << "\n\n";
    for (int t : {1, 2, 4, 8}) {
        double packed = run<Packed>("packed (false sharing)", t, 500ms);
        double padded = run<Padded>("padded (no sharing)   ", t, 500ms);
        run_shared(t, 500ms);
        std::cout << "padded/packed ratio: " << (packed > 0 ? padded / packed : 0.0) << "\n\n";
    }
    // Expected shape (typical, not a promise): at 1 thread all three are equal; as threads grow,
    // padded scales roughly linearly, packed flattens or drops, true-shared drops the most.
    return 0;
}
