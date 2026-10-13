// Pager, PageCache and Wal unit tests.

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "minidb/btree.hpp"
#include "minidb/page_cache.hpp"
#include "minidb/pager.hpp"
#include "minidb/wal.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

Page page_with_byte(char c) {
    Page page;
    page.set_type(PageType::Leaf);
    std::memset(page.payload(), c, kPagePayloadSize);
    return page;
}

}  // namespace

TEST(Crc32, KnownVector) {
    // The standard check value for CRC-32/ISO-HDLC.
    EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
    // Chaining equals one pass.
    EXPECT_EQ(crc32("6789", 4, crc32("12345", 5)), 0xCBF43926u);
}

TEST(Pager, CreateWriteReadRoundTrip) {
    TempDir dir;
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    Pager pager(path);
    EXPECT_EQ(pager.file_page_count(), 2u);

    Page out = page_with_byte('x');
    pager.write(5, out);  // beyond the end: the file grows, pages 2..4 are holes
    pager.sync();
    Page in;
    pager.read(5, in);
    EXPECT_EQ(std::memcmp(in.bytes.data(), out.bytes.data(), kPageSize), 0);
    EXPECT_EQ(pager.file_page_count(), 6u);
    EXPECT_THROW(pager.read(3, in), CorruptionError);  // a hole is all zeros: checksum fails
    EXPECT_FALSE(pager.read_if_valid(3, in));
    EXPECT_FALSE(pager.read_if_valid(100, in));  // beyond EOF
}

TEST(Pager, DetectsCorruptedPage) {
    TempDir dir;
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    {
        File f = File::open(path, false);
        const char junk = 0x5A;
        f.write_at(kPageSize + 100, &junk, 1);  // flip a byte inside page 1
    }
    Pager pager(path);
    Page page;
    EXPECT_THROW(pager.read(1, page), CorruptionError);
    EXPECT_NO_THROW(pager.read(0, page));
}

TEST(PageCache, EvictsLeastRecentlyUsedAndWritesBackDirtyPages) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        pages[i] = page_with_byte(static_cast<char>('a' + i));
    }
    Pager::create(path, pages);
    Pager pager(path);
    std::vector<Lsn> flushed;
    PageCache cache(pager, 16, [&](Lsn lsn) { flushed.push_back(lsn); });

    {
        PageCache::PageRef ref = cache.fetch(3);
        ref.write().payload()[0] = 'Z';  // dirty, inside a "transaction"
    }
    cache.commit_txn();  // now an ordinary dirty page: evictable
    for (PageId id = 4; id < 20; ++id) {
        cache.fetch(id);  // 16 more pages through a 16-frame cache: page 3 must be evicted
    }
    EXPECT_GE(cache.stats().evictions, 1u);
    EXPECT_EQ(flushed.size(), 1u);  // the WAL rule hook ran before the write-back
    Page on_disk;
    pager.read(3, on_disk);
    EXPECT_EQ(on_disk.payload()[0], 'Z');
}

TEST(PageCache, NoStealAndAbortRestoresBeforeImage) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        pages[i] = page_with_byte('a');
    }
    Pager::create(path, pages);
    Pager pager(path);
    PageCache cache(pager, 16, [](Lsn) {});

    {
        PageCache::PageRef ref = cache.fetch(2);
        ref.write().payload()[0] = 'U';  // uncommitted
    }
    for (PageId id = 3; id < 20; ++id) {
        cache.fetch(id);  // pressure: 17 other pages through the 15 remaining frames
    }
    Page on_disk;
    pager.read(2, on_disk);
    EXPECT_EQ(on_disk.payload()[0], 'a');  // no-steal: the uncommitted byte never hit the file

    cache.abort_txn();
    EXPECT_EQ(cache.fetch(2).read().payload()[0], 'a');  // before-image restored
}

TEST(PageCache, ThrowsWhenEveryFrameIsPinned) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::vector<Page> pages(20, page_with_byte('a'));
    Pager::create(path, pages);
    Pager pager(path);
    PageCache cache(pager, 16, [](Lsn) {});
    std::vector<PageCache::PageRef> pins;
    for (PageId id = 0; id < 16; ++id) {
        pins.push_back(cache.fetch(id));
    }
    EXPECT_THROW(cache.fetch(16), DbError);
    pins.pop_back();  // unpin one
    EXPECT_NO_THROW(cache.fetch(16));
}

TEST(Wal, AppendSyncReplay) {
    TempDir dir;
    const std::string path = dir.file("wal");
    Lsn first = 0;
    {
        Wal wal(path);
        wal.truncate_at(wal.start_lsn());
        Page page = page_with_byte('q');
        first = wal.append_page(7, page);
        EXPECT_EQ(page.lsn(), first);  // the page carries its record's LSN
        wal.append_commit(1);
        wal.sync();
        EXPECT_EQ(wal.durable_lsn(), wal.end_lsn());
    }
    Wal wal(path);
    std::vector<WalRecord> seen;
    EXPECT_EQ(wal.replay([&](const WalRecord& r) { seen.push_back(r); }), 2u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].type, RecordType::PageImage);
    EXPECT_EQ(seen[0].page_id, 7u);
    EXPECT_EQ(seen[0].lsn, first);
    EXPECT_TRUE(seen[0].image.verify());
    EXPECT_EQ(seen[1].type, RecordType::Commit);
    EXPECT_EQ(seen[1].txn_id, 1u);
}

TEST(Wal, TornTailIsIgnored) {
    TempDir dir;
    const std::string path = dir.file("wal");
    {
        Wal wal(path);
        Page page = page_with_byte('q');
        wal.append_page(1, page);
        wal.append_commit(1);
        wal.append_page(2, page);
        wal.append_commit(2);
        wal.sync();
    }
    {
        // Simulate a crash in the middle of writing the second transaction's page image.
        File f = File::open(path, false);
        f.truncate(Wal::kHeaderSize + (Wal::kRecordHeaderSize + kPageSize) + (Wal::kRecordHeaderSize + 8) + 1000);
    }
    Wal wal(path);
    std::size_t commits = 0;
    EXPECT_EQ(wal.replay([&](const WalRecord& r) { commits += r.type == RecordType::Commit ? 1 : 0; }), 2u);
    EXPECT_EQ(commits, 1u);
}

TEST(Wal, ResetKeepsLsnsMonotonic) {
    TempDir dir;
    const std::string path = dir.file("wal");
    Wal wal(path);
    Page page = page_with_byte('q');
    wal.append_page(1, page);
    wal.append_commit(1);
    wal.sync();
    const Lsn end = wal.end_lsn();
    wal.reset();
    EXPECT_EQ(wal.start_lsn(), end);
    EXPECT_EQ(wal.size_bytes(), Wal::kHeaderSize);
    EXPECT_EQ(wal.append_commit(2), end);  // the next record continues the sequence
}
