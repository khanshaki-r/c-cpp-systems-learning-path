#include "tcpkv/store.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace tcpkv {

std::optional<std::string> Store::get(const std::string& key) const {
    const auto now = Clock::now();  // read the clock outside the lock: it is not free
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return std::nullopt;
    }
    return it->second.value;  // copy made under the lock; the caller owns it afterwards
}

void Store::set(std::string key, std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Entry& entry = map_[std::move(key)];
    entry.value = std::move(value);
    entry.expires_at.reset();  // like Redis: SET makes the key persistent again
}

bool Store::del(const std::string& key) {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }
    const bool was_live = !is_expired(it->second, now);
    map_.erase(it);
    return was_live;
}

bool Store::exists(const std::string& key) const {
    const auto now = Clock::now();
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    return it != map_.end() && !is_expired(it->second, now);
}

std::vector<std::string> Store::keys(std::string_view prefix) const {
    const auto now = Clock::now();
    std::vector<std::string> out;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [key, entry] : map_) {
            if (is_expired(entry, now)) {
                continue;
            }
            if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0) {
                out.push_back(key);
            }
        }
    }
    std::sort(out.begin(), out.end());  // sort after releasing the lock
    return out;
}

bool Store::expire(const std::string& key, Clock::duration ttl) {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return false;
    }
    if (ttl.count() <= 0) {
        map_.erase(it);
        return true;
    }
    it->second.expires_at = now + ttl;
    return true;
}

long long Store::ttl(const std::string& key) const {
    const auto now = Clock::now();
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return -2;
    }
    if (!it->second.expires_at) {
        return -1;
    }
    const auto remaining = *it->second.expires_at - now;
    return std::chrono::ceil<std::chrono::seconds>(remaining).count();
}

std::size_t Store::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return map_.size();
}

std::size_t Store::purge_expired() {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (is_expired(it->second, now)) {
            it = map_.erase(it);  // erase() returns the next valid iterator
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

}  // namespace tcpkv
