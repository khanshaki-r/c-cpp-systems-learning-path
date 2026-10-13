// tests/test_gmock_example.cpp - gMock introduction (module 05, section 2.7).
//
// Scenario: a future tcpkv feature persists every SET to a write-ahead log. We do not want the
// unit test to touch disk, so the logger is an interface and the test injects a mock.
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "tcpkv/store.hpp"

namespace {

// The seam: production code depends on this interface, not on a concrete file.
class WriteAheadLog {
public:
    virtual ~WriteAheadLog() = default;
    virtual bool append(std::string_view key, std::string_view value) = 0;
    virtual void sync() = 0;
};

// Mock: MOCK_METHOD(return, name, (args), (qualifiers)).
class MockWal : public WriteAheadLog {
public:
    MOCK_METHOD(bool, append, (std::string_view key, std::string_view value), (override));
    MOCK_METHOD(void, sync, (), (override));
};

// A small unit under test that composes Store with the log.
class PersistentStore {
public:
    explicit PersistentStore(WriteAheadLog& wal) : wal_(wal) {}

    // Returns false and leaves the store untouched if the log rejects the write: durability first.
    bool set(std::string key, std::string value) {
        if (!wal_.append(key, value)) return false;
        store_.set(std::move(key), std::move(value));
        return true;
    }
    void checkpoint() { wal_.sync(); }
    std::optional<std::string> get(std::string_view key) const { return store_.get(key); }

private:
    WriteAheadLog& wal_;
    tcpkv::Store store_;
};

using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;

TEST(PersistentStore, SetAppendsToLogBeforeStoring) {
    StrictMock<MockWal> wal;   // StrictMock: any un-EXPECTed call fails the test
    PersistentStore ps(wal);

    EXPECT_CALL(wal, append("k", "v")).WillOnce(Return(true));

    EXPECT_TRUE(ps.set("k", "v"));
    EXPECT_EQ(ps.get("k").value(), "v");
}

TEST(PersistentStore, FailedLogAppendLeavesStoreUnchanged) {
    StrictMock<MockWal> wal;
    PersistentStore ps(wal);

    EXPECT_CALL(wal, append(_, _)).WillOnce(Return(false));

    EXPECT_FALSE(ps.set("k", "v"));
    EXPECT_FALSE(ps.get("k").has_value());
}

TEST(PersistentStore, CheckpointSyncsExactlyOnce) {
    StrictMock<MockWal> wal;
    PersistentStore ps(wal);

    EXPECT_CALL(wal, sync()).Times(1);

    ps.checkpoint();
}

}  // namespace
