# gdb cheat sheet (with WinDbg equivalents)

Keep this open while doing modules 05 and 06. Everything here is in gdb 15 (Ubuntu 24.04).

## Starting

| Task | gdb | WinDbg |
|------|-----|--------|
| Debug a program | `gdb ./prog` then `run args` or `gdb --args ./prog args` | File > Launch executable |
| Load a core/dump | `gdb ./prog core` or `gdb -c core ./prog` | File > Open dump file |
| Attach to a process | `gdb -p PID` or `gdb ./prog PID` | File > Attach to process / `windbg -p PID` |
| Scripted / batch | `gdb -batch -ex 'bt' -ex 'info registers' ./prog core` | `cdb -z dump.dmp -c "!analyze -v; q"` |
| Quit | `q` | `q` |

## Running and stepping

| Task | gdb | WinDbg |
|------|-----|--------|
| Start, stop at main | `start` | `g main` after `bp main` |
| Continue | `c` | `g` |
| Step over line | `n` (`next`) | `p` |
| Step into | `s` (`step`) | `t` |
| Step one instruction | `ni` / `si` | `p` / `t` (in asm mode) |
| Run until current function returns | `finish` (prints the return value) | `gu` |
| Run to a line | `until 42` / `advance 42` | `g \`file.cpp:42\`` |
| Kill the program | `kill` | `.kill` |

## Breakpoints and watchpoints

| Task | gdb | WinDbg |
|------|-----|--------|
| Break at function / line | `b func` / `b file.cpp:42` | `bp module!func` / `bp \`file.cpp:42\`` |
| Conditional | `b file.cpp:42 if id == 7` | `bp addr "j (@rcx==7) ''; 'gc'"` |
| Temporary | `tbreak func` | `bp /1 func` |
| List / delete / disable | `info b` / `d N` / `disable N` | `bl` / `bc N` / `bd N` |
| Ignore first N hits | `ignore N 100` | `bp addr 100` |
| Run commands on hit | `commands N` ... `end` | `bp addr "cmd; cmd; g"` |
| Watch a variable (hardware) | `watch var` / `watch -l *(long*)0xADDR` | `ba w8 0xADDR` |
| Watch reads / both | `rwatch var` / `awatch var` | `ba r8 ADDR` |
| Break on C++ throw / catch | `catch throw` / `catch catch` | `sxe eh` (C++ EH = code e06d7363) |
| Break on syscall | `catch syscall write` | n/a (use `bp ntdll!NtWriteFile`) |
| Break on signal | `handle SIGPIPE stop print` | `sxe av` (access violation), `sx` to list |

## Stack and frames

| Task | gdb | WinDbg |
|------|-----|--------|
| Backtrace | `bt` | `k` |
| With locals in every frame | `bt full` | `kP` (params only; use `dv` per frame) |
| Innermost N / outermost N | `bt 5` / `bt -5` | `k 5` / n/a |
| Select frame | `frame 3` or `f 3`; `up`, `down` | `.frame 3` |
| Frame details (saved rip, rbp) | `info frame` | `.frame /r 3` |
| Locals / arguments | `info locals` / `info args` | `dv` / `dv /t /v` |
| All threads' stacks | `thread apply all bt` | `~*k` |
| Switch thread | `thread 2` | `~2s` |
| List threads | `info threads` | `~` |
| Thread that crashed | (gdb lands on it) | `~#s` |
| Raw stack scan for return addresses | `x/40ag $rsp` | `dps @rsp L40` |

## Data

| Task | gdb | WinDbg |
|------|-----|--------|
| Print expression | `p expr`, `p/x expr`, `p *ptr`, `p arr[0]@10` | `?? expr`, `dt type addr` |
| Print struct nicely | `set print pretty on` then `p *s` | `dt -r type addr` |
| Type of expression | `ptype expr` / `whatis expr` | `dt type` |
| Examine memory | `x/16xg ADDR` (16 giant hex), `x/s ADDR`, `x/10i $pc` | `dq ADDR L10`, `da ADDR`, `u @rip L10` |
| Registers | `info registers` / `p $rip` / `p/x $rax` | `r` / `r rip` |
| Set a variable / register | `set var x = 5` / `set $rax = 0` | `ed addr 5` / `r rax=0` |
| Call a function | `call func(1)` / `p func(1)` | `.call` (rare) |
| Signal info (fault address) | `p $_siginfo` and `p $_siginfo._sifields._sigfault.si_addr` | `.exr -1` |
| Disassemble | `disassemble` / `disassemble /s` (with source) | `uf func` / `u addr` / `ub addr` |
| Intel syntax | `set disassembly-flavor intel` | default |
| Memory mappings | `info proc mappings` | `!address` / `!address ADDR` |
| Shared libraries / modules | `info sharedlibrary` | `lm` / `lmvm module` |
| Find a symbol by address | `info symbol 0xADDR` | `ln 0xADDR` |
| Address of a symbol | `p &var` / `info address var` | `x module!var` |
| Search memory | `find /w 0xSTART, 0xEND, 0xDEADBEEF` | `s -d START L1000 deadbeef` |

## Symbols and sources

| Task | gdb | WinDbg |
|------|-----|--------|
| Separate debug file | `symbol-file prog.debug` / `set debug-file-directory DIR` | `.sympath srv*C:\sym*https://msdl.microsoft.com/download/symbols` |
| Debug symbol server | `set debuginfod enabled on` (`DEBUGINFOD_URLS`) | symbol server as above |
| Where libs come from (core) | `set sysroot /path/to/root` / `set solib-search-path DIR` | `.exepath` / `.sympath+` |
| Reload symbols | `sharedlibrary` | `.reload /f` |
| Verbose symbol loading | `set verbose on` | `!sym noisy` |
| Source directory | `directory /path/src` / `set substitute-path /build /home/me/src` | `.srcpath` / `.srcfix` |
| Show source | `list` / `list func` / `list 40,60` | `lsa addr` |

## Threads, processes, signals

| Task | gdb | WinDbg |
|------|-----|--------|
| Follow fork child | `set follow-fork-mode child` | `.childdbg 1` |
| Keep both after fork | `set detach-on-fork off` | n/a |
| Do not stop on a signal | `handle SIGUSR1 nostop noprint pass` | `sxi` |
| Non-stop mode | `set non-stop on` | n/a |
| Mutex owner | `p mtx` -> `__data.__owner` is the LWP id | `!locks` / `!cs -l -o` |
| CPU time per thread | n/a (`top -H -p PID` outside) | `!runaway` |

## Core dumps

| Task | gdb | WinDbg |
|------|-----|--------|
| Snapshot a live process | `gcore -o out PID` or inside gdb: `generate-core-file` | `.dump /ma C:\out.dmp` / ProcDump `-ma` |
| Verify binary matches core | gdb warns on mismatch; compare `readelf -n` build-ids | `!lmi module` / `.reload /f` complaints |
| What created the core | `file core` (outside gdb) / `info inferiors` | `vertarget` / `!peb` |

## Pretty printing and convenience

```
set print pretty on            # one field per line for structs
set print object on            # show dynamic type through base pointers
set print static-members off
set pagination off             # never "--Type <RET> for more--" in a long bt
set history save on            # keep command history across sessions
set confirm off
info pretty-printer            # is the libstdc++ printer loaded? (p vec should show "std::vector of length N")
p vec                          # std::vector of length 3, capacity 4 = {1, 2, 3}
p vec._M_impl._M_start[0]@3    # raw, when printers are missing (e.g. a core with a stripped libstdc++)
p *mp._M_t._M_impl._M_header._M_parent   # a std::map node, raw
p $_exitcode                   # after the program exits
p $_siginfo                    # after a signal
display/i $pc                  # show next instruction at every stop
tui enable  (or Ctrl-x a)      # text UI; layout src | asm | split | regs
python print(gdb.parse_and_eval("g_total"))   # Python API is always available
```

## Reverse debugging (mention)

```
record full          # start recording (slow, ~1000x); then run forward
reverse-next         # step backwards
reverse-continue     # run backwards to the previous breakpoint/watchpoint
record stop
```
`rr` (record-and-replay) is the practical tool for this on native Linux; it needs hardware
performance counters that WSL2 does not expose.
