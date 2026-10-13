# 05. gdb from zero to fluent

**Time:** ~4 hours   **Prerequisites:** 01, 02; `code/gdb_cheatsheet.md` open beside you

## 1. In plain words

gdb is the Linux debugger. It does what WinDbg does for you on Windows: run a program under
control, stop it where you say, show you variables, registers and the stack, and open crash
dumps. The commands are different and the interface is a terminal, but the concepts map one to
one. Because you already think in WinDbg, every command in this module is given next to its
WinDbg equivalent so you learn by translation rather than from zero.

The goal is fluency: not knowing every command, but being able to sit at a crashed or hanging
process and get to the answer without looking things up. That comes from doing the exercises
in this module with the programs in `code/`, not from reading.

## 2. The real thing

### 2.1 Building for debugging

| Build | Flags | Use |
|-------|-------|-----|
| Debug | `-g -O0 -fno-omit-frame-pointer` | stepping line by line; every variable visible; 3-10x slower |
| Debug-optimised | `-g -Og` | optimised but keeps debuggability; good default for dev |
| Production | `-g -O2 -fno-omit-frame-pointer` | what you ship; `<optimized out>` appears; backtraces fine |
| Macros too | `-g3` | `p SOME_MACRO` works |

`-g` never changes code; `-O0` does (module 02). Debug the production build when the bug only
shows there. Debug `-O0` when you need to step and inspect.

```bash
g++ -std=c++17 -Wall -Wextra -pthread -g -O0 -o build/m04_null_deref m04_null_deref.cpp
```

### 2.2 Starting and running

```
gdb ./prog                       # load binary
gdb --args ./prog arg1 arg2      # with arguments
gdb -p 12345                     # attach to a running process
gdb ./prog core.12345            # open a core dump (module 06)
gdb -q -batch -ex run -ex bt --args ./prog arg   # scripted: run, print bt, exit

(gdb) run            # r   : start (stops at breakpoints or signals)
(gdb) start          #      : run and break at main
(gdb) set args a b   #      : change arguments
(gdb) continue       # c    : WinDbg g
(gdb) kill
(gdb) quit           # q
```

Everything accepts unique prefixes (`b`, `c`, `n`, `s`, `p`, `bt`, `i r`). Enter repeats the last
command (press Enter to keep stepping). Tab completes symbols. `help cmd` and `apropos word`.

### 2.3 Breakpoints

```
(gdb) break main                      # b main           WinDbg: bp prog!main
(gdb) break m04_null_deref.cpp:52     # file:line
(gdb) break apply_fee if fee > 20     # conditional
(gdb) tbreak process_request          # one-shot
(gdb) break Account::deposit          # C++ method; use tab completion for templates/overloads
(gdb) rbreak ^handle_                 # regex: every function starting with handle_
(gdb) info breakpoints                # i b            WinDbg: bl
(gdb) delete 2 / disable 2 / enable 2 #                WinDbg: bc / bd / be
(gdb) condition 2 id == 7             # add/change condition
(gdb) ignore 2 100                    # skip first 100 hits
(gdb) commands 2                      # run these each time bp 2 hits
> silent
> printf "fee=%ld\n", fee
> continue
> end
```

Breakpoints in shared libraries not yet loaded: `set breakpoint pending on` (in the sample
`.gdbinit`). Breakpoints work by writing an `int3` (0xCC) byte at the address; gdb hides this
from you and single-steps over the original instruction when continuing.

### 2.4 Stepping

| gdb | Meaning | WinDbg |
|-----|---------|--------|
| `next` / `n` | next source line, step over calls | `p` |
| `step` / `s` | next source line, step into calls | `t` |
| `nexti` / `ni`, `stepi` / `si` | one machine instruction | `p`/`t` in asm mode |
| `finish` | run until the current function returns, print the return value | `gu` |
| `until` / `u` | like `next` but does not go backwards in loops | - |
| `advance 60` | run to line 60 | `g \`file:60\`` |
| `return` | force the current function to return now (dangerous) | - |

`step` into a function without debug info (libc) steps over it unless you `set step-mode on`.
`display/i $pc` shows the next instruction after every step; `display x` shows `x` every stop.

### 2.5 The stack

```
(gdb) bt                       # backtrace            WinDbg: k
(gdb) bt full                  # with locals          WinDbg: kP + dv
(gdb) bt 5   /  bt -5          # innermost 5 / outermost 5
(gdb) frame 2   / f 2          # select frame         WinDbg: .frame 2
(gdb) up / down                # move one frame
(gdb) info frame               # CFA, saved rip, saved regs (module 02)
(gdb) info locals              #                      WinDbg: dv
(gdb) info args
```

Frame 0 is where the program stopped. Variables and `list` are relative to the selected frame.
In a crash, go `up` until you reach your own code, then inspect arguments.

### 2.6 Looking at data

```
(gdb) print acct               # p acct      -> (Account *) 0x0
(gdb) p *acct                  # dereference (fails if 0x0: "Cannot access memory at address 0x0")
(gdb) p acct->balance
(gdb) p/x acct->balance        # hex;  /d /u /o /t (binary) /c /a (address+symbol) /s
(gdb) p g_ledger               # std::vector with pretty printer: "std::vector of length 3, ... = {{...}}"
(gdb) p g_ledger[1]            # operator[] works with the libstdc++ xmethods
(gdb) p g_ledger._M_impl._M_start[1]      # raw access when printers are absent
(gdb) p arr[0]@5               # five elements starting at arr[0]
(gdb) p sizeof(Account)  /  p offsetof(Account, balance)   # offsetof may need: p &((Account*)0)->balance
(gdb) p $rip  /  p/x $rax  /  info registers   # registers    WinDbg: r
(gdb) p $_siginfo              # signal details after a crash
(gdb) p $_siginfo._sifields._sigfault.si_addr   # the faulting address
(gdb) ptype acct               # type definition       WinDbg: dt Account
(gdb) whatis g_ledger          # just the type name
(gdb) x/16xg $rsp              # examine 16 giant (8-byte) words in hex at rsp     WinDbg: dq @rsp L10
(gdb) x/s acct->id             # as string                                          WinDbg: da
(gdb) x/8i $pc                 # 8 instructions                                     WinDbg: u @rip L8
(gdb) x/4xw 0x7ffff7dd1000     # 4 words (4-byte)
(gdb) set var acct->balance = 0           # modify                                  WinDbg: ed
(gdb) call std::printf("%s\n", acct->id)  # call a function in the inferior
(gdb) info symbol 0x555555555189          # which function is this address in     WinDbg: ln
(gdb) info address apply_fee              # where is this symbol
(gdb) info line *0x555555555189           # address -> source line
(gdb) list apply_fee  /  list 40,60  /  list   # show source
(gdb) disassemble /s             # current function with source interleaved        WinDbg: uf
```

`x` format letters: `x` hex, `d` decimal, `u` unsigned, `s` string, `i` instruction, `a` address,
`c` char. Size letters: `b` byte, `h` 2, `w` 4, `g` 8. `x/16xg` = 16 x 8-byte hex. Memorise this
one and `x/s`, `x/Ni`.

`set print pretty on` makes structs readable; it is in the sample `.gdbinit`.

### 2.7 Watchpoints (data breakpoints)

The most powerful tool for "who changed this value":

```
(gdb) watch g_total                        # stop whenever g_total is written (hardware, 4 available)
(gdb) watch -l acct->balance               # -l: watch the ADDRESS, even after acct goes out of scope
(gdb) watch *(long*)0x5555555592a0         # by address
(gdb) rwatch x  /  awatch x                # on read / on read-or-write
(gdb) watch x if x > 1000                  # conditional
```

x86-64 has 4 debug registers, each watching up to 8 bytes. Larger regions or more watchpoints
fall back to software watchpoints (single-stepping everything, thousands of times slower). When
it triggers, gdb prints old and new value and stops at the instruction *after* the write; `bt`
shows who did it. This is how you find "something is corrupting this field": watch the field's
address (`watch -l`) and continue until it fires with an unexpected stack.

WinDbg equivalent: `ba w8 <address>`.

### 2.8 Exceptions and signals

```
(gdb) catch throw                 # stop when any C++ exception is thrown (even if caught later)
(gdb) catch throw std::out_of_range    # specific type (gdb 9+)
(gdb) catch catch                 # stop at the catch
(gdb) catch syscall write         # stop on a system call
(gdb) catch fork  /  catch exec
(gdb) info signals                # table of signals and whether gdb stops
(gdb) handle SIGPIPE nostop noprint pass   # servers: SIGPIPE on closed sockets is normal
(gdb) handle SIGUSR1 nostop noprint pass
(gdb) signal SIGINT               # deliver a signal to the program and continue
```

`catch throw` is how you find where an exception that "came from somewhere" is thrown; `bt`
at that point shows the thrower. WinDbg: `sxe eh` (break on first-chance C++ exception,
code `e06d7363`).

### 2.9 Threads

```
(gdb) info threads                # list: Id, Target Id (LWP = kernel tid), Frame   WinDbg: ~
(gdb) thread 3                    # switch                                          WinDbg: ~3s
(gdb) thread apply all bt         # every thread's stack                            WinDbg: ~*k
(gdb) thread apply all bt full    # with locals
(gdb) thread apply 2 3 bt 5
(gdb) thread name worker-1        # label the current thread
(gdb) set scheduler-locking on    # only the current thread runs when you step
(gdb) set non-stop on             # other threads keep running while one is stopped (before run)
```

The `LWP` number in `info threads` is the kernel thread id (`gettid()`), the same number you see
in `top -H`, in `/proc/PID/task/`, in `pthread_mutex_t.__data.__owner` (module 10) and in your
logs if you log `gettid()`. Log it.

Default behaviour (all-stop): when one thread hits a breakpoint, all threads stop. Stepping
one thread lets the others run unless `scheduler-locking` is on.

### 2.10 Attaching to a running process

```bash
./build/m10_deadlock &            # hangs
gdb -p $(pgrep m10_deadlock)      # attach; the process freezes while attached
(gdb) thread apply all bt
(gdb) detach                      # let it continue
```

**ptrace permissions.** Ubuntu (and WSL) ship Yama with `/proc/sys/kernel/yama/ptrace_scope = 1`:
you may only attach to your own *descendant* processes. To attach to an arbitrary process you
own: `sudo gdb -p PID`, or temporarily `echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope`,
or give gdb the capability (`sudo setcap cap_sys_ptrace=eip $(which gdb)`). In containers you need
`--cap-add=SYS_PTRACE`. "ptrace: Operation not permitted" is this, not a gdb bug.

Attaching stops the process; in production that means requests stall. For a live server prefer
`gcore PID` (seconds of pause, then the process continues) and analyse the core offline (module
06). WinDbg's equivalent is non-invasive attach or ProcDump.

### 2.11 TUI and the environment

```
gdb -tui ./prog            # or Ctrl-x a inside gdb to toggle
(gdb) layout src           # source + command
(gdb) layout split         # source + asm + command
(gdb) layout regs          # add registers
(gdb) focus cmd            # Ctrl-x o cycles focus; arrow keys scroll the focused window
(gdb) refresh              # Ctrl-l when the screen is garbled
(gdb) winheight src 20
```

For a graphical experience with the same engine: VS Code's C++ extension on WSL drives gdb
(`"MIMode": "gdb"`), or `gdb -ex 'py import gdb' ...` with the `gdb-dashboard` script, or the
`pwndbg`/`gef` extensions that add heap and context views. Learn the raw commands first; the UIs
all break down to them.

### 2.12 `.gdbinit`

`code/gdbinit.sample` is a working starter; `cp` it to `~/.gdbinit`. Highlights:

```
set pagination off          # long bt without prompts
set print pretty on
set history save on
set disassembly-flavor intel
handle SIGPIPE nostop noprint pass
set debuginfod enabled on   # fetch libc/libstdc++ debug info by build-id automatically
define crashinfo ... end    # custom commands: signal, fault address, pc, instructions, bt
```

A project-local `.gdbinit` in the current directory is loaded too, if listed in
`add-auto-load-safe-path`. Teams keep one with `set substitute-path`, `directory`, and project
macros (`define dump_request`).

### 2.13 STL pretty printers

Without them `p vec` shows `_M_impl._M_start` and friends. Ubuntu auto-loads libstdc++'s Python
printers for the system libstdc++ (`info pretty-printer` lists them). With them:

```
(gdb) p g_ledger
$1 = std::vector of length 3, capacity 3 = {{id = "ACC-1\000\000\000\000\000\000", balance = 1000, pending = 0}, ...}
(gdb) p name           -> "shakir"
(gdb) p mp             -> std::map with 2 elements = {[1] = "a", [2] = "b"}
(gdb) p up             -> std::unique_ptr<Session> = {get() = 0x5555555592a0}
(gdb) p sp             -> std::shared_ptr<Session> (use count 2, weak count 0) = {get() = 0x...}
(gdb) p *sp            # dereference works
(gdb) p/r g_ledger     # /r: raw, bypass printers
(gdb) p g_ledger.size()   # calls the function (needs -O0 or non-inlined); printers make this unnecessary
```

For a core from another machine with a different libstdc++, load the printers manually (the
commented block in `gdbinit.sample`). Write your own printer for project types in 30 lines of
Python when you find yourself doing `p *(Account*)0x...` for the tenth time.

### 2.14 Reverse debugging (know that it exists)

```
(gdb) record full         # start recording after a breakpoint; everything after is replayable
(gdb) continue            # ... crash ...
(gdb) reverse-step  /  reverse-next  /  reverse-continue  /  reverse-finish
(gdb) record stop
```

`record full` is 1000x slower and limited (no AVX in some versions), fine for a short window.
**rr** (`rr record ./prog`, `rr replay`) records whole runs with ~2x overhead and gives you
reverse execution and deterministic replay of races; it is the single best tool for Heisenbugs
on native Linux. It needs hardware performance counters, which **WSL2 does not expose**, so plan
to use it on a Linux box or VM with PMU passthrough.

### 2.15 Scripting

```bash
gdb -q -batch -ex 'run' -ex 'bt' -ex 'info registers rip' --args ./build/m04_null_deref ACC-9999 2>&1 | tee crash.txt
gdb -q -batch -ex 'thread apply all bt' -p $(pgrep server) > stacks.txt    # poor man's profiler
gdb -q -batch -x commands.gdb ./prog core
```

The Python API (`python print(gdb.parse_and_eval("g_total"))`, `gdb.selected_frame()`,
`gdb.lookup_type`) lets you automate dump analysis: walk a linked list, dump every request in a
table, check invariants over all threads. A 50-line script that prints your server's internal
state from a core is worth building once.

## 3. Code

Use the module-04 programs. Session 1: a null dereference, start to finish.

```bash
cd code && ./build_debug.sh debug
gdb -q --args ./build/m04_null_deref_debug ACC-9999
(gdb) run
Program received signal SIGSEGV, Segmentation fault.
0x0000555555555281 in apply_fee (acct=0x0, fee=25) at m04_null_deref.cpp:52
52          acct->balance -= fee;
(gdb) p acct                        -> (Account *) 0x0
(gdb) p $_siginfo._sifields._sigfault.si_addr   -> (void *) 0x10
(gdb) x/i $pc                       -> mov rax, QWORD PTR [rax+0x10]     (reading acct->balance)
(gdb) bt
#0  apply_fee (acct=0x0, fee=25) at m04_null_deref.cpp:52
#1  process_request (id="ACC-9999") at m04_null_deref.cpp:60
#2  main (argc=2, argv=...) at m04_null_deref.cpp:66
(gdb) up
(gdb) list                          # see: Account *acct = find_account(id);  no null check
(gdb) p id                          -> "ACC-9999"
(gdb) p g_ledger                    # it is not in the ledger
```

Session 2: who modifies a value (watchpoint).

```bash
gdb -q ./build/m04_use_after_free_debug
(gdb) break audit_session
(gdb) run
(gdb) p *s                          # garbage or tcache pointer in id, since s was freed
(gdb) # now find the free: restart and watch the object
(gdb) start
(gdb) break open_session
(gdb) c
(gdb) finish                        # returns the Session*; "Value returned is $1 = (Session *) 0x5555555592a0"
(gdb) watch -l ((Session*)$1)->id   # watch the ADDRESS of the id field
(gdb) c                             # fires inside _int_free / tcache_put: glibc wrote the tcache next pointer
(gdb) bt                            # ... close_session <- main : there is your free
```

Session 3: threads and a hang.

```bash
./build/m10_deadlock_debug & sleep 1
gdb -q -p $!           # if "Operation not permitted": sudo gdb -p, or see 2.10
(gdb) info threads
(gdb) thread apply all bt
(gdb) thread 2
(gdb) frame 4          # the frame in transfer_a_to_b
(gdb) p g_connections_mtx._M_mutex.__data.__owner    # LWP of the holder: compare with info threads
(gdb) detach; q
kill %1
```

Session 4: C++ exceptions.

```bash
gdb -q ./build/m13_raii_fix_debug
(gdb) catch throw
(gdb) run
Catchpoint 1 (exception thrown), __cxxabiv1::__cxa_throw ...
(gdb) bt            # allocate_table <- main : the std::length_error thrower
(gdb) up 2 ; info locals
```

Do all four. Then do them again without looking at this file.

## 4. Under the hood (senior-level)

**ptrace.** gdb controls the inferior with the `ptrace` syscall: `PTRACE_ATTACH`,
`PTRACE_PEEKDATA/POKEDATA` to read/write memory, `PTRACE_GETREGS`, `PTRACE_SINGLESTEP`,
`PTRACE_CONT`. The traced process stops on every signal and the tracer is told first; this is why
gdb sees the SIGSEGV before the process dies and why `handle SIGPIPE nostop` matters (each
delivery is a round trip into gdb). ptrace is slow (one syscall per word for memory reads in old
gdb; `process_vm_readv` now), which is why software watchpoints and `record` crawl.

**Breakpoints.** gdb writes `0xCC` (`int3`) over the first byte of the instruction. Hitting it
raises SIGTRAP; gdb restores the byte, backs `rip` up by one, reports to you; on `continue` it
single-steps the original instruction, re-inserts `0xCC`, continues. Consequences: breakpoints in
shared code (a library mapped in another process) do not affect the other process because of
copy-on-write; a breakpoint inside a signal handler that runs on a corrupted stack can hang
things; and `x/i $pc` in a core never shows `0xCC` because cores are not from under gdb.

**Hardware watchpoints.** DR0-DR3 hold addresses, DR7 holds length/type; the CPU traps after the
access completes (hence gdb stops at the next instruction). Four slots system-wide per thread
context; the kernel virtualises them per process. A `watch` on a 16-byte struct uses two slots.

**`<optimized out>`.** DWARF location lists say where a variable is for each address range; if the
current `pc` is in a range where the value is in no register or memory (dead, or folded into
another value), gdb says so. It is honest, not broken. `-Og` and `-fno-omit-frame-pointer` reduce
it; `-fvar-tracking-assignments` (default with `-g -O2`) improves it.

**Disabling ASLR.** gdb starts the inferior with `personality(ADDR_NO_RANDOMIZE)`, which is why
addresses repeat between runs. `set disable-randomization off` restores production behaviour -
use it when a bug depends on layout.

**Why `finish` can print the wrong value.** It reads `rax`/`xmm0` per the ABI. For a struct
returned via hidden pointer or a type gdb cannot infer, it shows garbage or nothing. `p $rax`
after `finish` is the raw answer.

**debuginfod.** gdb hashes each loaded module's build-id and asks the server for the matching
debug file and source. First run on a new libc downloads ~100 MB into `~/.cache/debuginfod_client`.
It is what lets you see `__GI_raise` with line numbers in a backtrace through libc without
installing `libc6-dbg`.

## 5. In production

- **Attaching freezes the process.** On a live banking server that is an outage. Prefer
  `gcore` (brief pause) or let the crash handler produce a core, then debug offline. Attach only to
  a process that is already hung or to a canary instance.
- **Poor man's profiler.** `for i in 1..10: gdb -batch -ex 'thread apply all bt' -p PID` with a
  second between samples, then sort the stacks. Shows what a slow server is doing without any
  profiler installed. Each sample pauses the process for tens of milliseconds.
- **Scripted first-look.** Keep a `crash.gdb` with `crashinfo`, `thread apply all bt`, `info
  sharedlibrary`, `info proc mappings`, and run it on every core before a human looks:
  `gdb -batch -x crash.gdb ./server core > first_look.txt`. Attach the output to the ticket.
- **Symbols on the production box.** You will not have debug info there. Copy the core *and* the
  exact binary (and libc) to your machine where the `.debug` file lives (module 06). Never
  debug on the client's box longer than it takes to copy the core.
- **Watchpoints for "who corrupts this field"** work on a reproduction, not on a core. If the
  corruption is reproducible under load, a watchpoint on the field's address plus `commands` that
  print `bt` and `continue` logs every writer without stopping the test.
- **Signals your server uses.** A server that uses `SIGUSR1` for log rotation or `SIGPIPE` on
  disconnects will "stop" constantly under gdb until you `handle ... nostop noprint pass`. Put it
  in the project `.gdbinit`.
- **WinDbg habit translation.** `!analyze -v` -> there is no equivalent; `crashinfo` + `bt` is it.
  `dt` -> `p *` and `ptype`. `dv` -> `info locals`. `~*k` -> `thread apply all bt`. `.frame` ->
  `frame`. `dps @rsp` -> `x/40ag $rsp`. The cheat sheet has the full table.

## 6. Try it

1. **Easy - the null deref.** Do Session 1 and write down: signal, fault address, faulting
   instruction, the frame in your code, the line, the fix. Done when it takes under 3 minutes.
2. **Easy - stepping.** `start` `m02_backtrace_frames_debug`, `step` into `level1`, `level2`,
   `level3`, `finish` out of each, reading the return values. Done when you can predict each
   `Value returned` before `finish` prints it.
3. **Medium - watchpoint hunt.** Do Session 2 and identify the exact glibc function that overwrote
   the freed object's first bytes. Then repeat with `watch -l` on `balance` instead. Done when
   you can explain why one fires inside glibc and the other does not fire at all.
4. **Medium - attach and deadlock.** Do Session 3; produce a two-line statement "thread A (LWP x)
   holds M1 and waits for M2; thread B (LWP y) holds M2 and waits for M1". Done when the LWP
   numbers are the real ones from your run.
5. **Hard - a pretty printer.** Write a Python pretty printer for `Account` (from
   `m06_crash_example.cpp`) that prints `ACC-1: 1000 (pending 0)`, register it from `.gdbinit`,
   and show `p g_ledger` using it. Done when `p g_ledger` prints a clean one-line-per-account list.
6. **Hard - scripted triage.** Write `crash.gdb` that prints the crashinfo, all thread stacks,
   shared libraries and mappings, and run it in batch mode against a core from
   `m06_crash_example`. Done when a colleague could read `first_look.txt` and name the bug class.

## 7. Interview questions

**Q: How do you find what is writing garbage into a field?**
A: A hardware watchpoint on the field's address: `watch -l obj->field`, continue, and when it
fires `bt` shows the writer. If the writes are frequent, add `commands` to print the stack and
continue so you get a log of all writers. On Windows the equivalent is `ba w8 address`.

**Q: What is the difference between `next`, `step` and `finish`?**
A: `next` executes one source line stepping over calls; `step` enters called functions that have
debug info; `finish` runs until the current function returns and prints the return value. `ni`
and `si` are the instruction-level versions.

**Q: Why might `info locals` show `<optimized out>` and what do you do?**
A: In optimised code the variable lives in a register that was reused or was eliminated; DWARF
location lists tell gdb it has no value at this `pc`. Move to a different line where it is live,
read the register or memory it was derived from via disassembly, or rebuild with `-Og`/`-O0` if
the bug reproduces there.

**Q: You run `gdb -p PID` and get "Operation not permitted". Why?**
A: Yama's `ptrace_scope` (1 on Ubuntu) restricts attaching to descendant processes. Use `sudo`,
set `ptrace_scope` to 0 temporarily, grant `cap_sys_ptrace`, or in Docker add `SYS_PTRACE`. Also
check you own the process and it is not already traced.

**Q: How do you stop at the point where a C++ exception is thrown?**
A: `catch throw` (optionally with a type) sets a catchpoint on `__cxa_throw`; when it fires,
`bt` shows the throwing frame and you can inspect its locals before unwinding destroys them.
`catch catch` stops at the handler instead.

**Q: Why does attaching gdb to a production server need care?**
A: ptrace stops the whole process while attached; requests stall and timeouts fire. Reading
memory and stepping is slow. Prefer `gcore` for a snapshot or a crash-produced core, and analyse
offline with the matching debug symbols. Attach live only for a process that is already hung.

**Q: What does gdb do differently from a plain run that can hide bugs?**
A: It disables ASLR (`disable-randomization on`) so layout-dependent bugs may vanish; it changes
timing (races may not reproduce); breakpoints and stepping serialise threads; and the environment
differs (`LINES`, `COLUMNS` are set). `set disable-randomization off` and running outside gdb with
a core dump are the countermeasures.

## 8. Recap

- Build `-g -O0` to step, `-g -O2 -fno-omit-frame-pointer` to match production; `-g` is free.
- `run`, `b`, `c`, `n`, `s`, `finish`, `bt`, `frame`, `info locals`, `p`, `x/16xg`, `x/s`,
  `x/8i $pc`, `info registers`, `p $_siginfo` - the core twenty commands.
- `watch -l addr` finds who writes a value; 4 hardware slots; `commands` logs without stopping.
- `catch throw` finds where an exception is thrown; `handle SIG nostop noprint pass` silences
  signals the program uses normally.
- `info threads` (LWP = kernel tid), `thread N`, `thread apply all bt`; `set scheduler-locking on`.
- `gdb -p PID` needs ptrace permission (Yama `ptrace_scope`); attaching freezes the process -
  prefer `gcore` in production.
- `.gdbinit`: pagination off, pretty on, history, debuginfod, custom `define`s; STL pretty
  printers make `p vec` readable.
- `record`/`rr` give reverse execution; rr needs perf counters (not in WSL2).
- Script it: `gdb -batch -x crash.gdb ./bin core` for a first look at every core.
