# 09. Leaks and memory growth

**Time:** ~3 hours   **Prerequisites:** 03, 08

## 1. In plain words

"The server's memory keeps going up" is the most common long-running-process ticket and the
most misdiagnosed. People say "leak", but memory grows for three different reasons that need
three different fixes: a **leak** (memory nobody can reach any more and nobody will free), **growth**
(memory that is reachable and in use - a cache, a table, a queue that is legitimately or
illegitimately bigger than planned) and **fragmentation** (memory the allocator holds but cannot
reuse or return). Leak tools find only the first. A cache without an eviction policy is not a
leak to Valgrind, but it will still take the server down on day 40.

So the job has two halves. First, measure what kind of growth you have: resident memory over
time, and inside it, live bytes versus allocator overhead. Second, find the allocation site that
accounts for the growth, which is what heap profilers do: they record a stack trace for every
allocation and show you which stacks own the most bytes that are still alive. This module does
both on Linux (heaptrack, massif, LSan) and on Windows (UMDH, `!heap`), and ends with the C++
specific traps: smart pointer cycles and containers that never shrink.

## 2. The real thing

### 2.1 Three kinds of growth

| Kind | Reachable? | Will it ever be freed? | Tool that sees it | Fix |
|------|-----------|------------------------|-------------------|-----|
| Leak | no | no | LSan, Valgrind (definitely lost), heaptrack "leaked" | RAII / fix the missing free |
| Growth (unbounded live data) | yes | only if the program decides to | heaptrack/massif/UMDH: large and growing live stacks; Valgrind "still reachable" | eviction policy, size cap, erase when done |
| Fragmentation / allocator retention | n/a (free) | it is already free | `malloc_stats`: system >> in-use; `!heap -s` committed >> busy | `malloc_trim`, arena cap, pooling, different allocator |

A fourth that looks like memory growth but is not heap: **thread count** (each thread's stack
and arena), **file descriptors / handles** (not memory, but "resource leak"), and **mapped
files** (page cache attributed to the process). Check these first; they are a `ls /proc/PID/fd |
wc -l`, `ls /proc/PID/task | wc -l`, `!handle`, away.

### 2.2 Measuring: RSS over time, and what is inside

```bash
# resident set, every 10 s, forever; graph it later
while sleep 10; do echo "$(date +%s) $(ps -o rss= -p $PID)"; done >> rss.log
# or one shot with more detail
grep -E 'VmRSS|VmHWM|VmSize|VmData|VmStk|Threads' /proc/$PID/status
cat /proc/$PID/smaps_rollup                # Rss, Pss, Anonymous, Private_Dirty totals
pmap -x $PID | sort -k3 -n | tail -20      # biggest mappings by RSS: [heap]? anon (arenas)? a file?
```

Interpretation:

- **RSS rises then plateaus**: warm-up (caches filling, arenas reaching steady state). Normal.
  Alarm thresholds must sit above the plateau.
- **RSS rises linearly with requests, never falls**: leak or unbounded growth. Proportional to
  traffic is the tell.
- **RSS rises in steps and stays**: fragmentation or arena growth under thread churn; or periodic
  big jobs whose freed memory is retained.
- **`[heap]` grows**: main-arena allocations (single-threaded or main thread). **Anonymous
  64 MiB-aligned mappings grow**: per-thread arenas. **A file mapping grows**: page cache, not
  your bug.

Inside the process (glibc):

```c
#include <malloc.h>
malloc_stats();                 /* to stderr: "system bytes" (from OS) vs "in use bytes" (live) per arena */
struct mallinfo2 mi = mallinfo2();  /* mi.uordblks = in use, mi.fordblks = free in bins, mi.hblkhd = mmapped, mi.arena = brk size */
malloc_info(0, stdout);         /* XML with per-arena, per-bin detail */
```

Expose these on an admin endpoint or log them hourly. "In use" climbing = leak/growth; "system"
climbing while "in use" is flat = fragmentation/retention. That one comparison decides which
half of this module you are in. `code/m09_growth_not_leak.cpp` prints exactly this.

Windows equivalents: Performance Monitor counters `Process\Private Bytes` (committed private
memory, closest to "heap grew"), `Process\Working Set` (RSS), `Process\Handle Count`,
`Process\Thread Count`; `!heap -s` in a dump for committed vs busy per heap; VMMap for the
categorisation.

### 2.3 Allocator retention and fragmentation (glibc)

Reasons freed memory does not come back:

- Small chunks go to bins, not to the OS (module 03). Only the top of an arena is trimmed, and
  only when the free top exceeds `M_TRIM_THRESHOLD` (128 KiB). One live 64-byte block near the
  top pins everything below it.
- Each thread arena retains its own free lists. 100 worker threads, each with a 20 MB working set
  at some point, keep 2 GB even when idle.
- `std::vector`, `std::string`, `std::unordered_map` keep capacity after `clear()`.
- `realloc` growth patterns leave holes.

Remedies, in order of how often they are the right answer:

1. Fix unbounded containers first (2.6); fragmentation is often a symptom of churn in huge
   containers.
2. `MALLOC_ARENA_MAX=2` (or `mallopt(M_ARENA_MAX, 2)`) for servers with many threads and
   modest allocation rate. Measure contention before and after.
3. Call `malloc_trim(0)` on an idle tick (every few minutes). Cheap; returns whole free pages
   with `MADV_DONTNEED`. `m09_growth_not_leak.cpp` shows the effect.
4. Pool or arena-allocate the hot, same-size, same-lifetime objects (requests, records).
5. Switch allocator (`LD_PRELOAD=libjemalloc.so.2`); jemalloc's size classes and background
   purging reduce retention and give `malloc_stats_print()` with much better diagnostics.

### 2.4 Leak hunting tools

**LeakSanitizer** (build with ASan or `-fsanitize=leak`): at exit, every unreachable block with
its allocation stack (module 08). For a server that never exits cleanly, add an admin command
that calls `__lsan_do_recoverable_leak_check()`; or make shutdown clean (SIGTERM handler that
frees and exits) so the exit report is meaningful. Noise: `still reachable` globals are not
reported; singletons are fine.

**heaptrack** (the best Linux heap profiler for this job):

```bash
sudo apt install heaptrack heaptrack-gui
heaptrack ./server args                 # or: heaptrack -p PID  (attach to a running process, via LD_PRELOAD injection - needs ptrace)
# ... run the workload, stop the server or ^C heaptrack ...
heaptrack_print heaptrack.server.4242.zst | less
heaptrack --analyze heaptrack.server.4242.zst      # GUI: flame graph, "Allocations", "Temporary", "Leaked", "Peak" tabs, consumption over time
```

It records every `malloc`/`free`/`new`/`delete` with a stack trace (via libunwind, cheap) and
compresses the stream. The report sections: **peak heap memory consumption** with the stacks that
held it, **leaked** (never freed by the end of the trace - includes still-reachable growth, which
is what you want for a server), **most calls to allocation functions**, **most temporary
allocations** (malloc immediately followed by free - a performance smell). The GUI's "Consumed"
graph over time lets you see which stacks are responsible for the slope. Overhead is modest
(roughly 1.5-3x on allocation-heavy code; it depends on stack depth) - it has been run in staging
for hours.

**Valgrind massif**:

```bash
valgrind --tool=massif --time-unit=B --detailed-freq=1 --max-snapshots=200 ./prog
ms_print massif.out.<pid> | less           # ASCII graph of heap size over time + per-snapshot allocation trees
valgrind --tool=massif --pages-as-heap=yes ./prog   # count mmap too (arenas, big blocks)
```

Massif gives heap snapshots over time with the allocation tree at each peak. Slower than
heaptrack (Valgrind overhead) and cannot attach to a running process, but needs no install
beyond valgrind and is precise about `--pages-as-heap` (total mapped memory, not just malloc).

**Valgrind memcheck** `--leak-check=full --show-leak-kinds=all` at exit: definitely/indirectly
lost for leaks; `still reachable` with big counts and the stack points at growth.

**gdb on a core**: when you only have a dump of the bloated process and no profiler was running,
you can still find the growth: `info proc mappings` to see which region is big; if `[heap]`, walk
it by hand (`x/4xg` on chunk headers, or a Python script that walks from the arena start,
reading `size` fields) and histogram the sizes; a dominant size class points at one struct type.
Then `x/16xg` a few blocks of that size to recognise the type (vtable pointer, string lengths).
This is slow and manual; the Windows side has it built in (`!heap -stat`).

### 2.5 Windows: UMDH and `!heap`

**UMDH** (User-Mode Dump Heap, Debugging Tools for Windows) is heaptrack by snapshots:

```
gflags /i YourServer.exe +ust                  # enable stack-trace DB for the process (next start)
set _NT_SYMBOL_PATH=srv*C:\Symbols*https://msdl.microsoft.com/download/symbols;D:\pdb
umdh -p:<pid> -f:snap1.txt                     # snapshot 1: every allocation stack with counts and bytes
... let it run / drive load ...
umdh -p:<pid> -f:snap2.txt
umdh snap1.txt snap2.txt -f:diff.txt           # the DIFF: stacks whose byte count increased, sorted
```

`diff.txt` lines like `+ 4194304 ( 4259840 - 65536) 65536 allocs BackTrace7A3B` followed by the
stack: 64 K more allocations of 64 bytes from this stack between snapshots. That is your growth
site, whether leak or unbounded container. Requires `+ust` (allocation stacks captured), symbols,
and the process to be alive. Overhead of `+ust` is acceptable in many production servers.

**In a full dump** (`!heap`):

```
!heap -s                                 # per heap: Committed vs Virt vs Free; the heap whose committed bytes are huge
!heap -stat -h <heap>                    # size histogram: "size 0x40: 1,200,000 blocks = 76 MB"  <- the dominant size class
!heap -flt s 0x40                        # list blocks of that size (take a few addresses)
dt YourServer!Session <UserPtr>          # does it look like a Session? vtable? strings?
!heap -p -a <UserPtr>                    # with +ust: the allocation stack  <- the site
!heap -l                                 # heuristic leak detection: busy blocks with no pointer to them (slow)
```

The `-stat` -> `-flt` -> recognise -> `-p -a` chain is the Windows "walk the heap by hand" and it
is faster than the Linux manual version. Do it on two dumps taken an hour apart and compare the
histograms: the class that grew is the culprit.

Also: `!address -summary` (is it even heap? or `VirtualAlloc`'d private data, or mapped files?),
`!handle 0 0` counts (handle leak), `~` thread count.

### 2.6 C++-specific causes

- **Containers that only grow**: `std::map<SessionId, Session>` where `erase` happens on a path
  that is skipped on error; `std::unordered_map` keyed by request id "for correlation"; a
  `std::vector<LogEntry>` ring that was never made a ring; a `std::deque` queue with a consumer
  that fell behind. The fix is a *policy*: max size, TTL, LRU eviction; and a metric for the size.
- **`clear()` keeps capacity**: `vector::clear()` leaves `capacity()` unchanged; `shrink_to_fit()`
  or swap with an empty vector releases it. A per-request `std::vector` member that once held
  10 MB holds 10 MB forever. Same for `std::string`. `unordered_map` keeps its bucket array.
- **`shared_ptr` cycles**: A holds `shared_ptr<B>`, B holds `shared_ptr<A>`; use counts never
  reach zero. Parent/child, observer/subject, node/graph structures. LSan reports these as leaks
  (unreachable from roots). Fix: one direction becomes `weak_ptr` (child -> parent, observer ->
  subject). `p sp` in gdb shows `use count`; a use count that never drops below 1 after the owner
  released is the sign.
- **Lambdas capturing `shared_ptr` by value** stored in callbacks/timers/`std::function` members
  of the same object: a cycle through the closure. Capture `weak_ptr` and lock inside.
- **`std::thread` detached with captured state**, or threads never joined: each keeps its
  stack and arena.
- **Exceptions between `new` and `delete`**: not a leak if you use `unique_ptr`; a leak with raw
  `new`. Also `new[]` / `delete` mismatch (ASan `alloc-dealloc-mismatch`).
- **Static/global caches** ("still reachable"): intentional, but they need bounds.
- **C-style ownership in the migration engine**: a record struct with `char *name` freed on the
  success path but not on `continue` in the loop - the `m09_leak.cpp` shape. In C the cure is
  `goto cleanup` discipline or arenas (module 03); in C++ it is RAII.

### 2.7 Hunting a leak in a long-running server: the procedure

1. **Confirm it is heap.** `smaps_rollup` / `pmap` / `!address -summary`: anonymous heap growing,
   not threads, fds, mapped files.
2. **Confirm live bytes grow.** `malloc_stats` in-use (or `!heap -s` busy) climbing. If only
   system/committed climbs, it is retention: go to 2.3.
3. **Correlate with traffic.** Does it grow per request, per session, per minute, per batch? Which
   request type? Logs with request ids (module 11) answer this; it halves the suspects.
4. **Profile in staging with the same traffic**: heaptrack (Linux) or UMDH diff (Windows). Look
   at the top growing stacks. Nine times out of ten you now have the line.
5. **If only production shows it**: `+ust` + UMDH on the production box in a maintenance window;
   or heaptrack `-p` attach for 10 minutes; or two full dumps an hour apart and the
   `!heap -stat` / heap-walk comparison; or `gcore` twice and compare with a Python heap walker.
6. **Classify**: leak (fix the free / RAII), growth (add a bound/eviction), retention (trim /
   arena cap / allocator).
7. **Verify**: same workload, RSS flat after warm-up, `in use` flat, heaptrack "leaked" near zero.
   Put the metric on a dashboard so recurrence is visible (module 12's verification section).

## 3. Code

`code/m09_leak.cpp`: a request loop that leaks on the error path (every 4th request). Build and
run three ways:

```bash
./build_debug.sh debug asan
./build/m09_leak_asan 1000
# ==ERROR: LeakSanitizer: detected memory leaks
# Direct leak of 32000 byte(s) in 250 object(s) allocated from: #1 handle_request m09_leak.cpp:33
# Indirect leak of 16250 byte(s) in 250 object(s) ...                    <- the std::string buffers
valgrind --leak-check=full --show-leak-kinds=all ./build/m09_leak_debug 1000
#    definitely lost: 32,000 bytes in 250 blocks / indirectly lost: 16,250 bytes in 250 blocks
heaptrack ./build/m09_leak_debug 100000 && heaptrack_print heaptrack.m09_leak_debug.*.zst | sed -n '/MEMORY LEAKED/,/^$/p'
```

Note that LSan's direct+indirect equals Valgrind's definitely+indirectly: a `Request` (direct)
owning a `std::string` buffer (indirect). Fixing the one missing `delete` (or using
`unique_ptr`, as `m13_raii_fix.cpp` does) removes both.

`code/m09_growth_not_leak.cpp`: 3 million 64-byte blocks, then free 99.9% of them, then
`malloc_trim(0)`. Typical output shape (your numbers will differ):

```
start                        VmRSS =      3 MiB
after 3M x 64B malloc        VmRSS =    230 MiB
after freeing 99.9%          VmRSS =    230 MiB      <- free() returned nothing to the OS
after malloc_trim(0)         VmRSS =     20 MiB      <- most pages given back; 3000 live blocks pin ~3000 pages
malloc_stats(): system bytes = ..., in use bytes = ~200 KB
```

Run it under Valgrind or LSan: zero leaks. That is the whole lesson in one screen: RSS high, in-use
tiny, no leak. Then try `MALLOC_TRIM_THRESHOLD_=0 ./build/m09_growth_debug` and see how the
"after freeing" line changes.

## 4. Under the hood (senior-level)

**Why RSS does not fall on `free`.** `free` puts the chunk in a bin; the page it lives on stays
resident because other chunks on the same page may be live and because `munmap`/`madvise` per
small free would be far slower than the free itself. glibc trims only the top (`sbrk` negative
or `munmap` of a secondary heap's tail) when it exceeds the trim threshold, and `malloc_trim`
additionally scans for whole free pages and `madvise(MADV_DONTNEED)`s them. `MADV_DONTNEED` drops
the physical pages; the virtual range stays mapped; a later touch faults in a zero page. That is
why RSS falls but VSZ does not.

**How LSan decides "unreachable".** At the check, it stops all threads, takes the root set
(globals in writable sections of all modules, every thread's stack and registers, TLS), and does a
conservative mark from them: any aligned word that looks like a pointer into a live allocation
marks that allocation, recursively. Anything unmarked is a leak; a leak pointed to only by other
leaks is "indirect". Conservative scanning means an integer that happens to equal a block's
address hides a leak (rare), and pointers stored XORed or in unusual places cause false positives.

**heaptrack's trick.** It `LD_PRELOAD`s a library that interposes `malloc`/`free`/etc., records
each with a stack trace captured by libunwind, deduplicates traces into ids, and streams
`(trace_id, size, ptr)` events to a compressor. Replaying the stream tells you at any instant
which trace ids own which bytes. "Leaked" is simply "allocated, not freed by end of trace", so it
includes still-reachable growth - exactly what you want for a server.

**Why UMDH needs `+ust`.** The NT heap by default stores no allocation stacks. `+ust` makes
`RtlAllocateHeap` capture a stack (via `RtlCaptureStackBackTrace`) into a per-process stack trace
database and store its index in the block header. UMDH reads that database; so does `!heap -p -a`.
Without `+ust` you can histogram sizes but cannot see who allocated.

**`shared_ptr` cycles and the mark-sweep contrast.** Reference counting cannot collect cycles;
garbage-collected runtimes can. LSan's mark from roots *is* a tracing collector's reachability
analysis, which is why it sees the cycle as a leak. `weak_ptr` breaks the cycle by making one
edge non-owning: the control block keeps a weak count so `lock()` can fail safely after the
object is gone.

**Heap walking in a core.** glibc's `main_arena` (in libc's `.data`; needs libc symbols) has
`top` and the bins; the main heap starts at the `[heap]` mapping start; chunks are contiguous:
`next = chunk + (size & ~7)`. A 60-line Python gdb script can walk it, bucket sizes and print the
first 32 bytes of samples from the dominant bucket. Windows' `!heap -stat` is this script,
shipped.

## 5. In production

- **Instrument before you need it.** Log `mallinfo2().uordblks`, RSS, thread count and fd count
  every few minutes, and the sizes of your big containers (sessions, caches, queues). Most leak
  investigations become "look at the graph" when these exist, and the RCA's verification section
  becomes a screenshot.
- **Set the alarm on slope, not level.** RSS 6 GB is fine if it has been 6 GB for a month. +50 MB
  per hour for six hours is not, even at 2 GB.
- **Distinguish before you fix.** Adding `malloc_trim` to a server with a real leak hides the
  symptom for a while and costs CPU; adding a cache bound to a server with fragmentation does
  nothing. The `in use` vs `system` comparison is five minutes and settles it.
- **Restarts are a mitigation, not a fix.** Banks run month-long processes; a nightly restart
  "to clear memory" is sometimes accepted operationally but must be documented as a known issue
  with a tracked fix, never as the resolution in an RCA (module 12).
- **Session tables in RPC servers.** A client that disconnects uncleanly (TCP RST, crash) leaves a
  session entry if cleanup only runs on a graceful close. Timeouts and periodic sweeps are not
  optional. Shakir's multithreaded RPC-over-TCP server is the textbook case: count sessions,
  compare to connections, alarm on divergence.
- **Windows specifics.** `Private Bytes` is the counter to trend. UMDH diffs with `+ust` are
  routine in a maintenance window. Beware the CRT debug heap in debug builds (`_CrtDumpMemoryLeaks`
  reports leaks at exit for the debug CRT, useful in tests) - not available in release.
- **Third-party and driver leaks.** Database client libraries (connection handles, prepared
  statements not finalised), TLS contexts, compression streams. The profiler stack will end in
  the library's allocation with your call above it - the fix is usually a missing `*_free` call on
  an error path in your code.

## 6. Try it

1. **Easy - three tools, one leak.** Run `m09_leak` under LSan, Valgrind and heaptrack. Done when
   you can show that all three attribute the same bytes to `handle_request` and explain
   direct/indirect vs definitely/indirectly.
2. **Easy - growth vs leak.** Run `m09_growth_not_leak`, then run it under LSan. Done when you
   can write the three RSS numbers and the `in use bytes` number on one line and name the kind of
   growth.
3. **Medium - RSS trend script.** Write a shell script that samples RSS, `VmHWM`, thread count
   and fd count of a PID every 5 seconds to a CSV, run it against `m09_leak 10000000` (slow it
   down with a small `usleep` if needed) and plot or eyeball the slope. Done when you can state
   bytes leaked per request from the slope alone and compare it with LSan's total.
4. **Medium - fix it two ways.** Fix `m09_leak` with (a) a `delete` on the error path and (b)
   `std::unique_ptr`. Introduce a `throw` in `validate` and show that only (b) still does not
   leak. Done when LSan is clean for (b) with the throw and dirty for (a).
5. **Hard - shared_ptr cycle.** Write a `Node` with `std::shared_ptr<Node> next` and make a
   two-node cycle that goes out of scope. Show the leak with LSan, inspect `use count` in gdb,
   fix with `weak_ptr`. Done when LSan is clean and you can explain why reference counting
   cannot collect cycles.
6. **Hard - heap histogram from a core.** Run `m09_leak 200000` under gdb, break at the end,
   `gcore`, and write a gdb Python script that walks the main heap from the `[heap]` start,
   histograms chunk sizes and prints the top 3 size classes with the first 32 bytes of a sample
   chunk. Done when the dominant class matches `sizeof(Request)` rounded to a chunk size.

## 7. Interview questions

**Q: RSS grows steadily but Valgrind and LSan report no leaks. What are the possibilities?**
A: Unbounded but reachable growth (a container, cache or queue without a bound - "still
reachable" to Valgrind), allocator retention/fragmentation (freed memory kept in bins and arenas,
visible as `malloc_stats` system bytes far above in-use bytes), thread or handle growth, or
mapped-file page cache. I distinguish them with `malloc_stats`/`mallinfo2`, `smaps`, thread and
fd counts, and a heap profiler that shows live bytes per stack over time.

**Q: How do you find the allocation site responsible for growth in a running Linux server?**
A: heaptrack attached to the process (or run under it in staging with the same traffic): it
records a stack per allocation and reports the stacks owning the most still-live bytes and the
growth over time. Alternatives: massif, LSan's recoverable leak check from an admin command, or
two `gcore`s and a heap-walking script.

**Q: What is UMDH and how do you use it?**
A: Windows' user-mode heap snapshot tool. Enable the stack trace database for the process
(`gflags /i app.exe +ust`), take `umdh -p:PID -f:snap1.txt`, let the growth happen, take a second
snapshot, then `umdh snap1 snap2` diffs them and lists the allocation stacks whose byte counts
increased - the growth sites. Needs symbols and a live process.

**Q: Why does `free` not return memory to the OS, and how can you make it?**
A: Small chunks go into the allocator's bins for reuse; the OS only gets memory back when the top
of an arena exceeds the trim threshold or a large mmapped block is freed. `malloc_trim(0)` scans
for whole free pages and `madvise`s them away; capping arenas (`MALLOC_ARENA_MAX`) limits
per-thread retention; pooling same-size objects or switching to jemalloc reduces fragmentation.
Measure in-use vs system bytes first to confirm retention is the problem.

**Q: How can `std::shared_ptr` leak?**
A: Through reference cycles: two objects owning each other via `shared_ptr` never reach a zero
count. Also via lambdas that capture a `shared_ptr` to the object that stores the lambda. The fix
is to make the back edge a `weak_ptr` and `lock()` it when needed. LSan detects the cycle as
unreachable; `p sp` in gdb shows a use count that never drops.

**Q: A `std::vector` member of a long-lived object once held 100 MB and now holds 10 elements.
How much memory does it use?**
A: Still about 100 MB: `clear()` and `erase` do not reduce capacity. Call `shrink_to_fit()` or
swap with an empty vector to release it. The same applies to `std::string` and to the bucket
arrays of unordered containers.

**Q: What do you log in a server so leak investigations are short?**
A: Periodic RSS, `mallinfo2` in-use and system bytes, thread count, fd/handle count, and the
sizes of every major container (sessions, caches, queues), plus request counts by type with
request ids so growth can be correlated to traffic. With those, most investigations start from a
graph that already names the suspect.

## 8. Recap

- Three kinds of growth: leak (unreachable), growth (reachable, unbounded), retention/
  fragmentation (free but held). Leak tools see only the first.
- First measure: RSS trend; then `malloc_stats`/`mallinfo2` in-use vs system (Windows:
  `!heap -s`, Private Bytes). In-use flat + system rising = retention; both rising = leak/growth.
- Check threads, fds/handles and mapped files before blaming the heap.
- Linux tools: LSan (exit or `__lsan_do_recoverable_leak_check`), heaptrack (attach or run; live
  bytes per stack over time), massif, Valgrind leak kinds, manual heap walk in gdb.
- Windows tools: UMDH snapshots + diff with `+ust`; `!heap -s` / `-stat` / `-flt` / `-p -a`;
  `!address -summary`; VMMap; Private Bytes counter.
- Retention remedies: `malloc_trim(0)` on idle, `MALLOC_ARENA_MAX`, pooling/arenas, jemalloc.
- C++ traps: containers without bounds, `clear()` keeps capacity, `shared_ptr` cycles (use
  `weak_ptr`), capturing lambdas, unjoined threads, raw `new` on exception paths.
- Instrument by design: log memory stats and container sizes; alarm on slope; verify the fix with
  the same graph.
