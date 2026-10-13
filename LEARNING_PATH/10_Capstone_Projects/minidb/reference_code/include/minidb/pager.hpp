#pragma once

#include <string>
#include <vector>

#include "minidb/file.hpp"
#include "minidb/page.hpp"

namespace minidb {

// Raw page I/O on the single data file: page N lives at byte offset N * kPageSize.
// Checksums are applied on write and verified on read. No caching, no policy: that is the
// PageCache's job. Not thread-safe; the Db serialises access.
class Pager {
public:
    // Opens an existing data file. Throws std::system_error if it does not exist.
    explicit Pager(const std::string& path);

    // Creates a new data file atomically: writes `pages` (page i at index i) to path + ".tmp",
    // fsyncs it, renames it over `path`, fsyncs the directory. A crash leaves either no file
    // or a complete one, never a half-initialised database.
    static void create(const std::string& path, std::vector<Page> pages);

    // Reads and verifies page `id`. Throws CorruptionError on a short read or bad checksum.
    void read(PageId id, Page& out) const;

    // Recovery helper: like read(), but reports a missing or torn page as `false` instead
    // of throwing, because during redo that is an expected state, not corruption.
    bool read_if_valid(PageId id, Page& out) const;

    // Seals (checksums) the page and writes it in place. Not durable until sync().
    void write(PageId id, Page& page);

    void sync() { file_.sync(); }

    // Pages physically present in the file (may be fewer than Meta::page_count after a
    // crash: allocated pages are only written at eviction or checkpoint).
    PageId file_page_count() const;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    File file_;
};

}  // namespace minidb
