// m04_shared_mutex_map.cpp — the tcpkv store: unordered_map behind a shared_mutex, plus a
// sharded variant, plus a steady-state benchmark.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m04_shared_mutex_map.cpp -o m04
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <string>
#include <optional>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <functional>
#include <cstddef>

class Store {
    mutable std::shared_mutex m_;
    std::unordered_map<std::string, std::string> map_;   // GUARDED_BY(m_)
public:
    std::optional<std::string> get(const std::string& k) const {
        std::shared_lock<std::shared_mutex> lk(m_);
        auto it = map_.find(k);
        if (it == map_.end()) return std::nullopt;
        return it->second;                 // WHY copy: references die when lk is released
    }
    void set(std::string k, std::string v) {
        std::unique_lock<std::shared_mutex> lk(m_);
        map_[std::move(k)] = std::move(v);
    }
    bool del(const std::string& k) {
        std::unique_lock<std::shared_mutex> lk(m_);
        return map_.erase(k) > 0;
    }
    std::size_t size() const {
        std::shared_lock<std::shared_mutex> lk(m_);
        return map_.size();
    }
};

// A sharded variant: N independent stores, chosen by hash. Same interface.
template <std::size_t N>
class ShardedStore {
    Store shards_[N];
    static std::size_t idx(const std::string& k) { return std::hash<std::string>{}(k) % N; }
public:
    std::optional<std::string> get(const std::string& k) const { return shards_[idx(k)].get(k); }
    void set(std::string k, std::string v) {
        std::size_t i = idx(k);                 // compute before moving k
        shards_[i].set(std::move(k), std::move(v));
    }
    bool del(const std::string& k) { return shards_[idx(k)].del(k); }
};

template <class S>
double run_benchmark(S& store, int readers, int writers, std::chrono::milliseconds dur) {
    std::atomic<bool> stop{false};
    std::atomic<long> ops{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 1000; ++i) store.set("key" + std::to_string(i), "value");

    for (int r = 0; r < readers; ++r)
        ts.emplace_back([&] {
            long local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 1000; ++i) {
                    (void)store.get("key" + std::to_string(i % 1000));
                    ++local;
                }
            }
            ops.fetch_add(local, std::memory_order_relaxed);   // WHY: one atomic add per thread, not per op
        });
    for (int w = 0; w < writers; ++w)
        ts.emplace_back([&] {
            long local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                store.set("key" + std::to_string(local % 1000), "v2");
                ++local;
            }
            ops.fetch_add(local, std::memory_order_relaxed);
        });

    std::this_thread::sleep_for(dur);
    stop.store(true);
    for (auto& t : ts) t.join();
    return static_cast<double>(ops.load()) / static_cast<double>(dur.count()) / 1000.0;   // Mops/s
}

int main() {
    using namespace std::chrono_literals;
    Store s;
    ShardedStore<16> sh;
    std::cout << "shared_mutex, 8 readers 0 writers: " << run_benchmark(s, 8, 0, 500ms) << " Mops/s\n";
    std::cout << "shared_mutex, 8 readers 2 writers: " << run_benchmark(s, 8, 2, 500ms) << " Mops/s\n";
    std::cout << "sharded x16,  8 readers 2 writers: " << run_benchmark(sh, 8, 2, 500ms) << " Mops/s\n";

    // Correctness smoke test
    Store c;
    c.set("a", "");
    std::cout << "present-empty: " << (c.get("a").has_value() ? "yes" : "no")
              << ", absent: " << (c.get("zz").has_value() ? "yes" : "no")
              << ", size: " << c.size() << '\n';
    std::cout << "del a: " << (c.del("a") ? "yes" : "no") << ", del a again: " << (c.del("a") ? "yes" : "no") << '\n';
    return 0;
}
