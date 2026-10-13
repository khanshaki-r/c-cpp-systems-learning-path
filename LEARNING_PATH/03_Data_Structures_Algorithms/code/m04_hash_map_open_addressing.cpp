// m04_hash_map_open_addressing.cpp
// Linear-probing hash map with tombstones, power-of-two capacity, Fibonacci bit mixing,
// rehash at 0.7 load (counting tombstones). Differential-tested against std::unordered_map
// and timed against it for point lookups.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m04_hash_map_open_addressing.cpp -o m04_open

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

template <typename K, typename V, typename Hash = std::hash<K>>
class FlatMap {
    enum class State : std::uint8_t { Empty, Full, Tombstone };
    struct Slot {
        K key{};
        V value{};
        State state = State::Empty;
    };

public:
    explicit FlatMap(std::size_t initial_log2 = 4) { allocate(initial_log2); }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

    V* find(const K& key) {
        for (std::size_t i = probe_start(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.state == State::Empty) return nullptr;              // a hole ends the cluster
            if (s.state == State::Full && s.key == key) return &s.value;
            // Tombstone, or a different key: continue the probe sequence
        }
    }

    bool insert(const K& key, const V& value) {
        if (V* existing = find(key)) { *existing = value; return false; }
        // Tombstones occupy slots too; count them so a churned table still rehashes.
        if ((size_ + tombstones_ + 1) * 10 > slots_.size() * 7) rehash(log2_size_ + 1);
        std::size_t i = probe_start(key);
        while (slots_[i].state == State::Full) i = (i + 1) & mask_;  // first non-Full slot
        if (slots_[i].state == State::Tombstone) --tombstones_;       // reuse it
        slots_[i].key = key;
        slots_[i].value = value;
        slots_[i].state = State::Full;
        ++size_;
        return true;
    }

    bool erase(const K& key) {
        for (std::size_t i = probe_start(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.state == State::Empty) return false;
            if (s.state == State::Full && s.key == key) {
                // Cannot mark Empty: keys that probed past this slot would become unreachable.
                s.state = State::Tombstone;
                --size_;
                ++tombstones_;
                return true;
            }
        }
    }

    // Average probes per successful find: a health metric you would expose in production.
    double average_probe_length() const {
        std::size_t total = 0, found = 0;
        for (const Slot& s : slots_) {
            if (s.state != State::Full) continue;
            std::size_t i = probe_start(s.key), probes = 1;
            while (!(slots_[i].state == State::Full && slots_[i].key == s.key)) { i = (i + 1) & mask_; ++probes; }
            total += probes;
            ++found;
        }
        return found ? static_cast<double>(total) / static_cast<double>(found) : 0.0;
    }

private:
    void allocate(std::size_t log2) {
        log2_size_ = log2;
        slots_.assign(std::size_t{1} << log2, Slot{});
        mask_ = slots_.size() - 1;
    }

    // Fibonacci hashing: multiply by 2^64/phi and take the HIGH bits. This spreads keys that
    // share low bits (pointers, multiples of 8) which an identity hash + mask would pile up.
    std::size_t probe_start(const K& key) const {
        std::uint64_t h = static_cast<std::uint64_t>(hash_(key)) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(h >> (64 - log2_size_));
    }

    void rehash(std::size_t new_log2) {
        std::vector<Slot> old = std::move(slots_);
        allocate(new_log2);
        size_ = 0;
        tombstones_ = 0;
        for (Slot& s : old)
            if (s.state == State::Full) insert(s.key, s.value);   // tombstones are dropped here
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t log2_size_ = 0;
    std::size_t size_ = 0;
    std::size_t tombstones_ = 0;
    Hash hash_;
};

namespace {
double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main() {
    // ---- differential test --------------------------------------------------------------
    std::mt19937 rng(7);
    std::unordered_map<int, int> ref;
    FlatMap<int, int> mine;
    for (int step = 0; step < 300'000; ++step) {
        int key = static_cast<int>(rng() % 8000) * 8;   // multiples of 8: stress the mixing
        switch (rng() % 3) {
            case 0: ref[key] = step; mine.insert(key, step); break;
            case 1: ref.erase(key); mine.erase(key); break;
            default: {
                auto it = ref.find(key);
                int* p = mine.find(key);
                assert((it == ref.end()) == (p == nullptr));
                if (p) assert(*p == it->second);
            }
        }
    }
    assert(ref.size() == mine.size());
    std::printf("FlatMap: %zu entries in %zu slots, avg probe length %.2f\n", mine.size(),
                mine.capacity(), mine.average_probe_length());

    // ---- lookup timing: chained std::unordered_map vs flat linear probing ----------------
    const int n = 1'000'000;
    std::vector<int> keys(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) keys[static_cast<std::size_t>(i)] = static_cast<int>(rng());

    std::unordered_map<int, int> um;
    um.reserve(static_cast<std::size_t>(n));
    FlatMap<int, int> fm(21);   // 2^21 slots = 2M, load ~0.5
    for (int k : keys) { um.emplace(k, k); fm.insert(k, k); }

    std::shuffle(keys.begin(), keys.end(), rng);   // random access order: defeat the prefetcher
    long long sum1 = 0, sum2 = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (int k : keys) sum1 += um.find(k)->second;
    double t_um = seconds_since(t0);

    t0 = std::chrono::steady_clock::now();
    for (int k : keys) sum2 += *fm.find(k);
    double t_fm = seconds_since(t0);

    assert(sum1 == sum2);
    std::printf("%d random lookups: unordered_map %.1f ns/op, FlatMap %.1f ns/op\n", n,
                t_um * 1e9 / n, t_fm * 1e9 / n);
    return 0;
}
