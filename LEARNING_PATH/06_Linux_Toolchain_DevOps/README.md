# 06. Linux, GCC, CMake, Git, Docker and CI/CD for a Windows C programmer

## What this course covers

You have spent 2.5 years shipping production C on Windows with Visual Studio, MSBuild and WinDbg.
Almost every C and C++ job you will interview for in late 2026 assumes you can do the same work
on Linux: build with GCC or Clang, describe the build with CMake, test with GoogleTest, keep history
in Git, ship in a Docker image and let a CI pipeline prove every commit still builds and passes.

This course takes you from "I have WSL installed" to "I can set up a Linux C++ project from an empty
directory, with CMake, tests, sanitizers, a multi-stage Dockerfile and a green CI badge, and I can
explain what every line does". Every module ends with commands you type into WSL Ubuntu 24.04.

The running example is **tcpkv**, a small TCP key-value server in C++17 that you also build in
08_Networking. Here it is the thing we build, test, containerise and ship. The `code/` folder
contains a complete CMake skeleton for it that you can copy into your own repository.

## Why it is on the resume

- "Linux, GCC, CMake, GDB, Git, Docker, CI/CD (Azure DevOps, GitHub Actions)" is a tool list.
  Interviewers probe tool lists. "Tell me how a shared library is found at run time" or
  "what does PRIVATE mean in target_link_libraries" are routine screening questions.
- "Builds clean under -Wall -Wextra -Werror with CMake; GoogleTest suite" is a claim about a
  project. You must be able to show the CMakeLists.txt and explain why it is structured that way.
- Your Windows background is an asset only if you can **compare** it with Linux precisely:
  `.lib`/`.dll` vs `.a`/`.so`, MSBuild vs CMake+Ninja, WinDbg vs GDB, `/W4 /WX` vs `-Wall -Werror`.

## Prerequisites

- C at a professional level (you have this).
- Basic C++17 syntax (classes, `std::string`, `std::vector`). You are learning this in parallel;
  the C++ in this course is deliberately plain.
- WSL Ubuntu 24.04 installed. Verify: open a terminal and run `wsl -l -v`; it should show
  `Ubuntu-24.04` with `VERSION 2`.
- Nothing else. Module 01 installs every tool used later.

## First 10 minutes: set up the toolchain

Open Ubuntu (Start menu -> Ubuntu 24.04) and run:

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential gdb cmake ninja-build git clang clang-tidy clang-format \
    valgrind strace ltrace linux-tools-generic htop lsof tcpdump python3 python3-pip \
    pkg-config curl wget unzip tree hyperfine
gcc --version      # expect 13.x
g++ --version
cmake --version    # expect 3.28.x on Ubuntu 24.04
git --version
```

Docker Desktop for Windows with the WSL2 backend is covered in module 08; install it when you get there.

## Modules

| #  | File | Topic | Hours |
|----|------|-------|-------|
| 01 | `01_linux_for_a_windows_developer.md` | WSL2, shell, filesystem, permissions, processes, pipes, grep/sed/awk, apt, ssh | 3 |
| 02 | `02_how_a_cpp_program_is_built.md` | Preprocess, compile, assemble, link; ELF, symbols, static and shared libraries, the dynamic loader | 3 |
| 03 | `03_gcc_and_clang_flags_that_matter.md` | Optimisation, debug info, warnings, sanitizers, defines, include and library paths; MSVC equivalents | 2 |
| 04 | `04_make_and_cmake_from_scratch.md` | Make, then CMake targets, visibility, build types, presets, Ninja, FetchContent, install; tcpkv layout | 3 |
| 05 | `05_testing_with_googletest_and_ctest.md` | GoogleTest, fixtures, parameterised tests, death tests, gMock, ctest, sanitizers in tests | 2.5 |
| 06 | `06_git_properly.md` | Object model, branching, rebase, conflicts, bisect, reflog, PR workflow, semver, GitHub profile | 3 |
| 07 | `07_debugging_and_profiling_tools_on_linux.md` | gdb quick ref, strace, ltrace, perf, callgrind, /proc, lsof, ss, tcpdump, hyperfine | 2.5 |
| 08 | `08_docker_for_cpp.md` | Images, containers, multi-stage builds, layers, volumes, networking, compose with PostgreSQL and Redis | 3 |
| 09 | `09_ci_cd_github_actions_and_azure_devops.md` | GitHub Actions matrix build, sanitizers, Valgrind, caching, badges; azure-pipelines.yml; secrets | 2.5 |
| 10 | `10_scripting_bash_and_a_little_python.md` | Bash for build/test/deploy, traps, load generation; a Python socket client for tcpkv | 2 |
| 11 | `11_linux_systems_programming_essentials.md` | Syscalls, file I/O, mmap, signals, fork/exec, pipes, epoll preview, errno, systemd, ulimit, OOM | 3 |
| 12 | `12_windows_toolchain_reference.md` | MSVC, MSBuild, vcpkg, VS debugger, WinDbg Preview, ProcDump, dumpbin; Windows vs Linux side by side | 2 |

Total: about 31 hours. Suggested pace: two modules a week alongside the other courses, so about six weeks.
Modules 01-04 are the foundation and should be done in order. 05-09 can be reordered.
Do 10-12 when you have a real repository to apply them to.

## Suggested order relative to the other courses

- Do 01 and 02 before starting any C++ course that asks you to compile on WSL.
- Do 04 and 05 before you write the tcpkv code in 08_Networking, so that the project starts
  with CMake and tests from the first commit.
- Do 07 together with 05_Memory_Debugging_RCA; that course owns the deep GDB material.
- Do 08 and 09 once tcpkv builds and passes tests locally.

## You are done when you can...

- [ ] Explain what WSL2 is (a real Linux kernel in a lightweight VM) and why code under `/mnt/c` is slow.
- [ ] Draw the four build stages and name the tool and file type at each step.
- [ ] Read an "undefined reference to `foo`" error and state the three possible causes.
- [ ] Explain the difference between `.a` and `.so`, what `-fPIC` is for, and how `ld.so` finds a library.
- [ ] Write a CMakeLists.txt for an executable plus a library plus GoogleTest, from memory, with correct
      PUBLIC/PRIVATE/INTERFACE visibility, and explain why that visibility matters.
- [ ] Configure and build with `cmake --preset` and Ninja, and produce `compile_commands.json`.
- [ ] Write a GoogleTest fixture and a parameterised test, run it under ASan via ctest.
- [ ] Explain Git's object model, the difference between merge and rebase, and recover a lost commit with reflog.
- [ ] Read `strace -f -tt` output for a server accepting a connection and name each syscall.
- [ ] Record a `perf` profile and read the report.
- [ ] Write a multi-stage Dockerfile for a C++ server that produces an image under 100 MB.
- [ ] Write a GitHub Actions workflow with a gcc/clang matrix, `-Werror`, ctest, an ASan job and a cache step.
- [ ] Write a bash script with `set -euo pipefail` and a trap, and explain each flag.
- [ ] Write a systemd unit for tcpkv with `Restart=on-failure` and read its logs with `journalctl`.
- [ ] Compare MSVC/MSBuild/WinDbg with GCC/CMake/GDB precisely, in two minutes, in an interview.

## Resume line this course backs up

- "Linux, GCC, CMake, GDB, Git, Docker, CI/CD (Azure DevOps, GitHub Actions)"
- "Windows (Win32, MSVC, Visual Studio, MSBuild, WinDbg)"
- tcpkv: "Builds clean under -Wall -Wextra -Werror with CMake; GoogleTest suite"

Each of these phrases is a promise. By the end of the course you will be able to demonstrate each
one live, on your own laptop, in under five minutes.

## Files in `code/`

```
code/
  Makefile                       minimal Makefile (module 04)
  m02_static_lib/                static library demo: mathlib.h, mathlib.cpp, main.cpp, build.sh
  m02_shared_lib/                shared library demo with -fPIC, rpath, ldd
  m02_undefined_ref/             reproduces "undefined reference" three ways
  m02_odr_violation/             two definitions of the same symbol, what the linker does
  tcpkv/                         complete CMake project skeleton
    CMakeLists.txt
    CMakePresets.json
    .clang-format
    include/tcpkv/protocol.hpp, store.hpp
    src/protocol.cpp, store.cpp, main.cpp
    tests/CMakeLists.txt, test_protocol.cpp, test_store.cpp
    Dockerfile                   multi-stage build (module 08)
    docker-compose.yml           tcpkv + PostgreSQL + Redis
    .dockerignore
    .github/workflows/ci.yml     GitHub Actions (module 09)
    azure-pipelines.yml          Azure DevOps equivalent
    scripts/build.sh, run_tests.sh, load_test.sh
    scripts/tcpkv_client.py      Python client that drives the server and checks results
    scripts/bisect_test.sh       git bisect run script (module 06)
    scripts/dump_stacks.gdb      batch stack dump of a running server (module 07)
    deploy/tcpkv.service         systemd unit (module 11)
  m11_syscalls.c                 open/read/write/fsync, mmap, fork/exec, pipe, signals
  m11_epoll_preview.c            epoll echo server skeleton
  m10_bash_patterns.sh           set -euo pipefail, traps, functions, loops
```
