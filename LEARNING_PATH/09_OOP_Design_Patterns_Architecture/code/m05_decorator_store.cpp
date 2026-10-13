// m05_decorator_store.cpp
// DECORATOR: wrap an object implementing an interface with another object
// implementing the SAME interface, adding behaviour before/after forwarding.
// Decorators stack: Logging(Metrics(Retry(RealStore))). The service holds a
// Store& and does not know how many layers are under it.
//
// C equivalent: a store_ops table whose functions call another store_ops table
// stored in the context. Possible, but each layer re-declares every slot.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m05c m05_decorator_store.cpp

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// The interface everything implements.
// ---------------------------------------------------------------------------
class Store {
public:
    virtual ~Store() = default;
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual bool erase(const std::string& key) = 0;
};

// ---------------------------------------------------------------------------
// The real thing being decorated.
// ---------------------------------------------------------------------------
class MemoryStore final : public Store {
public:
    void put(const std::string& key, const std::string& value) override { data_[key] = value; }
    std::optional<std::string> get(const std::string& key) const override {
        auto it = data_.find(key);
        return it == data_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    bool erase(const std::string& key) override { return data_.erase(key) > 0; }

private:
    std::map<std::string, std::string> data_;
};

// A store that fails the first N calls, to show the retry decorator working.
class FlakyStore final : public Store {
public:
    FlakyStore(std::unique_ptr<Store> inner, int failures) : inner_(std::move(inner)), failures_left_(failures) {}
    void put(const std::string& k, const std::string& v) override { maybe_fail(); inner_->put(k, v); }
    std::optional<std::string> get(const std::string& k) const override { maybe_fail(); return inner_->get(k); }
    bool erase(const std::string& k) override { maybe_fail(); return inner_->erase(k); }

private:
    void maybe_fail() const {
        if (failures_left_ > 0) { --failures_left_; throw std::runtime_error("transient failure"); }
    }
    std::unique_ptr<Store> inner_;
    mutable int failures_left_;
};

// ---------------------------------------------------------------------------
// A base for decorators that forwards everything. Concrete decorators override
// only what they change. This avoids re-typing the forwarding in every layer.
// ---------------------------------------------------------------------------
class StoreDecorator : public Store {
public:
    explicit StoreDecorator(std::unique_ptr<Store> inner) : inner_(std::move(inner)) {}
    void put(const std::string& k, const std::string& v) override { inner_->put(k, v); }
    std::optional<std::string> get(const std::string& k) const override { return inner_->get(k); }
    bool erase(const std::string& k) override { return inner_->erase(k); }

protected:
    Store& inner() { return *inner_; }
    const Store& inner() const { return *inner_; }

private:
    std::unique_ptr<Store> inner_;   // the decorator OWNS the layer below it
};

// ---------------------------------------------------------------------------
// Decorator 1: logging.
// ---------------------------------------------------------------------------
class LoggingStore final : public StoreDecorator {
public:
    using StoreDecorator::StoreDecorator;
    void put(const std::string& k, const std::string& v) override {
        std::printf("  [log] put %s=%s\n", k.c_str(), v.c_str());
        inner().put(k, v);
    }
    std::optional<std::string> get(const std::string& k) const override {
        auto r = inner().get(k);
        std::printf("  [log] get %s -> %s\n", k.c_str(), r ? r->c_str() : "(miss)");
        return r;
    }
    bool erase(const std::string& k) override {
        bool r = inner().erase(k);
        std::printf("  [log] erase %s -> %s\n", k.c_str(), r ? "yes" : "no");
        return r;
    }
};

// ---------------------------------------------------------------------------
// Decorator 2: metrics. Counts and times every call. Notice it does not know
// or care whether the inner store is Logging, Retry or Memory.
// ---------------------------------------------------------------------------
class MetricsStore final : public StoreDecorator {
public:
    struct Counters {
        std::uint64_t puts = 0, gets = 0, erases = 0, errors = 0;
        std::chrono::nanoseconds total_time{0};
    };

    using StoreDecorator::StoreDecorator;

    void put(const std::string& k, const std::string& v) override {
        time_it([&] { inner().put(k, v); });
        ++c_.puts;
    }
    std::optional<std::string> get(const std::string& k) const override {
        std::optional<std::string> r;
        time_it([&] { r = inner().get(k); });
        ++c_.gets;
        return r;
    }
    bool erase(const std::string& k) override {
        bool r = false;
        time_it([&] { r = inner().erase(k); });
        ++c_.erases;
        return r;
    }
    const Counters& counters() const { return c_; }

private:
    template <class F>
    void time_it(F&& f) const {
        auto t0 = std::chrono::steady_clock::now();
        try {
            f();
        } catch (...) {
            ++c_.errors;
            c_.total_time += std::chrono::steady_clock::now() - t0;
            throw;   // metrics observe, they do not swallow
        }
        c_.total_time += std::chrono::steady_clock::now() - t0;
    }
    mutable Counters c_;   // get() is const for callers but updates counters
};

// ---------------------------------------------------------------------------
// Decorator 3: retry. Adds a policy around transient failures.
// ---------------------------------------------------------------------------
class RetryingStore final : public StoreDecorator {
public:
    RetryingStore(std::unique_ptr<Store> inner, int attempts) : StoreDecorator(std::move(inner)), attempts_(attempts) {}

    void put(const std::string& k, const std::string& v) override { retry([&] { inner().put(k, v); }); }
    std::optional<std::string> get(const std::string& k) const override {
        std::optional<std::string> r;
        retry([&] { r = inner().get(k); });
        return r;
    }
    bool erase(const std::string& k) override {
        bool r = false;
        retry([&] { r = inner().erase(k); });
        return r;
    }

private:
    template <class F>
    void retry(F&& f) const {
        for (int i = 1;; ++i) {
            try {
                f();
                return;
            } catch (const std::runtime_error& e) {
                if (i >= attempts_) throw;
                std::printf("  [retry] attempt %d failed (%s), retrying\n", i, e.what());
            }
        }
    }
    int attempts_;
};

// ---------------------------------------------------------------------------
// The service sees a Store. It is unaware of the stack.
// ---------------------------------------------------------------------------
static void run_service(Store& s) {
    s.put("acct:1001", "500");
    s.get("acct:1001");
    s.get("acct:9999");
    s.erase("acct:1001");
}

int main() {
    // Build the stack from the inside out. Ownership flows inward: the outermost
    // decorator owns everything below. One unique_ptr at the top owns the lot.
    auto real = std::make_unique<MemoryStore>();
    auto flaky = std::make_unique<FlakyStore>(std::move(real), 2);          // fails first 2 calls
    auto retry = std::make_unique<RetryingStore>(std::move(flaky), 3);
    auto metrics_layer = std::make_unique<MetricsStore>(std::move(retry));
    MetricsStore* metrics = metrics_layer.get();                              // observer, to read counters later
    std::unique_ptr<Store> top = std::make_unique<LoggingStore>(std::move(metrics_layer));

    std::printf("stack: Logging -> Metrics -> Retry -> Flaky -> Memory\n");
    run_service(*top);

    const auto& c = metrics->counters();
    std::printf("metrics: puts=%llu gets=%llu erases=%llu errors=%llu total=%lld us\n",
                static_cast<unsigned long long>(c.puts), static_cast<unsigned long long>(c.gets),
                static_cast<unsigned long long>(c.erases), static_cast<unsigned long long>(c.errors),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(c.total_time).count()));

    // Reorder the stack: Retry OUTSIDE metrics now counts a failed attempt as an error.
    std::printf("\nstack: Retry -> Metrics -> Flaky -> Memory\n");
    auto m2 = std::make_unique<MetricsStore>(std::make_unique<FlakyStore>(std::make_unique<MemoryStore>(), 1));
    MetricsStore* m2p = m2.get();
    RetryingStore top2(std::move(m2), 3);
    run_service(top2);
    std::printf("metrics: errors=%llu (retry above metrics => failures are visible)\n",
                static_cast<unsigned long long>(m2p->counters().errors));
    return 0;
}
