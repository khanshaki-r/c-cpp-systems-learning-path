// m11_job_scheduler.cpp
// LLD WALKTHROUGH: a job scheduler (in-process, like a mini cron).
//
// Requirements (as clarified):
//   - schedule_at(when, job)       : run once at a time point.
//   - schedule_every(period, job)  : run repeatedly (fixed RATE: next run is
//                                    previous DUE time + period, so it does not drift).
//   - cancel(id) -> bool           : a cancelled job never starts again.
//   - Jobs that throw are counted and do not kill the scheduler. A failing
//     one-shot job is retried up to N times with exponential back-off.
//   - Jobs run on a worker pool so one slow job does not delay the others.
//   - Graceful shutdown: stop() wakes the dispatcher, workers finish their
//     current job, nothing new starts.
//
// Two-layer design (the key interview point):
//   SchedulerCore  : pure logic, single-threaded, time passed in as `now`.
//                    run_due(now) returns the jobs that are due. Fully
//                    deterministic; tests need no threads and no sleeps.
//   Scheduler      : the threaded shell. One dispatcher thread sleeps with
//                    condition_variable::wait_until(next due time), wakes on
//                    new earlier jobs or stop, and hands due jobs to workers.
//
//   +-----------+ owns +---------------+ owns   +--------------------------+
//   | Scheduler |<>--->| SchedulerCore |<>----->| min-heap of (due, seq, id)|
//   | dispatcher|      | jobs_ by id   |        +--------------------------+
//   | workers[] |      +---------------+
//   +-----------+
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11g m11_job_scheduler.cpp

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;
using JobId     = std::uint64_t;
using JobFn     = std::function<void()>;

// ---------------------------------------------------------------------------
// Pure core.
// ---------------------------------------------------------------------------
class SchedulerCore {
public:
    struct Due {                 // what run_due hands out: a job to execute now
        JobId id;
        JobFn fn;
        int attempt;
    };

    JobId schedule_at(TimePoint when, JobFn fn, int max_retries = 0) {
        const JobId id = next_id_++;
        jobs_.emplace(id, Job{std::move(fn), std::nullopt, max_retries, 0});
        heap_.push(Entry{when, seq_++, id});
        return id;
    }

    JobId schedule_every(TimePoint first, Duration period, JobFn fn) {
        const JobId id = next_id_++;
        jobs_.emplace(id, Job{std::move(fn), period, 0, 0});
        heap_.push(Entry{first, seq_++, id});
        return id;
    }

    // Lazy cancellation: erase from the map; the heap entry is skipped when it
    // surfaces. Removing from the middle of a binary heap is O(n); this is O(1).
    // Also forget an in-flight one-shot job, so a failure report cannot re-arm it.
    bool cancel(JobId id) { return jobs_.erase(id) + in_flight_.erase(id) > 0; }

    // Pops every job due at or before `now`. Recurring jobs are re-armed at
    // due + period (catching up at most to `now`: if we were asleep for 10
    // periods we run once, not 10 times; the module discusses the choice).
    std::vector<Due> run_due(TimePoint now) {
        std::vector<Due> out;
        while (!heap_.empty() && heap_.top().due <= now) {
            Entry e = heap_.top();
            heap_.pop();
            auto it = jobs_.find(e.id);
            if (it == jobs_.end()) continue;                 // cancelled
            Job& j = it->second;
            out.push_back(Due{e.id, j.fn, j.attempt});
            if (j.period) {
                TimePoint next = e.due + *j.period;
                while (next <= now) next += *j.period;      // skip missed runs
                heap_.push(Entry{next, seq_++, e.id});
            } else {
                in_flight_.emplace(e.id, std::move(j));      // keep for a possible retry
                jobs_.erase(it);
            }
        }
        return out;
    }

    // The executor reports back. Failed one-shot jobs with retries left are
    // re-armed with exponential back-off: base * 2^attempt.
    void report(JobId id, bool ok, TimePoint now, Duration backoff_base) {
        auto it = in_flight_.find(id);
        if (it == in_flight_.end()) return;                  // recurring or cancelled
        Job j = std::move(it->second);
        in_flight_.erase(it);
        if (ok || j.attempt >= j.max_retries) return;
        ++j.attempt;
        const TimePoint when = now + backoff_base * (1 << (j.attempt - 1));
        jobs_.emplace(id, std::move(j));
        heap_.push(Entry{when, seq_++, id});
    }

    std::optional<TimePoint> next_due() {
        while (!heap_.empty() && !jobs_.count(heap_.top().id)) heap_.pop();   // drop cancelled
        if (heap_.empty()) return std::nullopt;
        return heap_.top().due;
    }

    std::size_t pending() const { return jobs_.size(); }

private:
    struct Job {
        JobFn fn;
        std::optional<Duration> period;
        int max_retries;
        int attempt;
    };
    struct Entry {
        TimePoint due;
        std::uint64_t seq;       // tie-breaker: FIFO among jobs due at the same instant
        JobId id;
        bool operator>(const Entry& o) const { return due != o.due ? due > o.due : seq > o.seq; }
    };
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;   // min-heap
    std::unordered_map<JobId, Job> jobs_, in_flight_;
    JobId next_id_ = 1;
    std::uint64_t seq_ = 0;
};

// ---------------------------------------------------------------------------
// Threaded shell.
// ---------------------------------------------------------------------------
class Scheduler {
public:
    explicit Scheduler(std::size_t workers, Duration backoff_base = std::chrono::milliseconds(10))
        : backoff_(backoff_base) {
        for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
        dispatcher_ = std::thread([this] { dispatch(); });
    }
    ~Scheduler() { stop(); }
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    JobId schedule_after(Duration d, JobFn fn, int max_retries = 0) {
        std::lock_guard<std::mutex> g(m_);
        const JobId id = core_.schedule_at(Clock::now() + d, std::move(fn), max_retries);
        cv_.notify_all();        // the new job may be earlier than what the dispatcher waits for
        return id;
    }
    JobId schedule_every(Duration period, JobFn fn) {
        std::lock_guard<std::mutex> g(m_);
        const JobId id = core_.schedule_every(Clock::now() + period, period, std::move(fn));
        cv_.notify_all();
        return id;
    }
    bool cancel(JobId id) { std::lock_guard<std::mutex> g(m_); return core_.cancel(id); }

    void stop() {
        {
            std::lock_guard<std::mutex> g(m_);
            if (stopped_) return;
            stopped_ = true;
        }
        cv_.notify_all();
        work_cv_.notify_all();
        dispatcher_.join();
        for (auto& w : workers_) w.join();
    }

    std::uint64_t failures() const { return failures_.load(); }

private:
    void dispatch() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stopped_) {
            auto next = core_.next_due();
            if (!next) { cv_.wait(lk); continue; }                 // nothing scheduled
            if (cv_.wait_until(lk, *next) == std::cv_status::no_timeout) continue;   // woken: re-check
            for (auto& d : core_.run_due(Clock::now())) ready_.push_back(std::move(d));
            work_cv_.notify_all();
        }
    }

    void work() {
        for (;;) {
            SchedulerCore::Due d;
            {
                std::unique_lock<std::mutex> lk(m_);
                work_cv_.wait(lk, [this] { return stopped_ || !ready_.empty(); });
                if (stopped_) return;                               // do not start new work
                d = std::move(ready_.front());
                ready_.pop_front();
            }
            bool ok = true;
            try { d.fn(); }                                         // OUTSIDE the lock
            catch (...) { ok = false; ++failures_; }
            std::lock_guard<std::mutex> g(m_);
            core_.report(d.id, ok, Clock::now(), backoff_);
            cv_.notify_all();                                       // a retry may now be armed
        }
    }

    const Duration backoff_;
    std::mutex m_;
    std::condition_variable cv_, work_cv_;
    SchedulerCore core_;
    std::deque<SchedulerCore::Due> ready_;
    bool stopped_ = false;
    std::atomic<std::uint64_t> failures_{0};
    std::vector<std::thread> workers_;
    std::thread dispatcher_;                 // threads LAST: everything they use exists first
};

int main() {
    using namespace std::chrono_literals;

    // 1. Deterministic tests of the core: no threads, no sleeping.
    {
        SchedulerCore c;
        const TimePoint t0{};
        std::vector<int> ran;
        c.schedule_at(t0 + 30ms, [&] { ran.push_back(3); });
        c.schedule_at(t0 + 10ms, [&] { ran.push_back(1); });
        const JobId b = c.schedule_at(t0 + 20ms, [&] { ran.push_back(2); });
        c.schedule_at(t0 + 10ms, [&] { ran.push_back(11); });       // same time: FIFO after "1"
        assert(c.cancel(b) && !c.cancel(b));
        for (auto& d : c.run_due(t0 + 25ms)) d.fn();
        assert((ran == std::vector<int>{1, 11}));
        assert(c.next_due() == t0 + 30ms);
        for (auto& d : c.run_due(t0 + 1s)) d.fn();
        assert((ran == std::vector<int>{1, 11, 3}));
        assert(!c.next_due());
    }
    {
        SchedulerCore c;
        const TimePoint t0{};
        int n = 0;
        const JobId r = c.schedule_every(t0 + 10ms, 10ms, [&] { ++n; });
        for (auto& d : c.run_due(t0 + 10ms)) d.fn();
        for (auto& d : c.run_due(t0 + 20ms)) d.fn();
        for (auto& d : c.run_due(t0 + 75ms)) d.fn();                // missed 30..70: runs once
        assert(n == 3 && c.next_due() == t0 + 80ms);
        assert(c.cancel(r) && !c.next_due());
    }
    {
        SchedulerCore c;                                            // retry with back-off
        const TimePoint t0{};
        const JobId id = c.schedule_at(t0, [] {}, /*max_retries=*/2);
        auto d = c.run_due(t0);
        assert(d.size() == 1 && d[0].attempt == 0);
        c.report(id, false, t0, 100ms);                             // retry at +100 ms
        assert(c.next_due() == t0 + 100ms);
        d = c.run_due(t0 + 100ms);
        c.report(id, false, t0 + 100ms, 100ms);                     // retry at +200 ms more
        assert(c.next_due() == t0 + 300ms);
        d = c.run_due(t0 + 300ms);
        assert(d[0].attempt == 2);
        c.report(id, false, t0 + 300ms, 100ms);                     // out of retries
        assert(!c.next_due() && c.pending() == 0);
    }

    // 2. Threaded shell, real time (short delays).
    {
        Scheduler s(2, 5ms);
        std::promise<void> once;
        s.schedule_after(20ms, [&] { once.set_value(); });
        assert(once.get_future().wait_for(2s) == std::future_status::ready);

        std::atomic<int> ticks{0};
        const JobId tick = s.schedule_every(5ms, [&] { ++ticks; });
        while (ticks < 3) std::this_thread::yield();
        assert(s.cancel(tick));

        std::atomic<int> attempts{0};
        std::promise<void> third;
        s.schedule_after(1ms, [&] {
            if (++attempts < 3) throw std::runtime_error("transient");
            third.set_value();
        }, /*max_retries=*/3);
        assert(third.get_future().wait_for(2s) == std::future_status::ready);
        assert(s.failures() == 2);

        const JobId never = s.schedule_after(1h, [] { assert(false); });
        assert(s.cancel(never));
        s.stop();                                                   // prompt even with a 1 h job
    }

    std::puts("m11_job_scheduler: all checks passed");
    return 0;
}
