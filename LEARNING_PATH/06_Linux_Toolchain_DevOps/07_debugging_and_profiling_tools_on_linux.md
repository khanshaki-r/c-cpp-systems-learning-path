# 07. Debugging and profiling tools on Linux

**Time:** ~2.5 hours   **Prerequisites:** 02, 03; tcpkv built with the `release` and `debug` presets

## 1. In plain words

On Windows you have WinDbg for crashes, Process Monitor for "what files and registry keys is it
touching", PerfView for "where is the time going", and Resource Monitor for "which process holds
that port". Linux has one or two small tools for each of those questions, and they are all on the
command line, scriptable, and installed with one `apt` command. This module is a tour of the
toolbox with the five commands per tool that you will actually use, and a worked example on tcpkv.

The key mental shift: on Linux the kernel exposes almost everything as text under `/proc` and
through tracing hooks (`ptrace`, `perf_event_open`, tracepoints). The tools are thin wrappers over
those. When a tool is missing, `cat /proc/<pid>/...` often answers the question directly.

Analogy: Process Monitor is a security camera in the hallway between your program and the OS.
`strace` is the same camera. `perf` is a stopwatch that clicks 1,000 times a second and writes down
which line of code was running. `valgrind` runs your program inside a slow-motion simulator that
checks every memory access.

## 2. The real thing

### 2.1 Which tool for which question

| Question | Tool |
|---|---|
| Why did it crash? Where? What were the variables? | `gdb` (live or core), sanitizers |
| What syscalls is it making? Why is it hanging? What file did it fail to open? | `strace` |
| Which library functions is it calling? | `ltrace` |
| Where is CPU time going? | `perf record` / `perf report`, flame graphs |
| Exact instruction and cache counts, call graph with costs | `valgrind --tool=callgrind` + `kcachegrind` |
| Memory errors | ASan (fast), `valgrind --tool=memcheck` (no rebuild) |
| What is the process doing right now (memory, threads, fds)? | `/proc/<pid>/`, `top`, `htop`, `pidstat` |
| Who has this file or port open? | `lsof`, `ss` |
| What is on the wire? | `tcpdump`, Wireshark |
| How long does it take? | `time`, `hyperfine`, `perf stat` |
| What is the system doing (disk, net, CPU)? | `vmstat 1`, `iostat -x 1`, `sar`, `dstat` |

### 2.2 gdb quick reference

Full coverage, including core dumps, multithreaded debugging and Python scripting, is in
05_Memory_Debugging_RCA. The essentials:

```bash
gdb ./build/debug/tcpkv                       # start
gdb --args ./build/debug/tcpkv 7001           # with program arguments
gdb -p $(pgrep tcpkv)                         # attach to a running process (needs ptrace permission)
gdb ./build/release/tcpkv core.1234           # post-mortem on a core file
gdb -batch -ex 'thread apply all bt' -p PID   # non-interactive: dump all stacks and exit
```

Inside gdb:

```
(gdb) break tcpkv::parse_command          # b: break at function; b protocol.cpp:62; b main if argc > 1
(gdb) run                                 # r
(gdb) next / step / finish / continue     # n: over, s: into, fin: to return, c
(gdb) backtrace                           # bt; bt full shows locals in every frame
(gdb) frame 2                             # f 2: select frame; up / down
(gdb) print line                          # p: any expression; p *this; p store.map_.size(); p/x val
(gdb) info locals / info args / info registers
(gdb) watch g_stop                        # hardware watchpoint: stop when value changes
(gdb) info threads / thread 3 / thread apply all bt
(gdb) x/16xb ptr                          # examine memory: 16 hex bytes
(gdb) list                                # source around current line
(gdb) display var                         # auto-print on each stop
(gdb) set var port = 7001                 # modify
(gdb) catch throw                         # break on C++ exception throw
(gdb) handle SIGPIPE nostop noprint       # do not stop on this signal
(gdb) tui enable                          # split-screen source view (Ctrl-x a)
```

Enable core dumps once: `ulimit -c unlimited` and check `/proc/sys/kernel/core_pattern` (on
Ubuntu it is often piped to `apport` or `systemd-coredump`; `coredumpctl list` and
`coredumpctl gdb <pid>` read them). Attaching requires `ptrace_scope` permission:
`echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope` on a dev box, or run gdb with `sudo`.

### 2.3 strace: the syscall camera

`strace` uses `ptrace` to stop the process at every syscall entry and exit and print the call,
arguments and return value. It answers "what is the program asking the kernel to do".

```bash
strace ./build/release/tcpkv 7001                 # trace from start; Ctrl-C to stop
strace -f -p $(pgrep tcpkv)                       # attach; -f follows threads and children
strace -f -tt -T -o trace.log ./tcpkv 7001        # -tt timestamps, -T time spent in each call, -o to file
strace -e trace=network ./tcpkv 7001              # only socket calls; also: file, process, memory, signal, desc
strace -e trace=openat,connect -f ./prog          # specific syscalls
strace -c ./prog                                  # summary: count, time, errors per syscall
strace -s 256 ...                                 # show 256 bytes of string args instead of 32
strace -y ...                                     # print the path behind each fd: 3<socket:[12345]>
strace -k ...                                     # stack trace per syscall (slow, superb for "who opened this")
```

Reading tcpkv accepting a connection and serving `SET k v` then `GET k`:

```
[pid 4123] 10:21:03.112 poll([{fd=3, events=POLLIN}], 1, 500) = 1 ([{fd=3, revents=POLLIN}]) <0.084>
[pid 4123] 10:21:03.112 accept4(3, {sa_family=AF_INET, sin_port=htons(51234), sin_addr=inet_addr("127.0.0.1")}, [16], SOCK_CLOEXEC) = 4 <0.000021>
[pid 4123] 10:21:03.112 setsockopt(4, SOL_TCP, TCP_NODELAY, [1], 4) = 0 <0.000008>
[pid 4123] 10:21:03.112 write(2, "2026-10-03T10:21:03 INFO  accepted 127.0.0.1:51234\n", 52) = 52 <0.000019>
[pid 4123] 10:21:03.113 clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|..., stack=0x7f...}, 88) = 4130 <0.000061>
[pid 4130] 10:21:03.113 recvfrom(4, "SET k v\n", 4096, 0, NULL, NULL) = 8 <0.512>
[pid 4130] 10:21:03.625 sendto(4, "OK\n", 3, MSG_NOSIGNAL, NULL, 0) = 3 <0.000024>
[pid 4130] 10:21:03.625 recvfrom(4, "GET k\n", 4096, 0, NULL, NULL) = 6 <1.203>
[pid 4130] 10:21:04.829 sendto(4, "VALUE v\n", 8, MSG_NOSIGNAL, NULL, 0) = 8 <0.000019>
[pid 4130] 10:21:04.829 recvfrom(4, "", 4096, 0, NULL, NULL) = 0 <2.001>
[pid 4130] 10:21:06.830 close(4) = 0 <0.000011>
```

Reading it: pid 4123 is the main thread in `poll` with the 500 ms timeout; `accept4` returns fd 4;
`clone3` with `CLONE_THREAD` is `std::thread` starting; the new thread 4130 blocks in `recvfrom`
(`recv` is `recvfrom` with null address) for 0.5 s waiting for the client to type; `= 8` bytes
received; `sendto` with `MSG_NOSIGNAL`; `recvfrom` returning `0` is EOF (client closed);
`close`. Every `<time>` is wall time inside the syscall, so a server "doing nothing" shows big
numbers on blocking calls and that is normal; a server "doing nothing" with tiny numbers in a tight
loop is a busy-wait bug.

Typical finds: `openat(..., "/etc/app.conf") = -1 ENOENT` (wrong path), `connect(...) = -1
ECONNREFUSED` (nothing listening), `futex(...)` forever (deadlock: now use gdb to see who holds
the lock), `write(1, ...) = -1 EPIPE`, thousands of `stat` calls (library probing paths),
`mmap`/`munmap` churn (allocator thrash), `EAGAIN` loops (non-blocking fd polled wrongly).

Cost: `strace` stops the process twice per syscall; a syscall-heavy program can run 10-100x slower
(typical). Never leave it attached to a production server; use `perf trace` or `bpftrace` there.

### 2.4 ltrace: library calls

```bash
ltrace ./prog                             # calls into shared libraries: malloc, strlen, SSL_read...
ltrace -e 'malloc+free' ./prog            # filter
ltrace -c ./prog                          # summary
ltrace -S ./prog                          # include syscalls
```

Works through the PLT (module 02), so it only sees calls that cross a shared-library boundary;
`-static-libstdc++` or inlined calls are invisible. Useful for "how many times does it call
`malloc` per request" and for seeing what a closed-source library does.

### 2.5 perf: sampling profiler

`perf` reads hardware performance counters and samples the instruction pointer (and optionally the
stack) at a fixed rate. It needs the kernel package `linux-tools-$(uname -r)` or
`linux-tools-generic`; on WSL2 the Microsoft kernel does not ship a matching `perf`, so install
`linux-tools-generic` and run the binary directly from `/usr/lib/linux-tools/*/perf`, or build
perf from the WSL2 kernel source. Hardware counters may be unavailable in the VM (software
clock sampling still works with `-e cpu-clock`).

```bash
# permissions for non-root sampling
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
echo 0 | sudo tee /proc/sys/kernel/kptr_restrict

perf stat ./build/release/tcpkv_tests                 # counters: cycles, instructions, IPC, branch-misses, cache-misses
perf stat -e task-clock,context-switches,page-faults ./prog

perf record -g ./build/release/tcpkv 7001            # -g: call graphs via frame pointers (build with -fno-omit-frame-pointer)
perf record -g -p $(pgrep tcpkv) -- sleep 30          # attach for 30 s while load_test.sh runs
perf record --call-graph dwarf -F 999 -p PID -- sleep 10   # unwinding via DWARF when no frame pointers; bigger files
perf report                                           # TUI: % per symbol; Enter expands callers/callees
perf report --stdio --no-children | head -50          # text; --no-children = self time
perf annotate tcpkv::Store::get                       # per-instruction hot spots with source
perf top                                              # live, system-wide
perf trace                                            # strace-like, far lower overhead
```

Reading `perf report --no-children` on tcpkv under load (illustrative shape):

```
# Overhead  Command  Shared Object       Symbol
    24.1%   tcpkv    libc.so.6           [.] __memmove_avx_unaligned_erms
    18.7%   tcpkv    [kernel.kallsyms]   [k] _raw_spin_unlock_irqrestore
    11.2%   tcpkv    libc.so.6           [.] malloc
     9.8%   tcpkv    tcpkv               [.] std::_Hash_bytes
     7.4%   tcpkv    libc.so.6           [.] pthread_mutex_lock
     5.1%   tcpkv    tcpkv               [.] tcpkv::parse_command
```

Interpretation: `memmove` and `malloc` at the top means string copying dominates (we copy the
key into a `std::string` for every lookup because C++17 `unordered_map` has no heterogeneous
lookup, module 05 noted it). `_raw_spin_unlock_irqrestore` in the kernel is the socket send/recv
path. `pthread_mutex_lock` at 7% says the single store mutex is contended; `perf report -g` and
expanding it shows the callers. This is how you decide what to optimise: measure, then act on the
top entry.

**Flame graphs** turn the call-graph samples into one SVG where width is time and depth is stack:

```bash
git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph
perf record -g -p $(pgrep tcpkv) -- sleep 20
perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > tcpkv.svg
# open tcpkv.svg in a browser (from Windows: \\wsl.localhost\Ubuntu-24.04\home\...\tcpkv.svg)
```

Read a flame graph bottom-up: `main` at the bottom, wide plateaus at the top are where CPU is
actually spent (leaf functions). Click to zoom. Colours are random unless you use
`--color=java`-style palettes. Off-CPU flame graphs (time spent blocked) need `perf sched` or
eBPF tools; see 08_Networking for latency analysis.

### 2.6 valgrind: memcheck and callgrind

Valgrind runs the binary on a synthetic CPU, instrumenting every instruction. 20-50x slower
(typical), no rebuild needed, works on any binary with symbols.

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./build/debug/tcpkv_tests
#   --track-origins: where did the uninitialised value come from
#   exit code: use --error-exitcode=1 so CI fails on errors (module 09)
valgrind --tool=callgrind --dump-instr=yes --collect-jumps=yes ./build/release/tcpkv_tests
callgrind_annotate callgrind.out.<pid> | head -40          # text: inclusive/self cost per function
kcachegrind callgrind.out.<pid>                             # GUI (apt install kcachegrind); works over WSLg
valgrind --tool=massif ./prog && ms_print massif.out.<pid>  # heap profile over time
valgrind --tool=helgrind ./prog                             # race detector (TSan is usually better)
```

Memcheck vs ASan: ASan is 10x faster and catches stack/global overflows that memcheck misses;
memcheck catches uninitialised reads (MSan territory, Clang only) and needs no rebuild. CI runs
both (module 09). Callgrind vs perf: callgrind counts instructions exactly and gives precise call
graphs regardless of sampling noise, but its "time" is simulated and ignores cache/memory stalls
unless you enable `--cache-sim=yes`; perf measures reality including stalls. Use perf first;
callgrind when you need exact counts or the program is too short to sample.

### 2.7 /proc: the process as a directory

```bash
P=$(pgrep tcpkv)
cat /proc/$P/status            # State, Threads, VmRSS (resident), VmSize, voluntary_ctxt_switches
cat /proc/$P/cmdline | tr '\0' ' '
ls -l /proc/$P/fd              # every open descriptor and what it points to
ls /proc/$P/task               # one directory per thread (TIDs)
cat /proc/$P/task/*/comm       # thread names (set with pthread_setname_np)
cat /proc/$P/maps              # memory map: code, heap, each .so, stacks, [vdso]
cat /proc/$P/smaps_rollup      # Rss, Pss (proportional), Shared, Private, Swap
cat /proc/$P/limits            # ulimits in effect (open files!)
cat /proc/$P/stack             # kernel stack: where is it blocked (root)
cat /proc/$P/wchan             # kernel function it is waiting in
cat /proc/$P/io                # bytes read/written
cat /proc/$P/environ | tr '\0' '\n'
cat /proc/$P/sched             # scheduler stats
cat /proc/meminfo /proc/loadavg /proc/cpuinfo
cat /proc/sys/net/core/somaxconn   # kernel tunables; writable via sysctl
```

`VmRSS` is what `top` calls RES. For "is it leaking", watch `VmRSS` over time while load runs:
`watch -n1 grep VmRSS /proc/$P/status`. `Pss` is the honest number when many processes share libraries.

### 2.8 lsof, ss: who holds what

```bash
lsof -p $(pgrep tcpkv)                    # all fds of a process: files, sockets, pipes, libraries mapped
lsof -i :7000                             # who is listening on / connected to port 7000
lsof -i TCP -s TCP:ESTABLISHED            # established TCP connections, all processes
lsof +L1                                  # open files that were deleted (disk space mystery, module 01)
lsof /var/log/tcpkv.log                   # who has this file open
lsof -u shakir                            # everything a user has open

ss -tlnp                                  # TCP listening sockets with PIDs (replaces netstat -tlnp)
ss -tanp | grep 7000                      # all TCP sockets on 7000 with state
ss -s                                     # summary: sockets by state (how many TIME_WAIT?)
ss -ti dst :7000                          # per-connection TCP internals: rtt, cwnd, retrans
ss -tan state time-wait | wc -l
ss -xlp                                   # unix domain sockets
```

`ss` reads netlink, `netstat` reads `/proc/net/tcp`; `ss` is faster and shows more. `-p` needs
root to see other users' processes. "Address already in use" on restart: `ss -tanp | grep :7000`
shows either the old process still alive or sockets in TIME_WAIT (which `SO_REUSEADDR` handles,
module 11, 08_Networking).

### 2.9 tcpdump: what is on the wire

Introduction only; 08_Networking goes deep.

```bash
sudo tcpdump -i lo -nn port 7000                       # -nn: no name resolution. Watch the handshake and data
sudo tcpdump -i lo -nn -A port 7000                    # -A: print payload as ASCII (text protocols)
sudo tcpdump -i lo -nn -X port 7000                    # hex + ASCII
sudo tcpdump -i any -nn -w capture.pcap port 7000      # write pcap; open in Wireshark on Windows
sudo tcpdump -i lo -nn 'tcp[tcpflags] & (tcp-syn|tcp-fin|tcp-rst) != 0'   # connection events only
sudo tcpdump -i lo -nn -c 20 'host 127.0.0.1 and port 7000'              # stop after 20 packets
```

A SET/GET exchange:

```
10:30:01.000 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [S], seq 1, win 65495, options [mss 65495,...]
10:30:01.000 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [S.], seq 2, ack 2, ...
10:30:01.000 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [.], ack 1
10:30:01.500 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [P.], seq 1:9, ack 1, length 8        <- "SET k v\n"
10:30:01.500 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [P.], seq 1:4, ack 9, length 3        <- "OK\n"
10:30:03.000 IP 127.0.0.1.51234 > 127.0.0.1.7000: Flags [F.], seq 9, ack 4                    <- client close
10:30:03.000 IP 127.0.0.1.7000 > 127.0.0.1.51234: Flags [F.], seq 4, ack 10
```

`[S]` SYN, `[S.]` SYN-ACK, `[.]` ACK, `[P.]` PSH+ACK (data), `[F.]` FIN, `[R]` RST. Seeing `[R]`
from the server right after `[S]` means nothing is listening; a `[P.]` with no reply means the
server received it and is not answering (now `strace` or `gdb` the server).

### 2.10 time, hyperfine, perf stat: how long

```bash
time ./build/release/tcpkv_tests            # real (wall), user (CPU in user space), sys (CPU in kernel)
/usr/bin/time -v ./prog                     # GNU time: max RSS, page faults, context switches, I/O
hyperfine --warmup 3 './build/release/tcpkv_tests' './build/debug/tcpkv_tests'   # statistics, comparison
hyperfine -N --runs 50 './parser_bench 100000'                                   # -N: no shell overhead
perf stat -r 10 ./prog                       # 10 runs with mean and stddev of counters
```

`real >> user + sys` means waiting (I/O, sleep, locks, network). `sys` high means syscall-heavy
(small reads, lock contention through futex). `user` high means compute: profile with perf.
`hyperfine` does warmup, many runs, outlier detection and side-by-side comparison; it is the right
tool for "is this change faster" because single `time` runs are noise.

### 2.11 Worked example: profile tcpkv under load

```bash
cmake --preset release && cmake --build --preset release          # -O2 -g; add -fno-omit-frame-pointer in the preset for perf
./build/release/tcpkv 7001 &  SERVER=$!
./scripts/load_test.sh 7001 8 20000 &                              # 8 clients, 20k ops each (module 10)
perf record -g -p $SERVER -o perf.data -- sleep 15
perf report -i perf.data --no-children --stdio | head -30
perf script -i perf.data | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > tcpkv.svg
strace -c -f -p $SERVER -o strace_summary.txt & sleep 5; kill %2   # syscall mix during load
cat strace_summary.txt
ss -tan | grep -c 7001                                              # how many connections
cat /proc/$SERVER/status | grep -E 'Threads|VmRSS'
kill $SERVER
```

Expected findings on the thread-per-connection design: `recvfrom`/`sendto` dominate syscalls, one
pair per command (no batching); `pthread_mutex_lock` visible in perf under 8 clients; string
allocation in `parse_command` and `Store::get`. Each finding maps to a known fix (buffered writes,
pipelining, sharded locks, `string_view` keys with heterogeneous lookup in C++20), and the tools
tell you which one matters first.

## 3. Code

No new source. The tools apply to `code/tcpkv` and to anything else. For a deliberately slow
program to profile, use the tests binary (`tcpkv_tests` with `--gtest_repeat=200` on the
`StoreConcurrency` suite) or the load generator in `code/tcpkv/scripts/load_test.sh`.

A gdb command file for non-interactive stack dumps of a hung server (`code/tcpkv/scripts/` is a
good home; inline here):

```
# dump_stacks.gdb  - usage: gdb -batch -x dump_stacks.gdb -p $(pgrep tcpkv)
set pagination off
set print thread-events off
info threads
thread apply all bt 15
detach
quit
```

## 4. Under the hood (senior-level)

**ptrace.** `strace`, `ltrace` and `gdb` all use the `ptrace(2)` syscall. The tracer attaches
(`PTRACE_ATTACH` or `PTRACE_SEIZE`); the kernel then stops the tracee at the requested events
(each syscall entry/exit for `PTRACE_SYSCALL`, breakpoints via `int3` for gdb) and wakes the tracer
with `waitpid`. The tracer reads registers and memory with `PTRACE_GETREGS` /
`process_vm_readv`. Each stop is two context switches, which is the overhead. Yama's
`ptrace_scope=1` (Ubuntu default) only allows tracing your own descendants, hence the permission
step for attaching.

**How gdb breakpoints work.** gdb writes the byte `0xCC` (`int3`) over the first byte of the
target instruction. Executing it raises `SIGTRAP`, the kernel stops the tracee, gdb restores the
original byte, rewinds the instruction pointer by one, lets you poke around, then single-steps the
original instruction and re-inserts `0xCC`. Hardware watchpoints use debug registers DR0-DR3 (four
on x86) so `watch var` costs nothing until the write happens; software watchpoints single-step
everything and are thousands of times slower.

**perf sampling.** `perf_event_open(2)` asks the kernel to program a PMU counter (cycles) to
overflow every N events, or a hrtimer for `cpu-clock`. On overflow an NMI/interrupt fires; the
kernel records the IP, PID, TID, timestamp and, with `-g`, walks the user stack following frame
pointers (`rbp` chain) into a ring buffer mapped into `perf`'s address space. Without frame
pointers (`-O2` default) the chain is broken after the first frame, which is why
`-fno-omit-frame-pointer` (module 03) or `--call-graph dwarf` (copies a chunk of stack per sample
and unwinds offline) is needed. `perf_event_paranoid` gates access: 2 = own processes without
kernel samples, 1 = plus kernel, -1 = everything.

**Symbolisation.** perf and gdb map addresses to names using `.symtab` and DWARF (`-g`) from the
binary and from the shared libraries listed in `/proc/<pid>/maps`. Stripped binaries show hex
addresses; `debuginfod` can fetch symbols for distribution packages
(`export DEBUGINFOD_URLS="https://debuginfod.ubuntu.com"`). Build IDs (`readelf -n`) match a
binary to its separate debug file.

**Valgrind's engine.** Valgrind disassembles machine code into its own IR (VEX), instruments it
(memcheck adds shadow bits: 1 "addressable" bit per byte and 8 "defined" bits per byte, so it
tracks uninitialised values bit-precisely through arithmetic), JIT-compiles back to machine code
and runs it. It replaces `malloc`/`free` with its own allocator that adds redzones and delays reuse.
Because every instruction is translated, system calls are intercepted and their argument memory
checked too. The 20-50x cost is the translation plus shadow bookkeeping.

**`/proc` is a kernel filesystem.** Reading `/proc/PID/status` calls into `fs/proc/` which formats
`task_struct` fields on the fly; there is no file on disk. That is why it is always current and
costs microseconds. `smaps` walks the process's VMAs and page tables to count resident and shared
pages, which is expensive for a large process (hundreds of ms); `smaps_rollup` is the cheaper sum.

**TIME_WAIT and `ss`.** After the side that closes first sends its FIN and receives the final ACK,
the kernel keeps the socket in TIME_WAIT for 60 s (Linux, `tcp_fin_timeout` is for FIN_WAIT_2;
TIME_WAIT is fixed at 2*MSL = 60 s) to absorb stray packets. A load test that opens and closes
20,000 connections leaves 20,000 TIME_WAIT entries visible in `ss -s`; `SO_REUSEADDR` lets the
server rebind, and clients exhaust ephemeral ports (`ip_local_port_range`) if they churn connections.

## 5. In production

- **The order of operations in an incident:** (1) `ss`/`lsof`: is it listening, how many
  connections; (2) `/proc/PID/status`: threads, RSS, state; (3) `strace -f -p` for 5 seconds: is it
  doing anything, what is it blocked on; (4) `gdb -batch -ex 'thread apply all bt' -p`: where
  exactly is every thread; (5) `perf record -p` if it is CPU-bound. That sequence takes two minutes
  and answers 90% of "the server is slow/hung" pages.
- **Attaching gdb stops the process.** On a production server with a 30-second health check that
  gets you restarted mid-investigation. Use `gdb -batch` for a snapshot (sub-second), or
  `gcore PID` to write a core without killing it and analyse offline.
- **strace on production** can make a syscall-heavy server miss its latency SLO while attached.
  Attach for seconds, not minutes, or use `perf trace` / eBPF (`bpftrace`, `strace`-like
  `opensnoop`, `tcpconnect`) which have far lower overhead.
- **Keep symbols.** `perf report` showing `[unknown]` and gdb showing `??` means stripped binaries
  without separate debug files. The build pipeline (module 09) must archive them by build ID.
- **Mapping to WinDbg:** `!analyze -v` roughly equals `gdb core` + `bt full` + reading the
  faulting instruction with `x/i $pc`; `~*k` is `thread apply all bt`; `!locks` has no direct
  equivalent (inspect `pthread_mutex_t` owner field or use TSan/helgrind); `lm` is `info sharedlibrary`;
  `.ecxr` for the exception context is automatic when gdb loads a core.
- **3 AM:** server at 100% CPU, no requests being served. `perf top` shows `pthread_mutex_lock`
  and `__lll_lock_wait` dominating: a lock convoy. `gdb -batch thread apply all bt` shows 200
  threads waiting on the store mutex and one thread inside `malloc` under the lock, which is
  itself waiting on the allocator's arena lock held by a thread blocked writing to a dead socket.
  Fix: do not hold the store lock while doing I/O or allocation that can block. Tools found it in
  ten minutes; without them it is a guess.

## 6. Try it

1. **(easy)** Run `strace -f -tt -T -e trace=network ./build/debug/tcpkv 7001` and connect with
   `nc localhost 7001`, send `PING`, `SET a 1`, `GET a`, close. Done when you can name every
   syscall in the trace and explain the `= 0` from `recvfrom`.
2. **(easy)** Run `ltrace -c ./build/debug/tcpkv_tests --gtest_filter='StoreTest.*'`. Done when
   you can state how many `malloc` calls the Store tests make and which test causes most of them
   (hint: `--gtest_filter` one at a time).
3. **(medium)** Add `-fno-omit-frame-pointer` to the release preset, rebuild, run the server under
   `perf record -g` while `load_test.sh` runs, and produce a flame graph. Done when you can name
   the top three self-time symbols and propose one change for each.
4. **(medium)** Run `valgrind --tool=callgrind` on `tcpkv_tests --gtest_filter='ParseCommand.*'`
   and open the output in `callgrind_annotate`. Compare the inclusive cost of `parse_command`
   against `std::string` construction. Done when you can quantify what fraction of the parser's
   instructions are allocation.
5. **(hard)** Simulate a deadlock: add a second mutex to `Store` and acquire the two in opposite
   orders in `set` and `del`. Run the concurrency test until it hangs (or use `timeout`). Attach
   gdb, `thread apply all bt`, identify the two threads and the two locks; then confirm with
   `cat /proc/PID/task/*/wchan`. Then build with the `tsan` preset and read the lock-order
   inversion report. Done when you can show both diagnoses.
6. **(hard)** Capture a tcpkv session with `tcpdump -w`, open the pcap in Wireshark on Windows,
   and follow the TCP stream. Then produce the same view with `tcpdump -A` only. Done when you can
   point at the SYN, SYN-ACK, the PSH carrying `SET`, and the FIN exchange in both.

## 7. Interview questions

**Q: A Linux server process is hung. Walk me through your investigation.**
Confirm with `ss -tlnp` that it still listens and `cat /proc/PID/status` for state and thread
count. `strace -f -p PID` for a few seconds to see whether threads are in `futex`, `recvfrom` or
spinning. `gdb -batch -ex 'thread apply all bt' -p PID` to get every stack without a long stop.
If `futex` everywhere, read the stacks for the lock owner; if CPU-bound, `perf record -p`.

**Q: What is the difference between strace and ltrace?**
strace traces system calls (the kernel boundary) via ptrace; ltrace traces calls into shared
libraries via the PLT. strace tells you what the program asks the OS; ltrace tells you which
library functions it calls, such as `malloc` counts or SSL calls, but misses statically linked or
inlined code.

**Q: How does perf find where time goes, and why do you build with frame pointers?**
It programs a hardware counter or timer to interrupt at a fixed rate and records the instruction
pointer and stack at each interrupt. Aggregating samples gives the time distribution. Stack
capture follows the `rbp` chain, which `-O2` removes; `-fno-omit-frame-pointer` restores it for
about 1% cost, or `--call-graph dwarf` unwinds offline at higher recording cost.

**Q: Valgrind memcheck vs AddressSanitizer?**
ASan is compiled in, roughly 2x slowdown, catches heap, stack and global overflows,
use-after-free, leaks. Memcheck needs no rebuild, is 20-50x slower, misses stack overflows but
tracks uninitialised values bit-precisely and checks syscall arguments. Use ASan during
development and CI; memcheck for third-party binaries and uninitialised-value hunts.

**Q: What does `real 2.0s user 0.1s sys 0.1s` tell you?**
The program spent 1.8 seconds waiting: I/O, sleeping, blocked on locks or the network. CPU
profiling will not help; look at syscalls (`strace -T`), lock contention or the remote side.

**Q: How do you find which process owns a port?**
`ss -tlnp 'sport = :7000'` or `lsof -i :7000`. Both read kernel socket tables and map inodes to
processes through `/proc/*/fd`.

**Q: How does a gdb breakpoint work?**
gdb overwrites the first byte of the instruction with `int3` (0xCC). Hitting it raises SIGTRAP,
the kernel stops the ptraced process, gdb restores the byte and rewinds the PC. To continue it
single-steps the original instruction and re-arms the breakpoint. Hardware watchpoints use CPU
debug registers so they are free until triggered.

## 8. Recap

- gdb for crashes and stacks: `bt`, `thread apply all bt`, `p`, `watch`, `-batch` for snapshots, cores via `coredumpctl`.
- strace shows syscalls with `-f -tt -T -y`; read blocked time, errors (`ENOENT`, `ECONNREFUSED`) and `= 0` EOFs.
- ltrace shows library calls through the PLT; `-c` for counts.
- perf: `stat` for counters, `record -g` + `report --no-children` for hot spots, `perf script` to flame graphs; needs frame pointers.
- valgrind memcheck without rebuild, callgrind for exact instruction counts, massif for heap over time.
- `/proc/PID/{status,fd,maps,smaps_rollup,limits,task}` answers most "what is it doing" questions for free.
- `lsof -i :port`, `lsof +L1`, `ss -tlnp`, `ss -s`, `ss -ti` for files, ports and TCP state.
- tcpdump `-i lo -nn -A port N`: SYN, SYN-ACK, PSH data, FIN; RST means nothing listening.
- `time` splits wall/user/sys; `hyperfine` for honest comparisons; `perf stat -r`.
- Incident order: ss -> /proc -> strace -> gdb -batch -> perf. Keep symbols so every tool can name functions.
