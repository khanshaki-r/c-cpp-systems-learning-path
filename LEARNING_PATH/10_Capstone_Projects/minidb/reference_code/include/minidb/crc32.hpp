#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), the one zlib and Ethernet use.
// Chainable: crc32(b, nb, crc32(a, na)) == crc32(a followed by b).
std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t previous = 0) noexcept;

}  // namespace minidb
