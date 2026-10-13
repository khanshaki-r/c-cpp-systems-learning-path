// m02_vector_basics.cpp
// std::vector API tour: size vs capacity, insert/erase cost, iterator invalidation done
// right, the erase-remove idiom, and your array.cpp rewritten with a vector.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m02_vector_basics.cpp -o m02_basics
//
// No `using namespace std;` anywhere in this course: explicit std:: avoids name collisions
// (size, count, distance, data...) and tells the reader where every name comes from.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

void print(const char* label, const std::vector<int>& v) {
    std::printf("%-28s", label);
    for (int x : v) std::printf("%d ", x);
    std::printf("  (size=%zu cap=%zu)\n", v.size(), v.capacity());
}

}  // namespace

int main() {
    // ---- array.cpp, redone ----------------------------------------------------------------
    std::vector<int> arr = {1, 3, 2, 4, 5, 8, 6, 7, 9};
    print("initial", arr);

    // Linear search: std::find returns end() when not found. O(n).
    auto it = std::find(arr.begin(), arr.end(), 7);
    std::printf("find 7: %s\n", it != arr.end() ? "found" : "missing");

    // Deletion by value: find + erase. erase shifts the tail left, exactly like your loop,
    // but with memmove for trivially copyable types.
    it = std::find(arr.begin(), arr.end(), 9);
    if (it != arr.end()) arr.erase(it);
    print("after erase(9)", arr);

    // Insert in the middle: O(n - position) because the tail shifts right.
    arr.insert(arr.begin() + 2, 100);
    print("after insert(2, 100)", arr);

    // ---- size vs capacity -----------------------------------------------------------------
    std::vector<int> v;
    v.reserve(8);                   // capacity 8, size 0: nothing constructed yet
    assert(v.empty() && v.capacity() >= 8);
    v.resize(3);                    // size 3, values 0
    print("reserve(8); resize(3)", v);
    v.push_back(42);
    print("push_back(42)", v);

    // ---- iterator invalidation: the right ways ------------------------------------------
    std::vector<int> w = {1, 2, 3, 4, 5, 6, 7, 8};

    // Way 1: erase returns the iterator to the element after the erased one. O(n^2) overall
    // because each erase shifts the tail, but correct.
    for (auto p = w.begin(); p != w.end();) {
        if (*p % 2 == 0) p = w.erase(p);
        else ++p;
    }
    print("erase evens (loop)", w);

    // Way 2: erase-remove idiom. remove_if compacts kept elements to the front (slow/fast
    // two pointers) and returns the new logical end; erase chops the tail once. O(n).
    w = {1, 2, 3, 4, 5, 6, 7, 8};
    w.erase(std::remove_if(w.begin(), w.end(), [](int x) { return x % 2 == 0; }), w.end());
    print("erase evens (remove_if)", w);

    // Way 3: order does not matter -> swap with back and pop. O(1) per deletion.
    w = {1, 2, 3, 4, 5, 6, 7, 8};
    for (std::size_t i = 0; i < w.size();) {
        if (w[i] % 2 == 0) {
            w[i] = w.back();   // overwrite with last element
            w.pop_back();      // do not advance i: the new w[i] is unprocessed
        } else {
            ++i;
        }
    }
    print("erase evens (swap-pop)", w);

    // ---- the dangerous one, shown commented so you recognise it in review -----------------
    // for (int x : w) if (x == 3) w.push_back(99);   // UB: push_back may reallocate while iterating

    // Safe alternative: snapshot the size and index.
    std::size_t n = w.size();
    for (std::size_t i = 0; i < n; ++i)
        if (w[i] == 3) w.push_back(99);
    print("append while iterating", w);

    // ---- raw buffer access for C APIs -----------------------------------------------------
    int* raw = w.data();           // contiguous, raw[i] == w[i]
    std::printf("w.data()[0] = %d, sizeof(std::vector<int>) = %zu bytes\n", raw[0],
                sizeof(std::vector<int>));
    return 0;
}
