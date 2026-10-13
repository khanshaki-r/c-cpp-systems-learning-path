#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tcpkv {

// Lock-free latency histogram with fixed log-scale buckets in microseconds.
// record() is a single relaxed fetch_add on one counter, so it is cheap enough to call per
// command from every worker thread. Percentiles are bucket upper bounds, i.e. "p99 <= X us".
class LatencyHistogram {
public:
    static constexpr std::size_t kBucketCount = 16;
    // Bucket i (i < 15) counts latencies in (bounds[i-1], bounds[i]] microseconds;
    // the last bucket counts everything above bounds[14].
    static constexpr std::array<std::uint64_t, kBucketCount - 1> kBoundsUs = {
        50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
        1000000, 5000000};

    struct Snapshot {
        std::uint64_t count = 0;
        std::uint64_t total_us = 0;
        std::array<std::uint64_t, kBucketCount> buckets{};

        double mean_us() const noexcept;
        // p in (0, 1], e.g. 0.99. Returns the upper bound of the bucket holding that rank.
        std::uint64_t percentile_us(double p) const noexcept;
        void merge(const Snapshot& other) noexcept;
    };

    LatencyHistogram() = default;
    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    void record(std::chrono::nanoseconds latency) noexcept;
    Snapshot snapshot() const noexcept;

private:
    std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> total_us_{0};
};

// All server counters live here. Plain atomics with relaxed ordering: each counter is
// independent and we never need one to be ordered relative to another.
struct Metrics {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_active{0};
    std::atomic<std::uint64_t> connections_rejected{0};  // "server busy": queue was full
    std::atomic<std::uint64_t> commands_total{0};
    std::atomic<std::uint64_t> commands_failed{0};  // parse errors
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> bytes_out{0};
    LatencyHistogram command_latency;  // execution time of the store operation, server side
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();

    // "name value\n" per line. This is what the STATS command returns.
    std::string render() const;
};

}  // namespace tcpkv
