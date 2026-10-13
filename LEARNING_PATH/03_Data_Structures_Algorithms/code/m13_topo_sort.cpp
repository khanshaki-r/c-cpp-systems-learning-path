// m13_topo_sort.cpp
// Topological sort two ways (Kahn's in-degree BFS and DFS post-order), cycle reporting,
// a validator, "course schedule", build order for named targets (like make or a migration
// dependency graph), and alien dictionary (derive an order from sorted words).
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m13_topo_sort.cpp -o m13_topo_sort

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Graph = std::vector<std::vector<int>>;

// Kahn: repeatedly take a vertex with no remaining incoming edges. If fewer than n vertices
// come out, the rest are on (or behind) a cycle. O(V + E).
std::vector<int> topo_kahn(const Graph& g) {
    int n = static_cast<int>(g.size());
    std::vector<int> indeg(n, 0);
    for (const auto& adj : g)
        for (int v : adj) ++indeg[v];
    std::queue<int> q;
    for (int u = 0; u < n; ++u)
        if (indeg[u] == 0) q.push(u);
    std::vector<int> order;
    order.reserve(n);
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (int v : g[u])
            if (--indeg[v] == 0) q.push(v);
    }
    if (static_cast<int>(order.size()) != n) return {};   // cycle: no valid order
    return order;
}

// Kahn with a min-heap instead of a FIFO: the lexicographically smallest valid order.
// O((V + E) log V). Useful when the output must be deterministic (build logs, tests).
std::vector<int> topo_kahn_smallest(const Graph& g) {
    int n = static_cast<int>(g.size());
    std::vector<int> indeg(n, 0);
    for (const auto& adj : g)
        for (int v : adj) ++indeg[v];
    std::priority_queue<int, std::vector<int>, std::greater<int>> q;
    for (int u = 0; u < n; ++u)
        if (indeg[u] == 0) q.push(u);
    std::vector<int> order;
    while (!q.empty()) {
        int u = q.top();
        q.pop();
        order.push_back(u);
        for (int v : g[u])
            if (--indeg[v] == 0) q.push(v);
    }
    if (static_cast<int>(order.size()) != n) return {};
    return order;
}

// DFS: a vertex is appended AFTER all its descendants (post-order); reverse at the end.
// Grey-on-stack detection doubles as cycle detection.
bool topo_dfs_visit(const Graph& g, int u, std::vector<int>& colour, std::vector<int>& post) {
    colour[u] = 1;
    for (int v : g[u]) {
        if (colour[v] == 1) return false;                   // back edge
        if (colour[v] == 0 && !topo_dfs_visit(g, v, colour, post)) return false;
    }
    colour[u] = 2;
    post.push_back(u);
    return true;
}

std::vector<int> topo_dfs(const Graph& g) {
    std::vector<int> colour(g.size(), 0), post;
    for (std::size_t u = 0; u < g.size(); ++u)
        if (colour[u] == 0 && !topo_dfs_visit(g, static_cast<int>(u), colour, post)) return {};
    std::reverse(post.begin(), post.end());
    return post;
}

bool is_valid_topo(const Graph& g, const std::vector<int>& order) {
    if (order.size() != g.size()) return false;
    std::vector<int> pos(g.size(), -1);
    for (std::size_t i = 0; i < order.size(); ++i) pos[order[i]] = static_cast<int>(i);
    for (std::size_t u = 0; u < g.size(); ++u)
        for (int v : g[u])
            if (pos[u] >= pos[v]) return false;              // every edge must point forward
    return true;
}

// Course schedule: prereqs[i] = {course, needs}. Edge needs -> course.
bool can_finish(int n, const std::vector<std::pair<int, int>>& prereqs) {
    Graph g(n);
    for (auto [course, needs] : prereqs) g[needs].push_back(course);
    return !topo_kahn(g).empty() || n == 0;
}

// Build order for named targets: deps maps target -> things it depends on.
std::vector<std::string> build_order(const std::unordered_map<std::string, std::vector<std::string>>& deps) {
    std::unordered_map<std::string, int> id;
    std::vector<std::string> name;
    auto get = [&](const std::string& s) {
        auto it = id.find(s);
        if (it != id.end()) return it->second;
        id.emplace(s, static_cast<int>(name.size()));
        name.push_back(s);
        return static_cast<int>(name.size()) - 1;
    };
    std::vector<std::pair<int, int>> edges;
    for (const auto& [target, list] : deps) {
        int t = get(target);
        for (const auto& d : list) edges.push_back({get(d), t});   // dependency first
    }
    Graph g(name.size());
    for (auto [u, v] : edges) g[u].push_back(v);
    std::vector<std::string> out;
    for (int u : topo_kahn(g)) out.push_back(name[u]);
    return out;
}

// Alien dictionary: words are sorted in an unknown alphabet; recover one valid letter order.
// Only the FIRST differing letter of each adjacent pair gives an edge. A word followed by its
// own proper prefix ("abc" before "ab") is invalid input.
std::string alien_order(const std::vector<std::string>& words) {
    std::vector<char> present(26, 0);
    for (const auto& w : words)
        for (char ch : w) present[ch - 'a'] = 1;
    Graph g(26);
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        const std::string& a = words[i];
        const std::string& b = words[i + 1];
        std::size_t k = 0;
        while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
        if (k == a.size() || k == b.size()) {
            if (a.size() > b.size()) return "";             // "abc" before "ab"
            continue;
        }
        g[a[k] - 'a'].push_back(b[k] - 'a');
    }
    // Run Kahn only over letters that appear.
    std::vector<int> indeg(26, 0);
    for (int u = 0; u < 26; ++u)
        for (int v : g[u]) ++indeg[v];
    std::queue<int> q;
    int letters = 0;
    for (int u = 0; u < 26; ++u)
        if (present[u]) { ++letters; if (indeg[u] == 0) q.push(u); }
    std::string out;
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        out.push_back(static_cast<char>('a' + u));
        for (int v : g[u])
            if (--indeg[v] == 0) q.push(v);
    }
    return static_cast<int>(out.size()) == letters ? out : "";
}

int main() {
    // 5 -> 2 -> 3 -> 1,  5 -> 0,  4 -> 0,  4 -> 1   (the classic CLRS-style example)
    Graph g(6);
    g[5] = {2, 0};
    g[4] = {0, 1};
    g[2] = {3};
    g[3] = {1};
    auto k = topo_kahn(g);
    auto d = topo_dfs(g);
    assert(is_valid_topo(g, k));
    assert(is_valid_topo(g, d));
    assert((topo_kahn_smallest(g) == std::vector<int>{4, 5, 0, 2, 3, 1}));

    Graph cyc(3);
    cyc[0] = {1};
    cyc[1] = {2};
    cyc[2] = {0};
    assert(topo_kahn(cyc).empty() && topo_dfs(cyc).empty());

    assert(can_finish(2, {{1, 0}}));
    assert(!can_finish(2, {{1, 0}, {0, 1}}));

    auto order = build_order({{"app", {"libnet", "libdb"}}, {"libdb", {"libcore"}}, {"libnet", {"libcore"}}});
    assert(order.size() == 4 && order.front() == "libcore" && order.back() == "app");

    assert(alien_order({"wrt", "wrf", "er", "ett", "rftt"}) == "wertf");
    assert(alien_order({"z", "x", "z"}).empty());           // z<x and x<z: cycle
    assert(alien_order({"abc", "ab"}).empty());             // prefix rule broken

    // Random DAGs: edges only from lower to higher id after a random relabelling, so a valid
    // order always exists; both algorithms must produce a valid one.
    std::mt19937 rng(42);
    for (int trial = 0; trial < 200; ++trial) {
        int n = 1 + static_cast<int>(rng() % 30);
        std::vector<int> label(n);
        for (int i = 0; i < n; ++i) label[i] = i;
        std::shuffle(label.begin(), label.end(), rng);
        Graph r(n);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (rng() % 4 == 0) r[label[i]].push_back(label[j]);
        assert(is_valid_topo(r, topo_kahn(r)));
        assert(is_valid_topo(r, topo_dfs(r)));
    }

    std::printf("all topological sort checks passed\n");
    return 0;
}
