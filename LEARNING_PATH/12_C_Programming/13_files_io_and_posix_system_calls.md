# 13. Files, I/O and POSIX system calls

**Time:** ~3 hours   **Prerequisites:** modules 05 (strings), 06 (structs and layout), 10; 06_Linux_Toolchain_DevOps/11_linux_systems_programming_essentials.md (sections 2.1 to 2.4) is a useful companion

## 1. In plain words

There are two layers for doing I/O in C on Linux, and most I/O bugs come from not knowing which
layer you are in.

- **stdio** (`FILE *`, `fopen`, `fprintf`, `fread`) is the portable C library layer. It keeps a
  **buffer** in your process and only calls the kernel when the buffer fills, when you flush, or
  at certain characters (a newline, for a terminal).
- **System calls** (`open`, `read`, `write`, `close`) work on **file descriptors**, small
  integers that index the kernel's table of open files. Every call goes to the kernel, and the
  kernel is allowed to do **less** than you asked: `read` may return fewer bytes than requested,
  `write` may write fewer, and either may be interrupted by a signal.

Below both layers, the kernel has its own buffer (the page cache). When `write` returns, your data
is in kernel memory, **not on the disk**. A power cut can still lose it.

A shopping analogy. stdio is a basket: you collect items and go to the till once. `write()` is
paying at the till for each item separately. And the till's receipt (`write` returned) is not the
bank statement (`fsync` returned): only the statement proves the money really moved.

This module shows each of these facts with a program you can run, then covers binary file formats,
`mmap`, a minimal TCP server and client in plain C, and the Win32 equivalents.

## 2. The real thing

### 2.1 The layers

```text
your code      fprintf(f, ...)                  write(fd, buf, n)
                    |                                  |
stdio buffer   [ in your process, ~4 KiB ]             |
                    | fflush / buffer full / '\n'      |
                    v                                  v
system call    write(fd, ...)  ----------------------> kernel
                                                       |
page cache     [ dirty pages in kernel memory ]  <-----+
                    | writeback (seconds later) or fsync
                    v
disk           [ durable ]
```

`fileno(f)` gives the descriptor under a `FILE *`; `fdopen(fd, mode)` wraps a descriptor in a
`FILE *`. Mixing the two layers on the same file without `fflush` reorders output (section 3.1).

### 2.2 stdio buffering

| Mode | Set with | Flushes when | Default for |
|------|----------|--------------|-------------|
| Full (`_IOFBF`) | `setvbuf(f, NULL, _IOFBF, size)` | buffer full, `fflush`, `fclose`, normal `exit` | files, pipes, and stdout when it is not a terminal |
| Line (`_IOLBF`) | `setvbuf(f, NULL, _IOLBF, size)` | also at every `'\n'` | stdout when it is a terminal |
| None (`_IONBF`) | `setvbuf(f, NULL, _IONBF, 0)` | every call | stderr |

Consequences you will meet:

- `setvbuf` must be called **before** any other I/O on the stream.
- `printf` debugging "loses" the last lines when a program crashes with output redirected to a
  file: stdout was fully buffered and the buffer died with the process. Write diagnostics to
  stderr, or `fflush(stdout)`.
- `fclose` writes the last buffer, so **`fclose` can fail** (disk full, quota, network
  filesystem). For an output file, check its return value.
- `fork()` copies the process including unflushed stdio buffers; both copies write them later, so
  the output appears twice. `fflush(NULL)` before `fork`, and use `_exit` in a child that does not
  `exec`.
- `exit()` flushes stdio; `_exit()` and a crash do not.

### 2.3 `open`, `read`, `write`, `close`

```c
int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
```

Flags worth knowing: `O_CREAT` with a mode (filtered by the process `umask`), `O_TRUNC`,
`O_EXCL` (fail if it exists: safe creation of lock or temp files), `O_APPEND` (every write goes
to the current end of file atomically with respect to other writers: what log files need when
several processes write), `O_CLOEXEC` (close on `exec`, so child processes do not inherit the
descriptor), `O_NONBLOCK` (calls fail with `EAGAIN` instead of waiting).

**Return values that do less than you asked.** `read` returns the number of bytes read (possibly
fewer than requested), `0` at end of file, or `-1` with `errno`. `write` returns the number
written (possibly fewer), or `-1`. Short transfers are normal for pipes, sockets and terminals,
and happen on regular files at EOF, when a signal arrives, or when the disk fills. So every read
or write of a known amount is a loop:

```c
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;                /* interrupted before any data: just retry */
            return -1;
        }
        if (r == 0)
            break;                       /* EOF: the other side closed */
        done += (size_t)r;
    }
    return (ssize_t)done;
}
```

**EINTR.** If a signal handler runs while a thread is blocked in a slow system call (reading a
pipe, socket or terminal, `wait`, `accept`), the call fails with `EINTR` unless the handler was
installed with `SA_RESTART`, in which case most calls restart automatically. Some never restart
regardless (`select`, `poll`, `epoll_wait`, `nanosleep`). Library code cannot know how the
application installed its handlers, so it always retries on `EINTR`.

### 2.4 Durability: `write` is not "saved"

`write` returns when the data is in the page cache. The kernel writes dirty pages to disk later
(by default within about 30 seconds on Linux, controlled by `vm.dirty_expire_centisecs`). To
make data survive a power cut:

- `fsync(fd)` flushes the file's data **and metadata** (size, timestamps) to stable storage and
  asks the drive to flush its own cache. `fdatasync(fd)` skips metadata that is not needed to read
  the data back.
- Opening with `O_SYNC` or `O_DSYNC` makes every `write` behave like write plus sync.
- **Replacing a file safely** is four steps: write a temp file in the same directory, `fsync` it,
  `rename` it over the old name (atomic on POSIX within one filesystem), then `fsync` the
  **directory** so the rename itself is durable. After a crash you see the old file or the new
  one, never a mix. That is `durable_replace` in `m13_robust_io.c`.
- **A failed `fsync` is not retryable.** In 2018 PostgreSQL developers found that after `fsync`
  reports an error on Linux, the kernel may already have dropped the dirty pages, and a second
  `fsync` can then succeed even though the data never reached disk ("fsyncgate"). PostgreSQL now
  treats an `fsync` failure as fatal and recovers from its write-ahead log. The lesson for your
  code: on `fsync` failure, assume the data is lost and fail loudly.

Databases are built on exactly these primitives: see 07_Databases_Internals/04_wal_and_crash_recovery.md.

### 2.5 Binary file formats

`fwrite(&record, sizeof record, 1, f)` writes the in-memory bytes of a struct, which depend on
the compiler (padding), the CPU (byte order) and the platform (`sizeof(long)` is 4 on Windows and
8 on 64-bit Linux). A file written by the Windows server and read by a Linux tool is then garbage,
silently. A format meant to last is encoded field by field:

- **Fixed-width types** (`uint32_t`, `int64_t`) and **one documented byte order**. Shifts are
  endian-independent: `p[0] = v; p[1] = v >> 8; ...` writes little-endian on any CPU.
- **A magic number** at offset 0 (so a wrong file is detected at once) and a **version**.
- **A header-size field**, so a newer writer can append header fields that an older reader skips.
- **A checksum** (CRC-32 here) over the payload, so corruption and truncation are detected rather
  than decoded.
- **Length-prefixed** variable fields, and a decoder that checks **every** length against the
  bytes actually present. The file is input; input can be truncated, corrupted or hostile.

This is the same discipline as a network protocol (08_Networking_Distributed/04_framing_and_protocol_design.md).

### 2.6 `mmap`

`mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0)` maps a file into your address space. Reading the
memory triggers page faults that load pages from the page cache; there is no `read` call and no
copy into a user buffer.

- **Good for**: large files read randomly, read-only data shared between processes, and formats
  you can parse in place.
- **`MAP_SHARED` + `PROT_WRITE`**: writes go to the page cache and other mappers see them;
  `msync(addr, len, MS_SYNC)` is the `fsync` of a mapping.
- **Pitfalls**: if another process truncates the file, touching the missing pages raises
  **SIGBUS**, which kills the process unless handled; I/O errors also arrive as signals instead of
  return codes; and the mapping stays valid after `close(fd)` until `munmap`.

On Windows the same idea is `CreateFileMappingW` plus `MapViewOfFile`.

### 2.7 A TCP echo server and client in C

The call sequence, in plain C (the C++ RAII version and the details are in
08_Networking_Distributed/03_sockets_api_blocking_server.md):

```text
server: getaddrinfo -> socket -> setsockopt(SO_REUSEADDR) -> bind -> listen -> accept (loop) -> read/write -> close
client: getaddrinfo -> socket -> connect -> write -> shutdown(SHUT_WR) -> read until 0 -> close
```

- `getaddrinfo` handles IPv4 and IPv6 and name resolution; its errors come back as its return
  value and are printed with `gai_strerror`, not `errno`.
- `SO_REUSEADDR` lets a restarted server bind while old connections sit in `TIME_WAIT`.
- `shutdown(fd, SHUT_WR)` sends FIN ("I have finished sending") while still allowing reads. The
  server sees `read` return 0 and knows the request is complete.
- Writing to a socket whose peer has gone raises **SIGPIPE**, whose default action kills the
  process. Servers set `signal(SIGPIPE, SIG_IGN)` (or pass `MSG_NOSIGNAL` to `send`) and handle
  `EPIPE`.
- A TCP stream has no message boundaries: one `write` of 32 KiB may arrive as several `read`s,
  and two `write`s may arrive in one `read`. That is why both sides loop.

### 2.8 Win32 equivalents

| POSIX / C | Win32 | Notes |
|-----------|-------|-------|
| `open` | `CreateFileW` | returns `INVALID_HANDLE_VALUE` on failure; share modes decide whether others may open the file |
| `read` / `write` | `ReadFile` / `WriteFile` | byte counts come back through a `DWORD *` out-parameter; also partial for pipes and sockets |
| `close` | `CloseHandle` | sockets use `closesocket` instead |
| `fsync` | `FlushFileBuffers` | also flushes the drive cache |
| `rename` (atomic replace) | `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)` or `ReplaceFileW` | renaming over a file that someone has open usually fails on Windows |
| `mmap` | `CreateFileMappingW` + `MapViewOfFile` | `UnmapViewOfFile` to release |
| `errno` / `strerror` | `GetLastError` / `FormatMessageW` | sockets: `WSAGetLastError` |
| sockets setup | `WSAStartup(MAKEWORD(2, 2), &wsa)` once per process, `WSACleanup` at exit | without `WSAStartup`, every socket call fails with `WSANOTINITIALISED` |
| `socket`, `bind`, `listen`, `accept`, `connect` | same names in Winsock2 | `SOCKET` is an unsigned handle: test against `INVALID_SOCKET`, not `< 0` |
| `read` / `write` on a socket | `recv` / `send` | Winsock sockets are not file descriptors |
| `fopen(path, "w")` | `fopen(path, "wb")` | **text mode translates `\n` to `\r\n` on Windows**: always use `"rb"`/`"wb"` for binary data |
| `SIGPIPE` | none | Winsock returns `WSAECONNRESET` or `WSAECONNABORTED` instead |

## 3. Code

### 3.1 `m13_stdio_buffering.c`: watch the buffer with `stat`

The program checks the file size on disk after each step. Real output:

```text
full buffering : after fputs 0 bytes on disk, after fflush 6
full buffering : 5006 bytes written, 4102 on disk (st_blksize = 4096)
line buffering : before newline 0 bytes, after newline 4
no buffering   : after fputs("abc") 3 bytes
mixing layers  : wrote A then B, file contains "BA"
fork with pending : file contains "xx"
fork after fflush  : file contains "x"
all checks passed
```

The second line: glibc sized the buffer from the filesystem block size (4096), flushed one full
buffer (4096 bytes on top of the 6 already flushed) and kept the remaining 904 bytes in memory.
`strace -e trace=write ./m13_stdio_buffering` shows the system calls behind the first five lines:

```text
write(3, "hello\n", 6)                  = 6
write(3, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"..., 4096) = 4096
write(3, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"..., 904) = 904
write(3, "abc\n", 4)                    = 4
write(3, "abc", 3)                      = 3
write(3, "B", 1)                        = 1
write(3, "A", 1)                        = 1
```

5000 `fputc` calls became two system calls. And "A" was written by the program before "B", but
reached the kernel after it, because it waited in the stdio buffer while `write(fileno(f), "B", 1)`
went straight through.

The fork case:

```c
f = open_fresh(-1);
fputs("x", f);                   /* sits in the user-space buffer */
fflush(stdout);
pid_t pid = fork();
if (pid == 0)
    exit(0);                     /* exit() flushes the child's copy of the buffer */
waitpid(pid, &status, 0);
fclose(f);                       /* the parent flushes its own copy: "xx" */
```

### 3.2 `m13_robust_io.c`: partial reads, EINTR and durable replace

A child process writes a 46-byte message into a pipe in five pieces, 60 ms apart. Meanwhile the
parent has a 20 ms interval timer whose `SIGALRM` handler was installed **without**
`SA_RESTART`, so the blocking `read` keeps failing with `EINTR`:

```c
struct sigaction sa;
memset(&sa, 0, sizeof sa);
sa.sa_handler = on_alarm;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;                    /* no SA_RESTART: blocking read() fails with EINTR */
sigaction(SIGALRM, &sa, NULL);
struct itimerval every20ms = { { 0, 20000 }, { 0, 20000 } };
setitimer(ITIMER_REAL, &every20ms, NULL);

ssize_t got = read_full(p[0], buf, len);
```

Real output:

```text
read_full: 46 bytes intact; short reads=4 or more, EINTR retries=1 or more
durable_replace: new contents visible, no temp file left
all checks passed
```

A naive single `read` would have returned the first 10 bytes, or `-1` with `EINTR`, and a naive
caller would have treated either as the whole message or as a fatal error.

### 3.3 `m13_binary_format_mmap.c`: an explicit format, read back through `mmap`

```c
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int64_t u64_to_i64(uint64_t u)      /* no implementation-defined conversion */
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(UINT64_MAX - u) - 1;
}
```

Real output, with the header decoded by hand underneath:

```text
file is 93 bytes; first 48:
00000000  41 43 43 54 01 00 10 00 03 00 00 00 96 a8 ce 98
00000010  e9 03 00 00 00 00 00 00 40 78 7d 01 00 00 00 00
00000020  08 41 73 68 61 20 52 61 6f ea 03 00 00 00 00 00
  id=1001 balance=25000000 name=Asha Rao
  id=1002 balance=-150 name=Bilal Khan
  id=1003 balance=9223372036854775807 name=Treasury
corrupt, truncated, wrong-magic and wrong-version inputs all rejected
all checks passed
```

```text
41 43 43 54   "ACCT"                     magic
01 00         version 1                  little-endian
10 00         header size 16
03 00 00 00   3 records
96 a8 ce 98   CRC-32 of the payload      = 0x98cea896
e9 03 00 00 00 00 00 00   id 1001        (0x3e9)
40 78 7d 01 00 00 00 00   25000000 paise (0x017d7840)
08            name length 8, then "Asha Rao"
```

The test flips one bit, truncates the file, breaks the magic and bumps the version, and each is
rejected with a specific code instead of being decoded into garbage.

### 3.4 `m13_tcp_echo.c`: server and client in plain C

```c
static long echo_connection(int cfd)
{
    char buf[4096];
    long total = 0;
    for (;;) {
        ssize_t n = read(cfd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0)
            return total;                /* orderly shutdown from the client */
        if (write_full(cfd, buf, (size_t)n) != 0)
            return -1;
        total += n;
    }
}
```

With no arguments the program binds `127.0.0.1` port 0 (the kernel picks a free port), forks a
server child and runs two round trips as the client. Real output (the port changes each run):

```text
echo of "PING ACC-1001": 13 bytes back on port 35189
echo of 32768 bytes: intact
all checks passed
```

It also runs as a real server and client in two terminals:
`./m13_tcp_echo server 9000` and `./m13_tcp_echo client 127.0.0.1 9000 "hello"`.

## 4. Under the hood (senior-level)

**What a `read` costs.** The `syscall` instruction switches to kernel mode; the kernel looks up
the descriptor in the process's file table, goes through the VFS to the filesystem, finds the
pages in the page cache (or starts disk I/O and sleeps), and copies the bytes to your buffer with
`copy_to_user`. A system call costs on the order of 100 ns or more on current hardware (typical;
more with mitigations for speculative-execution attacks). That fixed cost is why stdio buffers:
5000 one-byte writes become two `write` calls.

**Where the stdio buffer comes from.** glibc allocates it on the first I/O, sized by `st_blksize`
from `fstat` (4096 here), which is why the strace shows 4096-byte writes. `setvbuf` after the
first I/O is too late because the buffer already exists.

**Writeback.** Dirty pages are written by kernel flusher threads when they are older than
`vm.dirty_expire_centisecs` (3000, that is 30 s, by default) or when dirty memory exceeds
`vm.dirty_background_ratio`; past `vm.dirty_ratio`, writers are throttled inside `write`. `fsync`
writes the file's dirty pages, waits for them, commits the filesystem journal for its metadata,
and issues a cache-flush command to the device. On an SSD that is commonly hundreds of
microseconds to a few milliseconds (typical; it depends heavily on the device). This cost is why
databases batch commits ("group commit").

**`rename` atomicity.** On ext4, XFS and other POSIX filesystems, `rename` replaces the directory
entry atomically: a concurrent reader opens either the old inode or the new one. Durability of the
rename is a separate question, answered by `fsync` on the directory. (ext4 has a heuristic,
`auto_da_alloc`, that forces data out before a rename-over-existing, which hides missing `fsync`
calls on ext4 and not elsewhere: do not depend on it.)

**EINTR mechanics.** A thread blocked in an interruptible sleep receives a signal; the kernel
interrupts the wait and arranges to run the handler on the way back to user space. If no data has
been transferred, the system call returns `-ERESTARTSYS` internally, which becomes either an
automatic restart (`SA_RESTART`) or `EINTR`. If some data was transferred, the call returns the
partial count instead, which is one of the ways a short read happens.

**`mmap` faults.** The first touch of each page is a page fault: the kernel finds the page in the
page cache (minor fault) or reads it from disk (major fault) and maps it. Sequential access
triggers readahead. For a file you read once from start to end, `read` into a reused buffer is
often as fast and has simpler error handling; `mmap` shines for random access and sharing.

## 5. In production

- **The last log lines before a crash are missing.** The log was written with `fprintf` to a fully
  buffered `FILE *` and the crash destroyed the buffer. Make the logger line-buffered or
  unbuffered, or have it format into a local buffer and call `write` once per line (which is also
  atomic with `O_APPEND` for lines up to `PIPE_BUF` on pipes, and in practice for small writes to
  local files). Do not call `fflush` from a crash handler: stdio is not async-signal-safe.
- **Disk full.** `ENOSPC` can come from `write`, from `fsync`, or from `close` (network
  filesystems report deferred errors at close). A server that ignores these keeps "writing" an
  audit log that is not there. Check all three and alert.
- **Descriptor leaks.** A descriptor not closed on an error path (module 10) leaks one fd per bad
  request until `accept` or `open` fails with `EMFILE`. Watch `ls /proc/<pid>/fd | wc -l` or
  `lsof -p <pid>`, and open with `O_CLOEXEC` so child processes do not inherit them.
- **Binary files on Windows.** `fopen(path, "w")` in text mode turns every `0x0A` byte into
  `0x0D 0x0A`. A binary file written that way decodes fine on Windows (the read side translates
  back) and is corrupt everywhere else. Use `"rb"` and `"wb"` for anything that is not text.
- **Partial writes on sockets are real under load.** With non-blocking sockets or a full send
  buffer, `send` returns less than requested. A server that ignores the count sends truncated
  responses only under load: exactly the bug that never reproduces on a developer machine.
- **Config and state files are replaced, not overwritten.** Truncating and rewriting a file in
  place leaves an empty or half-written file if the process dies in between. Use the
  temp-fsync-rename-fsync recipe (on Windows, `MoveFileExW` with `MOVEFILE_WRITE_THROUGH` or
  `ReplaceFileW`).

## 6. Try it

1. **Easy - predict the output.** Without running it, predict what
   `printf("a\n"); write(1, "b\n", 2); printf("c\n");` prints on a terminal and when piped to
   `cat`. Then run it both ways. Done when you can explain why the two orders differ, using
   section 2.2.
2. **Easy - strace.** Run `strace -e trace=openat,read,write,close ./m13_binary_format_mmap`.
   Done when you can point at the `write` of 93 bytes and explain why there is no `read` of the
   file afterwards.
3. **Medium - write_full under pressure.** Make the pipe writer in `m13_robust_io.c` the parent
   and the reader a slow child, and write 1 MiB with `O_NONBLOCK` on the write end. Count how many
   partial writes and `EAGAIN` errors occur and add `poll` to wait for writability. Done when
   1 MiB arrives intact and you can report both counts.
4. **Medium - format version 2.** Add a `u32 branch_code` field to each record and bump the
   version to 2. Make the reader accept both versions. Done when a version 1 file and a version 2
   file both decode, and a version 3 file is rejected.
5. **Hard - crash-test durable_replace.** Write a loop that calls `durable_replace` with
   alternating contents A and B while a second script `kill -9`s it at random moments. Check the
   file after each kill. Done when 1000 kills always leave exactly A or exactly B. Then remove the
   `fsync` calls and explain why the test on WSL may still pass (hint: a process kill is not a
   power cut).
6. **Hard - Win32 port.** Port `m13_tcp_echo.c` to Winsock2 (`WSAStartup`, `SOCKET`,
   `closesocket`, `recv`, `send`, `WSAGetLastError`) and build it with MSVC `/W4 /WX`. Done when
   the Windows client talks to the Linux server running in WSL.

## 7. Interview questions

**Q: What is the difference between a `FILE *` and a file descriptor?**
A: A file descriptor is a small integer that indexes the kernel's per-process table of open
files; `read` and `write` on it are system calls with no user-space buffering. A `FILE *` is a C
library object that wraps a descriptor with a buffer and formatting functions, so many small
writes become few system calls. Mixing both on one file without `fflush` reorders data.

**Q: Why must `read` and `write` be called in a loop?**
A: Both may transfer fewer bytes than requested: on pipes, sockets and terminals routinely, and on
any file when a signal arrives or near EOF or a full disk. They can also fail with `EINTR` when a
signal interrupts a blocking call. A correct helper loops until the full count is transferred,
EOF is reached, or a real error occurs.

**Q: Does `write` returning success mean the data is on disk?**
A: No. It means the data is in the kernel page cache; the kernel writes it later. Use `fsync` or
`fdatasync` (or `O_SYNC`/`O_DSYNC`) for durability. To replace a file atomically and durably,
write a temp file, fsync it, rename it over the target, and fsync the directory.

**Q: What does `fork` do to unflushed stdio buffers?**
A: The child gets a copy of the parent's memory, including any unflushed buffer, so both processes
eventually write the same data and the output appears twice. Call `fflush(NULL)` before `fork`,
and use `_exit` in a child that does not `exec` so it does not flush the copied buffers.

**Q: Why not `fwrite` a struct to a file?**
A: The bytes depend on padding, byte order and type sizes, which differ between compilers and
platforms (`long` is 4 bytes on Windows and 8 on Linux), so another program or platform reads
garbage. Encode each field with fixed-width types in a documented byte order, add a magic number,
a version and a checksum, and bounds-check every length when decoding.

**Q: When would you use `mmap` instead of `read`?**
A: For large files accessed randomly, data shared between processes, or formats parsed in place,
because it avoids system calls and copies. The costs are page-fault overhead, errors arriving as
signals (SIGBUS if the file is truncated), and more complex durability (`msync`). For one
sequential pass, `read` into a reused buffer is usually as fast and simpler.

**Q: What is SIGPIPE and how does a server deal with it?**
A: Writing to a socket or pipe whose reading end is closed raises SIGPIPE, whose default action
terminates the process. Servers ignore it (`signal(SIGPIPE, SIG_IGN)`) or pass `MSG_NOSIGNAL` to
`send`, and handle the resulting `EPIPE` error by closing that connection.

## 8. Recap

- Two layers: stdio buffers in your process; system calls go to the kernel; below both, the page
  cache.
- Buffering: full for files and pipes, line for a terminal stdout, none for stderr; `setvbuf`
  first; `fclose` can fail; `fork` duplicates unflushed buffers.
- `read` and `write` may do less than asked and may fail with `EINTR`: loop with
  `read_full`/`write_full`.
- `write` success is not durability: `fsync` the file, rename atomically, `fsync` the directory;
  a failed `fsync` means the data is lost.
- Binary formats: fixed widths, explicit byte order, magic, version, header size, CRC, and
  bounds-checked decoding.
- `mmap` avoids copies and calls; beware SIGBUS and signal-based errors.
- TCP: `getaddrinfo`, `SO_REUSEADDR`, `shutdown(SHUT_WR)` for a clean end of request, ignore
  SIGPIPE, loop on every transfer.
- Win32: `CreateFileW`/`ReadFile`/`WriteFile`/`CloseHandle`, `FlushFileBuffers`, `MoveFileExW`,
  `WSAStartup` before any socket call, and `"rb"`/`"wb"` for binary files.
