# 13. Graphs: BFS, DFS and friends

**Time:** ~5 hours   **Prerequisites:** 04, 06, 07, 12

## 1. In plain words

A **graph** is a set of things (vertices, also called nodes) and a set of connections between
them (edges). A road map is a graph: cities are vertices, roads are edges. So is a
dependency list ("libdb needs libcore"), a social network, a set of bank accounts linked by
transfers, or the grid of cells in a maze. Trees (module 10) are a special case: a connected
graph with no cycles.

Almost every graph question comes down to "walk the graph and remember where you have been".
There are two basic ways to walk. **BFS** (breadth-first search) is a ripple in a pond: visit
everything one step away, then everything two steps away, and so on. Because it goes in rings,
the first time BFS reaches a vertex it has found the shortest route there (counting edges).
**DFS** (depth-first search) is a person in a maze who keeps going forward until they hit a dead
end, then backs up to the last junction. DFS is the natural tool for "explore everything
reachable", "is there a cycle", and "what order must these tasks run in".

On top of those two walks sit a handful of algorithms you must know from memory:
**topological sort** (order tasks so every dependency comes first), **cycle detection**,
**connected components**, **Dijkstra** (shortest path when edges have weights, like travel
times), and **union-find** (a tiny structure that answers "are these two in the same group?"
almost in O(1)). This module covers all of them and eight interview problems built on them.

## 2. The real thing

### 2.1 Vocabulary in one line each

- **Directed** graph: edges have a direction (`u -> v`). **Undirected**: `u - v` works both ways.
- **Weighted**: each edge has a number (cost, distance, latency).
- **Degree** of `v`: number of edges touching it (in-degree / out-degree when directed).
- **Path**: a sequence of vertices joined by edges. **Cycle**: a path that returns to its start.
- **DAG**: directed acyclic graph. Dependency graphs must be DAGs.
- **Connected component**: a maximal set of vertices all reachable from each other (undirected).
- **Sparse** graph: E is about V (roads, dependency graphs). **Dense**: E is close to V^2.
- We write V for the number of vertices and E for the number of edges.

### 2.2 Representations

```
Graph: 0-1, 0-2, 1-2, 2-3   (undirected)

Edge list:          [(0,1), (0,2), (1,2), (2,3)]

Adjacency matrix:       0 1 2 3
                    0 [ 0 1 1 0 ]
                    1 [ 1 0 1 0 ]
                    2 [ 1 1 0 1 ]
                    3 [ 0 0 1 0 ]

Adjacency list:     0: 1 2
                    1: 0 2
                    2: 0 1 3
                    3: 2

CSR (compressed):   offsets = [0, 2, 4, 7, 8]
                    targets = [1,2, 0,2, 0,1,3, 2]
                    neighbours of u = targets[offsets[u] .. offsets[u+1])
```

| Representation | Memory | Edge test `u-v`? | Iterate neighbours of u | Use when |
|----------------|--------|------------------|-------------------------|----------|
| Edge list | O(E) | O(E) | O(E) | Kruskal (sort the edges), input format |
| Matrix | O(V^2) | O(1) | O(V) | Dense graphs, V <= a few thousand, Floyd-Warshall |
| Adjacency list `vector<vector<int>>` | O(V + E) | O(deg u) | O(deg u) | **Default for interviews** |
| CSR (two flat arrays) | O(V + E) | O(deg u) | O(deg u), contiguous | Large static graphs in production |

In C++ the interview default is `std::vector<std::vector<int>> g(n)` and `g[u].push_back(v)`.
For weighted graphs, `std::vector<std::vector<std::pair<int,int>>>` or a small `struct Edge
{ int to; int w; }`. Use vertex ids `0..n-1`; if the input uses strings, map them to ids
first with an `unordered_map<std::string,int>`.

**Implicit graphs.** Many problems never give you a graph. A grid is a graph whose neighbours
are the 4 adjacent cells; a word ladder is a graph whose neighbours are words one letter
away; a puzzle state is a graph whose neighbours are legal moves. You generate neighbours on
the fly instead of storing edges. Recognising an implicit graph is half the problem.

### 2.3 BFS

```
bfs(src):
    dist[*] = -1; dist[src] = 0; queue = [src]
    while queue not empty:
        u = pop front
        for v in neighbours(u):
            if dist[v] == -1:            # not yet discovered
                dist[v] = dist[u] + 1
                parent[v] = u
                push v
```

- **Visits in order of distance.** The queue always holds vertices of distance d followed by
  vertices of distance d+1, never anything else. So `dist[v]` is the length of the shortest
  path in edges. This is the property every BFS problem relies on.
- **Mark when you enqueue, not when you dequeue.** If you mark on dequeue, the same vertex can
  be pushed many times (once by each neighbour) and the queue can grow to O(E) — or, on a grid,
  exponentially many duplicates in practice. This is the number one BFS bug.
- **Path reconstruction**: store `parent[v]`; walk back from the target and reverse.
- **Layer by layer**: `for (size_t k = q.size(); k > 0; --k)` processes exactly one distance
  level, which is how you count "minutes" or "steps" in problems like rotting oranges.
- **Multi-source BFS**: push all sources with distance 0 at the start. Distances become
  "distance to the nearest source". Used for "distance to nearest exit", rotting oranges,
  walls and gates.
- **Complexity**: each vertex is enqueued once, each edge is examined once (twice if undirected):
  O(V + E) time, O(V) extra space.

### 2.4 DFS

```
dfs(u):
    seen[u] = true
    for v in neighbours(u):
        if not seen[v]: dfs(v)
```

- Same O(V + E). DFS finds *a* path, not the shortest one.
- **Recursion depth** equals the length of the current path, up to V. A grid of 1000 x 1000
  land cells gives a path of 10^6 frames; at ~50-100 bytes per frame that blows the default
  8 MB Linux thread stack (1 MB on Windows). In production, and in interviews where the input
  can be large, use an explicit stack. Say this out loud — it is a senior signal.
- An explicit stack that pushes all neighbours at once visits vertices in a *different* order
  from the recursion. That is fine for flood fill and reachability. When the order matters
  (topological sort, Tarjan), keep `(vertex, next neighbour index)` frames, exactly like the
  call stack — `dfs_iter` in `m13_graph_bfs_dfs.cpp`.
- **Edge classification** in a directed DFS: *tree* edge (to an unvisited vertex), *back* edge
  (to an ancestor still on the stack — means a cycle), *forward* edge (to a finished
  descendant), *cross* edge (to a finished vertex in another branch). Only back edges matter
  for cycle detection.

### 2.5 Connected components

Loop over every vertex; if it has not been seen, start a BFS/DFS from it and give everything
reached the same component number. O(V + E). Number of islands is exactly this on a grid.
Union-find (2.9) is the alternative when edges arrive one at a time.

### 2.6 Cycle detection

**Undirected**: during DFS/BFS, meeting an already-seen vertex that is not the one you just
came from means a cycle. (With parallel edges, skip by edge id instead of parent vertex.)
Or with union-find: an edge whose endpoints are already in the same set closes a cycle.

**Directed**: "already seen" is not enough. In the diamond `0->1->3, 0->2->3`, vertex 3 is seen
twice with no cycle. Use three colours:

```
white = not visited   grey = on the current DFS path   black = finished
edge to a grey vertex  -> back edge -> cycle
edge to a black vertex -> cross/forward edge -> no cycle
```

Or run Kahn's topological sort and check whether every vertex came out.

### 2.7 Topological sort

A linear order of a DAG's vertices such that every edge `u -> v` has `u` before `v`. Exists if
and only if the graph has no directed cycle. Two algorithms, both O(V + E):

**Kahn (BFS on in-degrees)**: compute in-degrees; queue every vertex with in-degree 0; pop one,
output it, decrement the in-degree of each successor, enqueue those that reach 0. If the output
has fewer than V vertices, the remaining ones are on or behind a cycle. Using a min-heap
instead of a FIFO gives the lexicographically smallest order (O((V+E) log V)).

**DFS post-order**: run DFS; append a vertex to a list when it *finishes* (after all its
descendants); reverse the list. A vertex finishes only after everything it points to, so in
reverse finishing order it comes first. Grey detection gives cycle detection for free.

```
5 -> 2 -> 3 -> 1        Kahn:   start {4, 5}  ->  4 5 0 2 3 1   (one valid order)
5 -> 0                  DFS:    finish order 1 3 2 0 5 4  ->  reversed 4 5 0 2 3 1
4 -> 0,  4 -> 1
```

Kahn is easier to reason about, gives you "which tasks can run in parallel now" (everything
in the queue at once), and needs no recursion. DFS is shorter to write if you already have a
DFS. Know both.

### 2.8 Dijkstra

Single-source shortest paths with **non-negative** edge weights.

```
dist[*] = INF; dist[src] = 0; heap = {(0, src)}
while heap not empty:
    (d, u) = pop min
    if d != dist[u]: continue              # stale entry (lazy deletion, module 12)
    for (v, w) in edges(u):
        if d + w < dist[v]:                # relax
            dist[v] = d + w; parent[v] = u
            push (dist[v], v)
```

**Why it works.** When `u` is popped with the smallest tentative distance, no other route can
beat it: any other route leaves the "finished" set through some vertex whose distance is
already >= `dist[u]`, and the remaining edges only add non-negative weight. That argument is
exactly where a negative edge breaks it.

**Complexity.** Binary heap with lazy deletion: O((V + E) log E), and log E <= 2 log V, so
O((V + E) log V). The heap may hold up to E entries. With an array instead of a heap (scan for
the minimum each round): O(V^2), which is *better* for dense graphs where E is about V^2.
Fibonacci heaps give O(E + V log V) in theory; nobody uses them in practice because the
constants and pointer-chasing lose to a binary or 4-ary heap.

**Related algorithms, one line each:**

| Problem | Algorithm | Cost |
|---------|-----------|------|
| Unweighted shortest path | BFS | O(V + E) |
| Weights only 0 or 1 | 0-1 BFS with a deque (0-edges to the front) | O(V + E) |
| Non-negative weights | Dijkstra | O((V + E) log V) |
| Negative weights, detect negative cycle | Bellman-Ford: relax all edges V-1 times | O(V E) |
| All pairs, small V | Floyd-Warshall: `d[i][j] = min(d[i][j], d[i][k] + d[k][j])` | O(V^3) |
| Shortest path in a DAG (any weights) | Relax edges in topological order | O(V + E) |
| At most k edges ("cheapest flight within k stops") | Bellman-Ford limited to k+1 rounds, copy array each round | O(k E) |

### 2.9 Union-find (disjoint set union, DSU)

Keeps a collection of disjoint sets and supports two operations: `find(x)` (which set is `x`
in — returns a representative "root") and `unite(a, b)` (merge two sets). Each set is a tree
stored as a `parent[]` array; the root is the vertex with `parent[x] == x`.

```
parent:  [0, 0, 1, 3, 3]          0         3
                                  |         |
                                  1         4
                                  |
                                  2
find(2): 2 -> 1 -> 0 (root)
```

Two optimisations, both one line:

- **Path compression**: after `find(x)`, point every vertex on the path directly at the root.
- **Union by size (or rank)**: hang the smaller tree under the larger, so height stays <= log2 n
  even without compression.

With both, a sequence of m operations costs O(m alpha(n)), where alpha is the inverse
Ackermann function — at most 4 for any n that fits in the universe. Call it "effectively
constant", and say "amortised" (module 01). With only one of the two: O(log n) amortised.
With neither: O(n) per find in the worst case (a chain).

Use union-find when edges arrive over time and you only ask "same group?" — dynamic
connectivity, Kruskal's MST, "redundant connection", "accounts merge", grouping records that
share any identifier. It cannot split sets (no delete) and it does not give you paths.

**Kruskal's MST**: sort edges by weight; take each edge whose endpoints are in different sets.
O(E log E) for the sort, near-linear after. **Prim's MST** is Dijkstra's shape with key =
edge weight instead of path length. Both are in `m13_union_find.cpp`.

## 3. Code

Files:

- `code/m13_graph_bfs_dfs.cpp` — edge list to adjacency list, matrix and CSR; BFS distances and
  path; DFS recursive and an iterative version that visits in the same order; components;
  undirected and directed (three-colour) cycle detection; bipartite check; grid BFS.
- `code/m13_topo_sort.cpp` — Kahn, Kahn with a min-heap, DFS post-order, a validator, course
  schedule, build order for named targets, alien dictionary, random-DAG differential test.
- `code/m13_dijkstra.cpp` — Dijkstra with lazy deletion and path reconstruction, O(V^2)
  Dijkstra, 0-1 BFS, Bellman-Ford with negative-cycle detection, all cross-checked against
  Floyd-Warshall on random graphs.
- `code/m13_union_find.cpp` — DSU with iterative path compression and union by size, a naive
  DSU to show the O(n) chain, Kruskal vs Prim on random graphs, redundant connection, accounts
  merge.
- `code/m13_graph_problems.cpp` — the eight problems in section 3.3.

Build any of them with `g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread file.cpp -o f && ./f`.

### 3.1 BFS and three-colour DFS (key parts)

```cpp
using Graph = std::vector<std::vector<int>>;

std::vector<int> bfs_dist(const Graph& g, int src) {
    std::vector<int> dist(g.size(), -1);
    std::queue<int> q;
    dist[src] = 0;                             // mark when ENQUEUED
    q.push(src);
    while (!q.empty()) {
        int u = q.front(); q.pop();
        for (int v : g[u]) {
            if (dist[v] != -1) continue;
            dist[v] = dist[u] + 1;
            q.push(v);
        }
    }
    return dist;
}

// 0 = white, 1 = grey (on the current path), 2 = black (finished)
bool dfs_colour(const Graph& g, int u, std::vector<int>& colour) {
    colour[u] = 1;
    for (int v : g[u]) {
        if (colour[v] == 1) return true;                         // back edge: cycle
        if (colour[v] == 0 && dfs_colour(g, v, colour)) return true;
    }
    colour[u] = 2;
    return false;
}
```

### 3.2 Kahn, Dijkstra, DSU (key parts)

```cpp
std::vector<int> topo_kahn(const Graph& g) {
    int n = static_cast<int>(g.size());
    std::vector<int> indeg(n, 0);
    for (const auto& adj : g) for (int v : adj) ++indeg[v];
    std::queue<int> q;
    for (int u = 0; u < n; ++u) if (indeg[u] == 0) q.push(u);
    std::vector<int> order;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        order.push_back(u);
        for (int v : g[u]) if (--indeg[v] == 0) q.push(v);
    }
    if (static_cast<int>(order.size()) != n) return {};          // cycle
    return order;
}

std::vector<std::int64_t> dijkstra(const WGraph& g, int src) {
    std::vector<std::int64_t> dist(g.size(), kInf);
    using Item = std::pair<std::int64_t, int>;                    // (distance, vertex)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;   // min-heap
    dist[src] = 0;
    pq.push({0, src});
    while (!pq.empty()) {
        auto [du, u] = pq.top(); pq.pop();
        if (du != dist[u]) continue;                              // stale
        for (const Edge& e : g[u])
            if (du + e.w < dist[e.to]) { dist[e.to] = du + e.w; pq.push({dist[e.to], e.to}); }
    }
    return dist;
}

class DSU {
    std::vector<int> parent_, size_;
public:
    explicit DSU(int n) : parent_(n), size_(n, 1) { std::iota(parent_.begin(), parent_.end(), 0); }
    int find(int x) {
        int root = x;
        while (parent_[root] != root) root = parent_[root];
        while (parent_[x] != root) { int nx = parent_[x]; parent_[x] = root; x = nx; }   // compress
        return root;
    }
    bool unite(int a, int b) {
        a = find(a); b = find(b);
        if (a == b) return false;
        if (size_[a] < size_[b]) std::swap(a, b);                 // smaller under larger
        parent_[b] = a; size_[a] += size_[b];
        return true;
    }
};
```

`kInf` is `INT64_MAX / 4`, not `INT64_MAX`: `kInf + w` must not overflow if you ever add to an
unreached distance. The same reason makes `int` code use `INT_MAX / 2`.

### 3.3 The eight problems

Every solution below is in `code/m13_graph_problems.cpp` with tests.

**Problem 1 — Number of islands.** Grid of `'1'` (land) and `'0'` (water); count 4-connected
land groups. *Pattern:* components on an implicit grid graph. Scan cells; on each unvisited
`'1'`, count one and flood-fill it to `'0'`. Iterative, to survive a huge all-land grid.

```cpp
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
                auto [y, x] = st.back(); st.pop_back();
                const int dy[4] = {1, -1, 0, 0}, dx[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    int ny = y + dy[k], nx = x + dx[k];
                    if (ny < 0 || ny >= R || nx < 0 || nx >= C || grid[ny][nx] != '1') continue;
                    grid[ny][nx] = '0';                               // mark on push
                    st.push_back({ny, nx});
                }
            }
        }
    return count;
}
```

O(R C) time and O(R C) worst-case stack. Taking `grid` by value is deliberate: we mutate it
as the visited set and do not want to destroy the caller's copy. Mention that if the caller
allows mutation you can take it by reference and save the copy.

**Problem 2 — Rotting oranges.** 0 empty, 1 fresh, 2 rotten; each minute rot spreads to
4-neighbours; return the minutes until no fresh orange remains, or -1. *Pattern:* multi-source
BFS, layer by layer.

```cpp
int oranges_rotting(std::vector<std::vector<int>> grid) {
    int R = static_cast<int>(grid.size()), C = static_cast<int>(grid[0].size());
    std::queue<std::pair<int, int>> q;
    int fresh = 0;
    for (int r = 0; r < R; ++r)
        for (int c = 0; c < C; ++c) {
            if (grid[r][c] == 2) q.push({r, c});
            else if (grid[r][c] == 1) ++fresh;
        }
    int minutes = 0;
    const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
    while (!q.empty() && fresh > 0) {
        for (std::size_t layer = q.size(); layer > 0; --layer) {     // one minute
            auto [r, c] = q.front(); q.pop();
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k], nc = c + dc[k];
                if (nr < 0 || nr >= R || nc < 0 || nc >= C || grid[nr][nc] != 1) continue;
                grid[nr][nc] = 2; --fresh; q.push({nr, nc});
            }
        }
        ++minutes;
    }
    return fresh == 0 ? minutes : -1;
}
```

The `fresh > 0` in the loop condition stops one minute too many from being counted after the
last orange rots. Edge cases: no fresh at all -> 0; fresh but no rotten -> -1. O(R C).

**Problem 3 — Course schedule II.** `n` courses, pairs `{a, b}` meaning "b before a"; return
any valid order or empty. *Pattern:* topological sort (Kahn).

```cpp
std::vector<int> find_order(int n, const std::vector<std::pair<int, int>>& prereqs) {
    std::vector<std::vector<int>> g(n);
    std::vector<int> indeg(n, 0);
    for (auto [a, b] : prereqs) { g[b].push_back(a); ++indeg[a]; }   // edge b -> a
    std::queue<int> q;
    for (int i = 0; i < n; ++i) if (indeg[i] == 0) q.push(i);
    std::vector<int> order;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        order.push_back(u);
        for (int v : g[u]) if (--indeg[v] == 0) q.push(v);
    }
    if (static_cast<int>(order.size()) != n) return {};
    return order;
}
```

O(V + E). The usual bug is the edge direction: say aloud "b must come first, so the edge is
b -> a" before writing it.

**Problem 4 — Clone graph.** Deep-copy a connected undirected graph given one node.
*Pattern:* BFS with an `old -> new` hash map that doubles as the visited set.

```cpp
struct GNode { int val; std::vector<GNode*> nbrs; };

GNode* clone_graph(GNode* start, GraphPool& pool) {
    if (!start) return nullptr;
    std::unordered_map<GNode*, GNode*> copy;
    std::queue<GNode*> q;
    copy[start] = pool.make(start->val);
    q.push(start);
    while (!q.empty()) {
        GNode* u = q.front(); q.pop();
        for (GNode* v : u->nbrs) {
            auto it = copy.find(v);
            if (it == copy.end()) {                                 // first time we see v
                it = copy.emplace(v, pool.make(v->val)).first;
                q.push(v);
            }
            copy[u]->nbrs.push_back(it->second);
        }
    }
    return copy[start];
}
```

O(V + E). Ownership is the C++ angle: a cyclic graph cannot be owned through `unique_ptr`
edges (who owns whom in a cycle?). Nodes live in a pool (`vector<unique_ptr<GNode>>`) and edges
are non-owning raw pointers. Saying that is worth more than the BFS.

**Problem 5 — Word ladder.** Shortest transformation `begin -> end`, one letter at a time,
every intermediate word in a dictionary; return the number of words, or 0. *Pattern:* BFS on
an implicit graph; generate neighbours by trying 26 letters at each position.

```cpp
int ladder_length(const std::string& begin, const std::string& end, const std::vector<std::string>& words) {
    std::unordered_set<std::string> dict(words.begin(), words.end());
    if (!dict.count(end)) return 0;
    std::queue<std::string> q;
    q.push(begin);
    dict.erase(begin);
    int steps = 1;
    while (!q.empty()) {
        for (std::size_t layer = q.size(); layer > 0; --layer) {
            std::string w = q.front(); q.pop();
            if (w == end) return steps;
            for (std::size_t i = 0; i < w.size(); ++i) {
                char orig = w[i];
                for (char ch = 'a'; ch <= 'z'; ++ch) {
                    if (ch == orig) continue;
                    w[i] = ch;
                    auto it = dict.find(w);
                    if (it != dict.end()) { q.push(w); dict.erase(it); }   // erase = visited
                }
                w[i] = orig;
            }
        }
        ++steps;
    }
    return 0;
}
```

O(N L * 26 * L): N words, L letters, 26 L candidates per word, each hashed in O(L). Comparing
every pair of words instead is O(N^2 L) — worse once N is larger than about 26 L.
Bidirectional BFS (expand the smaller frontier from both ends) cuts the explored set from
b^d to about 2 b^(d/2); mention it as the follow-up.

**Problem 6 — Network delay time.** Directed weighted edges `{u, v, w}`, nodes `1..n`; how long
until a signal from `k` reaches every node, or -1. *Pattern:* Dijkstra, then the max distance.

```cpp
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
        auto [d, u] = pq.top(); pq.pop();
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
```

O((V + E) log V). `d + w` cannot overflow here because `d` is a popped, finite distance; if
you relax from unreached vertices anywhere, use `INF / 2`.

**Problem 7 — Number of provinces.** `n x n` adjacency matrix; count connected groups.
*Pattern:* union-find (BFS/DFS works equally well — say so, then pick one).

```cpp
int find_circle_num(const std::vector<std::vector<int>>& adj) {
    int n = static_cast<int>(adj.size());
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        return parent[x] == x ? x : parent[x] = find(parent[x]);
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
```

O(n^2 alpha(n)) — reading the matrix dominates. Count sets by decrementing on every
successful union: no second pass needed.

**Problem 8 — Is graph bipartite.** Can the vertices be split into two sides with every edge
crossing? *Pattern:* BFS 2-colouring; a conflict means an odd cycle.

```cpp
bool is_bipartite(const std::vector<std::vector<int>>& g) {
    std::vector<int> colour(g.size(), -1);
    for (std::size_t s = 0; s < g.size(); ++s) {
        if (colour[s] != -1) continue;                    // may be disconnected
        colour[s] = 0;
        std::queue<int> q;
        q.push(static_cast<int>(s));
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : g[u]) {
                if (colour[v] == -1) { colour[v] = colour[u] ^ 1; q.push(v); }
                else if (colour[v] == colour[u]) return false;
            }
        }
    }
    return true;
}
```

O(V + E). Forgetting the outer loop over all start vertices is the classic bug: a
disconnected graph whose second component has a triangle would be reported bipartite.

## 4. Under the hood (senior-level)

### 4.1 Memory layout decides the constant factor

`std::vector<std::vector<int>>` is V separate heap allocations. Each inner vector is a 24-byte
header (pointer, size, capacity) in the outer array, pointing to its own block somewhere on
the heap. Visiting `u`'s neighbours costs one miss for the header (often already cached when
iterating in order) and one for the block. For BFS over a 10-million-vertex graph that is
roughly two cache misses per vertex plus one per neighbour that is not already cached.

CSR (`offsets` + `targets`) is two allocations. Neighbour lists are contiguous and back to
back, so the hardware prefetcher streams them. Memory is `4(V+1) + 4E` bytes instead of
`24V + 4E + allocator overhead` (glibc malloc adds 8-16 bytes per block and rounds up). Graph
engines, routing engines and the Linux kernel's own static tables use CSR or close variants.
The price: CSR is immutable — adding an edge means rebuilding. Build with adjacency lists,
freeze into CSR, then query.

The access *pattern* of BFS on a large graph is still random (`dist[v]` for arbitrary `v`), so
it is bounded by memory latency, not bandwidth. Two standard fixes: renumber vertices so
neighbours have nearby ids (BFS order or a space-filling curve on a road network), and use
bitsets for `visited` (one bit per vertex fits 8x more of the frontier in cache).

### 4.2 `std::queue` is a `std::deque`

`std::queue<int>` defaults to `std::deque<int>` underneath. libstdc++'s deque allocates 512-byte
chunks and keeps a map of chunk pointers, so push/pop are O(1) with an allocation every 128
ints. For hot BFS loops a plain `std::vector<int>` with a read index (`head`) is faster: no
chunking, no allocation after the first growth, and for BFS you never need to reclaim the
front because each vertex is pushed at most once (the vector reaches at most V). Many
competitive and production BFS implementations do exactly this.

```cpp
std::vector<int> q; q.reserve(n); q.push_back(src);
for (std::size_t head = 0; head < q.size(); ++head) {
    int u = q[head];
    for (int v : g[u]) if (dist[v] == -1) { dist[v] = dist[u] + 1; q.push_back(v); }
}
```

### 4.3 Recursion depth, concretely

Each DFS frame holds the return address, saved registers, `u`, the loop iterator and the
references passed in: on x86-64 with GCC at `-O0` that is typically 64-128 bytes; at `-O2` the
compiler may inline or shrink it but cannot remove the recursion. Linux main thread stack:
8 MB by default (`ulimit -s`); `std::thread` stacks: 8 MB by default on glibc (from
`RLIMIT_STACK`); Windows: 1 MB by default for every thread (`/STACK` linker option). A path of
10^5 vertices is borderline on Windows, 10^6 is dead on both. The crash is a SIGSEGV on the
guard page — in a core dump you see thousands of identical frames. You have debugged crash
dumps; this is what one looks like.

### 4.4 Dijkstra's heap in practice

Lazy deletion means the heap can hold up to E entries, not V. On a road graph with E about
2.5V that costs little; on a dense graph it costs memory. The alternatives: an **indexed heap**
with decrease-key (module 12, exactly V entries), or a **4-ary heap** (shallower, children in
one cache line). Routing engines go further: **bidirectional Dijkstra** (search from both
ends, stop when the frontiers meet), **A\*** (add an admissible lower bound such as straight-line
distance to the priority, which steers the search toward the goal) and **contraction
hierarchies** (precompute shortcuts once, answer queries in microseconds on continental maps).
You do not need to implement those, but naming them shows you know where the textbook ends.

### 4.5 Why union-find is "almost O(1)"

Union by size alone guarantees height <= log2 n: a vertex's depth increases only when its tree
is hung under a tree at least as large, which at least doubles the size of its set, and that
can happen at most log2 n times. Path compression then flattens every path it touches. Tarjan
proved the combination costs O(m alpha(n)) for m operations. In code, after a few operations
almost every `find` is one or two array reads. The `parent[]` array is contiguous and small,
so union-find over millions of elements is one of the fastest structures in this course.

The recursive `find` (`return parent[x] == x ? x : parent[x] = find(parent[x]);`) is fine with
union by size (depth <= log n), but on its own — compression without balancing — the first
`find` on a long chain can recurse n deep. The iterative two-pass version in
`m13_union_find.cpp` has no such risk.

### 4.6 Graphs in the systems you know

- **PostgreSQL** detects deadlocks by building the *waits-for graph* (process A waits for a lock
  held by B) and searching it for a cycle — DFS — after `deadlock_timeout` (1 s by default)
  expires. The planner's join search is a DP over subsets of tables (module 14), which falls
  back to a genetic algorithm (GEQO) past `geqo_threshold` tables.
- **Berkeley DB** has a deadlock detector (`db_deadlock`, `DB_ENV->lock_detect`) that does the
  same waits-for cycle search and picks a victim by policy (oldest, youngest, most locks).
- **Build systems and migrations**: `make`, CMake, Bazel and every schema-migration tool
  topologically sort a dependency DAG; Kahn's queue at each step is the set of jobs that can
  run in parallel. Your BDB -> YugabyteDB migration had to load parent tables before tables
  with foreign keys to them — that is a topological sort of the FK graph, and a circular FK is
  the cycle that makes it fail (the fix is to load data with constraints deferred, then add them).
- **Linux**: the kernel's lock-validator `lockdep` builds a graph of lock-acquisition orders and
  reports a potential deadlock when a new edge would create a cycle — at the first time the
  bad order *could* happen, not when it actually deadlocks.
- **Garbage collectors and reference-cycle detection** (Python's `gc`, `std::shared_ptr`
  cycles you must break with `weak_ptr`) are reachability searches from roots.

## 5. In production

- **Deadlock analysis in your RPC server.** Each worker thread holds some locks and waits for
  one. Dump "thread -> lock held" and "thread -> lock waited" from a crash dump or a debug
  endpoint, build the waits-for graph, and run the three-colour DFS: the grey vertex you hit
  is in the cycle. This is how you go from "the server is hung" to "thread 12 holds the
  account lock and wants the session lock, thread 31 holds them in the opposite order".
- **Dependency ordering at startup and shutdown.** Services that depend on each other (DB pool
  before session manager before listener) start in topological order and stop in reverse. A
  hand-maintained list rots; a DAG with Kahn's algorithm fails loudly on a cycle at startup
  instead of deadlocking at 3 AM.
- **Linking records.** "These two customer records share a phone number, these two share a PAN,
  therefore all four are one customer" — union-find over record ids, uniting on every shared
  identifier (accounts merge). It is linear-ish over hundreds of millions of rows and runs in a
  single pass over the data. Doing it with repeated SQL self-joins is quadratic and slow.
- **Pitfall: visited sets that are too big.** `std::unordered_set<std::string>` of visited states
  for a BFS over 50 million states needs gigabytes. Map states to dense integer ids and use a
  `std::vector<char>` or a bitset; or encode the state into a `uint64_t`.
- **Pitfall: recursion on user-controlled input.** Any DFS whose depth depends on input size
  (a chain of 1M transfers, a deep JSON document, a nested menu) must be iterative in a
  server. A malicious or merely large input otherwise crashes the process.
- **Pitfall: Dijkstra with "negative" adjustments.** Fees can be negative (rebates); a
  "cheapest route" with a negative edge silently gives wrong answers with Dijkstra. Validate
  weights or use Bellman-Ford.

## 6. Try it

1. **(easy)** Build the adjacency list for an undirected graph from an edge list and print BFS
   distances from vertex 0. Then convert it to CSR and run BFS on the CSR form. Done when both
   give identical distances on a random graph with 10^5 vertices and 3 x 10^5 edges.
2. **(easy)** Flood fill: given a grid, a start cell and a new colour, recolour the start cell's
   4-connected region. Done when you have a recursive and an iterative version and the
   iterative one handles a 3000 x 3000 single-colour grid without crashing (the recursive one
   should crash there — observe it).
3. **(medium)** Course schedule with *parallel* scheduling: return the minimum number of
   semesters needed if any number of courses with satisfied prerequisites can be taken per
   semester. Done when `n=3, {1->3, 2->3}` gives 2 and a cycle gives -1. (Hint: Kahn, counting
   layers.)
4. **(medium)** Cheapest flights within k stops: directed weighted edges, find the cheapest
   price from `src` to `dst` with at most `k` stops. Done when the LeetCode example
   (`n=4, k=1`) gives 700 and you can explain why plain Dijkstra on (vertex) state is wrong and
   what Bellman-Ford with k+1 rounds (copying the array each round) fixes.
5. **(hard)** Implement Tarjan's or Kosaraju's strongly connected components and use it to find
   all deadlock cycles in a waits-for graph of 10^5 threads/locks. Done when every SCC of size
   > 1 is printed and the result matches a brute-force "can u reach v and v reach u" check on
   random graphs of 200 vertices.
6. **(hard)** Word ladder with bidirectional BFS. Done when it returns the same answers as the
   one-directional version on 1000 random queries over a 5000-word dictionary and you can
   report how many words each version expanded.

## 7. Interview questions

**Q: BFS or DFS — when do you use which?**
BFS when you need the shortest path in edges or anything layered by distance ("minimum number of
moves", "minutes until"). DFS for reachability, cycle detection, topological order, and
backtracking-style exploration. Both are O(V + E); DFS uses O(depth) stack, BFS O(width) queue.

**Q: Why must BFS mark a vertex visited when it is enqueued, not when dequeued?**
Otherwise a vertex can be enqueued once per incoming edge before it is ever dequeued, so the
queue grows to O(E) and work repeats. Marking on enqueue guarantees each vertex enters the queue
exactly once.

**Q: How do you detect a cycle in a directed graph?**
DFS with three colours: an edge to a grey vertex (on the current path) is a back edge and means
a cycle. Or run Kahn's topological sort: if fewer than V vertices come out, there is a cycle.
"Seen before" alone is not enough because of diamonds.

**Q: Explain topological sort and its two algorithms.**
An order with every edge pointing forward; exists iff the graph is a DAG. Kahn repeatedly
removes in-degree-zero vertices with a queue; DFS appends vertices in post-order and reverses.
Both O(V + E); Kahn also shows which tasks can run in parallel at each step.

**Q: Why does Dijkstra fail with negative edges, and what do you use instead?**
Dijkstra finalises the vertex with the smallest tentative distance, assuming no later path can
be shorter because weights only add. A negative edge can make a later path shorter, so a
finalised distance can be wrong. Use Bellman-Ford (O(VE), also detects negative cycles), or
topological-order relaxation if the graph is a DAG.

**Q: What is the complexity of Dijkstra with a binary heap, and when is O(V^2) better?**
O((V + E) log V) with lazy deletion or decrease-key. The array version that scans for the minimum
is O(V^2), which wins on dense graphs where E is close to V^2 because it avoids the log factor
and the heap's overhead.

**Q: What are path compression and union by rank, and what do they give you?**
Path compression points every node on a find path at the root; union by rank/size attaches the
shorter tree under the taller. Together they make m operations cost O(m alpha(n)), effectively
constant per operation. Either alone gives O(log n) amortised.

**Q: Adjacency list vs adjacency matrix?**
List: O(V + E) memory and neighbour iteration proportional to degree — right for sparse graphs,
which is almost all real ones. Matrix: O(V^2) memory, O(1) edge test — right for dense graphs or
when V is small and you need constant-time edge checks (Floyd-Warshall). For large static graphs
in production, CSR: the list's complexity with contiguous memory.

## 8. Recap

- Graph = vertices + edges; default representation `std::vector<std::vector<int>>`; CSR for
  large static graphs.
- Many problems are implicit graphs: grids, words one letter apart, puzzle states.
- BFS: queue, mark on enqueue, gives shortest paths in edges, O(V + E); multi-source BFS starts
  with all sources in the queue.
- DFS: recursion or explicit stack, O(V + E); deep inputs need the explicit stack.
- Directed cycle: three colours, edge to grey = cycle. Undirected: seen and not the parent.
- Topological sort: Kahn (in-degrees + queue) or DFS post-order reversed; fails iff a cycle.
- Dijkstra: min-heap, lazy deletion, non-negative weights, O((V + E) log V); Bellman-Ford for
  negative weights; 0-1 BFS for 0/1 weights.
- Union-find: parent array, path compression + union by size, O(alpha(n)) amortised; Kruskal,
  dynamic connectivity, record linking.
- Systems: deadlock detection is cycle detection in a waits-for graph; dependency ordering is
  topological sort.
