// m13_graph_problems.cpp
// The eight graph problems from module 13, each a self-contained function with asserts:
//   1. Number of islands            (grid DFS / flood fill)
//   2. Rotting oranges              (multi-source BFS)
//   3. Course schedule II           (Kahn's topological sort)
//   4. Clone graph                  (BFS + old->new hash map)
//   5. Word ladder                  (BFS on an implicit graph)
//   6. Network delay time           (Dijkstra)
//   7. Number of provinces          (union-find)
//   8. Is graph bipartite           (BFS 2-colouring)
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m13_graph_problems.cpp -o m13_graph_problems

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---- 1. Number of islands ------------------------------------------------------------------
// Count 4-connected groups of '1'. Sink each island as you find it so it is not counted twice.
// Uses an explicit stack: a recursive flood fill on a 2000x2000 all-land grid would recurse
// 4 million deep and blow an 8 MB thread stack.
int num_islands(std::vector<std::string> grid) {
    int R = static_cast<int>(grid.size());
    if (R == 0) return 0;
    int C = static_cast<int>(grid[0].size());
    int count = 0;
    std::vector<std::pair<int, int>> st;
    for (int r = 0; r < R; ++r)
        for (int c = 0; c < C; ++c) {
            if (grid[r][c] != '1') continue;
            ++count;
            grid[r][c] = '0';
            st.push_back({r, c});
            while (!st.empty()) {
                auto [y, x] = st.back();
                st.pop_back();
                const int dy[4] = {1, -1, 0, 0};
                const int dx[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int ny = y + dy[k], nx = x + dx[k];
                    if (ny < 0 || ny >= R || nx < 0 || nx >= C || grid[ny][nx] != '1') continue;
                    grid[ny][nx] = '0';                       // mark on push, not on pop
                    st.push_back({ny, nx});
                }
            }
        }
    return count;
}

// ---- 2. Rotting oranges -------------------------------------------------------------------
// 0 empty, 1 fresh, 2 rotten. Each minute rot spreads to 4-neighbours. Minutes until no fresh
// remain, or -1. All rotten cells start in the queue together: BFS layers = minutes.
int oranges_rotting(std::vector<std::vector<int>> grid) {
    int R = static_cast<int>(grid.size());
    int C = static_cast<int>(grid[0].size());
    std::queue<std::pair<int, int>> q;
    int fresh = 0;
    for (int r = 0; r < R; ++r)
        for (int c = 0; c < C; ++c) {
            if (grid[r][c] == 2) q.push({r, c});
            else if (grid[r][c] == 1) ++fresh;
        }
    int minutes = 0;
    const int dr[4] = {1, -1, 0, 0};
    const int dc[4] = {0, 0, 1, -1};
    while (!q.empty() && fresh > 0) {
        for (std::size_t layer = q.size(); layer > 0; --layer) {   // process exactly one minute
            auto [r, c] = q.front();
            q.pop();
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k], nc = c + dc[k];
                if (nr < 0 || nr >= R || nc < 0 || nc >= C || grid[nr][nc] != 1) continue;
                grid[nr][nc] = 2;
                --fresh;
                q.push({nr, nc});
            }
        }
        ++minutes;
    }
    return fresh == 0 ? minutes : -1;
}

// ---- 3. Course schedule II ----------------------------------------------------------------
// prereqs[i] = {a, b} means "to take a you must first take b". Return an order or {}.
std::vector<int> find_order(int n, const std::vector<std::pair<int, int>>& prereqs) {
    std::vector<std::vector<int>> g(n);
    std::vector<int> indeg(n, 0);
    for (auto [a, b] : prereqs) {
        g[b].push_back(a);
        ++indeg[a];
    }
    std::queue<int> q;
    for (int i = 0; i < n; ++i)
        if (indeg[i] == 0) q.push(i);
    std::vector<int> order;
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (int v : g[u])
            if (--indeg[v] == 0) q.push(v);
    }
    if (static_cast<int>(order.size()) != n) return {};
    return order;
}

// ---- 4. Clone graph -----------------------------------------------------------------------
// Nodes are owned by a pool (unique_ptr), neighbours are non-owning raw pointers: a graph with
// cycles cannot be expressed with unique_ptr ownership edges.
struct GNode {
    int val;
    std::vector<GNode*> nbrs;
};
struct GraphPool {
    std::vector<std::unique_ptr<GNode>> nodes;
    GNode* make(int v) {
        nodes.push_back(std::make_unique<GNode>(GNode{v, {}}));
        return nodes.back().get();
    }
};

GNode* clone_graph(GNode* start, GraphPool& pool) {
    if (!start) return nullptr;
    std::unordered_map<GNode*, GNode*> copy;                     // old -> new; also the "seen" set
    std::queue<GNode*> q;
    copy[start] = pool.make(start->val);
    q.push(start);
    while (!q.empty()) {
        GNode* u = q.front();
        q.pop();
        for (GNode* v : u->nbrs) {
            auto it = copy.find(v);
            if (it == copy.end()) {
                it = copy.emplace(v, pool.make(v->val)).first;
                q.push(v);
            }
            copy[u]->nbrs.push_back(it->second);
        }
    }
    return copy[start];
}

// ---- 5. Word ladder -----------------------------------------------------------------------
// Fewest words in a transformation begin -> end changing one letter at a time, every
// intermediate word in the dictionary. Neighbours are generated (26 * L candidates), not stored.
int ladder_length(const std::string& begin, const std::string& end, const std::vector<std::string>& words) {
    std::unordered_set<std::string> dict(words.begin(), words.end());
    if (!dict.count(end)) return 0;
    std::queue<std::string> q;
    q.push(begin);
    dict.erase(begin);                                           // erase = mark visited
    int steps = 1;
    while (!q.empty()) {
        for (std::size_t layer = q.size(); layer > 0; --layer) {
            std::string w = q.front();
            q.pop();
            if (w == end) return steps;
            for (std::size_t i = 0; i < w.size(); ++i) {
                char orig = w[i];
                for (char ch = 'a'; ch <= 'z'; ++ch) {
                    if (ch == orig) continue;
                    w[i] = ch;
                    auto it = dict.find(w);
                    if (it != dict.end()) {
                        q.push(w);
                        dict.erase(it);
                    }
                }
                w[i] = orig;
            }
        }
        ++steps;
    }
    return 0;
}

// ---- 6. Network delay time ----------------------------------------------------------------
// times[i] = {u, v, w}: a signal from u reaches v after w. Nodes 1..n. Time for a signal from k
// to reach all nodes = max over shortest distances, or -1 if some node is unreachable.
int network_delay(const std::vector<std::vector<int>>& times, int n, int k) {
    std::vector<std::vector<std::pair<int, int>>> g(n + 1);
    for (const auto& t : times) g[t[0]].push_back({t[1], t[2]});
    const int INF = std::numeric_limits<int>::max();
    std::vector<int> dist(n + 1, INF);
    using Item = std::pair<int, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    dist[k] = 0;
    pq.push({0, k});
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d != dist[u]) continue;
        for (auto [v, w] : g[u])
            if (d + w < dist[v]) { dist[v] = d + w; pq.push({dist[v], v}); }
    }
    int best = 0;
    for (int v = 1; v <= n; ++v) {
        if (dist[v] == INF) return -1;
        best = std::max(best, dist[v]);
    }
    return best;
}

// ---- 7. Number of provinces (connected components via union-find) --------------------------
int find_circle_num(const std::vector<std::vector<int>>& adj) {
    int n = static_cast<int>(adj.size());
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        return parent[x] == x ? x : parent[x] = find(parent[x]);   // recursive compression, depth is small here
    };
    int sets = n;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (adj[i][j]) {
                int a = find(i), b = find(j);
                if (a != b) { parent[a] = b; --sets; }
            }
    return sets;
}

// ---- 8. Is graph bipartite ----------------------------------------------------------------
bool is_bipartite(const std::vector<std::vector<int>>& g) {
    std::vector<int> colour(g.size(), -1);
    for (std::size_t s = 0; s < g.size(); ++s) {
        if (colour[s] != -1) continue;                           // graph may be disconnected
        colour[s] = 0;
        std::queue<int> q;
        q.push(static_cast<int>(s));
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : g[u]) {
                if (colour[v] == -1) { colour[v] = colour[u] ^ 1; q.push(v); }
                else if (colour[v] == colour[u]) return false;
            }
        }
    }
    return true;
}

int main() {
    assert(num_islands({"11110", "11010", "11000", "00000"}) == 1);
    assert(num_islands({"11000", "11000", "00100", "00011"}) == 3);
    assert(num_islands({std::string(300, '1')}) == 1);

    assert(oranges_rotting({{2, 1, 1}, {1, 1, 0}, {0, 1, 1}}) == 4);
    assert(oranges_rotting({{2, 1, 1}, {0, 1, 1}, {1, 0, 1}}) == -1);
    assert(oranges_rotting({{0, 2}}) == 0);

    {
        auto o = find_order(4, {{1, 0}, {2, 0}, {3, 1}, {3, 2}});
        assert(o.size() == 4 && o.front() == 0 && o.back() == 3);
        assert(find_order(2, {{1, 0}, {0, 1}}).empty());
    }

    {
        // 1 - 2
        // |   |
        // 4 - 3
        GraphPool src, dst;
        GNode* a = src.make(1);
        GNode* b = src.make(2);
        GNode* c = src.make(3);
        GNode* d = src.make(4);
        a->nbrs = {b, d};
        b->nbrs = {a, c};
        c->nbrs = {b, d};
        d->nbrs = {a, c};
        GNode* ca = clone_graph(a, dst);
        assert(ca != a && ca->val == 1 && dst.nodes.size() == 4);
        assert(ca->nbrs.size() == 2 && ca->nbrs[0]->val == 2 && ca->nbrs[1]->val == 4);
        assert(ca->nbrs[0]->nbrs[0] == ca);                     // the cycle is preserved, in the copy
        for (const auto& n : dst.nodes)
            for (GNode* x : n->nbrs)
                assert(x != a && x != b && x != c && x != d);    // no pointer leaks into the original
    }

    assert(ladder_length("hit", "cog", {"hot", "dot", "dog", "lot", "log", "cog"}) == 5);
    assert(ladder_length("hit", "cog", {"hot", "dot", "dog", "lot", "log"}) == 0);

    assert(network_delay({{2, 1, 1}, {2, 3, 1}, {3, 4, 1}}, 4, 2) == 2);
    assert(network_delay({{1, 2, 1}}, 2, 2) == -1);

    assert(find_circle_num({{1, 1, 0}, {1, 1, 0}, {0, 0, 1}}) == 2);
    assert(find_circle_num({{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}) == 3);

    assert(!is_bipartite({{1, 2, 3}, {0, 2}, {0, 1, 3}, {0, 2}}));
    assert(is_bipartite({{1, 3}, {0, 2}, {1, 3}, {0, 2}}));

    std::printf("all 8 graph problems passed\n");
    return 0;
}
