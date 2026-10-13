// m03_asan_demo.cpp - a one-past-the-end write that only AddressSanitizer reliably catches.
//
//   g++ -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
//       m03_asan_demo.cpp -o asan_demo && ./asan_demo
//
// Without ASan this usually "works": the extra int lands in allocator padding or the next
// heap block, and the corruption shows up later, somewhere else, or never.
#include <cstdio>
#include <vector>

int main(int argc, char**) {
    std::vector<int> v(4);
    // argc is 1 with no arguments, so idx == 4 == v.size(): one past the end.
    // Using argc stops the compiler from proving the index at compile time and warning.
    const int idx = argc + 3;
    v[static_cast<std::size_t>(idx)] = 42;   // heap-buffer-overflow: WRITE of size 4
    std::printf("v[0]=%d (reached only without ASan)\n", v[0]);
    return 0;
}
