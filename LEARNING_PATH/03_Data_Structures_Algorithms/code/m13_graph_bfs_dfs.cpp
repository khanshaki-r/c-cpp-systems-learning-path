// m13_graph_bfs_dfs.cpp
// Graph representations (edge list, adjacency matrix, adjacency list, CSR), BFS with
// distances and path reconstruction, DFS recursive and iterative, connected components,
// cycle detection (undirected with parent, directed with three colours), bipartite check,
// and BFS on a grid. Every function is checked with assert in main().
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m13_graph_bfs_dfs.cpp -o m13_graph_bfs_dfs

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <queue>
#include <stack>
#include <string>
#include <utility>
#include <vector>

using Graph = std::vector<std::vector<int>>;   // adjacency list: g[u] = neighbours of u

// ---- representations -----------------------------------------------------------------------

Graph from_edges(int n, const std::vector<std::pair<int, int>>& edges, bool directed) {
    Graph g(n);
    for (auto [u, v] : edges) {
        g[u].push_back(v);
        if (!directed) g[v].push_back(u);      // undirected = two directed arcs
    }
    return g;
}

std::vector<std::vector<char>> to_matrix(const Graph& g) {
    int n = static_cast<int>(g.size());
    std::vector<std::vector<char>> m(n, std::vector<char>(n, 0));
    for (int u = 0; u < n; ++u)
        for (int v : g[u]) m[u][v] = 1;        // O(1) edge test, O(V^2) memory
    return m;
}

// CSR (compressed sparse row): one offsets array and one flat targets array.
// Neighbours of u are targets[offsets[u] .. offsets[u+1]). Two allocations total, and a
// neighbour scan is a contiguous read: this is what graph libraries use for big static graphs.
struct Csr {
    std::vector<int> offsets;
    std::vector<int> targets;
};

Csr to_csr(const Graph& g) {
    Csr c;
    c.offsets.assign(g.size() + 1, 0);
    for (std::size_t u = 0; u < g.size(); ++u)
        c.offsets[u + 1] = c.offsets[u] + static_cast<int>(g[u].size());
    c.targets.reserve(static_cast<std::size_t>(c.offsets.back()));
    for (const auto& adj : g) c.targets.insert(c.targets.end(), adj.begin(), adj.end());
    return c;
}

// ---- BFS -----------------------------------------------------------------------------------

// Shortest path lengths (in edges) from src; -1 means unreachable. O(V + E).
std::vector<int> bfs_dist(const Graph& g, int src, std::vector<int>* parent = nullptr) {
    std::vector<int> dist(g.size(), -1);
    if (parent) parent->assign(g.size(), -1);
    std::queue<int> q;
    dist[src] = 0;                             // mark when ENQUEUED, not when popped
    q.push(src);
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        for (int v : g[u]) {
            if (dist[v] != -1) continue;
            dist[v] = dist[u] + 1;
            if (parent) (*parent)[v] = u;
            q.push(v);
        }
    }
    return dist;
}

std::vector<int> bfs_path(const Graph& g, int src, int dst) {
    std::vector<int> parent;
    std::vector<int> dist = bfs_dist(g, src, &parent);
    if (dist[dst] == -1) return {};
    std::vector<int> path;
    for (int v = dst; v != -1; v = parent[v]) path.push_back(v);
    std::reverse(path.begin(), path.end());
    return path;
}

// ---- DFS -----------------------------------------------------------------------------------

void dfs_rec(const Graph& g, int u, std::vector<char>& seen, std::vector<int>& order) {
    seen[u] = 1;
    order.push_back(u);
    for (int v : g[u])
        if (!seen[v]) dfs_rec(g, v, seen, order);
}

// Iterative DFS that visits in the SAME order as the recursive one. The trick: keep
// (node, next-neighbour-index) frames, exactly like the call stack would.
std::vector<int> dfs_iter(const Graph& g, int src) {
    std::vector<char> seen(g.size(), 0);
    std::vector<int> order;
    std::vector<std::pair<int, std::size_t>> st;
    seen[src] = 1;
    order.push_back(src);
    st.push_back({src, 0});
    while (!st.empty()) {
        auto& [u, i] = st.back();
        if (i == g[u].size()) { st.pop_back(); continue; }
        int v = g[u][i++];                     // advance this frame before descending
        if (seen[v]) continue;
        seen[v] = 1;
        order.push_back(v);
        st.push_back({v, 0});                  // may invalidate u and i; we do not use them again
    }
    return order;
}

// ---- connected components (undirected) ---------------------------------------------------

int components(const Graph& g, std::vector<int>& comp) {
    int n = static_cast<int>(g.size());
    comp.assign(n, -1);
    int c = 0;
    for (int s = 0; s < n; ++s) {
        if (comp[s] != -1) continue;
        std::queue<int> q;
        q.push(s);
        comp[s] = c;
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : g[u])
                if (comp[v] == -1) { comp[v] = c; q.push(v); }
        }
        ++c;
    }
    return c;
}

// ---- cycle detection -----------------------------------------------------------------------

// Undirected: a cycle exists if DFS meets an already-seen vertex that is not the parent.
// (Assumes no parallel edges; with parallel edges, skip the parent edge by edge id instead.)
bool has_cycle_undirected(const Graph& g) {
    int n = static_cast<int>(g.size());
    std::vector<int> parent(n, -2);            // -2 = unvisited
    for (int s = 0; s < n; ++s) {
        if (parent[s] != -2) continue;
        std::stack<int> st;
        st.push(s);
        parent[s] = -1;
        while (!st.empty()) {
            int u = st.top();
            st.pop();
            for (int v : g[u]) {
                if (parent[v] == -2) { parent[v] = u; st.push(v); }
                else if (v != parent[u]) return true;
            }
        }
    }
    return false;
}

// Directed: white (0) = unvisited, grey (1) = on the current DFS path, black (2) = finished.
// A back edge to a GREY vertex is a cycle. A grey/black distinction is required: an edge to a
// black vertex is a cross or forward edge, not a cycle.
bool dfs_colour(const Graph& g, int u, std::vector<int>& colour) {
    colour[u] = 1;
    for (int v : g[u]) {
        if (colour[v] == 1) return true;
        if (colour[v] == 0 && dfs_colour(g, v, colour)) return true;
    }
    colour[u] = 2;
    return false;
}

bool has_cycle_directed(const Graph& g) {
    std::vector<int> colour(g.size(), 0);
    for (std::size_t s = 0; s < g.size(); ++s)
        if (colour[s] == 0 && dfs_colour(g, static_cast<int>(s), colour)) return true;
    return false;
}

// ---- bipartite (2-colouring) ---------------------------------------------------------------

bool is_bipartite(const Graph& g) {
    std::vector<int> side(g.size(), -1);
    for (std::size_t s = 0; s < g.size(); ++s) {
        if (side[s] != -1) continue;
        std::queue<int> q;
        q.push(static_cast<int>(s));
        side[s] = 0;
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : g[u]) {
                if (side[v] == -1) { side[v] = 1 - side[u]; q.push(v); }
                else if (side[v] == side[u]) return false;   // odd cycle
            }
        }
    }
    return true;
}

// ---- grid BFS: shortest path from top-left to bottom-right through '.' cells ---------------

int grid_shortest(const std::vector<std::string>& grid) {
    int R = static_cast<int>(grid.size());
    int C = static_cast<int>(grid[0].size());
    if (grid[0][0] != '.' || grid[R - 1][C - 1] != '.') return -1;
    std::vector<int> dist(R * C, -1);          // flatten (r, c) -> r * C + c
    std::queue<int> q;
    dist[0] = 0;
    q.push(0);
    const int dr[4] = {1, -1, 0, 0};
    const int dc[4] = {0, 0, 1, -1};
    while (!q.empty()) {
        int cell = q.front();
        q.pop();
        int r = cell / C, c = cell % C;
        if (r == R - 1 && c == C - 1) return dist[cell];
        for (int k = 0; k < 4; ++k) {
            int nr = r + dr[k], nc = c + dc[k];
            if (nr < 0 || nr >= R || nc < 0 || nc >= C) continue;
            if (grid[nr][nc] != '.' || dist[nr * C + nc] != -1) continue;
            dist[nr * C + nc] = dist[cell] + 1;
            q.push(nr * C + nc);
        }
    }
    return -1;
}

int main() {
    //   0 - 1 - 2      5 - 6
    //   |   |
    //   3 - 4
    Graph g = from_edges(7, {{0, 1}, {1, 2}, {0, 3}, {1, 4}, {3, 4}, {5, 6}}, false);

    auto m = to_matrix(g);
    assert(m[0][1] == 1 && m[1][0] == 1 && m[0][2] == 0);
    Csr c = to_csr(g);
    assert(c.offsets.size() == 8 && c.targets.size() == 12);   // 6 undirected edges = 12 arcs
    assert(c.offsets[2] - c.offsets[1] == 3);                  // vertex 1 has degree 3

    auto d = bfs_dist(g, 0);
    assert((d == std::vector<int>{0, 1, 2, 1, 2, -1, -1}));
    auto p = bfs_path(g, 3, 2);
    assert(p.size() == 4 && p.front() == 3 && p.back() == 2);  // 3 edges: 3-0-1-2 or 3-4-1-2
    assert(bfs_path(g, 0, 6).empty());

    std::vector<char> seen(g.size(), 0);
    std::vector<int> rec;
    dfs_rec(g, 0, seen, rec);
    assert((rec == std::vector<int>{0, 1, 2, 4, 3}));
    assert(dfs_iter(g, 0) == rec);                              // same order as recursion

    std::vector<int> comp;
    assert(components(g, comp) == 2);
    assert(comp[0] == comp[4] && comp[5] == comp[6] && comp[0] != comp[5]);

    assert(has_cycle_undirected(g));                            // 0-1-4-3-0
    assert(!has_cycle_undirected(from_edges(4, {{0, 1}, {1, 2}, {1, 3}}, false)));

    assert(has_cycle_directed(from_edges(3, {{0, 1}, {1, 2}, {2, 0}}, true)));
    // Diamond 0->1->3, 0->2->3: vertex 3 is reached twice but there is no cycle.
    assert(!has_cycle_directed(from_edges(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}}, true)));

    assert(is_bipartite(g));                                    // the 4-cycle is even
    assert(!is_bipartite(from_edges(3, {{0, 1}, {1, 2}, {2, 0}}, false)));

    assert(grid_shortest({"..#", "#..", "#.."}) == 4);
    assert(grid_shortest({".#", "#."}) == -1);

    std::printf("all graph BFS/DFS checks passed\n");
    return 0;
}
