// m01_vector_growth.cpp
// Demonstrates: vector capacity doubling, amortised copy count, grow-by-constant penalty,
// and the cache effect of row-major vs column-major traversal.
//
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m01_vector_growth.cpp -o m01
//
// We never write `using namespace std;`. In a real codebase names like size, count, distance
// and begin collide with user code once the whole std namespace is pulled into global scope.
// Five characters of `std::` tell every reader where a name comes from.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// A deliberately bad dynamic array: grows by a fixed step. Total copies are quadratic.
class StepArray {
public:
    explicit StepArray(std::size_t step) : step_(step) {}
    ~StepArray() { std::free(data_); }
    StepArray(const StepArray&) = delete;
    StepArray& operator=(const StepArray&) = delete;

    void push_back(int x) {
        if (size_ == cap_) {
            // Growth by a constant: after k growths capacity is k*step and we have copied
            // step + 2*step + ... + k*step = O(k^2 * step) elements in total.
            std::size_t new_cap = cap_ + step_;
            int* nd = static_cast<int*>(std::malloc(new_cap * sizeof(int)));
            for (std::size_t i = 0; i < size_; ++i) nd[i] = data_[i];
            copies_ += static_cast<long long>(size_);
            std::free(data_);
            data_ = nd;
            cap_ = new_cap;
        }
        data_[size_++] = x;
    }
    long long copies() const { return copies_; }

private:
    int* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cap_ = 0;
    std::size_t step_;
    long long copies_ = 0;
};

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
    // ---- Part 1: watch std::vector double its capacity -----------------------------------
    {
        std::vector<int> v;
        std::size_t last_cap = 0;
        long long copies = 0;
        for (int i = 0; i < 1000; ++i) {
            v.push_back(i);
            if (v.capacity() != last_cap) {
                // On a reallocation every element that was already present got moved.
                copies += static_cast<long long>(v.size() - 1);
                std::printf("size=%4zu capacity=%4zu\n", v.size(), v.capacity());
                last_cap = v.capacity();
            }
        }
        std::printf("std::vector: total element copies for 1000 pushes = %lld (bound 2n = 2000)\n\n",
                    copies);
    }

    // ---- Part 2: doubling vs constant-step growth at n = 1e6 -----------------------------
    {
        const std::size_t n = 1'000'000;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<int> a;
        for (std::size_t i = 0; i < n; ++i) a.push_back(static_cast<int>(i));
        double t_vec = seconds_since(t0);

        t0 = std::chrono::steady_clock::now();
        std::vector<int> b;
        b.reserve(n);  // one allocation, zero reallocations
        for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<int>(i));
        double t_reserved = seconds_since(t0);

        t0 = std::chrono::steady_clock::now();
        StepArray s(1024);
        for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<int>(i));
        double t_step = seconds_since(t0);

        std::printf("n = %zu\n", n);
        std::printf("  vector push_back (doubling):     %.4f s\n", t_vec);
        std::printf("  vector push_back after reserve:  %.4f s\n", t_reserved);
        std::printf("  grow-by-1024 array:              %.4f s, copies = %lld (~n^2/2048 = %lld)\n\n",
                    t_step, s.copies(), static_cast<long long>(n) * static_cast<long long>(n) / 2048);
    }

    // ---- Part 3: same O(n^2), very different wall clock: cache effects --------------------
    {
        const std::size_t N = 4096;  // 4096*4096*4 bytes = 64 MB, far bigger than L3
        std::vector<int> m(N * N, 1);

        auto t0 = std::chrono::steady_clock::now();
        long long sum_row = 0;
        for (std::size_t r = 0; r < N; ++r)
            for (std::size_t c = 0; c < N; ++c)
                sum_row += m[r * N + c];  // consecutive addresses: one cache line serves 16 ints
        double t_row = seconds_since(t0);

        t0 = std::chrono::steady_clock::now();
        long long sum_col = 0;
        for (std::size_t c = 0; c < N; ++c)
            for (std::size_t r = 0; r < N; ++r)
                sum_col += m[r * N + c];  // stride of N*4 = 16 KB: every access is a new line
        double t_col = seconds_since(t0);

        std::printf("matrix %zux%zu sum (both O(n^2), both = %lld)\n", N, N, sum_row);
        std::printf("  row-major:    %.4f s\n", t_row);
        std::printf("  column-major: %.4f s  (ratio %.1fx)\n", t_col,
                    t_row > 0 ? t_col / t_row : 0.0);
        if (sum_row != sum_col) return 1;  // keep the optimiser honest and check correctness
    }
    return 0;
}
