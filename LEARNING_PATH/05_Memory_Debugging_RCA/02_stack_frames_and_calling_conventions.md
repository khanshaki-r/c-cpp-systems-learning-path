# 02. Stack frames and calling conventions

**Time:** ~3 hours   **Prerequisites:** 01

## 1. In plain words

When one function calls another, the CPU needs to remember where to come back to, and the
callee needs space for its local variables. Both go on the stack. Each active function has a
slice of the stack called its frame. The frames are stacked like plates: the function running
now is on top, `main` is at the bottom. A backtrace is simply a walk down the plates, reading
the "come back here" address written on each one.

The rules for how arguments are passed, which registers a function may trash and how the frame
is shaped are called the calling convention. Linux and Windows use different ones on x86-64,
and you need both because you debug on both. The debugger relies on these rules plus extra
information the compiler leaves behind (DWARF on Linux, PDB files on Windows) to rebuild the
backtrace and show you variables. When the compiler optimises, some frames and variables
disappear; knowing why stops you from distrusting a correct backtrace or trusting a wrong one.

## 2. The real thing

### 2.1 What a frame holds (x86-64, frame pointer kept)

```
higher addresses
        |  caller's locals ...                      |
        |  arg 7, arg 8 ... (only if > 6 int args)  |  <- pushed by caller, SysV
        |  return address                           |  <- pushed by `call`
rbp --> |  saved rbp (caller's frame pointer)       |  <- pushed by `push rbp` in prologue
        |  callee-saved registers (rbx, r12-r15)    |
        |  local variables                          |
        |  spill slots (temporaries)                |
        |  outgoing args for the next call          |
rsp --> |  (red zone: 128 bytes below rsp, SysV only, leaf functions may use without moving rsp)
lower addresses
```

Prologue and epilogue the compiler emits at `-O0`:

```asm
push   rbp              ; save caller's frame pointer
mov    rbp, rsp         ; this frame's base
sub    rsp, 0x40        ; room for locals (kept 16-byte aligned)
...
leave                   ; mov rsp, rbp ; pop rbp
ret                     ; pop rip  <- jumps to the return address
```

So with frame pointers the frames form a **linked list**: `[rbp]` is the previous `rbp`,
`[rbp+8]` is the return address into the caller. A debugger (or `backtrace()`) can walk this
list with no other information. That is exactly what `x/2xg $rbp` in gdb shows you.

### 2.2 x86-64 System V ABI (Linux, macOS, BSD)

| Item | Rule |
|------|------|
| Integer/pointer args | `rdi, rsi, rdx, rcx, r8, r9`; the 7th onward on the stack |
| Floating args | `xmm0`-`xmm7` |
| Return value | `rax` (and `rdx` for 128-bit); floats in `xmm0`; large structs via hidden pointer in `rdi` |
| Callee-saved (preserved) | `rbx, rbp, r12, r13, r14, r15` |
| Caller-saved (clobbered) | `rax, rcx, rdx, rsi, rdi, r8-r11`, all `xmm` |
| Stack alignment | `rsp` is 16-byte aligned **before** `call` (so `rsp+8` aligned at function entry) |
| Red zone | 128 bytes below `rsp` usable by leaf functions without adjusting `rsp` |
| `this` pointer | first integer argument (`rdi`) |
| Variadic | `al` holds the number of vector registers used |

Debugging consequence: at a breakpoint **at function entry**, `rdi` is `this` or the first
argument, `rsi` the second. Once the function has run a few instructions those registers may
have been reused. At `-O0` the compiler stores the arguments to the frame immediately, which is
why `info args` always works at `-O0` and sometimes fails at `-O2`.

### 2.3 Windows x64 calling convention

| Item | Rule |
|------|------|
| Integer/pointer args | `rcx, rdx, r8, r9`; the 5th onward on the stack |
| Floating args | `xmm0`-`xmm3` (position-based: 2nd arg in `xmm1` even if 1st was int) |
| Return value | `rax`; floats `xmm0`; structs > 8 bytes via hidden pointer in `rcx` |
| Callee-saved | `rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15` |
| Caller-saved | `rax, rcx, rdx, r8-r11, xmm0-xmm5` |
| Shadow space (home space) | caller reserves **32 bytes** above the return address for the callee to spill `rcx, rdx, r8, r9` |
| Red zone | none |
| Stack alignment | 16 bytes at `call` |
| `this` pointer | `rcx` |
| Unwinding | mandatory **unwind metadata** in `.pdata`/`.xdata` (`RUNTIME_FUNCTION`, `UNWIND_INFO`) for every non-leaf function |

Debugging consequence: in WinDbg `kb` shows "child-SP, RetAddr, Args to Child" but on x64 the
first four args were in registers; the values `kb` prints are whatever is in the home space,
which is only meaningful if the callee spilled them there (debug builds do, release builds often
do not). `kv` adds the calling-convention column. Trust `dv` (locals via symbols) over `kb`.

### 2.4 How a debugger reconstructs the backtrace

There are three methods, in order of reliability:

1. **Unwind tables** (`.eh_frame` / `.debug_frame` CFI on Linux; `.pdata`/`.xdata` on Windows).
   For every code address they describe where the return address and saved registers are,
   relative to the current `rsp` or `rbp`. GCC emits `.eh_frame` by default even without `-g`
   because C++ exceptions need it. This is what gdb uses. It works with `-fomit-frame-pointer`.
2. **Frame pointer chain** (`[rbp]` -> `[rbp]` -> ...). Fast, needs no tables, used by
   `backtrace()` fallbacks, perf, many profilers. Breaks when any function in the chain was built
   with `-fomit-frame-pointer` (the default at `-O1` and above in GCC) - those frames are skipped
   or garbage follows.
3. **Heuristic stack scanning.** Look at every 8-byte word on the stack and ask "does this point
   into a code region just after a `call` instruction?" WinDbg does this when unwind info is
   missing ("Stack unwind information not available. Following frames may be wrong."); gdb's
   `x/40ag $rsp` lets you do it by hand. Produces false frames from stale return addresses that
   were never overwritten.

When you see `?? ()` frames in gdb or `0x00000000` frames in WinDbg, one of these methods ran
out: a module without unwind info (hand-written asm, JIT code, a stripped third-party library),
or the stack itself is corrupted (a buffer overflow wrote over the saved `rbp`/return address).

### 2.5 Why optimised builds have missing frames

| Optimisation | Effect on the backtrace |
|--------------|-------------------------|
| **Inlining** (`-O2`, `inline`, small functions) | The callee has no frame at all. With DWARF `DW_TAG_inlined_subroutine` gdb synthesises an "inlined frame" (`#1 0x... in level3 (...) at file.cpp:30` with no real `rbp`), so the frame appears but has no separate stack memory, and `finish` behaves oddly. Without debug info the function simply vanishes from `bt`. |
| **Tail-call optimisation** (`-O2`: `jmp` instead of `call; ret`) | The caller's frame is reused by the callee; the caller disappears. `bt` shows the callee's caller's caller. Marked by gdb as `[tail call frame]` when DWARF 5 call-site info is present. |
| **Frame pointer omission** (`-O1`+) | `rbp` is a general register; frame-pointer walkers fail; CFI-based unwinders are fine. `-fno-omit-frame-pointer` costs ~1% and makes `perf` and crash handlers reliable. Ubuntu 24.04 builds its packages with frame pointers for this reason. |
| **Register allocation / dead store** | Locals live in registers or are gone: `<optimized out>`. Arguments may be unrecoverable once the register is reused. `-Og` keeps more. |
| **Identical code folding, cloning** (`func.part.0`, `func.constprop.1`, `func.isra.2`) | Names in `bt` have suffixes: `.part` = outlined part, `.constprop` = specialised for a constant argument, `.isra` = scalar replacement of aggregates, `.cold` = unlikely path moved out of line. They are your function. |
| **`-fomit-frame-pointer` + no CFI** (asm, some JITs) | Unwinding stops; `??` frames. |

Rule of thumb for production: build with `-O2 -g -fno-omit-frame-pointer`, ship the binary
stripped, keep the debug info separately (next section). You lose almost no speed and gain
correct backtraces from both the debugger and the in-process crash handler.

### 2.6 Debug info: DWARF and PDB

**DWARF** (Linux, ELF) is stored in sections of the object itself:

| Section | Contents |
|---------|----------|
| `.debug_info` | types, variables, functions, their locations (register/stack offset) and inlining tree |
| `.debug_line` | address -> file:line mapping (what `list`, `bt` and `addr2line` use) |
| `.debug_frame` / `.eh_frame` | call frame information for unwinding |
| `.debug_str`, `.debug_abbrev`, `.debug_aranges`, `.debug_loclists` | strings, abbreviations, lookup tables, location lists for variables that move between registers |
| `.symtab` | ELF symbol table (function names and addresses) - this is **not** DWARF; `strip` removes it; `.dynsym` stays |

Flags: `-g` (DWARF 5 on GCC 13), `-g3` adds macro definitions, `-gsplit-dwarf` writes `.dwo`
files to shrink objects, `-gz` compresses. Debug info does **not** change the generated code;
`-g -O2` is the same machine code as `-O2`. Anyone who says "debug info slows it down" is
confusing `-g` with `-O0`.

**Separate debug files** (how distributions and sensible teams ship):

```bash
g++ -O2 -g -fno-omit-frame-pointer -o server server.cpp
objcopy --only-keep-debug server server.debug     # extract debug info
strip --strip-debug --strip-unneeded server       # ship this (keeps .dynsym)
objcopy --add-gnu-debuglink=server.debug server   # tell gdb where to look
readelf -n server | grep 'Build ID'               # same build-id in both files
```

gdb finds `server.debug` via the `.gnu_debuglink` section (same directory, `.debug/`
subdirectory, or `/usr/lib/debug/`), or via the **build-id**:
`/usr/lib/debug/.build-id/ab/cdef....debug`. The build-id is a hash of the code; a mismatched
debug file is rejected. **Archive the `.debug` file for every build you ship**, keyed by
build-id. A core from a client is useless without it.

**debuginfod**: a server that hands out debug files by build-id. Ubuntu runs one
(`https://debuginfod.ubuntu.com`); gdb 15 asks to enable it on first run. For your own builds you
can run `debuginfod -F /path/to/debugfiles` internally. For libc symbols on Ubuntu you can also
install `libc6-dbg`.

**PDB** (Windows): debug info is always in a separate `.pdb` file produced by `cl /Zi` and
`link /DEBUG`. The executable stores the PDB's path, GUID and age; the debugger loads only a PDB
with a matching GUID (`.symopt+ 0x40` forces a mismatch, dangerous but occasionally useful).
`/DEBUG:FULL` vs `/DEBUG:FASTLINK` matters: fastlink PDBs reference object files and are useless
on another machine. Public vs private symbols: a public PDB (stripped with `/PDBSTRIPPED`) has
function names but no locals/types; private PDBs have everything. Microsoft's symbol server
provides public PDBs for Windows DLLs. Your release pipeline should `symstore add` every PDB
into your own symbol store so `.sympath` can find them by GUID (module 07).

### 2.7 Reading a frame by hand in gdb

```
(gdb) info frame
Stack level 0, frame at 0x7fffffffe0d0:
 rip = 0x555555555189 in level3 (m02_backtrace_frames.cpp:33); saved rip = 0x5555555551e6
 called by frame at 0x7fffffffe100
 source language c++.
 Arglist at 0x7fffffffe0c0, args: c=6
 Locals at 0x7fffffffe0c0, Previous frame's sp is 0x7fffffffe0d0
 Saved registers:
  rbp at 0x7fffffffe0c0, rip at 0x7fffffffe0c8
```

"frame at" is the canonical frame address (CFA): the value of `rsp` before the `call`. "saved
rip" is the return address, stored 8 bytes below the CFA. "rbp at" tells you where the caller's
`rbp` is saved. `x/2xg 0x7fffffffe0c0` prints both. Do this once with the example program and
the stack stops being abstract.

## 3. Code

`code/m02_backtrace_frames.cpp` has four nested functions, each printing the address of a local
so you can see the stack grow downward, and `level3` calls glibc's `backtrace()` so you see an
in-process unwind next to gdb's.

```cpp
__attribute__((noinline)) static long level3(long c)
{
    volatile long local3 = c * 3;        // volatile forces a stack slot even at -O2
    std::printf("level3: &local3 = %p\n", (void *)&local3);
    print_native_backtrace();            // backtrace() + backtrace_symbols_fd()
    return local3 + 1;
}
static long level2(long b) { long local2 = b * 2; ...; return level3(local2) + 1; }
static long level1(long a) { long local1 = a + 1; ...; return level2(local1) + 1; }
```

Build both ways and compare:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 -fno-omit-frame-pointer -o build/m02_frames_O0 m02_backtrace_frames.cpp
g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O2 -o build/m02_frames_O2 m02_backtrace_frames.cpp

gdb -q ./build/m02_frames_O0
(gdb) b level3
(gdb) run
(gdb) bt
#0  level3 (c=6) at m02_backtrace_frames.cpp:33
#1  0x... in level2 (b=3) at m02_backtrace_frames.cpp:41
#2  0x... in level1 (a=2) at m02_backtrace_frames.cpp:48
#3  0x... in main (argc=1) at m02_backtrace_frames.cpp:55
(gdb) info frame
(gdb) x/2xg $rbp          # saved rbp, then return address == "saved rip" above
(gdb) p/x *(long*)($rbp+8) # the return address, by hand
(gdb) info symbol *(long*)($rbp+8)   # "level2 + 42 in section .text"
(gdb) up
(gdb) info locals          # local2
(gdb) finish               # "Run till exit from #0 ..." then "Value returned is $1 = 19"
```

Now the `-O2` binary: `level1` and `level2` are inlined into `main`; gdb still shows them as
frames thanks to DWARF but `info frame` for them says "inlined into frame 3", `local2` may be
`<optimized out>`, and `backtrace()` output (which uses unwind tables, not DWARF) shows only
`level3` and `main`.

Also look at the raw unwind data once, so it is not magic:

```bash
readelf --debug-dump=frames-interp build/m02_frames_O2 | head -60   # CFI rows: CFA = rsp+16, rip at cfa-8 ...
objdump -d --no-show-raw-insn build/m02_frames_O0 | grep -A12 '<_ZL6level3l>:'   # prologue: push rbp / mov rbp,rsp
addr2line -f -i -e build/m02_frames_O2 0x1189   # address -> function + inlined chain + file:line
```

## 4. Under the hood (senior-level)

**`call` and `ret` are the whole mechanism.** `call target` pushes the address of the next
instruction and jumps. `ret` pops into `rip`. Nothing else is architectural. Frames, frame
pointers and calling conventions are conventions between compilers. This is why overwriting the
return address (stack buffer overflow) hijacks control flow: `ret` happily jumps wherever the
corrupted word says. Stack protector (`-fstack-protector-strong`, MSVC `/GS`) puts a random
canary between locals and the return address and checks it before `ret`; a mismatch calls
`__stack_chk_fail` (SIGABRT, "*** stack smashing detected ***") or on Windows raises
`STATUS_STACK_BUFFER_OVERRUN 0xC0000409` via `__fastfail`. A crash *in `__stack_chk_fail`* is
therefore a diagnosis, not a mystery: a local array in the function that was about to return
overflowed.

**CFI is a state machine.** `.eh_frame` encodes, for each instruction address range, rules like
"CFA = rsp + 16; return address at CFA - 8; rbx saved at CFA - 24". The unwinder executes these
rules frame by frame. `readelf --debug-dump=frames-interp` shows the table. Hand-written assembly
without `.cfi_*` directives has no rows, and unwinding stops there. This is why a crash inside
an optimised `memcpy` from libc still unwinds fine (glibc's asm has CFI) but a crash inside a
JIT-generated buffer does not.

**Shadow stack / CET.** Modern CPUs (Intel CET, `-fcf-protection`) keep a hardware shadow stack
of return addresses; `ret` compares the two and faults on mismatch (`SIGSEGV` with `si_code
SEGV_CPERR` on Linux 6.6+, enabled per-process). Windows 11 exposes this as "Hardware-enforced
Stack Protection". It turns a return-address overwrite into an immediate, precise fault.

**Red zone and signal handlers.** The SysV red zone lets leaf functions use 128 bytes below
`rsp` without moving it. The kernel knows this and skips 128 bytes before pushing the signal
frame. A crash handler that corrupts the red zone (hand-written asm) corrupts the interrupted
function's temporaries.

**Variadic functions and `al`.** On SysV the caller sets `al` to the number of vector registers
used. `printf` with a `double` argument but `%d` format reads `rsi` instead of `xmm0` and prints
garbage; `-Wformat` catches it. Across the Windows x64 boundary, floating varargs are duplicated
in the integer register too, which is why `printf("%d", 1.5)` prints different garbage on each OS.

**Windows x64 unwinding is table-only.** There is no frame-pointer chain requirement; the
`UNWIND_INFO` for each function lists prologue operations (`UWOP_PUSH_NONVOL`, `UWOP_ALLOC_SMALL`,
`UWOP_SET_FPREG`). `RtlVirtualUnwind` interprets them. A function with wrong or missing unwind
info (hand-written asm without `.pdata`) breaks exceptions **and** stack walks; `kv` shows
"(FPO: ...)" markers and `.fnent address` dumps the entry for a function. This is also why SEH
works on x64 without any runtime cost on the non-exceptional path.

## 5. In production

- **The frame you crash in is often not the frame at fault.** `pc` in `memcpy`, `strlen`,
  `free`, `std::__cxx11::basic_string::_M_assign` means your code passed a bad pointer or size.
  Walk up (`up`, `.frame`) to the first frame in your own code and inspect its arguments.
- **"Following frames may be wrong"** in WinDbg, or `??` in gdb, right after a frame with a
  `char buf[N]` local: the stack was overwritten. The frames above it are untrustworthy; the
  function with the buffer is the suspect.
- **Mismatched symbols lie confidently.** A PDB from a different build or a debug file with a
  different build-id produces a backtrace with plausible-looking but wrong function names. gdb
  warns about build-id mismatch; WinDbg marks modules with `*** WARNING: Unable to verify
  checksum` or loads nothing. Always check `lm` / `info sharedlibrary` first. Keep every shipped
  build's debug info; a client core for a build whose symbols you deleted is a lost week.
- **Optimised backtraces in your crash handler.** `backtrace()` uses the unwind tables (good)
  but `backtrace_symbols_fd` needs `-rdynamic` or `.symtab` to print names; stripped binaries
  print `prog(+0x1234)`. Log the offsets anyway; `addr2line -e prog.debug 0x1234` recovers
  everything later. Log the build-id at startup so you know which `.debug` to use.
- **`-O2 -g` is the correct production build.** Not `-O0` (slow), not `-O2` without `-g`
  (undebuggable). The debug info goes in a separate file. Shakir's server should ship like this
  if it does not already; it is the single biggest improvement to post-crash diagnosis.
- **Windows: `kb` arguments are unreliable on x64.** Ask for locals with `dv` in each frame,
  or read registers at the time of the call with `.frame /r`. In release builds `dv` often shows
  `<value unavailable>`; then you read the disassembly and registers (module 07).

## 6. Try it

1. **Easy - read a frame.** With `m02_frames_O0` stopped in `level3`, use `info frame`,
   `x/2xg $rbp` and `info symbol` to show that `[rbp+8]` is the return address into `level2`.
   Done when you can draw the three frames with real addresses from your run.
2. **Easy - registers at entry.** `break level3`, `run`, `p $rdi`, `p c`. Then `ni` a few times
   and watch `p $rdi` change while `p c` stays. Done when you can explain where `c` lives
   after the prologue (`info address c`).
3. **Medium - inlining.** With `m02_frames_O2`, `break level3`, `run`, `bt`, `info frame` in
   frame 1 and 2. Compare with `backtrace()` output printed by the program. Done when you can
   explain why they differ.
4. **Medium - separate debug file.** Build `m04_null_deref` with `-O2 -g`, run `objcopy
   --only-keep-debug`, `strip`, `--add-gnu-debuglink`. Run gdb on the stripped binary (a) with
   the `.debug` file present and (b) renamed away. Done when `bt` shows source lines in (a) and
   only addresses in (b), and you can state the build-id of both files.
5. **Hard - stack scan by hand.** Crash `m04_null_deref ACC-9999` under gdb, then **without
   using `bt`** use `x/40ag $rsp` and `info symbol` on each plausible word to reconstruct the
   call chain. Done when your manual chain matches `bt`.
6. **Hard - Windows x64.** Build a 3-level program with MSVC `/O2 /Zi`, break in the innermost
   function in WinDbg, run `kv`, `.frame /r 1`, `dv`, `.fnent @rip`, and `dps @rsp L20`. Done
   when you can identify the shadow space and the return address in the raw dump.

## 7. Interview questions

**Q: What is in a stack frame?**
A: The return address pushed by `call`, usually the saved frame pointer of the caller, saved
callee-preserved registers, the function's locals and spill slots, and space for outgoing
arguments to callees. With frame pointers the frames form a linked list through the saved `rbp`
values.

**Q: How does gdb produce a backtrace when the compiler omitted frame pointers?**
A: From the call frame information in `.eh_frame`/`.debug_frame`: for each code address range
it records how to compute the previous frame's stack pointer and where the return address and
saved registers are. GCC always emits it because C++ exceptions need it. Without it (assembly
without CFI directives, JIT code) unwinding stops and you see `??` frames.

**Q: Name the argument registers for SysV and for Windows x64, and one practical difference.**
A: SysV: `rdi, rsi, rdx, rcx, r8, r9`; Windows: `rcx, rdx, r8, r9` plus a 32-byte shadow space
reserved by the caller. Practical difference: Windows has no red zone and requires unwind
metadata for every non-leaf function, so stack walking is table-driven; SysV leaf functions may
use 128 bytes below `rsp` without adjusting it.

**Q: Why does an optimised backtrace show fewer frames than the source suggests?**
A: Inlining removes callee frames (gdb can still show them as inlined frames if DWARF describes
them), tail-call optimisation replaces `call/ret` with `jmp` so the caller's frame disappears,
and functions may be split into `.part`/`.cold` clones. Variables shown as `<optimized out>`
lived in registers that were reused or were eliminated entirely.

**Q: Does `-g` make the program slower?**
A: No. `-g` adds DWARF sections and does not change code generation; `-O2 -g` is byte-identical
code to `-O2`. Slowness comes from `-O0`. The right production build is `-O2 -g` with the debug
info split into a separate file via `objcopy --only-keep-debug` and matched by build-id.

**Q: How does WinDbg know which PDB to load, and what happens with the wrong one?**
A: The executable's debug directory stores the PDB path, GUID and age; the debugger looks the
GUID up in the symbol path (local folders and symbol servers). A PDB with a different GUID is
refused unless you force it with `.symopt+ 0x40`, in which case the names it shows will be
wrong while looking plausible.

**Q: What does a crash inside `__stack_chk_fail` or exception code 0xC0000409 tell you?**
A: The stack protector detected that a canary between locals and the return address was
overwritten when a function was about to return. Some local buffer in that function (or one it
inlined) overflowed. On Windows 0xC0000409 is also used for other fail-fast conditions; check
the subcode in the exception parameters.

## 8. Recap

- `call` pushes the return address, `ret` pops it; everything else about frames is convention.
- With frame pointers: `[rbp]` = caller's `rbp`, `[rbp+8]` = return address, a linked list.
- SysV args: `rdi rsi rdx rcx r8 r9`; Win64 args: `rcx rdx r8 r9` + 32-byte shadow space.
- Callee-saved SysV: `rbx rbp r12-r15`; Win64 adds `rdi rsi xmm6-15`.
- Debuggers unwind with CFI (`.eh_frame` / `.pdata`), fall back to frame pointers, then to
  stack scanning; `??` frames mean the chain broke or the stack is corrupt.
- Inlining, tail calls and register allocation make frames and variables vanish at `-O2`;
  `-fno-omit-frame-pointer` and `-Og` help.
- DWARF lives in `.debug_*` sections; split it out with `objcopy --only-keep-debug` and match
  by build-id; PDBs are always separate and matched by GUID.
- Ship `-O2 -g -fno-omit-frame-pointer`, strip the binary, archive the debug file. Always.
