// m04_invalid_iterator.cpp - erasing from a std::vector while iterating over it with a
// range-for; also keeping a reference across push_back.
//
// Build:  g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_invalid_iterator m04_invalid_iterator.cpp
// Run:    ./build/m04_invalid_iterator
//         May print duplicates, skip elements, or crash. Depends on reallocation.
//
// Build with the libstdc++ debug mode to catch it at the point of misuse:
//         g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -D_GLIBCXX_DEBUG -o build/m04_invalid_iterator_dbg
//             m04_invalid_iterator.cpp
//         ./build/m04_invalid_iterator_dbg
//         /usr/include/c++/13/debug/safe_iterator.h:NNN:
//           Error: attempt to increment a singular iterator.
//         ... followed by abort(). Debug mode changes container ABI; use it only for a whole build.
//         Cheaper: -D_GLIBCXX_ASSERTIONS adds bounds checks to operator[] without changing ABI.
//
// ASan build: heap-use-after-free when the vector reallocates and the old buffer is read
//             (container-overflow if ASan's container annotations are active).
//
// Warnings with -Wall -Wextra: none.
#include <cstdio>
#include <vector>

int main()
{
    std::vector<int> v = {1, 2, 3, 4, 5, 6};

    // Bug 1: erase inside range-for. The hidden iterator is invalidated by erase().
    for (int &x : v) {
        if (x % 2 == 0)
            v.erase(v.begin() + (&x - &v[0]));   // invalidates every iterator at or after this point
    }

    // Bug 2: reference into a vector kept across push_back (reallocation moves the elements).
    std::vector<int> w;
    w.reserve(1);
    w.push_back(10);
    int &first = w[0];
    for (int i = 0; i < 100; ++i) w.push_back(i);  // reallocates; `first` now dangles
    std::printf("first=%d (reads freed memory)\n", first);

    for (int x : v) std::printf("%d ", x);
    std::puts("");
    return 0;
}
