// m09_token_bucket.cpp - thread-safe token-bucket rate limiter, a per-client keyed limiter, and
// a deterministic test plus a multi-threaded demo.
//
// Build:  g++ -std=c++17 -Wall -Wextra -Werror -pthread -O2 m09_token_bucket.cpp -o token_bucket
// Run:    ./token_bucket
//
// Token bucket: a bucket holds up to `capacity` tokens and refills at `rate` tokens per second.
// A request takes one token (or more for expensive requests). Empty bucket => reject (or wait).
// Capacity = the burst you allow; rate = the sustained average you allow. Two numbers, both
// meaningful to the people who set the policy: "100 requests per second, bursts up to 500".
#undef NDEBUG
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

class TokenBucket {
public:
    // `now` is injectable so tests can drive time deterministically.
    TokenBucket(double capacity, double rate_per_sec, std::function<TimePoint()> now = Clock::now)
        : capacity_(capacity), rate_(rate_per_sec), tokens_(capacity), now_(std::move(now)), last_(now_()) {}

    // Take `n` tokens if available. O(1), one mutex, no background thread: tokens are computed
    // lazily from elapsed time on each call ("lazy refill"), which is both cheaper and exact.
    bool try_acquire(double n = 1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        if (tokens_ >= n) {
            tokens_ -= n;
            return true;
        }
        return false;
    }

    // How long until `n` tokens will be available (0 if now). Lets a caller sleep instead of
    // spinning, or lets a server fill in a Retry-After header.
    std::chrono::microseconds time_until(double n = 1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        if (tokens_ >= n) return std::chrono::microseconds(0);
        double missing = n - tokens_;
        return std::chrono::microseconds(static_cast<long long>(std::ceil(missing / rate_ * 1e6)));
    }

    double tokens() {
        std::lock_guard<std::mutex> lk(mu_);
        refill();
        return tokens_;
    }

private:
    void refill() {
        TimePoint t = now_();
        double elapsed = std::chrono::duration<double>(t - last_).count();
        if (elapsed > 0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
            last_ = t;
        }
    }

    double capacity_;
    double rate_;
    double tokens_;
    std::function<TimePoint()> now_;
    TimePoint last_;
    std::mutex mu_;
};

// One bucket per key (client id, branch code, IP). Buckets idle for a long time are evicted so
// the map does not grow forever. In production the map is sharded or the state lives in Redis.
class KeyedLimiter {
public:
    KeyedLimiter(double capacity, double rate, std::chrono::seconds idle_evict)
        : capacity_(capacity), rate_(rate), idle_evict_(idle_evict) {}

    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto now = Clock::now();
        // try_emplace constructs the Entry in place: TokenBucket owns a mutex and cannot be moved.
        auto it = buckets_.try_emplace(key, capacity_, rate_, now).first;
        it->second.last_seen = now;
        maybe_evict(now);
        return it->second.bucket.try_acquire();
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return buckets_.size();
    }

private:
    struct Entry {
        TokenBucket bucket;
        TimePoint last_seen;
        Entry(double cap, double rate, TimePoint t) : bucket(cap, rate), last_seen(t) {}
    };
    void maybe_evict(TimePoint now) {
        if (now - last_sweep_ < idle_evict_) return;
        last_sweep_ = now;
        for (auto it = buckets_.begin(); it != buckets_.end();) {
            if (now - it->second.last_seen > idle_evict_) it = buckets_.erase(it);
            else ++it;
        }
    }

    double capacity_, rate_;
    std::chrono::seconds idle_evict_;
    std::mutex mu_;
    std::unordered_map<std::string, Entry> buckets_;
    TimePoint last_sweep_ = Clock::now();
};

// ----- deterministic test with a fake clock ---------------------------------------------------
static void test_with_fake_clock() {
    TimePoint fake = Clock::now();
    auto now = [&fake] { return fake; };
    TokenBucket b(/*capacity=*/10, /*rate=*/5, now);

    // A full bucket allows a burst of exactly `capacity`.
    int ok = 0;
    for (int i = 0; i < 20; ++i) ok += b.try_acquire() ? 1 : 0;
    assert(ok == 10);
    assert(!b.try_acquire());

    // After 1 s at 5/s, 5 tokens are back.
    fake += std::chrono::seconds(1);
    ok = 0;
    for (int i = 0; i < 20; ++i) ok += b.try_acquire() ? 1 : 0;
    assert(ok == 5);

    // Empty: next token in 200 ms (1/5 s).
    auto wait = b.time_until();
    assert(wait.count() >= 200000 && wait.count() <= 200001); // ceil() of a floating-point product

    // Refill never exceeds capacity, no matter how long we idle.
    fake += std::chrono::hours(1);
    assert(std::fabs(b.tokens() - 10.0) < 1e-9);

    // Expensive request costing 4 tokens.
    assert(b.try_acquire(4));
    assert(std::fabs(b.tokens() - 6.0) < 1e-9);
    std::puts("fake-clock test passed");
}

// ----- multi-threaded demo with the real clock -------------------------------------------------
static void demo_concurrent() {
    const double capacity = 50, rate = 100; // burst 50, then 100/s sustained
    TokenBucket b(capacity, rate);
    std::atomic<long> accepted{0}, rejected{0};
    const auto run_for = std::chrono::milliseconds(1000);

    auto worker = [&] {
        auto end = Clock::now() + run_for;
        while (Clock::now() < end) {
            if (b.try_acquire()) ++accepted;
            else ++rejected;
            std::this_thread::sleep_for(std::chrono::microseconds(200)); // ~5000 attempts/s/thread
        }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();

    // Expect about capacity + rate * 1 s = 150 accepted, regardless of how many threads hammer it.
    std::printf("8 threads, 1 s, burst %.0f + %.0f/s: accepted=%ld rejected=%ld (expected ~150)\n", capacity, rate,
                accepted.load(), rejected.load());
    assert(accepted >= 140 && accepted <= 165); // generous bounds for scheduler jitter
}

static void demo_keyed() {
    KeyedLimiter lim(/*capacity=*/3, /*rate=*/1, std::chrono::seconds(60));
    int a = 0, c = 0;
    for (int i = 0; i < 5; ++i) {
        a += lim.allow("branch-A") ? 1 : 0;
        c += lim.allow("branch-C") ? 1 : 0;
    }
    assert(a == 3 && c == 3); // each key has its own bucket
    assert(lim.size() == 2);
    std::puts("keyed limiter test passed");
}

int main() {
    test_with_fake_clock();
    demo_keyed();
    demo_concurrent();
    std::puts("token bucket: all passed");
    return 0;
}
