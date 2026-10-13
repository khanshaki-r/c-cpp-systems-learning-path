// m07_rcu_snapshot.cpp
// READ-COPY-UPDATE style snapshots with shared_ptr<const T>.
//
// Problem: configuration (or a routing table, fee schedule, feature flags) is
// read on every request by many threads and updated rarely. A mutex around
// every read serialises the hot path for the sake of a once-a-minute write.
//
// Solution: readers grab a shared_ptr<const Config> (one atomic load + refcount
// increment), use it lock-free for as long as they like, and drop it. Writers
// build a NEW Config (copy, modify), then atomically swap the pointer. Readers
// holding the old snapshot keep a consistent view until they finish; the old
// object is freed when the last reader releases it.
//
// This is the user-space analogue of the Linux kernel's RCU: readers never
// block, writers pay the copy, old versions linger until quiescence.
//
// Immutability is the key: a snapshot is never modified after publication,
// so sharing it across threads needs no lock at all.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m07d m07_rcu_snapshot.cpp

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Config {
    int version;
    int max_connections;
    std::map<std::string, int> fee_bps;   // fee basis points by product
};

class ConfigHolder {
public:
    explicit ConfigHolder(Config initial) : current_(std::make_shared<const Config>(std::move(initial))) {}

    // Reader path: no mutex. std::atomic_load on shared_ptr is the C++17 spelling
    // (C++20 has std::atomic<std::shared_ptr<T>>). It does an atomic pointer load
    // plus an atomic increment of the refcount.
    std::shared_ptr<const Config> snapshot() const { return std::atomic_load_explicit(&current_, std::memory_order_acquire); }

    // Writer path: copy-on-write, serialised among writers by a mutex (writers
    // are rare; this costs nothing on the read path).
    template <class Mutator>
    void update(Mutator&& mutate) {
        std::lock_guard<std::mutex> lk(writer_mu_);
        Config next = *std::atomic_load(&current_);         // copy the current version
        mutate(next);                                        // modify the private copy
        next.version += 1;
        auto published = std::make_shared<const Config>(std::move(next));
        std::atomic_store_explicit(&current_, published, std::memory_order_release);   // publish atomically
        // Old Config is destroyed when the last reader's snapshot goes away.
    }

private:
    std::shared_ptr<const Config> current_;   // accessed only via atomic_load/atomic_store
    std::mutex writer_mu_;
};

int main() {
    ConfigHolder holder(Config{1, 100, {{"loan", 50}, {"card", 120}}});

    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::atomic<int> inconsistent{0};

    // Readers: hammer the snapshot. Each read sees ONE consistent version even
    // while writers are swapping. The consistency check: within a snapshot,
    // version N must always carry max_connections == 100 + N (writer keeps that invariant).
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto cfg = holder.snapshot();              // lock-free
                if (cfg->max_connections != 100 + cfg->version - 1) inconsistent.fetch_add(1);
                (void)cfg->fee_bps.count("loan");
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Writer: a few updates while readers run.
    for (int i = 0; i < 20; ++i) {
        holder.update([](Config& c) {
            c.max_connections += 1;                           // keep invariant max == 100 + version - 1
            c.fee_bps["loan"] += 1;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop.store(true);
    for (auto& t : readers) t.join();

    auto final_cfg = holder.snapshot();
    std::printf("final version = %d, max_connections = %d, loan bps = %d\n", final_cfg->version,
                final_cfg->max_connections, final_cfg->fee_bps.at("loan"));
    std::printf("reads = %ld, inconsistent snapshots = %d (expected 0)\n", reads.load(), inconsistent.load());
    std::printf("snapshot use_count now = %ld (holder + this local)\n", static_cast<long>(final_cfg.use_count()));
    return 0;
}
