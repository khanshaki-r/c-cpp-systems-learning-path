// BPlusTree through the Db API, recovery, and a model-based property test against std::map.

#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "minidb/db.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%08d", i);
    return buf;
}

}  // namespace

TEST(Db, PutGetDelOnFreshDatabase) {
    TempDir dir;
    Db db(dir.file("db"));
    EXPECT_FALSE(db.get("a").has_value());
    db.put("a", "1");
    db.put("b", "2");
    db.put("a", "3");  // overwrite
    EXPECT_EQ(db.get("a").value_or(""), "3");
    EXPECT_TRUE(db.del("a"));
    EXPECT_FALSE(db.del("a"));
    EXPECT_FALSE(db.get("a").has_value());
    EXPECT_EQ(db.check(), 1u);
}

TEST(Db, RejectsOversizedKeysAndValues) {
    TempDir dir;
    Db db(dir.file("db"));
    EXPECT_THROW(db.put("", "v"), std::invalid_argument);
    EXPECT_THROW(db.put(std::string(kMaxKeySize + 1, 'k'), "v"), std::invalid_argument);
    EXPECT_THROW(db.put("k", std::string(kMaxValueSize + 1, 'v')), std::invalid_argument);
    EXPECT_NO_THROW(db.put(std::string(kMaxKeySize, 'k'), std::string(kMaxValueSize, 'v')));
}

TEST(Db, ManySplitsKeepTheTreeValidAndScansOrdered) {
    TempDir dir;
    Db db(dir.file("db"), Options{32, 1u << 20});  // small cache: forces eviction and write-back
    constexpr int kCount = 6000;  // with 300..699-byte values: ~1,000 leaves, so internal nodes split too
    for (int i = 0; i < kCount; ++i) {
        const int k = (i * 7919) % kCount;  // a permutation: inserts arrive out of order
        db.put(key_of(k), std::string(static_cast<std::size_t>(300 + k % 400), 'v'));
    }
    EXPECT_EQ(db.check(), static_cast<std::size_t>(kCount));
    const auto all = db.scan("", "");
    ASSERT_EQ(all.size(), static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(all[static_cast<std::size_t>(i)].first, key_of(i));
    }
    const auto range = db.scan(key_of(100), key_of(110));
    ASSERT_EQ(range.size(), 10u);
    EXPECT_EQ(range.front().first, key_of(100));
    EXPECT_EQ(range.back().first, key_of(109));
    EXPECT_EQ(db.scan(key_of(5), "", 3).size(), 3u);
}

TEST(Db, MatchesStdMapUnderRandomOperations) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::map<std::string, std::string> model;
    std::mt19937 rng(12345);
    {
        Db db(path, Options{24, 256u << 10});
        for (int step = 0; step < 10000; ++step) {
            const std::string key = key_of(static_cast<int>(rng() % 1500));
            const unsigned op = rng() % 10;
            if (op < 6) {
                const std::string value(rng() % (kMaxValueSize + 1), static_cast<char>('a' + rng() % 26));
                db.put(key, value);
                model[key] = value;
            } else if (op < 9) {
                EXPECT_EQ(db.del(key), model.erase(key) == 1);
            } else {
                const auto got = db.get(key);
                const auto it = model.find(key);
                ASSERT_EQ(got.has_value(), it != model.end());
                if (got) {
                    EXPECT_EQ(*got, it->second);
                }
            }
        }
        EXPECT_EQ(db.check(), model.size());
    }
    Db reopened(path);  // clean close and reopen: same contents
    const auto all = reopened.scan("", "");
    ASSERT_EQ(all.size(), model.size());
    auto it = model.begin();
    for (const auto& [k, v] : all) {
        EXPECT_EQ(k, it->first);
        EXPECT_EQ(v, it->second);
        ++it;
    }
}

TEST(Recovery, CommittedWritesSurviveWithoutCheckpoint) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        // A huge checkpoint threshold and no clean close: everything after open lives only
        // in the log and in the cache. Simulate the crash by copying the files before the
        // destructor runs its checkpoint.
        Db db(path, Options{64, 1u << 30});
        for (int i = 0; i < 500; ++i) {
            db.put(key_of(i), "value" + std::to_string(i));
        }
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    Db db(path + ".crash");
    EXPECT_GT(db.recovery_stats().committed_txns, 0u);
    EXPECT_EQ(db.check(), 500u);
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(db.get(key_of(i)).value_or(""), "value" + std::to_string(i));
    }
}

namespace {

// Ends of the committed records in a log file, and the page ids they carry.
struct LogSummary {
    Lsn end_of_last_commit = 0;
    std::vector<PageId> pages;
};

LogSummary summarize_log(const std::string& wal_path) {
    LogSummary summary;
    Wal wal(wal_path);
    summary.end_of_last_commit = wal.start_lsn();
    wal.replay([&](const WalRecord& r) {
        if (r.type == RecordType::PageImage) {
            summary.pages.push_back(r.page_id);
        } else {
            summary.end_of_last_commit = r.lsn + Wal::kRecordHeaderSize + 8;
        }
    });
    return summary;
}

}  // namespace

TEST(Recovery, TornDataPageIsRepairedFromTheLog) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        Db db(path, Options{64, 1u << 30});
        for (int i = 0; i < 300; ++i) {
            db.put(key_of(i), "v");
        }
        db.checkpoint();                        // data file current, log empty
        db.put(key_of(0), "after-checkpoint");  // logged; the leaf is dirty only in the cache
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    const LogSummary log = summarize_log(path + ".crash-wal");
    ASSERT_FALSE(log.pages.empty());
    {
        // Tear the logged page in the copy: second half overwritten with garbage, as if the
        // power failed in the middle of the 4 KiB write.
        File f = File::open(path + ".crash", false);
        const std::string junk(kPageSize / 2, '\x7f');
        f.write_at(static_cast<std::uint64_t>(log.pages.front()) * kPageSize + kPageSize / 2, junk.data(),
                   junk.size());
    }
    Db db(path + ".crash");
    EXPECT_GE(db.recovery_stats().pages_redone, 1u);
    EXPECT_EQ(db.get(key_of(0)).value_or(""), "after-checkpoint");
    EXPECT_EQ(db.check(), 300u);
}

TEST(Recovery, UncommittedTailIsDiscardedAndNotReplayedLater) {
    TempDir dir;
    const std::string path = dir.file("db");
    {
        Db db(path, Options{64, 1u << 30});
        db.put("a", "1");
        std::filesystem::copy_file(path, path + ".crash");
        std::filesystem::copy_file(path + "-wal", path + ".crash-wal");
    }
    {
        // Append one page image with no commit record: a crash between append and commit.
        // The image is an empty leaf for the root page; applying it would wipe "a".
        const LogSummary log = summarize_log(path + ".crash-wal");
        Wal wal(path + ".crash-wal");
        wal.truncate_at(log.end_of_last_commit);
        Page empty_leaf = BPlusTree::initial_pages()[1];
        wal.append_page(1, empty_leaf);
        wal.sync();
    }
    {
        Db db(path + ".crash");
        EXPECT_EQ(db.recovery_stats().discarded_records, 1u);
        EXPECT_EQ(db.get("a").value_or(""), "1");
        db.put("b", "2");  // appended after the discarded tail was cut off
    }
    Db db(path + ".crash");
    EXPECT_EQ(db.get("a").value_or(""), "1");
    EXPECT_EQ(db.get("b").value_or(""), "2");
}
