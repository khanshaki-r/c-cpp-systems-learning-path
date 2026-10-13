#include "minidb/page_cache.hpp"

#include <string>
#include <utility>

namespace minidb {

PageCache::PageCache(Pager& pager, std::size_t capacity, WalFlush wal_flush)
    : pager_(pager), wal_flush_(std::move(wal_flush)), frames_(capacity) {
    if (capacity < 16) {
        // A put pins one page per tree level and a split creates up to one new page per level
        // plus a new root; 16 frames covers trees far deeper than 4 KiB pages ever produce.
        throw DbError("page cache needs at least 16 frames");
    }
    free_.reserve(capacity);
    for (std::size_t i = capacity; i > 0; --i) {
        free_.push_back(i - 1);
    }
}

PageCache::PageRef PageCache::fetch(PageId id) {
    const auto it = table_.find(id);
    if (it != table_.end()) {
        ++stats_.hits;
        pin(it->second);
        return PageRef(this, it->second);
    }
    ++stats_.misses;
    const std::size_t index = obtain_frame();
    Frame& frame = frames_[index];
    try {
        pager_.read(id, frame.page);
    } catch (...) {
        free_.push_back(index);  // frame was not registered yet; just give it back
        throw;
    }
    frame.id = id;
    frame.used = true;
    frame.dirty = false;
    table_.emplace(id, index);
    pin(index);
    return PageRef(this, index);
}

PageCache::PageRef PageCache::create(PageId id) {
    if (table_.count(id) != 0) {
        throw DbError("page " + std::to_string(id) + " already cached; allocated twice?");
    }
    const std::size_t index = obtain_frame();
    Frame& frame = frames_[index];
    frame.page = Page{};
    frame.id = id;
    frame.used = true;
    frame.dirty = true;
    frame.in_txn = true;
    frame.created_in_txn = true;  // abort removes it; there is no before-image to restore
    txn_frames_.push_back(index);
    table_.emplace(id, index);
    pin(index);
    return PageRef(this, index);
}

Page& PageCache::begin_write(std::size_t index) {
    Frame& frame = frames_[index];
    if (!frame.in_txn) {
        before_images_.emplace(frame.id, BeforeImage{frame.page, frame.dirty});
        frame.in_txn = true;
        txn_frames_.push_back(index);
    }
    frame.dirty = true;
    return frame.page;
}

void PageCache::for_each_txn_page(const std::function<void(PageId, Page&)>& fn) {
    for (const std::size_t index : txn_frames_) {
        fn(frames_[index].id, frames_[index].page);
    }
}

void PageCache::commit_txn() noexcept {
    for (const std::size_t index : txn_frames_) {
        frames_[index].in_txn = false;
        frames_[index].created_in_txn = false;  // now an ordinary dirty, committed page
    }
    txn_frames_.clear();
    before_images_.clear();
}

void PageCache::abort_txn() noexcept {
    for (const std::size_t index : txn_frames_) {
        Frame& frame = frames_[index];
        if (frame.created_in_txn) {
            drop_frame(index);  // the allocation is undone by the meta page's before-image
            continue;
        }
        const auto it = before_images_.find(frame.id);
        if (it != before_images_.end()) {
            frame.page = it->second.page;
            frame.dirty = it->second.was_dirty;
        }
        frame.in_txn = false;
    }
    txn_frames_.clear();
    before_images_.clear();
}

void PageCache::flush_all() {
    for (Frame& frame : frames_) {
        if (frame.used && frame.dirty && !frame.in_txn) {
            write_back(frame);
        }
    }
}

std::size_t PageCache::obtain_frame() {
    if (!free_.empty()) {
        const std::size_t index = free_.back();
        free_.pop_back();
        return index;
    }
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        Frame& frame = frames_[*it];
        if (frame.in_txn) {
            continue;  // no-steal: uncommitted bytes never reach the data file
        }
        if (frame.dirty) {
            write_back(frame);  // may throw; the frame stays cached and consistent
        }
        const std::size_t index = *it;
        lru_.erase(it);
        frame.in_lru = false;
        table_.erase(frame.id);
        frame.used = false;
        ++stats_.evictions;
        return index;
    }
    throw DbError("page cache exhausted: every frame is pinned or part of the open transaction");
}

void PageCache::write_back(Frame& frame) {
    wal_flush_(frame.page.lsn());  // the WAL rule: log record durable before the page
    pager_.write(frame.id, frame.page);
    frame.dirty = false;
    ++stats_.writebacks;
}

void PageCache::pin(std::size_t index) {
    Frame& frame = frames_[index];
    if (frame.in_lru) {
        lru_.erase(frame.lru_pos);
        frame.in_lru = false;
    }
    ++frame.pins;
}

void PageCache::unpin(std::size_t index) noexcept {
    Frame& frame = frames_[index];
    if (--frame.pins == 0 && frame.used) {
        frame.lru_pos = lru_.insert(lru_.end(), index);  // most recently used at the back
        frame.in_lru = true;
    }
}

void PageCache::drop_frame(std::size_t index) noexcept {
    Frame& frame = frames_[index];
    if (frame.in_lru) {
        lru_.erase(frame.lru_pos);
        frame.in_lru = false;
    }
    table_.erase(frame.id);
    frame.used = false;
    frame.dirty = false;
    frame.in_txn = false;
    frame.created_in_txn = false;
    if (frame.pins == 0) {
        free_.push_back(index);
    }
    // A dropped frame that is still pinned would be a bug in the caller (abort while a
    // PageRef is alive); it then leaks one frame rather than corrupting another page.
}

}  // namespace minidb
