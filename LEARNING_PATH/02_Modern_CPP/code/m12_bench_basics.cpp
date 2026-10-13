// m12_bench_basics.cpp
// Micro-benchmarking basics: steady_clock, best-of-N, defeating the optimiser, vector vs list.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m12_bench_basics.cpp
// Then build again with -O0 and compare. The optimiser changes the picture completely.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <list>
#include <numeric>
#include <random>
#include <vector>

template <class F>
double time_ms(F&& f, int reps = 5) {   // best-of-N removes scheduler noise and keeps the "true" cost
    double best = 1e300;
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now();   // steady_clock never jumps (system_clock can, after NTP)
        f();
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

int main() {
    const std::size_t n = 1'000'000;
    std::mt19937 rng(42);   // fixed seed: reproducible input
    std::vector<std::uint32_t> data(n);
    for (auto& x : data) x = rng();

    std::vector<std::uint32_t> vec(data.begin(), data.end());   // contiguous: 4 MB, prefetcher-friendly
    std::list<std::uint32_t>   lst(data.begin(), data.end());   // one heap node per element, scattered

    std::uint64_t sink = 0;   // results MUST be used, or the optimiser is allowed to delete the whole loop

    double v_ms = time_ms([&] { sink += std::accumulate(vec.begin(), vec.end(), std::uint64_t{0}); });
    double l_ms = time_ms([&] { sink += std::accumulate(lst.begin(), lst.end(), std::uint64_t{0}); });
    std::cout << "sum 1M elements: vector " << v_ms << " ms, list " << l_ms
              << " ms  (same O(n); the difference is cache misses)\n";

    // Insert at the front: list is O(1) per insert, vector is O(n). For small n, vector still often wins.
    double vf_ms = time_ms([&] { std::vector<int> v; for (int i = 0; i < 20000; ++i) v.insert(v.begin(), i); sink += v.size(); }, 3);
    double lf_ms = time_ms([&] { std::list<int> l;   for (int i = 0; i < 20000; ++i) l.push_front(i);        sink += l.size(); }, 3);
    std::cout << "20k front inserts: vector " << vf_ms << " ms, list " << lf_ms << " ms\n";

    // Linear scan vs binary search on sorted data.
    std::sort(vec.begin(), vec.end());
    std::uint32_t needle = vec[n / 2];
    double lin_ms = time_ms([&] { sink += static_cast<std::uint64_t>(std::find(vec.begin(), vec.end(), needle) - vec.begin()); });
    double bin_ms = time_ms([&] { sink += static_cast<std::uint64_t>(std::lower_bound(vec.begin(), vec.end(), needle) - vec.begin()); });
    std::cout << "find the middle element: linear " << lin_ms << " ms, binary " << bin_ms << " ms\n";

    std::cout << "(sink=" << sink << ")\n";
    return 0;
}
