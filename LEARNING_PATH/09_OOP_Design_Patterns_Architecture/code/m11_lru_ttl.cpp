// m11_lru_ttl.cpp
// LLD WALKTHROUGH: an LRU cache with per-entry time-to-live (TTL).
//
// Requirements (as clarified in the interview):
//   - get(key) -> value or nothing; put(key, value, optional ttl); erase(key).
//   - Fixed capacity in entries. When full, evict the LEAST RECENTLY USED entry.
//   - Each entry expires after its TTL (default TTL from the constructor).
//     Expired entries behave as missing and are removed lazily on access, plus
//     an explicit purge_expired() sweep that a background timer may call.
//   - O(1) get and put. Stats: hits, misses, evictions, expirations.
//   - Testable: time comes from an injected Clock, so tests never sleep.
//   - Thread-safe variant: a wrapper with one mutex, and a sharded version.
//
// Data structure: a doubly linked list in recency order (front = most recent)
// plus a hash map from key to list iterator. std::list iterators stay valid
// when OTHER nodes are inserted or erased, and splice() moves a node to the
// front without reallocating it, which is what makes "touch" O(1).
//
// C equivalent: an intrusive doubly linked list (prev/next inside the entry
// struct) plus a hash table of pointers to entries. Same shape, manual memory.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11b m11_lru_ttl.cpp

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using Duration  = std::chrono::milliseconds;
using TimePoint = std::chrono::steady_clock::time_point;

// ---------------------------------------------------------------------------
// The clock seam. Production uses steady_clock (never system_clock for TTLs:
// wall time jumps when NTP corrects it). Tests use FakeClock and advance it.
// ---------------------------------------------------------------------------
class Clock {
public:
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SteadyClock final : public Clock {
public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

class FakeClock final : public Clock {
public:
    TimePoint now() const override { return t_; }
    void advance(Duration d) { t_ += d; }
private:
    TimePoint t_{};   // epoch of steady_clock; the absolute value never matters
};

struct CacheStats {
    std::size_t hits = 0, misses = 0, evictions = 0, expirations = 0;
};

// ---------------------------------------------------------------------------
// Single-threaded core. Not thread-safe ON PURPOSE: locking is a separate
// decision, made by the wrapper below. Keeping the core lock-free makes it
// simple to test and reusable inside a shard.
// ---------------------------------------------------------------------------
template <class K, class V>
class LruTtlCache {
public:
    LruTtlCache(std::size_t capacity, Duration default_ttl, const Clock& clock)
        : capacity_(capacity), default_ttl_(default_ttl), clock_(clock) {
        assert(capacity_ > 0);
        index_.reserve(capacity_);   // no rehash on the hot path
    }

    // Returns a COPY: a reference or pointer into the cache would dangle on
    // the next eviction, and in the thread-safe wrapper would escape the lock.
    std::optional<V> get(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end()) { ++stats_.misses; return std::nullopt; }
        auto node = it->second;
        if (is_expired(*node)) {                 // lazy expiry
            remove(it);
            ++stats_.expirations;
            ++stats_.misses;
            return std::nullopt;
        }
        lru_.splice(lru_.begin(), lru_, node);   // touch: O(1), no allocation
        ++stats_.hits;
        return node->value;
    }

    void put(const K& key, V value, std::optional<Duration> ttl = std::nullopt) {
        const TimePoint expires = clock_.now() + ttl.value_or(default_ttl_);
        auto it = index_.find(key);
        if (it != index_.end()) {                // overwrite: update and touch
            it->second->value   = std::move(value);
            it->second->expires = expires;
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }
        if (index_.size() == capacity_) evict_one();
        lru_.push_front(Entry{key, std::move(value), expires});
        index_.emplace(key, lru_.begin());
    }

    bool erase(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        remove(it);
        return true;
    }

    // O(n) sweep. Called by a timer in production so that cold expired entries
    // do not sit in memory forever. For huge caches, see the module text for a
    // min-heap or timing-wheel expiry index.
    std::size_t purge_expired() {
        std::size_t n = 0;
        for (auto node = lru_.begin(); node != lru_.end();) {
            if (is_expired(*node)) {
                index_.erase(node->key);
                node = lru_.erase(node);
                ++n;
            } else {
                ++node;
            }
        }
        stats_.expirations += n;
        return n;
    }

    std::size_t size() const noexcept { return index_.size(); }
    const CacheStats& stats() const noexcept { return stats_; }

    // For tests: keys from most to least recently used.
    std::vector<K> keys_mru_first() const {
        std::vector<K> out;
        for (const auto& e : lru_) out.push_back(e.key);
        return out;
    }

private:
    struct Entry {
        K key;            // stored so eviction can erase the map entry
        V value;
        TimePoint expires;
    };
    using List = std::list<Entry>;

    bool is_expired(const Entry& e) const { return clock_.now() >= e.expires; }

    void remove(typename std::unordered_map<K, typename List::iterator>::iterator it) {
        lru_.erase(it->second);
        index_.erase(it);
    }

    // Policy: prefer evicting an already-expired entry at the tail, else the
    // tail itself. Either way it is the least recently used one.
    void evict_one() {
        auto last = std::prev(lru_.end());
        if (is_expired(*last)) ++stats_.expirations; else ++stats_.evictions;
        index_.erase(last->key);
        lru_.pop_back();
    }

    const std::size_t capacity_;
    const Duration default_ttl_;
    const Clock& clock_;                         // not owned; outlives the cache
    List lru_;
    std::unordered_map<K, typename List::iterator> index_;
    CacheStats stats_;
};

// ---------------------------------------------------------------------------
// Thread-safe wrapper: one mutex. Note get() MUTATES (it moves the node and
// bumps counters), so a shared_mutex would not help: every call is a writer.
// ---------------------------------------------------------------------------
template <class K, class V>
class SyncLruTtlCache {
public:
    SyncLruTtlCache(std::size_t cap, Duration ttl, const Clock& c) : cache_(cap, ttl, c) {}
    std::optional<V> get(const K& k) { std::lock_guard<std::mutex> g(m_); return cache_.get(k); }
    void put(const K& k, V v, std::optional<Duration> ttl = std::nullopt) {
        std::lock_guard<std::mutex> g(m_);
        cache_.put(k, std::move(v), ttl);
    }
    bool erase(const K& k) { std::lock_guard<std::mutex> g(m_); return cache_.erase(k); }
    std::size_t size() const { std::lock_guard<std::mutex> g(m_); return cache_.size(); }
private:
    mutable std::mutex m_;
    LruTtlCache<K, V> cache_;
};

// ---------------------------------------------------------------------------
// Sharded: N independent caches, key hashed to a shard. Contention drops by
// about N. Trade-off: LRU becomes per-shard (approximate globally), and the
// capacity is split, so one hot shard can evict while others have room.
// ---------------------------------------------------------------------------
template <class K, class V, std::size_t N = 8>
class ShardedLruTtlCache {
public:
    ShardedLruTtlCache(std::size_t total_cap, Duration ttl, const Clock& c)
        : shards_(make_shards(total_cap, ttl, c, std::make_index_sequence<N>{})) {}
    std::optional<V> get(const K& k) { return shard(k).get(k); }
    void put(const K& k, V v) { shard(k).put(k, std::move(v)); }
    std::size_t size() const {
        std::size_t n = 0;
        for (const auto& s : shards_) n += s.size();
        return n;
    }
private:
    using Shard = SyncLruTtlCache<K, V>;
    template <std::size_t... I>
    static std::array<Shard, N> make_shards(std::size_t cap, Duration ttl, const Clock& c,
                                            std::index_sequence<I...>) {
        // Each shard gets cap/N (at least 1). The comma trick repeats the args N times.
        return {{((void)I, Shard(cap / N ? cap / N : 1, ttl, c))...}};
    }
    Shard& shard(const K& k) { return shards_[std::hash<K>{}(k) % N]; }
    std::array<Shard, N> shards_;
};

// ---------------------------------------------------------------------------
// Self-checks.
// ---------------------------------------------------------------------------
int main() {
    using namespace std::chrono_literals;
    FakeClock clock;

    // 1. LRU order and eviction.
    {
        LruTtlCache<std::string, int> c(3, 1000ms, clock);
        c.put("a", 1); c.put("b", 2); c.put("c", 3);
        assert(c.get("a") == 1);                       // a is now most recent
        c.put("d", 4);                                 // evicts b (least recent)
        assert(!c.get("b"));
        assert((c.keys_mru_first() == std::vector<std::string>{"d", "a", "c"}));
        assert(c.stats().evictions == 1);
        c.put("a", 10);                                // overwrite touches
        assert(c.get("a") == 10 && c.size() == 3);
        assert(c.erase("c") && !c.erase("c"));
    }

    // 2. TTL: default and per-entry, lazy expiry and purge.
    {
        LruTtlCache<std::string, std::string> c(10, 100ms, clock);
        c.put("short", "x", 10ms);
        c.put("long", "y");                            // default 100 ms
        clock.advance(10ms);
        assert(!c.get("short"));                       // expires exactly at deadline
        assert(c.get("long") == std::string("y"));
        clock.advance(200ms);
        c.put("fresh", "z");
        assert(c.purge_expired() == 1);                // "long" swept
        assert(c.size() == 1);
        assert(c.stats().expirations == 2);
    }

    // 3. Eviction prefers counting an expired tail as an expiration.
    {
        LruTtlCache<int, int> c(2, 50ms, clock);
        c.put(1, 1, 5ms); c.put(2, 2);
        clock.advance(6ms);
        c.put(3, 3);                                   // tail (1) is expired
        assert(c.stats().expirations == 1 && c.stats().evictions == 0);
    }

    // 4. Concurrency smoke test on the sharded cache with the real clock.
    {
        SteadyClock real;
        ShardedLruTtlCache<int, int, 4> c(400, 10000ms, real);
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&c, t] {
                for (int i = 0; i < 1000; ++i) {
                    c.put(t * 1000 + i, i);
                    (void)c.get(t * 1000 + i / 2);
                }
            });
        for (auto& th : ts) th.join();
        assert(c.size() <= 400);
    }

    std::puts("m11_lru_ttl: all checks passed");
    return 0;
}
