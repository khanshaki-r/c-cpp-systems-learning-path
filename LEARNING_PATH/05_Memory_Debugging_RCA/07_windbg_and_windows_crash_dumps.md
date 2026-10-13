# 07. WinDbg and Windows crash dumps

**Time:** ~4 hours   **Prerequisites:** 02, 03, 05, 06 (you will map every command to gdb)

## 1. In plain words

You already use WinDbg. This module is about using it the way a senior engineer does: knowing
what `!analyze -v` did so you can do it by hand when it is wrong, reading an optimised stack
when the symbols are partial, knowing which dump type you need before the incident, and - the
big one - turning a heap corruption fail-fast into the exact line that overflowed, using PageHeap
and Application Verifier. Every command is paired with its gdb equivalent so the two debuggers
become one skill in your head.

Vocabulary note: Windows people say "bug check" for a kernel crash (blue screen, `!analyze` on a
kernel dump) and "exception code" for a user-mode crash. The codes you meet in a user-mode
server crash (`0xC0000005`, `0xC0000374`, `0xC0000409`, `0xC00000FD`) are NTSTATUS exception
codes, not bug check codes. Interviewers notice the distinction.

## 2. The real thing

### 2.1 Dump types

| Type | How made | Contains | Use |
|------|----------|----------|-----|
| **Minidump** (`MiniDumpNormal`) | WER default, `MiniDumpWriteDump` | thread stacks, registers, module list, exception record; **no heap** | crash location and stack only; tiny (hundreds of KB) |
| Minidump with data segments / handles / unloaded modules | `MiniDumpWithDataSegs`, `...WithHandleData`, `...WithUnloadedModules` | plus globals, handle table | more context for little size |
| **Full user-mode dump** (`MiniDumpWithFullMemory`) | ProcDump `-ma`, Task Manager "Create dump file", WER `DumpType=2` | everything mapped in the process: heaps, stacks, images | heap corruption, leaks, UAF, deadlocks - what you want for an RCA |
| Kernel / complete memory dump | system crash | kernel memory | drivers, BSOD; not this course |

"Full dump" in Windows speech means full user-mode dump (`.dmp`, same size as the process's
committed memory). It is the equivalent of a Linux core with the default filter. A minidump is
roughly a Breakpad minidump: stacks without heap. If a client sends you a 400 KB `.dmp` for a
heap corruption, you will get the stack and nothing else; ask for a full dump next time and
configure WER so it happens automatically.

### 2.2 Capturing a dump

**WER LocalDumps** (the config every production Windows server should have):

```
HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\YourServer.exe
    DumpFolder   REG_EXPAND_SZ  D:\dumps
    DumpType     REG_DWORD      2          (0 custom, 1 minidump, 2 full)
    DumpCount    REG_DWORD      10
```

Any unhandled exception in `YourServer.exe` then writes `YourServer.exe.<pid>.dmp` without a
debugger installed. This is the Windows `core_pattern`.

**ProcDump** (Sysinternals; the `gcore`/`ulimit` swiss army knife):

```
procdump -accepteula -ma -e -x D:\dumps YourServer.exe args    # launch, full dump on unhandled exception
procdump -ma -e 1 -f "" <pid>                                  # dump on FIRST-chance exceptions (all), useful for swallowed ones
procdump -ma -e 1 -f C0000005 <pid>                            # first-chance access violations only
procdump -ma -t <pid>                                          # dump at process termination
procdump -ma -h <pid>                                          # dump when a window hangs (GUI)
procdump -ma -c 90 -s 10 -n 3 <pid>                            # 3 dumps when CPU > 90% for 10 s (spinning thread)
procdump -ma -m 4000 <pid>                                     # dump when committed memory exceeds 4000 MB (leak)
procdump -ma <pid>                                             # snapshot NOW (hang, like gcore)
procdump -ma -w YourServer.exe                                 # wait for the process to start, then attach
```

**Task Manager** > right-click process > "Create dump file" writes a full dump to `%TEMP%`. Fine
for a one-off hang. Note: a 32-bit process dumped from 64-bit Task Manager produces a dump that
needs `.effmach x86` or `!wow64exts.sw`; use the 32-bit Task Manager (`SysWOW64\taskmgr.exe`) or
ProcDump instead.

**From code**: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` (dbghelp) to write your own
dump, ideally from a **watchdog process** rather than the crashing one (the crashing process's
heap may be the corrupted thing). This is the Windows twin of `code/m11_crash_handler.cpp`.

**From the debugger**: `.dump /ma D:\dumps\now.dmp` while attached.

### 2.3 Symbols

```
.sympath srv*C:\Symbols*https://msdl.microsoft.com/download/symbols   # Microsoft public symbols, cached locally
.sympath+ \\build\symbols\YourServer                                   # your own symbol store (symstore.exe add ...)
.sympath+ D:\builds\2026.10.03\pdb                                     # a plain folder of PDBs
.symfix                                                                # shortcut for the MS symbol server
.reload /f                                                             # force load for all modules
.reload /f YourServer.exe                                              # just one
lm                                                                     # module list: "(pdb symbols)" good, "(export symbols)" bad, "(deferred)" not loaded yet
lmvm YourServer                                                        # verbose: PDB path, GUID, timestamp, checksum
!sym noisy                                                             # show every symbol lookup and why it failed
.symopt+ 0x40                                                          # SYMOPT_LOAD_ANYTHING: accept mismatched PDB (last resort; names may be wrong)
!lmi YourServer                                                        # image info incl. debug directory
.srcpath D:\src;srv*                                                   # source paths; srv* = source server if PDBs are source-indexed
```

Environment variable `_NT_SYMBOL_PATH` sets the default. **Every shipped build's PDB must be in
your symbol store** (`symstore add /r /f D:\build\*.pdb /s \\build\symbols /t YourServer /v
2026.10.03`). Private PDBs have locals and types; public (stripped) PDBs only names. Keep the
private ones internally.

gdb mapping: `.sympath` = `set debug-file-directory` / `debuginfod`; `lm` = `info sharedlibrary`;
`.reload` = `sharedlibrary`; `!sym noisy` = `set verbose on`; `lmvm` = `readelf -n` build-id.

### 2.4 `!analyze -v` and what it actually does

`!analyze -v` reads the exception record stored in the dump, switches to that thread's exception
context, walks the stack, applies heuristics (known bad patterns, heap corruption signatures,
stack overflow, CLR frames) and prints:

```
FAULTING_IP:            YourServer!post_entry+11  ... sub qword ptr [rdi+0Ch],rax
EXCEPTION_RECORD:       ExceptionCode: c0000005 (Access violation)
                        ExceptionAddress, NumberParameters: 2
                        Parameter[0]: 0000000000000001       <- 0 read, 1 write, 8 execute (DEP)
                        Parameter[1]: 000000000000000c       <- the address   (= gdb si_addr)
                        Attempt to write to address 000000000000000c
CONTEXT:                the registers at the exception     (= gdb info registers)
PROCESS_NAME / ERROR_CODE / EXCEPTION_CODE_STR: c0000005
STACK_TEXT:             the walked stack                   (= gdb bt)
SYMBOL_NAME / MODULE_NAME / IMAGE_NAME
FAILURE_BUCKET_ID:      INVALID_POINTER_WRITE_c0000005_YourServer.exe!post_entry
```

The `FAILURE_BUCKET_ID` is how WER groups crashes; two dumps with the same bucket are the same
bug - very useful for "is this the same crash as last month" (module 11). What `!analyze` gets
wrong: heap corruption (it points at the heap manager, not your code), stack corruption (the walk
is garbage), second-chance exceptions where the first-chance context was more informative, and
wrong-thread cases (it analyses the excepting thread; the culprit may be another). So learn the
manual steps:

```
.ecxr                  # switch to the Exception Context Record (registers at the fault); like landing in gdb frame 0
.exr -1                # the exception record: code, address, parameters   (= p $_siginfo)
.cxr                   # back to the current (non-exception) context
.lastevent             # what stopped the debugger
r                      # registers (after .ecxr)                             (= info registers)
u @rip                 # disassemble at rip                                  (= x/8i $pc)
ub @rip                # disassemble backwards
k                      # stack                                               (= bt)
kv                     # with FPO/calling convention info
kb                     # with first 3 "args" - unreliable on x64 (module 02)
kp / kP                # with parameters from private symbols
kn                     # with frame numbers (needed for .frame)
kL                     # no source lines
~*k                    # all threads                                         (= thread apply all bt)
~*kb
~#s                    # switch to the thread that raised the exception      (gdb lands on it)
~3s                    # switch to thread 3                                  (= thread 3)
~~[1a2c]s              # switch by OS thread id (hex)
.frame 2               # select frame 2                                      (= frame 2)
.frame /r 2            # with the registers as they were in that frame
dv                     # locals and args                                     (= info locals / info args)
dv /t /v               # with types and addresses
dt Account 0x...       # dump typed memory                                   (= p *(Account*)0x...)
dt -r Account 0x...    # recursive
dt YourServer!g_ledger # a global
dq @rsp L20            # 20 quadwords at rsp                                 (= x/20xg $rsp)
dps @rsp L40           # quadwords with symbol lookup - stack scan           (= x/40ag $rsp)
dpp / dpu              # pointer to pointer / pointer to unicode
da / du 0x...          # ASCII / Unicode string                              (= x/s)
dd / dw / db           # dwords / words / bytes
!address 0x...         # which region (heap, stack, image, free)             (= info proc mappings)
!address -summary      # memory usage by type (like VMMap)
ln 0x...               # nearest symbol                                      (= info symbol)
x YourServer!g_*       # list symbols matching                                (= info variables / info functions)
!error c0000374        # decode an NTSTATUS/Win32 code
!gle                   # GetLastError for the current thread
!teb / !peb            # thread/process environment blocks (stack limits, env, cmdline)
!runaway               # CPU time per thread (find the spinner)
!uniqstack             # all threads, duplicates collapsed (deadlock overview)
!findstack YourServer!LockTable 2   # threads whose stack contains that symbol
!handle 0 f            # handle table (handle leaks)
s -d 0 L?0x7fffffffffff 41414141    # search memory for a pattern (slow on full dumps; scope it)
```

Exception code parameters for an access violation: `Parameter[0]` = 0 read, 1 write, 8 execute
(DEP violation: jumping into data - a smashed return address or vtable); `Parameter[1]` = the
address. Exactly `si_code`/`si_addr`.

### 2.5 Reading the stack when symbols are partial

With only public symbols for Windows DLLs and private ones for your module, a typical stack:

```
 # Child-SP          RetAddr               Call Site
00 000000e3`6f5ff2c0 00007ff6`9a4b1e58     YourServer!post_entry+0x11 [D:\src\ledger.cpp @ 58]
01 000000e3`6f5ff2f0 00007ff6`9a4b22a3     YourServer!process_batch+0x1b8 [D:\src\ledger.cpp @ 92]
02 000000e3`6f5ff3a0 00007ff6`9a4c0f2c     YourServer!main+0x9c
03 000000e3`6f5ff3f0 00007ffd`b2a17374     YourServer!__scrt_common_main_seh+0x10c
04 000000e3`6f5ff430 00007ffd`b3fdcc91     KERNEL32!BaseThreadInitThunk+0x14
05 000000e3`6f5ff460 00000000`00000000     ntdll!RtlUserThreadStart+0x21
```

`Child-SP` is the stack pointer in that frame; the difference between consecutive values is the
frame size (`0x30` for frame 0). `RetAddr` is the return address into the caller. Red flags:
`0x00000000` or a tiny `RetAddr` in the middle; a frame `YourServer+0x1234` without a function
name (symbols not loaded for your module - fix symbols before anything else); `*** Stack trace
for last set context - .thread/.cxr resets it`; and "Stack unwind information not available.
Following frames may be wrong." - the walk is now heuristic; verify with `dps @rsp`.

When `dv` shows `<value unavailable>` in release builds, do what module 02 taught: `.frame /r N`
for the registers at that frame, `u` the function, see which register/stack slot holds the
variable at the faulting instruction, read it with `dq`/`dt`.

### 2.6 Common exception codes and what they mean

| Code | Name | What to do |
|------|------|-----------|
| `0xC0000005` | STATUS_ACCESS_VIOLATION | `.exr -1`: read/write/execute + address. Small address: null+offset. Execute: smashed return address or vtable. Address inside heap but process still has it mapped: stale pointer (full dump: `!heap -p -a` tells you if the block is free). |
| `0xC0000374` | STATUS_HEAP_CORRUPTION | Fail-fast from the heap manager (`ntdll!RtlReportCriticalFailure` -> `RtlpHeapHandleError`). The stack shows **where corruption was detected**, not where it happened. `!heap -s`, `!heap -p -a <addr>` on the parameter / the block being freed. To find the writer: PageHeap or AppVerifier (2.8). |
| `0xC0000409` | STATUS_STACK_BUFFER_OVERRUN | Historically `/GS` cookie failure (`__report_gsfailure`): a local buffer overflowed. Since Windows 8 it is also the generic `__fastfail` code; the **first exception parameter is the FAST_FAIL subcode**: 2 = stack cookie, 3 = corrupt `LIST_ENTRY`, 4 = incorrect stack, 5 = invalid argument, 7 = fatal app exit (`abort()`), 8 = range check, 10 = control flow guard check (`CFG`) failure... Decode it before saying "buffer overrun". |
| `0xC00000FD` | STATUS_STACK_OVERFLOW | `~#k` shows thousands of frames or a huge local; `!teb` gives stack base/limit; compare `@rsp`. Default thread stack is 1 MiB. |
| `0xC0000008` | STATUS_INVALID_HANDLE | closed/garbage handle; `!handle`; enable AppVerifier Handles |
| `0xC000001D` | STATUS_ILLEGAL_INSTRUCTION | `ud2`/garbage code: jumped into data or `__fastfail` on older OS |
| `0xC0000094` | STATUS_INTEGER_DIVIDE_BY_ZERO | `idiv` by zero; `.ecxr` and read the divisor register |
| `0xC000008C` | STATUS_ARRAY_BOUNDS_EXCEEDED | rare; `/RTC` or checked indexer |
| `0xE06D7363` | C++ exception (`msc`) | `.exr -1`: parameters include the thrown object and its type; `!analyze -v` decodes `std::exception::what()` often. First-chance means thrown, second-chance means uncaught. |
| `0x80000003` | STATUS_BREAKPOINT | `DebugBreak()`, `__debugbreak`, `int 3` in code, or the CRT `abort()` path in debug |
| `0x80000004` | STATUS_SINGLE_STEP | trace flag; debugger artefact |
| `0xC0000017` | STATUS_NO_MEMORY | commit exhausted; `!address -summary`, `!heap -s` |

`!error 0xc0000374` prints the name. `sx` lists exception handling settings; `sxe c0000374`
breaks when it happens in a live session.

### 2.7 Heap commands

With a **full** dump:

```
!heap -s                     # summary of every heap: handle, reserved, committed, virtual blocks, free, LFH on/off
!heap -s -h 0000012ab4c00000   # one heap
!heap -stat -h <heap>        # allocation size histogram for that heap: which sizes dominate (leak hunting, module 09)
!heap -flt s 0x40            # list every busy block of size 0x40 (after -stat points at a size)
!heap -p -a 0x0000012ab4c53a20   # which block contains this address: heap, entry, user ptr, size, state (busy/free) and, if +ust was enabled, the ALLOCATING STACK
!heap -p -all                # every block with page-heap info (slow)
!heap -x 0x...               # search heaps for a block containing the address
!heap -l                     # leak detection: busy blocks with no reference anywhere (slow, heuristic)
!heap -h <heap> -v           # validate; reports where the walk breaks = corruption location
!heap -i <addr>              # detailed info on a heap entry (segment, flags, size, unused bytes)
```

Reading `!heap -p -a` output after a `0xC0000374`:

```
    address 0000012ab4c53a20 found in
    _HEAP @ 12ab4c00000
              HEAP_ENTRY Size Prev Flags            UserPtr UserSize - state
        0000012ab4c53a00 0004 0000  [00]   0000012ab4c53a10    00030 - (busy)
        ...
```

Size is in 16-byte units (`0004` = 64 bytes). A `UserSize` of 0x30 (48) in a 64-byte entry means
your request was 48 bytes. If the entry's `Flags` or `Prev` are inconsistent with the neighbour,
that neighbour's header was overwritten: an overflow of the block **before** it (just like glibc's
"invalid next size"). The gdb/glibc equivalent is reading the chunk header by hand (module 03).

With user-mode stack trace database on (`gflags /i YourServer.exe +ust`), `!heap -p -a` also
prints the stack that allocated the block. That answers "whose block is this" instantly.

### 2.8 Finding the heap corruption at the source: PageHeap and Application Verifier

This is the Windows answer to ASan. The heap manager detects corruption *later*; PageHeap makes
the **overflowing write itself** fault.

**Full PageHeap** (`gflags`, from Debugging Tools for Windows):

```
gflags /p /enable YourServer.exe /full      # every allocation on its own page(s), followed by an inaccessible guard page
gflags /p /enable YourServer.exe            # "normal" page heap: fill patterns + checks on free (cheaper, catches on free)
gflags /p /disable YourServer.exe
gflags /i YourServer.exe +ust               # user-mode stack trace DB: allocation stacks for !heap -p -a and UMDH
gflags /p                                   # show current settings
```

With `/full`, a write one byte past a block hits the guard page -> `0xC0000005` at the exact
instruction, with the full stack, and `!heap -p -a <addr>` shows the block, its size and the
allocating stack. Cost: memory use rises hugely (every allocation takes at least a page plus a
guard page) and speed drops; use it on a test instance or a reproduction, not production.
Settings live in the registry (`HKLM\...\Image File Execution Options\YourServer.exe`) and apply
to the next process start.

**Application Verifier** (`appverif.exe`): a broader runtime checker. Enable for your exe:
Basics > Heaps (this is PageHeap with extra checks), Handles (invalid/closed handle use), Locks
(critical section misuse: deleting a held lock, unlock by non-owner, lock order?), Memory
(`VirtualFree` misuse, stack overflow pre-check), TLS, Exceptions (first-chance access
violations). Run the process under WinDbg; AppVerifier breaks with a **verifier stop** (`!avrf`
shows details): stop code, description, and the stacks. Example stop: `Heap: Corrupted heap
block... Heap block overrun` with the block, the allocation stack and the current stack.
Verifier stops are the single best heap-bug input to an RCA: they name the allocation site and the
corrupting site.

Comparison for the brief: PageHeap ~ ASan for the heap (no stack/global coverage); AppVerifier
Locks ~ a subset of TSan deadlock checks plus lock-API misuse; UMDH ~ heaptrack/LSan (module 09).

### 2.9 Locks and deadlocks

```
!locks                       # every critical section with a non-zero lock count: address, owner thread id, recursion, waiters
!locks -v                    # include uncontended
!cs -l -o                    # list locked critical sections with owner stacks (needs +ust or symbols)
!cs 0x...                    # one critical section
dt ntdll!_RTL_CRITICAL_SECTION 0x...   # OwningThread, LockCount, RecursionCount
~~[<OwningThread>]s ; k      # jump to the owner and see what it is waiting on
!runaway                     # a thread with huge user time while others wait: spinning holder
!uniqstack                   # collapse duplicate waiting stacks; the odd one out is often the culprit
```

SRW locks (`SRWLOCK`, `std::shared_mutex` on MSVC) and `std::mutex` (MSVC implements it on
SRWLOCK) do **not** record an owner thread; `!locks` cannot show them. You infer the owner from
`~*k`: whoever is *inside* the protected region (not in `RtlAcquireSRWLockExclusive`) holds it.
Waiters sit in `ntdll!NtWaitForAlertByThreadId`. Module 10 covers the reasoning.

### 2.10 Command map (WinDbg -> gdb)

| WinDbg | gdb | Notes |
|--------|-----|-------|
| `!analyze -v` | `crashinfo` macro + `bt` + `info registers` | no equivalent; do the steps |
| `.ecxr` / `.exr -1` | automatic / `p $_siginfo` | fault address: Parameter[1] / `si_addr` |
| `k`, `kn`, `kv`, `kp` | `bt`, `bt full`, `info frame` | `kb` args unreliable on x64 |
| `~*k` | `thread apply all bt` | |
| `~Ns`, `~#s`, `~~[tid]s` | `thread N`, (auto), `thread find LWP` | |
| `.frame N` / `.frame /r N` | `frame N` / `info frame` + `info registers` | |
| `dv`, `dv /t /v` | `info locals`, `info args`, `ptype` | |
| `dt Type addr` / `dt -r` | `p *(Type*)addr` with `set print pretty on` | |
| `dq/dd/dw/db addr L n` | `x/ngx`, `x/nwx`, `x/nhx`, `x/nbx addr` | |
| `dps @rsp L40` | `x/40ag $rsp` | stack scan |
| `da` / `du` | `x/s` / `x/ws` | |
| `u`, `ub`, `uf` | `x/8i $pc`, `disassemble /s` | |
| `r`, `r rax` | `info registers`, `p $rax` | |
| `ba w8 addr` | `watch -l *(long*)addr` | hardware data breakpoint |
| `bp mod!func`, `bl`, `bc` | `break func`, `info b`, `delete` | |
| `g`, `p`, `t`, `gu` | `continue`, `next(i)`, `step(i)`, `finish` | |
| `sxe eh` | `catch throw` | |
| `lm`, `lmvm`, `.reload` | `info sharedlibrary`, `readelf -n`, `sharedlibrary` | |
| `.sympath` | `set debug-file-directory`, `debuginfod`, `set sysroot` | |
| `!address` / `!address -summary` | `info proc mappings` / `pmap -x` | |
| `!heap -s` / `-stat` / `-p -a` | `malloc_stats()`, heaptrack, `x/4xg ptr-16` | glibc has no `!heap` |
| `!locks`, `!cs` | `p mutex.__data.__owner` | |
| `!runaway` | `top -H -p PID` | |
| `!handle` | `ls -la /proc/PID/fd` | |
| `.dump /ma file` | `generate-core-file` / `gcore` | |
| ProcDump `-ma -e` | `ulimit -c unlimited` + `core_pattern` | |
| gflags `/full` PageHeap, AppVerifier | ASan, Valgrind | |
| UMDH | heaptrack, massif, LSan | module 09 |

## 3. Code

The programs in `code/` are g++/Linux. All of them except the ones that use `execinfo.h`,
`/proc`, `malloc.h` and `sigaction` (`m01`, `m02`, `m03`, `m09_growth`, `m11`) compile unchanged
with MSVC (`cl /std:c++17 /W4 /Zi /EHsc /Od file.cpp`). Use them to practise WinDbg on the same
bugs you debugged with gdb:

```
cl /std:c++17 /W4 /Zi /EHsc /Od /Fe:m04_null_deref.exe m04_null_deref.cpp
windbg m04_null_deref.exe ACC-9999        # or: WinDbg > Launch executable (advanced) with arguments
0:000> g
(1a2c.3f10): Access violation - code c0000005 (first chance)
0:000> .exr -1
    ExceptionCode: c0000005 (Access violation)
    Parameter[0]: 0000000000000000       <- read
    Parameter[1]: 0000000000000010       <- offset of balance: null + 0x10, same as gdb
0:000> k
0:000> dv                                 <- acct = 0x0000000000000000
0:000> u @rip L1                          <- mov rax,qword ptr [rcx+10h]   (rcx: Win64 first argument!)
```

Note the register: SysV passes `acct` in `rdi`; Win64 in `rcx`. Same bug, same offset, different
register - that is module 02 made concrete.

Heap overflow with PageHeap:

```
cl /std:c++17 /W4 /Zi /EHsc /Od /Fe:m04_heap_overflow.exe m04_heap_overflow.cpp
gflags /p /enable m04_heap_overflow.exe /full
gflags /i m04_heap_overflow.exe +ust
windbg m04_heap_overflow.exe 10
0:000> g
Access violation - code c0000005 (first chance)        <- at the stray write, not at delete[]
0:000> k                                                <- fill_totals is frame 0
0:000> .exr -1                                          <- Parameter[1] = address one past the 40-byte block
0:000> !heap -p -a <Parameter[1] - 4>                   <- the block, UserSize 0x28, and the allocating stack (operator new[] <- main)
gflags /p /disable m04_heap_overflow.exe
```

Without PageHeap the same program either runs "fine" or dies in `delete[]` with `0xC0000374`
and a stack in `ntdll!RtlReportCriticalFailure` - exactly the "far from the bug" symptom.

Deadlock: build `m10_deadlock.cpp` with MSVC (std::mutex is an SRWLOCK underneath), run, attach
with WinDbg, `~*k`, and identify the two waiters in `NtWaitForAlertByThreadId` and what each
holds by reading the frames of `transfer_a_to_b`/`transfer_b_to_a` (`!locks` will show nothing
for SRW locks - that is the lesson). Then rebuild using `CRITICAL_SECTION` and see `!locks`
name the owners.

## 4. Under the hood (senior-level)

**First-chance and second-chance exceptions.** When an exception is raised, the debugger is
notified first (first chance); if no handler catches it the debugger gets it again (second
chance) and then the process dies. WER dumps are second-chance; ProcDump `-e 1` catches first
chance, which matters when code swallows exceptions with a catch-all and continues in a bad state.
Structured Exception Handling on x64 is table-driven (`.pdata`/`.xdata`, module 02): the OS walks
the stack with `RtlVirtualUnwind` to find handlers; a corrupted stack makes unwinding fail and
the process is terminated with the original exception reported as unhandled.

**Why `0xC0000374` looks like a crash in ntdll.** The heap manager validates headers on
allocate/free (and `RtlValidateHeap` on demand). On an inconsistency it calls
`RtlpLogHeapFailure` which records the failure type (corrupted header, double free, invalid
address), the heap and the block, and then `RtlReportCriticalFailure` raises the fail-fast
`0xC0000374` with that information in the exception parameters. The encoded header (XOR with a
per-heap key) is what makes most overflows *detectable*; without it a few overwritten bytes could
go unnoticed much longer. `!analyze -v` on such a dump often prints `HEAP_CORRUPTION_ACTIONABLE_
BlockNotBusy_DOUBLEFREE` or similar, which is the failure type decoded.

**Why PageHeap works.** With `/full`, `RtlAllocateHeap` is redirected to `RtlpDebugPageHeap`
allocators which place each user block at the *end* of a page so the next byte is in a
`PAGE_NOACCESS` guard page; freed blocks are decommitted (so UAF faults too) and kept out of
reuse for a while; fill patterns (`0xC0` / `0xF0`) detect uninitialised use and underflow on
free. The cost is one+ page per allocation, hence the memory blow-up. `/full` is ASan's
redzone+quarantine implemented with MMU pages instead of shadow memory - precise, but 4 KiB
granularity per block instead of 8 bytes.

**Fill patterns you will see in Windows dumps** (the equivalent of module 03's tcache pointers):

| Pattern | Who writes it | Meaning |
|---------|---------------|---------|
| `0xCDCDCDCD` | debug CRT `malloc`/`new` | allocated, never written (uninitialised heap) |
| `0xDDDDDDDD` | debug CRT `free`/`delete` | freed heap memory (use-after-free if you see it in a live object) |
| `0xFDFDFDFD` | debug CRT | guard bytes ("no man's land") around a block; overwritten = overflow |
| `0xCCCCCCCC` | `/RTC` or `/Zi` debug stack init | uninitialised stack variable |
| `0xFEEEFEEE` | NT heap on free (when a debugger is attached / heap tail checking) | freed block |
| `0xBAADF00D` | NT heap with debug flags on allocate | uninitialised heap |
| `0xABABABAB` | NT heap | guard bytes after a block (`HeapAlloc` debug) |
| `0xC0C0C0C0` / `0xF0F0F0F0` | PageHeap | uninitialised / freed under page heap |

A pointer equal to one of these, or a field full of one, is a diagnosis by itself.

**Symbol server internals.** The debugger computes `<pdbname>/<GUID><age>/<pdbname>` and asks
each path in `.sympath` in order; `srv*` caches downloads. `.reload /f` refuses a PDB whose GUID
mismatches the executable's debug directory entry - the same guarantee as the ELF build-id. A
PDB "matches" only the exact link; rebuilding from the same source produces a different GUID,
which is why you archive PDBs rather than rebuilding them later.

## 5. In production

- **Configure WER LocalDumps with `DumpType=2` on every Windows server you support.** A minidump
  of a heap corruption is an unsolvable ticket. Put the dump folder on a disk with space and
  rotate with `DumpCount`.
- **Do not trust `!analyze -v` for `0xC0000374`/`0xC0000409`.** It tells you the detector, not
  the bug. Collect the dump for the record, then reproduce under PageHeap/AppVerifier in a test
  environment to get the writer's stack. If it cannot be reproduced, `!heap -p -a` with `+ust`
  enabled in production (moderate overhead) at least tells you whose block was corrupted, which
  narrows the suspects to code that writes near that block type.
- **`+ust` in production** is acceptable overhead for many servers (allocation stacks captured
  per allocation; memory grows); full PageHeap is not. Know which switch you are flipping on a
  bank's box and document it in the change record.
- **The excepting thread is not always the guilty one.** `~*k` every time. Another worker inside
  the same object's mutating method, or inside `HeapFree` for the block the excepting thread is
  using, is the real story (module 10).
- **32-bit vs 64-bit confusion** costs hours: a 32-bit process dumped by a 64-bit tool shows
  `wow64` frames and nonsense until `!wow64exts.sw`/`.effmach x86`. Check `vertarget` and `lm`
  first.
- **Shakir's current practice to upgrade**: keep a per-release PDB store (`symstore`), enable
  LocalDumps full dumps at every client, run the regression suite with PageHeap and AppVerifier
  (Heaps, Handles, Locks) once per release, and write the `FAILURE_BUCKET_ID` into every incident
  ticket so recurrence is searchable.

## 6. Try it

1. **Easy - manual analyze.** Open any `0xC0000005` dump (make one with `m04_null_deref.exe`
   under ProcDump `-e`). Without `!analyze`, use `.ecxr`, `.exr -1`, `k`, `u @rip L1`, `dv` to
   state read/write, address, instruction, register, frame and line. Done when it matches
   `!analyze -v` afterwards.
2. **Easy - symbol hygiene.** Deliberately point `.sympath` at an empty folder, `.reload /f`,
   `lm`, `k`. Then fix the path, `.reload /f`, `lm`, `k`. Done when you can describe three
   differences in `lm` and `k` output between the two states.
3. **Medium - PageHeap.** Follow the heap overflow walkthrough in section 3. Done when you
   have the faulting stack, the `!heap -p -a` block info and the allocating stack from `+ust`.
4. **Medium - fail-fast subcode.** Build a program that calls `abort()` and one that overflows a
   stack buffer under `/GS`. Dump both (`0xC0000409`), run `.exr -1` and decode the first
   parameter. Done when you can explain why both are 0xC0000409 and how you told them apart.
5. **Hard - deadlock, two ways.** Build `m10_deadlock.cpp` with MSVC (std::mutex), hang it,
   attach, and identify owners from `~*k` alone. Then rewrite with `CRITICAL_SECTION` and use
   `!locks` and `~~[tid]s`. Done when both analyses name the same two threads and the same lock
   order inversion.
6. **Hard - AppVerifier stop.** Run `m04_use_after_free.exe` under AppVerifier (Heaps) in WinDbg.
   Done when you can quote the verifier stop code and description, and show both the allocation
   stack and the faulting stack.

## 7. Interview questions

**Q: What is the difference between a minidump and a full user-mode dump, and which do you want
for heap corruption?**
A: A minidump has thread stacks, registers, module list and the exception record but no heap
memory; a full dump (`MiniDumpWithFullMemory`) has everything the process had mapped. Heap
corruption, use-after-free and leak analysis need the heap, so you need a full dump; configure
WER LocalDumps `DumpType=2` or ProcDump `-ma`.

**Q: `!analyze -v` says the crash is in `ntdll!RtlReportCriticalFailure` with code 0xC0000374.
Is ntdll broken?**
A: No. 0xC0000374 is the heap manager's fail-fast when it detects a corrupted block during an
allocate or free. The stack shows where the corruption was detected, not where it was caused. Use
`!heap -p -a` to identify the block, then find the writer with PageHeap (`gflags /p /enable app
/full`) or Application Verifier, which make the overflowing write itself fault.

**Q: What does exception code 0xC0000409 mean?**
A: Historically a `/GS` stack cookie failure - a local buffer overran into the return address.
On Windows 8 and later it is the generic `__fastfail` code; the first exception parameter is the
FAST_FAIL subcode (2 = stack cookie, 7 = fatal app exit from `abort()`, 10 = Control Flow Guard
failure, etc.). You decode the subcode before concluding "buffer overrun".

**Q: How does PageHeap find a heap overflow that the normal heap only detects later?**
A: Full PageHeap places every allocation at the end of its own page with an inaccessible guard
page after it, so a write one byte past the block raises an access violation at that instruction
with the full stack; freed blocks are decommitted so use-after-free faults too. It trades large
memory and speed overhead for precise detection - the Windows analogue of AddressSanitizer.

**Q: How do you find the owner of a lock in a deadlock dump?**
A: For critical sections, `!locks` or `!cs -l -o` lists each held section with its owning
thread id; `~~[tid]s` then `k` shows what the owner is waiting on, and you follow the cycle. For
SRW locks and MSVC `std::mutex` no owner is recorded, so I read `~*k`: threads blocked in
`NtWaitForAlertByThreadId` are waiters, and the thread executing inside the protected region
holds the lock.

**Q: How do symbols get matched, and what happens if you force a mismatched PDB?**
A: The executable's debug directory stores the PDB GUID and age; the debugger requests
`pdbname/GUIDage/pdbname` from each symbol path and refuses a GUID mismatch. Forcing with
`.symopt+ 0x40` loads it anyway and you get plausible but wrong function names and line numbers.
That is why PDBs for every shipped build are archived in a symbol store.

**Q: Name the WinDbg equivalents of `bt`, `info locals`, `p $_siginfo`, `thread apply all bt`,
`watch -l`, and `info proc mappings`.**
A: `k` (or `kn`/`kv`), `dv`, `.exr -1` (after `.ecxr`), `~*k`, `ba w8 <addr>`, and `!address`.

## 8. Recap

- Minidump = stacks only; full dump = everything. Configure WER LocalDumps `DumpType=2`; use
  ProcDump `-ma -e` / `-t` / `-c` / `-m` / plain for exceptions, exit, CPU, memory, snapshot.
- Symbols: `.sympath srv*cache*MS-server` + your own `symstore`; `lm` must say `(pdb symbols)` for
  your module; GUID match is mandatory; archive PDBs.
- `!analyze -v` = `.ecxr` + `.exr -1` + `k` + heuristics + `FAILURE_BUCKET_ID`; learn to do it
  manually and distrust it for heap/stack corruption.
- `.exr -1` Parameter[0] read/write/execute, Parameter[1] address = gdb `si_code`/`si_addr`.
- Codes: `C0000005` AV, `C0000374` heap corruption (detector, not cause), `C0000409` fast-fail
  (decode subcode), `C00000FD` stack overflow, `E06D7363` C++ exception.
- `!heap -s`, `-stat`, `-flt`, `-p -a` (+`+ust` for alloc stacks); `!locks`/`!cs` for critical
  sections; `~*k` for SRW locks; `!runaway`, `!uniqstack`, `!address`.
- PageHeap (`gflags /p /enable app /full`) and Application Verifier trap the corrupting write at
  the source: the Windows ASan.
- Win64 passes the first argument in `rcx` (SysV: `rdi`); same bug, same offset, different register.
