# 08. Valgrind and the sanitizers

**Time:** ~4 hours   **Prerequisites:** 03, 04

## 1. In plain words

Debuggers show you the aftermath. Valgrind and the sanitizers catch the bug at the moment it
happens: the exact instruction that wrote past a buffer, read a freed block, used an
uninitialised value, raced another thread or overflowed an integer - with the stack trace of
that moment and often the stacks of the allocation and the free as well. They turn the "crash
far from the bug" problem of module 03 into "here is the line".

They do it in two different ways. Valgrind runs your unmodified binary on a software CPU that
checks every memory access; nothing to rebuild, but 20 to 50 times slower. The sanitizers are
compiler features: you rebuild with a flag and the compiler inserts checks into the code; 2 to
3 times slower for ASan, fast enough to run your whole test suite under them every night. A
senior engineer knows which to reach for, how to read each report in ten seconds, and what each
one cannot see.

## 2. The real thing

### 2.1 Valgrind memcheck

```bash
sudo apt install valgrind
g++ -std=c++17 -g -O0 -o prog prog.cpp          # -g for line numbers; -O0 for exact lines; -O1 ok
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --num-callers=30 \
         --error-exitcode=99 ./prog args
```

Memcheck runs the program on Valgrind's synthetic CPU (dynamic binary instrumentation via the VEX
IR) and keeps **shadow state** for every byte: an **A bit** (addressable?) and eight **V bits**
(are these bits defined?). Every load/store is checked against A bits; every use of a value in a
way that affects behaviour (branch, syscall argument, address) is checked against V bits. Heap
blocks get **red zones** around them and freed blocks are **quarantined** (not reused for a
while) so UAF is caught. It intercepts `malloc`/`free`/`new`/`delete` with its own versions.

Report types and what they mean:

| Report line | Meaning | Bug class |
|-------------|---------|-----------|
| `Invalid read of size 8` / `Invalid write of size 4` | access to a non-addressable byte | overflow, UAF, wild pointer |
| `Address 0x... is 0 bytes after a block of size 40 alloc'd` | just past the end of a heap block, with its alloc stack | heap overflow / off-by-one |
| `Address 0x... is 8 bytes inside a block of size 48 free'd` | inside a freed block, with the free stack and alloc stack | use-after-free |
| `Address 0x... is not stack'd, malloc'd or (recently) free'd` | nowhere Valgrind knows | null+offset, wild pointer, stale pointer to long-freed memory |
| `Address 0x... is on thread 1's stack` with `N bytes below stack pointer` | stack misuse | stack-use-after-return, bad `alloca` |
| `Invalid free() / delete / delete[] / realloc()` | pointer is not the start of a live block | double free, freeing interior/stack pointer |
| `Mismatched free() / delete / delete []` | `new[]` freed with `delete`, or `malloc` with `delete` | mismatched allocator |
| `Conditional jump or move depends on uninitialised value(s)` | a branch used undefined bits | uninitialised read |
| `Use of uninitialised value of size 8` | undefined value used as an address or in a syscall | uninitialised pointer |
| `Syscall param write(buf) points to uninitialised byte(s)` | sending undefined memory out | uninit; information leak |
| `Source and destination overlap in memcpy(...)` | `memcpy` with overlap (UB; use `memmove`) | |
| `Uninitialised value was created by a heap allocation` / `stack allocation` | origin (with `--track-origins=yes`) | points you at the `malloc`/local |

Leak summary categories:

| Category | Meaning | Action |
|----------|---------|--------|
| **definitely lost** | no pointer to the block anywhere | a real leak; fix |
| **indirectly lost** | only reachable from a definitely-lost block (children of a leaked tree) | fixed by fixing the parent |
| **possibly lost** | only an interior pointer points to it | usually a leak unless you use interior pointers deliberately |
| **still reachable** | a pointer exists at exit (globals, caches) | not a leak by Valgrind's definition; may still be unbounded growth (module 09) |
| **suppressed** | matched a suppression file | ignore |

Useful switches: `--xtree-leak=yes` (leak sites as a callgrind tree), `--gen-suppressions=all`
(print suppression entries for noisy third-party code; collect into `--suppressions=file`),
`--vgdb=yes --vgdb-error=0` (attach gdb to the valgrind'd process at the first error: `gdb` then
`target remote | vgdb`), `--trace-children=yes`, `--log-file=vg.%p.txt`, `--xml=yes` for CI
parsing, `--malloc-fill=0xAA --free-fill=0xBB`, `--redzone-size=256` (catch bigger overruns),
`--undef-value-errors=no` (speed when you only want addressability).

Limits: ~20-50x slowdown and 2x memory (the Valgrind manual's own figures for memcheck); no
detection of **stack or global buffer overflows** (there are no red zones around stack arrays -
ASan does this); only one thread runs at a time (races are hidden; use helgrind); does not work
with ASan-instrumented binaries; some SIMD instructions unsupported in old versions. Works on WSL2.

### 2.2 Helgrind and DRD

```bash
valgrind --tool=helgrind ./prog          # data races, lock order violations, pthread API misuse
valgrind --tool=drd --check-stack-var=yes ./prog   # similar, different algorithm, lower memory
```

Helgrind tracks every lock/unlock/create/join to build a happens-before relation (vector clocks)
and reports accesses to the same address from two threads with no ordering:

```
==1234== Possible data race during write of size 8 at 0x4A4C040 by thread #3
==1234== Locks held: none
==1234==    at 0x109234: worker(int) (m10_data_race.cpp:30)
==1234== This conflicts with a previous write of size 8 by thread #2
==1234== Locks held: none
==1234==    at 0x109234: worker(int) (m10_data_race.cpp:30)
==1234==  Address 0x4A4C040 is 0 bytes inside data symbol "g_total"
```

It also reports `Thread #1: lock order "0x... before 0x..." violated` with both acquisition
stacks - a potential deadlock found on a run that did not deadlock. Costs ~100x; use TSan
when you can rebuild, helgrind when you cannot (third-party binary, no source). Both understand
pthreads and C++11 threads (which are pthreads underneath); custom spinlocks and lock-free code
produce false positives unless annotated.

### 2.3 Cachegrind and Callgrind (intro)

```bash
valgrind --tool=cachegrind ./prog  && cg_annotate cachegrind.out.<pid>
valgrind --tool=callgrind ./prog   && callgrind_annotate callgrind.out.<pid>   # or kcachegrind GUI
```

Cachegrind simulates I1/D1/LL caches and counts instructions and misses per line; Callgrind adds
the call graph and inclusive costs. Deterministic, so two runs compare exactly - good for "did my
change reduce cache misses in the hot loop". Not for wall-clock profiling (`perf` is), and 50x
slow. Mentioned here so you know the names; it is not a debugging tool.

### 2.4 AddressSanitizer (ASan)

```bash
g++ -std=c++17 -g -O1 -fsanitize=address -fno-omit-frame-pointer -o prog_asan prog.cpp
ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1 ./prog_asan
```

The compiler inserts a check before every load/store: it looks up the **shadow byte** for the
address (shadow = `(addr >> 3) + 0x7fff8000` on x86-64 Linux; one shadow byte per 8 bytes of
memory) and traps if the shadow says the bytes are poisoned. The ASan runtime replaces `malloc`:
every block gets poisoned **redzones** before and after, freed blocks go into a **quarantine**
(256 MB by default) poisoned as "freed", stack frames get redzones around each local, globals get
redzones after them. The result: overflows, underflows, UAF and double free are caught at the
access, with stacks for the access, the allocation and the free.

A report, annotated:

```
==4242==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000038 at pc 0x55e9... bp 0x7ffd... sp 0x7ffd...
WRITE of size 4 at 0x602000000038 thread T0                         <- what: a 4-byte write
    #0 0x55e9... in fill_totals(int*, int) m04_heap_overflow.cpp:27  <- where: your line
    #1 0x55e9... in main m04_heap_overflow.cpp:36
0x602000000038 is located 0 bytes after 40-byte region [0x602000000010,0x602000000038)   <- which block, how far past
allocated by thread T0 here:                                        <- who allocated it
    #0 0x7f... in operator new[](unsigned long)
    #1 0x55e9... in main m04_heap_overflow.cpp:33
SUMMARY: AddressSanitizer: heap-buffer-overflow m04_heap_overflow.cpp:27 in fill_totals(int*, int)
Shadow bytes around the buggy address:
  0x0c047fff8000: fa fa 00 00 00 00 00[fa]fa fa ...                  <- [fa] = the shadow byte hit: heap redzone
Shadow byte legend (one shadow byte represents 8 application bytes):
  Addressable:           00
  Partially addressable: 01 02 03 04 05 06 07
  Heap left redzone:       fa
  Freed heap region:       fd
  Stack left redzone:      f1
  Stack mid redzone:       f2
  Stack right redzone:     f3
  Stack after return:      f5
  Stack use after scope:   f8
  Global redzone:          f9
```

Error names you will meet: `heap-buffer-overflow`, `stack-buffer-overflow`,
`global-buffer-overflow`, `heap-use-after-free`, `stack-use-after-return`,
`stack-use-after-scope`, `double-free`, `attempting free on address which was not malloc()-ed`
(bad free), `alloc-dealloc-mismatch` (`new[]`/`delete`), `memcpy-param-overlap`,
`negative-size-param`, `container-overflow` (writing into a `vector`'s unused capacity when
libstdc++ annotations are on), `SEGV on unknown address` (ASan caught a plain segfault; it adds
"Hint: address points to the zero page" for null derefs), `stack-overflow`.

`ASAN_OPTIONS` worth knowing: `detect_leaks=1` (LSan; on by default on Linux x86-64),
`halt_on_error=0` (keep going after the first error, needs `-fsanitize-recover=address`),
`abort_on_error=1` (SIGABRT so you get a core), `detect_stack_use_after_return=1`,
`strict_string_checks=1`, `check_initialization_order=1`, `symbolize=1`, `log_path=/tmp/asan`,
`malloc_context_size=30`, `quarantine_size_mb=`, `print_stats=1`, `handle_segv=0` (let your own
handler run), `allocator_may_return_null=1`. `__asan_poison_memory_region` and
`ASAN_DEFAULT_OPTIONS` via `__asan_default_options()` for custom allocators (arena allocators!
poison the free part of your arena so ASan sees overflows inside it).

Cost (from the ASan paper and GCC/LLVM docs, typical): ~2x CPU, ~2-3x memory (shadow + redzones
+ quarantine). Fast enough for CI and for a canary test instance; not for production.

Cannot: detect uninitialised reads (that is MSan, clang-only); see overflows that stay inside a
block (writing field B through a pointer to field A); detect races; be combined with Valgrind or
TSan in the same binary. Uninstrumented libraries (a vendor `.so` built without ASan) are not
checked inside, but their `malloc`s are still tracked.

### 2.5 LeakSanitizer (LSan)

Part of ASan; runs at exit and reports blocks not reachable from roots (globals, stacks,
registers, TLS):

```
==4242==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 32000 byte(s) in 250 object(s) allocated from:
    #0 0x7f... in operator new(unsigned long)
    #1 0x55... in handle_request(int) m09_leak.cpp:33
Indirect leak of 16000 byte(s) in 250 object(s) allocated from: ...   <- the std::string buffers owned by the leaked Requests
SUMMARY: AddressSanitizer: 48000 byte(s) leaked in 500 allocation(s).
```

Standalone, without ASan's overhead: `-fsanitize=leak`. Runtime check on demand in a long-running
server: `#include <sanitizer/lsan_interface.h>` and call `__lsan_do_recoverable_leak_check()` from
an admin command - it reports leaks accumulated so far without exiting. Suppressions:
`LSAN_OPTIONS=suppressions=lsan.supp` with lines like `leak:libcrypto`. "Direct" vs "indirect" is
the same idea as Valgrind's definitely vs indirectly lost. LSan has no "still reachable" category;
growth in live containers is invisible to it (module 09).

### 2.6 UndefinedBehaviorSanitizer (UBSan)

```bash
g++ -std=c++17 -g -O1 -fsanitize=undefined -fno-sanitize-recover=all -o prog_ubsan prog.cpp
UBSAN_OPTIONS=print_stacktrace=1 ./prog_ubsan
m04_integer_overflow_size.cpp:27:21: runtime error: signed integer overflow: 1073741824 * 4 cannot be represented in type 'int'
```

Checks inserted for: signed integer overflow, division by zero, shift exponent out of range or
shifting into the sign bit, null pointer dereference (`-fsanitize=null`), misaligned access
(`alignment`), out-of-bounds index on arrays with a known bound (`bounds`), `vptr` (clang: wrong
dynamic type via `dynamic_cast`-style checks), `return` (falling off a non-void function),
`vla-bound`, `float-cast-overflow` (not in the default group), `enum`/`bool` invalid values,
`unreachable`, `pointer-overflow`, `nonnull-attribute`, `object-size`. Unsigned wrap is not UB,
so `-fsanitize=unsigned-integer-overflow` exists only in clang and is opt-in.

Cost: small (tens of percent), no shadow memory; it combines with ASan or TSan. Default behaviour
is to print and continue; `-fno-sanitize-recover=all` makes the first error fatal, which is what
you want in CI. It is the cheapest sanitizer and the one most people forget. Every "appears only
at -O2" bug (module 04) is a UBSan candidate.

### 2.7 ThreadSanitizer (TSan)

```bash
g++ -std=c++17 -g -O1 -fsanitize=thread -fno-omit-frame-pointer -pthread -o prog_tsan prog.cpp
TSAN_OPTIONS=second_deadlock_stack=1:detect_deadlocks=1:halt_on_error=0 ./prog_tsan
```

TSan instruments every memory access and every synchronisation operation (mutexes, atomics,
thread create/join, condition variables) and keeps, per 8-byte word, the last few accesses with
their thread and **vector clock**. Two accesses to the same location, at least one a write, not
ordered by the happens-before relation = a data race, reported with both stacks:

```
WARNING: ThreadSanitizer: data race (pid=4242)
  Write of size 8 at 0x55e9c2c0a0c0 by thread T2:
    #0 worker(int) m10_data_race.cpp:30
  Previous write of size 8 at 0x55e9c2c0a0c0 by thread T1:
    #0 worker(int) m10_data_race.cpp:30
  Location is global 'g_total' of size 8 at 0x55e9c2c0a0c0
  Thread T2 (tid=4245, running) created by main thread at:
    #0 pthread_create
    #1 std::thread::_M_start_thread
    #2 main m10_data_race.cpp:41
SUMMARY: ThreadSanitizer: data race m10_data_race.cpp:30 in worker(int)
```

Also reports: `lock-order-inversion (potential deadlock)` with `detect_deadlocks=1` (both
acquisition orders, even if the run did not deadlock - run `m10_deadlock_tsan`), `thread leak`,
`destroy of a locked mutex`, `double lock of a mutex`, `unlock of an unlocked mutex`,
`signal-unsafe call inside of a signal`, `data race on vptr` (destructor racing a virtual call).

Cost (LLVM docs, typical): 5-15x CPU, 5-10x memory. Not combinable with ASan. Needs *all* code
instrumented to see all races: an uninstrumented library's internal races are invisible, and its
synchronisation may be misunderstood (false positives) unless it uses pthread/atomics TSan
intercepts. Races that never execute in the test run are not found - TSan finds races in the
paths you exercise, so coverage matters; it does not need the race to *manifest*, only both
accesses to run unordered.

If you see `FATAL: ThreadSanitizer: unexpected memory mapping` on a new kernel, run under
`setarch $(uname -m) -R ./prog_tsan` (disables ASLR for that run) or update the toolchain.

### 2.8 MemorySanitizer (mention)

`clang++ -fsanitize=memory` finds uninitialised reads like Valgrind, at ~3x instead of 30x, but
requires **everything** including libstdc++ to be instrumented (otherwise false positives), so in
practice it means a clang build of your whole dependency tree. On GCC you use Valgrind for this.

### 2.9 Build flags table

| Tool | Compile/link flags | Runtime env | Slowdown (typical) | Memory | Combine with |
|------|--------------------|-------------|--------------------|--------|--------------|
| Valgrind memcheck | none needed; `-g` for lines, `-O0`/`-O1` for accuracy | `valgrind --leak-check=full --track-origins=yes` | 20-50x | 2x | nothing instrumented |
| Helgrind / DRD | none; `-g` | `valgrind --tool=helgrind` | 50-100x | high | - |
| ASan (+LSan) | `-fsanitize=address -fno-omit-frame-pointer -g -O1` | `ASAN_OPTIONS=...` | ~2x | 2-3x | UBSan |
| LSan alone | `-fsanitize=leak -g` | `LSAN_OPTIONS=...` | ~1x | small | ASan built-in, UBSan |
| UBSan | `-fsanitize=undefined -fno-sanitize-recover=all -g` | `UBSAN_OPTIONS=print_stacktrace=1` | 1.1-1.5x | none | ASan or TSan |
| TSan | `-fsanitize=thread -fno-omit-frame-pointer -g -O1 -pthread` | `TSAN_OPTIONS=detect_deadlocks=1` | 5-15x | 5-10x | UBSan |
| MSan (clang) | `-fsanitize=memory -fsanitize-memory-track-origins` | `MSAN_OPTIONS=...` | ~3x | 2x | UBSan |
| libstdc++ debug | `-D_GLIBCXX_DEBUG` (ABI change) / `-D_GLIBCXX_ASSERTIONS` (no ABI change) | - | small | small | any |
| glibc checks | none; `MALLOC_CHECK_=3`, `MALLOC_PERTURB_=165` | env vars | ~1x | none | any |

Always `-g`. Prefer `-O1` for sanitizers: `-O0` can hide some bugs the optimiser would expose and
is slow; `-O2` inlines so much that line numbers get fuzzy. `-fno-omit-frame-pointer` makes the
fast stack unwinder in the sanitizer runtime reliable. For CI, build three variants: ASan+UBSan,
TSan+UBSan, plain `-O2 -Wall -Werror`.

### 2.10 Choosing

| Situation | Reach for |
|-----------|-----------|
| You can rebuild, want overflow/UAF/double-free/leaks, want it fast enough for the full test suite | **ASan** (+LSan, +UBSan) |
| Suspected uninitialised read, GCC toolchain | **Valgrind memcheck** `--track-origins=yes` |
| Cannot rebuild (vendor binary, production build, no source) | **Valgrind** |
| Nondeterministic wrong results, multithreaded | **TSan** (rebuild) or helgrind (no rebuild) |
| Deadlock that will not reproduce | **TSan** `detect_deadlocks=1` on the test suite |
| "Only fails at -O2" / weird arithmetic / bad casts | **UBSan** |
| Memory grows over days, no leak reported | heaptrack / massif (module 09), not a sanitizer |
| Which line is slow / cache misses | Cachegrind / `perf`, not a debugger |
| Windows | PageHeap/AppVerifier (ASan-like), MSVC `/fsanitize=address` (ASan exists in MSVC 2019 16.9+), UMDH (leaks), no TSan |

ASan beats Valgrind when: you own the build, you need stack/global overflow detection, you need
speed, or the program is large. Valgrind beats ASan when: you cannot rebuild, you need
uninitialised-value detection on GCC, or you need to check a binary exactly as shipped.

## 3. Code

Everything is driven by `code/build_debug.sh`:

```bash
cd code
./build_debug.sh all            # debug, opt, asan, tsan, ubsan variants of every program
./build_debug.sh run-asan       # heap overflow, UAF, double free, leak, stack overflow, null deref, ...
./build_debug.sh run-valgrind   # memcheck on the -O0 builds, helgrind on the race
./build_debug.sh run-tsan       # the data race and the lock-order inversion
./build_debug.sh run-ubsan      # the integer overflow
```

The exact flags live in `flags_for()` in that script - the same table as 2.9. Run each and read
the first 25 lines of every report until the format is boring. Specific things to confirm:

- `m04_uninit_read`: ASan silent, Valgrind `Conditional jump ... depends on uninitialised
  value(s)` with the origin `created by a heap allocation` at `make_record`.
- `m04_heap_overflow`: ASan `WRITE of size 4 ... 0 bytes after 40-byte region`; Valgrind
  `Invalid write of size 4 ... 0 bytes after a block of size 40 alloc'd`.
- `m04_use_after_free`: both tools give use, free and alloc stacks.
- `m09_leak`: LSan direct + indirect leaks; Valgrind definitely + indirectly lost with matching
  byte counts.
- `m10_data_race`: TSan names `g_total` and both threads; helgrind same with different wording.
- `m10_deadlock` under TSan: `lock-order-inversion (potential deadlock)` within seconds, no hang
  needed.
- `m04_integer_overflow_size 1073741824` under UBSan: `signed integer overflow`.

Also try: `MALLOC_PERTURB_=165 ./build/m04_uninit_read_debug` (glibc fills allocations with
`0xA5`; the garbage becomes a recognisable pattern) and `-D_GLIBCXX_DEBUG` on
`m04_invalid_iterator` (immediate abort with "attempt to increment a singular iterator").

## 4. Under the hood (senior-level)

**Shadow memory arithmetic.** ASan maps 8 application bytes to 1 shadow byte: value 0 = all 8
addressable, k in 1..7 = first k bytes addressable, negative = poisoned with a reason code
(`fa` heap redzone, `fd` freed, `f1-f3` stack redzones, `f5` after-return, `f8` after-scope,
`f9` global redzone). The check for an 8-byte access is `if (*(addr>>3 + offset)) report()`; for
smaller accesses it compares the low 3 bits of the address against k. The shadow region
(1/8th of the address space) is reserved at startup with `mmap(MAP_NORESERVE)`, which is why ASan
processes show ~20 TB of virtual size in `top`. This is also why ASan cannot catch an overflow
that lands in another *addressable* byte (inside the same block or in an adjacent live block
with no redzone between them in a custom allocator): the shadow says "fine".

**Why ASan needs its own allocator.** Redzones must surround every block and freed memory must
not be reused immediately. The ASan allocator uses size classes, puts redzones (16 bytes
minimum, larger for big blocks) around each block, records the allocation stack in the block's
header, and moves freed blocks through a FIFO quarantine of `quarantine_size_mb`. UAF detection
is only as good as the quarantine: a program that churns gigabytes between the free and the use
can evict the block, after which the use is seen as a valid access to a new block. Increase the
quarantine when hunting a long-gap UAF.

**Stack-use-after-return** requires the compiler to allocate frames with address-taken locals on
a **fake stack** (heap-allocated per thread) so that after return the memory stays poisoned
instead of being reused by the next call. That is why it is a runtime option with a compile-time
component and why it costs more.

**Valgrind's V bits are per bit, not per byte.** This lets it be precise about partially
initialised values (bitfields, `struct` padding copied with `memcpy`) and defer complaints until
the undefined bits actually influence a decision. That deferral is why the report points at a
*branch* rather than the load, and why `--track-origins=yes` is needed to find the allocation.
It is also why Valgrind is slow: every arithmetic instruction propagates V bits.

**TSan's happens-before.** Each thread has a vector clock; each synchronisation object stores
the clock at release; an acquire merges it. Each 8-byte word's shadow holds up to 4 recent
accesses (thread id, epoch, size, write?). A new access races with a stored one if their clocks
are not ordered. Consequence: TSan has no false positives for pthread/atomic-based code (if it
reports a race, two accesses really were unordered), but "unordered" is defined by the
synchronisation TSan can see; a hand-rolled spinlock using `volatile` is invisible and reports
false races - and is also a real bug under the C++ memory model (04_Concurrency).

**UBSan is mostly branches.** Each check is a compare-and-branch to a runtime handler, inserted
where the front-end knows the operation can be UB. That is why it is cheap and why it finds only
what the compiler could instrument - no shadow, no heap tracking.

**Why not run sanitizers in production?** Memory (2-10x), speed, and the attack surface of the
runtime (the sanitizer runtime is not hardened; `ASAN_OPTIONS` is an environment variable). Some
companies run a small fraction of production traffic through sanitized canaries; banks rarely
can. The production equivalents are hardening flags (`_FORTIFY_SOURCE`, stack protector, CFG -
module 13), not sanitizers.

## 5. In production

- **Sanitizers belong in CI, not in debugging emergencies.** The time to find a heap overflow is
  the night before the release, in the ASan job, not in a client's dump. Three nightly jobs
  (ASan+UBSan, TSan+UBSan, Valgrind on the slow integration tests) catch most of the catalogue
  before a bank sees it. This is the single highest-leverage practice in this course for Shakir's
  server.
- **Reproduction-first triage.** When a dump says heap corruption, do not stare at it for hours.
  Build the ASan variant, run the request mix that preceded the crash (from logs, module 11), and
  let ASan name the line. Return to the dump to confirm the same block type/size is involved.
- **Valgrind for the thing you cannot rebuild.** A vendor library, a stripped production binary
  the client insists on reproducing with, a Python/Java process embedding your C code: Valgrind
  runs it as is. Budget for the slowdown (timeouts fire; raise them or stub out time checks).
- **Suppressions are a maintained artefact.** Third-party noise (OpenSSL uninitialised-value
  tricks, leaks in `dlopen`) will drown the signal; keep `valgrind.supp`/`lsan.supp` in the repo
  and review it when you upgrade dependencies.
- **Sanitizer output in the RCA.** An ASan report with the three stacks is the strongest
  possible evidence of root cause; paste it (module 12). It converts "we believe" into "we
  observed".
- **Windows parity.** MSVC has `/fsanitize=address` since VS 2019 16.9 (no LSan/TSan/UBSan);
  PageHeap/AppVerifier for the rest; UMDH for leaks. If the Windows build is the production one,
  run the ASan job on the Linux build of the same code anyway - most bugs are platform-neutral
  and the Linux job is cheaper.

## 6. Try it

1. **Easy - run them all.** `./build_debug.sh all && ./build_debug.sh run-asan run-valgrind
   run-tsan run-ubsan`. For each program, write the tool, the error name and the line number.
   Done when you have a 10-row table.
2. **Easy - read the shadow.** From the `m04_heap_overflow` ASan report, find the `[fa]` in the
   shadow dump and explain each shadow byte on that line (`00`, `fa`, and the partial byte if
   any). Done when you can compute the block's start address from the shadow line.
3. **Medium - who sees what.** Make a table of the 8 module-04 programs x {ASan, Valgrind,
   UBSan, TSan} with "catches / misses / n.a." and run every cell. Done when every cell has been
   observed, not guessed.
4. **Medium - quarantine eviction.** Modify `m04_use_after_free` to allocate and free 300 MB of
   junk between the free and the use. Run under ASan with default and with
   `quarantine_size_mb=1024`. Done when you can show the report disappear and reappear and
   explain why.
5. **Hard - annotate an arena.** Write a bump allocator (module 03), use it for 100 small
   objects, overflow one by 1 byte. Show ASan is silent. Then add
   `__asan_poison_memory_region` on the unused tail after each allocation (include
   `<sanitizer/asan_interface.h>`, guard with `#ifdef __SANITIZE_ADDRESS__`). Done when ASan
   reports the overflow with your allocator's stack.
6. **Hard - CI script.** Write a `ci_sanitize.sh` that builds the ASan+UBSan and TSan+UBSan
   variants of `m13_raii_fix.cpp` and `m13_fuzz_target.cpp` (plain build), runs them with
   `halt_on_error=1`, and exits non-zero on any report, printing only the `SUMMARY:` lines. Done
   when it passes on the clean programs and fails when you point it at a buggy one.

## 7. Interview questions

**Q: ASan or Valgrind - when do you pick which?**
A: ASan when I can rebuild: it is 2x slow instead of 30x, catches stack and global overflows
that Valgrind cannot, and runs in CI on the whole suite. Valgrind when I cannot rebuild (vendor
or production binary) or when I need uninitialised-value detection on a GCC toolchain, since ASan
does not detect that and MemorySanitizer is clang-only and needs everything instrumented.

**Q: How does AddressSanitizer detect a heap buffer overflow?**
A: Its allocator surrounds each block with poisoned redzones and the compiler inserts a check
before each load/store that reads the shadow byte for the address (one shadow byte per 8 bytes;
`addr>>3 + offset`). An access to a redzone byte sees a poisoned shadow value and the runtime
reports the access stack plus the block's allocation stack. Freed blocks are quarantined and
poisoned so use-after-free is caught the same way.

**Q: What does "Conditional jump or move depends on uninitialised value(s)" mean and how do you
find the cause?**
A: Valgrind tracked definedness per bit and saw a branch whose outcome depends on bits that were
never written. The report points at the branch; `--track-origins=yes` adds where the undefined
memory was allocated (heap block or stack frame), which is the variable that was never
initialised.

**Q: What can ThreadSanitizer find that a debugger cannot, and what are its limits?**
A: It finds data races and lock-order inversions on any run where both accesses execute
unordered, even if the race did not produce a visible failure, with both stacks. Limits: 5-15x
slowdown, cannot combine with ASan, needs all code instrumented (uninstrumented libraries hide
races or cause false reports), and it only sees paths the test exercised.

**Q: Explain definitely lost, indirectly lost, possibly lost and still reachable.**
A: Definitely lost: no pointer to the block exists - a true leak. Indirectly lost: reachable only
from a definitely lost block (children of a leaked structure). Possibly lost: only an interior
pointer references it. Still reachable: a pointer exists at exit (globals, caches) so Valgrind
does not call it a leak, though it may still be unbounded growth.

**Q: Why run UBSan if the program "works"?**
A: Undefined behaviour lets the optimiser assume things that break code later: a null check
removed after a dereference, an overflow check folded away, a bounds check skipped. UBSan makes
the UB visible at low cost (tens of percent), combines with ASan or TSan, and finds the class of
bugs that appear only at -O2 or after a compiler upgrade.

**Q: What is the Windows equivalent of ASan?**
A: Full PageHeap (`gflags /p /enable app /full`) and Application Verifier's Heaps checks: each
allocation gets a guard page so overflow and use-after-free fault at the access, with the
allocation stack available via `!heap -p -a`. MSVC also ships a real `/fsanitize=address` since
VS 2019 16.9. There is no TSan on MSVC; UMDH covers leaks.

## 8. Recap

- Valgrind: no rebuild, 20-50x slower, catches heap overflow/UAF/double free/leaks **and
  uninitialised reads**; misses stack/global overflow; helgrind/DRD for races; cachegrind for
  cache profiling.
- ASan: rebuild with `-fsanitize=address -fno-omit-frame-pointer -g -O1`; ~2x; shadow bytes +
  redzones + quarantine; catches heap/stack/global overflow, UAF, double free, bad free,
  use-after-return/scope; LSan included; misses uninitialised reads.
- UBSan: `-fsanitize=undefined -fno-sanitize-recover=all`; cheap; integer/shift/null/alignment/
  bounds/vptr UB; combine with everything.
- TSan: `-fsanitize=thread`; 5-15x; data races and lock-order inversions with both stacks; not
  with ASan; needs all code instrumented.
- Read reports top-down: error name -> access stack -> block location/alloc stack -> free stack ->
  SUMMARY line. Paste them into the RCA.
- Leak categories: definitely / indirectly / possibly lost, still reachable; LSan direct /
  indirect.
- Run ASan+UBSan and TSan+UBSan in CI nightly; use Valgrind for what you cannot rebuild;
  PageHeap/AppVerifier/`/fsanitize=address`/UMDH on Windows.
- Sanitizers are for test and reproduction, not production; hardening flags are for production.
