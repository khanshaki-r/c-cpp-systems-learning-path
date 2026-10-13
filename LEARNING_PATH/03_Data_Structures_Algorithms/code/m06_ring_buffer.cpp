// m06_ring_buffer.cpp
// A fixed-capacity ring buffer with power-of-two capacity, monotonically increasing 64-bit
// head/tail counters (no full/empty ambiguity), reject-when-full and overwrite-oldest
// policies, and bulk write/read of bytes that may wrap, the way a socket buffer is used.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m06_ring_buffer.cpp -o m06_ring

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

template <typename T>
class RingBuffer {
public:
    enum class Policy { Reject, Overwrite };

    // capacity is rounded up to a power of two so index = counter & mask (no division).
    explicit RingBuffer(std::size_t min_capacity, Policy policy = Policy::Reject)
        : policy_(policy) {
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    std::size_t capacity() const { return buf_.size(); }
    std::size_t size() const { return static_cast<std::size_t>(tail_ - head_); }   // counters never wrap in practice (2^64)
    bool empty() const { return head_ == tail_; }
    bool full() const { return size() == capacity(); }

    bool push(const T& x) {
        if (full()) {
            if (policy_ == Policy::Reject) return false;
            ++head_;                                  // drop the oldest
            ++dropped_;
        }
        buf_[static_cast<std::size_t>(tail_ & mask_)] = x;
        ++tail_;
        return true;
    }

    std::optional<T> pop() {
        if (empty()) return std::nullopt;
        T x = buf_[static_cast<std::size_t>(head_ & mask_)];
        ++head_;
        return x;
    }

    // i-th element from the front without removing it.
    const T& peek(std::size_t i) const { return buf_[static_cast<std::size_t>((head_ + i) & mask_)]; }

    // Bulk write: copy as many as fit, possibly in two pieces around the wrap point.
    std::size_t write(const T* src, std::size_t n) {
        std::size_t can = std::min(n, capacity() - size());
        std::size_t start = static_cast<std::size_t>(tail_ & mask_);
        std::size_t first = std::min(can, capacity() - start);     // up to the physical end
        std::copy(src, src + first, buf_.begin() + static_cast<std::ptrdiff_t>(start));
        std::copy(src + first, src + can, buf_.begin());            // wrapped remainder (maybe empty)
        tail_ += can;
        return can;
    }

    std::size_t read(T* dst, std::size_t n) {
        std::size_t can = std::min(n, size());
        std::size_t start = static_cast<std::size_t>(head_ & mask_);
        std::size_t first = std::min(can, capacity() - start);
        std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(start),
                  buf_.begin() + static_cast<std::ptrdiff_t>(start + first), dst);
        std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(can - first), dst + first);
        head_ += can;
        return can;
    }

    std::uint64_t dropped() const { return dropped_; }

private:
    std::vector<T> buf_;
    std::size_t mask_ = 0;
    std::uint64_t head_ = 0;     // next slot to read; also "sequence number" of the front element
    std::uint64_t tail_ = 0;     // next slot to write
    std::uint64_t dropped_ = 0;
    Policy policy_;
};

int main() {
    // ---- reject policy: behaves as a bounded queue ---------------------------------------
    RingBuffer<int> rb(5);                       // rounded up to 8
    assert(rb.capacity() == 8);
    for (int i = 0; i < 8; ++i) assert(rb.push(i));
    assert(rb.full() && !rb.push(99));           // rejected
    assert(rb.pop() == 0 && rb.pop() == 1);
    assert(rb.push(8) && rb.push(9));            // tail wraps around to indices 0 and 1
    assert(rb.size() == 8);
    for (int expect = 2; expect <= 9; ++expect) assert(rb.pop() == expect);   // FIFO survives the wrap
    assert(rb.empty() && !rb.pop().has_value());

    // ---- overwrite policy: a log ring that keeps the most recent N -----------------------
    RingBuffer<int> log(4, RingBuffer<int>::Policy::Overwrite);
    for (int i = 0; i < 10; ++i) log.push(i);
    assert(log.size() == 4 && log.dropped() == 6);
    assert(log.peek(0) == 6 && log.peek(3) == 9);
    std::printf("log ring holds the last 4: %d %d %d %d (dropped %llu)\n", log.peek(0), log.peek(1),
                log.peek(2), log.peek(3), static_cast<unsigned long long>(log.dropped()));

    // ---- byte stream with wrap: the socket-buffer use case -------------------------------
    RingBuffer<char> sock(16);
    const char* msg1 = "HELLO-WORLD";      // 11 bytes
    const char* msg2 = "ABCDEFGHIJ";       // 10 bytes
    assert(sock.write(msg1, std::strlen(msg1)) == 11);
    char out[32] = {};
    assert(sock.read(out, 6) == 6);        // consume "HELLO-"; head = 6, tail = 11
    assert(std::string(out, 6) == "HELLO-");
    // 10 more bytes: 5 fit before the physical end (11..15), 5 wrap to 0..4.
    assert(sock.write(msg2, std::strlen(msg2)) == 10);
    assert(sock.size() == 15);
    std::size_t got = sock.read(out, sizeof(out));
    assert(got == 15);
    assert(std::string(out, got) == "WORLDABCDEFGHIJ");
    std::printf("wrapped read gave: %.*s\n", static_cast<int>(got), out);

    // Partial write when nearly full returns how much fit (backpressure signal).
    RingBuffer<char> small(4);
    assert(small.write("abcdef", 6) == 4);
    assert(small.write("x", 1) == 0);

    std::printf("all ring-buffer checks passed\n");
    return 0;
}
