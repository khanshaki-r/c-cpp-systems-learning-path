// tests/test_store.cpp - unit tests for Store, including a concurrency smoke test that
// ThreadSanitizer (preset 'tsan') turns into a real race detector.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/store.hpp"

// ---------------------------------------------------------------- fixture
// A fixture gives every test a fresh Store plus shared helpers. GoogleTest constructs a new
// fixture object per TEST_F, so tests cannot leak state into each other.
class StoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_.set("a", "1");
        store_.set("b", "2");
    }
    tcpkv::Store store_;
};

TEST_F(StoreTest, GetReturnsValueForExistingKey) {
    const auto v = store_.get("a");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "1");
}

TEST_F(StoreTest, GetReturnsNulloptForMissingKey) {
    EXPECT_FALSE(store_.get("missing").has_value());
}

TEST_F(StoreTest, SetOverwritesExistingValue) {
    store_.set("a", "changed");
    EXPECT_EQ(store_.get("a").value(), "changed");
    EXPECT_EQ(store_.size(), 2u);  // overwrite does not add an entry
}

TEST_F(StoreTest, DelReturnsTrueOnlyWhenKeyExisted) {
    EXPECT_TRUE(store_.del("a"));
    EXPECT_FALSE(store_.del("a"));
    EXPECT_FALSE(store_.get("a").has_value());
    EXPECT_EQ(store_.size(), 1u);
}

TEST_F(StoreTest, ClearEmptiesStore) {
    store_.clear();
    EXPECT_EQ(store_.size(), 0u);
    EXPECT_FALSE(store_.get("a").has_value());
}

TEST_F(StoreTest, EmptyValueIsStoredAndDistinctFromMissing) {
    store_.set("empty", "");
    const auto v = store_.get("empty");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->empty());
}

TEST_F(StoreTest, KeysAreCaseSensitive) {
    store_.set("Key", "upper");
    EXPECT_EQ(store_.get("Key").value(), "upper");
    EXPECT_FALSE(store_.get("key").has_value());
}

TEST_F(StoreTest, LargeValueRoundTrips) {
    const std::string big(64 * 1024, 'x');
    store_.set("big", big);
    EXPECT_EQ(store_.get("big").value(), big);
}

// ---------------------------------------------------------------- concurrency
// Under 'tsan' this is a race detector for Store; under 'asan' it checks lifetime bugs; in a
// plain build it is a smoke test. Deterministic expected results make it a real test, not a demo.
TEST(StoreConcurrency, ParallelWritersToDisjointKeysAllLand) {
    tcpkv::Store store;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                store.set("t" + std::to_string(t) + ":" + std::to_string(i), std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(store.size(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(store.get("t3:1999").value(), "1999");
}

TEST(StoreConcurrency, ReadersAndWritersOnSameKeySeeOnlyValidValues) {
    tcpkv::Store store;
    store.set("hot", "0");
    std::atomic<bool> stop{false};
    std::atomic<int> bad_reads{0};

    std::thread writer([&] {
        for (int i = 1; i <= 5000; ++i) store.set("hot", std::to_string(i));
        stop = true;
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop) {
                const auto v = store.get("hot");
                // Every observed value must be a complete integer string: torn values mean a race.
                if (!v || v->empty() || v->find_first_not_of("0123456789") != std::string::npos) {
                    ++bad_reads;
                }
            }
        });
    }
    writer.join();
    for (auto& th : readers) th.join();
    EXPECT_EQ(bad_reads.load(), 0);
    EXPECT_EQ(store.get("hot").value(), "5000");
}
