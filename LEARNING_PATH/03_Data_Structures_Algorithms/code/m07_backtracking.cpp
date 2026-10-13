// m07_backtracking.cpp
// Backtracking: subsets (two forms), permutations (unique and with duplicates), combination
// sum, N-queens (count + boards), generate parentheses, letter combinations, word search.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m07_backtracking.cpp -o m07_bt

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using Out = std::vector<std::vector<int>>;

// ---- subsets: include/exclude at each index ------------------------------------------------
void subsets_ie(const std::vector<int>& v, std::size_t i, std::vector<int>& cur, Out& out) {
    if (i == v.size()) { out.push_back(cur); return; }
    subsets_ie(v, i + 1, cur, out);        // exclude
    cur.push_back(v[i]);                   // include
    subsets_ie(v, i + 1, cur, out);
    cur.pop_back();                        // undo
}

// ---- subsets: for-loop with start index (the template for combinations) -------------------
void subsets_loop(const std::vector<int>& v, std::size_t start, std::vector<int>& cur, Out& out) {
    out.push_back(cur);                    // every prefix reached is itself a subset
    for (std::size_t i = start; i < v.size(); ++i) {
        cur.push_back(v[i]);
        subsets_loop(v, i + 1, cur, out);  // i + 1, never i: each element at most once, no dups
        cur.pop_back();
    }
}

// ---- permutations by swapping in place -----------------------------------------------------
void permute(std::vector<int>& v, std::size_t pos, Out& out) {
    if (pos == v.size()) { out.push_back(v); return; }
    for (std::size_t i = pos; i < v.size(); ++i) {
        std::swap(v[pos], v[i]);           // choose v[i] for slot pos
        permute(v, pos + 1, out);
        std::swap(v[pos], v[i]);           // un-choose
    }
}

// ---- permutations with duplicate values: sorted input + used[] + first-unused-copy rule -----
void permute_unique(const std::vector<int>& v, std::vector<char>& used, std::vector<int>& cur, Out& out) {
    if (cur.size() == v.size()) { out.push_back(cur); return; }
    for (std::size_t i = 0; i < v.size(); ++i) {
        // Skip used slots, and skip a duplicate value unless its previous copy is in use:
        // that forces equal values to be taken in index order, so each multiset perm appears once.
        if (used[i] || (i > 0 && v[i] == v[i - 1] && !used[i - 1])) continue;
        used[i] = 1;
        cur.push_back(v[i]);
        permute_unique(v, used, cur, out);
        cur.pop_back();
        used[i] = 0;
    }
}

// ---- combination sum: unlimited reuse, sorted candidates so we can break --------------------
void combination_sum(const std::vector<int>& c, std::size_t start, int remaining,
                     std::vector<int>& cur, Out& out) {
    if (remaining == 0) { out.push_back(cur); return; }
    for (std::size_t i = start; i < c.size(); ++i) {
        if (c[i] > remaining) break;       // sorted: all later candidates are too big as well
        cur.push_back(c[i]);
        combination_sum(c, i, remaining - c[i], cur, out);   // i (not i+1): reuse allowed
        cur.pop_back();
    }
}

// ---- N-queens -----------------------------------------------------------------------------
struct Queens {
    int n;
    std::vector<int> col_of;               // col_of[row] = column of the queen in that row
    std::vector<char> cols, diag1, diag2;  // occupied column / (row-col) / (row+col)
    int count = 0;
    std::vector<std::vector<std::string>> boards;

    explicit Queens(int n_) : n(n_), col_of(static_cast<std::size_t>(n_)),
        cols(static_cast<std::size_t>(n_), 0),
        diag1(static_cast<std::size_t>(2 * n_ - 1), 0),
        diag2(static_cast<std::size_t>(2 * n_ - 1), 0) {}

    void solve(int row, bool collect) {
        if (row == n) {
            ++count;
            if (collect) {
                std::vector<std::string> b(static_cast<std::size_t>(n), std::string(static_cast<std::size_t>(n), '.'));
                for (int r = 0; r < n; ++r) b[static_cast<std::size_t>(r)][static_cast<std::size_t>(col_of[static_cast<std::size_t>(r)])] = 'Q';
                boards.push_back(std::move(b));
            }
            return;
        }
        for (int c = 0; c < n; ++c) {
            std::size_t a = static_cast<std::size_t>(row - c + n - 1);   // shift to >= 0
            std::size_t b = static_cast<std::size_t>(row + c);
            std::size_t cc = static_cast<std::size_t>(c);
            if (cols[cc] || diag1[a] || diag2[b]) continue;           // prune in O(1)
            cols[cc] = diag1[a] = diag2[b] = 1;
            col_of[static_cast<std::size_t>(row)] = c;
            solve(row + 1, collect);
            cols[cc] = diag1[a] = diag2[b] = 0;                       // undo
        }
    }
};

// ---- generate all valid parentheses strings for n pairs -----------------------------------
void gen_parens(int n, int open, int close, std::string& cur, std::vector<std::string>& out) {
    if (static_cast<int>(cur.size()) == 2 * n) { out.push_back(cur); return; }
    if (open < n)     { cur.push_back('('); gen_parens(n, open + 1, close, cur, out); cur.pop_back(); }
    if (close < open) { cur.push_back(')'); gen_parens(n, open, close + 1, cur, out); cur.pop_back(); }
}

// ---- letter combinations of a phone number ------------------------------------------------
void letter_combos(const std::string& digits, std::size_t i, std::string& cur, std::vector<std::string>& out) {
    static const char* const kMap[10] = {"", "", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
    if (i == digits.size()) { if (!cur.empty()) out.push_back(cur); return; }
    for (const char* p = kMap[digits[i] - '0']; *p; ++p) {
        cur.push_back(*p);
        letter_combos(digits, i + 1, cur, out);
        cur.pop_back();
    }
}

// ---- word search in a grid: DFS with in-place marking as the visited set ------------------
bool word_search_from(std::vector<std::string>& g, int r, int c, const std::string& w, std::size_t k) {
    if (k == w.size()) return true;
    if (r < 0 || c < 0 || r >= static_cast<int>(g.size()) || c >= static_cast<int>(g[0].size())) return false;
    std::size_t rr = static_cast<std::size_t>(r), cc = static_cast<std::size_t>(c);
    if (g[rr][cc] != w[k]) return false;
    char saved = g[rr][cc];
    g[rr][cc] = '#';                        // mark visited by mutating the grid
    bool found = word_search_from(g, r + 1, c, w, k + 1) || word_search_from(g, r - 1, c, w, k + 1) ||
                 word_search_from(g, r, c + 1, w, k + 1) || word_search_from(g, r, c - 1, w, k + 1);
    g[rr][cc] = saved;                      // undo the mark
    return found;
}
bool word_search(std::vector<std::string> g, const std::string& w) {
    for (int r = 0; r < static_cast<int>(g.size()); ++r)
        for (int c = 0; c < static_cast<int>(g[0].size()); ++c)
            if (word_search_from(g, r, c, w, 0)) return true;
    return false;
}

}  // namespace

int main() {
    {
        std::vector<int> v = {1, 2, 3};
        std::vector<int> cur;
        Out a, b;
        subsets_ie(v, 0, cur, a);
        subsets_loop(v, 0, cur, b);
        assert(a.size() == 8 && b.size() == 8);
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        assert(a == b);
        std::printf("subsets of {1,2,3}: %zu\n", a.size());
    }
    {
        std::vector<int> v = {1, 2, 3, 4};
        Out out;
        permute(v, 0, out);
        assert(out.size() == 24);
        std::sort(out.begin(), out.end());
        assert(std::unique(out.begin(), out.end()) == out.end());   // all distinct
        std::printf("permutations of 4: %zu\n", out.size());

        std::vector<int> d = {1, 1, 2};
        std::vector<char> used(d.size(), 0);
        std::vector<int> cur;
        Out uo;
        permute_unique(d, used, cur, uo);
        assert(uo.size() == 3);                                       // 112, 121, 211
    }
    {
        std::vector<int> c = {2, 3, 6, 7};
        std::vector<int> cur;
        Out out;
        combination_sum(c, 0, 7, cur, out);
        assert(out.size() == 2);                                      // {2,2,3}, {7}
    }
    {
        Queens q8(8);
        q8.solve(0, true);
        assert(q8.count == 92 && q8.boards.size() == 92);
        std::printf("8-queens: %d solutions; first:\n", q8.count);
        for (const std::string& row : q8.boards[0]) std::printf("  %s\n", row.c_str());
        Queens q10(10);
        q10.solve(0, false);
        assert(q10.count == 724);
    }
    {
        std::string cur;
        std::vector<std::string> out;
        gen_parens(3, 0, 0, cur, out);
        assert(out.size() == 5);
        out.clear();
        letter_combos("23", 0, cur, out);
        assert(out.size() == 9 && out[0] == "ad");
    }
    {
        std::vector<std::string> grid = {"ABCE", "SFCS", "ADEE"};
        assert(word_search(grid, "ABCCED"));
        assert(word_search(grid, "SEE"));
        assert(!word_search(grid, "ABCB"));
    }
    std::printf("all backtracking checks passed\n");
    return 0;
}
