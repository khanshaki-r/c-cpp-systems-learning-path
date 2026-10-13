// m13_union_find.cpp
// Disjoint set union (union-find) with path compression and union by size, a naive version
// for comparison, Kruskal's minimum spanning tree, Prim's MST as a cross-check, redundant
// connection, and accounts merge (group records that share an identifier: the "same
// customer under two account numbers" problem).
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m13_union_find.cpp -o m13_union_find

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

class DSU {
public:
    explicit DSU(int n) : parent_(n), size_(n, 1), sets_(n) {
        std::iota(parent_.begin(), parent_.end(), 0);           // everyone is their own root
    }
    // Iterative find with full path compression: two passes, no recursion depth risk.
    int find(int x) {
        int root = x;
        while (parent_[root] != root) root = parent_[root];
        while (parent_[x] != root) {
            int next = parent_[x];
            parent_[x] = root;
            x = next;
        }
        return root;
    }
    // Returns false if a and b were already in the same set (the edge would close a cycle).
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (size_[a] < size_[b]) std::swap(a, b);               // hang the smaller tree under the bigger
        parent_[b] = a;
        size_[a] += size_[b];
        --sets_;
        return true;
    }
    bool same(int a, int b) { return find(a) == find(b); }
    int set_size(int x) { return size_[find(x)]; }
    int sets() const { return sets_; }

private:
    std::vector<int> parent_;
    std::vector<int> size_;
    int sets_;
};

// No compression, no balancing: find can be O(n). Kept for the depth comparison.
struct NaiveDSU {
    std::vector<int> parent;
    explicit NaiveDSU(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x, int& steps) {
        while (parent[x] != x) { x = parent[x]; ++steps; }
        return x;
    }
};

// ---- Kruskal: sort edges by weight, take each edge that joins two different components -----

struct WEdge {
    int u, v;
    std::int64_t w;
};

std::int64_t kruskal(int n, std::vector<WEdge> edges, std::vector<WEdge>* chosen = nullptr) {
    std::sort(edges.begin(), edges.end(), [](const WEdge& a, const WEdge& b) { return a.w < b.w; });
    DSU dsu(n);
    std::int64_t total = 0;
    for (const WEdge& e : edges) {
        if (!dsu.unite(e.u, e.v)) continue;                     // would form a cycle
        total += e.w;
        if (chosen) chosen->push_back(e);
    }
    return dsu.sets() == 1 ? total : -1;                        // -1: graph not connected
}

// Prim with a heap, for cross-checking Kruskal on random graphs.
std::int64_t prim(int n, const std::vector<WEdge>& edges) {
    std::vector<std::vector<std::pair<int, std::int64_t>>> g(n);
    for (const WEdge& e : edges) {
        g[e.u].push_back({e.v, e.w});
        g[e.v].push_back({e.u, e.w});
    }
    std::vector<char> in(n, 0);
    using Item = std::pair<std::int64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.push({0, 0});
    std::int64_t total = 0;
    int taken = 0;
    while (!pq.empty()) {
        auto [w, u] = pq.top();
        pq.pop();
        if (in[u]) continue;
        in[u] = 1;
        total += w;
        ++taken;
        for (auto [v, wv] : g[u])
            if (!in[v]) pq.push({wv, v});
    }
    return taken == n ? total : -1;
}

// ---- redundant connection: the edge that, added last, closes the first cycle ---------------

std::pair<int, int> redundant_connection(int n, const std::vector<std::pair<int, int>>& edges) {
    DSU dsu(n + 1);                                              // vertices are 1..n
    for (auto [u, v] : edges)
        if (!dsu.unite(u, v)) return {u, v};
    return {-1, -1};
}

// ---- accounts merge --------------------------------------------------------------------------
// Each account is {name, id1, id2, ...}. Two accounts belong to the same person if they share
// any id. Output: one entry per person, ids sorted.
std::vector<std::vector<std::string>> accounts_merge(const std::vector<std::vector<std::string>>& accounts) {
    int n = static_cast<int>(accounts.size());
    DSU dsu(n);
    std::unordered_map<std::string, int> owner;                  // id -> first account that had it
    for (int i = 0; i < n; ++i)
        for (std::size_t j = 1; j < accounts[i].size(); ++j) {
            auto [it, inserted] = owner.emplace(accounts[i][j], i);
            if (!inserted) dsu.unite(i, it->second);
        }
    std::map<int, std::vector<std::string>> groups;              // root -> ids (std::map: stable output order)
    for (const auto& [id, acc] : owner) groups[dsu.find(acc)].push_back(id);
    std::vector<std::vector<std::string>> out;
    for (auto& [root, ids] : groups) {
        std::sort(ids.begin(), ids.end());
        std::vector<std::string> row{accounts[root][0]};
        row.insert(row.end(), ids.begin(), ids.end());
        out.push_back(std::move(row));
    }
    return out;
}

int main() {
    DSU d(6);
    assert(d.sets() == 6);
    assert(d.unite(0, 1) && d.unite(2, 3) && d.unite(1, 3));
    assert(!d.unite(0, 2));                                      // already connected
    assert(d.same(0, 3) && !d.same(0, 4));
    assert(d.set_size(2) == 4 && d.sets() == 3);

    // Depth: chain unions 0<-1<-2<-... in the naive version give an O(n) find.
    {
        const int n = 10000;
        NaiveDSU nd(n);
        for (int i = 1; i < n; ++i) nd.parent[i - 1] = i;        // worst-case chain
        int steps = 0;
        nd.find(0, steps);
        assert(steps == n - 1);
        DSU good(n);
        for (int i = 1; i < n; ++i) good.unite(i - 1, i);
        // With union by size, every tree has height <= log2(n) even before compression.
        assert(good.set_size(0) == n && good.sets() == 1);
    }

    // Kruskal on a small graph: MST weight 1 + 2 + 3 = 6.
    std::vector<WEdge> e = {{0, 1, 1}, {1, 2, 2}, {0, 2, 4}, {2, 3, 3}, {1, 3, 5}};
    std::vector<WEdge> chosen;
    assert(kruskal(4, e, &chosen) == 6 && chosen.size() == 3);
    assert(prim(4, e) == 6);
    assert(kruskal(4, {{0, 1, 1}}) == -1);                      // disconnected

    std::mt19937 rng(3);
    for (int trial = 0; trial < 300; ++trial) {
        int n = 1 + static_cast<int>(rng() % 40);
        std::vector<WEdge> r;
        for (int u = 0; u < n; ++u)
            for (int v = u + 1; v < n; ++v)
                if (rng() % 3 == 0) r.push_back({u, v, static_cast<std::int64_t>(rng() % 50)});
        assert(kruskal(n, r) == prim(n, r));
    }

    assert((redundant_connection(3, {{1, 2}, {1, 3}, {2, 3}}) == std::pair<int, int>{2, 3}));
    assert((redundant_connection(5, {{1, 2}, {2, 3}, {3, 4}, {1, 4}, {1, 5}}) == std::pair<int, int>{1, 4}));

    auto merged = accounts_merge({{"Asha", "A1", "A2"},
                                  {"Asha", "A3"},
                                  {"Asha", "A2", "A4"},
                                  {"Ravi", "R1"}});
    assert(merged.size() == 3);
    bool found = false;
    for (const auto& row : merged)
        if (row == std::vector<std::string>{"Asha", "A1", "A2", "A4"}) found = true;
    assert(found);

    std::printf("all union-find checks passed\n");
    return 0;
}
