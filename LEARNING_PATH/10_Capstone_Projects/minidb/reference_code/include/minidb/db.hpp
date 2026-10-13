#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minidb/btree.hpp"
#include "minidb/page_cache.hpp"
#include "minidb/pager.hpp"
#include "minidb/recovery.hpp"
#include "minidb/wal.hpp"

namespace minidb {

struct Options {
    std::size_t cache_pages = 256;                    // 1 MiB of page cache
    std::uint64_t checkpoint_wal_bytes = 8u << 20;    // checkpoint when the log passes 8 MiB
};

// The public API. Every put/del is its own transaction and is durable when the call returns:
// its WAL records have been fdatasynced. One mutex serialises all calls.
class Db {
public:
    // Opens `path` (and "<path>-wal"), creating both if the data file does not exist,
    // and runs crash recovery.
    explicit Db(const std::string& path, Options options = {});
    ~Db();  // checkpoints, so a clean shutdown leaves an empty log
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void put(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key);
    bool del(std::string_view key);
    std::vector<std::pair<std::string, std::string>> scan(std::string_view start, std::string_view end = {},
                                                          std::size_t limit = SIZE_MAX);
    void checkpoint();
    std::size_t check();  // full structural check; returns key count

    const RecoveryStats& recovery_stats() const noexcept { return recovery_; }
    std::uint64_t wal_bytes() const noexcept { return wal_.size_bytes(); }
    const PageCache::Stats& cache_stats() const noexcept { return cache_.stats(); }

private:
    template <class F>
    auto write_txn(F&& body);
    void commit();
    void checkpoint_locked();
    void throw_if_failed() const;

    std::mutex mutex_;
    Options options_;
    Pager pager_;           // declaration order is construction order:
    Wal wal_;               // the log must exist before recovery,
    RecoveryStats recovery_;  // recovery must finish before the cache reads any page
    PageCache cache_;
    BPlusTree tree_;
    std::uint64_t next_txn_id_ = 1;
    bool failed_ = false;   // set when a commit fails part-way; see commit()
};

}  // namespace minidb
