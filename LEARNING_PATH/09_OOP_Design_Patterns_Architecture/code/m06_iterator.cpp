// m06_iterator.cpp
// ITERATOR: walk a container without exposing its structure. In C++ this is
// not a pattern you implement with a virtual interface; it is a protocol the
// language and the standard library already speak: begin()/end(), operator*,
// operator++, operator!=. Implement it and your type works with range-for,
// std::find, std::accumulate and every algorithm.
//
// Here: a fixed-capacity ring buffer (the kind every server has for recent
// log lines or latency samples) with a forward iterator, plus a "generator"
// style iterator over a paginated result set (how you iterate a Berkeley DB
// cursor or a libpq result without loading everything).
//
// C equivalent: a cursor struct + next() function + "while (next(&cur, &item))".
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m06h m06_iterator.cpp

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Ring buffer with a proper forward iterator.
// ---------------------------------------------------------------------------
template <class T, std::size_t N>
class RingBuffer {
public:
    void push(T v) {
        buf_[(start_ + size_) % N] = std::move(v);
        if (size_ < N) ++size_;
        else start_ = (start_ + 1) % N;   // full: overwrite oldest
    }
    std::size_t size() const { return size_; }

    // The iterator. The five typedefs are what <algorithm> needs to know how
    // to use it (iterator_traits). forward_iterator_tag: can advance and
    // compare, cannot go back or jump.
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        iterator(const RingBuffer* rb, std::size_t pos) : rb_(rb), pos_(pos) {}
        reference operator*() const { return rb_->buf_[(rb_->start_ + pos_) % N]; }
        pointer operator->() const { return &**this; }
        iterator& operator++() { ++pos_; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++pos_; return tmp; }
        bool operator==(const iterator& o) const { return rb_ == o.rb_ && pos_ == o.pos_; }
        bool operator!=(const iterator& o) const { return !(*this == o); }

    private:
        const RingBuffer* rb_;   // observer: the iterator must not outlive the buffer
        std::size_t pos_;        // logical index 0..size, oldest first
    };

    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, size_); }

private:
    T buf_[N]{};
    std::size_t start_ = 0;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// 2. Paginated source iterator: pulls one page at a time from a "backend" and
//    yields rows one by one. The consumer never sees pages. This is the shape
//    of a DB cursor wrapper.
// ---------------------------------------------------------------------------
class PagedSource {
public:
    explicit PagedSource(int total_rows, int page_size) : total_(total_rows), page_(page_size) {}

    // Fetch page starting at offset. Simulates one DB round trip.
    std::vector<std::string> fetch(int offset) const {
        std::vector<std::string> rows;
        for (int i = offset; i < offset + page_ && i < total_; ++i) rows.push_back("row-" + std::to_string(i));
        ++round_trips_;
        return rows;
    }
    int round_trips() const { return round_trips_; }

private:
    int total_, page_;
    mutable int round_trips_ = 0;
};

class RowIterator {
public:
    using iterator_category = std::input_iterator_tag;   // single pass: once consumed, gone
    using value_type = std::string;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::string*;
    using reference = const std::string&;

    RowIterator() = default;                              // end sentinel
    explicit RowIterator(const PagedSource* src) : src_(src) { load(0); }

    reference operator*() const { return page_[idx_]; }
    pointer operator->() const { return &page_[idx_]; }
    RowIterator& operator++() {
        if (++idx_ >= page_.size()) load(offset_ + static_cast<int>(page_.size()));
        return *this;
    }
    RowIterator operator++(int) { RowIterator tmp = *this; ++*this; return tmp; }
    bool operator==(const RowIterator& o) const { return src_ == o.src_; }   // both end <=> both null
    bool operator!=(const RowIterator& o) const { return !(*this == o); }

private:
    void load(int offset) {
        offset_ = offset;
        page_ = src_->fetch(offset);
        idx_ = 0;
        if (page_.empty()) src_ = nullptr;   // exhausted: become the end sentinel
    }
    const PagedSource* src_ = nullptr;
    std::vector<std::string> page_;
    std::size_t idx_ = 0;
    int offset_ = 0;
};

struct Rows {   // range adaptor so range-for works
    const PagedSource& src;
    RowIterator begin() const { return RowIterator(&src); }
    RowIterator end() const { return RowIterator(); }
};

int main() {
    RingBuffer<int, 4> latencies;
    for (int v : {12, 7, 30, 5, 9, 41}) latencies.push(v);   // 12 and 7 overwritten

    std::printf("ring (oldest first):");
    for (int v : latencies) std::printf(" %d", v);            // range-for via begin()/end()
    std::printf("\n");

    int total = std::accumulate(latencies.begin(), latencies.end(), 0);
    auto worst = std::max_element(latencies.begin(), latencies.end());
    std::printf("sum=%d max=%d count=%zu\n", total, *worst, latencies.size());

    PagedSource src(7, 3);
    std::printf("rows:");
    for (const auto& row : Rows{src}) std::printf(" %s", row.c_str());
    std::printf("\nround trips: %d (7 rows, page size 3 => 3 pages + 1 empty probe)\n", src.round_trips());

    auto it = std::find(RowIterator(&src), RowIterator(), std::string("row-4"));
    std::printf("find row-4: %s\n", it != RowIterator() ? it->c_str() : "not found");
    return 0;
}
