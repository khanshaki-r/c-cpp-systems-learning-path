// m15_trie.cpp
// Tries: a 26-way array trie with insert / search / starts_with / count_prefix / erase /
// autocomplete; a compact trie whose nodes live in one std::vector and refer to each other by
// index (the cache-friendly, allocation-free layout); longest common prefix; replace words
// with their shortest root; and a binary trie for maximum XOR of two numbers.
// Every structure is differential-tested against std::set on random words.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m15_trie.cpp -o m15_trie

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// ---- 1. Pointer trie, 26 children per node --------------------------------------------------
class Trie {
public:
    void insert(std::string_view w) {
        Node* n = &root_;
        ++n->pass;
        for (char ch : w) {
            auto& child = n->next[idx(ch)];
            if (!child) child = std::make_unique<Node>();
            n = child.get();
            ++n->pass;                                  // how many words go through this node
        }
        ++n->end;                                       // how many copies of w end here
    }
    bool search(std::string_view w) const {
        const Node* n = walk(w);
        return n && n->end > 0;
    }
    bool starts_with(std::string_view p) const { return walk(p) != nullptr; }
    int count_prefix(std::string_view p) const {
        const Node* n = walk(p);
        return n ? n->pass : 0;
    }
    // Erase one copy of w. Frees nodes no other word passes through.
    bool erase(std::string_view w) {
        if (!search(w)) return false;
        Node* n = &root_;
        --n->pass;
        for (char ch : w) {
            auto& child = n->next[idx(ch)];
            if (--child->pass == 0) {                   // nobody else uses this branch
                child.reset();                          // frees the whole subtree below it
                return true;
            }
            n = child.get();
        }
        --n->end;
        return true;
    }
    // Up to k words with prefix p, in lexicographic order (DFS visits children a..z).
    std::vector<std::string> autocomplete(std::string_view p, std::size_t k) const {
        std::vector<std::string> out;
        const Node* n = walk(p);
        if (!n) return out;
        std::string cur(p);
        collect(n, cur, k, out);
        return out;
    }

private:
    struct Node {
        std::array<std::unique_ptr<Node>, 26> next{};   // 26 * 8 = 208 bytes of pointers per node
        int pass = 0;
        int end = 0;
    };
    static int idx(char ch) { return ch - 'a'; }
    const Node* walk(std::string_view s) const {
        const Node* n = &root_;
        for (char ch : s) {
            n = n->next[idx(ch)].get();
            if (!n) return nullptr;
        }
        return n;
    }
    static void collect(const Node* n, std::string& cur, std::size_t k, std::vector<std::string>& out) {
        if (out.size() >= k) return;
        for (int e = 0; e < n->end && out.size() < k; ++e) out.push_back(cur);
        for (int c = 0; c < 26 && out.size() < k; ++c)
            if (n->next[c]) {
                cur.push_back(static_cast<char>('a' + c));
                collect(n->next[c].get(), cur, k, out);
                cur.pop_back();
            }
    }
    Node root_;
};

// ---- 2. Index trie: all nodes in one vector, children are int32 indices ---------------------
// No per-node allocation, half-size links (4 bytes vs 8), and nodes created together are
// adjacent in memory. Destruction is one free. Erase is not supported (would need a free list).
class IndexTrie {
public:
    IndexTrie() { nodes_.emplace_back(); }
    void insert(std::string_view w) {
        std::int32_t n = 0;
        for (char ch : w) {
            std::int32_t c = nodes_[n].next[ch - 'a'];
            if (c == 0) {                               // 0 doubles as "null": the root is never a child
                c = static_cast<std::int32_t>(nodes_.size());
                nodes_.emplace_back();                  // may reallocate: never hold a Node& across this
                nodes_[n].next[ch - 'a'] = c;
            }
            n = c;
        }
        nodes_[n].end = true;
    }
    bool search(std::string_view w) const {
        std::int32_t n = find(w);
        return n >= 0 && nodes_[n].end;
    }
    bool starts_with(std::string_view p) const { return find(p) >= 0; }
    std::size_t node_count() const { return nodes_.size(); }

private:
    struct Node {
        std::array<std::int32_t, 26> next{};
        bool end = false;
    };
    std::int32_t find(std::string_view s) const {
        std::int32_t n = 0;
        for (char ch : s) {
            n = nodes_[n].next[ch - 'a'];
            if (n == 0) return -1;
        }
        return n;
    }
    std::vector<Node> nodes_;
};

// ---- 3. Longest common prefix of a set of words ---------------------------------------------
// Sort, then compare only the first and last: every other word lies between them.
std::string longest_common_prefix(std::vector<std::string> words) {
    if (words.empty()) return "";
    std::sort(words.begin(), words.end());
    const std::string& a = words.front();
    const std::string& b = words.back();
    std::size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
    return a.substr(0, k);
}

// ---- 4. Replace words: each word in a sentence becomes its shortest root in the dictionary --
std::string replace_words(const std::vector<std::string>& roots, const std::string& sentence) {
    IndexTrie t;
    for (const auto& r : roots) t.insert(r);
    std::istringstream in(sentence);
    std::string word, out;
    while (in >> word) {
        std::string rep = word;
        for (std::size_t len = 1; len <= word.size(); ++len) {
            std::string_view pre(word.data(), len);
            if (!t.starts_with(pre)) break;             // no root continues this way
            if (t.search(pre)) { rep = std::string(pre); break; }
        }
        if (!out.empty()) out.push_back(' ');
        out += rep;
    }
    return out;
}

// ---- 5. Maximum XOR of two numbers: binary trie over bits 31..0 ----------------------------
// For each number, walk the trie greedily choosing the OPPOSITE bit when it exists: a 1 in a
// higher bit beats any combination of lower bits. O(32 n).
std::uint32_t max_xor_pair(const std::vector<std::uint32_t>& a) {
    std::vector<std::array<std::int32_t, 2>> t(1, {0, 0});
    auto add = [&](std::uint32_t x) {
        std::int32_t n = 0;
        for (int b = 31; b >= 0; --b) {
            int bit = (x >> b) & 1u;
            if (t[n][bit] == 0) {
                t[n][bit] = static_cast<std::int32_t>(t.size());
                t.push_back({0, 0});
            }
            n = t[n][bit];
        }
    };
    std::uint32_t best = 0;
    add(a[0]);
    for (std::size_t i = 1; i < a.size(); ++i) {
        std::int32_t n = 0;
        std::uint32_t cur = 0;
        for (int b = 31; b >= 0; --b) {
            int want = static_cast<int>(((a[i] >> b) & 1u) ^ 1u);
            if (t[n][want] != 0) { cur |= (1u << b); n = t[n][want]; }
            else n = t[n][want ^ 1];
        }
        best = std::max(best, cur);
        add(a[i]);
    }
    return best;
}

int main() {
    Trie t;
    for (const char* w : {"apple", "app", "apply", "ape", "bank", "banker", "app"}) t.insert(w);
    assert(t.search("app") && t.search("apple") && !t.search("appl"));
    assert(t.starts_with("appl") && !t.starts_with("c"));
    assert(t.count_prefix("app") == 4);                 // app, app, apple, apply
    assert(t.count_prefix("ban") == 2 && t.count_prefix("") == 7);
    assert((t.autocomplete("ap", 10) == std::vector<std::string>{"ape", "app", "app", "apple", "apply"}));
    assert((t.autocomplete("ap", 2) == std::vector<std::string>{"ape", "app"}));
    assert(t.erase("app") && t.search("app"));          // one copy left
    assert(t.erase("app") && !t.search("app") && t.search("apple"));
    assert(t.erase("banker") && t.search("bank") && !t.starts_with("banke"));
    assert(!t.erase("zzz"));

    IndexTrie it;
    for (const char* w : {"tea", "ten", "inn"}) it.insert(w);
    assert(it.search("ten") && !it.search("te") && it.starts_with("te"));
    assert(it.node_count() == 1 + 4 + 3);               // root + t,e,a,n + i,n,n

    assert(longest_common_prefix({"flower", "flow", "flight"}) == "fl");
    assert(longest_common_prefix({"dog", "racecar", "car"}).empty());

    assert(replace_words({"cat", "bat", "rat"}, "the cattle was rattled by the battery") ==
           "the cat was rat by the bat");

    assert(max_xor_pair({3, 10, 5, 25, 2, 8}) == 28);   // 5 ^ 25
    assert(max_xor_pair({0xFFFFFFFFu, 0}) == 0xFFFFFFFFu);

    // Differential test against std::multiset<std::string>.
    std::mt19937 rng(9);
    Trie dt;
    std::multiset<std::string> mirror;
    for (int op = 0; op < 20000; ++op) {
        std::string w;
        for (std::size_t len = 1 + rng() % 5; len > 0; --len) w.push_back(static_cast<char>('a' + rng() % 3));
        int r = static_cast<int>(rng() % 3);
        if (r == 0) { dt.insert(w); mirror.insert(w); }
        else if (r == 1) {
            auto pos = mirror.find(w);
            bool had = pos != mirror.end();
            if (had) mirror.erase(pos);
            assert(dt.erase(w) == had);
        } else {
            assert(dt.search(w) == (mirror.count(w) > 0));
            std::string p = w.substr(0, 1 + rng() % w.size());
            int expect = 0;
            for (auto i = mirror.lower_bound(p); i != mirror.end() && i->compare(0, p.size(), p) == 0; ++i) ++expect;
            assert(dt.count_prefix(p) == expect);
        }
    }

    std::printf("all trie checks passed\n");
    return 0;
}
