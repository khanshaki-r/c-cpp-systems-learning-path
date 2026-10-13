# 05. Memory Debugging and RCA - all exercises

Every exercise from modules 01 to 13 in one place. The full wording and the "done when" criterion
are in each module's section 6; this file gives a one-line summary, the difficulty and a hint.
Build everything in WSL Ubuntu 24.04 with `code/build_debug.sh` unless the exercise says Windows.

Suggested path if short on time: do every **easy** one, then the **medium** ones in modules 04, 06,
07, 08, 10 and 12, then pick two **hard** ones that match your work (for example 07.5 and 12.6).

Difficulty totals: 26 easy, 26 medium, 26 hard.

---

## Module 01 - Process memory layout

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 1.1 | Map the program: match each address printed by `m01_memory_layout` to its line in the maps dump. | easy | Compare the address against the start-end range of each line; large `malloc` goes to an anonymous `mmap` region, not `[heap]` (threshold is 128 KiB by default). |
| 1.2 | Fault addresses: dereference `0x10`, `0xFFFF...FF` and one past a `malloc(4096)` block; record `si_addr`. | easy | The third one usually does not fault: the page after the block is still mapped heap. Faults happen at page granularity, not object granularity. |
| 1.3 | Find the guard page: crash `m04_stack_overflow`, compute `si_addr - stack_start`. | medium | `info proc mappings` gives the `[stack]` start; the fault is just below it, in the guard gap the kernel keeps under the stack. |
| 1.4 | ASLR on and off: run `m01_memory_layout` outside gdb, inside gdb, and under `setarch -R`. | medium | gdb disables ASLR by default (`set disable-randomization on`); that is why addresses repeat inside gdb. |
| 1.5 | Thread stacks: 4 threads print local addresses and `pthread_getattr_np` info; find each stack and guard in maps. | hard | Thread stacks are anonymous `rw-p` mappings of 8 MiB (default `ulimit -s`) with a `---p` guard page just below; 9 MiB of use hits the guard and segfaults. |
| 1.6 | Windows comparison: map every VMMap category to a Linux maps line. | hard | Committed = backed by RAM or pagefile; Working Set = currently resident (like RSS); Private = not shareable (like anonymous mappings). |

## Module 02 - Stack frames and calling conventions

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 2.1 | Read a frame: show `[rbp+8]` is the return address into `level2`. | easy | `x/2xg $rbp` shows saved rbp then return address; `info symbol` on the second word names the caller. |
| 2.2 | Registers at entry: watch `$rdi` change while `p c` stays. | easy | At `-O0` the prologue spills `rdi` to the stack; `info address c` shows an rbp offset. |
| 2.3 | Inlining: compare gdb `bt` with `backtrace()` output in `m02_frames_O2`. | medium | gdb uses DWARF inline info to show inlined frames; `backtrace()` only sees real return addresses. |
| 2.4 | Separate debug file: `objcopy --only-keep-debug`, `strip`, `--add-gnu-debuglink`. | medium | `readelf -n` shows the same build id in both files; gdb matches by build id or debuglink. |
| 2.5 | Stack scan by hand: rebuild the call chain from `x/40ag $rsp` without `bt`. | hard | Look for words that `info symbol` resolves into `.text` just after a `call` instruction; those are return addresses. Ignore pointers into data. |
| 2.6 | Windows x64: `kv`, `.frame /r 1`, `dv`, `.fnent @rip`, `dps @rsp L20` in an MSVC `/O2` build. | hard | The 32 bytes above the return address belong to the callee's shadow space (home area for rcx, rdx, r8, r9); `.fnent` shows the unwind info that replaces frame pointers. |

## Module 03 - The heap and malloc internals

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 3.1 | Chunk sizes: predict chunk size for `malloc(1/16/17/24/40)`. | easy | Chunk = max(32, round_up(request + 8, 16)) on x86-64; the `size` field also has the low bit `PREV_INUSE` set. |
| 3.2 | tcache LIFO: free a, b, c then malloc three times. | easy | tcache is a singly linked stack per size class: last freed, first returned (c, b, a). |
| 3.3 | Read the abort message of `m04_heap_overflow` and translate it. | medium | `invalid next size` means the next chunk's size field was overwritten: your write went past the end of the block into the next header. |
| 3.4 | Perturb: run `m04_uninit_read` with `MALLOC_PERTURB_=165`. | medium | glibc fills freed and newly allocated memory with the byte (and its complement), so uninitialised reads give a consistent recognisable value. |
| 3.5 | Fragmentation vs leak: `m09_growth_not_leak` with `malloc_stats()` and Valgrind. | hard | Valgrind counts unreachable blocks; freed memory that glibc keeps in its bins is not a leak but still counts in RSS until `malloc_trim`. |
| 3.6 | Arena allocator: compare speed/RSS to malloc; overflow one record by a byte. | hard | No headers between arena objects means no glibc checks fire: corruption becomes silent data damage in the neighbour. |

## Module 04 - The catalogue of memory bugs

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 4.1 | Classify five symptoms without running anything. | easy | Heap overflow; null deref; race or uninitialised read; leak or unbounded growth; stack buffer overflow over the return address. |
| 4.2 | Run the suite: `./build_debug.sh run-asan`, one report line per program. | easy | The first `ERROR: AddressSanitizer:` line names the class; the `#0` frame below it names the line. |
| 4.3 | ASan vs Valgrind vs nothing on `m04_uninit_read`. | medium | ASan does not track initialisation (that is MSan, clang only); Valgrind Memcheck tracks definedness bit by bit. |
| 4.4 | Make the off-by-one in `m04_heap_overflow` abort every run. | medium | Control the layout: allocate the victim, then a neighbour of the same size class, so the stray byte hits the neighbour's size field. |
| 4.5 | Combination bug: integer overflow producing a small buffer, then a full-size copy. | hard | UBSan catches signed overflow (not unsigned wrap unless `-fsanitize=unsigned-integer-overflow` in clang); ASan catches the copy. |
| 4.6 | Turn a double free into silent corruption. | hard | Insert a same-size `malloc` between the frees: it takes the chunk out of tcache, so the second free releases a live object and the double-free check sees nothing. |

## Module 05 - gdb from zero to fluent

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 5.1 | Null deref in under 3 minutes: signal, address, instruction, frame, line, fix. | easy | `run`, `bt`, `p $_siginfo._sifields._sigfault.si_addr`, `x/i $pc`, `frame N`, `list`. |
| 5.2 | Stepping: predict each `Value returned` before `finish` prints it. | easy | Read the function body before `finish`; `step` enters, `next` steps over, `finish` returns. |
| 5.3 | Watchpoint hunt on the freed object. | medium | A hardware watchpoint on freed memory fires inside `free` because tcache writes its next pointer and key into the first 16 bytes; `watch -l balance` may never fire if that field is not overwritten. |
| 5.4 | Attach and deadlock statement with real LWPs. | medium | `thread apply all bt`, then in each waiting thread print the mutex: `__owner` is the holder's LWP. |
| 5.5 | Python pretty printer for `Account`. | hard | Subclass nothing: a class with `to_string()`, a lookup function matching `str(val.type.strip_typedefs())`, `gdb.pretty_printers.append(...)` from `.gdbinit`. |
| 5.6 | Scripted triage `crash.gdb` in batch mode. | hard | `gdb -batch -x crash.gdb binary core > first_look.txt`; include `info registers`, `x/i $pc`, `thread apply all bt full`, `info sharedlibrary`, `info proc mappings`. |

## Module 06 - Core dumps on Linux

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 6.1 | Make a core happen and find `NT_SIGINFO` and `NT_FILE` notes. | easy | WSL may pipe cores to apport or nothing; set `core_pattern` to a file path; `readelf -n core` lists the notes. |
| 6.2 | Run the fixed procedure of section 2.6 on that core and write one line. | easy | Signal, `si_code` (1 = `SEGV_MAPERR`, 2 = `SEGV_ACCERR`), `si_addr`, `x/i $pc`, the register in the instruction, frame, line, class. |
| 6.3 | Analyse `m06_crash_example` at `-O2` cold. | medium | Two bugs: the null and the stale pointer; the stale pointer is a reference into a `vector` taken before an `erase` shifted the elements. |
| 6.4 | Wrong binary: open an old core with a rebuilt binary. | medium | gdb warns about build-id mismatch (or not at all for the main binary on old versions); line numbers and local values are the first things that lie. |
| 6.5 | Analyse on another machine with `set sysroot`. | hard | Copy the libraries listed in `info sharedlibrary` (or the `NT_FILE` note) into a directory tree mirroring their absolute paths; set sysroot before loading the core. |
| 6.6 | `gcore` a hang of `m10_deadlock` and name both owners from the core. | hard | Same as 5.4 but from a static file; map `__owner` LWPs to gdb thread numbers with `info threads`. |

## Module 07 - WinDbg and Windows crash dumps

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 7.1 | Manual analyse of a `0xC0000005` dump without `!analyze`. | easy | `.ecxr` switches to the exception context; `.exr -1` parameter 0 is 0 = read, 1 = write, 8 = DEP; parameter 1 is the address. |
| 7.2 | Symbol hygiene: bad `.sympath` vs good. | easy | With bad symbols `lm` shows `(export symbols)` or `(deferred)`, `k` shows `module+0xoffset` and frames may be wrong. `!sym noisy` shows the search. |
| 7.3 | PageHeap walkthrough: faulting stack, `!heap -p -a`, allocation stack. | medium | `gflags /p /enable app.exe /full` places a guard page after each block; `+ust` must also be on for the allocation stack. |
| 7.4 | Fail-fast subcode: `abort()` vs `/GS` overflow, both `0xC0000409`. | medium | `.exr -1` parameter 0 is the `FAST_FAIL_*` code: 7 is `FAST_FAIL_FATAL_APP_EXIT` (abort), 2 is `FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`. |
| 7.5 | Deadlock two ways: `std::mutex` (SRW) from `~*k`, then `CRITICAL_SECTION` with `!locks`. | hard | SRW locks have no owner field: infer the holder from which thread is inside the protected function; `!locks` shows `OwningThread` for critical sections. |
| 7.6 | AppVerifier Heaps stop on `m04_use_after_free.exe`. | hard | Enable Heaps in `appverif.exe`, run under WinDbg; the verifier break shows the stop code; `!avrf` and `!heap -p -a <addr>` give the stacks. |

## Module 08 - Valgrind and sanitizers

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 8.1 | Run every tool on every program; 10-row table. | easy | `./build_debug.sh all` then the four `run-*` targets; take the first error line of each. |
| 8.2 | Read the ASan shadow line around `[fa]`. | easy | Shadow address = (addr >> 3) + 0x7fff8000; `00` = 8 addressable bytes, `0N` = first N addressable, `fa` = heap redzone. |
| 8.3 | Catch matrix: 8 programs x 4 tools, all observed. | medium | TSan and ASan cannot be combined in one build; expect "n.a." for TSan on single-threaded programs. |
| 8.4 | Quarantine eviction hides the UAF. | medium | ASan keeps freed memory in a 256 MB quarantine by default; churn more than that and the block is reused, so the access is "valid". |
| 8.5 | Poison a bump allocator for ASan. | hard | `ASAN_POISON_MEMORY_REGION(tail, len)` after each allocation, unpoison the returned bytes; ASan then reports `use-after-poison`. |
| 8.6 | `ci_sanitize.sh` that fails on any report. | hard | `ASAN_OPTIONS=halt_on_error=1`, `-fno-sanitize-recover=all` for UBSan, check `$?` per run, `grep SUMMARY:` the logs. |

## Module 09 - Leaks and memory growth

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 9.1 | Same leak in LSan, Valgrind and heaptrack. | easy | Direct = the leaked block itself; indirect = blocks reachable only from a leaked block. Valgrind calls them "definitely" and "indirectly lost". |
| 9.2 | Growth vs leak on `m09_growth_not_leak`. | easy | LSan reports nothing because everything is still reachable (or freed); RSS stays up because of retention or fragmentation. |
| 9.3 | RSS trend script to CSV; derive bytes per request. | medium | Read `VmRSS` and `VmHWM` from `/proc/PID/status`, `ls /proc/PID/fd | wc -l`; slope = delta RSS / delta requests. |
| 9.4 | Fix the leak with `delete` and with `unique_ptr`; add a `throw`. | medium | The `delete` on the error path does not run when an exception propagates; the `unique_ptr` destructor does. |
| 9.5 | `shared_ptr` cycle; fix with `weak_ptr`. | hard | Each node keeps the other's count at 1 after both outer pointers die; `p node.use_count()` or `p node._M_refcount` in gdb. |
| 9.6 | Heap histogram from a core with gdb Python. | hard | Start at the `[heap]` mapping start, read the size field at +8, mask the low 3 bits, step by size until the top chunk; count sizes in a dict. |

## Module 10 - Concurrency crashes and Heisenbugs

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 10.1 | See the race: plain x10, `taskset -c 0`, TSan. | easy | One core still interleaves via preemption but much less often; TSan reports the race even when the total is right. |
| 10.2 | Deadlock table with real LWPs from a `gcore`. | easy | `p g_connections_mtx.__data.__owner`, `info threads`; holds / waits-for / held-by for each thread. |
| 10.3 | Victim and culprit: delete in thread A, virtual call in thread B. | medium | The vtable pointer slot holds the tcache next pointer after free; thread A's stack (or its log) shows the `delete`. |
| 10.4 | Sleep injection: make a rare race fail every run with a `yield`. | medium | Split `g_total += 1` into load, `yield`, store; the yield gives the other thread time to run between the read and the write. |
| 10.5 | Ring buffer in the core; print the last 10 records per thread sorted by tsc. | hard | Make the trace table a global so gdb can find it; a small gdb Python script reads each ring and sorts. |
| 10.6 | Debug-only lock-rank checker that aborts on inversion. | hard | `thread_local std::vector<int>` of held ranks; on lock, assert the new rank is greater than `back()`; pop on unlock. |

## Module 11 - Production incident triage

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 11.1 | Core configuration audit and a core from `m11_crash_handler`. | easy | Build with `-rdynamic` so `backtrace_symbols_fd` prints names; `ulimit -c unlimited` in the same shell. |
| 11.2 | Write updates 1 and 2 for a fictional balance-enquiry incident. | easy | Use the 2.5 template; label anything not proven "we believe"; give the next update time. |
| 11.3 | Collect evidence from `m10_deadlock` with `m11_collect_evidence.sh`. | medium | Compare `thread apply all bt` of `core1` and `core2`: identical stacks for the blocked threads prove a deadlock. |
| 11.4 | Add structured logging and a request id to `m10_data_race`. | medium | Copy `log_event` from 3.3; set `t_request_id` at the top of each thread function; compare `tid=` with LWPs. |
| 11.5 | Find the killer request from log plus core only. | hard | Log `req.start` and `req.end`; the request with a start and no end on the crashing tid is the one; the crash handler's `request =` line confirms. |
| 11.6 | One-page runbook for a service you know. | hard | Test it on a colleague: if they need to ask you anything to take a dump or send an update, add it to the page. |

## Module 12 - Writing a formal RCA

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 12.1 | Label five statements trigger, root cause or symptom. | easy | Ask "if this happens again tomorrow, will the system survive?"; fd exhaustion is a symptom, unclosed connections is a root cause. |
| 12.2 | Rewrite a bad executive summary. | easy | What failed, when, impact with numbers, root cause in plain words, trigger, fix, data integrity statement; drop the double apology. |
| 12.3 | RCA sections 4, 5, 7, 10 for `m10_deadlock` with real evidence. | medium | Follow RCA 2; the appendix is your gdb output trimmed to the lines that prove the cycle. |
| 12.4 | Complete RCA for `m04_heap_overflow`. | medium | Follow RCA 1; you need three evidence types: the abort in the core, a correlation with an input, an ASan reproduction. |
| 12.5 | Preliminary RCA for `m10_data_race` with one core and no reproduction. | hard | Every mechanism statement says "we believe"; list what you ruled out; the action is a TSan run and a ring buffer or invariant assert in production. |
| 12.6 | Rewrite one of your own incidents in this structure. | hard | Anonymise everything; rehearse the 5-minute version aloud; lead with trigger vs root cause. |

## Module 13 - Defensive design

| # | Exercise | Difficulty | Hint |
|---|----------|-----------|------|
| 13.1 | Break `m13_raii_fix` back, one bug at a time; record which layer catches each. | easy | `i <= size()` with `[]` is caught by `_GLIBCXX_ASSERTIONS` and ASan; the raw `new` with early return by LSan; the stored raw pointer by ASan (UAF). |
| 13.2 | `checksec` an unhardened and a hardened build. | easy | Ubuntu's GCC already enables several flags by default; build with `-fno-stack-protector -no-pie -U_FORTIFY_SOURCE` for a true "before". |
| 13.3 | Fuzz `m13_fuzz_target`, turn the crash into a test, fix, re-fuzz. | medium | ASan alone misses it (the write lands in struct padding); use `-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`. |
| 13.4 | FORTIFY in action: `strcpy` from `argv[1]` into a 16-byte member. | medium | Needs `-O1` or higher; `objdump -d` shows a call to `__strcpy_chk`; the core's `bt` shows `__chk_fail`. |
| 13.5 | Static analysis baseline script. | hard | Store `clang-tidy ... | grep -c warning:` per file in a file; fail if any count increases. |
| 13.6 | Fuzz a real parser for an hour with a seed corpus. | hard | Make the target deterministic (no globals carried between calls); `-print_final_stats=1` and `-print_coverage=1` show what was reached. |
