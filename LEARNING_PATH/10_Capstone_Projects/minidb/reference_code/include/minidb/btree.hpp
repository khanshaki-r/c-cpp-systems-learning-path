#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minidb/page_cache.hpp"

namespace minidb {

// B+tree over pages from the PageCache. Keys and values are byte strings; keys are compared
// with std::string_view::compare (bytewise, like memcmp).
//
//   internal page: child0 | key1 child1 | key2 child2 | ...   key_i = smallest key in child_i
//   leaf page:     next_leaf | key1 value1 | key2 value2 | ...  sorted, unique keys
//
// Every leaf is at the same depth. Leaves are chained left to right through next_leaf, so a
// range scan descends once and then walks sideways.
//
// Simplification (documented, deliberate): erase() never merges or rebalances. A leaf may
// become empty; it stays in the chain and is skipped by scans. Space is reclaimed only by
// rebuilding the file. Many production B-trees also delete lazily for the same reason:
// merges are complex and rarely pay for themselves.
//
// Mutating calls must run inside a PageCache transaction (the Db does this). Not thread-safe.
class BPlusTree {
public:
    explicit BPlusTree(PageCache& cache) : cache_(cache) {}

    // Pages for a brand-new database: page 0 = meta (root = 1, page_count = 2), page 1 = empty leaf.
    static std::vector<Page> initial_pages();

    std::optional<std::string> get(std::string_view key);
    void put(std::string_view key, std::string_view value);
    bool erase(std::string_view key);

    // Keys in [start, end), ascending, at most `limit`. An empty `end` means "no upper bound".
    std::vector<std::pair<std::string, std::string>> scan(std::string_view start, std::string_view end,
                                                          std::size_t limit);

    // Walks the whole tree and checks every structural invariant (key order, separator
    // bounds, equal leaf depth, leaf chain matches in-order leaves). Throws CorruptionError on
    // the first violation; returns the number of keys. Used by tests and after recovery.
    std::size_t check();

private:
    struct Split {
        std::string separator;  // smallest key of the new right sibling
        PageId right = kNoPage;
    };

    std::optional<Split> insert(PageId id, std::string_view key, std::string_view value, int depth);
    PageId find_leaf(std::string_view key);
    PageId allocate_page();
    PageId root();
    void set_root(PageId root);

    PageCache& cache_;
};

}  // namespace minidb
