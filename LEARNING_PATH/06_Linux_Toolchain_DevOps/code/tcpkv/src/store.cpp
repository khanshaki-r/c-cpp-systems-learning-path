// src/store.cpp - one mutex, one hash map.
#include "tcpkv/store.hpp"

namespace tcpkv {

void Store::set(std::string key, std::string value) {
    std::lock_guard<std::mutex> lock(mu_);
    // insert_or_assign avoids the default-construct-then-assign of operator[].
    map_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> Store::get(std::string_view key) const {
    std::lock_guard<std::mutex> lock(mu_);
    // C++17 unordered_map has no heterogeneous lookup, so we must materialise a std::string.
    // (C++20 adds is_transparent support; see 02_Modern_Cpp for the upgrade.)
    const auto it = map_.find(std::string(key));
    if (it == map_.end()) return std::nullopt;
    return it->second;  // copy out while holding the lock
}

bool Store::del(std::string_view key) {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.erase(std::string(key)) > 0;
}

std::size_t Store::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
}

void Store::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
}

}  // namespace tcpkv
