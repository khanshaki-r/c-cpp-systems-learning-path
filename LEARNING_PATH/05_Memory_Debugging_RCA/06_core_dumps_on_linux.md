# 06. Core dumps on Linux

**Time:** ~4 hours   **Prerequisites:** 01, 02, 05

## 1. In plain words

A core dump is a snapshot of a process at the moment it died: its memory, its registers, the
signal that killed it, which files were mapped where. The kernel writes it when a process gets a
fatal signal like SIGSEGV or SIGABRT, if the system is configured to allow it. You load the core
into gdb together with the same binary that produced it, and gdb shows you the program frozen at
the instant of death: the backtrace of every thread, every variable, every byte of the heap.

It is the Linux equivalent of a Windows full user-mode dump. The main differences are in how you
get one (ulimit and core_pattern instead of WER and ProcDump), how you make sure the symbols
match (build-id instead of PDB GUID) and how you read it (gdb instead of WinDbg). This module
walks the whole path and ends with a worked example you can reproduce on WSL in ten minutes.

## 2. The real thing

### 2.1 Getting the kernel to write a core

Three things must be true:

1. **The core size limit is not zero.** `ulimit -c` shows it; `ulimit -c unlimited` sets it for
   the current shell and its children. It is a per-process resource limit (`RLIMIT_CORE`),
   inherited across `fork`/`exec`. Make it permanent:
   - for login shells: `/etc/security/limits.conf` -> `*  soft  core  unlimited`
   - for a systemd service: `LimitCORE=infinity` in the unit's `[Service]` section
   - from inside the program: `setrlimit(RLIMIT_CORE, {RLIM_INFINITY, RLIM_INFINITY})` at startup
     (your server should do this; it removes a whole class of "no core was produced" incidents)
2. **The process is dumpable.** Set-uid programs, programs that changed credentials, and those
   that called `prctl(PR_SET_DUMPABLE, 0)` do not dump unless `/proc/sys/fs/suid_dumpable` allows.
   Processes being ptraced by another debugger do not dump to the kernel.
3. **`core_pattern` points somewhere useful.** `/proc/sys/kernel/core_pattern` says where and
   how:

```bash
cat /proc/sys/kernel/core_pattern
# Ubuntu desktop default:
|/usr/share/apport/apport -p%p -s%s -c%c -d%d -P%P -u%u -g%g -- %E
# Fedora/Arch/most servers with systemd:
|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h
# A plain file pattern:
/var/crash/core.%e.%p.%t
```

A leading `|` means the kernel **pipes** the core into that program instead of writing a file.
On WSL and minimal containers the apport pipe target often is not running, so the core silently
disappears. Set a file pattern:

```bash
sudo sysctl -w kernel.core_pattern='/tmp/core.%e.%p.%t'     # until reboot
echo 'kernel.core_pattern=/tmp/core.%e.%p.%t' | sudo tee /etc/sysctl.d/60-core.conf   # persistent
sudo sysctl -w kernel.core_uses_pid=1
```

Pattern specifiers: `%e` executable name (15 chars), `%E` full path with `!` for `/`, `%p` pid,
`%P` pid in the initial namespace, `%t` UNIX time, `%s` signal number, `%u`/`%g` uid/gid,
`%h` hostname, `%c` core size limit. **core_pattern is global, not per container**: inside Docker
the host's pattern applies and a `|` handler runs on the host. Many "no core in the container"
tickets end here.

Core size is also shaped by `/proc/PID/coredump_filter` (bitmask: 0x01 anonymous private,
0x02 anonymous shared, 0x04 file-backed private, 0x08 file-backed shared, 0x10 ELF headers,
0x20 huge pages private, ..., default `0x33`). Servers with a 20 GiB cache that must dump
quickly set `MADV_DONTDUMP` on the cache with `madvise`, or lower the filter. A full-memory
core of a big process takes minutes and the process is frozen the whole time.

### 2.2 systemd-coredump and coredumpctl

When `core_pattern` pipes to `systemd-coredump` (the common server setup, and Ubuntu 24.04 if
you install `systemd-coredump`), cores are compressed into `/var/lib/systemd/coredump/` and
metadata goes to the journal:

```bash
coredumpctl list                      # TIME PID UID GID SIG COREFILE EXE SIZE
coredumpctl info 12345                # signal, command line, stack (if gdb installed), build-id, timestamp
coredumpctl debug 12345               # opens gdb on it with the right binary
coredumpctl dump 12345 -o core.12345  # extract to a file to copy elsewhere
journalctl -t systemd-coredump        # the log entries
```

Config: `/etc/systemd/coredump.conf` (`Storage=external`, `Compress=yes`, `ProcessSizeMax=`,
`ExternalSizeMax=`, `MaxUse=`). If a core is "truncated" or missing, `ProcessSizeMax` (defaults in
the low GiB) is the usual reason. WSL2 Ubuntu 24.04 runs systemd by default (`/etc/wsl.conf`
`[boot] systemd=true`), but `systemd-coredump` may not be installed; `cat core_pattern` tells you
which world you are in.

### 2.3 Getting a core from a live process

`gcore PID` (part of gdb) attaches, writes a core, detaches. The process pauses for the duration
(seconds for a few hundred MB). From inside gdb: `generate-core-file`. This is the equivalent of
ProcDump `-ma` and is the right tool for a hang, a leak snapshot, or "capture evidence before we
restart it" (module 11). Needs ptrace permission (module 05).

### 2.4 What is inside a core

A core is an ELF file of type `CORE` with a `PT_LOAD` segment for each memory mapping that was
dumped and a `PT_NOTE` segment with notes:

| Note | Contents |
|------|----------|
| `NT_PRSTATUS` (one per thread) | registers, pid/tid, pending signals, the signal that stopped the thread |
| `NT_PRPSINFO` | process name, state, uid, command line (truncated) |
| `NT_SIGINFO` | the `siginfo_t`: signal, `si_code`, **fault address** |
| `NT_AUXV` | auxiliary vector (program entry, page size, platform) |
| `NT_FILE` | **the list of file-backed mappings**: address ranges and paths - how gdb knows which libc to load |
| `NT_FPREGSET`, `NT_X86_XSTATE` | floating point / vector registers |

```bash
file core.12345                  # "ELF 64-bit LSB core file, x86-64, ... from './m06_crash_example', real uid: ..."
readelf -n core.12345 | less     # the notes, including NT_FILE mappings
readelf -l core.12345 | head -40 # the LOAD segments (which memory was dumped)
eu-readelf -n core.12345         # elfutils version, nicer NT_FILE output
```

What is **not** in a core: the binary's code pages (file-backed, excluded by default filter - gdb
reads them from the binary you supply), the shared libraries' code, and anything the filter
excluded. This is why you must give gdb the **same** binary and libraries.

### 2.5 Loading a core with the right binary and libraries

```bash
gdb ./m06_crash_example core.12345
# or
gdb -q -c core.12345 ./m06_crash_example
# or, if you forgot the binary:
gdb -c core.12345          # gdb reads NT_FILE/NT_PRPSINFO and tries to find the executable
(gdb) file ./m06_crash_example
```

Matching rules:

- The binary must be the **exact build**. gdb checks the build-id (`readelf -n prog | grep Build`)
  against the note in the core and warns `warning: core file may not match specified executable
  file`. A mismatched binary gives a plausible, wrong backtrace. Treat the warning as fatal.
- The core references libraries by path (`NT_FILE`). If you are on a different machine, those
  paths point at *your* libc, which differs. Either copy the originals into a sysroot and tell
  gdb, or install the matching packages:

```
(gdb) set sysroot /tmp/client-root       # contains lib/x86_64-linux-gnu/libc.so.6 etc. copied from the client box
(gdb) set solib-search-path /tmp/client-libs
(gdb) set debug-file-directory /tmp/client-debug:/usr/lib/debug
(gdb) info sharedlibrary                 # "(*)" = no debug info; "No" in Syms Read = library not found
(gdb) set debuginfod enabled on          # fetch matching libc debug info by build-id (Ubuntu's server has it)
```

Collect from the client box, every time: the core, the binary, `ldd binary` output, and copies of
each listed library. A tar of those four things is "a complete core". Script it (module 11).

Debug info for your own binary: the separate `.debug` file (module 02) in the same directory, in
`.debug/`, or under `/usr/lib/debug/.build-id/xx/yyyy.debug`, or `symbol-file server.debug`.
Without it you get function names (from `.dynsym`) but no lines or locals; with it, everything.

### 2.6 Reading a crash from a core: the fixed procedure

Do these in order, every time, before forming a theory:

```
(gdb) info inferiors / info program     # pid, "terminated with signal SIGSEGV"
(gdb) p $_siginfo                       # si_signo, si_code, si_addr
(gdb) p $_siginfo._sifields._sigfault.si_addr
(gdb) bt                                # the crashing thread (gdb selects it)
(gdb) info registers                    # rip, rsp, rbp, and the register the instruction used
(gdb) x/i $pc                           # the faulting instruction
(gdb) x/8i $pc-16                       # a little context (may misalign; use disassemble instead)
(gdb) disassemble /s                    # whole function with source
(gdb) frame N  (first frame in your code) ; info locals ; info args ; p *ptr
(gdb) info proc mappings                # which region is si_addr in?
(gdb) info sharedlibrary                # are the symbols right?
(gdb) thread apply all bt               # what were the other threads doing? (races, deadlocks)
```

`si_code` for SIGSEGV: `SEGV_MAPERR` (1) = address not mapped at all; `SEGV_ACCERR` (2) =
mapped but wrong permission (write to read-only, execute data). For SIGBUS: `BUS_ADRALN` =
misaligned access (SSE instruction on unaligned data), `BUS_ADRERR` = mmap'd file region past
EOF. For SIGABRT the `si_addr` is meaningless; look at the glibc message in stderr/logs and the
frame above `abort`. For SIGFPE: `FPE_INTDIV` = integer divide by zero (`idiv`).

Mapping the instruction back to source: `bt` and `disassemble /s` do it when debug info is
present. Without debug info: note `pc`, find the module base in `info proc mappings`, compute
the offset, and `addr2line -f -i -e prog.debug 0xOFFSET` on your machine with the debug file.
For a PIE binary gdb already does the relocation; for `addr2line` you need the file offset.

### 2.7 Reading the faulting instruction

```
=> 0x555555555291 <post_entry+17>:  sub    QWORD PTR [rdi+0xc],rax
```

Read it as: "subtract rax from the 8-byte value at rdi+12". Then `p/x $rdi`. If `rdi` is 0,
`si_addr` is `0xc`: null pointer, field at offset 12. If `rdi` is `0x5555555592a0` and that
address is not in any mapping, it is a stale pointer into freed/unmapped memory. If `rdi` is
`0x4141414141414141`, something overwrote the pointer with ASCII. The instruction plus the
register value plus the mapping table is the diagnosis; the backtrace tells you who.

Common instruction shapes:

| Instruction | Meaning |
|-------------|---------|
| `mov rax,[rdi+0x10]` / `mov [rdi+0x10],rax` | field read/write through pointer in rdi |
| `mov eax,[rax+rdx*4]` | array index: base rax, index rdx, 4-byte elements |
| `call [rax+0x8]` / `call rax` | virtual call through vtable (rax = vtable); garbage rax = type confusion/UAF |
| `rep movsb` / inside `memcpy` | bulk copy; look at `rsi`, `rdi`, `rcx` (src, dst, count) |
| `ret` with bad `pc` after | return address overwritten: stack overflow in the function that just returned |
| `ud2` | `__builtin_trap`, `std::unreachable`, or the compiler proving UB |
| `movaps [rsp+..],xmm0` -> SIGSEGV | misaligned stack (ABI violation, often from asm or wrong calling convention) |

## 3. Code: the worked example

`code/m06_crash_example.cpp` processes a batch of ledger entries. It pre-resolves account
pointers into a vector, then processes an entry that *removes* an account from the ledger
(`std::vector::erase`), shifting the remaining elements and leaving the pre-resolved pointers
stale; one entry also names an account that does not exist (lookup returned null). Build it
optimised, like production:

```bash
cd code
g++ -std=c++17 -Wall -Wextra -pthread -g -O2 -fno-omit-frame-pointer -o build/m06_crash_example m06_crash_example.cpp
ulimit -c unlimited
cat /proc/sys/kernel/core_pattern          # if it starts with '|' and you are on WSL, set a file pattern (2.1)
./build/m06_crash_example
processing 5 entries
Segmentation fault (core dumped)
ls -la /tmp/core.* core*                   # wherever your pattern put it
```

Now the analysis session. Addresses will differ on your machine; the shape will not.

```
$ gdb -q ./build/m06_crash_example /tmp/core.m06_crash_examp.4242.1759500000
Reading symbols from ./build/m06_crash_example...
[New LWP 4242]
Core was generated by `./build/m06_crash_example'.
Program terminated with signal SIGSEGV, Segmentation fault.
#0  0x000055d0c2a4a2f1 in post_entry (acct=0x0, e=...) at m06_crash_example.cpp:58
58          if (e.kind == 'D') acct->balance -= e.amount;
```

Step 1 - the signal and address:

```
(gdb) p $_siginfo.si_signo
$1 = 11
(gdb) p $_siginfo.si_code
$2 = 1                                    <- SEGV_MAPERR: not mapped at all
(gdb) p $_siginfo._sifields._sigfault.si_addr
$3 = (void *) 0xc                         <- 12 = offsetof(Account, balance)  (id is char[12])
```

Twelve is the offset of `balance` after a 12-byte `id`. Null pointer plus field offset. We
already know the class.

Step 2 - instruction and registers:

```
(gdb) x/i $pc
=> 0x55d0c2a4a2f1 <post_entry+17>:  sub    QWORD PTR [rdi+0xc],rax
(gdb) p/x $rdi
$4 = 0x0
(gdb) p/x $rax
$5 = 0x5                                  <- e.amount == 5 : this is the "D ACC-9 5" entry
```

Step 3 - the stack:

```
(gdb) bt
#0  0x000055d0c2a4a2f1 in post_entry (acct=0x0, e=...) at m06_crash_example.cpp:58
#1  0x000055d0c2a4a5b8 in process_batch (batch=std::vector of length 5, capacity 8 = {...}) at m06_crash_example.cpp:92
#2  0x000055d0c2a49f1c in main (argc=<optimized out>, argv=<optimized out>) at m06_crash_example.cpp:118
(gdb) frame 1
(gdb) p i
$6 = 4
(gdb) p batch[4]
$7 = {kind = 68 'D', id = "ACC-9\000\000\000\000\000\000", amount = 5}
(gdb) p resolved
$8 = std::vector of length 5, capacity 5 = {0x55d0c3e5e2c0, 0x55d0c3e5e2d8, 0x55d0c3e5e2c0, 0x55d0c3e5e2f0, 0x0}
(gdb) p g_ledger
$9 = std::vector of length 2, capacity 3 = {{id = "ACC-2", balance = 2050, pending = 0}, {id = "ACC-3", balance = 3000, pending = 0}}
```

Two findings from one `p`: `resolved[4]` is null (ACC-9 never existed - `lookup` returned
nullptr and nobody checked), and `resolved[3]` is `0x...2f0`, which is one past the end of the
current 2-element ledger: the pointer that was resolved to ACC-3 before the `X ACC-1` entry
shifted the vector. Entry 3 (`D ACC-3 20`) therefore wrote into a dead slot and the ledger shows
ACC-3 still at 3000 instead of 2980. The null is the trigger; the stale-pointer design is the
root cause (module 12 makes this distinction formal).

Step 4 - confirm the stale pointer:

```
(gdb) p *(Account*)0x55d0c3e5e2f0
$10 = {id = "ACC-3", balance = 2980, pending = 0}   <- the old element slot, written after erase shifted it
(gdb) p &g_ledger._M_impl._M_start[2]
$11 = (Account *) 0x55d0c3e5e2f0                      <- indeed one past _M_finish
(gdb) info proc mappings                             <- the address is in [heap]; mapped, so no fault on the stale write
```

Step 5 - the other threads (`thread apply all bt`; here there is one) and the libraries (`info
sharedlibrary`: everything resolved, no `(*)` on our binary). Done. Total time with practice:
under ten minutes. The write-up for the RCA: signal, address, instruction, frame, the two
pointers, the design flaw, the fix (resolve at use time, or keep ids not pointers, and check
lookup results) and the prevention (`-D_GLIBCXX_ASSERTIONS` would not catch this; ASan in CI
would catch the stale write as `heap-buffer-overflow` past `_M_finish`... only if the vector had
reallocated; a `std::vector` is a contiguous buffer so a review rule "no raw pointers into a
container that is mutated in the same scope" is the real prevention).

Compare with the `-O0` build: `argc` and `argv` are no longer `<optimized out>`, and `i` is
visible in frame 1 at every line. In the `-O2` core, `batch` was still printable because gdb had
the DWARF location for it; `e` printed as `...` because it was passed by reference and the
reference register had been reused. That is normal optimised-core reading.

## 4. Under the hood (senior-level)

**How the kernel writes the core.** On a fatal signal with `SIG_DFL` action and a core-dumping
disposition, `do_coredump()` in the kernel: checks `RLIMIT_CORE` and dumpability, formats
`core_pattern`, either opens the file or spawns the pipe program with the core on its stdin,
then walks the VMAs and writes those selected by `coredump_filter`, plus the notes, as an ELF
file. All threads are stopped first (the process is in a "coredumping" state and does not run
again). For a pipe handler the kernel waits for it to exit (`core_pipe_limit` caps concurrent
handlers). A 30 GiB process with a full filter takes minutes; during that time the process is
frozen and your service is down. Deciding the filter and the cache `MADV_DONTDUMP` is a design
decision for a banking server.

**Why gdb needs the binary.** Code pages are file-backed and not dumped (filter bit 0x04 off by
default), so gdb reads `.text` from the file to disassemble and to find `.eh_frame` for
unwinding. It also needs `.symtab`/DWARF for names. The build-id in the binary's
`.note.gnu.build-id` and in the core's `NT_FILE`/mapped ELF header is how gdb detects a mismatch.
Same for each shared library: `NT_FILE` says `/usr/lib/x86_64-linux-gnu/libc.so.6` was at
`0x7f...`, and gdb opens that path (under `sysroot`) to get its code and symbols.

**Why `thread apply all bt` matters even for a single-thread-looking crash.** The crashing
thread is the victim; in a race the culprit is another thread that freed or modified the object.
Its stack at the moment of the dump often shows it inside the mutating function (module 10).

**Core files and ASLR.** The core records where everything was in that run. Symbol resolution
uses the mapped addresses from `NT_FILE`, so ASLR is not a problem for gdb. It is a problem for
humans comparing addresses between runs; use `info symbol` to turn addresses into
`function+offset`, which is stable.

**`gcore` vs crash cores.** `gcore` uses ptrace to read the memory and writes the same ELF
format; registers are captured from the stopped state; there is no `NT_SIGINFO` fault address
because nothing faulted. A `gcore` of a hung server gives you all thread stacks and the heap -
enough for deadlock and leak analysis (modules 09, 10).

**Minidumps on Linux.** Google Breakpad/Crashpad produce small Windows-format minidumps from
Linux processes (stacks + selected memory), convertible with `minidump-2-core`. Useful when full
cores are too big to ship; you lose heap contents.

## 5. In production

- **Make sure cores can happen before you need one.** `setrlimit(RLIMIT_CORE)` in `main`, a
  file-based `core_pattern` (or systemd-coredump with a big `ProcessSizeMax`) on every server,
  `LimitCORE=infinity` in the unit file, disk space on the core partition, and a test: `kill
  -SEGV` a test instance and check the core appears. Most "we have no dump of the incident"
  stories are a missing one of these.
- **Ship the complete core.** Core + exact binary + `ldd` output + the listed libraries +
  `/proc/PID/maps` if you have it + the last 10k log lines. Name them by the incident id. The
  analyst usually is not on the client's box.
- **Keep every shipped build's debug file**, keyed by build-id, forever. A core without symbols
  is a guess. This is the Linux equivalent of your PDB symbol store.
- **A truncated core** (gdb: "warning: core file is truncated" / backtraces end in `??`) comes
  from `ulimit -c` being a number not `unlimited`, systemd `ProcessSizeMax`, or the disk filling.
  Check `ls -la` size against expected RSS.
- **Sensitive data.** A core of a banking server contains customer data, keys, PANs, in plain
  text. Treat core files as confidential artefacts: encrypt in transit, restrict access, delete
  after the RCA, and say so in your incident procedure. Some clients will not let a core leave
  their network; then you debug on their jump host with your symbols copied in.
- **SIGABRT cores**: the top frames are `raise/abort/__libc_message/malloc_printerr` (glibc
  heap check) or `__GI___assert_fail` (a failed `assert`) or `std::terminate` (uncaught
  exception). Go up to the first frame in your code; for `std::terminate`, `p $_siginfo` is
  useless but `frame` with `__cxa_throw` in it lets you `p *exception_object` - or find the
  exception type via `info symbol` on the `__cxa_exception` type_info pointer.
- **Hang cores** (from `gcore`): no fault; start with `thread apply all bt`, look for the
  pattern (module 10): all threads in `futex_wait` = deadlock or starvation; one thread spinning
  with others waiting = lock holder stuck; threads in `read`/`poll` = waiting on the network.

## 6. Try it

1. **Easy - make a core happen.** Configure `ulimit -c`, check `core_pattern`, crash
   `m04_null_deref ACC-9999`, find the core, `file` it, `readelf -n` it. Done when you can point
   at the `NT_SIGINFO` and `NT_FILE` notes.
2. **Easy - the procedure.** Open the core from (1) and run the fixed procedure of 2.6 in order.
   Done when you have written the signal, `si_code`, `si_addr`, instruction, register, frame,
   line and bug class on one line.
3. **Medium - the worked example cold.** Delete `batch.txt`, build `m06_crash_example` at `-O2`,
   produce the core, and reproduce the analysis in section 3 **without reading section 3**.
   Done when you have found both the null and the stale pointer and can explain the stale one
   in terms of `erase` shifting elements.
4. **Medium - wrong binary.** Change one character in a `printf` string of `m06_crash_example`,
   rebuild, and open the old core with the new binary. Done when you can show gdb's warning and
   one thing in the backtrace that is wrong or suspicious because of the mismatch.
5. **Hard - another machine.** Copy a core and binary from WSL to a different Linux (a VM, a
   Docker container with a different distro, or just a different directory simulating a
   different libc path), and make gdb load the correct libc using `set sysroot`. Done when `info
   sharedlibrary` shows the copied libc and `bt` goes through `__libc_start_main` with symbols.
6. **Hard - gcore of a hang.** Run `m10_deadlock`, `gcore` it, open the core, and determine
   both lock owners from `__owner` fields and `info threads`. Done when your two-line deadlock
   statement is backed by the core, not a live attach.

## 7. Interview questions

**Q: A client says the server crashed but there is no core file. What do you check?**
A: In order: `ulimit -c` / `RLIMIT_CORE` for the service (systemd `LimitCORE`), whether
`core_pattern` is a pipe to a handler that is not running or a path that does not exist or is
full, whether the process was dumpable (setuid, `PR_SET_DUMPABLE`), `coredump_filter` and
systemd `ProcessSizeMax` for truncation, and `dmesg` for an OOM kill (which produces no core).
Then I fix the configuration and prove it with a `kill -SEGV` test.

**Q: What do you need alongside a core to analyse it, and why?**
A: The exact binary that produced it (same build-id - code pages are not in the core and gdb
needs `.eh_frame` and symbols from the file), its separate debug info, and the shared libraries
at the paths recorded in the core's `NT_FILE` note (or a sysroot copy of them). A mismatched
binary gives a confident, wrong backtrace.

**Q: What is the first thing you look at in a core, and what does it tell you?**
A: `$_siginfo`: the signal and the fault address. A small address means null plus offset; an
address next to a stack mapping means stack overflow; SIGABRT means an assertion, uncaught
exception or allocator check - look at the glibc message and the frame above `abort`. Then the
faulting instruction and the register it dereferenced, then `bt`.

**Q: Explain `si_code` SEGV_MAPERR vs SEGV_ACCERR.**
A: MAPERR: the address is not mapped at all - null, stale, or wild pointer. ACCERR: the address
is mapped but the access violated permissions - writing to `.rodata` or code, executing data,
or hitting a `PROT_NONE` guard page. ACCERR on a write to a string literal is a classic.

**Q: How do you take a core of a live, hung server without killing it?**
A: `gcore PID` (or ProcDump `-ma` on Windows): it attaches with ptrace, writes the memory and
registers to an ELF core, and detaches; the process pauses for seconds and continues. Needs
ptrace permission. Then `thread apply all bt` offline shows what every thread was waiting on.

**Q: How do you read an optimised core where variables are `<optimized out>`?**
A: Use what is there: the faulting instruction and registers give the dereferenced pointer and
offset; DWARF often keeps function arguments and some locals; callers' frames may hold the value
in a visible variable; the heap object itself can be printed by casting its address. `-O2 -g
-fno-omit-frame-pointer` keeps enough in practice. Rebuild at `-O0` only to reproduce, never to
analyse the original core.

**Q: What are the risks of core dumps in a banking environment?**
A: They contain customer data and secrets in clear text, so they must be treated as confidential
artefacts: access controlled, encrypted in transit, retained only as long as the RCA needs,
and sometimes analysed only inside the client's network. Also, writing a multi-GB core freezes
the process for minutes - a design decision between forensic completeness and availability.

## 8. Recap

- A core needs `ulimit -c unlimited` (or `LimitCORE=infinity`/`setrlimit`), a dumpable process,
  and a `core_pattern` that works; `|` means a pipe handler, which is host-global even in Docker.
- `coredumpctl list/info/debug/dump` for systemd-coredump systems; `gcore PID` for a live
  snapshot.
- A core holds dumped memory segments plus notes: `NT_PRSTATUS` (registers per thread),
  `NT_SIGINFO` (signal + fault address), `NT_FILE` (mappings). Code pages are not in it.
- Load with the exact same binary (build-id) and the same libraries (`set sysroot`); add the
  `.debug` file for lines and locals; `info sharedlibrary` to verify.
- Fixed procedure: `$_siginfo` -> `bt` -> `info registers` -> `x/i $pc` -> frame in your code ->
  `info locals`/`p *ptr` -> `info proc mappings` -> `thread apply all bt`.
- Fault address small = null+offset; near stack = overflow; mapped-but-faulting = ACCERR
  permission; SIGABRT = read the glibc/assert message.
- The worked example: null from an unchecked lookup (trigger) and stale pointers after
  `vector::erase` (root cause) found from one core in ten minutes.
- Ship core + binary + libraries + debug file together; keep debug files forever; treat cores as
  confidential.
