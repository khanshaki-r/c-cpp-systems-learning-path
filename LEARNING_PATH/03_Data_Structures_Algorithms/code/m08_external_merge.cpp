// m08_external_merge.cpp
// Simulated external merge sort: the "disk" is a vector of blocks, "memory" holds at most M
// elements. Phase 1 sorts M-sized runs; phase 2 does a k-way merge with a min-heap of run
// heads, refilling one block at a time. Counts block reads and writes so you can see the
// 2 x n x passes I/O cost. Supports a limited fan-in and therefore multiple merge passes.
//
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -pthread m08_external_merge.cpp -o m08_ext

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <queue>
#include <random>
#include <tuple>
#include <vector>

namespace {

constexpr std::size_t kBlock = 1024;   // elements per "disk block"

struct Disk {
    std::vector<std::vector<int>> runs;    // each run is a sorted sequence stored as a flat vector
    std::size_t block_reads = 0, block_writes = 0;

    void write_run(std::vector<int> run) {
        block_writes += (run.size() + kBlock - 1) / kBlock;
        runs.push_back(std::move(run));
    }
    // Read block `b` of run `r` into out. Returns false when past the end.
    bool read_block(std::size_t r, std::size_t b, std::vector<int>& out) {
        const std::vector<int>& run = runs[r];
        std::size_t start = b * kBlock;
        if (start >= run.size()) return false;
        ++block_reads;
        out.assign(run.begin() + static_cast<std::ptrdiff_t>(start),
                   run.begin() + static_cast<std::ptrdiff_t>(std::min(start + kBlock, run.size())));
        return true;
    }
};

// Phase 1: read M elements at a time, sort in memory, write a run.
void make_runs(const std::vector<int>& input, std::size_t M, Disk& disk) {
    for (std::size_t i = 0; i < input.size(); i += M) {
        std::vector<int> run(input.begin() + static_cast<std::ptrdiff_t>(i),
                             input.begin() + static_cast<std::ptrdiff_t>(std::min(i + M, input.size())));
        disk.block_reads += (run.size() + kBlock - 1) / kBlock;
        std::sort(run.begin(), run.end());
        disk.write_run(std::move(run));
    }
}

// One k-way merge of runs [first, first+k) into a new run. Memory: k input blocks + heap.
std::vector<int> merge_runs(Disk& disk, std::size_t first, std::size_t k) {
    struct Cursor {
        std::vector<int> buf;    // current block
        std::size_t pos = 0;     // index in buf
        std::size_t block = 0;   // which block is loaded
    };
    std::vector<Cursor> cur(k);
    // Heap of (value, run index). std::greater makes it a min-heap.
    using Item = std::pair<int, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;

    for (std::size_t i = 0; i < k; ++i)
        if (disk.read_block(first + i, 0, cur[i].buf)) heap.emplace(cur[i].buf[0], i);

    std::vector<int> out;
    while (!heap.empty()) {
        auto [val, i] = heap.top();
        heap.pop();
        out.push_back(val);
        Cursor& c = cur[i];
        if (++c.pos == c.buf.size()) {                       // block exhausted: fetch the next
            c.pos = 0;
            if (!disk.read_block(first + i, ++c.block, c.buf)) continue;   // run exhausted
        }
        heap.emplace(c.buf[c.pos], i);
    }
    return out;
}

// Phase 2: merge passes with fan-in F until one run remains.
std::vector<int> external_sort(const std::vector<int>& input, std::size_t M, std::size_t fan_in, Disk& disk,
                               int& passes) {
    make_runs(input, M, disk);
    passes = 1;
    while (disk.runs.size() > 1) {
        ++passes;
        Disk next;
        next.block_reads = disk.block_reads;
        next.block_writes = disk.block_writes;
        for (std::size_t first = 0; first < disk.runs.size(); first += fan_in) {
            std::size_t k = std::min(fan_in, disk.runs.size() - first);
            std::vector<int> merged = merge_runs(disk, first, k);
            next.block_reads = disk.block_reads;   // merge_runs counted reads on `disk`
            next.write_run(std::move(merged));
        }
        disk = std::move(next);
    }
    return disk.runs.empty() ? std::vector<int>{} : disk.runs[0];
}

}  // namespace

int main() {
    std::mt19937 rng(99);
    const std::size_t n = 2'000'000;         // "100 GB"
    const std::size_t M = 100'000;           // "1 GB of memory": 20 runs
    std::vector<int> data(n);
    for (int& x : data) x = static_cast<int>(rng());
    std::vector<int> expect = data;
    std::sort(expect.begin(), expect.end());

    // Enough fan-in for a single merge pass.
    {
        Disk disk;
        int passes = 0;
        std::vector<int> out = external_sort(data, M, 32, disk, passes);
        assert(out == expect);
        std::size_t blocks = (n + kBlock - 1) / kBlock;
        std::printf("n=%zu, M=%zu, fan-in 32: %d passes, %zu block reads, %zu block writes (n = %zu blocks)\n",
                    n, M, passes, disk.block_reads, disk.block_writes, blocks);
    }
    // Limited fan-in forces a second merge pass: every pass reads and writes all the data once.
    {
        Disk disk;
        int passes = 0;
        std::vector<int> out = external_sort(data, M, 5, disk, passes);
        assert(out == expect);
        std::printf("n=%zu, M=%zu, fan-in 5:  %d passes, %zu block reads, %zu block writes\n", n, M, passes,
                    disk.block_reads, disk.block_writes);
    }
    std::printf("external merge sort output matches std::sort\n");
    return 0;
}
