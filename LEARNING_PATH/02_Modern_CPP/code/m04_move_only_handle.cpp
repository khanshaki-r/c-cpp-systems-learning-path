// m04_move_only_handle.cpp
// Move-only types, moved-from state, and the rule of five written correctly (copy-and-swap).
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m04_move_only_handle.cpp
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <utility>
#include <vector>

// ---- A move-only handle (no copy at all) ---------------------------------------------------
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { if (fd_ != -1) std::cout << "  close(" << fd_ << ")\n"; }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}   // noexcept matters: see main()
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) {
            if (fd_ != -1) std::cout << "  close(" << fd_ << ")\n";
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
private:
    int fd_ = -1;
};

// ---- A copyable AND movable resource owner: full rule of five --------------------------------
class Buffer {
public:
    Buffer() noexcept = default;
    explicit Buffer(std::size_t n) : data_(static_cast<char*>(std::malloc(n))), size_(n) {
        if (!data_ && n) throw std::bad_alloc();
        std::memset(data_, 0, n);
        std::cout << "  alloc " << n << '\n';
    }
    ~Buffer() {
        if (data_) std::cout << "  free " << size_ << '\n';
        std::free(data_);   // free(nullptr) is a no-op, so a moved-from Buffer is safe to destroy
    }
    Buffer(const Buffer& o) : data_(o.size_ ? static_cast<char*>(std::malloc(o.size_)) : nullptr), size_(o.size_) {
        if (size_) {
            if (!data_) throw std::bad_alloc();
            std::memcpy(data_, o.data_, size_);   // deep copy: two Buffers must never share one block
        }
        std::cout << "  copy " << size_ << '\n';
    }
    Buffer(Buffer&& o) noexcept : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {
        std::cout << "  move " << size_ << '\n';   // no allocation, no copy: just two pointer-sized stores
    }
    // Unified assignment via copy-and-swap. 'o' is a copy (if the caller passed an lvalue) or a
    // move (if an rvalue). Swapping cannot throw, so this gives the strong exception guarantee and
    // handles self-assignment for free: the old block dies with 'o' at the closing brace.
    Buffer& operator=(Buffer o) noexcept {
        swap(*this, o);
        return *this;
    }
    friend void swap(Buffer& a, Buffer& b) noexcept {
        using std::swap;
        swap(a.data_, b.data_);
        swap(a.size_, b.size_);
    }
    std::size_t size() const noexcept { return size_; }
    char* data() noexcept { return data_; }
private:
    char* data_ = nullptr;
    std::size_t size_ = 0;
};

int main() {
    std::cout << "-- move-only handles in a vector\n";
    std::vector<UniqueFd> fds;
    fds.push_back(UniqueFd(3));         // rvalue: moved in
    UniqueFd f(4);
    fds.push_back(std::move(f));        // f is now in the moved-from state: valid, holds -1
    // fds.push_back(f);                // error: copy constructor is deleted
    std::cout << "f after move holds " << f.get() << '\n';
    fds.emplace_back(5);                // constructed directly inside the vector's storage
    // When the vector grows it MOVES elements only because UniqueFd's move ctor is noexcept.
    // Without noexcept, std::vector falls back to copying (and here, would fail to compile).
    for (const auto& x : fds) std::cout << x.get() << ' ';
    std::cout << '\n';
    // std::vector<UniqueFd> copy = fds;        // error: element type is not copyable
    std::vector<UniqueFd> moved = std::move(fds);  // O(1): steals the buffer pointer; fds is left empty
    std::cout << "fds.size()=" << fds.size() << " moved.size()=" << moved.size() << '\n';

    std::cout << "-- rule of five\n";
    Buffer b(16);
    Buffer c = b;               // copy ctor: alloc + memcpy
    Buffer d = std::move(b);    // move ctor: pointer steal; b.size()==0 now
    std::cout << "b.size()=" << b.size() << " d.size()=" << d.size() << '\n';
    c = d;                      // copy assignment: 'o' is a copy of d, swapped in, old 16-byte block freed
    d = Buffer(8);              // move assignment: temporary moved into 'o', swapped in, old block freed
    // d = std::move(d);        // self-move: harmless with copy-and-swap (d ends up empty, which is allowed),
                                // but GCC 13's -Wself-move (in -Wall) rightly flags it as a likely bug.
    std::cout << "end of main\n";
    return 0;
}
