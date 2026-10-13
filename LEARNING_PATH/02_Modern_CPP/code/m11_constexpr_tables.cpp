// m11_constexpr_tables.cpp
// constexpr functions and variables, compile-time tables, static_assert, literal types, inline variables.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread m11_constexpr_tables.cpp
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

// constexpr function: usable at compile time AND at run time with the same source.
constexpr std::uint32_t crc32_table_entry(std::uint32_t i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}

// A 1 KiB table computed entirely by the compiler: zero start-up cost, lives in .rodata.
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) t[i] = crc32_table_entry(i);
    return t;
}
inline constexpr auto kCrcTable = make_crc_table();   // inline variable (C++17): one object even if this header is included 50 times

constexpr std::uint32_t crc32(std::string_view data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) c = kCrcTable[(c ^ ch) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Tests that run at COMPILE time. If any fails, the build fails. No test framework needed.
static_assert(kCrcTable[0] == 0, "entry 0 must be zero");
static_assert(kCrcTable[1] == 0x77073096u, "known CRC-32 table value");
static_assert(crc32("123456789") == 0xCBF43926u, "CRC-32 check value for the standard test vector");

// A literal type: usable in constant expressions (constexpr constructor and member functions).
struct Version {
    int major, minor, patch;
    constexpr bool operator<(const Version& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
    constexpr int packed() const { return major * 10000 + minor * 100 + patch; }
};
constexpr Version kMinSupported{1, 4, 0};
static_assert(Version{1, 5, 2}.packed() == 10502);
static_assert(kMinSupported < Version{2, 0, 0});

// const vs constexpr
const int     runtime_const = std::rand();     // const: cannot be modified, but the value exists only at run time
constexpr int compile_const = 60 * 60 * 24;    // constexpr: MUST be computable at compile time

enum class Op { Get, Put, Del };
constexpr std::string_view op_name(Op op) {
    switch (op) {
        case Op::Get: return "GET";
        case Op::Put: return "PUT";
        case Op::Del: return "DEL";
    }
    return "?";
}
static_assert(op_name(Op::Put) == "PUT");

int main() {
    std::printf("crc32(\"123456789\") = 0x%08X   (folded to a constant by the compiler)\n", crc32("123456789"));
    std::string input = "hello";
    std::printf("crc32(runtime \"%s\") = 0x%08X   (same function, run-time loop)\n", input.c_str(), crc32(input));

    std::cout << "table entries=" << kCrcTable.size() << " seconds/day=" << compile_const
              << " runtime_const=" << runtime_const << '\n';

    constexpr int hours_per_day = compile_const / 3600;
    int hours[hours_per_day] = {};                    // constexpr value usable as an array bound
    std::cout << "hours[] has " << sizeof(hours) / sizeof(hours[0]) << " slots\n";
    std::cout << op_name(Op::Del) << '\n';
    return 0;
}
