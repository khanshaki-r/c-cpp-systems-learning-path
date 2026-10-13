// kv_store.hpp - the public API of a tiny in-memory key-value library (used by m14_library_api_demo.cpp).
//
// API-design rules from module 14 made visible:
//   - explicit ownership (move-only), no raw new/delete anywhere in the interface
//   - std::string_view for read-only inputs, std::string by value for "sink" inputs
//   - std::optional for "maybe a value", never a sentinel or an out-parameter
//   - [[nodiscard]] on results that are easy to forget
//   - noexcept only where it is actually true
//   - const-correct: anything that does not change the store is const
//   - private representation; callers never see the map
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tcpkv {

class KvStore {
public:
    KvStore() = default;
    explicit KvStore(std::size_t expected_keys);   // explicit: 'KvStore s = 100;' must not compile

    // A store is an identity, not a value. Copying one is almost always a bug, so it is a compile error.
    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;
    KvStore(KvStore&&) noexcept = default;
    KvStore& operator=(KvStore&&) noexcept = default;
    ~KvStore() = default;

    // Returns a COPY so the caller never holds a reference into our map (which rehash/erase would invalidate).
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

    // Returns true if the key was newly inserted, false if an existing value was overwritten.
    bool put(std::string_view key, std::string value);

    // Not noexcept: in C++17 the lookup must build a std::string from the view, which can allocate.
    // (C++20 heterogeneous lookup for unordered containers removes that cost.)
    [[nodiscard]] bool erase(std::string_view key);
    [[nodiscard]] bool contains(std::string_view key) const;

    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    void clear() noexcept { map_.clear(); }

private:
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace tcpkv
