// m08_templates_basics.cpp
// Function templates, class templates, specialisation, two-phase lookup, SFINAE.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m08_templates_basics.cpp
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Function template: a recipe. NO code is generated until it is instantiated with concrete types.
template <class T>
T max_of(T a, T b) { return (b < a) ? a : b; }

// Full specialisation for C strings: compare contents, not pointer values.
template <>
const char* max_of<const char*>(const char* a, const char* b) { return (std::strcmp(b, a) < 0) ? a : b; }

// Class template with a non-type parameter: the capacity is part of the type.
template <class T, std::size_t N>
class RingBuffer {
public:
    bool push(const T& v) {
        if (count_ == N) return false;
        buf_[(head_ + count_) % N] = v;
        ++count_;
        return true;
    }
    bool pop(T& out) {
        if (count_ == 0) return false;
        out = buf_[head_];
        head_ = (head_ + 1) % N;
        --count_;
        return true;
    }
    static constexpr std::size_t capacity() { return N; }
    std::size_t size() const { return count_; }
private:
    T buf_[N]{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

// Partial specialisation: a different implementation when T is a pointer type.
template <class T> struct Describe        { static const char* kind() { return "value"; } };
template <class T> struct Describe<T*>    { static const char* kind() { return "pointer"; } };

// Two-phase lookup: non-dependent names are checked when the template is DEFINED;
// dependent names (item.size() depends on Container) are checked only when it is INSTANTIATED.
template <class Container>
std::size_t total_size(const Container& c) {
    std::size_t n = 0;
    for (const auto& item : c) n += item.size();   // compiles for any element type that has .size()
    return n;
}

// SFINAE (Substitution Failure Is Not An Error): if T has no .c_str(), this template silently drops out
// of the overload set instead of causing an error, and the plain overload below is chosen.
template <class T, class = std::void_t<decltype(std::declval<const T&>().c_str())>>
std::size_t text_len(const T& t) { return std::strlen(t.c_str()); }
std::size_t text_len(long long v) { return std::to_string(v).size(); }

int main() {
    std::cout << max_of(3, 7) << ' ' << max_of(2.5, 1.5) << ' ' << max_of<const char*>("pear", "apple") << '\n';
    // max_of(3, 2.5);   // error: T deduced as int from 'a' and double from 'b'. Fix: max_of<double>(3, 2.5)

    RingBuffer<int, 4> rb;   // RingBuffer<int,4> and RingBuffer<int,8> are two unrelated types
    for (int i = 1; i <= 5; ++i) std::cout << "push " << i << (rb.push(i) ? " ok" : " full") << '\n';
    int x = 0;
    while (rb.pop(x)) std::cout << x << ' ';
    std::cout << "capacity=" << RingBuffer<int, 4>::capacity() << " sizeof=" << sizeof(RingBuffer<int, 4>) << '\n';

    std::cout << Describe<int>::kind() << ' ' << Describe<int*>::kind() << '\n';

    std::vector<std::string> words{"ab", "cde"};
    std::cout << "total_size=" << total_size(words) << '\n';
    // total_size(std::vector<int>{1,2});   // error at INSTANTIATION: int has no member 'size'. The message
                                            // points into the template body; C++20 concepts point at the call.

    std::cout << text_len(std::string("hello")) << ' ' << text_len(12345LL) << '\n';

    static_assert(std::is_same_v<decltype(max_of(1, 2)), int>);
    static_assert(std::is_trivially_copyable_v<RingBuffer<int, 4>>, "no owned resources: memcpy-able");
    return 0;
}
