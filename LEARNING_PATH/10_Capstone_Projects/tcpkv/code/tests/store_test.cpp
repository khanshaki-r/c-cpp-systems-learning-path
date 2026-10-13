#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tcpkv/store.hpp"

using tcpkv::Store;

// ------------------------------------------------------------------ single-threaded semantics

TEST(Store, GetMissingKeyReturnsNullopt) {
    Store store;
    EXPECT_FALSE(store.get("nope").has_value());
    EXPECT_EQ(store.size(), 0u);
}

TEST(Store, SetThenGet) {
    Store store;
    store.set("k", "v");
    const auto value = store.get("k");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "v");
    EXPECT_EQ(store.size(), 1u);
}

TEST(Store, SetOverwrites) {
    Store store;
    store.set("k", "one");
    store.set("k", "two");
    EXPECT_EQ(store.get("k").value_or(""), "two");
    EXPECT_EQ(store.size(), 1u);
}

TEST(Store, EmptyAndBinaryValuesRoundTrip) {
    Store store;
    store.set("empty", "");
    const std::string binary("\x00\x01\xff\n", 4);
    store.set("bin", binary);
    ASSERT_TRUE(store.get("empty").has_value());
    EXPECT_TRUE(store.get("empty")->empty());
    EXPECT_EQ(store.get("bin").value_or(""), binary);
}

TEST(Store, DelReportsWhetherKeyExisted) {
    Store store;
    store.set("k", "v");
    EXPECT_TRUE(store.del("k"));
    EXPECT_FALSE(store.del("k"));
    EXPECT_FALSE(store.get("k").has_value());
}

TEST(Store, Exists) {
    Store store;
    EXPECT_FALSE(store.exists("k"));
    store.set("k", "v");
    EXPECT_TRUE(store.exists("k"));
}

TEST(Store, KeysAreSortedAndFilteredByPrefix) {
    Store store;
    store.set("user:2", "");
    store.set("user:1", "");
    store.set("order:9", "");

    const std::vector<std::string> all = {"order:9", "user:1", "user:2"};
    EXPECT_EQ(store.keys(), all);
    const std::vector<std::string> users = {"user:1", "user:2"};
    EXPECT_EQ(store.keys("user:"), users);
    EXPECT_TRUE(store.keys("zzz").empty());
}

TEST(Store, ExpireOnMissingKeyReturnsFalse) {
    Store store;
    EXPECT_FALSE(store.expire("nope", std::chrono::seconds(10)));
}

TEST(Store, TtlSemantics) {
    Store store;
    EXPECT_EQ(store.ttl("missing"), -2);
    store.set("forever", "v");
    EXPECT_EQ(store.ttl("forever"), -1);
    store.set("temp", "v");
    EXPECT_TRUE(store.expire("temp", std::chrono::seconds(100)));
    const long long ttl = store.ttl("temp");
    EXPECT_GE(ttl, 99);
    EXPECT_LE(ttl, 100);
}

TEST(Store, SetClearsTtl) {
    Store store;
    store.set("k", "v");
    ASSERT_TRUE(store.expire("k", std::chrono::seconds(100)));
    store.set("k", "again");
    EXPECT_EQ(store.ttl("k"), -1);
}

TEST(Store, ExpireWithZeroOrNegativeDeletesImmediately) {
    Store store;
    store.set("a", "v");
    store.set("b", "v");
    EXPECT_TRUE(store.expire("a", std::chrono::seconds(0)));
    EXPECT_TRUE(store.expire("b", std::chrono::seconds(-5)));
    EXPECT_FALSE(store.exists("a"));
    EXPECT_FALSE(store.exists("b"));
    EXPECT_EQ(store.size(), 0u);
}

TEST(Store, ExpiredKeyIsInvisibleUntilPurged) {
    Store store;
    store.set("k", "v");
    ASSERT_TRUE(store.expire("k", std::chrono::milliseconds(20)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Logically gone...
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_FALSE(store.exists("k"));
    EXPECT_TRUE(store.keys().empty());
    EXPECT_EQ(store.ttl("k"), -2);
    EXPECT_FALSE(store.expire("k", std::chrono::seconds(5)));
    // ...but physically still there until purge runs.
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.purge_expired(), 1u);
    EXPECT_EQ(store.size(), 0u);
}

TEST(Store, DelOnExpiredKeyReturnsFalseAndRemovesIt) {
    Store store;
    store.set("k", "v");
    ASSERT_TRUE(store.expire("k", std::chrono::milliseconds(10)));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_FALSE(store.del("k"));
    EXPECT_EQ(store.size(), 0u);
}

TEST(Store, SetAfterExpiryRevivesKey) {
    Store store;
    store.set("k", "old");
    ASSERT_TRUE(store.expire("k", std::chrono::milliseconds(10)));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    store.set("k", "new");
    EXPECT_EQ(store.get("k").value_or(""), "new");
    EXPECT_EQ(store.ttl("k"), -1);
}

// ------------------------------------------------------------------ multi-threaded stress
//
// These tests cannot prove the absence of races, but under ThreadSanitizer (cmake --preset tsan)
// any access to the map outside the lock is reported. Without TSan they still catch torn
// values and crashes.

namespace {

constexpr int kKeys = 32;
constexpr int kIterations = 20000;

std::string key_for(int i) { return "k" + std::to_string(i % kKeys); }

// Writer `id` always stores a run of one repeated letter, so a reader can detect a torn value.
std::string value_for(int writer, int i) {
    return std::string(static_cast<std::size_t>(1 + i % 64), static_cast<char>('a' + writer));
}

bool looks_valid(const std::string& value, int writers) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const char c = value.front();
    if (c < 'a' || c >= 'a' + writers) {
        return false;
    }
    for (const char x : value) {
        if (x != c) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(StoreStress, ConcurrentReadersAndWriters) {
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;

    Store store;
    std::atomic<int> torn{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kIterations; ++i) {
                store.set(key_for(i), value_for(w, i));
            }
        });
    }
    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&, r] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kIterations; ++i) {
                const auto value = store.get(key_for(i + r));
                if (value && !looks_valid(*value, kWriters)) {
                    ++torn;
                }
                if (i % 1000 == 0) {
                    EXPECT_LE(store.keys("k").size(), static_cast<std::size_t>(kKeys));
                    (void)store.exists(key_for(i));
                }
            }
        });
    }
    go.store(true);
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(torn.load(), 0);
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        const auto value = store.get(key_for(i));
        ASSERT_TRUE(value.has_value());
        EXPECT_TRUE(looks_valid(*value, kWriters));
    }
}

TEST(StoreStress, SetDeleteAndExpireRace) {
    constexpr int kWriters = 3;

    Store store;
    std::atomic<int> torn{0};
    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < kIterations; ++i) {
                store.set(key_for(i), value_for(w, i));
            }
        });
    }
    threads.emplace_back([&] {
        for (int i = 0; i < kIterations; ++i) {
            store.del(key_for(i * 7));
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < kIterations; ++i) {
            store.expire(key_for(i * 3), std::chrono::milliseconds(1));
            if (i % 500 == 0) {
                store.purge_expired();
            }
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < kIterations; ++i) {
            const auto value = store.get(key_for(i));
            if (value && !looks_valid(*value, kWriters)) {
                ++torn;
            }
            (void)store.ttl(key_for(i + 1));
        }
    });
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(torn.load(), 0);
    // Whatever interleaving happened, the store must still be fully usable.
    for (int i = 0; i < kKeys; ++i) {
        store.set(key_for(i), "final");
    }
    store.purge_expired();
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) {
        EXPECT_EQ(store.get(key_for(i)).value_or(""), "final");
    }
}
