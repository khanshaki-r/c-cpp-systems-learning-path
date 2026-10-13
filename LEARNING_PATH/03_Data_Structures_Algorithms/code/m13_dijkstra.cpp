// m13_dijkstra.cpp
// Weighted shortest paths: Dijkstra with a binary heap and lazy deletion, path
// reconstruction, the O(V^2) array version for dense graphs, 0-1 BFS, Bellman-Ford (handles
// negative edges, detects negative cycles), and a differential test of all of them against
// Floyd-Warshall on random graphs.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m13_dijkstra.cpp -o m13_dijkstra

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <utility>
#include <vector>

struct Edge {
    int to;
    std::int64_t w;
};
using WGraph = std::vector<std::vector<Edge>>;

constexpr std::int64_t kInf = std::numeric_limits<std::int64_t>::max() / 4;   // headroom for INF + w

// Dijkstra, lazy deletion: push a new (dist, v) whenever dist[v] improves; when an entry is
// popped whose dist is larger than the best known, it is stale and skipped.
// O((V + E) log E) time, heap holds up to E entries. Requires w >= 0.
std::vector<std::int64_t> dijkstra(const WGraph& g, int src, std::vector<int>* parent = nullptr) {
    std::vector<std::int64_t> dist(g.size(), kInf);
    if (parent) parent->assign(g.size(), -1);
    using Item = std::pair<std::int64_t, int>;                       // (distance, vertex)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    dist[src] = 0;
    pq.push({0, src});
    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();
        if (du != dist[u]) continue;                                 // stale entry
        for (const Edge& e : g[u]) {
            std::int64_t nd = du + e.w;
            if (nd < dist[e.to]) {                                   // relax
                dist[e.to] = nd;
                if (parent) (*parent)[e.to] = u;
                pq.push({nd, e.to});
            }
        }
    }
    return dist;
}

std::vector<int> shortest_path(const WGraph& g, int src, int dst) {
    std::vector<int> parent;
    auto dist = dijkstra(g, src, &parent);
    if (dist[dst] >= kInf) return {};
    std::vector<int> path;
    for (int v = dst; v != -1; v = parent[v]) path.push_back(v);
    std::reverse(path.begin(), path.end());
    return path;
}

// O(V^2) Dijkstra without a heap: better when E is close to V^2 (dense graphs).
std::vector<std::int64_t> dijkstra_dense(const WGraph& g, int src) {
    std::size_t n = g.size();
    std::vector<std::int64_t> dist(n, kInf);
    std::vector<char> done(n, 0);
    dist[src] = 0;
    for (std::size_t iter = 0; iter < n; ++iter) {
        int u = -1;
        for (std::size_t v = 0; v < n; ++v)
            if (!done[v] && (u == -1 || dist[v] < dist[u])) u = static_cast<int>(v);
        if (u == -1 || dist[u] >= kInf) break;
        done[u] = 1;
        for (const Edge& e : g[u]) dist[e.to] = std::min(dist[e.to], dist[u] + e.w);
    }
    return dist;
}

// 0-1 BFS: weights are only 0 or 1. A deque replaces the heap: 0-edges go to the front,
// 1-edges to the back. O(V + E).
std::vector<std::int64_t> zero_one_bfs(const WGraph& g, int src) {
    std::vector<std::int64_t> dist(g.size(), kInf);
    std::deque<int> dq;
    dist[src] = 0;
    dq.push_back(src);
    while (!dq.empty()) {
        int u = dq.front();
        dq.pop_front();
        for (const Edge& e : g[u]) {
            std::int64_t nd = dist[u] + e.w;
            if (nd < dist[e.to]) {
                dist[e.to] = nd;
                if (e.w == 0) dq.push_front(e.to);
                else dq.push_back(e.to);
            }
        }
    }
    return dist;
}

// Bellman-Ford: relax every edge V-1 times. O(V * E). Works with negative weights.
// Returns false if a negative cycle is reachable from src.
bool bellman_ford(const WGraph& g, int src, std::vector<std::int64_t>& dist) {
    std::size_t n = g.size();
    dist.assign(n, kInf);
    dist[src] = 0;
    for (std::size_t round = 0; round + 1 < n; ++round) {
        bool changed = false;
        for (std::size_t u = 0; u < n; ++u) {
            if (dist[u] >= kInf) continue;
            for (const Edge& e : g[u])
                if (dist[u] + e.w < dist[e.to]) { dist[e.to] = dist[u] + e.w; changed = true; }
        }
        if (!changed) return true;                                   // early exit
    }
    for (std::size_t u = 0; u < n; ++u) {
        if (dist[u] >= kInf) continue;
        for (const Edge& e : g[u])
            if (dist[u] + e.w < dist[e.to]) return false;            // still improving: cycle
    }
    return true;
}

// Floyd-Warshall: all pairs, O(V^3). Used here only as the reference answer.
std::vector<std::vector<std::int64_t>> floyd(const WGraph& g) {
    std::size_t n = g.size();
    std::vector<std::vector<std::int64_t>> d(n, std::vector<std::int64_t>(n, kInf));
    for (std::size_t u = 0; u < n; ++u) {
        d[u][u] = 0;
        for (const Edge& e : g[u]) d[u][e.to] = std::min(d[u][e.to], e.w);
    }
    for (std::size_t k = 0; k < n; ++k)
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
                if (d[i][k] < kInf && d[k][j] < kInf) d[i][j] = std::min(d[i][j], d[i][k] + d[k][j]);
    return d;
}

int main() {
    //        1        2
    //   0 ------> 1 ------> 3
    //   |         ^         ^
    //  4|        1|         | 5
    //   v         |         |
    //   2 --------+---------+
    WGraph g(4);
    g[0] = {{1, 1}, {2, 4}};
    g[1] = {{3, 2}};
    g[2] = {{1, 1}, {3, 5}};
    auto d = dijkstra(g, 0);
    assert((d == std::vector<std::int64_t>{0, 1, 4, 3}));
    assert((shortest_path(g, 0, 3) == std::vector<int>{0, 1, 3}));
    assert(shortest_path(g, 3, 0).empty());
    assert(dijkstra_dense(g, 0) == d);

    // Why Dijkstra fails with a negative edge: 0->1 (2), 0->2 (5), 2->1 (-4). Dijkstra finalises
    // 1 at distance 2 before it sees the cheaper route 0->2->1 = 1.
    WGraph neg(3);
    neg[0] = {{1, 2}, {2, 5}};
    neg[2] = {{1, -4}};
    std::vector<std::int64_t> bf;
    assert(bellman_ford(neg, 0, bf));
    assert(bf[1] == 1);
    // (The lazy-deletion version above actually re-relaxes and happens to get 1 here, because
    // it never marks a vertex final; with a node that has outgoing edges after the bad one it
    // can still be wrong, and it loses its complexity bound. Do not rely on it.)

    WGraph negcyc(3);
    negcyc[0] = {{1, 1}};
    negcyc[1] = {{2, -2}};
    negcyc[2] = {{1, 1}};
    assert(!bellman_ford(negcyc, 0, bf));

    std::mt19937 rng(7);
    for (int trial = 0; trial < 300; ++trial) {
        int n = 1 + static_cast<int>(rng() % 25);
        WGraph r(n), r01(n);
        for (int u = 0; u < n; ++u)
            for (int v = 0; v < n; ++v)
                if (u != v && rng() % 3 == 0) {
                    r[u].push_back({v, static_cast<std::int64_t>(rng() % 100)});
                    r01[u].push_back({v, static_cast<std::int64_t>(rng() % 2)});
                }
        auto ref = floyd(r);
        auto ref01 = floyd(r01);
        for (int s = 0; s < n; ++s) {
            assert(dijkstra(r, s) == ref[s]);
            assert(dijkstra_dense(r, s) == ref[s]);
            std::vector<std::int64_t> b;
            assert(bellman_ford(r, s, b) && b == ref[s]);
            assert(zero_one_bfs(r01, s) == ref01[s]);
        }
    }

    std::printf("all shortest-path checks passed\n");
    return 0;
}
