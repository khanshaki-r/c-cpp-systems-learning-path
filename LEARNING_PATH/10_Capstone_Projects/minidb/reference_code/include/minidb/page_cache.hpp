#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minidb/page.hpp"
#include "minidb/pager.hpp"

namespace minidb {

// Fixed-size buffer pool in front of the Pager.
//
//  * Pinning: fetch()/create() return a PageRef that pins the frame; a pinned frame is never
//    evicted. The PageRef unpins in its destructor (RAII), so an exception cannot leak a pin.
//  * Replacement: least recently unpinned frame first (LRU over unpinned frames).
//  * WAL rule: before a dirty page is written to the data file, wal_flush(page.lsn()) is
//    called so the log record carrying that image is durable first.
//  * No-steal: a page modified by the current transaction is never written to the data file
//    before commit_txn(). On abort_txn() its before-image is restored from memory. This is
//    what makes a redo-only WAL sufficient: the data file never contains uncommitted bytes.
//
// Not thread-safe; the Db holds one mutex around every operation.
class PageCache {
public:
    using WalFlush = std::function<void(Lsn)>;

    PageCache(Pager& pager, std::size_t capacity, WalFlush wal_flush);
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    class PageRef {
    public:
        PageRef() noexcept = default;
        PageRef(PageRef&& other) noexcept : cache_(other.cache_), frame_(other.frame_) { other.cache_ = nullptr; }
        PageRef& operator=(PageRef&& other) noexcept {
            if (this != &other) {
                release();
                cache_ = other.cache_;
                frame_ = other.frame_;
                other.cache_ = nullptr;
            }
            return *this;
        }
        PageRef(const PageRef&) = delete;
        PageRef& operator=(const PageRef&) = delete;
        ~PageRef() { release(); }

        PageId id() const noexcept { return cache_->frames_[frame_].id; }
        const Page& read() const noexcept { return cache_->frames_[frame_].page; }
        // Joins the page to the current transaction (saving its before-image the first time)
        // and returns it for modification. Always call write() *before* changing bytes.
        Page& write() { return cache_->begin_write(frame_); }

    private:
        friend class PageCache;
        PageRef(PageCache* cache, std::size_t frame) noexcept : cache_(cache), frame_(frame) {}
        void release() noexcept {
            if (cache_ != nullptr) {
                cache_->unpin(frame_);
                cache_ = nullptr;
            }
        }
        PageCache* cache_ = nullptr;
        std::size_t frame_ = 0;
    };

    PageRef fetch(PageId id);   // pins; reads (and verifies) from disk on a miss
    PageRef create(PageId id);  // pins a zeroed frame for a newly allocated page id

    // Transaction support. A transaction is implicit: it starts with the first write() and
    // ends with commit_txn() or abort_txn().
    void for_each_txn_page(const std::function<void(PageId, Page&)>& fn);
    void commit_txn() noexcept;
    void abort_txn() noexcept;
    bool in_txn() const noexcept { return !txn_frames_.empty(); }

    // Writes every dirty, committed page. The caller then calls Pager::sync().
    void flush_all();

    struct Stats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t writebacks = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    std::size_t capacity() const noexcept { return frames_.size(); }

private:
    struct Frame {
        PageId id = 0;
        Page page;
        int pins = 0;
        bool used = false;
        bool dirty = false;
        bool in_txn = false;
        bool created_in_txn = false;
        bool in_lru = false;
        std::list<std::size_t>::iterator lru_pos;
    };
    struct BeforeImage {
        Page page;
        bool was_dirty = false;
    };

    std::size_t obtain_frame();  // a free frame, or an evicted one; throws if none
    void write_back(Frame& frame);
    void pin(std::size_t frame);
    void unpin(std::size_t frame) noexcept;
    Page& begin_write(std::size_t frame);
    void drop_frame(std::size_t frame) noexcept;

    Pager& pager_;
    WalFlush wal_flush_;
    std::vector<Frame> frames_;                      // never resized: references stay valid
    std::unordered_map<PageId, std::size_t> table_;  // page id -> frame index
    std::list<std::size_t> lru_;                     // unpinned frames, least recent at front
    std::vector<std::size_t> free_;                  // frames holding no page
    std::vector<std::size_t> txn_frames_;            // frames written by the current transaction
    std::unordered_map<PageId, BeforeImage> before_images_;
    Stats stats_;
};

}  // namespace minidb
