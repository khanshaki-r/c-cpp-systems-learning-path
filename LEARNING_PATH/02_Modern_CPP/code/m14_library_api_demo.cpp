// m14_library_api_demo.cpp
// Header/source split and API design for a small library. In a real project the implementation
// (kv_store.cpp) and the user (main.cpp) are separate translation units linked together; here they
// share one file so the example builds with one command:
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m14_library_api_demo.cpp
#include "kv_store.hpp"

#include <iostream>
#include <type_traits>
#include <utility>

// ---- what would be kv_store.cpp ------------------------------------------------------------------
namespace tcpkv {

KvStore::KvStore(std::size_t expected_keys) { map_.reserve(expected_keys); }

std::optional<std::string> KvStore::get(std::string_view key) const {
    auto it = map_.find(std::string(key));   // C++17: must materialise a std::string for the lookup
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

bool KvStore::put(std::string_view key, std::string value) {
    auto [it, inserted] = map_.insert_or_assign(std::string(key), std::move(value));
    (void)it;
    return inserted;
}

bool KvStore::erase(std::string_view key) { return map_.erase(std::string(key)) > 0; }

bool KvStore::contains(std::string_view key) const { return map_.find(std::string(key)) != map_.end(); }

}  // namespace tcpkv

// ---- what would be main.cpp (a user of the library) ----------------------------------------------
int main() {
    tcpkv::KvStore store(16);
    std::cout << "put new: " << store.put("acct:1", "5000") << '\n';
    std::cout << "put overwrite: " << store.put("acct:1", "5500") << '\n';
    if (auto v = store.get("acct:1")) std::cout << "acct:1=" << *v << '\n';
    std::cout << "missing present? " << store.get("missing").has_value() << '\n';

    // store.erase("acct:1");   // error with -Werror: [[nodiscard]] result ignored. The API forces a decision.
    bool erased = store.erase("acct:1");
    std::cout << "erased=" << erased << " contains=" << store.contains("acct:1") << " size=" << store.size() << '\n';

    tcpkv::KvStore moved = std::move(store);   // allowed: ownership of the whole map transfers in O(1)
    // tcpkv::KvStore copy = moved;            // error: copy constructor is deleted on purpose
    std::cout << "moved.size()=" << moved.size() << '\n';

    static_assert(!std::is_copy_constructible_v<tcpkv::KvStore>);
    static_assert(std::is_nothrow_move_constructible_v<tcpkv::KvStore>);
    return 0;
}
