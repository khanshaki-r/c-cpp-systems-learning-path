// m11_rcu_snapshot_cache.cpp — RCU-style read-mostly cache: readers grab an immutable snapshot
// through an atomic shared_ptr load; a refresher builds a new table off to the side and publishes
// it with one atomic store. Readers never block on the refresher and never see a half-built table.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m11_rcu_snapshot_cache.cpp -o m11_rcu
// TSan:  g++ -std=c++17 -g -O1 -fsanitize=thread -pthread m11_rcu_snapshot_cache.cpp -o m11_rcu_tsan
// Run:   ./m11_rcu        (8 readers)     ./m11_rcu 3     (3 readers: use when you have 4 cores)
// Note: std::atomic_load(&shared_ptr) is the C++17 spelling. C++20 replaces it with
// std::atomic<std::shared_ptr<T>>; same idea, same libstdc++ implementation (a spinlock pool).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Table {
    long version;
    std::vector<long> rates;
    long checksum;                                  // consistency witness: sum of rates
    explicit Table(long v) : version(v), rates(1024), checksum(0) {
        for (std::size_t i = 0; i < rates.size(); ++i) {
            rates[i] = v * 1000 + static_cast<long>(i);
            checksum += rates[i];
        }
    }
};

class SnapshotCache {
    std::shared_ptr<const Table> cur_;   // only ever touched through the atomic free functions
public:
    explicit SnapshotCache(std::shared_ptr<const Table> t) : cur_(std::move(t)) {}

    std::shared_ptr<const Table> get() const {
        return std::atomic_load_explicit(&cur_, std::memory_order_acquire);   // WHY acquire: see the table's contents
    }
    void publish(std::shared_ptr<const Table> next) {
        std::atomic_store_explicit(&cur_, std::move(next), std::memory_order_release);   // WHY release: table fully built first
    }
};

int main(int argc, char** argv) {
    // Default 8 readers as in the module. Pass a smaller number to avoid oversubscription:
    // with more threads than cores the refresher gets descheduled and its publish time balloons.
    const int kReaders = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
    const unsigned hw = std::thread::hardware_concurrency();
    const auto kRun = std::chrono::milliseconds(1000);

    SnapshotCache cache(std::make_shared<const Table>(0));
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0}, inconsistent{0}, max_version_seen{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r)
        readers.emplace_back([&] {
            long local_reads = 0, local_bad = 0, local_max = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto t = cache.get();                  // one refcount increment; no reader lock
                long s = 0;
                for (long x : t->rates) s += x;        // use the snapshot as long as we like
                if (s != t->checksum) ++local_bad;     // never happens: the table is immutable
                local_max = std::max(local_max, t->version);
                ++local_reads;
            }                                          // t dies: last holder of an old table frees it
            reads.fetch_add(local_reads);
            inconsistent.fetch_add(local_bad);
            long prev = max_version_seen.load();
            while (prev < local_max && !max_version_seen.compare_exchange_weak(prev, local_max)) {}
        });

    // Refresher: slow build unlocked, fast publish. Measure only the publish.
    long version = 1;
    long publishes = 0;
    std::vector<long long> publish_ns;
    long long build_ns_total = 0;
    auto end = Clock::now() + kRun;
    while (Clock::now() < end) {
        auto b0 = Clock::now();
        auto next = std::make_shared<const Table>(version++);   // slow build, nobody waits for it
        auto p0 = Clock::now();
        cache.publish(std::move(next));                          // the only shared step
        auto p1 = Clock::now();
        long long pns = std::chrono::duration_cast<std::chrono::nanoseconds>(p1 - p0).count();
        build_ns_total += std::chrono::duration_cast<std::chrono::nanoseconds>(p0 - b0).count();
        publish_ns.push_back(pns);
        ++publishes;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));   // a refresh every ~5 ms
    }
    stop.store(true);
    for (auto& t : readers) t.join();

    std::sort(publish_ns.begin(), publish_ns.end());
    auto pct = [&](double q) -> long long {
        if (publish_ns.empty()) return 0;
        return publish_ns[static_cast<std::size_t>(q * static_cast<double>(publish_ns.size() - 1))];
    };
    std::cout << "readers:            " << kReaders << " (hardware threads " << hw << ")\n"
              << "total reads:        " << reads.load() << '\n'
              << "inconsistent reads: " << inconsistent.load() << " (expected 0)\n"
              << "publishes:          " << publishes << '\n'
              << "newest version seen by a reader: " << max_version_seen.load()
              << " (last published " << version - 1 << ")\n"
              << "avg build ns:       " << (publishes ? build_ns_total / publishes : 0) << '\n'
              << "publish ns median:  " << pct(0.50) << '\n'
              << "publish ns p99:     " << pct(0.99) << '\n'
              << "publish ns max:     " << pct(1.0) << '\n';
    if (hw != 0 && static_cast<unsigned>(kReaders) + 1 > hw)
        std::cout << "note: more threads than cores; publish times include time spent descheduled.\n"
                  << "      re-run with " << (hw > 1 ? hw - 1 : 1) << " readers for the uncontended picture.\n";
    // Typical on an idle machine with spare cores: median publish is around a microsecond or less.
    // libstdc++ guards atomic shared_ptr operations with a small pool of mutexes keyed by the
    // shared_ptr's address (_Sp_locker), so 8 readers hammering get() contend on the SAME pool
    // entry the refresher needs; the tail (p99, max) shows that contention plus scheduling.
    // Readers still never wait for the slow build, which is the property that matters.
    // The old table's destructor runs wherever the last reference drops: inside publish() if no
    // reader holds it, or on a READER thread if one does. In a latency-critical path, keep
    // destructors cheap or hand old snapshots to a reclaimer thread.
    return 0;
}
