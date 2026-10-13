#include "minidb/wal.hpp"

#include <array>

#include "minidb/crc32.hpp"

namespace minidb {

namespace {

constexpr std::uint64_t kWalMagic = 0x314C41574244494DULL;  // arbitrary, fixed forever

std::uint32_t payload_size(RecordType type) noexcept {
    return type == RecordType::PageImage ? static_cast<std::uint32_t>(kPageSize) : 8u;
}

void encode_record(std::string& out, Lsn lsn, RecordType type, PageId page_id, const char* payload,
                   std::uint32_t payload_len) {
    std::array<char, Wal::kRecordHeaderSize> header{};
    write_as(header.data() + 4, payload_len);
    write_as(header.data() + 8, lsn);
    write_as(header.data() + 16, static_cast<std::uint8_t>(type));
    write_as(header.data() + 20, page_id);
    std::uint32_t crc = crc32(header.data() + 4, header.size() - 4);
    crc = crc32(payload, payload_len, crc);
    write_as(header.data(), crc);
    out.append(header.data(), header.size());
    out.append(payload, payload_len);
}

}  // namespace

void Wal::create_empty(const std::string& path, Lsn start_lsn) {
    std::array<char, kHeaderSize> header{};
    write_as(header.data(), kWalMagic);
    write_as(header.data() + 8, start_lsn);
    write_as(header.data() + 16, crc32(header.data(), 16));

    const std::string tmp = path + ".tmp";
    {
        File file = File::open(tmp, /*create=*/true);
        file.truncate(0);
        file.write_at(0, header.data(), header.size());
        file.sync();
    }
    rename_file(tmp, path);         // atomic: readers see the old log or the new one
    sync_parent_directory(path);    // and the rename survives a power cut
}

Wal::Wal(const std::string& path) : path_(path) {
    if (!file_exists(path)) {
        create_empty(path, kFirstLsn);
    }
    file_ = File::open(path, /*create=*/false);

    std::array<char, kHeaderSize> header{};
    const std::size_t n = file_.read_at(0, header.data(), header.size());
    if (n != kHeaderSize || read_as<std::uint64_t>(header.data()) != kWalMagic ||
        read_as<std::uint32_t>(header.data() + 16) != crc32(header.data(), 16)) {
        throw CorruptionError("WAL " + path + ": invalid header");
    }
    start_lsn_ = read_as<Lsn>(header.data() + 8);
    end_lsn_ = start_lsn_;
    durable_lsn_ = start_lsn_;
}

std::size_t Wal::replay(const std::function<void(const WalRecord&)>& fn) {
    std::size_t count = 0;
    Lsn lsn = start_lsn_;
    WalRecord record;
    std::array<char, kRecordHeaderSize> header{};
    std::array<char, 8> commit_payload{};
    for (;;) {
        const std::uint64_t offset = offset_of(lsn);
        if (file_.read_at(offset, header.data(), header.size()) != header.size()) {
            break;  // clean end of log, or a torn record header
        }
        const auto len = read_as<std::uint32_t>(header.data() + 4);
        const auto type = static_cast<RecordType>(read_as<std::uint8_t>(header.data() + 16));
        if ((type != RecordType::PageImage && type != RecordType::Commit) || len != payload_size(type) ||
            read_as<Lsn>(header.data() + 8) != lsn) {
            break;  // garbage: not a record we wrote at this position
        }
        char* payload = type == RecordType::PageImage ? record.image.bytes.data() : commit_payload.data();
        if (file_.read_at(offset + kRecordHeaderSize, payload, len) != len) {
            break;  // torn payload
        }
        std::uint32_t crc = crc32(header.data() + 4, header.size() - 4);
        crc = crc32(payload, len, crc);
        if (crc != read_as<std::uint32_t>(header.data())) {
            break;  // torn or corrupted record
        }
        record.lsn = lsn;
        record.type = type;
        record.page_id = read_as<PageId>(header.data() + 20);
        record.txn_id = type == RecordType::Commit ? read_as<std::uint64_t>(commit_payload.data()) : 0;
        fn(record);
        ++count;
        lsn += kRecordHeaderSize + len;
    }
    return count;
}

void Wal::truncate_at(Lsn end) {
    if (end < start_lsn_) {
        throw DbError("WAL truncate_at: LSN before start of log");
    }
    // Cut the uncommitted or torn tail so new records follow the last commit directly.
    // Otherwise a later replay would see the orphaned images, or stop at the old garbage
    // before reaching newer committed records.
    file_.truncate(offset_of(end));
    file_.sync();
    buffer_.clear();
    end_lsn_ = end;
    durable_lsn_ = end;
}

Lsn Wal::append_page(PageId id, Page& page) {
    const Lsn lsn = end_lsn_;
    page.set_lsn(lsn);
    page.seal();
    encode_record(buffer_, lsn, RecordType::PageImage, id, page.bytes.data(),
                  static_cast<std::uint32_t>(kPageSize));
    end_lsn_ += kRecordHeaderSize + kPageSize;
    return lsn;
}

Lsn Wal::append_commit(std::uint64_t txn_id) {
    const Lsn lsn = end_lsn_;
    std::array<char, 8> payload{};
    write_as(payload.data(), txn_id);
    encode_record(buffer_, lsn, RecordType::Commit, 0, payload.data(), 8);
    end_lsn_ += kRecordHeaderSize + 8;
    return lsn;
}

void Wal::sync() {
    if (!buffer_.empty()) {
        file_.write_at(offset_of(durable_lsn_), buffer_.data(), buffer_.size());
    }
    file_.sync();
    durable_lsn_ = end_lsn_;
    buffer_.clear();
}

void Wal::ensure_durable(Lsn lsn) {
    if (lsn >= durable_lsn_) {
        sync();
    }
}

void Wal::discard_buffer() noexcept {
    buffer_.clear();
    end_lsn_ = durable_lsn_;
}

void Wal::reset() {
    if (!buffer_.empty()) {
        sync();
    }
    file_ = File();  // close before replacing the file
    create_empty(path_, end_lsn_);
    file_ = File::open(path_, /*create=*/false);
    start_lsn_ = end_lsn_;
    durable_lsn_ = end_lsn_;
}

}  // namespace minidb
