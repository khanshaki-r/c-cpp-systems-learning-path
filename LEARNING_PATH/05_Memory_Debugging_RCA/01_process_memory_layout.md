# 01. Process memory layout

**Time:** ~3 hours   **Prerequisites:** none

## 1. In plain words

Every running program believes it owns the whole computer's memory, from address 0 up to
some enormous number. It does not. The operating system gives each process a private map, called
the virtual address space, and the CPU translates every address the program uses into a real
physical location using that map. If the program touches an address that is not on its map, the
CPU raises a fault and the OS kills the process. That is a segmentation fault on Linux and an
access violation on Windows.

Think of the address space as a very long street where each house number is an address. Most of
the street is empty land. The program's code lives in one block of houses, its global variables
in the next, the heap grows upward from there, shared libraries are placed somewhere in the
middle, and the stack hangs from the top and grows downward. A pointer is just a house number.
Dereferencing it means walking to that house. If there is no house there, you fall in a hole.

Almost every crash you will ever debug comes down to one question: which part of this map was
the bad pointer pointing at, and how did it get that value? So you need to know the map cold.

## 2. The real thing

### 2.1 Virtual memory, pages and page tables

- The CPU works in **pages**, normally 4 KiB. The OS maps virtual pages to physical frames
  through **page tables**. x86-64 uses a 4-level tree (PML4, PDPT, PD, PT); each level is indexed
  by 9 bits of the address and the final 12 bits are the offset within the page.
- Only 48 bits of a 64-bit address are used on current x86-64 (57 with 5-level paging, rare).
  Linux gives user space the lower half: `0x0000_0000_0000_0000` to `0x0000_7FFF_FFFF_FFFF`
  (128 TiB). Kernel addresses start with `0xFFFF_8...`. A user pointer that looks like
  `0xFFFF...` or `0x0000_8000_...` is garbage; it can never be valid.
- Each page table entry carries permission bits: present, writable, user, no-execute. A
  page can be readable but not writable (code, string literals) or not present at all.
- The **TLB** caches translations. A **page fault** happens when the entry is not present or the
  access violates permissions. The kernel then decides:
  - **minor fault**: the page is legitimately part of the process but not yet backed (first
    touch of a heap page, copy-on-write after `fork`) -> allocate a frame, continue silently.
  - **major fault**: the page must be read from disk (file-backed mapping, swap) -> slow, continue.
  - **invalid**: the address is not in any mapping, or the permission is wrong -> `SIGSEGV`
    (Linux) / `STATUS_ACCESS_VIOLATION 0xC0000005` (Windows).
- This is why **freed heap memory can be read without a crash**: the page is still mapped. The
  allocator gave the bytes back to its own pool, not to the OS. And why `*(int*)0 = 1` always
  crashes: the first page (and on Linux the first 64 KiB, `mmap_min_addr`) is never mapped.

### 2.2 The regions of a Linux process (low to high)

```
0x0000_0000_0000_0000  +----------------------------+
                       |  unmapped (null page etc.) |  <- any access here faults
0x0000_5555_5555_4000  +----------------------------+  (PIE binaries load around 0x5555...)
                       |  .text    r-x  code        |
                       |  .rodata  r--  literals    |
                       |  .data    rw-  initialised globals
                       |  .bss     rw-  zero-init globals (no bytes in the file)
                       +----------------------------+
                       |  [heap]   rw-  brk area, grows UP  (glibc main arena)
                       +----------------------------+
                       |            ...  huge gap  ...
                       +----------------------------+
0x0000_7FFF_F7xx_xxxx  |  mmap region, grows DOWN:  |
                       |   - large malloc blocks (>= 128 KiB default)
                       |   - thread stacks (8 MiB each, with guard page)
                       |   - secondary malloc arenas (64 MiB reservations)
                       |   - shared libraries (libc.so.6, libstdc++.so.6, ld-linux)
                       |   - file mappings (mmap of data files)
                       +----------------------------+
                       |  [stack]  rw-  main thread, grows DOWN, 8 MiB limit (ulimit -s)
                       |  [vvar] [vdso]  kernel-provided pages (gettimeofday etc.)
0x0000_7FFF_FFFF_F000  +----------------------------+
                       |  [vsyscall] (legacy, at 0xFFFF_FFFF_FF60_0000)
```

Facts you will use when reading a dump:

| Region | How it is created | Permissions | What lives there | Typical bad-pointer symptom |
|--------|-------------------|-------------|------------------|-----------------------------|
| .text | loaded from ELF | r-x | machine code | writing to it: SIGSEGV with `si_code=SEGV_ACCERR`; jumping into data: SIGSEGV or SIGILL |
| .rodata | loaded from ELF | r-- | string literals, const tables | `strcpy` into a literal -> SIGSEGV on write |
| .data / .bss | loaded / zero-filled | rw- | globals, statics | overflow of a global array quietly corrupts the next global |
| [heap] | `brk`/`sbrk` grown by glibc | rw- | small/medium malloc blocks | UAF reads stale data; overflow corrupts the next block; crash later in `free()` |
| mmap | `mmap(MAP_ANONYMOUS)` | rw- | big malloc blocks, thread stacks, arenas | reading just past a big block: SIGSEGV immediately (next page often unmapped) |
| shared libs | `mmap` of .so by `ld.so` | r-x / r-- / rw- | libc code and data | a crash with `pc` inside libc usually means your argument was bad, not libc |
| [stack] | kernel at exec | rw- | locals, return addresses | overflow -> SIGSEGV just below the mapping; overwrite return address -> crash at `ret` with garbage `pc` |

### 2.3 `/proc/<pid>/maps`

The kernel's own list of every mapping. Each line:

```
address range             perms offset   dev   inode    pathname
5555_5555_4000-5555_5555_5000 r--p 00000000 08:10 1234 /home/me/prog     <- ELF header, .rodata part
5555_5555_5000-5555_5555_6000 r-xp 00001000 08:10 1234 /home/me/prog     <- .text
5555_5555_6000-5555_5555_7000 r--p 00002000 08:10 1234 /home/me/prog     <- .rodata, .eh_frame
5555_5555_7000-5555_5555_8000 r--p 00002000 08:10 1234 /home/me/prog     <- .data.rel.ro (RELRO)
5555_5555_8000-5555_5555_9000 rw-p 00003000 08:10 1234 /home/me/prog     <- .data, .bss
5555_5555_9000-5555_5557_a000 rw-p 00000000 00:00 0    [heap]
7fff_f7c0_0000-7fff_f7c2_8000 r--p 00000000 08:10 5678 /usr/lib/x86_64-linux-gnu/libc.so.6
7fff_f7c2_8000-7fff_f7db_0000 r-xp 00028000 08:10 5678 /usr/lib/x86_64-linux-gnu/libc.so.6
...
7fff_fffd_e000-7fff_ffff_f000 rw-p 00000000 00:00 0    [stack]
```

- `perms`: `r` read, `w` write, `x` execute, `p` private (copy-on-write) or `s` shared.
- `offset`: offset into the file for file-backed mappings.
- No pathname and inode 0: anonymous memory (heap, mmap'd malloc, thread stacks).
- `pmap -x PID` gives the same with RSS per mapping. `cat /proc/PID/smaps` adds per-mapping
  detail (Rss, Pss, Private_Dirty, Swap). `/proc/PID/status` summarises: `VmSize` (virtual),
  `VmRSS` (resident), `VmHWM` (peak RSS), `VmData`, `VmStk`.

In gdb the equivalent is `info proc mappings`, which also works on a core dump (gdb reads the
`NT_FILE` note). When you have a faulting address, this is the first table you compare it with.

### 2.4 ASLR and PIE

**ASLR** (address space layout randomisation) shifts the base of the stack, the heap, the mmap
area and, for **PIE** (position independent executable) binaries, the program itself, on every
run. `/proc/sys/kernel/randomize_va_space` is `2` on Ubuntu (everything randomised). Ubuntu's gcc
produces PIE by default (`-pie`), hence program addresses like `0x5555_5555_xxxx` under gdb and
`0x5Xxx_xxxx_xxxx` in production.

Consequences for debugging:

- Addresses in a crash log from production will not match addresses on your machine. You need
  the **offset within the module** (address minus module base), which is what `addr2line -e
  prog 0xOFFSET` and `info symbol` want. Core dumps carry the mappings so gdb does this for you.
- gdb turns ASLR **off** by default (`set disable-randomization on`) so runs repeat. A bug that
  depends on layout (uninitialised pointer that happens to be valid) can vanish under gdb. Use
  `set disable-randomization off` to debug with production-like randomisation.
- ASLR is a mitigation, not a fix: it makes exploitation of memory bugs harder, it does not
  make the bugs go away.

### 2.5 What a pointer really is

A pointer is an integer. The hardware does not know what type it points to; the type is a
compile-time fiction that tells the compiler how many bytes to load and how to interpret them.
`acct->balance` where `balance` is at offset 16 compiles to `mov rax, [rdi + 0x10]`. If `rdi`
is 0, the CPU tries to read address `0x10`, the page at 0 is unmapped, the fault address
reported to you is `0x10`. **Small fault addresses are null pointers plus a field offset.** Learn
to recognise them: `0x0`, `0x8`, `0x10`, `0x18`, `0x40` ... are all "null + offset".

Other recognisable values:

| Fault address looks like | Probable meaning |
|--------------------------|------------------|
| `0x0` to `0xFFF` | null pointer + field/array offset |
| `0x7FFF_xxxx_xxxx` just below `[stack]` | stack overflow (guard page) |
| `0x5555_...` or `0x55xx_...` but unmapped | pointer into the program's area that was freed/unmapped, or an offset computed wrongly |
| `0x4141_4141_4141_4141`, `0x2020_2020...` | pointer overwritten by ASCII data ("AAAA", spaces): a string overflow |
| `0xFFFF_FFFF_FFFF_FFFF` | -1 used as pointer (an error return that was not checked) |
| `0xdead_beef`, `0xcdcd_cdcd`, `0xdddd_dddd` | debug fill patterns (see module 10 for the Windows table) |
| an address that IS mapped but `pc` is in `free`/`malloc` | heap metadata corruption (module 03) |

### 2.6 Stack growth and stack overflow

- The main thread's stack is a mapping near the top of user space with a soft limit (`ulimit -s`,
  8 MiB on Ubuntu). The kernel grows it on demand downward until the limit. Below the limit is
  a gap; touching it is a fault.
- Thread stacks (pthreads, `std::thread`) are **mmap'd by glibc**, default size from
  `ulimit -s` (so also 8 MiB; override with `pthread_attr_setstacksize`). Each has a **guard
  page** (`PROT_NONE`) at its low end. Running into it is an immediate SIGSEGV with a fault
  address just below the thread's stack mapping.
- Each call pushes a return address and usually allocates a frame. 4 KiB of locals per frame
  means about 2000 frames fit in 8 MiB. A `bt` with thousands of identical frames plus a fault
  address next to a stack mapping is a stack overflow. Causes: unbounded recursion, huge local
  arrays (`char buf[1 << 20]`), `alloca` in a loop, deep recursion in a parser on malicious input.
- A large local array is not the only way to blow the stack: `std::array<char, 1<<20>` or a
  big struct by value does it too, and in a thread with a reduced stack size (some servers use
  256 KiB per worker to allow thousands of threads) it happens much sooner.
- Windows: the exception is `STATUS_STACK_OVERFLOW 0xC00000FD`. The default thread stack is
  **1 MiB** (linker `/STACK`, or `CreateThread` parameter), eight times smaller than Linux. Code
  that runs on Linux can overflow on Windows.

### 2.7 How Windows differs

| Concept | Linux | Windows |
|---------|-------|---------|
| Address space view | `/proc/PID/maps`, `pmap` | **VMMap** (Sysinternals), `!address` in WinDbg, `VirtualQuery` API |
| User space size (x64) | 128 TiB | 128 TB (Windows 8.1+), 8 TB earlier |
| Executable image | ELF, PIE at `0x5555...` | PE, `/DYNAMICBASE` relocates it; images at `0x7FF6...`/`0x7FF7...` |
| Default heap | glibc `[heap]` via `brk` + mmap arenas | NT heap via `HeapAlloc` on top of `VirtualAlloc`; several heaps per process (process heap, CRT heap, private heaps) |
| Reserve vs commit | mmap is reserve+commit lazily (overcommit) | explicit: `MEM_RESERVE` then `MEM_COMMIT`; stack is reserved 1 MiB, committed page by page |
| Thread stack | 8 MiB default, guard page | 1 MiB default, guard page, `STATUS_STACK_OVERFLOW` |
| Shared libraries | `.so` via `ld.so`, `LD_LIBRARY_PATH` | `.dll` via loader, search order, `KnownDLLs` |
| ASLR | `randomize_va_space`, PIE | `/DYNAMICBASE`, `/HIGHENTROPYVA`, system-wide "mandatory ASLR" option |
| Null page | first 64 KiB unmapped (`mmap_min_addr`) | first 64 KiB reserved and never mapped |
| Big allocations | `mmap` directly (>= 128 KiB) | `VirtualAlloc` directly for >= ~512 KiB (heap's `VirtualMemoryThreshold`) |

VMMap shows you the same picture as `/proc/PID/maps` but with Windows categories: Image,
Mapped File, Shareable, Heap, Managed Heap, Stack, Private Data, Page Table, Unusable, Free.
"Private Data" is roughly Linux anonymous mmap. The "Committed" column is what counts against
the system commit limit; "Working Set" is roughly RSS.

## 3. Code

`code/m01_memory_layout.c` prints the address of one object of each kind and then dumps
`/proc/self/maps` so you can match them. Key part:

```c
extern char etext, edata, end;          /* linker-provided end-of-section markers      */
static int        g_bss_counter;        /* .bss: zero-initialised, costs no file bytes */
static int        g_data_counter = 42;  /* .data                                       */
static const char g_rodata_text[] = "hello";   /* .rodata: writing here faults        */

char *small_heap = malloc(100);         /* brk heap (main arena)                        */
char *big_heap   = malloc(1 << 20);     /* 1 MiB >= mmap threshold -> its own mapping   */
void *anon_map   = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
int   stack_local = 7;
```

Build and run:

```bash
cd code && mkdir -p build
gcc -std=c11 -Wall -Wextra -g -O0 -o build/m01_memory_layout m01_memory_layout.c
./build/m01_memory_layout
./build/m01_memory_layout        # run again: every address moved (ASLR)
gdb -batch -ex run ./build/m01_memory_layout | head -20   # under gdb: stable addresses
```

Check three things in the output:

1. `text < rodata < data < bss < heap`, all close together, all starting `0x5...`.
2. `malloc(1 MiB)` is **not** near `malloc(100)`; it is up at `0x7f...` next to libc, because it
   was served by `mmap` not `brk`.
3. The stack local is the highest address, and the `[stack]` line in the maps dump contains it.

## 4. Under the hood (senior-level)

**Demand paging and overcommit.** `malloc(1 << 30)` for 1 GiB returns almost instantly and RSS
does not move. glibc called `mmap`, the kernel recorded a mapping and did nothing else. Pages
become real only when first written (minor fault, zero-filled frame). With the default
`vm.overcommit_memory=0` the kernel lets processes map more than physical RAM + swap; when the
pages are actually touched and memory runs out, the **OOM killer** kills a process (often not the
one at fault). `dmesg | grep -i oom` is the first check when a server "disappeared" with no core.

**Copy-on-write after fork.** `fork` duplicates the page tables, not the pages, and marks them
read-only. The first write by either process faults and the kernel copies that one page. A
multi-GB server that forks a helper pays only for the pages the child dirties. This is also why
RSS of parent and child double-count shared pages; use PSS (`smaps`) for honest accounting.

**RSS vs VSZ.** Virtual size is how much is mapped; RSS is how many of those pages are resident.
Monitoring VSZ for leaks is meaningless (glibc reserves 64 MiB per arena); monitoring RSS is
better but still confused by freed-but-not-returned memory (module 09).

**Huge pages.** With Transparent Huge Pages (`/sys/kernel/mm/transparent_hugepage/enabled`) the
kernel may back a 2 MiB-aligned region with a single 2 MiB page. Fewer TLB misses, but a 4-byte
heap allocation can pin 2 MiB of RSS. Databases often disable THP for that reason.

**Why the gap matters.** Pointer arithmetic that goes a little past a buffer stays inside the
same page most of the time, so the hardware says nothing. Only when the pointer crosses into an
unmapped page do you get a fault. Faults are the exception; silent corruption is the rule. Tools
like ASan exist to turn silent corruption into loud faults (module 08).

**Windows reserve/commit.** On Windows, `VirtualAlloc(MEM_RESERVE)` claims address space and
`MEM_COMMIT` charges it against the commit limit (RAM + pagefile). The stack is reserved in full
(1 MiB) and committed one page at a time by the guard-page mechanism: touching the guard page
raises a guard exception, the OS commits the page and moves the guard down. Running out of
reservation gives `STATUS_STACK_OVERFLOW`. There is no overcommit; `HeapAlloc` fails with NULL
when commit is exhausted, which means Windows servers sometimes fail allocations where Linux
servers would have been OOM-killed later.

## 5. In production

- **Read `si_addr` before anything else.** A crash report with "SIGSEGV at address 0x18" is a
  null dereference of a field at offset 24, and you can often find the struct from that alone.
  "SIGSEGV at 0x7ffd1234000" next to the stack is an overflow. "SIGSEGV at 0x41414141" is a
  string that overwrote a pointer. Five seconds of thought before you even open gdb.
- **Thread stacks in a 500-thread RPC server.** 500 x 8 MiB = 4 GiB of virtual reservation, but
  only touched pages count toward RSS. People reduce stack size to allow more threads and then
  hit stack overflows in deep recursion (JSON parsers, regex engines, recursive descent) that the
  developer machine never saw. Decide stack size deliberately; log it at startup.
- **Big static buffers in .bss are free until touched.** A 256 MiB `.bss` array costs nothing in
  the binary or in RSS until used. Fine. But put it in `.data` (initialised to non-zero) and the
  binary is 256 MiB on disk.
- **`mlockall`, huge pages and RSS alarms.** Monitoring that alarms on RSS will fire on normal
  warm-up. Alarm on RSS trend over hours, not absolute value (module 09).
- **Mapping a file for a migration engine.** `mmap` of a 10 GiB data file shows 10 GiB of VSZ
  and a growing RSS as pages are read; it is reclaimable (page cache), not a leak. `smaps` shows
  it as file-backed `Rss` with zero `Anonymous`.
- **Windows: VMMap on a leaking server.** Snapshot, wait, snapshot, compare. If "Heap" grows:
  `!heap -s` (module 09). If "Private Data" grows: someone is calling `VirtualAlloc` directly or
  the heap is using large blocks. If "Image" grows: DLLs being loaded repeatedly without unload
  (plugin systems do this).

## 6. Try it

1. **Easy - map the program.** Build and run `m01_memory_layout`. In the maps dump, find the
   line containing each printed address and write down the region name. Done when you can
   explain why `malloc(1 MiB)` landed where it did.
2. **Easy - fault addresses.** Write a 10-line program that dereferences `(int*)0x10`,
   `(int*)0xFFFFFFFFFFFFFFFF` and a pointer `1` past the end of a `malloc(4096)` block. Run each
   under `gdb -batch -ex run -ex 'p $_siginfo._sifields._sigfault.si_addr'`. Done when you have
   three fault addresses and an explanation of each (and have noticed that the third one may not
   fault at all).
3. **Medium - find the guard page.** Start `m04_stack_overflow`, let it crash under gdb, then
   run `info proc mappings` and locate the `[stack]` line. Compute `si_addr - stack_start`.
   Done when you can state how far below the mapping the fault was and why.
4. **Medium - ASLR on and off.** Run `m01_memory_layout` five times outside gdb and five times
   inside gdb. Then `setarch x86_64 -R ./build/m01_memory_layout` (disables ASLR for one run).
   Done when you can explain the three sets of results and the security trade-off.
5. **Hard - thread stacks.** Write a program that starts 4 threads, each printing the address of
   a local variable and `pthread_getattr_np` stack size/base, then sleeps. From another shell,
   dump `/proc/PID/maps` and identify each thread's stack mapping and its guard page (the
   `---p` mapping right below). Done when you can point at all four stacks and say what
   happens if a thread uses 9 MiB.
6. **Hard - Windows comparison.** Run any small program on Windows, open it in VMMap, and map
   every VMMap category to a line in the Linux maps output. Done when you can explain
   "Committed" vs "Working Set" vs "Private" to a colleague in two sentences each.

## 7. Interview questions

**Q: What is the difference between virtual and physical memory, and why does a process never
see physical addresses?**
A: Each process has a private virtual address space; the CPU translates virtual to physical
through per-process page tables maintained by the OS. This isolates processes (one cannot
address another's memory), lets the OS lazily back pages on demand, share read-only pages
(libraries) between processes and swap. The program only ever sees virtual addresses.

**Q: Why does dereferencing a null pointer crash but dereferencing a freed pointer often does
not?**
A: The page at address 0 is never mapped, so the hardware faults. Freed heap memory stays
mapped: the allocator returns it to its own free lists, not to the OS. The read succeeds and
returns whatever the allocator or a later allocation left there. That is why use-after-free is
silent corruption rather than a crash, and why tools like ASan quarantine freed memory.

**Q: What does a fault address of 0x10 tell you?**
A: Almost certainly a null pointer plus a field offset of 16: code did `p->field` with `p ==
nullptr` where `field` is 16 bytes into the struct. Look at the faulting instruction (`mov ...
[rdi+0x10]`) and the type of the register's variable to confirm.

**Q: How would you recognise a stack overflow from a core dump?**
A: The signal is SIGSEGV with a fault address just below the `[stack]` mapping (or a thread's
stack mapping, next to a `---p` guard page), the stack pointer is near that address, and the
backtrace is very deep with repeating frames or shows a frame with a huge local array. On Windows
the exception code is 0xC00000FD.

**Q: Where do `malloc(100)` and `malloc(10 MiB)` come from and why does it matter?**
A: Small requests are carved from the arena heap (`brk` area for the main arena); large ones
(above the mmap threshold, 128 KiB by default) get their own `mmap` mapping. It matters for
debugging because overflow past an mmap'd block often hits an unmapped page and faults
immediately, while overflow past a small block silently corrupts the neighbouring chunk. It
matters for memory accounting because `free` of an mmap'd block returns memory to the OS at
once, while small frees do not.

**Q: What does ASLR change for a debugger?**
A: Addresses differ on every run, so you must work in module-relative offsets when matching
production logs to your build. gdb disables ASLR by default for reproducibility, which can hide
layout-dependent bugs; `set disable-randomization off` restores it.

**Q: How do Windows reserve and commit differ from Linux mmap?**
A: Windows separates reserving address space from committing backing store, and commit is
charged against RAM plus pagefile with no overcommit, so allocation fails deterministically when
commit is exhausted. Linux mmap reserves and (with overcommit) promises memory it may not have;
pages are backed on first touch and the OOM killer resolves shortages after the fact.

## 8. Recap

- A process sees a private 128 TiB virtual address space; the CPU translates through page tables
  in 4 KiB pages; unmapped or wrong-permission access is SIGSEGV / 0xC0000005.
- Regions low to high: text, rodata, data, bss, heap (brk), big gap, mmap area (big mallocs,
  thread stacks, libraries), main stack, vdso.
- `/proc/PID/maps`, `pmap -x`, `info proc mappings` (gdb), VMMap and `!address` (Windows) show
  the regions. Compare every bad address against them.
- A pointer is an integer. Small fault addresses are null plus offset. ASCII-looking addresses
  are string overflows. Addresses just below a stack mapping are stack overflow.
- Freed memory stays mapped, so UAF rarely faults; it corrupts.
- ASLR randomises bases each run; gdb turns it off by default; work in module offsets.
- Main stack 8 MiB (Linux) vs 1 MiB (Windows); thread stacks have a guard page.
- RSS counts touched pages; VSZ counts mapped bytes; neither alone proves a leak.
