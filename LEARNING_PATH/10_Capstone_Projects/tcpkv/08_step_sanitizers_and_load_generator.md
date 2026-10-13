# Step 08. Load generator, ASan, TSan, UBSan and Valgrind: clean under concurrent load

**Time:** ~8 hours   **Week:** 4 (Oct 27 – Nov 02)
**Read first:** 05_Memory_Debugging_RCA: `08_valgrind_and_sanitizers.md` (how each tool works),
`04_the_catalogue_of_memory_bugs.md`, `09_leaks_and_memory_growth.md`,
`10_concurrency_crashes_and_heisenbugs.md`. 04_Concurrency: `02_data_races_and_why_they_are_ub.md`,
`07_atomics_and_the_memory_model.md` (happens-before), `12_debugging_concurrency_bugs.md`.
06_Linux_Toolchain_DevOps: `03_gcc_and_clang_flags_that_matter.md` (`-fsanitize=`,
`-fno-omit-frame-pointer`), `09_ci_cd_github_actions_and_azure_devops.md` (matrix jobs).
02_Modern_CPP: `13_cpp17_features_tour.md` (`std::from_chars`).

## 1. Goal

Make the resume line "Valgrind-clean under concurrent load" true, and provable by anyone in
one command.

* `load_gen`, a C++ load generator that is also a **correctness checker**: every `GET` is
  verified against the last value that client wrote.
* `tools/load_gen.py`, the same contract in 220 lines of Python with no build step.
* `tools/valgrind_check.sh`: run the server under Valgrind memcheck, drive it with
  `load_gen`, stop it with SIGINT, fail on any error or leak.
* CI grows from `debug, asan` (step 01) to `debug, release, asan, tsan, ubsan` plus a
  Valgrind job. All green, every push.
* Zero findings from all four tools. Not "zero findings I could not explain away". Zero.

In plain words: so far the tests show the server does the right thing when you ask it
nicely, one thing at a time. This step makes sixteen clients shout at once for ten seconds
while three different X-ray machines watch the memory and the threads. If anything is read
after it was freed, read before it was written, or touched by two threads without a lock,
one of the machines lights up with a stack trace.

## 2. Design

### 2.1 Four tools, four kinds of bug

| tool | how it works | catches | typical cost (source) |
|------|--------------|---------|-----------------------|
| **ASan** (`-fsanitize=address`) | compiler adds a check before every load/store against *shadow memory* (1 shadow byte per 8 bytes of app memory); `malloc` is replaced to add poisoned *redzones* around blocks and to *quarantine* freed blocks | heap/stack/global buffer overflow, use-after-free, use-after-scope, double free; with **LSan** (on by default on x86-64 Linux) leaks at exit | ~2x CPU (AddressSanitizer docs) |
| **TSan** (`-fsanitize=thread`) | every memory access and every sync operation (mutex, atomic, thread create/join) is recorded; per-thread vector clocks compute *happens-before*; two accesses to the same address, at least one a write, with no happens-before between them = data race | data races, lock-order inversions (potential deadlock), unsafe calls in signal handlers, destroyed locked mutex | 5x–15x CPU, 5x–10x memory (ThreadSanitizer docs) |
| **UBSan** (`-fsanitize=undefined`) | compiler adds a check at each operation that can be UB | signed overflow, shift out of range, misaligned pointer, null deref through a reference, invalid `bool`/enum load, out-of-bounds index on a fixed array | small; usually under 2x |
| **Valgrind memcheck** | runs the *unmodified* binary on a synthetic CPU (dynamic binary translation); tracks a validity bit for every bit of memory and an addressability bit for every byte | invalid reads/writes on the heap, use of **uninitialised** values (ASan cannot), bad syscall arguments, leaks, open descriptors at exit (`--track-fds=yes`) | "typically 10–50 times slower" (Valgrind manual) |

Rules that follow from the mechanisms:

* **ASan and TSan cannot be combined.** Both own the shadow memory layout and both replace
  `malloc`. Hence separate presets and separate CI jobs.
* **Valgrind cannot run a sanitizer build.** Both intercept `malloc`; run Valgrind on a
  plain build (`relwithdebinfo`).
* **Valgrind and ASan overlap but are not redundant.** Valgrind sees uninitialised reads and
  checks the binary you would ship. ASan sees stack and global overflows, which memcheck
  cannot (it only knows heap block boundaries), and is fast enough to run the whole test
  suite.
* **TSan only sees races that happen in the run.** It does not prove absence. That is why
  the stress tests and `load_gen` use a small key space and no sleeps: maximise interleavings.
* **Leak checks happen at exit.** Only a clean exit makes "definitely lost" meaningful. A
  server killed with `SIGKILL` reports nothing. That is one more reason graceful shutdown
  (step 06) was not optional.

### 2.2 `load_gen`: a benchmark that is also a test

A load generator that only counts replies will happily measure a server that returns the
wrong value. So:

```
client i owns keys  "lg:<run_id>:<i>:0" .. "lg:<run_id>:<i>:<keyspace-1>"
versions[k] = last version client i wrote to key k   (0 = never written)

SET:  version = versions[k] + 1 ; value = "<i>:<k>:<version>:xxxx...x" ; versions[k] = version
GET:  versions[k] == 0  -> reply must be nil
      otherwise         -> reply must equal make_value(i, k, versions[k])
```

* **Disjoint key ranges** mean no other client can legitimately change your keys, so a
  mismatch is a server bug, never a test race. Shared-key workloads are possible but then you
  can only check "value is one of the values someone wrote", which is much weaker.
* **The run id** (time XOR `random_device`) makes key names unique per invocation. Without
  it, a second run against the same server would find the first run's values and count
  mismatches.
* **The value encodes who wrote it and which version**, so a torn read or a stale read is
  detected and points to the culprit.
* **Exit status 1 on any error or mismatch** makes it usable from scripts and CI.

Latency is recorded per operation into a `LatencyHistogram` (the same lock-free histogram the
server uses for `STATS` in step 09), one per worker thread, merged at the end. Percentiles are
bucket upper bounds, so the output says `p99<=200 us`, which is honest: the histogram cannot
know more precisely than its bucket.

### 2.3 Why the Valgrind check is a script, not a test

It needs a real server process (Valgrind wraps the process), a real load (a test with one
client exercises one worker), and a real SIGINT (so the shutdown and leak check run). A shell
script is the simplest thing that does all three. It uses `--error-exitcode=42` so "Valgrind
found something" is distinguishable from "the server crashed" (non-zero, not 42) and from
"the server was fine but `load_gen` saw a mismatch".

## 3. Files to create

### `include/tcpkv/metrics.hpp` and `src/metrics.cpp` (step-08 version: the histogram only)

`load_gen` needs the histogram now; the server's `Metrics` struct and `STATS` come in step 09
and are added to these same files. Step-08 header:

```cpp
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

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

}  // namespace tcpkv
```

`src/metrics.cpp` for this step is `code/src/metrics.cpp` without `Metrics::render()` and
without `#include <sstream>`. The implementation is explained in step 09.

Add `src/metrics.cpp` to the `tcpkv` library in `CMakeLists.txt`.

### `tools/load_gen.cpp`

Identical to `code/tools/load_gen.cpp`. The heart of it:

```cpp
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
```

Things a reviewer will check:

* **Each thread writes only its own `WorkerResult`** (passed by `std::ref(results[i])`), and
  `main` reads them only after `join()`. `join` is a synchronisation point (happens-before),
  so no atomics are needed. TSan agrees.
* **Each thread has its own `Client`, RNG and histograms.** Nothing shared, nothing locked
  in the measurement loop. A shared `std::mt19937_64` would be a data race; a shared one
  behind a mutex would measure the mutex.
* **The seed** mixes the run id with the thread id times the 64-bit golden-ratio constant,
  so threads do not walk the same key sequence.
* **`steady_clock`, never `system_clock`, for intervals.** `system_clock` can jump (NTP).
* **The latency includes the client's own parsing**, which is what a user of the server
  experiences. That is why the README says "client-observed round trip".

`main` parses options with `std::from_chars` (with range checks: `--clients` > 0,
`--value-size` <= 1 MiB, `--read-ratio` in [0, 1]), starts the threads, joins them, merges,
and prints:

```
tcpkv load_gen: 16 clients, 10 s, keyspace 10000 per client, value 64 B, read ratio 0.80, target 127.0.0.1:7379
total  <N> ops in 10.00 s  ->  <N/10> ops/s
GET    n=<...>  mean=<...> us  p50<=<...> us  p99<=<...> us  p99.9<=<...> us
SET    n=<...>  mean=<...> us  p50<=<...> us  p99<=<...> us  p99.9<=<...> us
errors 0, mismatches 0
```

### `tools/load_gen.py`

Identical to `code/tools/load_gen.py`. Same contract (disjoint keys, versions, exit 1 on
mismatch), its own 80-line client:

```python
    def set(self, key: str, value: bytes) -> None:
        self.sock.sendall(f"SET {key} {len(value)}\n".encode() + value + b"\n")
        kind, payload = self._read_reply()
        if kind != b"+" or payload != b"OK":
            raise ProtocolError(f"unexpected reply to SET: {kind!r} {payload!r}")
```

Why have it at all: it proves the protocol is easy to speak from another language (an
interviewer may ask "how would a Python service use this?"), it needs no build, and it is an
independent implementation of the client, so a bug shared by `Client` and the server
(both using `encode_set`, say) cannot hide from it. It is **not** a benchmark: CPython's GIL
lets only one thread run Python bytecode at a time, so its ops/s measures Python. Its
percentiles are exact (it keeps every sample) where the C++ tool's are bucketed.

### `tools/valgrind_check.sh`

Identical to `code/tools/valgrind_check.sh` **except** that in step 08 the server line has no
`--stats-interval 0` (that flag arrives in step 09; add it then, to keep the log quiet):

```bash
#!/usr/bin/env bash
# Runs tcpkv_server under Valgrind memcheck, drives it with load_gen, stops it with SIGINT and
# fails if memcheck reported any error or any definite/indirect leak.
#
#   bash tools/valgrind_check.sh build/relwithdebinfo
#
# Build with RelWithDebInfo: optimised enough to finish in CI, with symbols for readable reports.
set -euo pipefail

BUILD_DIR=${1:-build/relwithdebinfo}
PORT=${PORT:-7400}
CLIENTS=${CLIENTS:-8}
DURATION=${DURATION:-5}
VALGRIND_EXIT=42

if ! command -v valgrind >/dev/null; then
    echo "valgrind not installed (sudo apt install valgrind)" >&2
    exit 2
fi

valgrind \
    --quiet \
    --error-exitcode=$VALGRIND_EXIT \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    "$BUILD_DIR/tcpkv_server" --port "$PORT" --threads 4 &
SERVER_PID=$!

# Wait until the server answers PING (Valgrind makes start-up slow).
for _ in $(seq 1 100); do
    if "$BUILD_DIR/tcpkv_cli" -p "$PORT" ping >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! "$BUILD_DIR/tcpkv_cli" -p "$PORT" ping >/dev/null 2>&1; then
    echo "server did not come up on port $PORT" >&2
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" || true
    exit 1
fi

set +e
"$BUILD_DIR/load_gen" --port "$PORT" --clients "$CLIENTS" --duration "$DURATION" --keyspace 1000
LOAD_STATUS=$?
set -e

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SERVER_STATUS=$?
set -e

echo "load_gen exit: $LOAD_STATUS, server exit: $SERVER_STATUS"
if [ "$SERVER_STATUS" -eq "$VALGRIND_EXIT" ]; then
    echo "valgrind reported errors" >&2
    exit 1
fi
if [ "$SERVER_STATUS" -ne 0 ]; then
    echo "server exited abnormally" >&2
    exit 1
fi
if [ "$LOAD_STATUS" -ne 0 ]; then
    echo "load generator reported errors or mismatches" >&2
    exit 1
fi
echo "valgrind: clean"
```

Line by line, the parts that matter:

* `set -euo pipefail`: stop on the first failing command, on an unset variable, and on a
  failure anywhere in a pipeline. The script turns `-e` off (`set +e`) exactly around the two
  commands whose exit status it wants to inspect rather than die on.
* `--show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect`: only
  memory with no pointer to it at exit ("definitely lost") and memory reachable only from
  such blocks ("indirectly lost") fail the run. "Still reachable" (a global still points at
  it, as libstdc++ and glibc do for some one-time allocations) and "possibly lost" (an
  interior pointer exists) are not leaks of *your* server in the sense the resume means. If
  "possibly lost" ever shows up with a `pthread_create` frame, it means a thread was not
  joined: treat that as a bug anyway.
* The readiness loop polls with the CLI because Valgrind start-up takes seconds; a fixed
  `sleep` would be either flaky or slow.
* `kill -INT` then `wait` gives the server's exit status, which under Valgrind is 42 if
  memcheck found anything.
* **Clients versus threads.** A connection holds a worker for its whole life and a
  `load_gen` client stays connected until its deadline. The script starts 4 workers and 8
  clients, so 4 clients are served for the whole run and the other 4 wait in the pool queue
  and do their first operation only at the end. That is still a useful test (it exercises
  the queue and the queued-connection path at shutdown), but the true concurrency is 4. If
  you want "8 concurrent clients" to be literally true, change the server line to
  `--threads "$CLIENTS"`. Everywhere in this course: N concurrent `load_gen` clients need
  `--threads N` or more.

### `CMakeLists.txt`

Add the tools option and target:

```cmake
option(TCPKV_BUILD_TOOLS "Build the load generator" ON)
...
if(TCPKV_BUILD_TOOLS)
    add_executable(load_gen tools/load_gen.cpp)
    target_link_libraries(load_gen PRIVATE tcpkv tcpkv_warnings)
endif()
```

The sanitizer wiring from step 01 already applies to every target in the tree, including
`load_gen` and GoogleTest. That matters: a TSan-instrumented test linked against an
uninstrumented library would miss races inside the library and can report false ones.

### `.github/workflows/ci.yml` (final version)

```yaml
name: CI

on:
  push:
    branches: [main]
    tags: ["v*"]
  pull_request:

jobs:
  build-and-test:
    name: ${{ matrix.preset }}
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        preset: [debug, release, asan, tsan, ubsan]
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends ninja-build g++

      - name: Lower ASLR entropy for ThreadSanitizer
        # TSan in GCC 13's runtime aborts with "unexpected memory mapping" on kernels that use
        # 32 bits of mmap randomisation (6.x). 28 bits is the documented workaround.
        if: matrix.preset == 'tsan'
        run: sudo sysctl -w vm.mmap_rnd_bits=28

      - name: Configure
        run: cmake --preset ${{ matrix.preset }}

      - name: Build
        run: cmake --build --preset ${{ matrix.preset }} --parallel

      - name: Test
        run: ctest --preset ${{ matrix.preset }} --parallel 2

  valgrind:
    name: valgrind (memcheck under load)
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends ninja-build g++ valgrind

      - name: Configure and build (RelWithDebInfo, no tests)
        run: |
          cmake --preset relwithdebinfo -DTCPKV_BUILD_TESTS=OFF
          cmake --build --preset relwithdebinfo --parallel

      - name: Run the server under memcheck while load_gen hammers it
        run: bash tools/valgrind_check.sh build/relwithdebinfo

  format:
    name: clang-format (advisory)
    runs-on: ubuntu-24.04
    # Advisory until the whole tree has been formatted once (step 10 of the build guide
    # removes this line and makes formatting a hard requirement).
    continue-on-error: true
    steps:
      - uses: actions/checkout@v4
      - name: Install clang-format
        run: sudo apt-get update && sudo apt-get install -y --no-install-recommends clang-format
      - name: Check formatting
        run: |
          find include src apps tools tests -name '*.hpp' -o -name '*.cpp' \
            | xargs clang-format --dry-run --Werror
```

* `fail-fast: false`: if TSan fails you still want to know whether ASan passed.
* The `tsan` preset is `RelWithDebInfo`, not `Debug`: TSan's overhead on an `-O0` build makes
  the stress tests slow enough to hit the 300 s per-test timeout on a CI runner.
* The test presets set `halt_on_error=1` for every sanitizer, so the first report fails the
  test instead of scrolling past in a log nobody reads.
* `tags: ["v*"]` makes the `v1.0.0` tag (step 10) run CI too, so the release has a green run.

## 4. Build and run (WSL Ubuntu 24.04): the exact commands

Install once: `sudo apt install -y valgrind python3` (Valgrind 3.22 on Ubuntu 24.04).

### 4.1 Unit and integration tests under each sanitizer

```bash
cd ~/dev/tcpkv
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset ubsan && cmake --build --preset ubsan && ctest --preset ubsan
cmake --preset tsan  && cmake --build --preset tsan  && ctest --preset tsan
```

If every TSan test dies at start-up with `FATAL: ThreadSanitizer: unexpected memory mapping`,
your kernel uses more mmap randomisation than GCC 13's TSan runtime supports. Same fix as CI:

```bash
sudo sysctl -w vm.mmap_rnd_bits=28   # until the next WSL restart (wsl --shutdown)
```

Without sudo, turn address-space randomisation off for one command instead. This also
covers `cmake --build`, because `gtest_discover_tests` runs each test binary at build time to
list its tests, and a TSan binary that aborts there fails the build:

```bash
setarch $(uname -m) -R cmake --build --preset tsan
setarch $(uname -m) -R ctest --preset tsan
```

This failure is intermittent (it depends on where the kernel happens to place mappings), so
"it passed once" does not mean the setting is right. It is the WSL2 kernel's default of 32
random bits that triggers it, the same as on the GitHub runner.

To re-run only the stress tests many times (races are probabilistic):

```bash
ctest --preset tsan -R 'Stress|Concurrent' --repeat until-fail:20
```

### 4.2 The whole server under a sanitizer, driven by `load_gen`

The tests exercise the code paths; this exercises them with sixteen clients at once.

```bash
cmake --preset release && cmake --build --preset release      # an uninstrumented load_gen

# ThreadSanitizer server (drop the setarch prefix if you ran the sysctl above)
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
    setarch $(uname -m) -R ./build/tsan/tcpkv_server --port 7400 --threads 16 &
./build/release/load_gen --port 7400 --clients 16 --duration 10 --keyspace 100
kill -INT %1; wait %1; echo "server exit: $?"

# AddressSanitizer + LeakSanitizer server
ASAN_OPTIONS="detect_leaks=1 halt_on_error=1 detect_stack_use_after_return=1" \
    ./build/asan/tcpkv_server --port 7400 --threads 16 &
./build/release/load_gen --port 7400 --clients 16 --duration 10 --value-size 64
kill -INT %1; wait %1; echo "server exit: $?"

# UBSan server
UBSAN_OPTIONS="halt_on_error=1 print_stacktrace=1" \
    ./build/ubsan/tcpkv_server --port 7400 --threads 8 &
./build/release/load_gen --port 7400 --clients 8 --duration 10
kill -INT %1; wait %1; echo "server exit: $?"
```

Expected each time: `load_gen` prints `errors 0, mismatches 0`, the server logs
`tcpkv stopped cleanly`, `server exit: 0`, and no sanitizer report on stderr. `--keyspace 100`
for TSan concentrates traffic on few hash buckets; `--value-size 64` for ASan makes values
heap-allocated (short strings live inside the `std::string` object, the small-string
optimisation, and a use-after-free on them may not touch the heap at all).

### 4.3 Valgrind

```bash
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
bash tools/valgrind_check.sh build/relwithdebinfo
# ... load_gen output ...
# load_gen exit: 0, server exit: 0
# valgrind: clean
```

Longer and harder: `CLIENTS=16 DURATION=30 bash tools/valgrind_check.sh build/relwithdebinfo`.

Descriptor leaks are invisible to ASan. Check them by hand once:

```bash
valgrind --track-fds=yes --leak-check=full ./build/relwithdebinfo/tcpkv_server --port 7400 --threads 8 &
./build/relwithdebinfo/load_gen --port 7400 --clients 8 --duration 5
kill -INT %1; wait %1
```

At exit the `FILE DESCRIPTORS: N open` summary must list only 0, 1, 2 (stdin, stdout, stderr)
and, depending on the Valgrind version, Valgrind's own log descriptor. A socket in that list
is a leak. While the server runs, `ls -l /proc/$(pgrep -f tcpkv_server)/fd | wc -l` should
return to its idle value after `load_gen` finishes.

Optional: `valgrind --tool=helgrind` and `--tool=drd` are Valgrind's race detectors. They
understand `pthread` mutexes and rwlocks (which `std::mutex` and `std::shared_mutex` wrap on
glibc) but not C++ atomics, so they report false races on the atomic stop flag and metrics.
TSan is the race detector of record for this project; know that the Valgrind ones exist.

### 4.4 Python load generator

```bash
python3 tools/load_gen.py --port 7379 --clients 8 --duration 10
```

## 5. Test by hand

### 5.1 The green path

1. All six presets pass: `debug`, `release`, `asan`, `tsan`, `ubsan` via `ctest`, and
   `valgrind_check.sh` prints `valgrind: clean`.
2. `load_gen` against the debug server and the release server: `mismatches 0` both times.
   Note the ops/s difference; you will put release numbers in the README in step 10.
3. Push. All six CI jobs green. The `format` job may be red; it is advisory until step 10.

### 5.2 What sanitizers find in a server like this, and the fix

Every item below is a bug that real thread-per-connection servers ship with. Most are bugs
the tcpkv design already avoids; know *which line* avoids each, because "how did you get it
Valgrind-clean?" is a standard follow-up. The reports are abridged to their shape.

**1. A plain `bool` stop flag (TSan: data race).**

```
WARNING: ThreadSanitizer: data race (pid=...)
  Write of size 1 at 0x7b0c... by main thread:
    #0 tcpkv::Server::request_stop() src/server.cpp:...
  Previous read of size 1 at 0x7b0c... by thread T3:
    #0 tcpkv::Connection::serve() src/connection.cpp:...
```

A non-atomic flag written by one thread and read by another is a data race, which is
undefined behaviour, not just "might see a stale value": the compiler may hoist the load out
of the loop and spin forever. Fix: `std::atomic<bool>` (what `Server::stop_` is). Relaxed
loads are enough for the flag itself (step 06).

**2. Returning a reference or `string_view` into the map (ASan: heap-use-after-free).**

```cpp
const std::string& Store::get_ref(const std::string& key) const {   // WRONG
    std::shared_lock lock(mutex_);
    return map_.at(key).value;
}   // lock released here; the caller still holds a reference into the map
```

Another worker's `SET` on the same key assigns a new value (freeing the old buffer), or a
`DEL` erases the node outright. (A rehash alone would not hurt: `unordered_map` never moves
its nodes, so references survive rehashing. Erasure and reassignment are the killers.)
The caller's later read hits freed memory:

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x6060... READ of size 64
    #0 __interceptor_memcpy
    #1 std::string::string(const std::string&)
    #2 tcpkv::execute_command(...)
freed by thread T2 here:
    #0 operator delete(void*)
    #1 tcpkv::Store::set(...)
```

Fix: copy under the lock and return by value (`std::optional<std::string>`), which is what
`Store::get` does. This is also why the ASan run uses 64-byte values: with SSO, a 10-byte
value is stored inside the `std::string` object and the bug can go unreported.

**3. Capturing the connection by reference in the task (ASan: stack-use-after-return).**

```cpp
Connection connection(std::move(client), ...);
pool.submit([&connection] { connection.serve(); });   // WRONG: accept_one() returns first
```

The worker runs after `accept_one` has returned and its stack frame is reused. With
`detect_stack_use_after_return=1`, ASan reports `stack-use-after-return`; without it you get
random crashes. Fix: init-capture by move, `[conn = std::move(connection)]() mutable`, which
is why `MoveOnlyFunction` exists.

**4. Raw `new` for per-connection state (LSan: direct leak).**

```
ERROR: LeakSanitizer: detected memory leaks
Direct leak of 96 byte(s) in 3 object(s) allocated from:
    #0 operator new(unsigned long)
    #1 tcpkv::Server::accept_one(...)
```

A path that returns early (busy rejection, an exception from `peer_address()`, shutdown
dropping queued tasks) skips the `delete`. Fix: no raw `new`; values and `unique_ptr` only.
The dropped-queued-task path is exactly what `ThreadPool::stop()` exercises, and the
`Probe` test in `thread_pool_test.cpp` checks the destructors run.

**5. A leaked descriptor (Valgrind `--track-fds=yes`; ASan is blind to it).** A `close()`
missing from one error path of a C-style accept loop. Over days the process reaches
`ulimit -n` and `accept` fails with `EMFILE`. Fix: `FileDescriptor` owns every descriptor
(step 02); there is no `close()` outside `FileDescriptor::reset`. `SOCK_CLOEXEC`, `accept4`
and `pipe2(O_CLOEXEC)` also stop descriptors leaking into a `fork()`ed child.

**6. Logging from the signal handler (TSan: signal-unsafe call; in production: deadlock).**

```
WARNING: ThreadSanitizer: signal-unsafe call inside of a signal
    #0 malloc
    #1 tcpkv::log::format(...)
    #2 on_signal(int)
```

If the signal lands while the interrupted thread holds the `malloc` lock or the log mutex,
the handler waits for a lock its own thread holds: a hang on Ctrl-C, once a month. Fix: the
handler does an atomic store and one `write(2)` to the self-pipe (step 06), nothing else.

**7. Uninitialised values (Valgrind: "Conditional jump or move depends on uninitialised
value(s)", or "Syscall param ... points to uninitialised byte(s)").** Typical sources: a
member without a default initialiser (`bool stopped_;`, read by a worker before the
constructor body sets it) or a `socklen_t len;` passed to `getpeername` without setting it
to `sizeof storage`. ASan does not report these; Valgrind does. Fix: default member
initialisers everywhere (`bool stopped_ = false;`, `std::atomic<bool> stop_{false};`) and
value-initialisation for C structs (`sockaddr_storage storage{};`, `pollfd fds[2]{};`).

**8. Erasing while iterating (ASan: heap-use-after-free).**

```cpp
for (auto it = map_.begin(); it != map_.end(); ++it) {
    if (is_expired(it->second, now)) map_.erase(it);   // WRONG: ++it on an erased node
}
```

Fix: `it = map_.erase(it);` and increment only in the `else` branch, as in
`Store::purge_expired` (step 09).

**9. Signed overflow in number parsing and time arithmetic (UBSan).** A hand-written
`len = len * 10 + (c - '0')` overflows on `SET k 99999999999999999999` before any range check
can run. Fix: `std::from_chars`, which reports `result_out_of_range` instead. The same class
of bug hides in `std::chrono`: converting a huge `seconds` value to the clock's nanoseconds
multiplies by 10^9 in a signed 64-bit integer. Step 09 shows the one place in tcpkv where
this can happen, the UBSan report it produces, and the fix.

**10. Lock-order inversion (TSan: "lock-order-inversion (potential deadlock)").** Thread A
takes the store lock then a metrics mutex; thread B (handling `STATS`) takes the metrics
mutex then reads the store. TSan reports it even if the deadlock never happens in the run,
because it records lock acquisition order. tcpkv avoids the class entirely: `Metrics` is
atomics only, and no code path holds two locks at once. Say that sentence in interviews.

**11. Destroying a joinable `std::thread` (not a sanitizer finding: `std::terminate`).**
If `ThreadPool`'s constructor throws after starting some workers and does not join them, the
`std::thread` destructors call `std::terminate` and you get `terminate called without an
active exception` and a core dump. Fix: the constructor's catch block stops and joins before
rethrowing (step 06). In gdb, `bt` on the core shows `std::thread::~thread`.

**12. Shutdown races (TSan: data race on a destroyed object, or ASan: use-after-free).**
The signal handler calls `request_stop()` on a `Server` that `main` has already destroyed.
Fix: `g_server` is cleared (`store(nullptr)`) before `server` goes out of scope, and the
`Server` is not movable so the pointer cannot dangle by relocation.

### 5.3 Break it on purpose (do at least three)

You do not really know a tool until you have seen it catch something you planted.
Use a throwaway branch: `git switch -c break-it`.

1. **Race:** in `src/connection.cpp` add a global `std::uint64_t g_commands = 0;` in the
   anonymous namespace and `++g_commands;` in `process_events` next to the command
   execution. Run `ctest --preset tsan -R ManyConcurrentClients`. Read the report: the two
   conflicting accesses with their stacks, the "Location is global 'g_commands'" line, and
   the stacks that created the two threads. Then make it `std::atomic<std::uint64_t>` and
   watch the report disappear. (Test names in ctest are the gtest names, `Suite.Test`, so
   `-R` matches on those, not on file names.)
2. **Use-after-free:** add `get_ref` from item 2 to `Store` and use it in the `GET` branch of
   `execute_command`. Run the ASan server with `load_gen --keyspace 10 --value-size 64
   --read-ratio 0.5`. Expected: a report within seconds.
3. **Leak:** in `accept_one`, replace the move-capture with
   `auto* conn = new Connection(std::move(connection)); pool.submit([conn] { conn->serve(); });`.
   Run `ctest --preset asan -R 'ServerFixture|ServerShutdown'` and read the LSan summary.
   Then add `delete conn;` after `serve()`: the tests pass, but the program still leaks
   whenever a queued task is dropped at shutdown, because a dropped task never runs its body.
   Prove it: ASan server with `--threads 1`, three `nc` sessions, Ctrl-C. That is the
   argument for RAII, demonstrated.
4. **Uninitialised read:** remove `= false` from `bool stopped_ = false;` in `thread_pool.hpp`
   and run `bash tools/valgrind_check.sh build/relwithdebinfo`. The pool lives on `run()`'s
   stack, so `stopped_` starts as stack garbage: memcheck reports "Conditional jump or move
   depends on uninitialised value(s)" in `worker_loop`. If the garbage happened to be
   non-zero, the workers also exit immediately and `load_gen` fails: the same bug, seen from
   the outside. Neither `-Wall -Wextra` nor ASan says anything.
5. **Valgrind error exit:** confirm the script really fails: plant item 2 in a
   `relwithdebinfo` build and run the script. Expected: `valgrind reported errors`, exit 1.

`git switch main && git branch -D break-it` when done.

## 6. Commit

```bash
git add -A
git commit -m "Add load generators, Valgrind check and full sanitizer CI

- LatencyHistogram: lock-free log-scale histogram (relaxed atomics), snapshot/merge/percentile
- load_gen: N client threads with disjoint key ranges; verifies every GET against the last
  SET; per-op latency histograms; exit 1 on any error or mismatch
- load_gen.py: same contract in Python, independent client implementation
- valgrind_check.sh: memcheck on the server under load, SIGINT shutdown, fails on errors
  or definite/indirect leaks
- CI: debug/release/asan/tsan/ubsan matrix (tsan with vm.mmap_rnd_bits=28) + valgrind job"
git push
```

## 7. What you learned, and what to say in an interview

* **Each tool has a mechanism, and the mechanism tells you its blind spots.** ASan: shadow
  memory and redzones, no uninitialised reads, no descriptor leaks. TSan: happens-before over
  the accesses that actually happened. Valgrind: synthetic CPU, slow, sees uninitialised
  values and syscall arguments, cannot see stack overflows. UBSan: per-operation checks.
* **They do not combine**, so CI runs a matrix. Sanitize the whole build tree, gtest
  included.
* **A load generator must verify, not just count.** Disjoint key ranges plus versioned
  values turn a benchmark into a correctness test.
* **Leak checking needs a clean exit**, which makes graceful shutdown a testability
  requirement, not a nicety.
* **Most findings in a server come from five patterns:** non-atomic flags, references into
  shared containers, lambda captures by reference into async work, raw `new` on early-return
  paths, and unsafe work in signal handlers. tcpkv avoids each by construction, and you can
  point to the line.

Interview sentence: "Every push runs the full test suite under ASan with LeakSanitizer, TSan
and UBSan, and a separate job runs the release-with-debug-info server under Valgrind memcheck
while load-generator clients hammer it for five seconds, then stops it with SIGINT so
the leak check runs; any error fails the build. The load generator gives each client a
disjoint key range and versions every value, so it verifies every GET rather than just
counting replies. I planted the classic bugs on a branch (a plain bool stop flag, a reference
returned from the store, a raw new in the accept path) to make sure each tool catches them
the way I expect."
