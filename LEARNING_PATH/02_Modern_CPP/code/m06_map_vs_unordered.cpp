// m06_map_vs_unordered.cpp
// std::map (red-black tree) vs std::unordered_map (hash table): ordering, operator[], load factor, rehash.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m06_map_vs_unordered.cpp
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

int main() {
    std::map<std::string, int> ordered;             // red-black tree: sorted by key, O(log n), nodes never move
    std::unordered_map<std::string, int> hashed;    // hash table: average O(1), iteration order unspecified
    const char* keys[] = {"delta", "alpha", "charlie", "bravo", "echo"};
    for (int i = 0; i < 5; ++i) {
        ordered[keys[i]] = i;
        hashed[keys[i]] = i;
    }
    std::cout << "map (sorted):            ";
    for (const auto& [k, v] : ordered) std::cout << k << '=' << v << ' ';
    std::cout << "\nunordered_map (buckets): ";
    for (const auto& [k, v] : hashed) std::cout << k << '=' << v << ' ';
    std::cout << '\n';

    // operator[] INSERTS a default-constructed value when the key is missing. find() does not.
    std::cout << "size before ordered[\"zulu\"]: " << ordered.size();
    int z = ordered["zulu"];   // silently inserts "zulu" -> 0
    std::cout << ", after: " << ordered.size() << ", value=" << z << '\n';
    if (auto it = hashed.find("zulu"); it == hashed.end()) std::cout << "find() does not insert\n";
    std::cout << "count(\"alpha\")=" << hashed.count("alpha") << '\n';   // 0 or 1 for non-multi containers

    // Watching the hash table rehash as the load factor crosses max_load_factor (1.0 by default).
    std::unordered_map<int, int> h;
    std::size_t last_buckets = h.bucket_count();
    std::cout << "initial buckets=" << last_buckets << " max_load_factor=" << h.max_load_factor() << '\n';
    for (int i = 0; i < 2000; ++i) {
        h[i] = i;
        if (h.bucket_count() != last_buckets) {
            std::cout << "size=" << h.size() << " rehash " << last_buckets << " -> " << h.bucket_count()
                      << " (ALL iterators invalid; pointers/references to elements still valid)\n";
            last_buckets = h.bucket_count();
        }
    }
    std::unordered_map<int, int> pre;
    pre.reserve(2000);   // sizes the bucket array for 2000 elements: no rehash during the inserts
    std::size_t b0 = pre.bucket_count();
    for (int i = 0; i < 2000; ++i) pre[i] = i;
    std::cout << "with reserve(2000): buckets " << b0 << " -> " << pre.bucket_count()
              << ", load_factor=" << pre.load_factor() << '\n';

    // Only ORDERED containers can answer range questions.
    auto lb = ordered.lower_bound("c");
    std::cout << "first key >= \"c\": " << lb->first << '\n';
    std::cout << "keys in [\"b\", \"d\"): ";
    for (auto it = ordered.lower_bound("b"); it != ordered.lower_bound("d"); ++it) std::cout << it->first << ' ';
    std::cout << '\n';

    // Node sizes (typical for libstdc++ on x86-64): map node = 32 bytes header + pair; unordered node = next ptr + pair (+ cached hash).
    std::cout << "sizeof(pair<const string,int>)=" << sizeof(std::pair<const std::string, int>) << '\n';
    return 0;
}
