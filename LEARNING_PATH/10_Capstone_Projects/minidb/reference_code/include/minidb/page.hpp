#pragma once

#include <array>
#include <cstring>

#include "minidb/common.hpp"
#include "minidb/crc32.hpp"

namespace minidb {

// Fixed-width fields are stored in host byte order with memcpy (never a pointer cast, which
// would break strict aliasing and alignment rules). x86-64 and ARM64 Linux are both
// little-endian; a portable format would convert explicitly.
template <class T>
T read_as(const char* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

template <class T>
void write_as(char* p, T value) noexcept {
    std::memcpy(p, &value, sizeof value);
}

enum class PageType : std::uint16_t { Free = 0, Meta = 1, Leaf = 2, Internal = 3 };

// Every page starts with a 16-byte header:
//   [0, 4)   CRC-32 of bytes [4, kPageSize)
//   [4, 6)   PageType
//   [6, 8)   reserved, zero
//   [8, 16)  page LSN: the LSN of the WAL record that carried this page image
inline constexpr std::size_t kPageHeaderSize = 16;
inline constexpr std::size_t kPagePayloadSize = kPageSize - kPageHeaderSize;

struct Page {
    alignas(64) std::array<char, kPageSize> bytes{};

    PageType type() const noexcept { return static_cast<PageType>(read_as<std::uint16_t>(bytes.data() + 4)); }
    void set_type(PageType type) noexcept { write_as(bytes.data() + 4, static_cast<std::uint16_t>(type)); }
    Lsn lsn() const noexcept { return read_as<Lsn>(bytes.data() + 8); }
    void set_lsn(Lsn lsn) noexcept { write_as(bytes.data() + 8, lsn); }

    char* payload() noexcept { return bytes.data() + kPageHeaderSize; }
    const char* payload() const noexcept { return bytes.data() + kPageHeaderSize; }
    void clear_payload() noexcept { std::memset(payload(), 0, kPagePayloadSize); }

    // Computes and stores the checksum. Call after the last modification, before any write.
    void seal() noexcept { write_as(bytes.data(), crc32(bytes.data() + 4, kPageSize - 4)); }
    // A page of all zeros (never written, or a hole) fails this check: crc32 of zeros is not 0.
    bool verify() const noexcept {
        return read_as<std::uint32_t>(bytes.data()) == crc32(bytes.data() + 4, kPageSize - 4);
    }
};

// Page 0. Logged and recovered like every other page.
//   payload: u64 magic | u32 version | u32 root | u32 page_count
struct Meta {
    static constexpr std::uint64_t kMagic = 0x3142444E494D2121ULL;  // arbitrary, fixed forever
    static constexpr std::uint32_t kVersion = 1;

    PageId root = kNoPage;
    std::uint32_t page_count = 0;  // pages allocated so far, including page 0

    static Meta decode(const Page& page) {
        const char* p = page.payload();
        if (page.type() != PageType::Meta || read_as<std::uint64_t>(p) != kMagic) {
            throw CorruptionError("meta page: bad type or magic (not a minidb file?)");
        }
        if (read_as<std::uint32_t>(p + 8) != kVersion) {
            throw CorruptionError("meta page: unsupported format version");
        }
        Meta meta;
        meta.root = read_as<std::uint32_t>(p + 12);
        meta.page_count = read_as<std::uint32_t>(p + 16);
        if (meta.root == kNoPage || meta.root >= meta.page_count) {
            throw CorruptionError("meta page: root page out of range");
        }
        return meta;
    }

    void encode(Page& page) const noexcept {
        page.set_type(PageType::Meta);
        page.clear_payload();
        char* p = page.payload();
        write_as(p, kMagic);
        write_as(p + 8, kVersion);
        write_as(p + 12, root);
        write_as(p + 16, page_count);
    }
};

}  // namespace minidb
