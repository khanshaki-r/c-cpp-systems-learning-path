// m06_vector_growth.cpp
// std::vector growth, capacity, reserve, and iterator/pointer invalidation.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m06_vector_growth.cpp
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v;
    std::size_t last_cap = v.capacity();
    std::cout << "initial capacity=" << last_cap << '\n';
    for (int i = 0; i < 100; ++i) {
        v.push_back(i);
        if (v.capacity() != last_cap) {   // libstdc++ doubles: 1, 2, 4, 8, ... (MSVC grows by 1.5x)
            std::cout << "size=" << v.size() << " capacity " << last_cap << " -> " << v.capacity()
                      << "  (reallocated: every pointer/iterator/reference into the old buffer now dangles)\n";
            last_cap = v.capacity();
        }
    }

    // reserve(): one allocation up front, no reallocation during the pushes.
    std::vector<int> w;
    w.reserve(100);
    auto before = reinterpret_cast<std::uintptr_t>(w.data());   // stored as an integer, so comparing later is not UB
    for (int i = 0; i < 100; ++i) w.push_back(i);
    auto after = reinterpret_cast<std::uintptr_t>(w.data());
    std::cout << "with reserve(100): buffer moved? " << (before != after ? "yes" : "no")
              << ", size=" << w.size() << " capacity=" << w.capacity() << '\n';

    // clear() destroys elements but KEEPS the capacity. shrink_to_fit() is a request, not a guarantee.
    w.clear();
    std::cout << "after clear: size=" << w.size() << " capacity=" << w.capacity() << '\n';
    w.shrink_to_fit();
    std::cout << "after shrink_to_fit: capacity=" << w.capacity() << '\n';

    // Inserting while iterating: use an INDEX, never a cached iterator, because insert may reallocate.
    std::vector<int> nums{1, 2, 3, 4, 5};
    for (std::size_t i = 0; i < nums.size(); ++i) {
        if (nums[i] % 2 == 0) {
            nums.insert(nums.begin() + static_cast<std::ptrdiff_t>(i) + 1, -1);   // a fresh iterator each time
            ++i;   // skip the element we just inserted
        }
    }
    for (int x : nums) std::cout << x << ' ';
    std::cout << '\n';

    // Erasing while iterating: erase() returns the next valid iterator. Use it.
    for (auto it = nums.begin(); it != nums.end();) {
        if (*it < 0) it = nums.erase(it);   // elements after 'it' shift left; old iterators past 'it' are invalid
        else ++it;
    }
    for (int x : nums) std::cout << x << ' ';
    std::cout << '\n';

    // sizeof(std::vector) is three pointers (begin, end, capacity_end) regardless of element count.
    std::cout << "sizeof(std::vector<int>)=" << sizeof(std::vector<int>) << '\n';
    return 0;
}
