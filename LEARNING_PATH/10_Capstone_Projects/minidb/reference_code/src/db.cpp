#include "minidb/db.hpp"

#include <stdexcept>

namespace minidb {

namespace {

// Runs before the Pager member is constructed: makes sure a data file exists.
const std::string& ensure_database(const std::string& path) {
    if (!file_exists(path)) {
        // A log left over from an older database with the same name must not be replayed
        // into the new one: replace it with an empty log first, then create the data file.
        if (file_exists(path + "-wal")) {
            rename_file(path + "-wal", path + "-wal.orphaned");
        }
        Pager::create(path, BPlusTree::initial_pages());
    }
    return path;
}

void validate(std::string_view key, std::string_view value) {
    if (key.empty() || key.size() > kMaxKeySize) {
        throw std::invalid_argument("minidb: key must be 1.." + std::to_string(kMaxKeySize) + " bytes");
    }
    if (value.size() > kMaxValueSize) {
        throw std::invalid_argument("minidb: value must be at most " + std::to_string(kMaxValueSize) + " bytes");
    }
}

}  // namespace

Db::Db(const std::string& path, Options options)
    : options_(options),
      pager_(ensure_database(path)),
      wal_(path + "-wal"),
      recovery_(Recovery::run(pager_, wal_)),
      cache_(pager_, options.cache_pages, [this](Lsn lsn) { wal_.ensure_durable(lsn); }),
      tree_(cache_) {
    {
        PageCache::PageRef meta = cache_.fetch(kMetaPageId);
        (void)Meta::decode(meta.read());  // throws CorruptionError for a non-minidb file
    }
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_locked();  // fold the recovered log into the data file; start with an empty log
}

Db::~Db() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_) {
            checkpoint_locked();
        }
    } catch (...) {
        // Destructors must not throw. Nothing is lost: the log still holds every commit.
    }
}

void Db::throw_if_failed() const {
    if (failed_) {
        throw DbError("minidb: a previous commit failed; reopen the database to recover");
    }
}

template <class F>
auto Db::write_txn(F&& body) {
    throw_if_failed();
    try {
        auto result = body();
        commit();
        return result;
    } catch (...) {
        cache_.abort_txn();     // restore before-images: memory matches the last commit again
        wal_.discard_buffer();  // nothing of this transaction reaches the log
        throw;
    }
}

// The commit protocol, in the order that makes it crash-safe:
//   1. append a full image of every page this transaction changed (stamps page LSNs)
//   2. append a commit record
//   3. fdatasync the log                  <- the transaction is durable from here
//   4. release the pages to the cache as ordinary dirty pages (they may now be evicted)
// The data file is not touched. It catches up at eviction or checkpoint.
void Db::commit() {
    if (!cache_.in_txn()) {
        return;  // e.g. del() of a missing key changed nothing
    }
    cache_.for_each_txn_page([this](PageId id, Page& page) { wal_.append_page(id, page); });
    wal_.append_commit(next_txn_id_++);
    try {
        wal_.sync();
    } catch (...) {
        // fsync failed. The kernel may have dropped the dirty log pages, so we cannot know
        // what is on disk ("fsyncgate", PostgreSQL 2018). The only safe answer is to stop
        // accepting writes and let recovery decide on the next open.
        failed_ = true;
        throw;
    }
    cache_.commit_txn();
}

void Db::put(std::string_view key, std::string_view value) {
    validate(key, value);
    std::lock_guard<std::mutex> lock(mutex_);
    write_txn([&] {
        tree_.put(key, value);
        return true;
    });
    if (wal_.size_bytes() >= options_.checkpoint_wal_bytes) {
        checkpoint_locked();
    }
}

bool Db::del(std::string_view key) {
    if (key.empty() || key.size() > kMaxKeySize) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool removed = write_txn([&] { return tree_.erase(key); });
    if (wal_.size_bytes() >= options_.checkpoint_wal_bytes) {
        checkpoint_locked();
    }
    return removed;
}

std::optional<std::string> Db::get(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    return tree_.get(key);
}

std::vector<std::pair<std::string, std::string>> Db::scan(std::string_view start, std::string_view end,
                                                          std::size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    return tree_.scan(start, end, limit);
}

std::size_t Db::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    return tree_.check();
}

void Db::checkpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed();
    checkpoint_locked();
}

// Checkpoint: make the data file contain everything the log contains, then empty the log.
//   1. write every dirty page (each one's WAL record is already durable: the WAL rule)
//   2. fdatasync the data file
//   3. replace the log with an empty one starting at the current end LSN
// A crash before step 3 completes leaves the old log in place; replaying it is harmless.
void Db::checkpoint_locked() {
    cache_.flush_all();
    pager_.sync();
    wal_.reset();
}

}  // namespace minidb
