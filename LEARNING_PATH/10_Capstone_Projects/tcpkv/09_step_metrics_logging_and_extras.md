# Step 09. Metrics, logging, `STATS`, TTL, and `KEYS` revisited

**Time:** ~7 hours   **Week:** 5 (Nov 03 – Nov 09)
**Read first:** 04_Concurrency: `07_atomics_and_the_memory_model.md` (`memory_order_relaxed`
and when it is enough), `10_cache_lines_false_sharing_performance.md`,
`04_shared_mutex_and_readers_writers.md` (writer starvation, long readers).
02_Modern_CPP: `13_cpp17_features_tour.md` (`std::optional`, structured bindings),
`03_raii_the_core_idea.md` (RAII for things that are not memory: a counter guard).
07_Databases_Internals: `13_caching_and_redis.md` (TTL, lazy and active expiry, why `KEYS` is
banned in production Redis). 05_Memory_Debugging_RCA: `08_valgrind_and_sanitizers.md` (UBSan).
06_Linux_Toolchain_DevOps: `07_debugging_and_profiling_tools_on_linux.md`.

## 1. Goal

Make the server observable and finish the protocol.

* `Metrics`: atomic counters for connections, commands, bytes, and a latency histogram of
  command execution time.
* `STATS` returns them; the acceptor logs one throughput line every 10 seconds
  (`--stats-interval`).
* `EXPIRE` and `TTL`: per-key time-to-live with lazy expiry on read plus a once-a-second purge.
* `connections_active` that is correct on every exit path, via a small RAII guard.
* Revisit the two pieces that already exist: the logger (step 05) and `KEYS prefix`
  (steps 04 and 05), this time with their production costs.
* The `-ERR not implemented yet` branch disappears. The protocol in `00_spec.md` is now
  fully implemented.

In plain words: a server you cannot see into is a server you cannot run. At 3 AM the first
three questions are "is it up, how busy is it, how slow is it", and `STATS` plus one log line
every ten seconds answer all three without a debugger. TTL is the feature that turns a map
into a cache: "keep this for 60 seconds, then forget it".

## 2. Design

### 2.1 Metrics: relaxed atomics, no locks

```
worker threads (N)                      acceptor thread              any client
  ++commands_total  (relaxed)             every 10 s: read           STATS: render()
  ++bytes_in / bytes_out                  commands_total, diff,        reads every counter
  histogram.record(latency)               log ops/s and p99            (relaxed loads)
  connections_active +1 / -1 (guard)
```

* **Every counter is independent.** Nobody needs `commands_total` ordered relative to
  `bytes_out`, and no other data is published through a counter. So `memory_order_relaxed` is
  correct: each `fetch_add` is still atomic (no lost increments), it just does not act as a
  fence. On x86-64, a relaxed `fetch_add` and a seq_cst one compile to the same `lock xadd`;
  the difference is what the compiler may reorder around it, and on ARM it is a real
  difference in barriers.
* **No mutex anywhere in `Metrics`.** That rules out a whole bug class: no lock-order
  inversion with the store lock, no contention between `STATS` and workers.
* **A `STATS` snapshot is not atomic as a whole.** Counters may move between two loads, so
  `connections_active` and `commands_total` in one reply are not from one instant. For
  monitoring that is fine; say so rather than pretending otherwise.
* **False sharing, honestly.** All counters sit next to each other in one struct, so every
  worker's `fetch_add` pulls the same 64-byte cache line into its core in exclusive state.
  With 4 to 16 workers doing one network round trip per command, the syscalls dominate and
  this does not show up in a profile. If it ever did, the fixes are `alignas(64)` per hot
  counter, or per-thread counters summed on read (what the Linux kernel does with per-CPU
  counters). Measure first (see 04_Concurrency/10_cache_lines_false_sharing_performance.md).

### 2.2 The latency histogram

Sixteen buckets on a roughly logarithmic scale, upper bounds in microseconds:

```
<=50 <=100 <=200 <=500 <=1ms <=2ms <=5ms <=10ms <=20ms <=50ms <=100ms <=200ms <=500ms <=1s <=5s  >5s
```

* `record()` finds the bucket with a linear scan of 15 constants (cheaper than it sounds:
  the array is 120 bytes, one or two cache lines) and does three relaxed `fetch_add`s.
* `percentile_us(p)` walks the cumulative counts to the bucket holding rank `ceil(p * count)`
  and returns its **upper bound**. So "p99 = 100 us" means "99% of commands took at most
  100 us". You cannot report finer than the bucket, and you should not pretend to.
* Why not store every sample and sort? Memory grows without bound, and a sort under load
  is not free. HdrHistogram (used by many load tools) is the production-grade version of
  this idea with configurable precision; ours is the 60-line version.
* **What is measured:** server-side execution of the store operation only, from just before
  `execute_command` to just after. It excludes parsing, the socket write and network time.
  Small numbers here and large numbers in `load_gen` mean the time is in the network, the
  kernel or the queue for a worker, not in the store. That split is the point of measuring
  both.

### 2.3 TTL: lazy expiry plus a periodic purge

```cpp
struct Entry {
    std::string value;
    std::optional<Clock::time_point> expires_at;   // nullopt = lives forever
};
```

* **`steady_clock`, not `system_clock`.** A TTL is an interval. If NTP moves the wall clock
  back an hour, `system_clock` would make every key live an hour longer.
* **Lazy expiry:** every reader checks `is_expired(entry, now)` and reports "not found".
  Readers hold only a *shared* lock, so they cannot erase; they just do not see the entry.
* **Active expiry:** the acceptor calls `store.purge_expired()` once a second. It takes the
  exclusive lock and erases every expired entry. Without it, keys that are never read again
  stay in memory forever.
* **`SET` clears the TTL** (like Redis). `EXPIRE k 0` or negative deletes at once.
  `DEL` on an expired-but-unpurged key erases it and returns 0 (it was not live).
* **`TTL` rounds up** (`std::chrono::ceil`), so a key with 0.3 s left reports 1, not 0.
  0 would read as "expires now" and is easy to confuse with "no TTL".
* **Clock read outside the lock.** `Clock::now()` is a vDSO call, cheap but not free; doing
  it before taking the lock keeps the critical section to the hash lookup.

The trade-off to be able to state: `purge_expired()` is O(total keys) under the exclusive
lock, once a second. With a million keys that is a pause of the order of milliseconds during
which no writer or reader runs (measure it on your machine before quoting a number). Two
better designs:

1. **Sampling (what Redis does):** keep a separate set of keys that have a TTL; each cycle,
   test a small random sample and erase the expired ones; repeat if a large fraction was
   expired. Bounded work per cycle, probabilistic memory bound.
2. **An expiry index:** a min-heap or `std::multimap<time_point, key>`; purge pops from the
   front while `front <= now`. O(expired * log n) per cycle, at the cost of extra memory and
   of keeping the index in sync on `SET`/`DEL`/`EXPIRE`.

v1.0.0 uses the simple full scan because it is obviously correct and the key counts in the
benchmarks are small. Say exactly that.

### 2.4 `KEYS prefix`, revisited

`KEYS` has been there since step 04 (store) and step 05 (command): collect matching keys
under a shared lock, release, sort. Two production problems:

* **It is O(n) in the total number of keys, not in the number of matches.** An
  `unordered_map` has no order, so a prefix query must look at every key.
* **It holds the shared lock for the whole scan.** Other readers are unaffected, but every
  writer waits for the full scan. And libstdc++'s `std::shared_mutex` on Linux wraps
  `pthread_rwlock_t` with glibc's default *reader-preferring* policy
  (04_Concurrency/04_shared_mutex_and_readers_writers.md): a steady stream of `GET`s and
  `KEYS` overlapping each other can keep the reader count above zero indefinitely, and a
  `SET` waits until there is a gap. Long readers plus reader preference is how writer
  starvation happens.

This is why Redis documentation warns against `KEYS` in production and provides `SCAN`. If
asked how you would fix it, in order of effort:

1. Cap the reply (`KEYS prefix LIMIT n`) and document it. Cheap, honest.
2. `SCAN cursor [MATCH prefix] [COUNT n]`: return up to n keys and a cursor; the client loops.
   With an `unordered_map` the cursor can be a bucket index (iterate `bucket(i)`), which is
   stable across calls *unless a rehash happens*; Redis solves that with reverse-binary
   cursor iteration. Each call holds the lock for a bounded time.
3. A second, ordered index (`std::map<std::string, ...>` or a trie) so a prefix query is
   `lower_bound(prefix)` then walk while the prefix matches: O(log n + matches). Costs memory
   and a second structure to update on every write.

### 2.5 Logging, revisited

The logger arrived in step 05. What matters now that the server is concurrent and long-lived:

* **One line, one `fwrite`, under one mutex.** `write()` formats the whole line first, then
  locks only for the `fwrite` + `fflush`. Lines from different threads never interleave, and
  the critical section does no formatting.
* **The level check happens before formatting.** `log::debug(...)` with the level at
  `info` costs one relaxed atomic load and a compare; the arguments are never streamed.
  That is why per-connection events are at `debug`: a busy server would otherwise spend
  measurable time formatting and writing thousands of lines a second.
* **`localtime_r`, not `localtime`.** `localtime` returns a pointer to a static buffer shared
  by all threads: a data race TSan would report.
* **`gettid()` in the line**: the kernel thread id, the same number `top -H`, `ps -L` and gdb's
  `info threads` show. When a worker is stuck, the log tells you which LWP to attach to.
* **stderr, not a file.** The process does not manage log files, rotation or disk-full. Under
  systemd, stderr goes to the journal; in a container, to the container runtime's log driver.
  This is the twelve-factor convention, and it is what you want in production.
* **Not async-signal-safe.** It allocates and locks. Never call it from a signal handler
  (step 08, item 6).

## 3. Files to create or change

### `include/tcpkv/metrics.hpp` (final version)

Step 08 created the file with `LatencyHistogram`. Add `#include <string>` and the `Metrics`
struct below the histogram:

```cpp
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
```

`Metrics` is not copyable (atomics and the histogram are not), which is right: there is one
set of counters per server, passed by reference.

### `src/metrics.cpp` (final version)

```cpp
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
```

Details worth a sentence each:

* `std::max<std::uint64_t>(1, ...)` makes `percentile_us(0.0)` mean "the smallest sample"
  rather than rank 0, which does not exist.
* `duration_cast<microseconds>` truncates: a 49.9 us command lands in the `<=50` bucket.
  Negative durations (impossible with `steady_clock`, but cheap to guard) clamp to 0.
* `total_us` can in theory overflow `uint64_t` (max about 1.8 x 10^19). At one million
  commands per second averaging 100 us, it grows by 10^8 per second and wraps after about
  5,800 years. Unsigned overflow is defined (it wraps), so even then the mean is wrong, not
  undefined. Do this arithmetic out loud if asked; it is the kind of check seniors make.
* `render()` uses `ostringstream` because `STATS` is rare and readability wins. Format is
  Prometheus-like (`name value`) so a scraper could be written in ten lines.

### `include/tcpkv/store.hpp` and `src/store.cpp` (final versions)

Identical to `code/include/tcpkv/store.hpp` and `code/src/store.cpp`. Compared with step 04:
the value type becomes `Entry`, three methods are added, and every reader checks expiry.

```cpp
    // ttl <= 0 deletes the key. Returns false if the key does not exist.
    // Takes Clock::duration (not seconds) so tests can use short TTLs; std::chrono::seconds
    // converts implicitly.
    bool expire(const std::string& key, Clock::duration ttl);

    // -2: key does not exist, -1: exists without expiry, otherwise seconds left (rounded up).
    long long ttl(const std::string& key) const;

    std::size_t size() const;  // includes expired-but-not-yet-purged entries

    // Erases expired entries; returns how many were removed.
    std::size_t purge_expired();

private:
    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expires_at;
    };

    static bool is_expired(const Entry& entry, Clock::time_point now) noexcept {
        return entry.expires_at.has_value() && *entry.expires_at <= now;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
```

The new and changed methods:

```cpp
std::optional<std::string> Store::get(const std::string& key) const {
    const auto now = Clock::now();  // read the clock outside the lock: it is not free
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return std::nullopt;
    }
    return it->second.value;  // copy made under the lock; the caller owns it afterwards
}

void Store::set(std::string key, std::string value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Entry& entry = map_[std::move(key)];
    entry.value = std::move(value);
    entry.expires_at.reset();  // like Redis: SET makes the key persistent again
}

bool Store::del(const std::string& key) {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return false;
    }
    const bool was_live = !is_expired(it->second, now);
    map_.erase(it);
    return was_live;
}

bool Store::expire(const std::string& key, Clock::duration ttl) {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return false;
    }
    if (ttl.count() <= 0) {
        map_.erase(it);
        return true;
    }
    it->second.expires_at = now + ttl;
    return true;
}

long long Store::ttl(const std::string& key) const {
    const auto now = Clock::now();
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end() || is_expired(it->second, now)) {
        return -2;
    }
    if (!it->second.expires_at) {
        return -1;
    }
    const auto remaining = *it->second.expires_at - now;
    return std::chrono::ceil<std::chrono::seconds>(remaining).count();
}

std::size_t Store::purge_expired() {
    const auto now = Clock::now();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto it = map_.begin(); it != map_.end();) {
        if (is_expired(it->second, now)) {
            it = map_.erase(it);  // erase() returns the next valid iterator
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}
```

`exists` and `keys` gain the same `is_expired` check; in `keys` the structured binding
becomes `[key, entry]` and the step-04 `(void)value;` goes away. `size()` deliberately counts
expired-but-unpurged entries: it reports memory, not logical contents, and the tests rely on
that to observe the purge.

### `include/tcpkv/connection.hpp` and `src/connection.cpp` (final versions)

Identical to the files in `code/`. `execute_command` and `Connection` take a `Metrics&`:

```cpp
std::string execute_command(Command command, Store& store, Metrics& metrics);

    Connection(Socket socket, Store& store, Metrics& metrics, const std::atomic<bool>& stop_flag,
               ConnectionConfig config) noexcept;
    ...
    Metrics* metrics_;
```

The three branches that replace `-ERR not implemented yet`:

```cpp
        case CommandType::Expire: {
            const auto ttl = std::chrono::seconds(parse_seconds(command.args[1]));
            return reply::integer(store.expire(command.args[0], ttl) ? 1 : 0);
        }

        case CommandType::Ttl:
            return reply::integer(store.ttl(command.args[0]));

        case CommandType::Stats:
            return reply::bulk(metrics.render());
```

The active-connection guard, in the anonymous namespace:

```cpp
// Increments connections_active for the lifetime of serve(), whatever path leaves it.
class ActiveConnectionGuard {
public:
    explicit ActiveConnectionGuard(Metrics& metrics) noexcept : metrics_(metrics) {
        metrics_.connections_active.fetch_add(1, std::memory_order_relaxed);
    }
    ~ActiveConnectionGuard() { metrics_.connections_active.fetch_sub(1, std::memory_order_relaxed); }
    ActiveConnectionGuard(const ActiveConnectionGuard&) = delete;
    ActiveConnectionGuard& operator=(const ActiveConnectionGuard&) = delete;

private:
    Metrics& metrics_;
};
```

`serve()` starts with `const ActiveConnectionGuard active_guard(*metrics_);`. `serve()` has
five ways out (EOF, `QUIT`, fatal parse error, idle timeout, stop flag) plus any exception.
A manual `--active` at the end would be wrong on at least one of them. This is RAII for a
counter instead of memory, and the `ActiveConnectionsDropToZeroAfterClientsLeave` test proves
it. In C you would write `goto out;` and hope every path reaches it.

`serve()` also adds `bytes_in` after each read and `bytes_out` after each write.
`process_events` counts parse errors and times each command:

```cpp
        Command& command = std::get<Command>(*event);
        const bool quit = command.type == CommandType::Quit;
        const auto start = Clock::now();
        out += execute_command(std::move(command), *store_, *metrics_);
        metrics_->command_latency.record(Clock::now() - start);
        metrics_->commands_total.fetch_add(1, std::memory_order_relaxed);
```

Note `quit` is read *before* `std::move(command)`. Reading a moved-from object's members is
legal but its value is unspecified for the moved parts; reading first removes the question.

### `include/tcpkv/server.hpp` and `src/server.cpp` (final versions)

Identical to the files in `code/`. `ServerConfig` gains:

```cpp
    // Log a one-line throughput summary this often. 0 disables.
    std::chrono::seconds stats_log_interval{10};
```

and the constructor becomes `Server(ServerConfig config, Store& store, Metrics& metrics)`.
`accept_one` increments `connections_accepted` after a successful accept and
`connections_rejected` on the busy path, and passes `metrics_` to the `Connection`. The run
loop gains periodic maintenance after the accept:

```cpp
        const auto now = Clock::now();
        if (now - last_purge >= std::chrono::seconds(1)) {
            const std::size_t purged = store_.purge_expired();
            if (purged > 0) {
                log::debug("purged ", purged, " expired keys");
            }
            last_purge = now;
        }
        if (config_.stats_log_interval.count() > 0 && now - last_stats >= config_.stats_log_interval) {
            log_throughput(now, last_stats, last_commands);
        }
```

The acceptor's `poll` has a 1000 ms timeout, so the loop runs at least once a second even
with no new connections; that is what makes it a usable timer without a timer thread. Under
a flood of connections it runs more often, and the `>=` checks keep the cadence right.

```cpp
void Server::log_throughput(std::chrono::steady_clock::time_point now,
                            std::chrono::steady_clock::time_point& last_time,
                            std::uint64_t& last_commands) const {
    const std::uint64_t commands = metrics_.commands_total.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    const std::uint64_t delta = commands - last_commands;
    const auto rate = seconds > 0 ? static_cast<std::uint64_t>(static_cast<double>(delta) / seconds) : 0;
    log::info("stats: ", delta, " commands in last ", static_cast<std::uint64_t>(seconds), " s (",
              rate, " ops/s), active connections ",
              metrics_.connections_active.load(std::memory_order_relaxed), ", p99 ",
              metrics_.command_latency.snapshot().percentile_us(0.99), " us");
    last_time = now;
    last_commands = commands;
}
```

The rate is computed from deltas, so it is "ops/s over the last interval", not since start.
The p99 *is* since start (the histogram is cumulative). Know that difference if asked; a
windowed p99 would need a histogram per interval, swapped and reset by the acceptor.

The shutdown log line now reports active connections as well as queued ones.

### `apps/tcpkv_server.cpp` (final version)

Identical to `code/apps/tcpkv_server.cpp`. Compared with step 06: a `tcpkv::Metrics metrics;`
next to the `Store`, `Server server(options.server, store, metrics);`, the
`--stats-interval <s>` option, and the final log line includes the command count:

```cpp
        tcpkv::log::info("tcpkv stopped cleanly; ", metrics.commands_total.load(), " commands served");
```

Declaration order in `main` matters: `store` and `metrics` are declared before `server`, so
they are destroyed after it (reverse order), and every worker that touches them has been
joined by then. This is spec section 4.3 in three lines.

### `tools/valgrind_check.sh`

Add `--stats-interval 0` to the server's command line, as in `code/`. With Valgrind slowing
the server, the 10-second line would otherwise appear mid-run and clutter the CI log.

### `tests/store_test.cpp`

Add the TTL tests from `code/tests/store_test.cpp`: `ExpireOnMissingKeyReturnsFalse`,
`TtlSemantics`, `SetClearsTtl`, `ExpireWithZeroOrNegativeDeletesImmediately`,
`ExpiredKeyIsInvisibleUntilPurged`, `DelOnExpiredKeyReturnsFalseAndRemovesIt`,
`SetAfterExpiryRevivesKey`, and the stress test `StoreStress.SetDeleteAndExpireRace`. The one
that pins down lazy expiry:

```cpp
TEST(Store, ExpiredKeyIsInvisibleUntilPurged) {
    Store store;
    store.set("k", "v");
    ASSERT_TRUE(store.expire("k", std::chrono::milliseconds(20)));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Logically gone...
    EXPECT_FALSE(store.get("k").has_value());
    EXPECT_FALSE(store.exists("k"));
    EXPECT_TRUE(store.keys().empty());
    EXPECT_EQ(store.ttl("k"), -2);
    EXPECT_FALSE(store.expire("k", std::chrono::seconds(5)));
    // ...but physically still there until purge runs.
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.purge_expired(), 1u);
    EXPECT_EQ(store.size(), 0u);
}
```

This is why `expire` takes `Clock::duration` and not `seconds`: a 20 ms TTL keeps the test
fast. The sleeps are three times the TTL so a slow CI runner does not make it flaky.

### `tests/integration_test.cpp` (final version)

Identical to `code/tests/integration_test.cpp`. Changes from step 07:

* `#include "tcpkv/metrics.hpp"`, a `Metrics metrics_;` member in the fixture,
  `config.stats_log_interval = std::chrono::seconds(0);` and
  `std::make_unique<Server>(config, store_, metrics_)`. The two `ServerShutdown` tests get
  their own `Metrics` the same way.
* Delete `NotYetImplementedCommandsReplyWithError`.
* `ManyConcurrentClientsSeeTheirOwnWrites` also asserts
  `metrics_.connections_accepted == kClients`.
* Three new tests:

```cpp
TEST_F(ServerFixture, ExpireAndTtl) {
    Client client = connect();
    client.set("tmp", "v");
    EXPECT_TRUE(client.expire("tmp", 1));
    EXPECT_EQ(client.ttl("tmp"), 1);
    EXPECT_EQ(client.ttl("missing"), -2);
    client.set("forever", "v");
    EXPECT_EQ(client.ttl("forever"), -1);
    EXPECT_FALSE(client.expire("missing", 5));

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_FALSE(client.get("tmp").has_value());
    EXPECT_EQ(client.ttl("tmp"), -2);
}

TEST_F(ServerFixture, StatsReportsCounters) {
    Client client = connect();
    client.ping();
    client.set("k", "v");
    const std::string stats = client.stats();
    EXPECT_NE(stats.find("commands_total"), std::string::npos);
    EXPECT_NE(stats.find("connections_active 1\n"), std::string::npos);
    EXPECT_GE(metrics_.commands_total.load(), 3u);
    EXPECT_EQ(metrics_.connections_accepted.load(), 1u);
}

TEST_F(ServerFixture, ActiveConnectionsDropToZeroAfterClientsLeave) {
    {
        Client a = connect();
        Client b = connect();
        a.ping();
        b.ping();
        EXPECT_EQ(metrics_.connections_active.load(), 2u);
    }
    // Workers notice EOF on their next poll (<= 20 ms); give them a moment.
    for (int i = 0; i < 100 && metrics_.connections_active.load() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(metrics_.connections_active.load(), 0u);
}
```

`EXPECT_GE(commands_total, 3u)`, not `EQ`: `STATS` itself is counted *after* its reply is
rendered, so the counter may read 2 inside the reply and 3 afterwards. Asserting the exact
value would test a timing detail, not behaviour. The last test polls rather than sleeping a
fixed time: it passes in 20 ms normally and still passes on a slow TSan runner.

### A real UBSan finding: `EXPIRE` with a huge number of seconds

The parser accepts any `long long` for `seconds`. `execute_command` builds
`std::chrono::seconds(n)`, and `Store::expire` takes `Clock::duration`, which on Linux is
nanoseconds in a signed 64-bit integer. The implicit conversion multiplies by 10^9. Any `n`
above about 9.2 x 10^9 (roughly 292 years) overflows: signed overflow, undefined behaviour,
triggered by one line from any client. A minimal reproduction of the same conversion, built
with `-fsanitize=undefined` on GCC 13 in WSL, reports:

```
/usr/include/c++/13/bits/chrono.h:225:38: runtime error: signed integer overflow:
10000000000 * 1000000000 cannot be represented in type 'long long int'
    #0 ... std::chrono::__duration_cast_impl<...>::__cast<long, std::ratio<1l, 1l> >(...)
    #1 ... std::chrono::duration_cast<std::chrono::duration<long, std::ratio<1l, 1000000000l> >, ...>
    #2 ... std::chrono::duration<long, std::ratio<1l, 1000000000l> >::duration<long, std::ratio<1l, 1l>, void>(...)
```

Reproduce it against the server: run `./build/ubsan/tcpkv_server --port 7400`, then
`./build/ubsan/tcpkv_cli -p 7400 set k v` and `./build/ubsan/tcpkv_cli -p 7400 expire k 10000000000`.
Negative values overflow the same way (`expire k -10000000000`), *before* the `ttl <= 0`
check can run, because the conversion happens at the call.

None of the tests send such a value, so every CI job is green with this bug present. That is
the lesson: sanitizers only judge the inputs you give them. If the parser in your copy of
`code/` does not already reject these values, fix it in your repo:

```cpp
// include/tcpkv/protocol.hpp, in namespace limits
// 100 years. Converted to steady_clock's nanoseconds this is ~3.2e18, below INT64_MAX (~9.2e18),
// and adding it to steady_clock::now() (time since boot) cannot overflow either.
inline constexpr long long kMaxExpireSeconds = 100LL * 365 * 24 * 3600;
```

```cpp
// src/protocol.cpp, in the CommandType::Expire validation
            long long seconds = 0;
            if (!parse_signed(cmd.args[1], seconds) || seconds > limits::kMaxExpireSeconds ||
                seconds < -limits::kMaxExpireSeconds) {
                return fail("invalid expire seconds", false);
            }
```

```cpp
// tests/protocol_test.cpp
// Uses the next_error / next_command helpers already at the top of protocol_test.cpp.
TEST(Parser, ExpireSecondsOutOfRangeIsNonFatal) {
    Parser parser;
    parser.feed("EXPIRE k 10000000000\nEXPIRE k -10000000000\nEXPIRE k 3153600000\nPING\n");
    for (int i = 0; i < 2; ++i) {
        const auto error = next_error(parser);
        ASSERT_TRUE(error.has_value());
        EXPECT_EQ(error->message, "invalid expire seconds");
        EXPECT_FALSE(error->fatal);
    }
    const auto at_limit = next_command(parser);  // exactly 100 years: accepted
    ASSERT_TRUE(at_limit.has_value());
    EXPECT_EQ(at_limit->type, CommandType::Expire);
    const auto ping = next_command(parser);  // the stream is still in sync
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->type, CommandType::Ping);
}
```

Add a row for the limit to `docs/PROTOCOL.md` and the limits table in your README. Rejecting is better than
clamping: a client that sends 10^10 seconds has a bug, and an error tells it so.

Keep this story. "Our sanitizer jobs were green and the server still had UB on one input;
here is how I found it and the test that pins it" is a better interview answer than "it is
sanitizer-clean".

### `CMakeLists.txt`

No new files (`src/metrics.cpp` was added in step 08). Bump nothing yet; the version becomes
1.0.0 in step 10.

## 4. Build and run (WSL Ubuntu 24.04)

```bash
cd ~/dev/tcpkv
cmake --build --preset debug && ctest --preset debug
cmake --build --preset asan  && ctest --preset asan
setarch $(uname -m) -R cmake --build --preset tsan && setarch $(uname -m) -R ctest --preset tsan
cmake --build --preset ubsan && ctest --preset ubsan
cmake --build --preset relwithdebinfo && bash tools/valgrind_check.sh build/relwithdebinfo

./build/release/tcpkv_server --port 7379 --threads 8 --stats-interval 2
```

(Build `release` first if you have not: `cmake --preset release && cmake --build --preset release`.)

## 5. Test by hand

1. TTL from the CLI:

   ```
   tcpkv> set session abc
   OK
   tcpkv> expire session 5
   (integer) 1
   tcpkv> ttl session
   (integer) 5
   tcpkv> ttl nope
   (integer) -2
   ```

   Wait six seconds: `get session` prints `(nil)`, `ttl session` prints `(integer) -2`.
2. `SET` clears TTL: `expire k 100`, `set k again`, `ttl k` prints `(integer) -1`.
3. Purge: start the server with `--log-level debug`, run
   `for i in $(seq 1 1000); do echo "SET t$i 1"; echo x; echo "EXPIRE t$i 1"; done | nc -q1 127.0.0.1 7379 >/dev/null`.
   Within two seconds the log shows `purged 1000 expired keys` (possibly split across two
   purge cycles).
4. Throughput log: with `--stats-interval 2`, run
   `./build/release/load_gen --port 7379 --clients 8 --duration 10`. Every two seconds:
   `stats: <n> commands in last 2 s (<rate> ops/s), active connections 8, p99 <x> us`.
   (`connections_active` counts connections being *served*, inside `serve()`. Start the
   server with fewer threads than clients and it reports the thread count, not the client
   count: the rest are queued, not active.)
   After `load_gen` ends: `active connections 0`.
5. `./build/release/tcpkv_cli stats` prints the counters as one quoted bulk string with `\n`
   escapes. `printf 'STATS\n' | nc -q1 127.0.0.1 7379` shows the raw lines. Run it before and
   after a `load_gen --clients 8` run: `connections_accepted` grows by the 8 load clients
   plus one for each of your own `STATS` connections.
6. Busy rejection is counted: restart with `--threads 1 --max-pending 1`. Open three `nc`
   sessions in three terminals. The first is served, the second waits in the queue, the third
   gets `-ERR server busy, try again later` and is closed. Type `QUIT` in the first; the
   second is now served; type `STATS` in it: `connections_rejected 1`. (Note that
   `--max-pending 0` would reject *every* connection, because the check is
   `pending >= max_pending`; that is a sharp edge worth a sentence in `--help`.)
7. The UBSan finding above: reproduce it, fix it, see the new parser test pass and the
   `expire k 10000000000` command answer `(error) invalid expire seconds`.

## 6. Commit

Two commits, because they are two changes and a reviewer should be able to read each:

```bash
git add -A
git commit -m "Add Metrics, STATS, TTL and periodic maintenance

- Metrics: relaxed atomic counters (connections, commands, bytes) and a lock-free latency
  histogram of command execution time; STATS renders them as 'name value' lines
- Store: per-key expiry on steady_clock; lazy expiry on read, purge_expired() once a second
  from the acceptor; SET clears TTL; EXPIRE <= 0 deletes; TTL rounds up
- Connection: ActiveConnectionGuard keeps connections_active right on every exit path;
  bytes in/out and per-command latency recorded
- Server: accepted/rejected counters; throughput log line every --stats-interval seconds
- tests: TTL semantics and expire/purge race; integration tests for EXPIRE/TTL, STATS and
  active-connection accounting"

git add -A
git commit -m "Reject EXPIRE seconds beyond 100 years

std::chrono::seconds -> steady_clock::duration multiplies by 1e9 in int64; values above
~9.2e9 overflowed (UBSan: signed integer overflow in chrono.h). Bound the argument in the
parser so it is a non-fatal 'invalid expire seconds' error; add a parser test."
git push
```

## 7. Optional extensions (design notes only; not in v1.0.0)

Do none of these before v1.0.0 is tagged. If you do one after, it is a v1.1.0 with its own
tests and README section. Each is a good "what would you do next" answer even if never built.

* **`SCAN cursor [MATCH prefix] [COUNT n]`** (section 2.4). Cursor = bucket index; hold the
  shared lock per call, not per scan; document that keys inserted during a scan may or may
  not be returned and that a rehash can cause duplicates (Redis's reverse-binary cursor
  avoids misses across a rehash; read about it before claiming you would implement it).
* **`maxmemory` with LRU eviction.** Account `key.size() + value.size() + overhead` per entry
  (overhead: node, hash, `std::string` headers; estimate it as a constant and say so). Exact
  LRU needs an intrusive list updated on every `GET`, which turns reads into writes and kills
  the shared lock's benefit. Redis uses approximated LRU by sampling; that is the version
  compatible with a reader lock.
* **Sharded store.** N `Store` shards, `shard = hash(key) % N`, each with its own
  `shared_mutex`. Writers on different shards stop contending. Costs: `KEYS` and purge must
  visit every shard (and cannot give a consistent cross-shard snapshot); multi-key `DEL`
  takes several locks (lock in shard order to avoid deadlock). Do it only with a benchmark
  showing writer contention first (`perf`, `off-cpu` time in `futex`).
* **Prometheus endpoint.** A second listener that answers `GET /metrics` with the `render()`
  text plus `# TYPE` lines. Ten lines of HTTP, and the server plugs into every monitoring stack.
* **Windowed percentiles.** Two histograms; the acceptor swaps them every interval and logs
  the old one's p99, so the log line describes the last 10 seconds, not all time.

## 8. What you learned, and what to say in an interview

* **Relaxed atomics are correct for independent counters**, and you can say why (no data is
  published through them) and what they cost (a `lock`-prefixed instruction on x86, plus
  cache-line traffic if they share a line).
* **Histograms, not averages.** Means hide the tail; p99 is what users feel. Bucketed
  percentiles are upper bounds; say so.
* **Lazy plus active expiry** is the standard TTL design; a full-scan purge is the simple
  version; sampling or an expiry index is the scalable one.
* **`KEYS` is O(n) under a lock and stalls writers;** `SCAN` or an ordered index is the fix.
* **RAII for counters**: the active-connection guard is correct on every exit path by
  construction.
* **Green sanitizers prove nothing about inputs you never sent.** The `EXPIRE` overflow
  passed every CI job until one test sent a big number.

Interview sentence: "Observability is a `Metrics` struct of relaxed atomics, no locks, and a
16-bucket log-scale latency histogram of command execution time; `STATS` renders it and the
acceptor logs ops/s and p99 every ten seconds. TTL is lazy expiry on read under the shared
lock plus a once-a-second purge under the exclusive lock; I know the purge is O(n) and would
move to Redis-style sampling or an expiry heap at scale. And one honest story: UBSan in CI
was green, but `EXPIRE` with ten billion seconds overflowed the chrono conversion to
nanoseconds; I found it by feeding the UBSan build hostile input by hand, bounded it in the
parser, and pinned it with a test."
