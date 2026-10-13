// The crash test: a child process writes as fast as it can and reports every acknowledged
// operation through a pipe; the parent SIGKILLs it at a random moment, reopens the database
// and checks that the state equals the model after exactly the acknowledged operations
// (or after one more: the operation in flight may have committed just before the kill).
//
// SIGKILL cannot be caught, so no destructor, no checkpoint and no flush runs in the child.
// That is the same as a process crash. It is not the same as a power failure (the kernel
// still writes back the page cache); see the spec, section 8, for what that would take.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "minidb/db.hpp"
#include "test_util.hpp"

using namespace minidb;

namespace {

constexpr int kKeySpace = 400;

std::string key_of(int i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "k%05d", i % kKeySpace);
    return buf;
}

// Operation i is a pure function of i, so the parent can recompute the expected state for
// any prefix of the operation sequence without talking to the child.
struct Op {
    bool is_delete = false;
    std::string key;
    std::string value;
};

Op op_at(std::uint32_t i) {
    Op op;
    op.key = key_of(static_cast<int>((i * 7u) % kKeySpace));
    op.is_delete = i % 11 == 10;
    if (!op.is_delete) {
        op.value = "v" + std::to_string(i) + ":" + std::string(i % 600, 'x');  // 0..~610 bytes
    }
    return op;
}

std::map<std::string, std::string> model_after(std::uint32_t count) {
    std::map<std::string, std::string> model;
    for (std::uint32_t i = 0; i < count; ++i) {
        const Op op = op_at(i);
        if (op.is_delete) {
            model.erase(op.key);
        } else {
            model[op.key] = op.value;
        }
    }
    return model;
}

[[noreturn]] void child_main(const std::string& path, int ack_fd) {
    try {
        // Small cache and a small checkpoint threshold, so kills land during evictions and
        // during checkpoints, not only during commits.
        Db db(path, Options{16, 128u << 10});
        for (std::uint32_t i = 0;; ++i) {
            const Op op = op_at(i);
            if (op.is_delete) {
                db.del(op.key);
            } else {
                db.put(op.key, op.value);
            }
            // The acknowledgement: operation i returned, so it must survive any crash.
            const std::uint32_t done = i + 1;
            if (::write(ack_fd, &done, sizeof done) != static_cast<ssize_t>(sizeof done)) {
                ::_exit(3);
            }
        }
    } catch (...) {
        ::_exit(2);
    }
}

std::map<std::string, std::string> contents(Db& db) {
    std::map<std::string, std::string> out;
    for (auto& [k, v] : db.scan("", "")) {
        out.emplace(std::move(k), std::move(v));
    }
    return out;
}

}  // namespace

TEST(Crash, SigkillMidWriteLosesNoAcknowledgedOperation) {
    TempDir dir;
    const std::string path = dir.file("db");
    std::mt19937 rng(std::random_device{}());
    std::uint32_t total_acked = 0;

    for (int round = 0; round < 25; ++round) {
        // Every round starts from an empty database, so the expected state is simply the
        // model after the first `acked` operations.
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");

        int fds[2];
        ASSERT_EQ(::pipe(fds), 0);
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            ::close(fds[0]);
            child_main(path, fds[1]);  // never returns
        }
        ::close(fds[1]);

        // Let the child run for a random 20..200 ms, then kill it wherever it is.
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + rng() % 180));
        ASSERT_EQ(::kill(pid, SIGKILL), 0);
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFSIGNALED(status)) << "child exited early with status " << WEXITSTATUS(status);

        // Acknowledgements written before the kill are still in the pipe. read() returns 0
        // once they are drained, because the only write end died with the child.
        std::uint32_t acked = 0;
        std::uint32_t value = 0;
        while (::read(fds[0], &value, sizeof value) == static_cast<ssize_t>(sizeof value)) {
            acked = value;
        }
        ::close(fds[0]);

        Db db(path, Options{16, 128u << 10});  // runs recovery
        db.check();
        const auto actual = contents(db);
        EXPECT_TRUE(actual == model_after(acked) || actual == model_after(acked + 1))
            << "round " << round << ": state matches neither " << acked << " nor " << acked + 1
            << " operations (recovered " << db.recovery_stats().committed_txns << " txns, discarded "
            << db.recovery_stats().discarded_records << " records)";
        total_acked += acked;
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_GT(total_acked, 0u) << "the child never acknowledged anything; the test proved nothing";
    std::printf("[ crash ] %u operations acknowledged across 25 kills\n", total_acked);
}
