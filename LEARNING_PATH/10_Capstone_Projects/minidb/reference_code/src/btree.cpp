#include "minidb/btree.hpp"

#include <algorithm>
#include <iterator>
#include <string>

namespace minidb {

namespace {

constexpr int kMaxDepth = 32;  // a 4 KiB-page tree of any realistic size is under 6 levels

// Decoded node forms. We decode a page, change the vectors, and encode it back. Real engines
// edit slotted pages in place; decode/encode costs some CPU but is far easier to get right,
// and the on-disk format is identical either way.
struct LeafNode {
    PageId next = kNoPage;
    std::vector<std::pair<std::string, std::string>> cells;  // sorted by key, unique
};

struct InternalNode {
    std::vector<std::string> keys;  // separators, strictly increasing
    std::vector<PageId> children;   // keys.size() + 1 entries
};

// Leaf payload:     u16 count | u32 next | count x (u16 klen | u16 vlen | key | value)
// Internal payload: u16 count | u32 child0 | count x (u16 klen | key | u32 child)
constexpr std::size_t kNodeHeader = 2 + 4;

std::size_t leaf_cell_size(const std::pair<std::string, std::string>& cell) noexcept {
    return 4 + cell.first.size() + cell.second.size();
}

std::size_t internal_entry_size(const std::string& key) noexcept { return 2 + key.size() + 4; }

std::size_t leaf_size(const LeafNode& node) noexcept {
    std::size_t size = kNodeHeader;
    for (const auto& cell : node.cells) {
        size += leaf_cell_size(cell);
    }
    return size;
}

std::size_t internal_size(const InternalNode& node) noexcept {
    std::size_t size = kNodeHeader;
    for (const auto& key : node.keys) {
        size += internal_entry_size(key);
    }
    return size;
}

// Bounds-checked decoding: a corrupt length can never read past the page.
class Reader {
public:
    explicit Reader(const char* p) noexcept : p_(p), left_(kPagePayloadSize) {}
    template <class T>
    T get() {
        need(sizeof(T));
        const T value = read_as<T>(p_);
        p_ += sizeof(T);
        left_ -= sizeof(T);
        return value;
    }
    std::string bytes(std::size_t n) {
        need(n);
        std::string out(p_, n);
        p_ += n;
        left_ -= n;
        return out;
    }

private:
    void need(std::size_t n) const {
        if (n > left_) {
            throw CorruptionError("b+tree node overruns its page");
        }
    }
    const char* p_;
    std::size_t left_;
};

class Writer {
public:
    explicit Writer(char* p) noexcept : p_(p) {}
    template <class T>
    void put(T value) noexcept {
        write_as(p_, value);
        p_ += sizeof(T);
    }
    void bytes(const std::string& s) noexcept {
        std::memcpy(p_, s.data(), s.size());
        p_ += s.size();
    }

private:
    char* p_;
};

LeafNode decode_leaf(const Page& page) {
    if (page.type() != PageType::Leaf) {
        throw CorruptionError("expected a leaf page");
    }
    Reader r(page.payload());
    LeafNode node;
    const auto count = r.get<std::uint16_t>();
    node.next = r.get<PageId>();
    node.cells.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto klen = r.get<std::uint16_t>();
        const auto vlen = r.get<std::uint16_t>();
        std::string key = r.bytes(klen);
        std::string value = r.bytes(vlen);
        node.cells.emplace_back(std::move(key), std::move(value));
    }
    return node;
}

// Precondition: leaf_size(node) <= kPagePayloadSize (callers check before encoding).
void encode_leaf(const LeafNode& node, Page& page) {
    page.set_type(PageType::Leaf);
    page.clear_payload();
    Writer w(page.payload());
    w.put(static_cast<std::uint16_t>(node.cells.size()));
    w.put(node.next);
    for (const auto& [key, value] : node.cells) {
        w.put(static_cast<std::uint16_t>(key.size()));
        w.put(static_cast<std::uint16_t>(value.size()));
        w.bytes(key);
        w.bytes(value);
    }
}

InternalNode decode_internal(const Page& page) {
    if (page.type() != PageType::Internal) {
        throw CorruptionError("expected an internal page");
    }
    Reader r(page.payload());
    InternalNode node;
    const auto count = r.get<std::uint16_t>();
    node.keys.reserve(count);
    node.children.reserve(count + 1u);
    node.children.push_back(r.get<PageId>());
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto klen = r.get<std::uint16_t>();
        node.keys.push_back(r.bytes(klen));
        node.children.push_back(r.get<PageId>());
    }
    return node;
}

void encode_internal(const InternalNode& node, Page& page) {
    page.set_type(PageType::Internal);
    page.clear_payload();
    Writer w(page.payload());
    w.put(static_cast<std::uint16_t>(node.keys.size()));
    w.put(node.children.front());
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
        w.put(static_cast<std::uint16_t>(node.keys[i].size()));
        w.bytes(node.keys[i]);
        w.put(node.children[i + 1]);
    }
}

// Index of the child whose subtree may contain `key`: the number of separators <= key.
std::size_t child_index(const InternalNode& node, std::string_view key) {
    const auto it = std::upper_bound(node.keys.begin(), node.keys.end(), key,
                                     [](std::string_view k, const std::string& sep) { return k.compare(sep) < 0; });
    return static_cast<std::size_t>(it - node.keys.begin());
}

auto leaf_lower_bound(LeafNode& node, std::string_view key) {
    return std::lower_bound(node.cells.begin(), node.cells.end(), key,
                            [](const auto& cell, std::string_view k) { return std::string_view(cell.first).compare(k) < 0; });
}

}  // namespace

std::vector<Page> BPlusTree::initial_pages() {
    std::vector<Page> pages(2);
    Meta meta;
    meta.root = 1;
    meta.page_count = 2;
    meta.encode(pages[0]);
    encode_leaf(LeafNode{}, pages[1]);
    return pages;
}

PageId BPlusTree::root() {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    return Meta::decode(meta_ref.read()).root;
}

void BPlusTree::set_root(PageId new_root) {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    Meta meta = Meta::decode(meta_ref.read());
    meta.root = new_root;
    meta.encode(meta_ref.write());
}

PageId BPlusTree::allocate_page() {
    PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
    Meta meta = Meta::decode(meta_ref.read());
    if (meta.page_count == UINT32_MAX) {
        throw DbError("database full: page id space exhausted");
    }
    const PageId id = meta.page_count++;
    meta.encode(meta_ref.write());  // logged with the transaction; undone by abort
    return id;
}

PageId BPlusTree::find_leaf(std::string_view key) {
    PageId id = root();
    for (int depth = 0; depth <= kMaxDepth; ++depth) {
        PageCache::PageRef ref = cache_.fetch(id);
        const Page& page = ref.read();
        if (page.type() == PageType::Leaf) {
            return id;
        }
        const InternalNode node = decode_internal(page);
        id = node.children[child_index(node, key)];
    }
    throw CorruptionError("b+tree deeper than possible (cycle?)");
}

std::optional<std::string> BPlusTree::get(std::string_view key) {
    PageCache::PageRef ref = cache_.fetch(find_leaf(key));
    LeafNode leaf = decode_leaf(ref.read());
    const auto it = leaf_lower_bound(leaf, key);
    if (it == leaf.cells.end() || it->first != key) {
        return std::nullopt;
    }
    return std::move(it->second);
}

void BPlusTree::put(std::string_view key, std::string_view value) {
    const PageId old_root = root();
    std::optional<Split> split = insert(old_root, key, value, 0);
    if (!split) {
        return;
    }
    // The root split: the tree grows by one level, at the top. This is the only way a
    // B+tree gets taller, which is why all leaves stay at the same depth.
    InternalNode node;
    node.keys.push_back(std::move(split->separator));
    node.children = {old_root, split->right};
    const PageId new_root = allocate_page();
    PageCache::PageRef ref = cache_.create(new_root);
    encode_internal(node, ref.write());
    set_root(new_root);
}

std::optional<BPlusTree::Split> BPlusTree::insert(PageId id, std::string_view key, std::string_view value,
                                                  int depth) {
    if (depth > kMaxDepth) {
        throw CorruptionError("b+tree deeper than possible (cycle?)");
    }
    PageCache::PageRef ref = cache_.fetch(id);

    if (ref.read().type() == PageType::Leaf) {
        LeafNode leaf = decode_leaf(ref.read());
        const auto it = leaf_lower_bound(leaf, key);
        if (it != leaf.cells.end() && it->first == key) {
            it->second.assign(value.data(), value.size());
        } else {
            leaf.cells.emplace(it, std::string(key), std::string(value));
        }
        if (leaf_size(leaf) <= kPagePayloadSize) {
            encode_leaf(leaf, ref.write());
            return std::nullopt;
        }

        // Leaf split, balanced by bytes rather than by count, so that a few large values
        // cannot leave one half overfull. Because one cell is at most a quarter of a page
        // (kMaxKeySize, kMaxValueSize), both halves are guaranteed to fit.
        std::size_t total = 0;
        for (const auto& cell : leaf.cells) {
            total += leaf_cell_size(cell);
        }
        std::size_t mid = 0;
        std::size_t left_bytes = 0;
        while (mid + 1 < leaf.cells.size() && left_bytes + leaf_cell_size(leaf.cells[mid]) <= total / 2) {
            left_bytes += leaf_cell_size(leaf.cells[mid]);
            ++mid;
        }
        mid = std::max<std::size_t>(mid, 1);

        LeafNode right;
        right.cells.assign(std::make_move_iterator(leaf.cells.begin() + static_cast<std::ptrdiff_t>(mid)),
                           std::make_move_iterator(leaf.cells.end()));
        leaf.cells.resize(mid);

        const PageId right_id = allocate_page();
        right.next = leaf.next;  // splice the new leaf into the sibling chain
        leaf.next = right_id;
        PageCache::PageRef right_ref = cache_.create(right_id);
        encode_leaf(right, right_ref.write());
        encode_leaf(leaf, ref.write());
        return Split{right.cells.front().first, right_id};
    }

    InternalNode node = decode_internal(ref.read());
    const std::size_t index = child_index(node, key);
    std::optional<Split> child_split = insert(node.children[index], key, value, depth + 1);
    if (!child_split) {
        return std::nullopt;
    }
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(index), std::move(child_split->separator));
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(index) + 1, child_split->right);
    if (internal_size(node) <= kPagePayloadSize) {
        encode_internal(node, ref.write());
        return std::nullopt;
    }

    // Internal split: keys[mid] moves *up* to the parent (unlike a leaf split, which copies
    // the first right key up and keeps it in the leaf).
    std::size_t total = 0;
    for (const auto& k : node.keys) {
        total += internal_entry_size(k);
    }
    std::size_t mid = 0;
    std::size_t left_bytes = 0;
    while (mid + 2 < node.keys.size() && left_bytes + internal_entry_size(node.keys[mid]) <= total / 2) {
        left_bytes += internal_entry_size(node.keys[mid]);
        ++mid;
    }
    mid = std::max<std::size_t>(mid, 1);

    InternalNode right;
    right.keys.assign(std::make_move_iterator(node.keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                      std::make_move_iterator(node.keys.end()));
    right.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(mid) + 1, node.children.end());
    std::string promoted = std::move(node.keys[mid]);
    node.keys.resize(mid);
    node.children.resize(mid + 1);

    const PageId right_id = allocate_page();
    PageCache::PageRef right_ref = cache_.create(right_id);
    encode_internal(right, right_ref.write());
    encode_internal(node, ref.write());
    return Split{std::move(promoted), right_id};
}

bool BPlusTree::erase(std::string_view key) {
    PageCache::PageRef ref = cache_.fetch(find_leaf(key));
    LeafNode leaf = decode_leaf(ref.read());
    const auto it = leaf_lower_bound(leaf, key);
    if (it == leaf.cells.end() || it->first != key) {
        return false;
    }
    leaf.cells.erase(it);
    encode_leaf(leaf, ref.write());  // no merge: see the class comment
    return true;
}

std::vector<std::pair<std::string, std::string>> BPlusTree::scan(std::string_view start, std::string_view end,
                                                                 std::size_t limit) {
    std::vector<std::pair<std::string, std::string>> out;
    if (limit == 0) {
        return out;
    }
    std::uint32_t page_count = 0;
    {
        PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
        page_count = Meta::decode(meta_ref.read()).page_count;
    }
    PageId id = find_leaf(start);
    for (std::uint32_t hops = 0; id != kNoPage; ++hops) {
        if (hops > page_count) {
            throw CorruptionError("leaf chain longer than the file (cycle?)");
        }
        PageCache::PageRef ref = cache_.fetch(id);
        LeafNode leaf = decode_leaf(ref.read());
        for (auto& [key, value] : leaf.cells) {
            if (std::string_view(key).compare(start) < 0) {
                continue;
            }
            if (!end.empty() && std::string_view(key).compare(end) >= 0) {
                return out;
            }
            out.emplace_back(std::move(key), std::move(value));
            if (out.size() >= limit) {
                return out;
            }
        }
        id = leaf.next;
    }
    return out;
}

std::size_t BPlusTree::check() {
    struct Checker {
        explicit Checker(PageCache& c) : cache(c) {}
        PageCache& cache;
        std::uint32_t page_count = 0;
        int leaf_depth = -1;
        std::size_t keys = 0;
        std::vector<PageId> leaves;  // in key order

        void visit(PageId id, const std::string* lo, const std::string* hi, int depth) {
            if (depth > kMaxDepth || id == kNoPage || id >= page_count) {
                throw CorruptionError("check: bad child pointer or depth");
            }
            PageCache::PageRef ref = cache.fetch(id);
            if (ref.read().type() == PageType::Leaf) {
                const LeafNode leaf = decode_leaf(ref.read());
                if (leaf_depth == -1) {
                    leaf_depth = depth;
                } else if (leaf_depth != depth) {
                    throw CorruptionError("check: leaves at different depths");
                }
                for (std::size_t i = 0; i < leaf.cells.size(); ++i) {
                    const std::string& key = leaf.cells[i].first;
                    if (i > 0 && !(leaf.cells[i - 1].first < key)) {
                        throw CorruptionError("check: leaf keys not strictly increasing");
                    }
                    if ((lo != nullptr && key < *lo) || (hi != nullptr && !(key < *hi))) {
                        throw CorruptionError("check: leaf key outside its separator bounds");
                    }
                }
                keys += leaf.cells.size();
                leaves.push_back(id);
                return;
            }
            const InternalNode node = decode_internal(ref.read());
            ref = PageCache::PageRef();  // unpin before descending: pins stay O(1), not O(depth)
            if (node.children.size() != node.keys.size() + 1) {
                throw CorruptionError("check: internal node child count");
            }
            for (std::size_t i = 0; i < node.keys.size(); ++i) {
                if ((i > 0 && !(node.keys[i - 1] < node.keys[i])) || (lo != nullptr && node.keys[i] < *lo) ||
                    (hi != nullptr && !(node.keys[i] < *hi))) {
                    throw CorruptionError("check: separators out of order or out of bounds");
                }
            }
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                const std::string* child_lo = i == 0 ? lo : &node.keys[i - 1];
                const std::string* child_hi = i == node.keys.size() ? hi : &node.keys[i];
                visit(node.children[i], child_lo, child_hi, depth + 1);
            }
        }
    };

    Checker checker(cache_);
    {
        PageCache::PageRef meta_ref = cache_.fetch(kMetaPageId);
        checker.page_count = Meta::decode(meta_ref.read()).page_count;
    }
    checker.visit(root(), nullptr, nullptr, 0);

    // The sibling chain must visit exactly the leaves the tree walk found, in the same order.
    for (std::size_t i = 0; i < checker.leaves.size(); ++i) {
        PageCache::PageRef ref = cache_.fetch(checker.leaves[i]);
        const PageId expected_next = i + 1 < checker.leaves.size() ? checker.leaves[i + 1] : kNoPage;
        if (decode_leaf(ref.read()).next != expected_next) {
            throw CorruptionError("check: leaf chain does not match tree order");
        }
    }
    return checker.keys;
}

}  // namespace minidb
