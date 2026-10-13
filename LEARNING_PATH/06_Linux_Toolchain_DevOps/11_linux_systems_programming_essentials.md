# 11. Linux systems programming essentials

**Time:** ~3 hours   **Prerequisites:** 01, 02, 07; C at a professional level

## 1. In plain words

Your program cannot touch the disk, the network or another process directly. Only the kernel can.
When your code needs any of that, it asks the kernel through a **system call** (syscall): a
numbered request like "open this file", "read 4096 bytes from descriptor 5", "create a process".
On Windows you called `CreateFileW`, `ReadFile`, `CreateProcessW`, `WaitForSingleObject`; those
are Win32 API functions in `kernel32.dll` that eventually make `Nt*` system calls. On Linux the
C library (glibc) gives you thin wrappers, `open`, `read`, `fork`, `waitpid`, that are almost
one-to-one with the kernel's syscalls.

This module is the Linux half of what you already know from Win32: files and handles become files
and **file descriptors**, `GetLastError()` becomes `errno`, `CreateProcess` becomes `fork` + `exec`,
console control handlers become **signals**, services become **systemd units**, and the
"out of memory" you have seen as a failed `malloc` becomes the **OOM killer**. Knowing these lets
you read `strace` output, write a server that shuts down cleanly, and run it as a proper service.

Analogy: the kernel is a bank vault and syscalls are the teller window. You fill in a numbered
form (syscall number plus arguments), slide it through, and get back a result or an error code.
glibc is the friendly clerk who fills in the form for you; sometimes the clerk does extra work
first (buffering in `printf`, caching in `malloc`) before going to the window.

## 2. The real thing

### 2.1 Syscalls vs library calls

```
your code       printf("x=%d\n", x)       fopen/fread           malloc(64)
                     |                         |                     |
glibc           formats into a buffer;     FILE* buffer;         carves from an arena;
                write() when full/newline  read() in 4 KiB+      brk()/mmap() only when it grows
                     |                         |                     |
kernel          write(1, buf, n)           read(fd, buf, 4096)   brk / mmap
```

- A **syscall** crosses into the kernel: the CPU switches privilege level (`syscall` instruction
  on x86-64), the kernel does the work, and returns. Cost: on the order of 100 ns for a trivial
  one on modern hardware (typical; more with mitigations for CPU vulnerabilities), plus whatever
  the work costs.
- A **library call** may make zero, one or many syscalls. `printf` usually makes none until its
  buffer flushes. `malloc` usually makes none.
- `man 2 read` documents the syscall wrapper; `man 3 printf` documents the library function.
  Section 2 vs section 3 is the distinction.
- `strace` shows syscalls only; `ltrace` shows library calls (module 07).

Every syscall reports failure the same way: the wrapper returns `-1` (or `MAP_FAILED`, or `NULL`
for some library functions) and sets the thread-local variable `errno` to a positive code
(`ENOENT`, `EINTR`, `EAGAIN`, ...). Some newer interfaces (pthreads) return the error code directly
and do not touch `errno`.

### 2.2 errno discipline

```c
int fd = open(path, O_RDONLY | O_CLOEXEC);
if (fd < 0) {                         // 1. check the RETURN VALUE, never errno alone
    int e = errno;                    // 2. copy errno immediately: the next call may overwrite it
    log_error("open %s: %s", path, strerror(e));
    if (e == ENOENT) ...              // 3. branch on specific codes you can handle
}
```

Rules:
- `errno` is only meaningful right after a call that **reported** failure. Successful calls may
  leave garbage in it.
- It is per-thread (glibc defines `errno` as `(*__errno_location())`), so threads do not clobber
  each other.
- Any call in between, including `printf` and `free`, may change it. Save it first.
- Signal handlers must save and restore it (section 2.6).
- `strerror` is not thread-safe in old glibc; `strerror_r` or `%m` in `printf` are. In C++,
  `std::system_error(errno, std::generic_category(), "open")` carries it as an exception.
- Windows mapping: `errno` is `GetLastError()`, `strerror` is `FormatMessage`, and like
  `GetLastError` it is valid only right after the failing call.

The codes you must recognise on sight:

| errno | Meaning | Typical response |
|---|---|---|
| `EINTR` | a signal interrupted a blocking call | retry the call |
| `EAGAIN` / `EWOULDBLOCK` | non-blocking fd has no data or no space | wait for readiness (epoll) |
| `ENOENT` | no such file or directory | configuration error |
| `EACCES` / `EPERM` | permission denied / operation not permitted | user, mode, capabilities, SELinux/AppArmor |
| `EMFILE` | this process hit its fd limit (`ulimit -n`) | raise the limit; find the leak |
| `ENFILE` | the whole system hit its fd limit | `fs.file-max` |
| `ENOSPC` | disk full (or inodes exhausted) | `df -h`, `df -i` |
| `EPIPE` | wrote to a pipe/socket whose reader closed | close the connection (ignore SIGPIPE) |
| `ECONNRESET` | peer reset the TCP connection | close the connection |
| `EADDRINUSE` | port already bound | another instance; `SO_REUSEADDR` for TIME_WAIT |
| `ENOMEM` | kernel could not allocate | rare from `malloc` on Linux (overcommit); real for `mmap` with limits |

### 2.3 File I/O: open, read, write, fsync, close

```c
int fd = open("data.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
ssize_t n = write(fd, buf, len);     // may write fewer than len bytes: loop
fsync(fd);                           // wait until the device has it
close(fd);                           // can fail too: check it
```

- A **file descriptor** is a small integer indexing the process's table of open files. 0, 1, 2 are
  stdin, stdout, stderr. Sockets, pipes, timers, epoll instances and devices are all fds: "everything
  is a file" means one set of calls (`read`, `write`, `close`, `poll`) works on all of them.
- `open` flags: `O_RDONLY`/`O_WRONLY`/`O_RDWR`, `O_CREAT` (with a mode), `O_TRUNC`, `O_APPEND`
  (every write goes to the end, atomically for each `write` on local filesystems), `O_EXCL` (with
  `O_CREAT`: fail if it exists; the safe way to create lock files), `O_CLOEXEC` (close in the child
  after `exec`; always set it), `O_NONBLOCK`, `O_DIRECT` (bypass the page cache; databases).
- **Short reads and writes are normal.** `read` returns fewer bytes than asked at end of file, on
  pipes and sockets, and when a signal arrives. `read` returning 0 means end of file (or the peer
  closed a socket). Every serious codebase has a `write_all` loop; tcpkv's is `send_all` in
  `src/main.cpp`.
- **`write` is not durable.** It copies into the kernel's page cache and returns. The kernel
  writes dirty pages back later (seconds, typically). If the machine loses power before that, the
  data is gone, even though `write` succeeded. `fsync(fd)` blocks until the data and metadata are
  on stable storage; `fdatasync` skips non-essential metadata (timestamps). Creating a file also
  needs an `fsync` of the **directory** to make the directory entry durable.
- **The atomic-replace pattern** (configs, snapshots): write `file.tmp`, `fsync(tmp)`,
  `rename("file.tmp", "file")`, `fsync(dirfd)`. `rename` within one filesystem is atomic: a reader
  sees the old file or the new one, never half of each.
- Windows mapping: `CreateFileW` = `open`, `ReadFile`/`WriteFile` = `read`/`write`,
  `FlushFileBuffers` = `fsync`, `CloseHandle` = `close`, `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`
  = `rename`, `FILE_FLAG_NO_BUFFERING` = `O_DIRECT`.

### 2.4 mmap

`mmap` maps a file (or anonymous zeroed memory) into your address space. Reading the memory reads
the file; no `read` calls, no copy into a user buffer.

```c
void *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);   // file-backed, read-only
void *a = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);         // 1 MiB of zeroed memory
munmap(p, len);
```

- `MAP_SHARED`: writes go to the file (and other processes mapping it see them); `msync` flushes
  them like `fsync`. `MAP_PRIVATE`: copy-on-write; your writes are private.
- Pages load lazily on first touch (a **page fault**). `madvise(MADV_SEQUENTIAL / MADV_WILLNEED)`
  tunes read-ahead.
- Uses: fast read-only access to large files (search indexes, `.so` loading: the dynamic loader
  mmaps every shared library), shared memory between processes, and big allocations (glibc
  `malloc` uses `mmap` for blocks over 128 KiB by default).
- Pitfalls: an I/O error on a mapped file arrives as **SIGBUS**, not as an error code; a file that
  shrinks under you gives SIGBUS on access past the end; you cannot `mmap` length 0. Databases
  that use `mmap` for their main data (LMDB does; PostgreSQL does not) trade control over write
  ordering for simplicity.
- Windows mapping: `CreateFileMapping` + `MapViewOfFile` = `mmap`; `VirtualAlloc` = anonymous `mmap`.

### 2.5 Processes: fork, exec, wait, pipes

Linux splits "start a program" into two calls:

```
fork()   -> duplicates the calling process. Returns 0 in the child, child's PID in the parent.
            Memory is shared copy-on-write, so it is cheap. File descriptors are duplicated.
exec*()  -> replaces the current program image with a new one. Same PID, same fds (except
            O_CLOEXEC ones). Returns only on failure.
waitpid()-> parent collects the child's exit status. Until then the child is a zombie.
```

```c
int pfd[2];
pipe2(pfd, O_CLOEXEC);                 // pfd[0] read end, pfd[1] write end
pid_t pid = fork();
if (pid == 0) {                         // child
    dup2(pfd[1], STDOUT_FILENO);        // child's stdout now goes into the pipe
    execlp("uname", "uname", "-srm", (char *)NULL);
    _exit(127);                         // exec failed; _exit, not exit (no atexit, no stdio flush)
}
close(pfd[1]);                          // parent: close the write end or read never sees EOF
/* read(pfd[0], ...) until 0 */
int status;
waitpid(pid, &status, 0);
if (WIFEXITED(status))   printf("exit %d\n", WEXITSTATUS(status));
if (WIFSIGNALED(status)) printf("signal %d\n", WTERMSIG(status));
```

Why two calls instead of one `CreateProcess`? Between `fork` and `exec` the child can rearrange
its own environment with ordinary calls: redirect fds with `dup2`, change directory, drop
privileges, set limits. That is how the shell implements `cmd > file 2>&1 | other`. The cost is
that `fork` in a **multithreaded** process is dangerous: only the calling thread exists in the
child, and any mutex another thread held (inside `malloc`, inside your logger) stays locked
forever. Between `fork` and `exec` in a threaded program, call only async-signal-safe functions.
`posix_spawn` (one call, like `CreateProcess`) avoids the problem and is faster for large
processes; glibc implements it with `clone(CLONE_VM | CLONE_VFORK)`.

**Pipes** are one-way kernel buffers (64 KiB by default) between two fds. A reader blocks when
empty; a writer blocks when full; reading after all writers closed gives EOF (0); writing after
all readers closed gives **SIGPIPE** (and `EPIPE` if SIGPIPE is ignored).

**Zombies and orphans.** A child that exited but was not `waitpid`ed is a zombie (`Z` in `ps`): it
holds only a PID and its exit status. A parent that never reaps leaks PIDs. If the parent dies
first, the child is re-parented to PID 1 (or a subreaper), which reaps it. In a container, your
program may be PID 1, which is why the tcpkv image uses `tini` (module 08).

### 2.6 Signals and async-signal safety

A **signal** is an asynchronous notification delivered to a process: the kernel interrupts
whatever a thread was doing and runs your handler on its stack.

| Signal | Number | Default | Typical source |
|---|---|---|---|
| `SIGINT` | 2 | terminate | Ctrl-C |
| `SIGTERM` | 15 | terminate | `kill PID`, `systemctl stop`, `docker stop` |
| `SIGKILL` | 9 | terminate, **cannot be caught** | `kill -9`, OOM killer, stop timeout |
| `SIGSEGV` | 11 | core dump | invalid memory access |
| `SIGABRT` | 6 | core dump | `abort()`, failed `assert`, sanitizers |
| `SIGBUS` | 7 | core dump | I/O error on mmap, misaligned access on some CPUs |
| `SIGPIPE` | 13 | terminate | write to closed pipe/socket |
| `SIGCHLD` | 17 | ignore | a child changed state |
| `SIGHUP` | 1 | terminate | terminal closed; by convention "reload config" for daemons |
| `SIGUSR1/2` | 10/12 | terminate | application-defined (dump stats, rotate logs) |
| `SIGSTOP`/`SIGCONT` | 19/18 | stop/continue, cannot be caught | `kill -STOP`, Ctrl-Z sends `SIGTSTP` |

Install handlers with `sigaction`, not `signal` (whose semantics vary):

```c
struct sigaction sa = {0};
sa.sa_handler = on_signal;
sigemptyset(&sa.sa_mask);         // signals blocked while the handler runs (plus this one)
sa.sa_flags = SA_RESTART;         // auto-restart some interrupted syscalls; omit to get EINTR
sigaction(SIGTERM, &sa, NULL);
```

**Async-signal safety.** The handler can run between any two instructions of your program,
including in the middle of `malloc` while it holds the heap lock, or in the middle of `printf`
while it holds the `FILE` lock. If the handler calls `malloc` or `printf`, it may deadlock or
corrupt the heap. Only functions listed in `man 7 signal-safety` may be called: `write`, `read`,
`_exit`, `kill`, `sigaction`, `signal`-related functions, `open`, `close` and a few dozen more.
Not `printf`, `malloc`, `free`, `syslog`, `pthread_mutex_lock`, `exit`, or anything in C++ that
may allocate or lock (including `std::cout` and `std::string` construction).

Three safe patterns:

1. **Set a flag.** `volatile sig_atomic_t g_stop = 1;` in the handler; the main loop checks it.
   This is what tcpkv's `main.cpp` does. The main loop must wake up to see it: use a poll timeout
   or install the handler without `SA_RESTART` so blocking calls return `EINTR`.
2. **Self-pipe.** The handler `write`s one byte to a non-blocking pipe; the event loop `poll`s the
   read end along with everything else and handles the signal in normal context. `m11_syscalls.c
   signals` does this.
3. **signalfd** (Linux only): block the signals with `sigprocmask` (in every thread: do it before
   creating threads) and read them from an fd in the event loop. No handler at all.

In multithreaded programs, a process-directed signal is delivered to **any** thread that does not
block it. The robust design is: block signals in all threads, and have one thread receive them
via `sigwait` or `signalfd`.

`EINTR`: a blocking call (`read`, `accept`, `poll`, `nanosleep`) interrupted by a handled signal
fails with `EINTR`. Retry it in a loop, or treat it as "check your flags". `SA_RESTART` makes the
kernel restart many of them transparently, but never `poll`, `epoll_wait`, `select` or `nanosleep`.

Windows mapping: `SetConsoleCtrlHandler` handles Ctrl-C and close events on a **new thread**,
which is why Windows handlers can do more. Service stop requests arrive through the SCM handler.
Linux handlers interrupt an existing thread, hence the strict rules.

### 2.7 Timers

| Mechanism | Use |
|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | measuring durations; never jumps when NTP or an admin sets the clock |
| `clock_gettime(CLOCK_REALTIME)` | wall-clock timestamps for logs; can jump backwards |
| `nanosleep` / `std::this_thread::sleep_for` | simple delays |
| `timerfd_create` | a timer that is a file descriptor: put it in poll/epoll beside sockets |
| `poll`/`epoll_wait` timeout argument | the event loop's own "next deadline" |
| `alarm`, `setitimer`, `timer_create` | signal-based timers; avoid in new code (signal-safety rules again) |

Event-driven servers keep their timeouts (idle connections, request deadlines) in a min-heap or a
timer wheel and pass "time until the earliest deadline" to `epoll_wait`. `timerfd` is the simple
version. `std::chrono::steady_clock` is `CLOCK_MONOTONIC` in C++. Windows mapping:
`QueryPerformanceCounter` = `CLOCK_MONOTONIC`; waitable timers = `timerfd`.

### 2.8 epoll preview

tcpkv uses one thread per connection. That stops scaling at a few thousand connections (each
thread has a stack, typically 8 MiB reserved, and context switches cost). The alternative is one
thread watching many sockets and acting only on the ready ones:

```c
int ep = epoll_create1(EPOLL_CLOEXEC);
struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);        // register interest once
for (;;) {
    int n = epoll_wait(ep, events, 64, timeout_ms);  // sleep until something is ready
    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == listen_fd) accept_all_pending();   // accept4(... SOCK_NONBLOCK)
        else                                 handle_readable(events[i].data.fd);
    }
}
```

- `select`/`poll` pass the whole fd list on every call: O(n) per wait. `epoll` keeps the interest
  list in the kernel and returns only ready fds: cost proportional to activity.
- **Level-triggered** (default): reported on every `epoll_wait` while the condition holds. Easy.
  **Edge-triggered** (`EPOLLET`): reported once per change; you must read until `EAGAIN` or you
  will never hear about the leftover data. Faster, easier to get wrong.
- All fds must be **non-blocking**: a blocking `read` in an event loop stalls every client.
- Writes: when `write` returns `EAGAIN` or a short count, keep the rest in a per-connection
  buffer, add `EPOLLOUT`, and continue when writable.
- Windows mapping: I/O Completion Ports are **completion**-based (the kernel does the read and
  tells you it finished); epoll is **readiness**-based (the kernel says you can read now without
  blocking). Linux's completion-based interface is `io_uring`.

`code/m11_epoll_preview.c` is a complete level-triggered echo server. The full event loop with
output buffers, timers and edge-triggered mode is
`08_Networking_Distributed/06_nonblocking_io_epoll_event_loops.md`.

### 2.9 Resource limits: ulimit

Every process has soft and hard limits (`getrlimit`/`setrlimit`), inherited by children.

```bash
ulimit -a                 # all soft limits of this shell
ulimit -n                 # open files: often 1024 soft (Ubuntu default)
ulimit -Hn                # hard limit: the ceiling a non-root process may raise the soft limit to
ulimit -n 65536           # raise for this shell and its children
ulimit -c unlimited       # allow core dumps (05_Memory_Debugging_RCA/06_core_dumps_on_linux.md)
cat /proc/$(pgrep -n tcpkv)/limits    # the limits of a RUNNING process
prlimit --pid PID --nofile=65536:65536  # change them on a live process
```

The one that bites servers is `RLIMIT_NOFILE`. One TCP connection is one fd. With a soft limit of
1024, the 1021st client makes `accept` fail with `EMFILE`. Raise it where the process is started:
`LimitNOFILE=` in the systemd unit, `--ulimit nofile=` for Docker, `/etc/security/limits.conf`
for login sessions (it does **not** apply to systemd services). Other useful ones: `RLIMIT_CORE`
(core size), `RLIMIT_AS` (address space: makes `malloc` return NULL, good for testing out-of-memory
paths), `RLIMIT_NPROC` (processes/threads per user), `RLIMIT_STACK` (main thread stack, 8 MiB
default).

### 2.10 Memory overcommit and the OOM killer

Linux by default **overcommits**: `malloc(1 GiB)` succeeds even if there is not 1 GiB free, because
memory is only allocated when pages are first touched. If the system then runs out of RAM and swap
while processes touch their pages, the kernel cannot fail a `malloc` that already succeeded. It
picks a victim process and sends it **SIGKILL**. That is the OOM killer.

- Victim choice: the process with the highest `oom_score` (`/proc/PID/oom_score`), roughly its
  share of memory, adjusted by `oom_score_adj` (-1000 = never kill, +1000 = kill first). systemd's
  `OOMScoreAdjust=` sets it for a service.
- Evidence: the process vanishes with exit status 137 (128 + 9); `dmesg` or `journalctl -k` shows
  `Out of memory: Killed process 1234 (tcpkv) total-vm:... anon-rss:...`. There is no core dump
  and no log line from your program: SIGKILL cannot be caught.
- **cgroup limits** (Docker `--memory`, systemd `MemoryMax=`, Kubernetes limits) give each group its
  own OOM: the kernel kills inside the group that exceeded its limit, not a random process on the
  host. `docker inspect` then shows `OOMKilled: true` (module 08).
- `vm.overcommit_memory`: 0 heuristic (default), 1 always, 2 strict accounting (then `malloc`
  can really return NULL). Databases sometimes recommend 2.
- Windows mapping: Windows **commits** memory at allocation time against the commit limit (RAM +
  page file), so `VirtualAlloc`/`malloc` fails up front and there is no OOM killer. That is a real
  difference to mention in interviews.

### 2.11 systemd: running a server as a service

systemd is PID 1 on Ubuntu: it starts services at boot, supervises them, restarts them, collects
their output in the journal, and puts each one in a cgroup. A **unit file** describes one service.
The tcpkv unit, `code/tcpkv/deploy/tcpkv.service` (abridged):

```ini
[Unit]
Description=tcpkv TCP key-value server
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=60          # crash-loop guard: more than 5 starts in 60 s -> failed
StartLimitBurst=5

[Service]
Type=exec                         # foreground process; "started" once exec succeeded
ExecStart=/usr/local/bin/tcpkv 7000
User=tcpkv
Group=tcpkv
Restart=on-failure                # restart on crash or non-zero exit, not on clean stop
RestartSec=2
KillSignal=SIGTERM
TimeoutStopSec=30                 # SIGTERM, wait 30 s, then SIGKILL
StandardOutput=journal
StandardError=journal
LimitNOFILE=65536                 # ulimit -n for the service
LimitCORE=infinity
MemoryMax=512M                    # cgroup memory cap: OOM inside this service only
NoNewPrivileges=yes
ProtectSystem=strict              # whole filesystem read-only for this service
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target        # 'enable' hooks it into normal boot
```

The commands:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin tcpkv
sudo cp deploy/tcpkv.service /etc/systemd/system/
sudo systemctl daemon-reload            # after ANY unit file edit
sudo systemctl enable --now tcpkv       # start now and at boot
systemctl status tcpkv                  # active?, PID, memory, CPU, last 10 log lines
journalctl -u tcpkv -f                  # follow logs
journalctl -u tcpkv -b -p warning       # this boot, warnings and worse
journalctl -u tcpkv --since "2026-10-12 03:00" --until "03:15"
systemctl show tcpkv -p NRestarts,ExecMainStatus,Result
sudo systemctl edit tcpkv               # drop-in override without editing the original
systemd-analyze security tcpkv          # exposure score for the sandboxing options
```

Restart policies:

| `Restart=` | Restarts when |
|---|---|
| `no` (default) | never |
| `on-failure` | non-zero exit, killed by a signal (except clean stop signals), timeout, watchdog |
| `on-abnormal` | signal, timeout, watchdog; not on non-zero exit |
| `always` | any exit, even exit 0 (but not after `systemctl stop`) |

`on-failure` is right for a server whose clean exit means "someone asked me to stop". Docker's
`restart: unless-stopped` is the equivalent of `Restart=always` (module 08).

**Logging.** Under systemd, a service writes plain lines to stdout/stderr and journald stores
them with timestamp, unit, PID and priority. No log files, no rotation code, no `syslog()` needed.
Prefix a line with `<3>` to mark it as error priority (`sd-daemon(3)`), or use `sd_journal_send`
for structured fields. tcpkv's `log_line` writing to stderr is exactly right.

**Type=** matters: `simple`/`exec` for programs that stay in the foreground (all modern servers),
`forking` for old-style daemons that double-fork and write a PID file (do not write new ones),
`notify` for programs that call `sd_notify("READY=1")` when they are really ready to serve, which
lets dependent units wait for real readiness. Windows mapping: a systemd unit is a Windows service
registration (`sc create`), `Restart=` is the Recovery tab, journald is the Event Log,
`User=` is "Log on as".

WSL note: Ubuntu 24.04 on WSL runs systemd by default. If `systemctl` reports "System has not been
booted with systemd", put `[boot]` and `systemd=true` in `/etc/wsl.conf` and run `wsl --shutdown`.

### 2.12 strace as your syscall microscope

Module 07 introduced strace. Here it is the tool that makes this module concrete:

```bash
strace -f ./m11_syscalls spawn                  # clone, execve in the child, pipe2, dup2, wait4
strace -e trace=openat,read,write,fsync,close ./m11_syscalls fileio /tmp/a.txt
strace -e trace=%signal,poll,timerfd_settime ./m11_syscalls signals   # then kill -TERM it
strace -c ./m11_syscalls mmap /etc/services     # count: compare with reading the file in a loop
strace -f -e trace=epoll_wait,accept4,read,write ./epoll_echo 7100
strace -f -tt -T -p $(pgrep -n tcpkv)           # what is the live server doing, with timings
```

Reading a line: `openat(AT_FDCWD, "/tmp/a.txt", O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644) = 3` is
the call, its decoded arguments and the result. A failure looks like `= -1 ENOENT (No such file or
directory)`. `<0.000021>` with `-T` is the time spent inside the call. Note that glibc's `open`
is the `openat` syscall, and `fork` is `clone`; the wrapper names and the syscall names differ.

## 3. Code

### 3.1 `code/m11_syscalls.c`

Four demos in one C file, every return value checked:

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -g m11_syscalls.c -o m11_syscalls
./m11_syscalls fileio /tmp/m11.txt     # open/write/fsync/close, read back, ENOENT demo
./m11_syscalls mmap /etc/services      # map a file and count lines without read()
./m11_syscalls spawn                   # pipe2 + fork + dup2 + execlp + waitpid
./m11_syscalls signals                 # self-pipe + timerfd + poll; Ctrl-C or kill -TERM PID
```

The heart of the signals demo:

```c
static int g_sigpipe_w = -1;            /* write end of the self-pipe */

/* write() is async-signal-safe; printf, malloc and mutexes are not.
 * Save and restore errno because the interrupted code may be about to read it. */
static void on_signal(int signo) {
    const int saved = errno;
    const unsigned char b = (unsigned char)signo;
    ssize_t ignored = write(g_sigpipe_w, &b, 1);
    (void)ignored;
    errno = saved;
}

/* main loop: one poll() over the self-pipe and a timerfd */
struct pollfd pfds[2] = {{.fd = sp[0], .events = POLLIN}, {.fd = tfd, .events = POLLIN}};
for (;;) {
    const int pr = poll(pfds, 2, -1);
    if (pr < 0) {
        if (errno == EINTR) continue;   /* the self-pipe will tell us which signal */
        die("poll");
    }
    if (pfds[0].revents & POLLIN) { /* read the signal number; now printf and cleanup are fine */ }
    if (pfds[1].revents & POLLIN) { /* read a uint64_t expiration count from the timerfd */ }
}
```

The file I/O demo shows a detail people get wrong: `printf` output is buffered in user space and
`write(STDOUT_FILENO, ...)` is not, so mixing them reorders output unless you `fflush(stdout)` first.
The same buffering is why a child created by `fork` without a flush prints the parent's pending
output twice, and why the child must leave with `_exit`, not `exit`.

### 3.2 `code/m11_epoll_preview.c`

```bash
gcc -std=c11 -Wall -Wextra -Werror -O2 -g m11_epoll_preview.c -o epoll_echo
./epoll_echo 7100 &
for i in 1 2 3; do (echo "hello $i"; sleep 1) | nc localhost 7100 & done; wait
kill -INT %1
```

The accept path drains every pending connection, because one readiness event can stand for many:

```c
if (fd == lfd) {
    for (;;) {
        const int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   /* none left */
            if (errno == EINTR) continue;
            perror("accept4");                                   /* EMFILE: log, keep serving */
            break;
        }
        struct epoll_event cev = { .events = EPOLLIN | EPOLLRDHUP, .data.fd = cfd };
        epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
    }
}
```

### 3.3 `code/tcpkv/src/main.cpp` revisited

Read tcpkv's `main` now with this module's vocabulary: `signal(SIGPIPE, SIG_IGN)` plus
`MSG_NOSIGNAL` (get `EPIPE`, do not die); `sigaction` **without** `SA_RESTART` for SIGTERM/SIGINT
and a `volatile std::sig_atomic_t` flag (pattern 1); `poll` with a 500 ms timeout so the flag is
noticed even if the signal went to another thread; `SOCK_CLOEXEC` on every socket; `accept4`
failures such as `EMFILE` logged without exiting; a bounded drain on shutdown so
`TimeoutStopSec=30` and `docker stop`'s 10 seconds are never reached.

### 3.4 `code/tcpkv/deploy/tcpkv.service`

The full unit with comments. CMake installs it (`install(FILES deploy/tcpkv.service ...)`,
module 04). Try it on WSL:

```bash
cmake --preset release && cmake --build --preset release
sudo cmake --install build/release --prefix /usr/local --strip
sudo useradd --system --no-create-home --shell /usr/sbin/nologin tcpkv
sudo cp deploy/tcpkv.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl enable --now tcpkv
printf 'PING\n' | nc -N localhost 7000                 # PONG
sudo kill -SEGV $(systemctl show -p MainPID --value tcpkv)   # simulate a crash
sleep 3; systemctl status tcpkv                          # restarted; NRestarts=1
journalctl -u tcpkv -n 20
```

## 4. Under the hood (senior-level)

**What a syscall instruction does.** On x86-64, glibc's `read` wrapper puts the syscall number
(0 for `read`) in `rax` and arguments in `rdi, rsi, rdx, r10, r8, r9`, then executes `syscall`.
The CPU switches to ring 0 and jumps to the kernel's entry point, which saves registers, looks up
`sys_call_table[rax]`, runs `ksys_read`, and returns with the result in `rax`. A negative value
between -4095 and -1 is an error; the wrapper negates it into `errno` and returns -1. Some calls
(`clock_gettime`, `gettimeofday`, `getcpu`) never enter the kernel: the **vDSO** is a small shared
library the kernel maps into every process that reads the time from a shared page. That is why
`strace` does not show `clock_gettime` and why it costs tens of nanoseconds (typical).

**The fd table.** Each process has a table of pointers to **open file descriptions** (struct file
in the kernel), which hold the file offset and status flags and point to the inode. `dup2` and
`fork` copy pointers, so parent and child **share** the offset: if both write to an inherited fd
without `O_APPEND`, their writes interleave at one shared position. `open` twice creates two
descriptions with independent offsets. `O_CLOEXEC` is a flag on the fd-table slot, not the
description. Closing the last fd referring to a description releases it; for a socket, that
sends FIN.

**Page cache and writeback.** `read` and `write` go through the page cache: 4 KiB pages of file
content kept in otherwise free RAM. `write` marks pages dirty; the flusher threads write them
after `dirty_expire_centisecs` (30 s default) or when dirty memory exceeds `dirty_background_ratio`
(10 % default); a writer that produces data faster than the disk absorbs it is throttled in
`write` itself. `fsync` forces writeback of that file and issues a cache-flush command to the
device. **fsyncgate (2018):** PostgreSQL discovered that after a writeback error, Linux could mark
the pages clean and a retried `fsync` returned success while the data was lost. The fix in
PostgreSQL was to treat an `fsync` failure as fatal (PANIC and recover from the WAL). Lesson: an
`fsync` error is not retryable.

**fork and copy-on-write.** `fork` copies the page tables, not the pages, and marks every writable
page read-only in both processes. The first write by either side triggers a page fault; the
kernel copies that one page and makes it writable. A 10 GiB process can fork in milliseconds, but
copying its page tables is still proportional to its size, and a parent that keeps writing after
`fork` doubles its memory gradually. Redis's background save relies on exactly this behaviour.

**Signal delivery.** When a signal is sent, the kernel sets a bit in the target's pending set.
On the next return from kernel to user mode (after a syscall, an interrupt or a reschedule), it
checks for pending unblocked signals and, for a caught signal, builds a signal frame on the user
stack (saved registers, `siginfo`), points the instruction pointer at the handler and returns to
user mode. When the handler returns, it lands in a small trampoline that calls `rt_sigreturn`,
which restores the saved registers. If the thread was blocked in an interruptible syscall, the
syscall is aborted with `EINTR` or restarted per `SA_RESTART`. Standard signals do not queue: two
SIGCHLDs before you handle one become one, which is why a SIGCHLD handler must loop
`waitpid(-1, ..., WNOHANG)` until it returns 0.

**epoll internals.** `epoll_ctl(ADD)` inserts the fd into a red-black tree in the epoll instance
and registers a callback on the fd's wait queue. When the socket receives data, the network stack
wakes its wait queue; the callback appends the item to the epoll **ready list** and wakes threads
in `epoll_wait`. `epoll_wait` copies out ready items; for level-triggered items it puts them back
on the list if still ready. That is why `epoll_wait` is O(ready) and not O(registered). The
interest is registered on the **open file description**, not the fd number: closing an fd whose
description is still referenced elsewhere (a `dup` or a forked child) leaves the registration
alive and produces events for an fd you no longer have, a classic bug.

**systemd and cgroups.** Every unit gets its own cgroup under `/sys/fs/cgroup/system.slice/
tcpkv.service/`. `MemoryMax=` writes `memory.max`, `TasksMax=` writes `pids.max`, CPU options
write `cpu.max`. Because systemd tracks the service by cgroup and not by PID, it finds and kills
every process the service forked on stop, which PID files never could. `KillMode=mixed` sends
SIGTERM to the main process and SIGKILL to the rest after the timeout. The journal reads the
service's stdout/stderr from a socket systemd connected to fds 1 and 2 before `exec`, so it knows
which unit each line came from. `systemd-cgls` shows the tree; `systemd-cgtop` shows usage.

## 5. In production

- **The fd leak.** The server works for days, then every new connection fails and logs show
  `accept: Too many open files`. `ls /proc/PID/fd | wc -l` sits at exactly the `LimitNOFILE` value;
  `ls -l /proc/PID/fd` shows thousands of sockets in CLOSE_WAIT (`ss -tanp`): the code path for one
  error forgot to `close`. Raising the limit buys time; RAII (a C++ `unique_fd` class, or
  `std::unique_ptr` with a custom deleter) fixes it. Your C server's `goto cleanup` discipline is
  the C version of the same thing.
- **Missing fsync.** A service acknowledged writes after `write()` only. A host reboot lost the
  last 30 seconds of "committed" data. In the banking context this is the difference between the
  WAL being durable and not; it is why PostgreSQL's `synchronous_commit` and `fsync` settings exist.
  Ask "where is the fsync?" in every design review of something that persists data.
- **Logging from a signal handler.** A crash handler that calls `printf` or the logging library to
  print a stack trace works in testing and deadlocks in production when the crash happens inside
  `malloc`. Write a minimal message with `write(2, ...)`, then let the core dump carry the details
  (05_Memory_Debugging_RCA/06_core_dumps_on_linux.md).
- **Fork in a threaded server.** Calling `system()` or `popen()` from a multithreaded server to run
  a helper script occasionally hangs the child forever (a lock held by another thread at fork time),
  and doubles RSS momentarily for large processes. Use `posix_spawn`, or a small helper process
  started early, before any threads exist.
- **Silent OOM kills.** The service restarts every few hours with no error in its logs.
  `systemctl status` shows `status=9/KILL`, `journalctl -k | grep -i oom` shows the kill, and
  `systemctl show -p MemoryMax` shows the cap. Find the growth with heap profiling
  (05_Memory_Debugging_RCA/09_leaks_and_memory_growth.md), not by raising the limit.
- **SIGPIPE killed the server.** A client disconnects mid-response, the next `send` raises SIGPIPE,
  the process dies with status 141 and no log line. Ignore SIGPIPE at startup and use `MSG_NOSIGNAL`;
  every network server on Linux does this. On Windows the equivalent failure just returns
  `WSAECONNRESET`, so this one surprises Windows developers.
- **Restart loops.** `Restart=always` with `RestartSec=100ms` and a configuration error produces
  thousands of restarts per minute, floods the journal, and hides the first error. Use
  `on-failure`, a `RestartSec` of seconds, and `StartLimitBurst` so the unit ends up `failed`
  and alerts fire.
- **3 AM:** after a deploy, `systemctl restart tcpkv` takes exactly 30 seconds and then the service
  starts. The new code blocks SIGTERM by accident (a library called `sigprocmask` in its init) so
  the stop always runs into `TimeoutStopSec` and SIGKILL. `grep Sig /proc/PID/status` shows
  `SigBlk` with bit 15 set (decode the hex mask). Every restart was dropping in-flight requests.

## 6. Try it

1. **(easy)** Run each `m11_syscalls` demo under `strace -f`. Done when you can name the syscall
   behind the glibc wrappers `open` (`openat`), `fork` (`clone`) and `waitpid` (`wait4`), and
   explain why `clock_gettime` does not appear in any trace.
2. **(easy)** Run `ulimit -n 64` in a shell, start `./build/debug/tcpkv 7000` from it, and open
   100 connections with a small Python loop. Done when the server log shows `accept: Too many open
   files`, the server keeps serving existing clients, and `cat /proc/PID/limits` confirms the limit.
3. **(medium)** Remove the `fsync` from the fileio demo and measure both versions writing 10,000
   small records with `hyperfine`. Then write the atomic-replace version (`tmp`, `fsync`, `rename`,
   `fsync` of the directory). Done when you can state the cost of `fsync` on your disk and explain
   why the directory needs its own `fsync`.
4. **(medium)** Write `mini_shell.c` that reads a command line, splits on spaces, supports one
   `|` and one `> file`, and runs it with `fork`, `pipe`, `dup2`, `execvp` and `waitpid`, printing
   the exit status or signal. Done when `ls -l | wc -l` and `echo hi > /tmp/x` work, no zombies
   remain (`ps -o stat`), and `strace -f` shows the expected `dup2` calls.
5. **(medium)** Install tcpkv as a systemd service on WSL with the unit from `deploy/`. Kill it with
   SIGSEGV three times, then six times within a minute. Done when you can show `NRestarts` from
   `systemctl show`, the unit in `failed` state after the burst limit, and the relevant journal
   lines, and you can explain `StartLimitBurst`.
6. **(hard)** Turn `m11_epoll_preview.c` into a tcpkv server: non-blocking sockets, a
   per-connection input buffer split into lines, a per-connection output buffer flushed on
   `EPOLLOUT`, the existing C++ `parse_command` and `Store`, and a `signalfd` for SIGTERM. Done
   when `scripts/tcpkv_client.py --selftest` and `scripts/load_test.sh 7000 64 5000` pass against
   it, and `strace -c` shows one thread doing all the work.

## 7. Interview questions

**Q: What is the difference between a system call and a library function?**
A system call transfers control to the kernel through a CPU instruction to do something only the
kernel can do (I/O, process creation, memory mapping). A library function runs in user space and
may make zero or more syscalls: `printf` buffers and calls `write` occasionally, `malloc` calls
`brk` or `mmap` only when its arenas grow. `strace` shows the former, `ltrace` the latter.

**Q: Does `write()` returning success mean the data is on disk?**
No. It means the kernel copied it into the page cache. It reaches the device later through
writeback. For durability call `fsync` (or `fdatasync`) on the file, and `fsync` the directory
after creating or renaming files. Treat an `fsync` error as fatal for that data, because the
kernel may have dropped the dirty pages.

**Q: What can you safely do in a signal handler?**
Only async-signal-safe operations: set a `volatile sig_atomic_t` flag, `write` to a pipe or
eventfd, `_exit`. No `malloc`, `printf`, locks or C++ that might allocate, because the handler may
have interrupted the same code holding the same lock. Save and restore `errno`. Do the real work
in normal context via a flag, the self-pipe trick, or `signalfd`.

**Q: Explain fork and exec. Why are they separate?**
`fork` duplicates the process with copy-on-write memory and duplicated fds; `exec` replaces the
program image in the current process. Separating them lets the child set up its environment
(redirect fds, change directory, drop privileges) with ordinary calls before running the new
program, which is how shells implement redirection and pipes. In multithreaded programs only
async-signal-safe calls are allowed between them; `posix_spawn` is the safer single call.

**Q: What is EINTR and how do you handle it?**
A blocking syscall interrupted by a caught signal returns -1 with `errno == EINTR`. Retry the
call in a loop, or use the interruption to check a shutdown flag. `SA_RESTART` restarts many
calls automatically but never `poll`, `epoll_wait` or `select`.

**Q: Why is epoll more scalable than select or poll?**
`select` and `poll` pass the full fd set on every call and the kernel scans it, so each wait is
O(n) in watched fds. epoll registers interest once in a kernel object and maintains a ready list
filled by callbacks, so `epoll_wait` costs O(ready). Level-triggered reports while data remains;
edge-triggered reports changes only and requires draining to `EAGAIN`.

**Q: A process disappeared with exit code 137 and no logs. What happened?**
It received SIGKILL (128 + 9), most often from the OOM killer (system or cgroup limit) or a stop
timeout escalating from SIGTERM. Check `journalctl -k` or `dmesg` for "Out of memory: Killed
process", `docker inspect` for `OOMKilled`, or `systemctl status` for the stop timeout. SIGKILL
cannot be caught, so the program never logs it.

**Q: How do you run a C++ server as a service on Linux?**
A systemd unit: `ExecStart` in the foreground, a dedicated `User=`, `Restart=on-failure` with a
`RestartSec` and start-limit burst, `TimeoutStopSec` matching the graceful-drain time,
`LimitNOFILE` for connections, `MemoryMax` for a cgroup cap, sandboxing options, logs to
stdout/stderr collected by journald and read with `journalctl -u`. The server handles SIGTERM by
draining and exiting 0.

## 8. Recap

- Syscalls enter the kernel (`man 2`); library calls may or may not make syscalls (`man 3`). `strace` sees syscalls.
- Check the return value, then copy `errno` immediately; know EINTR, EAGAIN, EMFILE, EPIPE, ENOSPC on sight.
- `read`/`write` can be short: loop. `write` is not durable: `fsync` the file and the directory; `rename` is atomic.
- `mmap` maps files or anonymous memory; lazy page faults; I/O errors arrive as SIGBUS.
- `fork` (copy-on-write) + `exec` + `waitpid`; close unused pipe ends; `_exit` in the child; `posix_spawn` in threaded programs.
- Signal handlers: only async-signal-safe calls; flag, self-pipe or `signalfd`; save `errno`; ignore SIGPIPE.
- Timers: `CLOCK_MONOTONIC` for durations, `timerfd` or the epoll timeout for event loops.
- epoll: register once, wait returns ready fds; non-blocking fds; level vs edge triggered.
- `ulimit -n` caps connections; set `LimitNOFILE` in the unit; `/proc/PID/limits` shows the live values.
- Overcommit plus OOM killer: SIGKILL, exit 137, evidence in `journalctl -k`; cgroup limits contain it.
- systemd unit: `Type=exec`, `User=`, `Restart=on-failure`, `TimeoutStopSec`, `LimitNOFILE`, `MemoryMax`, journald logs.
