#include "minidb/pager.hpp"

#include <string>
#include <utility>

namespace minidb {

namespace {

std::uint64_t offset_of(PageId id) noexcept { return static_cast<std::uint64_t>(id) * kPageSize; }

}  // namespace

// The file size need not be a multiple of kPageSize: a crash while extending the file can
// leave a partial last page. Such a page was written at eviction or checkpoint, so its
// committed image is in the WAL; read_if_valid() reports it as missing and redo rewrites it.
Pager::Pager(const std::string& path) : path_(path), file_(File::open(path, /*create=*/false)) {}

void Pager::create(const std::string& path, std::vector<Page> pages) {
    const std::string tmp = path + ".tmp";
    {
        File file = File::open(tmp, /*create=*/true);
        file.truncate(0);  // a leftover .tmp from an earlier crashed create
        for (std::size_t i = 0; i < pages.size(); ++i) {
            pages[i].seal();
            file.write_at(offset_of(static_cast<PageId>(i)), pages[i].bytes.data(), kPageSize);
        }
        file.sync();
    }
    rename_file(tmp, path);
    sync_parent_directory(path);
}

void Pager::read(PageId id, Page& out) const {
    const std::size_t n = file_.read_at(offset_of(id), out.bytes.data(), kPageSize);
    if (n != kPageSize) {
        throw CorruptionError("page " + std::to_string(id) + ": short read (beyond end of file)");
    }
    if (!out.verify()) {
        throw CorruptionError("page " + std::to_string(id) + ": checksum mismatch");
    }
}

bool Pager::read_if_valid(PageId id, Page& out) const {
    const std::size_t n = file_.read_at(offset_of(id), out.bytes.data(), kPageSize);
    return n == kPageSize && out.verify();
}

void Pager::write(PageId id, Page& page) {
    page.seal();
    file_.write_at(offset_of(id), page.bytes.data(), kPageSize);
}

PageId Pager::file_page_count() const { return static_cast<PageId>(file_.size() / kPageSize); }

}  // namespace minidb
