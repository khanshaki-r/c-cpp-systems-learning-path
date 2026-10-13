#include "tcpkv/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tcpkv {

double LatencyHistogram::Snapshot::mean_us() const noexcept {
    return count == 0 ? 0.0 : static_cast<double>(total_us) / static_cast<double>(count);
}

std::uint64_t LatencyHistogram::Snapshot::percentile_us(double p) const noexcept {
    if (count == 0) {
        return 0;
    }
    const double clamped = std::min(std::max(p, 0.0), 1.0);
    const auto target = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::ceil(clamped * static_cast<double>(count))));
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        cumulative += buckets[i];
        if (cumulative >= target) {
            return i < kBoundsUs.size() ? kBoundsUs[i] : kBoundsUs.back();
        }
    }
    return kBoundsUs.back();
}

void LatencyHistogram::Snapshot::merge(const Snapshot& other) noexcept {
    count += other.count;
    total_us += other.total_us;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        buckets[i] += other.buckets[i];
    }
}

void LatencyHistogram::record(std::chrono::nanoseconds latency) noexcept {
    const auto signed_us = std::chrono::duration_cast<std::chrono::microseconds>(latency).count();
    const std::uint64_t us = signed_us < 0 ? 0 : static_cast<std::uint64_t>(signed_us);
    std::size_t index = 0;
    while (index < kBoundsUs.size() && us > kBoundsUs[index]) {
        ++index;
    }
    buckets_[index].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    total_us_.fetch_add(us, std::memory_order_relaxed);
}

LatencyHistogram::Snapshot LatencyHistogram::snapshot() const noexcept {
    // Not atomic as a whole: counters may move between loads. For monitoring that is fine.
    Snapshot s;
    s.count = count_.load(std::memory_order_relaxed);
    s.total_us = total_us_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        s.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
    }
    return s;
}

std::string Metrics::render() const {
    const auto uptime = std::chrono::steady_clock::now() - started_at;
    const auto latency = command_latency.snapshot();
    std::ostringstream os;
    os << "uptime_seconds " << std::chrono::duration_cast<std::chrono::seconds>(uptime).count()
       << '\n';
    os << "connections_accepted " << connections_accepted.load(std::memory_order_relaxed) << '\n';
    os << "connections_active " << connections_active.load(std::memory_order_relaxed) << '\n';
    os << "connections_rejected " << connections_rejected.load(std::memory_order_relaxed) << '\n';
    os << "commands_total " << commands_total.load(std::memory_order_relaxed) << '\n';
    os << "commands_failed " << commands_failed.load(std::memory_order_relaxed) << '\n';
    os << "bytes_in " << bytes_in.load(std::memory_order_relaxed) << '\n';
    os << "bytes_out " << bytes_out.load(std::memory_order_relaxed) << '\n';
    os << "latency_count " << latency.count << '\n';
    os << "latency_mean_us " << static_cast<std::uint64_t>(latency.mean_us()) << '\n';
    os << "latency_p50_us " << latency.percentile_us(0.50) << '\n';
    os << "latency_p99_us " << latency.percentile_us(0.99) << '\n';
    os << "latency_p999_us " << latency.percentile_us(0.999) << '\n';
    return os.str();
}

}  // namespace tcpkv
