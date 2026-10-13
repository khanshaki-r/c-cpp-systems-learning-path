// m13_shared_mutex_cache.cpp
// A read-mostly key-value store guarded by std::shared_mutex: many readers in parallel, writers exclusive.
// This is the locking pattern tcpkv uses. Deep dive: 04_Concurrency.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m13_shared_mutex_cache.cpp
#include <atomic>
#include <iostream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class KvStore {
public:
    std::optional<std::string> get(const std::string& key) const {
        std::shared_lock lock(mu_);          // SHARED: other get() calls run concurrently with this one
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        return it->second;                   // copy out WHILE locked; never hand out a reference into the map
    }
    void put(const std::string& key, std::string value) {
        std::unique_lock lock(mu_);          // EXCLUSIVE: waits for all readers to leave, blocks new ones
        map_[key] = std::move(value);
    }
    bool erase(const std::string& key) {
        std::unique_lock lock(mu_);
        return map_.erase(key) > 0;
    }
    std::size_t size() const {
        std::shared_lock lock(mu_);
        return map_.size();
    }
private:
    mutable std::shared_mutex mu_;           // mutable: locking is not a logical change, so const get() may lock
    std::unordered_map<std::string, std::string> map_;
};

int main() {
    KvStore store;
    for (int i = 0; i < 100; ++i) store.put("k" + std::to_string(i), "v" + std::to_string(i));

    std::atomic<long> hits{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            for (int i = 0; i < 100'000; ++i)
                if (store.get("k" + std::to_string(i % 100))) hits.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            store.put("k" + std::to_string(i % 100), "w" + std::to_string(i));
            std::this_thread::yield();
        }
    });
    for (auto& t : readers) t.join();
    writer.join();

    std::cout << "hits=" << hits.load() << " size=" << store.size()
              << " k7=" << store.get("k7").value_or("?") << '\n';
    std::cout << "sizeof(std::mutex)=" << sizeof(std::mutex)
              << " sizeof(std::shared_mutex)=" << sizeof(std::shared_mutex) << '\n';
    return 0;
}
