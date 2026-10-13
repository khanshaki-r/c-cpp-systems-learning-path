# 01. Environment Setup — one evening, then never think about it again

**Time:** ~2 hours total. **Prerequisites:** none.

## Why this course exists

Your Windows toolchain is MinGW GCC 6.3 from 2016. It does not support most of C++17
(no `std::optional`, no `std::string_view`, no `std::filesystem`, no `std::shared_mutex`, broken
threads). Everything on your resume says **Linux, GCC, CMake, GDB**. You already have the answer
installed: **WSL Ubuntu 24.04** with GCC 13.3, Clang 18, CMake 3.28, Ninja, GDB 15, Valgrind 3.22,
Docker, Python 3.12 and the PostgreSQL 16 client. Checked on 2026-10-03; nothing to install.

From now on **all C and C++ in this curriculum is built and run inside WSL**, edited from VS Code
on Windows through the Remote-WSL extension. Windows-only topics (WinDbg, MSVC, Win32 threads) stay
on Windows and are covered in their own modules.

## Modules

| # | Module | Time |
|---|---|---|
| 01 | [VS Code + WSL development loop](01_vscode_wsl_dev_loop.md) | 1 h |
| 02 | [Verify the toolchain: build, test, debug, sanitize](02_verify_toolchain.md) | 1 h |

## You are done when you can

- [ ] Open a folder that lives in `/home/shakir/...` in VS Code with the green "WSL: Ubuntu-24.04" badge.
- [ ] Build `code/hello.cpp` with `g++ -std=c++17 -Wall -Wextra -Werror -pthread` and run it.
- [ ] Build the tiny CMake project in `code/cmake_hello/` with Ninja and run its test with `ctest`.
- [ ] Hit a breakpoint in VS Code inside WSL, and separately in plain `gdb` in the terminal.
- [ ] Run a program under Valgrind and under AddressSanitizer and read both reports.
- [ ] Say in one sentence why the code must live in the Linux filesystem, not under `/mnt/d`.

## Resume lines this course backs up

"Linux, GCC, CMake, GDB" in Platforms & Tools, and "Building modern C++17 systems projects on Linux".
