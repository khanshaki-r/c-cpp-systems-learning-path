// m03_object_pool_arena.cpp
// Two ownership strategies for high-rate allocation in a server:
//   1. Object pool: fixed set of reusable objects, handed out and returned.
//      Returned objects are recycled through a custom deleter on unique_ptr, so
//      "return to pool" cannot be forgotten.
//   2. Arena (bump allocator): allocate many small things during one request,
//      free them all at once when the request ends. No per-object free.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m03b m03_object_pool_arena.cpp

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Object pool
// ---------------------------------------------------------------------------
struct RequestBuffer {
    char data[4096];
    std::size_t len = 0;
    void reset() { len = 0; }
};

class BufferPool {
public:
    // The deleter does NOT free; it hands the object back. unique_ptr with a
    // custom deleter type is the C++ equivalent of "every path must call
    // pool_release()", enforced by the compiler.
    struct Returner {
        BufferPool* pool;
        void operator()(RequestBuffer* b) const { pool->release(b); }
    };
    using Handle = std::unique_ptr<RequestBuffer, Returner>;

    explicit BufferPool(std::size_t n) {
        storage_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            storage_.push_back(std::make_unique<RequestBuffer>());
            free_.push_back(storage_.back().get());
        }
    }

    // Returns an empty handle if exhausted. Callers must check: this is
    // back-pressure, and it is better than unbounded heap growth under load.
    Handle acquire() {
        if (free_.empty()) return Handle(nullptr, Returner{this});
        RequestBuffer* b = free_.back();
        free_.pop_back();
        b->reset();
        return Handle(b, Returner{this});
    }

    std::size_t available() const { return free_.size(); }

private:
    void release(RequestBuffer* b) { free_.push_back(b); }

    std::vector<std::unique_ptr<RequestBuffer>> storage_;  // pool OWNS all buffers
    std::vector<RequestBuffer*> free_;                     // free list: observer pointers
};

// ---------------------------------------------------------------------------
// 2. Arena
// ---------------------------------------------------------------------------
class Arena {
public:
    explicit Arena(std::size_t bytes) : buf_(new std::byte[bytes]), cap_(bytes) {}
    ~Arena() { delete[] buf_; }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Bump allocation: round up to alignment, hand out, advance. O(1), no free list.
    void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t)) {
        std::size_t start = (used_ + align - 1) & ~(align - 1);
        if (start + size > cap_) throw std::bad_alloc();
        used_ = start + size;
        return buf_ + start;
    }

    // Construct a T inside the arena. Only for trivially destructible T, or
    // you must run destructors yourself before reset(). We restrict it here.
    template <class T, class... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena objects must not need destructors (nothing will call them)");
        void* p = allocate(sizeof(T), alignof(T));
        return new (p) T(std::forward<Args>(args)...);
    }

    // Copy a string into the arena and return a view that lives as long as the arena.
    const char* intern(const std::string& s) {
        char* p = static_cast<char*>(allocate(s.size() + 1, 1));
        std::memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }

    void reset() { used_ = 0; }  // "free everything": one store
    std::size_t used() const { return used_; }

private:
    std::byte* buf_;
    std::size_t cap_;
    std::size_t used_ = 0;
};

// A parsed field that points into arena memory. Trivially destructible.
struct Field {
    const char* name;
    const char* value;
};

// Simulate parsing a request into N fields, all allocated in the arena.
// At the end of the request we reset the arena: N frees become one.
static void handle_request(Arena& arena, int request_no) {
    arena.reset();
    std::vector<Field*> fields;  // the vector itself is heap; in a real server it would be arena too
    for (int i = 0; i < 5; ++i) {
        Field* f = arena.make<Field>();
        f->name = arena.intern("field" + std::to_string(i));
        f->value = arena.intern("value" + std::to_string(request_no * 10 + i));
        fields.push_back(f);
    }
    std::printf("request %d: %zu fields, arena used %zu bytes, last=%s=%s\n", request_no,
                fields.size(), arena.used(), fields.back()->name, fields.back()->value);
}

int main() {
    // --- pool ---
    BufferPool pool(2);
    std::printf("pool available: %zu\n", pool.available());
    {
        auto b1 = pool.acquire();
        auto b2 = pool.acquire();
        auto b3 = pool.acquire();  // exhausted
        std::printf("b1 %s, b2 %s, b3 %s, available %zu\n", b1 ? "ok" : "null", b2 ? "ok" : "null",
                    b3 ? "ok" : "null", pool.available());
        if (b1) {
            std::memcpy(b1->data, "hello", 5);
            b1->len = 5;
        }
    }  // b1, b2 returned here automatically; b3 was null, deleter not called
    std::printf("pool available after scope: %zu\n", pool.available());

    // --- arena ---
    Arena arena(64 * 1024);
    for (int r = 1; r <= 3; ++r) handle_request(arena, r);
    return 0;
}
