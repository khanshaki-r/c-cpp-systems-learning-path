// m12_consistent_hash.cpp - consistent hashing ring with virtual nodes, replica preference lists,
// and measurements of balance and key movement compared with plain `hash % N`.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m12_consistent_hash.cpp -o chash
// Run:    ./chash
//
// The ring: hash every node (many times, once per virtual node) onto a 64-bit circle. A key is
// owned by the first node point clockwise from hash(key). Adding a node steals only the arcs just
// before its points; removing one hands its arcs to the next points. So about K/N keys move when
// the N-th node joins, instead of nearly all of them with `hash % N`.
//
// Used by: Dynamo, Cassandra (token ring), Riak, memcached client libraries (ketama), Envoy and
// nginx consistent-hash load balancing. Not used by YugabyteDB for placement: it hashes keys into
// a fixed 16-bit space (0x0000-0xFFFF) split into tablet ranges, and moves whole tablets instead.
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// FNV-1a 64-bit followed by the splitmix64 finalizer. FNV alone clusters on similar strings like
// "node-1#0", "node-1#1"; the finalizer spreads those bits across the whole word.
static uint64_t hash64(std::string_view s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
    h += 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

class HashRing {
public:
    explicit HashRing(int vnodes_per_node) : vnodes_(vnodes_per_node) {}

    void add_node(const std::string& node) {
        for (int v = 0; v < vnodes_; ++v) {
            uint64_t point = hash64(node + "#" + std::to_string(v));
            // A collision between two nodes' points is astronomically unlikely with 64 bits;
            // if it happened, the later node would silently lose that point. Assert instead.
            bool inserted = ring_.emplace(point, node).second;
            assert(inserted);
        }
        nodes_.insert(node);
    }

    void remove_node(const std::string& node) {
        for (int v = 0; v < vnodes_; ++v) ring_.erase(hash64(node + "#" + std::to_string(v)));
        nodes_.erase(node);
    }

    // Owner = first point at or after hash(key), wrapping around. O(log P) with P total points.
    const std::string& owner(std::string_view key) const {
        assert(!ring_.empty());
        auto it = ring_.lower_bound(hash64(key));
        if (it == ring_.end()) it = ring_.begin();
        return it->second;
    }

    // Preference list for replication factor rf: walk clockwise and collect the first rf
    // DISTINCT physical nodes (skipping further virtual points of a node already chosen).
    // This is how Dynamo and Cassandra (SimpleStrategy) choose replicas.
    std::vector<std::string> replicas(std::string_view key, size_t rf) const {
        std::vector<std::string> out;
        rf = std::min(rf, nodes_.size());
        auto it = ring_.lower_bound(hash64(key));
        for (size_t steps = 0; out.size() < rf && steps < ring_.size(); ++steps, ++it) {
            if (it == ring_.end()) it = ring_.begin();
            if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
        }
        return out;
    }

    size_t node_count() const { return nodes_.size(); }

private:
    int vnodes_;
    std::map<uint64_t, std::string> ring_;  // sorted by position on the circle
    std::set<std::string> nodes_;
};

static std::vector<std::string> make_keys(int n) {
    // Banking-style business keys: branch code + account number.
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "BR%03d-ACC%07d", i % 250, i);
        keys.emplace_back(buf);
    }
    return keys;
}

struct Balance {
    double max_over_mean;
    double min_over_mean;
};

static Balance measure_balance(const HashRing& ring, const std::vector<std::string>& keys) {
    std::unordered_map<std::string, int> count;
    for (const auto& k : keys) ++count[ring.owner(k)];
    double mean = static_cast<double>(keys.size()) / ring.node_count();
    int mx = 0, mn = static_cast<int>(keys.size());
    for (const auto& kv : count) {
        mx = std::max(mx, kv.second);
        mn = std::min(mn, kv.second);
    }
    if (count.size() < ring.node_count()) mn = 0;  // a node that owns nothing at all
    return {mx / mean, mn / mean};
}

int main() {
    const std::vector<std::string> keys = make_keys(200000);
    const std::vector<std::string> five = {"node-1", "node-2", "node-3", "node-4", "node-5"};

    // 1. Balance as a function of virtual nodes per physical node.
    std::printf("Balance of %zu keys over 5 nodes (1.00 is perfect):\n", keys.size());
    std::printf("  vnodes   max/mean   min/mean\n");
    double max_at_1 = 0, max_at_200 = 0;
    for (int v : {1, 10, 50, 200}) {
        HashRing ring(v);
        for (const auto& n : five) ring.add_node(n);
        Balance b = measure_balance(ring, keys);
        std::printf("  %6d   %8.2f   %8.2f\n", v, b.max_over_mean, b.min_over_mean);
        if (v == 1) max_at_1 = b.max_over_mean;
        if (v == 200) max_at_200 = b.max_over_mean;
    }
    // With one point per node, arcs are random lengths and imbalance is large. With a couple of
    // hundred points per node the variance of each node's total arc shrinks roughly as 1/sqrt(v).
    assert(max_at_200 < max_at_1);
    assert(max_at_200 < 1.25);

    // 2. Key movement when a sixth node joins: ring vs modulo.
    HashRing ring(200);
    for (const auto& n : five) ring.add_node(n);
    std::unordered_map<std::string, std::string> before;
    for (const auto& k : keys) before[k] = ring.owner(k);

    ring.add_node("node-6");
    int moved = 0;
    for (const auto& k : keys) {
        const std::string& now = ring.owner(k);
        if (now != before[k]) {
            ++moved;
            // Property: keys only move TO the new node, never between old nodes.
            assert(now == "node-6");
        }
    }
    double ring_frac = static_cast<double>(moved) / keys.size();

    int moved_mod = 0;
    for (const auto& k : keys) {
        uint64_t h = hash64(k);
        if (h % 5 != h % 6) ++moved_mod;
    }
    double mod_frac = static_cast<double>(moved_mod) / keys.size();

    std::printf("\nAdding a 6th node:\n");
    std::printf("  consistent hashing moved %5.1f%% of keys (ideal 1/6 = 16.7%%)\n", 100 * ring_frac);
    std::printf("  hash %% N           moved %5.1f%% of keys (expected 5/6 = 83.3%%)\n", 100 * mod_frac);
    assert(ring_frac > 0.10 && ring_frac < 0.25);
    assert(mod_frac > 0.75);

    // 3. Removing a node: only that node's keys move, and they spread over the survivors
    // (because its 200 points are scattered, each arc goes to a different successor).
    std::unordered_map<std::string, std::string> with6;
    for (const auto& k : keys) with6[k] = ring.owner(k);
    ring.remove_node("node-3");
    std::unordered_map<std::string, int> receivers;
    for (const auto& k : keys) {
        const std::string& now = ring.owner(k);
        if (with6[k] != "node-3") assert(now == with6[k]);
        else ++receivers[now];
    }
    std::printf("\nRemoving node-3: its keys went to %zu different survivors:", receivers.size());
    for (const auto& kv : receivers) std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n");
    assert(receivers.size() >= 4);  // load spreads; with 1 vnode it would all hit one neighbour

    // 4. Replica preference lists for RF=3.
    std::printf("\nRF=3 preference lists:\n");
    for (const char* k : {"BR001-ACC0000001", "BR017-ACC0004242", "BR249-ACC0199999"}) {
        auto reps = ring.replicas(k, 3);
        assert(reps.size() == 3);
        std::set<std::string> distinct(reps.begin(), reps.end());
        assert(distinct.size() == 3);         // three different physical machines
        assert(reps[0] == ring.owner(k));     // first replica is the owner
        std::printf("  %-18s -> %s, %s, %s\n", k, reps[0].c_str(), reps[1].c_str(), reps[2].c_str());
    }

    std::printf("\nall consistent-hashing checks passed\n");
    return 0;
}
