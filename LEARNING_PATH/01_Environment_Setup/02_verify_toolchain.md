# 2. Verify the toolchain: build, test, debug, sanitize

**Time:** ~1 hour   **Prerequisites:** 01_vscode_wsl_dev_loop

## 1. In plain words

Before learning anything, prove that every tool you will rely on actually works on your machine:
the compiler with C++17, CMake with Ninja and a unit test, gdb with breakpoints, Valgrind and
AddressSanitizer catching a real bug. Each check takes minutes. Finding out in week 4 that
Valgrind was broken would cost you a weekend.

## 2. The real thing

Copy the `code/` folder of this course into Linux first:

```bash
mkdir -p ~/dev/setup && cp -r /mnt/d/Cpp_DSA/LEARNING_PATH/01_Environment_Setup/code/* ~/dev/setup/
cd ~/dev/setup
```

### Check 1: C++17 compiles

```bash
cb hello.cpp && ./hello
```

Expected output ends with `C++ standard: 201703` and `all C++17 checks passed`. If the compiler
reports `201703` you are on C++17; GCC 13 defaults to C++17 anyway, but we always pass `-std`
explicitly so the project builds the same on every machine and in CI.

### Check 2: CMake + Ninja + a test

```bash
cd ~/dev/setup/cmake_hello
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/greeter_app
```

Expected: `100% tests passed`. This is the exact shape of the tcpkv build.

### Check 3: gdb in the terminal

```bash
cd ~/dev/setup && cb hello.cpp
gdb -q ./hello
(gdb) break main
(gdb) run
(gdb) next
(gdb) next
(gdb) info locals
(gdb) print words
(gdb) continue
(gdb) quit
```

`print words` should show the vector contents thanks to the STL pretty printers that ship with
Ubuntu's gdb. If you see raw pointers instead, add `source /usr/share/gcc/python/libstdcxx/v6/printers.py`
style setup as described in 05_Memory_Debugging_RCA/05.

### Check 4: gdb inside VS Code

Open `~/dev/setup` in a WSL window, open `hello.cpp`, click in the gutter next to the first line
inside `main` to set a breakpoint, press `F5`. The debugger should stop there and show locals.

### Check 5: Valgrind and AddressSanitizer find the same bug

`buggy.cpp` reads one past the end of a heap array and leaks a buffer.

```bash
cb buggy.cpp && valgrind --leak-check=full ./buggy
```

Look for `Invalid read of size 4` and `definitely lost: 40 bytes in 1 blocks`.

```bash
cbs buggy.cpp && ./buggy_asan
```

Look for `heap-buffer-overflow` with the exact line number, then after the program exits,
`LeakSanitizer: detected memory leaks`. ASan is faster (about 2x slowdown) but needs a rebuild;
Valgrind is slower (20x to 50x) but runs on any binary. You will use both in 05_Memory_Debugging_RCA.

### Check 6: ThreadSanitizer finds a data race

```bash
cbt race.cpp && ./race_tsan
```

**If instead you see `FATAL: ThreadSanitizer: unexpected memory mapping`**, that is not your
code. Ubuntu 24.04's kernel randomises memory addresses with 32 bits of entropy, and the TSan
runtime in GCC 13 only understands up to 28 bits. It fails at random, so it may work once and
crash the next run. Fix it once per WSL boot:

```bash
sudo sysctl -w vm.mmap_rnd_bits=28
# make it permanent:
echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/99-tsan.conf
```

Or, without sudo, run one command with randomisation off: `setarch $(uname -m) -R ./race_tsan`.
The tcpkv CI workflow applies the same sysctl before its TSan job.

Look for `WARNING: ThreadSanitizer: data race`. Then open `race.cpp`, uncomment the
`std::lock_guard` line, rebuild, rerun: the warning disappears. That is the whole idea of
04_Concurrency in one minute.

### Check 7: a core dump

```bash
cb crash.cpp
ulimit -c unlimited
./crash            # prints "Segmentation fault (core dumped)"
ls core*           # Ubuntu 24.04 may route cores through apport; if no file appears:
cat /proc/sys/kernel/core_pattern
```

If `core_pattern` starts with `|/usr/share/apport/apport`, set a plain pattern for development:

```bash
sudo sysctl -w kernel.core_pattern=core.%e.%p
./crash && gdb -q ./crash core.crash.*    # inside gdb: bt
```

`bt` should show the crash inside `write_through_null`. Core dumps are the subject of
05_Memory_Debugging_RCA/06; this check only proves your machine can produce them.

## 3. Code

All files are in `code/`:

| File | Purpose |
|---|---|
| `hello.cpp` | Uses `std::optional`, `std::string_view`, structured bindings, `if constexpr`, `std::shared_mutex`, `std::filesystem` so a pre-C++17 compiler fails loudly. |
| `buggy.cpp` | Heap overflow read + leak for Valgrind/ASan. |
| `race.cpp` | Two threads incrementing an `int` for TSan. |
| `crash.cpp` | Null pointer write for the core dump check. |
| `cmake_hello/` | `CMakeLists.txt`, a tiny library, an app and a GoogleTest test fetched with FetchContent. |

## 4. Under the hood (senior-level)

- `-Wall -Wextra -Werror`: warnings become errors. Production C++ teams do this because a warning
  ignored today is a crash dump next quarter. The tcpkv resume bullet claims exactly this.
- `-g` emits DWARF debug information into the ELF file; gdb maps addresses back to lines with it.
  `-O0` disables optimisation so variables are not kept only in registers and stepping is faithful.
- ASan works by instrumenting every load and store at compile time and keeping "shadow memory"
  (1 byte of shadow per 8 bytes of application memory) that records which bytes are addressable.
  It also surrounds heap allocations with poisoned red zones; reading one past the end hits a red
  zone. Valgrind instead runs the unmodified binary on a synthetic CPU and tracks validity bits for
  every byte, which is why it is slower and needs no rebuild.
- TSan records, for every memory location, which threads accessed it and under which locks
  (a vector-clock algorithm). Two accesses with at least one write and no happens-before edge is a
  race. Memory cost is about 5x to 10x, so keep test inputs small.
- A core dump is the process's memory image plus register state written by the kernel on a fatal
  signal. `ulimit -c` is the size limit (0 disables); `kernel.core_pattern` decides the filename or
  the program that receives it on a pipe.

## 5. In production

The exact commands in this module are what a CI pipeline runs on every commit for a serious C++
service: a `-Werror` build, unit tests through `ctest`, one ASan+UBSan job and often one TSan job.
At your current job the equivalent on Windows is a `/W4 /WX` MSVC build plus Application Verifier
and PageHeap. Being able to say "I run ASan and TSan in CI and Valgrind before every release" is a
senior-level answer to "how do you keep a C++ server stable".

## 6. Try it

1. **Easy.** Make `hello.cpp` fail: change `-std=c++17` to `-std=c++11` in the `cb` function
   temporarily and read the first error. Done when you can say which C++17 feature it rejected.
2. **Easy.** In `buggy.cpp`, fix the overflow but keep the leak; confirm ASan now reports only the
   leak. Then fix the leak with `std::vector`; confirm a clean run.
3. **Medium.** In `cmake_hello`, add a second test that fails on purpose, run `ctest`, read the
   failure output, fix it. Done when you can explain `--output-on-failure`.
4. **Medium.** Build `race.cpp` without TSan and run it 20 times in a loop. Count how often the
   final value is wrong. Then explain why TSan reports the race every time even when the result
   happens to be right.

## 7. Interview questions

**Q: What does `-Werror` do and why do teams use it?**
Treats every warning as a compile error, so the build fails until the code is clean. It prevents
warning accumulation and catches real bugs (unused results, sign conversions, missing returns).

**Q: ASan vs Valgrind: when would you pick which?**
ASan during development and CI: fast, precise line numbers, needs a rebuild. Valgrind when you
cannot rebuild (third-party binary), need uninitialised-value tracking (`memcheck`), or want
cache/profiling tools (`cachegrind`, `callgrind`).

**Q: Why is `-O0 -g` used for debugging and `-O2 -g` for production symbols?**
At `-O0` every variable has a memory slot and lines map one-to-one, so stepping is faithful. At
`-O2` the compiler inlines, reorders and keeps values in registers, so frames and locals can be
"optimized out"; but production runs `-O2`, so production cores are read with `-O2 -g` symbols.

## 8. Recap

- Seven checks: C++17 compile, CMake+ctest, gdb terminal, gdb in VS Code, Valgrind, ASan, TSan, core dump.
- `-std=c++17 -Wall -Wextra -Werror -pthread` is the default flag set for the whole curriculum.
- ASan: compile-time instrumentation, shadow memory, fast. Valgrind: binary instrumentation, slow, no rebuild.
- TSan proves a race exists even when the output looks right.
- Core dumps need `ulimit -c unlimited` and a plain `core_pattern` on Ubuntu.
