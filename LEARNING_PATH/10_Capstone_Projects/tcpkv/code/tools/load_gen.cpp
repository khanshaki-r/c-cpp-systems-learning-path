// load_gen: concurrent load generator and correctness checker for tcpkv.
//
// Each client thread owns a disjoint key range ("lg:<run>:<client>:<n>") and remembers the
// last version it wrote to every key, so every GET can be verified exactly. The run id makes
// the key range unique per invocation, so leftovers from a previous run never cause false
// mismatches. Exit code is 1 if any error or mismatch was seen.
//
//   load_gen --port 7379 --clients 16 --duration 10 --keyspace 10000 --value-size 64 --read-ratio 0.8

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tcpkv/client.hpp"
#include "tcpkv/metrics.hpp"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;
    unsigned clients = 8;
    unsigned duration_s = 10;
    unsigned keyspace = 10000;
    unsigned value_size = 64;
    double read_ratio = 0.8;
};

struct WorkerResult {
    std::uint64_t gets = 0;
    std::uint64_t sets = 0;
    std::uint64_t errors = 0;
    std::uint64_t mismatches = 0;
    std::string fatal;  // message of the exception that ended the worker early, if any
    tcpkv::LatencyHistogram::Snapshot get_latency;
    tcpkv::LatencyHistogram::Snapshot set_latency;
};

std::string make_value(unsigned worker, unsigned key, std::uint32_t version, std::size_t size) {
    std::string value = std::to_string(worker) + ':' + std::to_string(key) + ':' +
                        std::to_string(version) + ':';
    if (value.size() < size) {
        value.append(size - value.size(), 'x');
    }
    return value;
}

void run_worker(unsigned id, std::uint64_t run_id, const Options& options,
                std::chrono::steady_clock::time_point deadline, WorkerResult& result) {
    using Clock = std::chrono::steady_clock;

    tcpkv::LatencyHistogram get_hist;
    tcpkv::LatencyHistogram set_hist;
    std::vector<std::uint32_t> versions(options.keyspace, 0);  // 0 = never written
    std::mt19937_64 rng(run_id ^ (static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL));
    std::uniform_int_distribution<unsigned> key_dist(0, options.keyspace - 1);
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);
    const std::string key_prefix = "lg:" + std::to_string(run_id) + ":" + std::to_string(id) + ":";

    try {
        tcpkv::Client client(options.host, options.port);
        while (Clock::now() < deadline) {
            const unsigned k = key_dist(rng);
            const std::string key = key_prefix + std::to_string(k);

            if (op_dist(rng) < options.read_ratio) {
                const auto t0 = Clock::now();
                const std::optional<std::string> value = client.get(key);
                get_hist.record(Clock::now() - t0);
                ++result.gets;
                if (versions[k] == 0) {
                    if (value.has_value()) {
                        ++result.mismatches;  // we never wrote it; nobody else may have
                    }
                } else if (!value.has_value() ||
                           *value != make_value(id, k, versions[k], options.value_size)) {
                    ++result.mismatches;
                }
            } else {
                const std::uint32_t version = versions[k] + 1;
                const std::string value = make_value(id, k, version, options.value_size);
                const auto t0 = Clock::now();
                client.set(key, value);
                set_hist.record(Clock::now() - t0);
                versions[k] = version;
                ++result.sets;
            }
        }
        client.quit();
    } catch (const std::exception& e) {
        ++result.errors;
        result.fatal = e.what();
    }
    result.get_latency = get_hist.snapshot();
    result.set_latency = set_hist.snapshot();
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool parse_double(std::string_view text, double& out) {
    try {
        std::size_t consumed = 0;
        const std::string copy(text);
        out = std::stod(copy, &consumed);
        return consumed == copy.size();
    } catch (const std::exception&) {
        return false;
    }
}

void print_usage(std::ostream& os) {
    os << "usage: load_gen [--host H] [--port P] [--clients N] [--duration S] [--keyspace K]\n"
          "                [--value-size B] [--read-ratio R]\n";
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return false;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << arg << '\n';
            return false;
        }
        const std::string_view value = argv[++i];
        bool ok = true;
        if (arg == "--host") {
            options.host = std::string(value);
        } else if (arg == "--port") {
            unsigned port = 0;
            ok = parse_number(value, port) && port <= 65535;
            options.port = static_cast<std::uint16_t>(port);
        } else if (arg == "--clients") {
            ok = parse_number(value, options.clients) && options.clients > 0;
        } else if (arg == "--duration") {
            ok = parse_number(value, options.duration_s) && options.duration_s > 0;
        } else if (arg == "--keyspace") {
            ok = parse_number(value, options.keyspace) && options.keyspace > 0;
        } else if (arg == "--value-size") {
            ok = parse_number(value, options.value_size) && options.value_size <= 1024 * 1024;
        } else if (arg == "--read-ratio") {
            ok = parse_double(value, options.read_ratio) && options.read_ratio >= 0.0 &&
                 options.read_ratio <= 1.0;
        } else {
            std::cerr << "unknown option " << arg << '\n';
            return false;
        }
        if (!ok) {
            std::cerr << "invalid value for " << arg << ": " << value << '\n';
            return false;
        }
    }
    return true;
}

void print_latency(const char* name, const tcpkv::LatencyHistogram::Snapshot& s) {
    std::cout << std::left << std::setw(6) << name << " n=" << std::setw(10) << s.count
              << " mean=" << std::setw(8) << std::fixed << std::setprecision(1) << s.mean_us()
              << " us  p50<=" << std::setw(7) << s.percentile_us(0.50) << " us  p99<=" << std::setw(7)
              << s.percentile_us(0.99) << " us  p99.9<=" << std::setw(7) << s.percentile_us(0.999)
              << " us\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage(std::cerr);
        return 2;
    }

    const std::uint64_t run_id =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        std::random_device{}();

    std::cout << "tcpkv load_gen: " << options.clients << " clients, " << options.duration_s
              << " s, keyspace " << options.keyspace << " per client, value " << options.value_size
              << " B, read ratio " << std::fixed << std::setprecision(2) << options.read_ratio
              << ", target " << options.host << ":" << options.port << '\n';

    std::vector<WorkerResult> results(options.clients);
    std::vector<std::thread> threads;
    threads.reserve(options.clients);

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(options.duration_s);
    for (unsigned i = 0; i < options.clients; ++i) {
        threads.emplace_back(run_worker, i, run_id, std::cref(options), deadline, std::ref(results[i]));
    }
    for (std::thread& t : threads) {
        t.join();
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    WorkerResult total;
    for (const WorkerResult& r : results) {
        total.gets += r.gets;
        total.sets += r.sets;
        total.errors += r.errors;
        total.mismatches += r.mismatches;
        total.get_latency.merge(r.get_latency);
        total.set_latency.merge(r.set_latency);
        if (!r.fatal.empty()) {
            std::cerr << "worker error: " << r.fatal << '\n';
        }
    }

    const std::uint64_t ops = total.gets + total.sets;
    std::cout << "total  " << ops << " ops in " << std::fixed << std::setprecision(2) << elapsed
              << " s  ->  " << std::setprecision(0) << (elapsed > 0 ? static_cast<double>(ops) / elapsed : 0.0)
              << " ops/s\n";
    print_latency("GET", total.get_latency);
    print_latency("SET", total.set_latency);
    std::cout << "errors " << total.errors << ", mismatches " << total.mismatches << '\n';

    return (total.errors == 0 && total.mismatches == 0) ? 0 : 1;
}
