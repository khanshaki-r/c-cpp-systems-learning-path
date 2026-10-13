// BPlusTree on a Pager + PageCache with no log: each operation is committed in the cache
// straight away. Isolates the tree from the WAL so tree bugs show up here, not in recovery.

#include <cstdio>
#include <map>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "minidb/btree.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%08d", i);
    return buf;
}

std::string create_database(const TempDir& dir) {
    const std::string path = dir.file("db");
    Pager::create(path, BPlusTree::initial_pages());
    return path;
}

class TreeFixture : public ::testing::Test {
protected:
    TreeFixture()
        : path_(create_database(dir_)),
          pager_(path_),
          cache_(pager_, 32, [](Lsn) {}),  // no WAL in this test: nothing to make durable
          tree_(cache_) {}

    void put(const std::string& k, const std::string& v) {
        tree_.put(k, v);
        cache_.commit_txn();  // what Db::commit does after logging
    }
    bool erase(const std::string& k) {
        const bool removed = tree_.erase(k);
        cache_.commit_txn();
        return removed;
    }

    TempDir dir_;
    std::string path_;
    Pager pager_;
    PageCache cache_;
    BPlusTree tree_;
};

}  // namespace

TEST_F(TreeFixture, EmptyTree) {
    EXPECT_FALSE(tree_.get("x").has_value());
    EXPECT_TRUE(tree_.scan("", "", 10).empty());
    EXPECT_EQ(tree_.check(), 0u);
}

TEST_F(TreeFixture, SequentialInsertsSplitLeavesAndInternals) {
    for (int i = 0; i < 5000; ++i) {
        put(key_of(i), std::string(400, 'v'));  // ~9 cells per leaf: hundreds of leaves
    }
    EXPECT_EQ(tree_.check(), 5000u);
    EXPECT_EQ(tree_.get(key_of(4321)).value_or("").size(), 400u);
    const auto range = tree_.scan(key_of(10), key_of(20), 100);
    ASSERT_EQ(range.size(), 10u);
    EXPECT_EQ(range.front().first, key_of(10));
}

TEST_F(TreeFixture, MatchesStdMapUnderRandomOperations) {
    std::map<std::string, std::string> model;
    std::mt19937 rng(7);
    for (int step = 0; step < 40000; ++step) {
        const std::string key = key_of(static_cast<int>(rng() % 4000));
        if (rng() % 3 != 0) {
            const std::string value(rng() % (kMaxValueSize + 1), static_cast<char>('a' + rng() % 26));
            put(key, value);
            model[key] = value;
        } else {
            EXPECT_EQ(erase(key), model.erase(key) == 1);
        }
        if (step % 5000 == 0) {
            ASSERT_EQ(tree_.check(), model.size());
        }
    }
    ASSERT_EQ(tree_.check(), model.size());
    const auto all = tree_.scan("", "", SIZE_MAX);
    ASSERT_EQ(all.size(), model.size());
    auto it = model.begin();
    for (const auto& [k, v] : all) {
        ASSERT_EQ(k, it->first);
        ASSERT_EQ(v, it->second);
        ++it;
    }
}

TEST_F(TreeFixture, MaximumSizeCellsAlwaysSplitIntoFittingHalves) {
    // Worst case for the split rule: every cell at the maximum size, inserted in reverse.
    for (int i = 300; i > 0; --i) {
        std::string key = key_of(i);
        key.resize(kMaxKeySize, 'k');
        put(key, std::string(kMaxValueSize, 'v'));
    }
    EXPECT_EQ(tree_.check(), 300u);
}
