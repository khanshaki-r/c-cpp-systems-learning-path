// tcpkv/store.hpp - thread-safe in-memory key-value store.
//
// One mutex around one unordered_map. Simple, correct, and measurable: module 07 profiles it,
// 04_Concurrency discusses sharding and reader-writer locks as the next step.
#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tcpkv {

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;             // a mutex is not copyable; make that explicit
    Store& operator=(const Store&) = delete;

    // Insert or overwrite. Takes by value so callers can std::move large values in.
    void set(std::string key, std::string value);

    // Returns a copy of the value so the caller never holds a reference into the map
    // after the lock is released.
    std::optional<std::string> get(std::string_view key) const;

    // Returns true if the key existed.
    bool del(std::string_view key);

    std::size_t size() const;
    void clear();

private:
    // 'mutable' so const members (get, size) can lock. The lock protects map_ only.
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace tcpkv
