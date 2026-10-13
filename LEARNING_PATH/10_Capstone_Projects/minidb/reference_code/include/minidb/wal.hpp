#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "minidb/file.hpp"
#include "minidb/page.hpp"

namespace minidb {

enum class RecordType : std::uint8_t { PageImage = 1, Commit = 2 };

struct WalRecord {
    Lsn lsn = 0;
    RecordType type = RecordType::Commit;
    PageId page_id = 0;        // PageImage
    std::uint64_t txn_id = 0;  // Commit
    Page image;                // PageImage: the full page after the change, sealed
};

// Write-ahead log: an append-only file next to the data file ("<db>-wal").
//
// File layout:
//   header (24 bytes): u64 magic | u64 start_lsn | u32 crc32(first 16 bytes) | u32 zero
//   records, back to back:
//     u32 crc32(bytes [4, 24) + payload) | u32 payload_len | u64 lsn | u8 type | 3 x u8 zero |
//     u32 page_id | payload (4096-byte page image, or u64 txn id for Commit)
//
// LSN = start_lsn + (file offset - header size). LSNs only grow: reset() starts the next log
// at the current end LSN, so a page LSN can be compared with any record LSN.
//
// The header is never rewritten in place. A new log (creation or reset) is built as
// "<path>.tmp", fsynced, and renamed over the old one, so the file always has a valid header
// and either the old records or none.
//
// Durability: append_*() only buffer in memory; sync() writes the buffer and fdatasyncs.
// A record is durable once durable_lsn() is greater than its LSN.
class Wal {
public:
    // LSN 0 is reserved: it is the LSN of pages that were never logged (written by
    // Pager::create). If the first record also had LSN 0, recovery would see a page with
    // LSN 0 >= record LSN 0 and wrongly skip the redo.
    static constexpr Lsn kFirstLsn = 1;

    // Opens the log at `path`, creating an empty one (start LSN kFirstLsn) if it does not exist.
    // Throws CorruptionError if an existing file has an invalid header.
    explicit Wal(const std::string& path);
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Recovery: calls `fn` for every valid record in order, stopping at the first record that
    // is incomplete, has a bad checksum or an unexpected LSN: the torn tail of a crash.
    // Returns the number of valid records. Does not modify the file.
    std::size_t replay(const std::function<void(const WalRecord&)>& fn);

    // Recovery: discard everything from `end` onwards (the records of a transaction that never
    // committed, and any torn tail). Must be called before the first append after replay(),
    // or those orphaned page images would be mistaken for part of the next transaction.
    void truncate_at(Lsn end);

    // Stamps the page with the record's LSN, seals it, and buffers the record.
    Lsn append_page(PageId id, Page& page);
    Lsn append_commit(std::uint64_t txn_id);

    void sync();                     // write buffered records, then fdatasync
    void ensure_durable(Lsn lsn);    // sync() unless the record at `lsn` is already durable
    void discard_buffer() noexcept;  // drop unsynced records (a transaction that failed)

    // Checkpoint: the data file has been synced and contains every logged change. Replace
    // the log with an empty one that starts at end_lsn().
    void reset();

    Lsn start_lsn() const noexcept { return start_lsn_; }
    Lsn end_lsn() const noexcept { return end_lsn_; }          // LSN the next record gets
    Lsn durable_lsn() const noexcept { return durable_lsn_; }  // everything below is on disk
    std::uint64_t size_bytes() const noexcept { return kHeaderSize + (end_lsn_ - start_lsn_); }

    static constexpr std::size_t kHeaderSize = 24;
    static constexpr std::size_t kRecordHeaderSize = 24;

private:
    static void create_empty(const std::string& path, Lsn start_lsn);
    std::uint64_t offset_of(Lsn lsn) const noexcept { return kHeaderSize + (lsn - start_lsn_); }

    std::string path_;
    File file_;
    Lsn start_lsn_ = 0;
    Lsn end_lsn_ = 0;
    Lsn durable_lsn_ = 0;
    std::string buffer_;  // encoded records in [durable_lsn_, end_lsn_)
};

}  // namespace minidb
