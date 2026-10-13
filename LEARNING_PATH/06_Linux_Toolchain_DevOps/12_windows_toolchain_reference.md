# 12. Windows toolchain reference: MSVC, MSBuild, vcpkg, WinDbg, and Linux side by side

**Time:** ~2 hours   **Prerequisites:** 02, 03, 04, 07

## 1. In plain words

You already use most of these tools at work. This module does two things. First, it fills the
gaps: the parts of the Windows toolchain people use daily without knowing what is underneath
(what `/MD` really selects, why LNK2038 happens, how a PDB is matched to a crash dump). Second, it
lines everything up against the Linux tools from modules 01 to 11, one row per concept, so that in
an interview you can answer "how would you do X on Linux?" by saying "on Windows I did it with Y;
on Linux it is Z, and here is the one way they differ".

That comparison is your selling point. Many Linux C++ candidates have never seen a crash dump
from a customer machine; many Windows developers have never written a CMakeLists.txt. You will be
able to talk about both precisely.

Analogy: two cities with the same services under different names. Both have a post office, a
hospital and a bank. If you know where each is in one city and the name it goes by in the other,
you can work in either one.

## 2. The real thing

### 2.1 The pieces of the MSVC toolchain

| Piece | File | Linux equivalent |
|---|---|---|
| Compiler driver | `cl.exe` | `g++` / `clang++` |
| Linker | `link.exe` | `ld` (GNU ld, gold, lld, mold), usually called through `g++` |
| Static library tool | `lib.exe` | `ar` |
| Binary inspector | `dumpbin.exe` (= `link /dump`) | `objdump`, `readelf`, `nm`, `ldd` |
| Name demangler | `undname.exe` | `c++filt` |
| Build engine | MSBuild (`.sln`, `.vcxproj`) | Make, Ninja |
| Meta-build | CMake (also generates `.sln`) | CMake |
| Package manager | vcpkg, NuGet, Conan | vcpkg, Conan, `apt install lib*-dev` |
| Platform headers and libs | Windows SDK (`windows.h`, `kernel32.lib`) | glibc headers, `/usr/include`, `/usr/lib` |
| C runtime | UCRT (`ucrtbase.dll`) + `vcruntime140.dll` | glibc (`libc.so.6`) |
| C++ standard library | MSVC STL (`msvcp140.dll`) | libstdc++ (`libstdc++.so.6`) or libc++ |
| Debugger | Visual Studio debugger, WinDbg | GDB, LLDB |
| Debug info | `.pdb` files, separate from the binary | DWARF sections inside the ELF, or split `.debug` files |

The compiler is not on `PATH` by default. The **Developer Command Prompt** (or
`vcvars64.bat`, or `Enter-VsDevShell` in PowerShell) sets `PATH`, `INCLUDE`, `LIB` and `LIBPATH`
to the right MSVC and SDK versions. CI tasks and CMake's Visual Studio generator do this for you.
On Linux the compiler is simply in `/usr/bin`.

### 2.2 cl.exe flags against GCC flags

| Purpose | MSVC | GCC / Clang |
|---|---|---|
| Standard | `/std:c++17` | `-std=c++17` |
| Strict conformance | `/permissive-` (on by default in new VS projects) | default; `-pedantic` |
| Correct `__cplusplus` | `/Zc:__cplusplus` | always correct |
| Exceptions | `/EHsc` | on by default |
| No optimisation | `/Od` | `-O0` |
| Optimise | `/O2` (speed), `/O1` (size) | `-O2`, `-Os` |
| Whole program / LTO | `/GL` + `/LTCG` | `-flto` |
| Debug info | `/Zi` (PDB), `/Z7` (in .obj) | `-g` |
| Linker debug info | `/DEBUG` (`/DEBUG:FULL`) | part of `-g` |
| Warnings | `/W4` | `-Wall -Wextra` |
| All warnings | `/Wall` (very noisy) | Clang `-Weverything` |
| Warnings as errors | `/WX` | `-Werror` |
| Static analysis | `/analyze` | `clang-tidy`, `-fanalyzer` (GCC) |
| Define | `/DNAME=1` | `-DNAME=1` |
| Include dir | `/I path` | `-I path` |
| AddressSanitizer | `/fsanitize=address` (VS 2019 16.9+) | `-fsanitize=address` |
| UBSan, TSan, MSan | not available in MSVC | `-fsanitize=undefined,thread,memory` |
| Stack protector | `/GS` (default on) | `-fstack-protector-strong` |
| Control-flow guard | `/guard:cf` | `-fcf-protection` |
| Source charset | `/utf-8` | UTF-8 by default |
| Parallel compile | `/MP` | `make -j`, Ninja (default parallel) |
| Remove unused code | `/OPT:REF /OPT:ICF` (linker) | `-ffunction-sections -Wl,--gc-sections`, `--icf` (lld/gold) |

The **runtime library** switch has no direct GCC counterpart and is the source of most MSVC link
errors:

| Flag | Meaning | CMake `CMAKE_MSVC_RUNTIME_LIBRARY` |
|---|---|---|
| `/MD` | release CRT and STL as DLLs | `MultiThreadedDLL` (CMake default) |
| `/MDd` | debug CRT as DLLs (checked iterators, debug heap) | `MultiThreadedDebugDLL` |
| `/MT` | release CRT linked statically into the module | `MultiThreaded` |
| `/MTd` | debug CRT statically | `MultiThreadedDebug` |

Every object file and static library linked into one module must use the same choice. Mixing
gives `LNK2038: mismatch detected for 'RuntimeLibrary'` or for `'_ITERATOR_DEBUG_LEVEL'` (Debug
and Release objects mixed). On Linux there is one glibc and one libstdc++ for the whole process,
so this class of error does not exist; the nearest equivalent is mixing `_GLIBCXX_DEBUG` and
normal objects, or the libstdc++ dual ABI (`_GLIBCXX_USE_CXX11_ABI`).

### 2.3 Libraries: .lib, .dll and the import library

| Concept | Windows | Linux |
|---|---|---|
| Object file | `.obj` (COFF) | `.o` (ELF) |
| Static library | `.lib` (archive of `.obj`) | `.a` (archive of `.o`) |
| Shared library | `.dll` | `.so` |
| What you link against | the **import library** `.lib` that comes with the DLL | the `.so` itself |
| Executable format | PE/COFF (`.exe`) | ELF (no extension) |
| Exported by default | **nothing**: `__declspec(dllexport)` or a `.def` file | **everything** with default visibility; `-fvisibility=hidden` to opt out |
| Run-time search | app directory, system directories, `PATH` (simplified) | `RPATH`/`RUNPATH`, `LD_LIBRARY_PATH`, `ld.so.cache`, `/lib`, `/usr/lib` |
| Embedded search path | none (use app directory, `SetDllDirectory`, side-by-side manifests) | `-Wl,-rpath,'$ORIGIN/../lib'` |
| Load at run time | `LoadLibraryW` / `GetProcAddress` / `FreeLibrary` | `dlopen` / `dlsym` / `dlclose` |
| List dependencies | `dumpbin /dependents app.exe`, Dependencies tool | `ldd app`, `readelf -d app` |
| List exports | `dumpbin /exports x.dll` | `nm -D --defined-only x.so` |
| Position independent | DLLs are relocated (ASLR) using a relocation table | `-fPIC` required for `.so` |

The portable export macro every cross-platform library has:

```cpp
// include/tcpkv/export.hpp - CMake's GenerateExportHeader produces the same thing for you.
#pragma once
#if defined(_WIN32)
#  if defined(TCPKV_BUILDING_DLL)
#    define TCPKV_API __declspec(dllexport)   // compiling the DLL itself
#  else
#    define TCPKV_API __declspec(dllimport)   // compiling a consumer of the DLL
#  endif
#else
#  define TCPKV_API __attribute__((visibility("default")))   // with -fvisibility=hidden
#endif

class TCPKV_API Store { /* ... */ };
```

Link errors, translated:

| MSVC | GNU ld | Meaning |
|---|---|---|
| `LNK2019: unresolved external symbol "int __cdecl add(int,int)" (?add@@YAHHH@Z) referenced in function main` | `undefined reference to 'add(int, int)'` | declared, used, never defined in what you linked |
| `LNK2001: unresolved external symbol` | same | same, reference not inside a function (e.g. a vtable or data) |
| `LNK2005: "x" already defined in a.obj` | `multiple definition of 'x'` | ODR violation the linker can see (module 02) |
| `LNK1104: cannot open file 'foo.lib'` | `cannot find -lfoo` | library path or name wrong |
| `LNK2038: mismatch detected for 'RuntimeLibrary'` | (no equivalent) | `/MT` and `/MD` objects mixed |
| `LNK4098: defaultlib 'LIBCMT' conflicts with use of other libs` | (no equivalent) | two CRTs requested; same root cause |

The three causes of an unresolved symbol are the same on both platforms: the definition is
missing from the link, the names differ (C vs C++ linkage, a different signature, `__cdecl` vs
`__stdcall` on 32-bit), or, for Windows only, the function is in a DLL that does not export it.
`dumpbin /symbols foo.obj` and `undname` play the role of `nm -C`.

### 2.4 MSBuild and CMake on Windows

**MSBuild** reads XML project files: a `.sln` lists `.vcxproj` projects; each `.vcxproj` lists
sources and per-configuration settings (`Debug|x64`, `Release|x64`), usually inheriting from
`.props` property sheets. From a Developer prompt:

```bat
msbuild tcpkv.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal
msbuild tcpkv.vcxproj -t:Rebuild -p:Configuration=Debug -p:Platform=x64
```

In Azure Pipelines, the `VSBuild@1` or `MSBuild@1` task runs the same command and `VSTest@2` runs
tests (module 09).

**CMake on Windows** has two families of generators:

```bat
:: Visual Studio generator: multi-config. Produces a .sln you can open; config chosen at build time.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

:: Ninja + MSVC: single-config, faster, the same commands as Linux. Run from a Developer prompt.
cmake -S . -B build/ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/ninja
```

Visual Studio's "Open Folder" reads `CMakePresets.json` directly, so the presets from module 04
work on Windows with a few extra entries:

```json
{
  "name": "windows-msvc",
  "displayName": "MSVC x64 (Ninja)",
  "inherits": "base",
  "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
  "architecture": { "value": "x64", "strategy": "external" },
  "cacheVariables": {
    "CMAKE_C_COMPILER": "cl",
    "CMAKE_CXX_COMPILER": "cl",
    "CMAKE_BUILD_TYPE": "Debug",
    "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  }
}
```

Portable CMake for both compilers keeps compiler-specific flags behind generator expressions, as
`cmake/CompilerWarnings.cmake` in tcpkv does:

```cmake
target_compile_options(tcpkv_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /Zc:__cplusplus /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
set_property(TARGET tcpkv_core PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")   # /MD or /MDd
```

tcpkv itself uses POSIX sockets and would need a Winsock port to build on Windows: `WSAStartup`
once, `SOCKET` (an unsigned handle, compare with `INVALID_SOCKET` not `< 0`), `closesocket` instead
of `close`, `WSAGetLastError()` instead of `errno`, `ioctlsocket(FIONBIO)` instead of `fcntl`, no
`SIGPIPE` (send returns `WSAECONNRESET`), and `ws2_32.lib` in the link. That is a good
exercise (section 6).

### 2.5 vcpkg

vcpkg is Microsoft's source-based C and C++ package manager. It works on Windows, Linux and macOS,
and integrates with CMake through a toolchain file.

**Manifest mode** (recommended): a `vcpkg.json` in the project root declares dependencies, and
CMake installs them during configure into `build/vcpkg_installed`.

```json
{
  "name": "tcpkv",
  "version": "0.1.0",
  "dependencies": [ "gtest", "fmt", "libpq", "hiredis" ],
  "builtin-baseline": "<commit sha of the vcpkg registry, pins every port version>"
}
```

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh   # .bat on Windows
export VCPKG_ROOT=~/vcpkg
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

```cmake
find_package(fmt CONFIG REQUIRED)
target_link_libraries(tcpkv_core PRIVATE fmt::fmt)
```

**Triplets** name the target platform and linkage: `x64-windows` (DLLs, `/MD`),
`x64-windows-static` (static libs, `/MT`), `x64-windows-static-md` (static libs, `/MD`),
`x64-linux` (static by default). Choose with `-DVCPKG_TARGET_TRIPLET=...`. Pick the triplet that
matches your runtime library choice or you are back at LNK2038.

**Binary caching** stores built packages (locally under `%LOCALAPPDATA%\vcpkg\archives` or
`~/.cache/vcpkg/archives`, or in NuGet feeds / cloud storage) keyed by a hash of the port, its
dependencies, the triplet and the compiler. In CI, caching that directory turns a 20-minute
dependency build into seconds.

**Classic mode** (`vcpkg install fmt:x64-windows` into a global tree) is what older projects use;
it cannot pin versions per project. Conan is the other major choice: binary packages, a Python
recipe language, more flexible, more to learn. On Linux, `apt install libfmt-dev` is the third
choice, fine for tools, poor for reproducible builds (versions follow the distro).

### 2.6 Visual Studio debugger: the features worth knowing

- **Conditional breakpoints**: right-click a breakpoint, Conditions: `key == "acct42"` or hit
  count `>= 1000`. GDB: `break store.cpp:40 if ...`, `ignore`.
- **Tracepoints** (Actions: "Log a message", continue execution): `Store::set key={key} tid=$TID`.
  printf debugging without rebuilding. GDB: `dprintf`.
- **Data breakpoints** (Debug > New Breakpoint > Data Breakpoint): break when 1, 2, 4 or 8 bytes
  at an address change. Uses CPU debug registers, so at most four (x86-64). The tool for "who is
  corrupting this field". GDB: `watch -l`.
- **Threads, Parallel Stacks, Parallel Watch** windows: all threads' stacks merged into a graph;
  freeze and thaw threads to force an interleaving. GDB: `info threads`, `thread apply all bt`,
  `set scheduler-locking on`.
- **Exception Settings** (Ctrl+Alt+E): break on first-chance C++ exceptions or Win32 exceptions
  such as `0xC0000005` access violation, before any handler runs. GDB: `catch throw`,
  `handle SIGSEGV stop`.
- **Watch format specifiers**: `value,x` (hex), `ptr,10` (show a pointer as a 10-element array),
  `str,s8` (UTF-8 string), `var,!` (raw, bypass visualizers). GDB: `p/x`, `p *ptr@10`.
- **Natvis** files (`.natvis`) define how your types display in the debugger; GDB's equivalent is
  Python pretty-printers.
- **Symbols**: Tools > Options > Debugging > Symbols. Add the Microsoft symbol server and your own
  symbol store. "Just My Code" hides library frames; turn it off when a crash is inside the CRT.
- **Diagnostic Tools**: CPU usage, memory snapshots with diffing (heap growth between two points).
  Linux: perf, heaptrack, Massif (module 07).
- **Open a dump file**: File > Open > a `.dmp`, then "Debug with Native Only". Set the symbol and
  source paths first. Linux: `gdb ./tcpkv core.1234`.
- **Attach to Process** (Ctrl+Alt+P), including remote debugging with `msvsmon.exe`. Linux:
  `gdb -p PID`, `gdbserver`. Visual Studio can also debug Linux processes over SSH and WSL with
  its CMake support, which is worth knowing if you keep the IDE.

### 2.7 WinDbg (WinDbg Preview, now just "WinDbg")

The modern WinDbg from the Microsoft Store or `winget install Microsoft.WinDbg` has the same
engine as classic WinDbg with a better UI, a data-model command `dx`, JavaScript extensions and
Time Travel Debugging. The deep material, including dump analysis walkthroughs, is in
`05_Memory_Debugging_RCA/07_windbg_and_windows_crash_dumps.md`. Command reference with GDB
equivalents:

| Task | WinDbg | GDB |
|---|---|---|
| Use Microsoft public symbols | `.symfix` then `.reload` | `debuginfod` (`set debuginfod enabled on`) |
| Add own symbols | `.sympath+ C:\build\pdb` | `set debug-file-directory`, `symbol-file` |
| Automatic triage | `!analyze -v` | (none built in; `bt` + reading) |
| Switch to the exception context | `.ecxr` | (core already stops at the signal) |
| Show exception record | `.exr -1` | `p $_siginfo` |
| Current stack | `k`, `kb`, `kp` (with params), `kn` (frame numbers) | `bt`, `bt full` |
| All threads' stacks | `~*k` | `thread apply all bt` |
| Switch thread | `~3s` | `thread 3` |
| Select frame, locals | `.frame 2`, `dv` | `frame 2`, `info locals` |
| Display a type | `dt tcpkv!Store 0x1234` | `ptype`, `p *(Store*)0x1234` |
| Loaded modules | `lm`, `lmvm tcpkv` | `info sharedlibrary` |
| Critical sections / locks | `!locks`, `!cs -l` | `info threads` + `p mutex` (`__owner` field) |
| Heap state | `!heap -s`, `!heap -p -a ADDR` (with page heap) | ASan, Valgrind, `malloc_info` |
| Memory map | `!address -summary` | `info proc mappings`, `/proc/PID/maps` |
| CPU time per thread | `!runaway` | `top -H -p PID` |
| Write a dump | `.dump /ma C:\dumps\x.dmp` | `gcore PID` / `generate-core-file` |
| Record and replay | Time Travel Debugging (TTD) | `rr record` / `rr replay`, GDB `record` |

**Page heap** (`gflags /p /enable tcpkv.exe /full`, or Application Verifier) puts every
allocation at the end of a page followed by a guard page, so a heap overrun faults at the
offending instruction instead of corrupting memory silently. It is the Windows ancestor of what
ASan does with redzones.

### 2.8 ProcDump and Windows Error Reporting

ProcDump (Sysinternals) writes dumps of a running process when a condition triggers:

```bat
procdump -accepteula -ma -e tcpkv.exe C:\dumps            :: full dump on unhandled exception
procdump -ma -e 1 -f C0000005 tcpkv.exe C:\dumps          :: on first-chance access violation
procdump -ma -h tcpkv.exe C:\dumps                        :: hung window (GUI apps)
procdump -ma -c 90 -s 10 -n 3 tcpkv.exe C:\dumps          :: CPU >= 90 % for 10 s, 3 dumps
procdump -ma -m 2048 tcpkv.exe C:\dumps                   :: commit memory >= 2048 MB
procdump -ma -w tcpkv.exe C:\dumps                        :: wait for the process to start
procdump -ma -i C:\dumps                                  :: install as the system post-mortem debugger
```

For a service on a customer machine, the zero-install alternative is **WER LocalDumps**:
registry key `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\tcpkv.exe`
with `DumpFolder`, `DumpCount` and `DumpType = 2` (full). Every crash then leaves a `.dmp`.

Linux equivalents: `ulimit -c unlimited` plus `/proc/sys/kernel/core_pattern`, or
`systemd-coredump` with `coredumpctl list` / `coredumpctl gdb`; `gcore PID` for a dump of a live
process. The "dump when CPU is high" trigger has no standard tool; a script with `top -b` and
`gcore` does it, and `perf record` is usually the better answer for CPU problems anyway.

### 2.9 dumpbin and the rest of the inspection kit

```bat
dumpbin /dependents tcpkv.exe        :: DLLs it imports                     (ldd, readelf -d)
dumpbin /imports tcpkv.exe           :: every imported function            (nm -D --undefined-only)
dumpbin /exports tcpkv_core.dll      :: exported functions                 (nm -D --defined-only)
dumpbin /headers tcpkv.exe           :: PE headers, machine, subsystem, debug directory with PDB path (readelf -h -l)
dumpbin /symbols store.obj           :: COFF symbol table of an object     (nm store.o)
dumpbin /linkermember tcpkv_core.lib :: symbols indexed in a static library (nm -s libx.a)
dumpbin /disasm /section:.text x.obj :: disassembly                        (objdump -d)
dumpbin /pdbpath tcpkv.exe           :: which PDB the binary expects       (readelf -n: build-id)
undname ?add@@YAHHH@Z                :: int __cdecl add(int,int)           (c++filt _Z3addii)
```

Other tools and their Linux counterparts:

| Windows | Linux | Use |
|---|---|---|
| Dependencies (lucasg), Dependency Walker | `ldd`, `lddtree` | DLL / .so resolution |
| Process Explorer | `htop`, `/proc`, `lsof` | processes, handles, loaded modules |
| `handle.exe`, Resource Monitor | `lsof -p PID`, `ls -l /proc/PID/fd` | open files and sockets |
| Process Monitor | `strace`, `inotifywait`, `fatrace` | file, registry, process activity |
| TCPView, `netstat -ano` | `ss -tanp` | sockets per process |
| ETW, WPR + WPA, PerfView | `perf`, `bpftrace`, flame graphs | CPU and system tracing |
| Application Verifier, page heap | ASan, Valgrind | heap and handle misuse |
| Event Viewer | `journalctl` | system and service logs |
| Services (`sc.exe`, `services.msc`) | `systemctl` | service management |
| Task Manager | `top`, `htop` | overview |

### 2.10 The interview table

The one-page comparison to know cold:

| Area | Windows | Linux |
|---|---|---|
| Compiler | `cl.exe` (MSVC), also `clang-cl` | `gcc`/`g++`, `clang`/`clang++` |
| Build system | MSBuild `.sln`/`.vcxproj` | Make, Ninja; CMake generates for both |
| Warnings | `/W4 /WX` | `-Wall -Wextra -Werror` |
| Optimised with symbols | `/O2 /Zi` + `/DEBUG /OPT:REF /OPT:ICF` | `-O2 -g` (RelWithDebInfo) |
| Binary format | PE/COFF | ELF |
| Debug info | separate `.pdb`, matched by GUID + age | DWARF in the binary or split, matched by build-id |
| Static library | `.lib` | `.a` |
| Shared library | `.dll` + import `.lib`; nothing exported by default | `.so`; everything exported by default |
| Runtime library choice | `/MD` vs `/MT`; one CRT heap per module with `/MT` | one glibc per process |
| Library search | app dir, system dirs, `PATH` | rpath, `LD_LIBRARY_PATH`, ld.so.cache |
| Data model | LLP64: `long` is 32-bit | LP64: `long` is 64-bit |
| `wchar_t` | 16-bit, UTF-16 APIs (`CreateFileW`) | 32-bit; APIs take UTF-8 `char*` |
| x64 calling convention | args in RCX, RDX, R8, R9; 32-byte shadow space; no red zone | args in RDI, RSI, RDX, RCX, R8, R9; 128-byte red zone |
| Name mangling | `?add@@YAHHH@Z` (`undname`) | `_Z3addii` (`c++filt`) |
| Crash mechanism | SEH exception (`0xC0000005`) | signal (`SIGSEGV`) |
| Crash dump | minidump / full dump `.dmp` (WER, ProcDump) | core file (`core_pattern`, systemd-coredump) |
| Debugger | Visual Studio, WinDbg | GDB, LLDB |
| Memory errors | ASan (`/fsanitize=address`), page heap, App Verifier | ASan, UBSan, TSan, MSan, Valgrind |
| Profiler | VS Profiler, ETW/WPA, PerfView | `perf`, Valgrind callgrind |
| Syscall tracing | Process Monitor, ETW | `strace`, `perf trace` |
| Handles / fds | `HANDLE`, `CloseHandle` | `int` fd, `close` |
| Last error | `GetLastError()`, `WSAGetLastError()` | `errno` |
| Process creation | `CreateProcessW` | `fork` + `exec`, `posix_spawn` |
| Threads | `CreateThread`, `_beginthreadex`, `std::thread` | `pthread_create`, `std::thread` |
| Locks | `CRITICAL_SECTION`, `SRWLOCK` | `pthread_mutex_t` (futex) |
| Scalable I/O | IOCP (completion-based) | epoll (readiness), io_uring (completion) |
| Services | Service Control Manager | systemd |
| Logs | Event Log, ETW | journald, syslog |
| Memory commit | committed at allocation, fails early | overcommit, OOM killer later |
| Package manager | vcpkg, NuGet, Conan | vcpkg, Conan, apt |
| Shell | PowerShell, cmd | bash |
| CI | Azure Pipelines (`VSBuild@1`, `VSTest@2`) | GitHub Actions, Azure Pipelines on Linux agents |

## 3. Code

No new files in `code/`; the examples are inline because they need a Windows machine. To try the
Windows side of tcpkv's build:

1. Install Visual Studio 2022 Community with the "Desktop development with C++" workload (it
   includes CMake, Ninja, vcpkg and the Windows SDK).
2. Open a "Developer PowerShell for VS 2022" in a clone of your tcpkv repository on the Windows
   filesystem (not `\\wsl$`: builds over the 9P bridge are slow, the same reason module 01 gave
   for `/mnt/c` in the other direction).
3. Build the protocol library and tests only (the server needs the Winsock port):

```powershell
cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
cmake --build build/vs --config Debug --target tcpkv_tests
ctest --test-dir build/vs -C Debug --output-on-failure
dumpbin /dependents build\vs\tests\Debug\tcpkv_tests.exe     # expect MSVCP140D.dll, VCRUNTIME140D.dll, ucrtbased.dll
```

The `TCPKV_WERROR` option in tcpkv's `cmake/CompilerWarnings.cmake` already maps to `/WX` for
MSVC, and the warning set to `/W4 /permissive-`. MSVC and GCC do not flag exactly the same
constructs, so expect some warnings GCC did not give (C4267 `size_t` to `int`, C4996 "unsafe"
CRT functions, for example). Fixing them is exercise 2.

A minimal Winsock initialisation for the port, showing the differences in one place:

```cpp
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")         // or target_link_libraries(... ws2_32)
using socket_t = SOCKET;                       // unsigned: never compare with < 0
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int last_socket_error() { return WSAGetLastError(); }
inline void close_socket(socket_t s) { closesocket(s); }
struct WinsockInit {                           // RAII: one per process, before any socket call
    WinsockInit() { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) != 0) std::abort(); }
    ~WinsockInit() { WSACleanup(); }
};
#else
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int last_socket_error() { return errno; }
inline void close_socket(socket_t s) { ::close(s); }
struct WinsockInit {};                         // nothing to do on POSIX
#endif
```

## 4. Under the hood (senior-level)

**PE vs ELF loading.** A PE image has an **import directory**: for each DLL, a list of
functions, and an **Import Address Table** (IAT) the loader fills with addresses at load time
(all at once, unless delay-loaded). Calls go through `call [IAT slot]`. ELF uses the **GOT**
(global offset table) for data and function addresses and the **PLT** (procedure linkage table)
stubs for calls, traditionally resolved lazily on first call; `-Wl,-z,now` (full RELRO) resolves
at load time like Windows. Both then make the table read-only. ELF symbol lookup is by name across
all loaded objects in order (**symbol interposition**: `LD_PRELOAD` can replace `malloc` for the
whole process). Windows lookup is two-level: each import names the DLL it comes from, so two DLLs
can export the same name without conflict and there is no global interposition.

**Default visibility and its consequences.** Because ELF exports everything by default and
resolves by global name, an `.so` built without `-fvisibility=hidden` has a huge dynamic symbol
table, slower loading, and calls to its own exported functions go through the PLT (they could be
interposed). Windows DLLs export only what is marked, so this problem does not arise; the cost is
the `dllexport`/`dllimport` macro dance.

**One heap or many.** With `/MT`, each DLL and the EXE carry their own copy of the CRT, each with
its own heap. Memory allocated by `malloc` in one module and freed by `free` in another goes to the
wrong heap: corruption or a debug assertion. The same applies to `FILE*` and to `std::string`
crossing DLL boundaries. The rules on Windows: free memory in the module that allocated it, or use
`/MD` everywhere, or pass only C types across DLL boundaries. On Linux one `libc.so.6` serves the
whole process and the problem does not exist (unless you statically link libstdc++ into several
`.so` files, which recreates a milder version of it).

**PDB vs DWARF matching.** The linker writes a CodeView debug directory entry (`RSDS`) into the PE
containing the PDB's GUID, an age, and the PDB path. The debugger only loads a PDB whose GUID and
age match exactly; that is why you must archive the PDB of every shipped build (a **symbol store**,
`symstore.exe add`, served over HTTP). Linux's equivalent is the **build-id** note
(`readelf -n`), a hash of the binary's contents; GDB finds separate debug files by build-id under
`/usr/lib/debug/.build-id/` or through a **debuginfod** server. CI archiving the unstripped
binary per commit (module 09) is the low-tech version.

**SEH vs signals.** A Windows access violation is a structured exception: the kernel builds an
exception record and context, and user-mode dispatch walks registered handlers (table-based on
x64, from the `.pdata` section), which `__try/__except` and C++ `catch(...)` with `/EHa` can catch,
first chance then second chance. A Linux `SIGSEGV` is a signal delivered to a handler with a
`ucontext_t`; C++ exceptions are a separate mechanism (Itanium ABI unwinding with `.eh_frame`)
and cannot catch it. Both end with a dump/core if unhandled. "Catching" access violations to keep
a server running is a bad idea on both: the process state is unknown.

**LLP64 vs LP64.** Windows kept `long` at 32 bits on 64-bit platforms for source compatibility
with Win32 structs; Linux made `long` 64-bit. Code that stores a pointer or a `size_t` in a
`long` works on Linux and truncates on Windows; code that assumes `long` is 32-bit for a file
format works on Windows and breaks on Linux. Use `int64_t`, `uint32_t`, `size_t`, `intptr_t`.

**Calling conventions** matter when reading crash dumps without full symbols: on Windows x64
the first four integer arguments are in RCX, RDX, R8, R9, and the caller reserves 32 bytes of
shadow space; on Linux x86-64 (System V) the first six are in RDI, RSI, RDX, RCX, R8, R9, and
leaf functions may use a 128-byte red zone below RSP. Knowing this lets you recover a `this`
pointer from a register in a release-build dump on either platform.

## 5. In production

- **Your experience, framed.** "Our core-banking application server is built with MSBuild in
  Azure Pipelines, crashes at customer sites arrive as WER or ProcDump dumps, and I analyse them in
  WinDbg with our symbol store." Then: "On Linux the same pipeline is CMake and Ninja in GitHub
  Actions, crashes are cores collected by systemd-coredump, and I analyse them in GDB with the
  archived unstripped binary or debuginfod." That pair of sentences answers half the Linux questions
  an interviewer will ask a Windows developer.
- **Missing redistributable.** A build that runs on every developer machine fails on a clean server
  with "VCRUNTIME140.dll was not found". Developer machines have Visual Studio. Ship the VC++
  redistributable with the installer, or use `/MT` for a self-contained exe. The Linux version is
  the "cannot open shared object file" from module 09's 3 AM story: same root cause, different
  runtime.
- **Symbols you did not keep.** A customer sends a dump from build 4.2.17; nobody archived its PDB.
  Rebuilding the same source does not help: the new PDB has a different GUID. You are reduced to
  reading raw assembly and register values. Every release pipeline must publish PDBs (or
  unstripped ELF / `.debug` files) to a symbol store keyed by build, automatically.
- **Porting pitfalls in mixed teams.** `long` size, `wchar_t` size, path separators and drive
  letters, case-insensitive file names (an `#include "Store.hpp"` that works on Windows fails on
  Linux), CRLF in shell scripts, `SOCKET` vs `int`, `closesocket`, missing `SIGPIPE` handling when
  code moves from Windows to Linux, `localtime_s` vs `localtime_r`, `_stricmp` vs `strcasecmp`.
  CI on both platforms catches them on the commit that introduces them.
- **ASan on MSVC** is real and useful but narrower: no UBSan, TSan or LeakSanitizer in MSVC, and
  it requires matching runtime libraries. Teams with Windows-only code often keep a Linux or
  `clang-cl` CI job just to get the other sanitizers.
- **3 AM:** the Windows service hangs at a customer. Before restarting it, take
  `procdump -ma tcpkv.exe` (it does not kill the process), then restart. In WinDbg, `~*k` shows 40
  threads blocked in `RtlEnterCriticalSection`; `!locks` names the owning thread, whose stack is
  inside a logging call waiting on another lock held by thread 12: a lock-order inversion. On Linux
  the identical procedure is `gcore PID` (or `gdb -batch -x scripts/dump_stacks.gdb -p PID`),
  `systemctl restart`, then `thread apply all bt` and the mutex `__owner` field.

## 6. Try it

1. **(easy)** On Windows, build any small C++ program in a Developer prompt with
   `cl /std:c++17 /W4 /EHsc /Zi /O2 hello.cpp`, then run `dumpbin /headers`, `/dependents` and
   `/pdbpath` on it. On WSL, do the same with `g++ -O2 -g` and `readelf -h`, `ldd`, `readelf -n`.
   Done when you have a two-column table of what each command told you.
2. **(medium)** Build tcpkv's `tcpkv_core` and `tcpkv_tests` with the Visual Studio generator as in
   section 3, at `/W4 /WX`. Fix every warning in a way that also compiles cleanly with GCC
   `-Werror`. Done when both platforms build the tests with warnings as errors and all tests pass.
3. **(medium)** Reproduce LNK2038: build a static library with `/MT` and an executable with `/MD`
   that links it. Then fix it by aligning `CMAKE_MSVC_RUNTIME_LIBRARY`. Done when you can explain
   why Linux has no equivalent error.
4. **(medium)** Add a `vcpkg.json` with `fmt` to a copy of tcpkv, configure with the vcpkg toolchain
   file on both Windows and WSL, and use `fmt::format` in `log_line`. Done when both builds succeed
   from a clean checkout and you can name the triplet used on each.
5. **(hard)** Port `src/main.cpp` to Windows using the `socket_t` shim from section 3: Winsock init,
   `closesocket`, `WSAGetLastError`, `SetConsoleCtrlHandler` for Ctrl-C instead of `sigaction`.
   Done when the same `scripts/tcpkv_client.py --selftest` (run from Windows Python) passes against
   the Windows build and the Linux build.
6. **(hard)** Crash the Windows build deliberately (null dereference on a `CRASH` command), capture
   a dump with `procdump -ma -e`, open it in WinDbg, and run `.symfix; .reload; !analyze -v; .ecxr;
   kp`. Then do the equivalent on WSL with `coredumpctl gdb`. Done when you have both stack traces
   pointing at the same source line and can list each command's counterpart.

## 7. Interview questions

**Q: How does linking against a DLL differ from linking against a shared object?**
On Windows you link against an import library (`.lib`) that tells the linker which DLL exports
each symbol; at run time the loader fills the Import Address Table. Only explicitly exported
symbols (`__declspec(dllexport)` or a `.def` file) are visible. On Linux you link against the
`.so` itself, everything with default visibility is exported, calls go through PLT/GOT, and the
loader searches rpath, `LD_LIBRARY_PATH` and the cache instead of the application directory.

**Q: What does LNK2038 "mismatch detected for RuntimeLibrary" mean?**
Objects compiled with different C runtime options (`/MT` vs `/MD`, or Debug vs Release) are being
linked into one module. Each choice selects a different CRT and STL build with different object
layouts and heaps. Fix by using one runtime library for everything, in CMake with
`CMAKE_MSVC_RUNTIME_LIBRARY`, and choosing the matching vcpkg triplet.

**Q: What are /MD and /MT and which do you ship?**
`/MD` links the CRT and STL as DLLs shared by all modules (one heap, requires the VC++
redistributable on the target); `/MT` links them statically into each module (self-contained, but
each DLL gets its own heap, so memory and CRT objects must not cross DLL boundaries). For a product
with several DLLs, `/MD` plus a redistributable is the usual choice; for a single tool exe, `/MT`.

**Q: How do you get a crash dump from a production Windows service, and the Linux equivalent?**
Configure WER LocalDumps in the registry for the exe, or run ProcDump with `-ma -e`; for a hang,
take `procdump -ma` on demand. Analyse in WinDbg with the archived PDB: `!analyze -v`, `.ecxr`,
`kp`, `~*k`. On Linux: `ulimit -c`/`LimitCORE` plus systemd-coredump or `core_pattern`, `gcore`
for live processes, and GDB with the unstripped binary or debuginfod.

**Q: Why might code that works on Windows break on Linux (or the reverse) even with the same compiler family?**
Data model (`long` is 32-bit on Windows, 64-bit on Linux), `wchar_t` size, file-name case
sensitivity, path separators, line endings, default symbol visibility, SIGPIPE on socket writes,
different socket handle types and error retrieval, and APIs with `_s`/`_r` variants. Fixed-width
integer types, CI on both platforms and portable wrappers catch these.

**Q: What is vcpkg manifest mode and what is a triplet?**
Manifest mode declares a project's dependencies in `vcpkg.json` and pins versions with a
`builtin-baseline`; CMake's vcpkg toolchain file installs them per project at configure time.
A triplet names the target architecture, OS and linkage (`x64-windows`, `x64-windows-static`,
`x64-linux`), and must match the runtime-library choice of the project.

**Q: Compare WinDbg and GDB for a hang.**
Both attach to a live process or open a dump. WinDbg: `~*k` for all stacks, `!locks` to find
critical-section owners, `!runaway` for CPU per thread. GDB: `thread apply all bt`, the mutex's
`__owner` field for the owning thread's LWP, `top -H` for CPU per thread. The diagnosis is
identical: find who holds the lock everyone waits for and what it is waiting for.

## 8. Recap

- MSVC = `cl` + `link` + `lib` + MSBuild; Linux = `g++` + `ld` + `ar` + Make/Ninja; CMake drives both.
- `/W4 /WX /permissive- /EHsc /std:c++17` is the MSVC mirror of `-Wall -Wextra -Werror -std=c++17`.
- `/MD` vs `/MT` must match across a module; LNK2038 is the symptom; Linux has one runtime per process.
- DLLs export nothing by default and link through import libraries; `.so` exports everything and links directly.
- LNK2019/LNK2001 = undefined reference; LNK2005 = multiple definition; `dumpbin`/`undname` = `nm`/`c++filt`.
- vcpkg manifest mode + toolchain file + matching triplet; cache binaries in CI.
- VS debugger: conditional breakpoints, tracepoints, data breakpoints, Parallel Stacks, exception settings.
- WinDbg: `.symfix; .reload; !analyze -v; .ecxr; kp; ~*k; !locks`; ProcDump `-ma -e`; WER LocalDumps.
- PDB matched by GUID + age, ELF debug info by build-id: archive symbols for every shipped build.
- LLP64 vs LP64, `wchar_t` 2 vs 4 bytes, SEH vs signals, IOCP vs epoll: the differences interviewers probe.
