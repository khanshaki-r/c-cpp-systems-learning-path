// m12_scheduler.cpp
// Two scheduler-style uses of a heap:
//   1. TimerQueue: per-connection idle timeouts with lazy cancellation (a version counter per
//      connection makes stale heap entries skippable), run_until(now) pops everything due,
//      next_deadline() is what an event loop would pass as its poll timeout.
//   2. A job dispatcher that always hands the next job to the worker that becomes free first
//      (min-heap keyed by free_at), i.e. least-loaded assignment, and reports makespan.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m12_scheduler.cpp -o m12_sched

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <vector>

class TimerQueue {
    struct Entry {
        std::int64_t deadline;
        std::size_t conn;
        std::uint64_t version;    // must match version_[conn] to be live
        bool operator>(const Entry& o) const { return deadline > o.deadline; }
    };

public:
    explicit TimerQueue(std::size_t connections) : version_(connections, 0), armed_(connections, false) {}

    // (Re)arm the timeout for a connection. Any earlier entry becomes stale via the version bump.
    void arm(std::size_t conn, std::int64_t deadline) {
        ++version_[conn];
        armed_[conn] = true;
        heap_.push(Entry{deadline, conn, version_[conn]});
    }
    void cancel(std::size_t conn) {       // O(1): the stale entry is skipped when it surfaces
        ++version_[conn];
        armed_[conn] = false;
    }

    // What an event loop passes to poll(): time until the earliest live deadline.
    std::optional<std::int64_t> next_deadline() {
        drop_stale();
        if (heap_.empty()) return std::nullopt;
        return heap_.top().deadline;
    }

    // Fire every live timer with deadline <= now. Returns the connections that timed out, in order.
    std::vector<std::size_t> run_until(std::int64_t now) {
        std::vector<std::size_t> fired;
        while (true) {
            drop_stale();
            if (heap_.empty() || heap_.top().deadline > now) break;
            Entry e = heap_.top();
            heap_.pop();
            armed_[e.conn] = false;
            ++version_[e.conn];           // the fired entry is consumed
            fired.push_back(e.conn);
        }
        return fired;
    }

    std::size_t heap_size() const { return heap_.size(); }

private:
    void drop_stale() {
        while (!heap_.empty() && heap_.top().version != version_[heap_.top().conn]) heap_.pop();
    }
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;   // earliest deadline on top
    std::vector<std::uint64_t> version_;
    std::vector<bool> armed_;
};

namespace {

// Assign jobs (in arrival order) to whichever worker is free first. Returns the makespan.
// Heap of (free_at, worker): O(jobs log workers).
std::int64_t dispatch(const std::vector<std::int64_t>& job_durations, std::size_t workers,
                      std::vector<std::size_t>* assignment) {
    using W = std::pair<std::int64_t, std::size_t>;
    std::priority_queue<W, std::vector<W>, std::greater<W>> free_at;
    for (std::size_t w = 0; w < workers; ++w) free_at.emplace(0, w);
    std::int64_t makespan = 0;
    for (std::int64_t d : job_durations) {
        auto [t, w] = free_at.top();
        free_at.pop();
        if (assignment) assignment->push_back(w);
        std::int64_t done = t + d;
        makespan = std::max(makespan, done);
        free_at.emplace(done, w);
    }
    return makespan;
}

}  // namespace

int main() {
    // ---- TimerQueue --------------------------------------------------------------------------
    {
        TimerQueue tq(4);
        tq.arm(0, 100);
        tq.arm(1, 50);
        tq.arm(2, 75);
        tq.arm(1, 120);                              // activity on conn 1: deadline pushed back
        tq.cancel(2);                                // conn 2 closed
        assert(tq.next_deadline() == 100);           // 50 and 75 are stale, 100 is next
        auto fired = tq.run_until(110);
        assert(fired.size() == 1 && fired[0] == 0);
        assert(tq.next_deadline() == 120);
        fired = tq.run_until(200);
        assert(fired.size() == 1 && fired[0] == 1);
        assert(!tq.next_deadline().has_value());
        std::printf("timer queue: basic arm/rearm/cancel/fire order correct\n");
    }
    // Simulation: 50k connections, random activity resets deadlines; compare the fired set
    // against a brute-force model. Also shows how big the heap grows with lazy deletion.
    {
        std::mt19937 rng(8);
        const std::size_t n = 50'000;
        const std::int64_t idle_timeout = 1000;
        TimerQueue tq(n);
        std::vector<std::int64_t> deadline(n, -1);   // brute-force model
        std::int64_t now = 0;
        for (std::size_t c = 0; c < n; ++c) { tq.arm(c, idle_timeout); deadline[c] = idle_timeout; }
        std::size_t total_fired = 0, max_heap = 0;
        for (int tick = 0; tick < 200; ++tick) {
            now += 10;
            for (int k = 0; k < 2000; ++k) {           // 2000 random activity events per tick
                std::size_t c = rng() % n;
                if (deadline[c] < 0) continue;         // already timed out
                tq.arm(c, now + idle_timeout);
                deadline[c] = now + idle_timeout;
            }
            auto fired = tq.run_until(now);
            for (std::size_t c : fired) { assert(deadline[c] >= 0 && deadline[c] <= now); deadline[c] = -1; }
            std::size_t expected = 0;
            for (std::size_t c = 0; c < n; ++c) if (deadline[c] >= 0 && deadline[c] <= now) ++expected;
            assert(expected == 0);                     // everything due was fired
            total_fired += fired.size();
            max_heap = std::max(max_heap, tq.heap_size());
        }
        std::printf("timer sim: %zu connections, %zu timeouts fired over 2000 ticks, heap peaked at %zu entries (lazy deletion)\n",
                    n, total_fired, max_heap);
    }

    // ---- job dispatcher -------------------------------------------------------------------
    {
        std::vector<std::size_t> who;
        std::int64_t ms = dispatch({5, 3, 8, 2, 7, 4}, 2, &who);
        // worker0: 5 (free 5), worker1: 3 (free 3); 8 -> w1 (11); 2 -> w0 (7); 7 -> w0 (14); 4 -> w1 (15)
        assert(ms == 15 && (who == std::vector<std::size_t>{0, 1, 1, 0, 0, 1}));
        std::mt19937 rng(9);
        std::vector<std::int64_t> jobs(100'000);
        std::int64_t total = 0;
        for (auto& j : jobs) { j = 1 + static_cast<std::int64_t>(rng() % 100); total += j; }
        std::int64_t m8 = dispatch(jobs, 8, nullptr);
        std::printf("dispatch 100k jobs (sum %lld) on 8 workers: makespan %lld (ideal %lld)\n",
                    static_cast<long long>(total), static_cast<long long>(m8), static_cast<long long>(total / 8));
        assert(m8 >= total / 8 && m8 <= total / 8 + 100);
    }
    std::printf("all scheduler checks passed\n");
    return 0;
}
