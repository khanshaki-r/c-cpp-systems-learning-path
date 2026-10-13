// m11_rate_limiter.cpp
// LLD WALKTHROUGH: a rate limiter.
//
// Requirements (as clarified):
//   - allow(client_key) -> true/false. Limits are per key (per customer id,
//     per API key, per IP).
//   - Three algorithms, chosen by configuration:
//       TokenBucket       : capacity C, refill R tokens/second. Allows bursts
//                           up to C, long-run rate R. The default choice.
//       FixedWindow       : at most N per aligned window (e.g. per minute).
//                           Cheap; allows 2N at a window boundary.
//       SlidingWindowLog  : at most N in ANY trailing window. Exact; memory
//                           O(N) per key.
//   - Thread-safe. Memory bounded: idle keys are evicted.
//   - Testable: time is injected; tests never sleep.
//
//   +---------------------+  owns *   +------------------------+
//   | KeyedRateLimiter    |<>-------->| Algorithm (interface)  |
//   |  allow(key)         |  per key  |  try_acquire(now)      |
//   |  evict_idle(now)    |           +------------------------+
//   |  factory: () -> Alg |              ^         ^         ^
//   +---------------------+   TokenBucket  FixedWindow  SlidingWindowLog
//
// Design point: the ALGORITHM is single-key and single-threaded and takes
// `now` as an argument. Locking, key lookup and eviction live in the keyed
// wrapper. That split keeps each algorithm a 15-line pure function of its
// state, which is the thing the interviewer wants to see you get right.
//
// Build: g++ -std=c++17 -Wall -Wextra -Werror -pthread -o m11e m11_rate_limiter.cpp

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = std::chrono::nanoseconds;

class Algorithm {
public:
    virtual ~Algorithm() = default;
    virtual bool try_acquire(TimePoint now) = 0;
};

// ---------------------------------------------------------------------------
// Token bucket. State: tokens (double) and the last refill time. Refill is
// computed lazily on each call, so there is no timer thread per key.
// ---------------------------------------------------------------------------
class TokenBucket final : public Algorithm {
public:
    TokenBucket(double capacity, double refill_per_sec, TimePoint now)
        : capacity_(capacity), rate_(refill_per_sec), tokens_(capacity), last_(now) {}
    bool try_acquire(TimePoint now) override {
        if (now > last_) {        // steady_clock never goes back, but be defensive
            const double secs = std::chrono::duration<double>(now - last_).count();
            tokens_ = std::min(capacity_, tokens_ + secs * rate_);
            last_ = now;
        }
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }
private:
    const double capacity_, rate_;
    double tokens_;
    TimePoint last_;
};

// ---------------------------------------------------------------------------
// Fixed window. Windows are aligned to multiples of `window` since the clock
// epoch, so every key's windows start at the same instants.
// ---------------------------------------------------------------------------
class FixedWindow final : public Algorithm {
public:
    FixedWindow(int limit, Duration window) : limit_(limit), window_(window) {}
    bool try_acquire(TimePoint now) override {
        const auto idx = now.time_since_epoch() / window_;   // integer division
        if (idx != current_) { current_ = idx; count_ = 0; }
        if (count_ >= limit_) return false;
        ++count_;
        return true;
    }
private:
    const int limit_;
    const Duration window_;
    Duration::rep current_ = -1;
    int count_ = 0;
};

// ---------------------------------------------------------------------------
// Sliding window log: keep the timestamps of accepted requests in the last
// window. Exact, but O(limit) memory per key. (The "sliding window counter"
// approximation keeps two fixed-window counts and weights them; see module.)
// ---------------------------------------------------------------------------
class SlidingWindowLog final : public Algorithm {
public:
    SlidingWindowLog(std::size_t limit, Duration window) : limit_(limit), window_(window) {}
    bool try_acquire(TimePoint now) override {
        while (!log_.empty() && now - log_.front() >= window_) log_.pop_front();
        if (log_.size() >= limit_) return false;
        log_.push_back(now);
        return true;
    }
private:
    const std::size_t limit_;
    const Duration window_;
    std::deque<TimePoint> log_;
};

// ---------------------------------------------------------------------------
// Keyed, thread-safe wrapper with lock striping: 16 independent maps, each
// with its own mutex, chosen by hash(key). Two different customers rarely
// contend. Eviction drops keys idle for longer than `idle_ttl`.
// ---------------------------------------------------------------------------
class KeyedRateLimiter {
public:
    using Factory = std::function<std::unique_ptr<Algorithm>(TimePoint now)>;
    using NowFn   = std::function<TimePoint()>;

    KeyedRateLimiter(Factory f, NowFn now, Duration idle_ttl)
        : factory_(std::move(f)), now_(std::move(now)), idle_ttl_(idle_ttl) {}

    bool allow(const std::string& key) {
        const TimePoint now = now_();
        Stripe& s = stripes_[std::hash<std::string>{}(key) % kStripes];
        std::lock_guard<std::mutex> g(s.m);
        auto it = s.map.find(key);
        if (it == s.map.end()) it = s.map.emplace(key, Entry{factory_(now), now}).first;
        it->second.last_seen = now;
        return it->second.algo->try_acquire(now);
    }

    // Called by a maintenance timer. Holds one stripe lock at a time.
    std::size_t evict_idle() {
        const TimePoint now = now_();
        std::size_t n = 0;
        for (auto& s : stripes_) {
            std::lock_guard<std::mutex> g(s.m);
            for (auto it = s.map.begin(); it != s.map.end();) {
                if (now - it->second.last_seen >= idle_ttl_) { it = s.map.erase(it); ++n; }
                else ++it;
            }
        }
        return n;
    }

    std::size_t tracked_keys() {
        std::size_t n = 0;
        for (auto& s : stripes_) { std::lock_guard<std::mutex> g(s.m); n += s.map.size(); }
        return n;
    }

private:
    static constexpr std::size_t kStripes = 16;
    struct Entry {
        std::unique_ptr<Algorithm> algo;
        TimePoint last_seen;
    };
    struct Stripe {
        std::mutex m;
        std::unordered_map<std::string, Entry> map;
    };
    Factory factory_;
    NowFn now_;
    const Duration idle_ttl_;
    std::array<Stripe, kStripes> stripes_;
};

int main() {
    using namespace std::chrono_literals;
    TimePoint t{};                 // fake time, advanced by hand
    auto now = [&t] { return t; };

    // 1. Token bucket: burst of 3, then 2 per second.
    {
        TokenBucket b(3, 2, t);
        assert(b.try_acquire(t) && b.try_acquire(t) && b.try_acquire(t));
        assert(!b.try_acquire(t));
        assert(b.try_acquire(t + 500ms));          // 1 token refilled
        assert(!b.try_acquire(t + 500ms));
        assert(b.try_acquire(t + 10s) && b.try_acquire(t + 10s) && b.try_acquire(t + 10s));
        assert(!b.try_acquire(t + 10s));           // capped at capacity, not 20 tokens
    }

    // 2. Fixed window: the boundary burst weakness, demonstrated.
    {
        FixedWindow w(2, 1s);
        const TimePoint edge = TimePoint{} + 10s;  // aligned window start
        assert(w.try_acquire(edge - 1ms) && w.try_acquire(edge - 1ms));
        assert(!w.try_acquire(edge - 1ms));
        assert(w.try_acquire(edge) && w.try_acquire(edge));   // 4 requests in 1 ms
        assert(!w.try_acquire(edge + 999ms));
    }

    // 3. Sliding log: exactly N in any trailing window.
    {
        SlidingWindowLog s(2, 1s);
        const TimePoint edge = TimePoint{} + 10s;
        assert(s.try_acquire(edge - 1ms) && s.try_acquire(edge - 1ms));
        assert(!s.try_acquire(edge));                    // boundary burst refused
        assert(!s.try_acquire(edge + 998ms));
        assert(s.try_acquire(edge + 999ms));             // first two slid out
    }

    // 4. Keyed limiter: independent keys, eviction of idle ones.
    {
        KeyedRateLimiter lim([](TimePoint n) { return std::make_unique<TokenBucket>(1, 1, n); },
                             now, 60s);
        assert(lim.allow("cust-1") && !lim.allow("cust-1"));
        assert(lim.allow("cust-2"));                     // a different customer is unaffected
        assert(lim.tracked_keys() == 2);
        t += 30s;
        assert(lim.allow("cust-2"));
        t += 31s;                                        // cust-1 idle 61 s, cust-2 idle 31 s
        assert(lim.evict_idle() == 1 && lim.tracked_keys() == 1);
    }

    // 5. Concurrency: 8 threads hammer one key; exactly `limit` succeed.
    {
        KeyedRateLimiter lim([](TimePoint) { return std::make_unique<SlidingWindowLog>(100, 1h); },
                             [] { return Clock::now(); }, 1h);
        std::atomic<int> ok{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] { for (int j = 0; j < 50; ++j) if (lim.allow("hot")) ++ok; });
        for (auto& th : ts) th.join();
        assert(ok == 100);
    }

    std::puts("m11_rate_limiter: all checks passed");
    return 0;
}
