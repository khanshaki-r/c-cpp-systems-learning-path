#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace minidb {

using PageId = std::uint32_t;
using Lsn = std::uint64_t;  // log sequence number: byte position in the logical WAL stream

inline constexpr std::size_t kPageSize = 4096;
inline constexpr PageId kMetaPageId = 0;
inline constexpr PageId kNoPage = 0;  // page 0 is the meta page, so 0 never names a tree node

// Chosen so that at least four maximum-size leaf cells fit in one page; that guarantees a
// byte-balanced split always produces two halves that fit (see btree.cpp).
inline constexpr std::size_t kMaxKeySize = 128;
inline constexpr std::size_t kMaxValueSize = 880;

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The on-disk bytes are not what we wrote: bad checksum, bad magic, impossible field values.
class CorruptionError : public DbError {
public:
    using DbError::DbError;
};

}  // namespace minidb
