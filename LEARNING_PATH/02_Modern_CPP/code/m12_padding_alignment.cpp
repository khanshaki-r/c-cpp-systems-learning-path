// m12_padding_alignment.cpp
// Object layout: sizeof, alignof, offsetof, padding, member ordering, alignas, #pragma pack.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m12_padding_alignment.cpp
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>

struct Bad {            // "natural" order; the compiler must insert padding so every member is aligned
    char   tag;         // offset 0
    double amount;      // offset 8  (7 bytes of padding before it: doubles need 8-byte alignment)
    int    id;          // offset 16
    char   flag;        // offset 20
};                      // size 24: 3 bytes of tail padding so arr[1] starts 8-byte aligned

struct Good {           // same members sorted by alignment, largest first: no internal padding
    double amount;      // 0
    int    id;          // 8
    char   tag;         // 12
    char   flag;        // 13
};                      // size 16

struct alignas(64) CacheLineBox {   // over-aligned: every object starts on its own 64-byte cache line
    int counter;
};

#pragma pack(push, 1)
struct WireHeader {     // packed: matches a byte-exact wire format. Members may be misaligned, so access
    std::uint8_t  type;     // through a pointer to a member can fault on strict CPUs and is slower on x86.
    std::uint32_t length;   // Use memcpy to/from a packed struct instead of taking member addresses.
    std::uint16_t flags;
};
#pragma pack(pop)

template <class T>
void report(const char* name) {
    std::cout << name << ": sizeof=" << sizeof(T) << " alignof=" << alignof(T)
              << " standard_layout=" << std::is_standard_layout_v<T>
              << " trivially_copyable=" << std::is_trivially_copyable_v<T> << '\n';
}

int main() {
    report<Bad>("Bad");
    report<Good>("Good");
    report<CacheLineBox>("CacheLineBox");
    report<WireHeader>("WireHeader");

    std::cout << "Bad offsets:  tag=" << offsetof(Bad, tag) << " amount=" << offsetof(Bad, amount)
              << " id=" << offsetof(Bad, id) << " flag=" << offsetof(Bad, flag) << '\n';
    std::cout << "Good offsets: amount=" << offsetof(Good, amount) << " id=" << offsetof(Good, id)
              << " tag=" << offsetof(Good, tag) << " flag=" << offsetof(Good, flag) << '\n';
    std::cout << "WireHeader offsets: type=" << offsetof(WireHeader, type) << " length=" << offsetof(WireHeader, length)
              << " flags=" << offsetof(WireHeader, flags) << " (sizeof=" << sizeof(WireHeader) << ", would be 12 unpacked)\n";

    std::cout << "1000 Bad = " << sizeof(Bad[1000]) << " bytes, 1000 Good = " << sizeof(Good[1000])
              << " bytes: " << sizeof(Bad[1000]) - sizeof(Good[1000]) << " bytes of pure padding\n";

    std::cout << "alignof: char=" << alignof(char) << " short=" << alignof(short) << " int=" << alignof(int)
              << " long=" << alignof(long) << " double=" << alignof(double) << " long double=" << alignof(long double)
              << " void*=" << alignof(void*) << " max_align_t=" << alignof(std::max_align_t) << '\n';

    // A struct's alignment is the largest alignment among its members; its size is a multiple of that.
    static_assert(sizeof(Good) % alignof(Good) == 0);
    static_assert(alignof(CacheLineBox) == 64 && sizeof(CacheLineBox) == 64);
    return 0;
}
