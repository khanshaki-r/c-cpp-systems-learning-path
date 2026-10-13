# 05. Memory Debugging and RCA - interview questions

36 questions with model answers, ordered from basic to senior. Answers are what you would say
aloud: 2 to 6 sentences. The module that covers each topic in depth is in brackets.

---

## Part A - Memory fundamentals

**1. What is virtual memory, and what does a pointer value actually mean?** [01]
Every process sees its own private address space; a pointer is an address in that space, not in
physical RAM. The CPU's MMU translates it through page tables (4 KiB pages on x86-64) to a physical
frame, and the kernel fills those tables lazily. If no valid mapping exists, or the access violates
the page's permissions, the CPU faults and the kernel sends `SIGSEGV` (Linux) or raises an access
violation `0xC0000005` (Windows).

**2. Describe the memory layout of a Linux process.** [01]
From low to high addresses: the executable's text (code), read-only data, data and BSS; the heap
growing up from just after BSS (`brk`); a large region of `mmap`s for shared libraries, large
allocations, thread stacks and file mappings; and the main thread's stack at the top growing down,
with a guard gap below it. ASLR randomises the base of each region. `/proc/PID/maps` shows all of
it; VMMap is the Windows equivalent.

**3. Why does dereferencing `0x10` crash, but dereferencing a freed heap pointer usually does
not?** [01, 04]
Page zero and the pages around it are never mapped, so any access near null faults at once -
`0x10` is a null struct pointer plus a field offset. Freed heap memory usually stays mapped: the
allocator keeps the pages and reuses them, so a read returns stale or recycled data and a write
silently corrupts whoever owns the block now. That is why use-after-free is so much worse than a
null dereference.

**4. What is in a stack frame on x86-64?** [02]
The return address pushed by `call`, optionally the saved caller's `rbp` (frame pointer), saved
callee-saved registers, local variables, and space for outgoing arguments beyond the first six in
registers (SysV) or four (Win64, which also reserves 32 bytes of shadow space for the callee).
Arguments arrive in `rdi, rsi, rdx, rcx, r8, r9` on Linux and `rcx, rdx, r8, r9` on Windows; the
return value is in `rax`.

**5. How does a debugger produce a backtrace when frame pointers are omitted?** [02]
It uses unwind tables: DWARF CFI in `.eh_frame` on Linux, and `.pdata`/`.xdata` unwind info on
Windows x64. For each return address, the table says how to compute the caller's stack pointer
and where the return address and saved registers are, so the unwinder walks frame by frame without
`rbp`. If the stack is corrupted or the code has no unwind info, the walk goes wrong and you must
scan the stack manually for plausible return addresses.

**6. What does a glibc malloc chunk look like?** [03]
Each chunk has an 8-byte header before the user pointer holding its size, with the low three bits
as flags (`PREV_INUSE`, `IS_MMAPPED`, `NON_MAIN_ARENA`); the 8 bytes before that hold the previous
chunk's size when the previous chunk is free. Chunks are 16-byte aligned with a 32-byte minimum.
When free, the user area holds list pointers: `fd`/`bk` in the bins, or the tcache `next` pointer
and a key used to detect double frees.

**7. What are tcache, fastbins, and arenas?** [03]
The tcache is a small per-thread LIFO cache per size class (7 entries by default) that serves most
small allocations with no locking. Fastbins are per-arena singly linked lists for small sizes that
are not coalesced immediately. Arenas are independent heaps (the main one plus extra ones created
for threads under contention), each with its own lock, so threads do not all serialise on one
allocator lock.

**8. Why does heap corruption crash far from the bug?** [03]
The allocator does not check its metadata when you write past a block; it checks it later, when it
next touches the neighbouring chunk - a `free`, a coalesce, a `malloc` from that bin. That can be
milliseconds or hours later, on another thread, in unrelated code. So the crash site shows where
the damage was found, not where it was done, and messages like `free(): invalid next size` tell you
"someone wrote past the end of the previous block", not who.

**9. Name the main memory bug classes and their typical symptoms.** [04]
Null dereference (`SIGSEGV` at a small address), stack overflow (fault just below the stack
mapping, deep recursion in `bt`), buffer overflow (heap: allocator aborts later; stack: `stack
smashing detected` or a garbage return address), use-after-free (garbage or freed-pattern values,
crash in a virtual call), double free (`double free detected in tcache`), uninitialised read
(nondeterministic values), leak (steady RSS growth), integer overflow in sizes (tiny buffer, big
copy), iterator invalidation (UAF into a reallocated container), and data races (wrong totals,
corrupted containers).

## Part B - Tools

**10. What do you do first with a core dump you have never seen?** [06]
Check the binary matches (build id), load it with symbols, then: the signal and `si_code`
(`p $_siginfo`), the fault address, the faulting instruction (`x/i $pc`) and the register it used,
`bt` for the crashing thread, `thread apply all bt` for the rest, then the locals of the frames in
my code. From the address and instruction I classify it - null, wild pointer, stack overflow,
heap corruption abort - before forming any theory.

**11. What must you have to analyse a core on another machine?** [06]
The core, the exact binary that produced it (same build id), its debug symbols, and the exact
shared libraries it had loaded (listed in the core's `NT_FILE` note), because libc and others
differ between distributions. In gdb you `set sysroot` to a directory mirroring the library paths
and `set debug-file-directory` for the symbols. Without matching libraries, stacks through libc are
wrong.

**12. Why might a crashing process produce no core?** [06, 11]
`ulimit -c 0` (often `LimitCORE` in the systemd unit), a `core_pattern` pointing at a missing
directory or a pipe handler that discards it, a full disk, a setuid or otherwise non-dumpable
process, or a crash handler that never re-raises the signal or deadlocks. Test the dump path after
every deployment.

**13. What does `!analyze -v` do, and why should you not rely on it alone?** [07]
It finds the exception record, switches to the faulting context, walks the stack with symbols and
applies heuristics to name a faulting module and a bucket id for grouping. It is a good first look
but it blames whoever touched the corrupted memory, which for heap corruption is the victim, and
it can be misled by bad symbols. I confirm with `.ecxr`, `.exr -1`, `k`, `u @rip` and `dv` myself.

**14. Give the WinDbg commands you use most and their gdb equivalents.** [07]
`~*k` = `thread apply all bt`; `.ecxr` and `.exr -1` = `p $_siginfo` plus the signal frame; `k`
= `bt`; `dv` = `info locals`; `dt Type addr` = `p *(Type*)addr`; `u @rip` = `x/10i $pc`; `lm` =
`info sharedlibrary`; `!heap -p -a addr` (with PageHeap) has no direct gdb equivalent - ASan's
report or `heaptrack` fill that role; `!locks` = printing each mutex's `__owner` and matching LWPs
with `info threads`.

**15. What do `!heap -s`, `!heap -p -a` and PageHeap give you?** [07, 09]
`!heap -s` summarises every heap's committed and reserved size, used for growth and fragmentation.
With PageHeap (`gflags /p /enable app.exe /full`), each allocation sits against a no-access page,
so an overflow faults at the faulting instruction instead of corrupting silently, and
`!heap -p -a addr` shows the block and, with `+ust`, the stack that allocated it. It turns a
"crash far away" into a crash at the bug.

**16. ASan or Valgrind - which do you use when?** [08]
ASan needs a rebuild, runs at about 2x slowdown, and catches heap, stack and global overflows,
use-after-free, double free and leaks with allocation and free stacks. Valgrind Memcheck needs no
rebuild but runs 20-50x slower (typical figures from their docs), catches heap errors and
uninitialised reads (which ASan does not) but not stack or global overflows. I use ASan in CI and
staging, Valgrind for a binary I cannot rebuild or for uninitialised-value bugs.

**17. What do TSan and UBSan catch, and what are their limits?** [08]
TSan detects data races and lock-order inversions from happens-before tracking, even when the race
did not manifest in that run; it costs 5-15x CPU and lots of memory and cannot be combined with
ASan. UBSan catches undefined behaviour such as signed overflow, bad shifts, misaligned access and
out-of-bounds array indexes with known bounds, at low cost. Both only see code paths the test
executes.

## Part C - Leaks, growth and concurrency

**18. How do you tell a leak from legitimate growth from fragmentation?** [09]
A leak is memory no longer reachable that was never freed: LSan or Valgrind report it and RSS grows
proportionally to requests forever. Growth is reachable memory that keeps increasing - an unbounded
cache or queue - which leak checkers do not report but a heap profiler (heaptrack, massif, UMDH)
attributes to an allocation site. Fragmentation is freed memory the allocator cannot return:
heap in-use bytes (`mallinfo2`) are flat while RSS is high, and `malloc_trim` or a different
allocator changes it.

**19. How do you hunt a leak in a server that runs for months?** [09]
Plot RSS against request count from metrics to get bytes per request and which request types
correlate. Reproduce in staging with LSan or heaptrack attached, or on Windows take UMDH snapshots
hours apart with `+ust` and diff them. Two production dumps compared with `!heap -s` or a chunk
histogram show which size class grows; the dominant size often identifies the structure.

**20. How do you recognise a race from a crash dump?** [10]
The crash is in shared structure internals (red-black tree rebalance, hashtable insert) or in an
object with a freed-pattern or garbage vtable in a thread that freed nothing; it depends on load or
core count and vanishes under Valgrind or a debugger. `thread apply all bt` often shows another
thread mutating, erasing or destroying the same object - compare `this` pointers. The crashing
thread is the victim; TSan on the same scenario confirms the culprit.

**21. Walk me through diagnosing a deadlock from a dump.** [10]
Take two dumps a minute apart; identical stacks mean nothing is moving. List the threads blocked in
lock waits (`__lll_lock_wait`, `RtlEnterCriticalSection`). On Linux print each mutex: `__owner` is
the holder's LWP and `__lock = 2` means contended; on Windows `!locks` gives the owning thread of
each critical section, while SRW locks need inference from which thread is inside the protected
code. Build a holds / waits-for table and find the cycle.

**22. Why does adding a `printf` make a race disappear?** [10]
`printf` takes the stdio lock and makes a syscall, which serialises the threads and changes timing
by microseconds, closing the race window. Instead, record events into per-thread lock-free ring
buffers with timestamps and read them from the core after a crash, use TSan, or use kernel tracing
(bpftrace, ETW) that does not change the code path.

**23. What are real fixes for races and deadlocks, and what are not?** [10, 13]
Real: correct synchronisation (a mutex or atomics with proper ordering), removing sharing
(thread-local data, message passing, immutable snapshots), fixing lifetime (`shared_ptr`, owners
that join users before destroying), a documented and asserted lock order, `std::scoped_lock` for
multiple locks. Not fixes: sleeps, retries, recursive mutexes, or "could not reproduce, closed".

## Part D - Incidents and RCA

**24. A server is hung and the client wants it restarted now. What do you do?** [11]
Say yes, and ask for 60-120 seconds first to take two dumps 30-60 seconds apart, plus logs and
per-thread CPU, with a prepared script. Then restart, or fail over first if there is a standby and
keep the hung node for analysis. Without the dumps the root cause is guesswork and the hang will
recur.

**25. What are the first 15 minutes of a Sev-1?** [11]
Acknowledge with a next-update time and start an incident log; scope what is broken, for whom and
since when; classify severity and escalate; capture evidence while the faulty state exists; apply
the smallest mitigation that restores service (block input, flag off, fail over, restart, roll
back); verify recovery with real success metrics and send the second update.

**26. What should a server log so incidents can be diagnosed?** [11]
Structured lines with a precise timestamp and timezone, level, pid, kernel thread id, request id,
component, a stable event name and key fields; request start and end with duration and result;
build version and config at startup; periodic resource gauges (RSS, heap in use, threads, fds,
queue depths). Sensitive data masked. The thread id joins a log line to a thread in the core; the
request id joins events across threads and services.

**27. What can a crash handler safely do?** [11]
Only async-signal-safe things: `write` preformatted text, the fault address, thread id and request
id, `backtrace_symbols_fd` (after warming `backtrace` up at startup), on an alternate signal stack
so stack overflows are handled. Then restore the default action and re-raise so the kernel still
writes the core. Calling `malloc` or `printf` there can deadlock on locks the crashed code held and
lose the core entirely.

**28. How do you communicate with a bank during an incident?** [11]
One voice to the client, updates on a fixed announced cadence even when there is no news, and each
update with status, impact, known facts, actions, what we need from them and the next update time.
Facts are kept separate from hypotheses, no root-cause time is promised, no internal names or blame,
and data integrity is addressed explicitly.

**29. What is the difference between a trigger and a root cause?** [12]
The trigger is what exposed the problem at that moment - a traffic spike, an unusual input, a
schedule overlap. The root cause is the defect that made the system fail under it; removing it
makes the trigger harmless. The test is whether the system will survive the same trigger tomorrow.
"High load at month-end" is a trigger; "a race in session expiry that needs two concurrent requests
on one session" is a root cause.

**30. What sections does a formal RCA contain?** [12]
Header, executive summary in plain language, impact with numbers and a data-integrity statement, a
timestamped timeline with sources, root cause with an evidence chain where each claim is labelled
proven, inferred or hypothesis, trigger, contributing factors, 5-whys down to the process gap,
resolution (mitigation and permanent fix), owned and dated preventive actions, verification, and an
appendix with evidence excerpts.

**31. How do you use 5-whys without it becoming blame or guesswork?** [12]
Each answer must be backed by evidence or be a clear process statement, and you follow the most
important branch while noting others as contributing factors. You go past the bug to why review,
tests and tools did not catch it, and you never stop at a person: "a developer made a mistake" is
always followed by "why could the mistake reach production?".

**32. How do you write an RCA when you could not prove the root cause?** [12]
Issue it as preliminary: evidence collected, the most likely mechanism clearly labelled as a
hypothesis, what was ruled out and how, the mitigation in place, and the diagnostics added so the
next occurrence is conclusive (ring buffers, invariant asserts, PageHeap on one node). It stays open
with a review date. A confident wrong answer that fails a month later costs far more trust.

## Part E - Prevention and senior judgement

**33. Which defences would you put into a C++ server's build and CI, and why?** [13]
Warnings as errors with a strict set; ASan+UBSan and TSan builds running the full test suite;
libFuzzer targets on every parser of external input; clang-tidy or `/analyze` with a baseline so new
code is clean; hardening in the release build (`_FORTIFY_SOURCE`, stack protector, stack clash
protection, PIE/RELRO/NX, CET; on Windows `/GS`, CFG, ASLR, DEP). Each catches a different class
earlier, and hardening turns silent corruption into a clean, diagnosable abort.

**34. How does RAII change the memory bugs you see?** [13]
Destructors release every resource exactly once on every exit path, so leaks on error paths,
double frees and unreleased locks largely disappear. With a single owner (`unique_ptr`, containers)
and ids or `weak_ptr` instead of stored raw pointers, most single-threaded use-after-free goes too.
What remains is lifetime across threads, iterator invalidation and logic errors, which is where
review and TSan focus.

**35. You find a fix for a crash but cannot reproduce it. Do you ship it?** [10, 12]
I ship it if the evidence supports the mechanism and the fix is low risk, but I do not call the
incident root-caused. The RCA says what is proven and what is inferred, the fix ships with
detection - an assertion or counter on the suspected path, a ring buffer, or a sanitizer build in
staging replaying traffic - and the incident stays open for an observation period that includes
the high-risk windows. If the detector fires, I have the proof; if the crash recurs without it
firing, my theory was wrong.

**36. Tell me about a hard production crash you root-caused.** [all]
Use the RCA structure as the story: impact and constraints in one sentence; the evidence you
captured and why (the dump before the restart); what the dump showed and why the obvious reading
was wrong (heap corruption found far from the bug); how you narrowed it (log correlation by request
and thread id, a reproduction under ASan or PageHeap); the root cause versus the trigger; the fix
and the preventive action you added to the process. End with a number that shows it held, such as
zero recurrences over a quarter.
