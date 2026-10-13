#include "minidb/crc32.hpp"

#include <array>

namespace minidb {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();  // built at compile time

}  // namespace

std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t previous) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = previous ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace minidb
