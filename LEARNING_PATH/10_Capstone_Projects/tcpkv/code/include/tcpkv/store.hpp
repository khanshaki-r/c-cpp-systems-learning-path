#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tcpkv {

// In-memory key/value store with optional per-key expiry.
//
// Concurrency: one std::shared_mutex. Readers (GET, EXISTS, KEYS, TTL) take a shared lock and
// run in parallel; writers (SET, DEL, EXPIRE, purge) take an exclusive lock. The map is never
// touched without the lock, so every method is safe to call from any thread.
//
// Expiry is lazy: a reader that finds an expired entry reports "not found" but does not erase
// it (it only holds a shared lock). purge_expired() erases them; the server calls it once a
// second from the acceptor thread.
class Store {
public:
    using Clock = std::chrono::steady_clock;

    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::optional<std::string> get(const std::string& key) const;
    void set(std::string key, std::string value);  // clears any TTL on the key
    bool del(const std::string& key);              // true if the key existed (and was live)
    bool exists(const std::string& key) const;

    // Live keys starting with `prefix` (empty prefix = all keys), sorted ascending.
    std::vector<std::string> keys(std::string_view prefix = {}) const;

    // ttl <= 0 deletes the key. Returns false if the key does not exist.
    // Takes Clock::duration (not seconds) so tests can use short TTLs; std::chrono::seconds
    // converts implicitly.
    bool expire(const std::string& key, Clock::duration ttl);

    // -2: key does not exist, -1: exists without expiry, otherwise seconds left (rounded up).
    long long ttl(const std::string& key) const;

    std::size_t size() const;  // includes expired-but-not-yet-purged entries

    // Erases expired entries; returns how many were removed.
    std::size_t purge_expired();

private:
    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expires_at;
    };

    static bool is_expired(const Entry& entry, Clock::time_point now) noexcept {
        return entry.expires_at.has_value() && *entry.expires_at <= now;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
};

}  // namespace tcpkv
