// m12_heap.cpp
// Binary heap from scratch (push/pop/top, O(n) build, comparator), an indexed heap with
// decrease_key/erase, invariant checks and a differential test against std::priority_queue;
// then the classic problems: top-k, k-th largest, merge k sorted, median of a stream,
// k most frequent, k closest points.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m12_heap.cpp -o m12_heap

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <queue>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// std::less<T> -> max-heap (largest on top), matching std::priority_queue's convention.
template <typename T, typename Compare = std::less<T>>
class BinaryHeap {
public:
    BinaryHeap() = default;
    // O(n) heapify: sift down every internal node, bottom-up.
    explicit BinaryHeap(std::vector<T> v, Compare c = Compare{}) : a_(std::move(v)), less_(c) {
        for (std::size_t i = a_.size() / 2; i-- > 0;) sift_down(i);
    }
    bool empty() const { return a_.empty(); }
    std::size_t size() const { return a_.size(); }
    const T& top() const { return a_[0]; }

    void push(T x) {
        a_.push_back(std::move(x));
        sift_up(a_.size() - 1);
    }
    T pop() {
        T out = std::move(a_[0]);
        a_[0] = std::move(a_.back());
        a_.pop_back();
        if (!a_.empty()) sift_down(0);
        return out;
    }
    bool is_heap() const {
        for (std::size_t i = 1; i < a_.size(); ++i)
            if (less_(a_[(i - 1) / 2], a_[i])) return false;   // a parent smaller than its child
        return true;
    }

private:
    void sift_up(std::size_t i) {
        while (i > 0) {
            std::size_t p = (i - 1) / 2;
            if (!less_(a_[p], a_[i])) return;        // parent is not smaller: done
            std::swap(a_[p], a_[i]);
            i = p;
        }
    }
    void sift_down(std::size_t i) {
        const std::size_t n = a_.size();
        while (true) {
            std::size_t l = 2 * i + 1, r = l + 1, best = i;
            if (l < n && less_(a_[best], a_[l])) best = l;
            if (r < n && less_(a_[best], a_[r])) best = r;
            if (best == i) return;
            std::swap(a_[i], a_[best]);
            i = best;
        }
    }
    std::vector<T> a_;
    Compare less_;
};

// Indexed MIN-heap over ids 0..n-1 with a priority each: supports decrease_key and erase by id.
// pos_[id] tracks where id lives in the array so we can sift from there.
class IndexedMinHeap {
public:
    explicit IndexedMinHeap(std::size_t n) : pos_(n, kAbsent) {}
    bool contains(std::size_t id) const { return pos_[id] != kAbsent; }
    bool empty() const { return heap_.empty(); }
    std::size_t size() const { return heap_.size(); }
    std::size_t top_id() const { return heap_[0]; }
    long long top_key() const { return key_[heap_[0]]; }

    void push(std::size_t id, long long key) {
        assert(!contains(id));
        if (key_.size() <= id) key_.resize(id + 1);
        key_[id] = key;
        pos_[id] = heap_.size();
        heap_.push_back(id);
        sift_up(heap_.size() - 1);
    }
    std::size_t pop() {
        std::size_t id = heap_[0];
        erase(id);
        return id;
    }
    void decrease_key(std::size_t id, long long key) {
        assert(contains(id) && key <= key_[id]);
        key_[id] = key;
        sift_up(pos_[id]);
    }
    void erase(std::size_t id) {
        std::size_t i = pos_[id];
        std::size_t last = heap_.size() - 1;
        if (i != last) {
            place(heap_[last], i);
            heap_.pop_back();
            // The moved element may need to go either way.
            sift_down(i);
            sift_up(pos_[heap_[i < heap_.size() ? i : 0]]);   // sift_up is a no-op if already fine
        } else {
            heap_.pop_back();
        }
        pos_[id] = kAbsent;
    }

private:
    static constexpr std::size_t kAbsent = static_cast<std::size_t>(-1);
    void place(std::size_t id, std::size_t i) { heap_[i] = id; pos_[id] = i; }
    void swap_at(std::size_t i, std::size_t j) {
        std::size_t a = heap_[i], b = heap_[j];
        place(b, i);
        place(a, j);
    }
    void sift_up(std::size_t i) {
        while (i > 0) {
            std::size_t p = (i - 1) / 2;
            if (key_[heap_[p]] <= key_[heap_[i]]) return;
            swap_at(p, i);
            i = p;
        }
    }
    void sift_down(std::size_t i) {
        const std::size_t n = heap_.size();
        while (true) {
            std::size_t l = 2 * i + 1, r = l + 1, best = i;
            if (l < n && key_[heap_[l]] < key_[heap_[best]]) best = l;
            if (r < n && key_[heap_[r]] < key_[heap_[best]]) best = r;
            if (best == i) return;
            swap_at(i, best);
            i = best;
        }
    }
    std::vector<std::size_t> heap_;   // ids in heap order
    std::vector<std::size_t> pos_;    // id -> index in heap_, or kAbsent
    std::vector<long long> key_;      // id -> priority
};

namespace {

// Top-k largest with a min-heap of size k: O(n log k), O(k) memory, streaming.
std::vector<int> top_k(const std::vector<int>& v, std::size_t k) {
    std::priority_queue<int, std::vector<int>, std::greater<int>> minq;
    for (int x : v) {
        if (minq.size() < k) minq.push(x);
        else if (x > minq.top()) { minq.pop(); minq.push(x); }
    }
    std::vector<int> out;
    while (!minq.empty()) { out.push_back(minq.top()); minq.pop(); }
    std::reverse(out.begin(), out.end());   // largest first
    return out;
}

int kth_largest(std::vector<int> v, std::size_t k) {
    // nth_element puts the element that would be at index k-1 in descending order in place: O(n) avg.
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k - 1), v.end(), std::greater<int>());
    return v[k - 1];
}

// Merge k sorted vectors with a heap of (value, list, index): O(N log k).
std::vector<int> merge_k(const std::vector<std::vector<int>>& lists) {
    using Item = std::tuple<int, std::size_t, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (std::size_t i = 0; i < lists.size(); ++i)
        if (!lists[i].empty()) q.emplace(lists[i][0], i, 0);
    std::vector<int> out;
    while (!q.empty()) {
        auto [val, li, idx] = q.top();
        q.pop();
        out.push_back(val);
        if (idx + 1 < lists[li].size()) q.emplace(lists[li][idx + 1], li, idx + 1);
    }
    return out;
}

// Median of a stream: lower half in a max-heap, upper half in a min-heap, sizes within 1.
class StreamMedian {
public:
    void add(int x) {
        if (lo_.empty() || x <= lo_.top()) lo_.push(x); else hi_.push(x);
        if (lo_.size() > hi_.size() + 1) { hi_.push(lo_.top()); lo_.pop(); }
        else if (hi_.size() > lo_.size()) { lo_.push(hi_.top()); hi_.pop(); }
    }
    double median() const {
        if (lo_.size() > hi_.size()) return lo_.top();
        return (static_cast<double>(lo_.top()) + hi_.top()) / 2.0;
    }
private:
    std::priority_queue<int> lo_;                                        // max-heap
    std::priority_queue<int, std::vector<int>, std::greater<int>> hi_;  // min-heap
};

std::vector<int> k_most_frequent(const std::vector<int>& v, std::size_t k) {
    std::unordered_map<int, int> freq;
    for (int x : v) ++freq[x];
    using Item = std::pair<int, int>;                                    // (count, value)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> minq;  // least frequent on top
    for (const auto& kv : freq) {
        minq.emplace(kv.second, kv.first);
        if (minq.size() > k) minq.pop();
    }
    std::vector<int> out;
    while (!minq.empty()) { out.push_back(minq.top().second); minq.pop(); }
    return out;
}

std::vector<std::pair<int, int>> k_closest(const std::vector<std::pair<int, int>>& pts, std::size_t k) {
    using Item = std::pair<long long, std::size_t>;                      // (dist^2, index)
    std::priority_queue<Item> maxq;                                      // farthest of the k on top
    for (std::size_t i = 0; i < pts.size(); ++i) {
        long long d = 1LL * pts[i].first * pts[i].first + 1LL * pts[i].second * pts[i].second;
        if (maxq.size() < k) maxq.emplace(d, i);
        else if (d < maxq.top().first) { maxq.pop(); maxq.emplace(d, i); }
    }
    std::vector<std::pair<int, int>> out;
    while (!maxq.empty()) { out.push_back(pts[maxq.top().second]); maxq.pop(); }
    return out;
}

}  // namespace

int main() {
    std::mt19937 rng(3);

    // ---- BinaryHeap vs std::priority_queue -------------------------------------------------
    {
        BinaryHeap<int> mine;
        std::priority_queue<int> ref;
        for (int step = 0; step < 200'000; ++step) {
            if (ref.empty() || rng() % 3) { int x = static_cast<int>(rng() % 100000); mine.push(x); ref.push(x); }
            else { assert(mine.pop() == ref.top()); ref.pop(); }
            assert(mine.size() == ref.size());
        }
        assert(mine.is_heap());

        // O(n) build vs n pushes.
        const std::size_t n = 5'000'000;
        std::vector<int> data(n);
        for (int& x : data) x = static_cast<int>(rng());
        auto t0 = std::chrono::steady_clock::now();
        BinaryHeap<int> built(data);
        double t_build = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t0 = std::chrono::steady_clock::now();
        BinaryHeap<int> pushed;
        for (int x : data) pushed.push(x);
        double t_push = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        assert(built.is_heap() && built.top() == pushed.top());
        std::printf("heapify %zu ints: build O(n) %.1f ms, n pushes O(n log n) %.1f ms\n", n, t_build, t_push);

        // Min-heap via std::greater.
        BinaryHeap<int, std::greater<int>> minh({5, 1, 4, 2, 3});
        assert(minh.pop() == 1 && minh.pop() == 2 && minh.top() == 3);
    }

    // ---- IndexedMinHeap vs std::set mirror ----------------------------------------------------
    {
        const std::size_t n = 2000;
        IndexedMinHeap h(n);
        std::set<std::pair<long long, std::size_t>> ref;
        std::vector<long long> key(n, 0);
        for (int step = 0; step < 300'000; ++step) {
            std::size_t id = rng() % n;
            unsigned op = rng() % 4;
            if (!h.contains(id)) {
                long long k = static_cast<long long>(rng() % 1'000'000);
                h.push(id, k); key[id] = k; ref.emplace(k, id);
            } else if (op == 0) {
                h.erase(id); ref.erase({key[id], id});
            } else if (op == 1) {
                long long k = key[id] - static_cast<long long>(rng() % 1000);
                ref.erase({key[id], id}); key[id] = k; ref.emplace(k, id);
                h.decrease_key(id, k);
            } else if (op == 2 && !h.empty()) {
                assert(h.top_key() == ref.begin()->first);
                std::size_t popped = h.pop();
                assert(ref.begin()->first == key[popped]);     // same key (ids may tie)
                ref.erase({key[popped], popped});
            }
            assert(h.size() == ref.size());
        }
        std::printf("IndexedMinHeap vs std::set: %zu live entries after 300k ops, consistent\n", h.size());
    }

    // ---- the problems ------------------------------------------------------------------------
    assert((top_k({3, 2, 1, 5, 6, 4}, 2) == std::vector<int>{6, 5}));
    assert(kth_largest({3, 2, 3, 1, 2, 4, 5, 5, 6}, 4) == 4);
    assert(kth_largest({3, 2, 1, 5, 6, 4}, 2) == 5);
    assert((merge_k({{1, 4, 5}, {1, 3, 4}, {2, 6}}) == std::vector<int>{1, 1, 2, 3, 4, 4, 5, 6}));
    assert(merge_k({}).empty() && merge_k({{}}).empty());

    StreamMedian sm;
    std::vector<double> medians;
    for (int x : {5, 15, 1, 3}) { sm.add(x); medians.push_back(sm.median()); }
    assert((medians == std::vector<double>{5, 10, 5, 4}));

    {
        auto mf = k_most_frequent({1, 1, 1, 2, 2, 3}, 2);
        std::sort(mf.begin(), mf.end());
        assert((mf == std::vector<int>{1, 2}));
    }
    {
        auto kc = k_closest({{1, 3}, {-2, 2}}, 1);
        assert(kc.size() == 1 && kc[0] == std::make_pair(-2, 2));
        kc = k_closest({{3, 3}, {5, -1}, {-2, 4}}, 2);
        std::sort(kc.begin(), kc.end());
        assert((kc == std::vector<std::pair<int, int>>{{-2, 4}, {3, 3}}));
    }
    std::printf("all heap checks passed\n");
    return 0;
}
