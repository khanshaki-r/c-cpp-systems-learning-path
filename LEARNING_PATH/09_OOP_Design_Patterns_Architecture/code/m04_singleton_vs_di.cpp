// m04_singleton_vs_di.cpp
// Singleton: three ways to write it, why it is usually a mistake, and the
// dependency-injection alternative that gives you the same "one instance"
// property without the global coupling.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m04b m04_singleton_vs_di.cpp

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Meyers singleton. Thread-safe since C++11: the standard guarantees that
//    initialisation of a function-local static happens exactly once even if
//    several threads arrive at the same time. GCC implements this with a guard
//    variable and __cxa_guard_acquire/release (a lock on first call only, then
//    a single byte load on the fast path).
//    Destroyed at program exit in reverse order of construction. If another
//    static's destructor uses it after it is gone: crash at exit.
// ---------------------------------------------------------------------------
class MetricsRegistry {
public:
    static MetricsRegistry& instance() {
        static MetricsRegistry inst;   // constructed on first call
        return inst;
    }
    void inc(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        counters_[name]++;
    }
    long get(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        return counters_[name];
    }

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

private:
    MetricsRegistry() = default;   // private: nobody else can construct one
    std::mutex mu_;
    // simple map substitute to keep the example small
    struct Counters {
        std::vector<std::pair<std::string, long>> v;
        long& operator[](const std::string& k) {
            for (auto& p : v)
                if (p.first == k) return p.second;
            v.emplace_back(k, 0);
            return v.back().second;
        }
    } counters_;
};

// ---------------------------------------------------------------------------
// 2. std::call_once. Same guarantee, explicit. Useful when the "once" thing is
//    not a static object but an action (open a log file, init a C library such
//    as OpenSSL or libpq's PQinitSSL).
// ---------------------------------------------------------------------------
class LegacyLibrary {
public:
    static void ensure_initialised() {
        std::call_once(flag_, [] {
            std::printf("[init] legacy C library initialised once\n");
            initialised_.store(true);
        });
    }
    static bool is_initialised() { return initialised_.load(); }

private:
    static std::once_flag flag_;
    static std::atomic<bool> initialised_;
};
std::once_flag LegacyLibrary::flag_;
std::atomic<bool> LegacyLibrary::initialised_{false};

// ---------------------------------------------------------------------------
// 3. The DI alternative. "There is one Config" is a WIRING decision, made in
//    main, not a TYPE property enforced by a private constructor. Any function
//    that needs the config receives it. Tests create as many as they like.
// ---------------------------------------------------------------------------
struct Config {
    std::string listen_addr;
    int workers;
};

class Metrics {   // ordinary class, no static instance()
public:
    void inc(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : counters_)
            if (p.first == name) { ++p.second; return; }
        counters_.emplace_back(name, 1);
    }
    long get(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : counters_)
            if (p.first == name) return p.second;
        return 0;
    }

private:
    std::mutex mu_;
    std::vector<std::pair<std::string, long>> counters_;
};

// Business code written against injected dependencies. Compare with code that
// calls MetricsRegistry::instance() inside: that code can only ever be tested
// against the global registry, and tests leak state into each other.
class RequestHandler {
public:
    RequestHandler(const Config& cfg, Metrics& metrics) : cfg_(cfg), metrics_(metrics) {}
    void handle() {
        metrics_.inc("requests");
        if (cfg_.workers > 4) metrics_.inc("fast_path");
    }

private:
    const Config& cfg_;
    Metrics& metrics_;
};

int main() {
    // Singleton usage: convenient, global, hidden dependency.
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([] {
            for (int j = 0; j < 1000; ++j) MetricsRegistry::instance().inc("hits");
        });
    for (auto& t : threads) t.join();
    std::printf("singleton hits = %ld (expected 4000)\n", MetricsRegistry::instance().get("hits"));

    // call_once usage: many callers, one initialisation.
    threads.clear();
    for (int i = 0; i < 4; ++i) threads.emplace_back([] { LegacyLibrary::ensure_initialised(); });
    for (auto& t : threads) t.join();
    std::printf("legacy initialised = %s\n", LegacyLibrary::is_initialised() ? "yes" : "no");

    // DI usage: one instance BY WIRING. Lifetime is obvious: these die at end of main,
    // in reverse order, before any static destructor runs. No exit-order surprises.
    Config cfg{"0.0.0.0:7000", 8};
    Metrics metrics;
    RequestHandler h1(cfg, metrics);
    RequestHandler h2(cfg, metrics);
    h1.handle();
    h2.handle();
    std::printf("DI requests = %ld, fast_path = %ld\n", metrics.get("requests"), metrics.get("fast_path"));

    // And for a test: a fresh Metrics, a different Config, zero shared state.
    Config test_cfg{"127.0.0.1:1", 1};
    Metrics test_metrics;
    RequestHandler under_test(test_cfg, test_metrics);
    under_test.handle();
    std::printf("test requests = %ld, fast_path = %ld (isolated)\n", test_metrics.get("requests"),
                test_metrics.get("fast_path"));
    return 0;
}
