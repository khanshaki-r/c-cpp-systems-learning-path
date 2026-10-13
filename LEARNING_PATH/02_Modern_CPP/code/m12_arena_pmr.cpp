// m12_arena_pmr.cpp
// A hand-written arena (bump) allocator with placement new, and the C++17 std::pmr equivalent.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_arena_pmr.cpp
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <string>
#include <utility>
#include <vector>

// The C idea "one big block per request, free everything at the end" in C++ clothes.
class Arena {
public:
    explicit Arena(std::size_t bytes) : buf_(new std::byte[bytes]), cap_(bytes) {}
    ~Arena() { delete[] buf_; }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(std::size_t n, std::size_t align = alignof(std::max_align_t)) {
        std::size_t aligned = (used_ + align - 1) & ~(align - 1);   // round up to the alignment (power of two)
        if (aligned + n > cap_) throw std::bad_alloc();
        used_ = aligned + n;
        ++count_;
        return buf_ + aligned;   // no per-allocation header, no free list, no locking
    }
    template <class T, class... Args>
    T* make(Args&&... args) {   // placement new: construct T in memory we already own
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }
    void reset() noexcept { used_ = 0; count_ = 0; }   // O(1) "free everything". Only for trivially destructible T!
    std::size_t used() const noexcept { return used_; }
    std::size_t count() const noexcept { return count_; }
private:
    std::byte*  buf_;
    std::size_t cap_;
    std::size_t used_ = 0;
    std::size_t count_ = 0;
};

struct Request { int id; double amount; char type; };

// A memory_resource that counts calls, so we can SEE how many times a container hits the heap.
class CountingResource : public std::pmr::memory_resource {
public:
    int allocs = 0;
    int deallocs = 0;
private:
    void* do_allocate(std::size_t n, std::size_t align) override {
        ++allocs;
        return std::pmr::new_delete_resource()->allocate(n, align);
    }
    void do_deallocate(void* p, std::size_t n, std::size_t align) override {
        ++deallocs;
        std::pmr::new_delete_resource()->deallocate(p, n, align);
    }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
};

int main() {
    Arena arena(4096);
    for (int round = 0; round < 3; ++round) {
        Request* a = arena.make<Request>(Request{1, 9.5, 'P'});
        Request* b = arena.make<Request>(Request{2, 0.5, 'G'});
        char* scratch = static_cast<char*>(arena.allocate(100, 1));
        scratch[0] = 'x';
        std::cout << "round " << round << ": a.id=" << a->id << " b.id=" << b->id << " used=" << arena.used()
                  << " allocations=" << arena.count() << '\n';
        arena.reset();   // the whole request's memory is "freed" in O(1); no per-object bookkeeping
    }

    // std::pmr: the standard library's pluggable version of the same idea.
    std::byte stack_buf[2048];
    CountingResource upstream;
    // monotonic_buffer_resource: bump allocator over stack_buf; asks 'upstream' only when the buffer is full.
    std::pmr::monotonic_buffer_resource pool(stack_buf, sizeof stack_buf, &upstream);
    {
        std::pmr::vector<std::pmr::string> names(&pool);   // the vector buffer AND each string buffer come from 'pool'
        for (int i = 0; i < 20; ++i)
            names.emplace_back(("connection-" + std::to_string(i) + "-long-enough-to-defeat-sso").c_str());
        std::cout << "pmr vector of " << names.size() << " strings: heap calls via upstream=" << upstream.allocs << '\n';
    }   // destructors run, but monotonic_buffer_resource ignores deallocate(): memory returns when 'pool' is released

    CountingResource direct;
    {
        std::pmr::vector<std::pmr::string> names(&direct);  // same work, every allocation goes to the heap
        for (int i = 0; i < 20; ++i)
            names.emplace_back(("connection-" + std::to_string(i) + "-long-enough-to-defeat-sso").c_str());
        std::cout << "without the pool: heap allocations=" << direct.allocs << " (vector growth + 20 strings)\n";
    }
    std::cout << "after scope: deallocations=" << direct.deallocs << '\n';
    pool.release();
    return 0;
}
