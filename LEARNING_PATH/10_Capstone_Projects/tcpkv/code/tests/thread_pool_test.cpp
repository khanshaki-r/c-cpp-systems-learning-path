#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "tcpkv/log.hpp"
#include "tcpkv/thread_pool.hpp"

using tcpkv::MoveOnlyFunction;
using tcpkv::ThreadPool;

namespace {

struct QuietLogs : ::testing::Test {
    void SetUp() override { tcpkv::log::set_level(tcpkv::log::Level::Warn); }
};

}  // namespace

TEST(MoveOnlyFunction, HoldsAndInvokesMoveOnlyCallable) {
    auto owned = std::make_unique<int>(42);
    int seen = 0;
    MoveOnlyFunction fn([p = std::move(owned), &seen] { seen = *p; });
    EXPECT_TRUE(static_cast<bool>(fn));
    fn();
    EXPECT_EQ(seen, 42);

    MoveOnlyFunction moved(std::move(fn));
    EXPECT_FALSE(static_cast<bool>(fn));  // NOLINT: checking moved-from state is the point
    EXPECT_TRUE(static_cast<bool>(moved));
}

TEST(MoveOnlyFunction, DefaultConstructedIsEmpty) {
    const MoveOnlyFunction fn;
    EXPECT_FALSE(static_cast<bool>(fn));
}

using ThreadPoolTest = QuietLogs;

TEST_F(ThreadPoolTest, RunsEveryTask) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(pool.submit([&counter] { ++counter; }));
    }
    pool.drain();
    EXPECT_EQ(counter.load(), 1000);
    EXPECT_EQ(pool.pending(), 0u);
}

TEST_F(ThreadPoolTest, SizeIsAtLeastOne) {
    const ThreadPool pool(0);
    EXPECT_EQ(pool.size(), 1u);
}

TEST_F(ThreadPoolTest, TasksRunOnWorkerThreadsNotTheCaller) {
    ThreadPool pool(3);
    std::mutex mutex;
    std::set<std::thread::id> ids;
    for (int i = 0; i < 200; ++i) {
        pool.submit([&] {
            std::lock_guard<std::mutex> lock(mutex);
            ids.insert(std::this_thread::get_id());
        });
    }
    pool.drain();
    EXPECT_GE(ids.size(), 1u);
    EXPECT_LE(ids.size(), 3u);
    EXPECT_EQ(ids.count(std::this_thread::get_id()), 0u);
}

TEST_F(ThreadPoolTest, AcceptsMoveOnlyTasks) {
    ThreadPool pool(2);
    auto payload = std::make_unique<int>(7);
    std::atomic<int> seen{0};
    ASSERT_TRUE(pool.submit([p = std::move(payload), &seen] { seen = *p; }));
    pool.drain();
    EXPECT_EQ(seen.load(), 7);
}

TEST_F(ThreadPoolTest, ExceptionInTaskDoesNotKillTheWorker) {
    ThreadPool pool(1);  // one worker: if it died, the second task could never run
    std::atomic<bool> second_ran{false};
    ASSERT_TRUE(pool.submit([] { throw std::runtime_error("boom"); }));
    ASSERT_TRUE(pool.submit([&] { second_ran = true; }));
    pool.drain();
    EXPECT_TRUE(second_ran.load());
}

TEST_F(ThreadPoolTest, StopDropsQueuedTasksAndRejectsNewOnes) {
    std::atomic<bool> started{false};
    std::atomic<int> ran{0};
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    {
        ThreadPool pool(1);
        ASSERT_TRUE(pool.submit([&, opened] {
            started = true;
            opened.wait();
            ++ran;
        }));
        while (!started.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 10; ++i) {
            ASSERT_TRUE(pool.submit([&] { ++ran; }));
        }
        EXPECT_EQ(pool.pending(), 10u);

        pool.stop();
        EXPECT_EQ(pool.pending(), 0u);
        EXPECT_FALSE(pool.submit([&] { ++ran; }));
        pool.stop();  // idempotent

        gate.set_value();  // let the running task finish; the destructor joins it
    }
    EXPECT_EQ(ran.load(), 1);
}

TEST_F(ThreadPoolTest, DestructorWaitsForRunningTask) {
    std::atomic<bool> finished{false};
    {
        ThreadPool pool(2);
        // The contract: stop() drops QUEUED tasks but lets RUNNING ones finish. So the test must
        // wait until a worker has actually picked the task up; otherwise, on a slow run (ASan,
        // Valgrind, loaded CI box) the destructor can fire while the task is still queued and
        // legitimately discard it. That race made this test flaky under ASan.
        std::promise<void> started;
        auto started_future = started.get_future();
        ASSERT_TRUE(pool.submit([&] {
            started.set_value();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            finished = true;
        }));
        started_future.wait();
    }
    EXPECT_TRUE(finished.load());
}

TEST_F(ThreadPoolTest, DrainReturnsWhenIdleAndAgainLater) {
    ThreadPool pool(2);
    pool.drain();  // nothing queued: must not block
    std::atomic<int> n{0};
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 50; ++i) {
            pool.submit([&n] { ++n; });
        }
        pool.drain();
        EXPECT_EQ(n.load(), 50 * (round + 1));
    }
}

TEST_F(ThreadPoolTest, QueuedTaskDestructorsRunWhenDropped) {
    struct Probe {
        std::atomic<int>* destroyed;
        explicit Probe(std::atomic<int>* d) : destroyed(d) {}
        Probe(Probe&& other) noexcept : destroyed(other.destroyed) { other.destroyed = nullptr; }
        Probe(const Probe&) = delete;
        ~Probe() {
            if (destroyed != nullptr) {
                ++*destroyed;
            }
        }
    };

    std::atomic<int> destroyed{0};
    std::atomic<bool> started{false};
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    {
        ThreadPool pool(1);
        pool.submit([&, opened] {
            started = true;
            opened.wait();
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 5; ++i) {
            pool.submit([probe = Probe(&destroyed)] { (void)probe; });
        }
        pool.stop();
        EXPECT_EQ(destroyed.load(), 5);  // the Connection-like payloads were released by stop()
        gate.set_value();
    }
}
