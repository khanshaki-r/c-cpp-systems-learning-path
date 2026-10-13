// m05_proxy_cache.cpp
// PROXY: an object that stands in for another and controls access to it.
// Here: a caching proxy in front of a slow database. Same interface as the
// real store (so it is a drop-in), but it answers repeated reads from memory,
// invalidates on write, and bounds its own size.
//
// Decorator vs proxy: identical structure (wraps, same interface). The
// difference is intent. A decorator ADDS behaviour the caller wants to see
// (logging). A proxy CONTROLS ACCESS and tries to be invisible (caching,
// lazy loading, remote access, permission checks).
//
// C equivalent: a store_ops table whose get() checks a hash table before
// calling the real one.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m05d m05_proxy_cache.cpp

#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

class Store {
public:
    virtual ~Store() = default;
    virtual void put(const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
};

// Stands in for a real DB round trip (~100 us to ~ms). We count calls instead
// of sleeping so the demo is deterministic.
class SlowDatabase final : public Store {
public:
    void put(const std::string& k, const std::string& v) override { ++round_trips_; data_[k] = v; }
    std::optional<std::string> get(const std::string& k) const override {
        ++round_trips_;
        auto it = data_.find(k);
        return it == data_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    std::uint64_t round_trips() const { return round_trips_; }

private:
    std::map<std::string, std::string> data_;
    mutable std::uint64_t round_trips_ = 0;
};

// ---------------------------------------------------------------------------
// The caching proxy. LRU eviction with a capacity bound. Write-through with
// cache update (so reads after a write are served from cache and are correct).
//
// Correctness contract it must keep (Liskov, Module 02): after put(k, v),
// get(k) returns v. It does. What it CANNOT promise: if someone else writes to
// the database behind our back, we serve stale data until eviction. That is
// the caching proxy's fundamental trade-off and must be stated in its docs.
// ---------------------------------------------------------------------------
class CachingStoreProxy final : public Store {
public:
    CachingStoreProxy(Store& backing, std::size_t capacity) : backing_(backing), capacity_(capacity) {}

    void put(const std::string& k, const std::string& v) override {
        backing_.put(k, v);          // write-through first: if it throws, cache is untouched
        insert_or_update(k, v);
    }

    std::optional<std::string> get(const std::string& k) const override {
        auto it = index_.find(k);
        if (it != index_.end()) {
            ++hits_;
            // Move to front = most recently used. splice is O(1) and keeps the
            // iterator stored in index_ valid (std::list iterators are stable).
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }
        ++misses_;
        auto v = backing_.get(k);
        if (v) insert_or_update(k, *v);   // negative results are NOT cached here; see exercise 3
        return v;
    }

    std::uint64_t hits() const { return hits_; }
    std::uint64_t misses() const { return misses_; }
    std::size_t size() const { return lru_.size(); }

private:
    using Entry = std::pair<std::string, std::string>;   // key, value
    using List = std::list<Entry>;

    void insert_or_update(const std::string& k, const std::string& v) const {
        auto it = index_.find(k);
        if (it != index_.end()) {
            it->second->second = v;
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }
        if (lru_.size() >= capacity_) {
            // Evict least recently used (back of the list).
            index_.erase(lru_.back().first);
            lru_.pop_back();
        }
        lru_.emplace_front(k, v);
        index_[k] = lru_.begin();
    }

    Store& backing_;                                              // observer: caller owns the DB
    std::size_t capacity_;
    mutable List lru_;                                            // front = most recent
    mutable std::unordered_map<std::string, List::iterator> index_;
    mutable std::uint64_t hits_ = 0, misses_ = 0;
};

// Application code sees a Store. It cannot tell proxy from database, which is
// the point: you can switch the proxy in or out in the composition root.
static void workload(Store& s) {
    s.put("acct:1", "100");
    s.put("acct:2", "200");
    s.put("acct:3", "300");
    for (int i = 0; i < 5; ++i) {
        s.get("acct:1");   // hot key
        s.get("acct:2");
    }
    s.get("acct:3");       // acct:3 evicted by now with capacity 2? depends on order; see output
    s.put("acct:1", "150");
    auto v = s.get("acct:1");
    std::printf("acct:1 after update -> %s\n", v ? v->c_str() : "(none)");
}

int main() {
    SlowDatabase db_direct;
    workload(db_direct);
    std::printf("direct:  round trips = %llu\n", static_cast<unsigned long long>(db_direct.round_trips()));

    SlowDatabase db;
    CachingStoreProxy proxy(db, 2);
    workload(proxy);
    std::printf("proxied: round trips = %llu, hits = %llu, misses = %llu, cached = %zu\n",
                static_cast<unsigned long long>(db.round_trips()), static_cast<unsigned long long>(proxy.hits()),
                static_cast<unsigned long long>(proxy.misses()), proxy.size());
    return 0;
}
