# 1. VS Code + WSL development loop

**Time:** ~1 hour   **Prerequisites:** none

## 1. In plain words

WSL2 is a real Linux kernel running inside a lightweight virtual machine on your Windows laptop.
Ubuntu 24.04 is installed in it, with your user `shakir`. You edit files with VS Code on Windows,
but the compiler, debugger and your program all run inside Linux. VS Code's Remote-WSL extension
makes this feel like one machine: the editor window is Windows, everything else is Linux.

Analogy: a Windows laptop with a Linux server sitting under the desk, connected by a very fast
cable. VS Code is the screen and keyboard, Linux does the work.

## 2. The real thing

### 2.1 Two filesystems, one rule

| Path seen from Linux | What it is | Speed for builds |
|---|---|---|
| `/home/shakir/...` | Linux's own ext4 disk (a virtual disk file on Windows) | Fast |
| `/mnt/c/...`, `/mnt/d/...` | Your Windows drives, mounted through a 9P network protocol | 5x to 20x slower, file watching unreliable |

**Rule: project code lives under `/home/shakir`.** From Windows you can still see it in Explorer at
`\\wsl$\Ubuntu-24.04\home\shakir\`. Your `D:\Cpp_DSA` folder stays where it is for notes and this
curriculum; when a module says "build this", copy or create the code under `~/dev/...` in Linux.

### 2.2 Memory

Your `C:\Users\msi1\.wslconfig` gives WSL 3 GB RAM, 4 processors, 2 GB swap. That is enough for all
C++ work and for a local PostgreSQL. It is **not** enough to run a YugabyteDB Docker node
comfortably (it wants about 2 GB on its own). When you reach 07_Databases_Internals module 11,
temporarily change `memory=3GB` to `memory=5GB`, run `wsl --shutdown` in PowerShell, and change it
back afterwards. Everything else in this curriculum works at 3 GB.

### 2.3 Install the VS Code side (Windows)

1. VS Code -> Extensions -> install **WSL** (publisher Microsoft, id `ms-vscode-remote.remote-wsl`).
2. Press `Ctrl+Shift+P` -> "WSL: Connect to WSL using Distro..." -> `Ubuntu-24.04`.
   A new window opens with a green badge bottom-left: `WSL: Ubuntu-24.04`.
3. In that window, install these extensions **into WSL** (the Install button will say
   "Install in WSL: Ubuntu-24.04"):
   - **C/C++** (`ms-vscode.cpptools`) — IntelliSense and the gdb debugger UI.
   - **CMake Tools** (`ms-vscode.cmake-tools`) — configure/build/test from the status bar.
   - **clangd** (`llvm-vs-code-extensions.vscode-clangd`) — optional, better diagnostics. If you
     install it, disable cpptools IntelliSense (`"C_Cpp.intelliSenseEngine": "disabled"`).

### 2.4 Prepare Linux

Open the VS Code terminal (it is a Linux bash now) and run:

```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build gdb valgrind \
    clang clang-tidy clang-format lldb git curl netcat-openbsd strace ltrace linux-tools-common \
    python3-pip postgresql-client libpq-dev libdb-dev libgtest-dev
mkdir -p ~/dev && cd ~/dev
git config --global user.name "MD Shakir Khan"
git config --global user.email "you@example.com"   # the email on your GitHub account
git config --global init.defaultBranch main
```

`libdb-dev` is Berkeley DB (for 07_Databases_Internals module 10). `libpq-dev` is the PostgreSQL
client library. `netcat-openbsd` gives you `nc` for talking to tcpkv by hand.

Add to the end of `~/.bashrc` (then `source ~/.bashrc`):

```bash
# C++17 quick build: cb hello.cpp  -> ./hello
cb() { g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O0 "$1" -o "${1%.*}" && echo "built ${1%.*}"; }
# Same but with AddressSanitizer + UBSan
cbs() { g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer "$1" -o "${1%.*}_asan" && echo "built ${1%.*}_asan"; }
# ThreadSanitizer
cbt() { g++ -std=c++17 -Wall -Wextra -Werror -pthread -g -O1 -fsanitize=thread "$1" -o "${1%.*}_tsan" && echo "built ${1%.*}_tsan"; }
export ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1
ulimit -c unlimited   # allow core dumps (05_Memory_Debugging_RCA)
```

### 2.5 Open a project folder

```bash
mkdir -p ~/dev/scratch && cd ~/dev/scratch && code .
```

`code .` from inside WSL opens that Linux folder in a Remote-WSL window. Always open folders this
way, or via File -> Open Folder in a WSL window (the file dialog then shows Linux paths).

### 2.6 Minimal `.vscode` for single-file experiments

Create `~/dev/scratch/.vscode/tasks.json`:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "g++ build active file (C++17, warnings as errors)",
      "type": "shell",
      "command": "g++",
      "args": ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-g", "-O0",
               "${file}", "-o", "${fileDirname}/${fileBasenameNoExtension}"],
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": ["$gcc"]
    }
  ]
}
```

and `~/dev/scratch/.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "gdb: active file",
      "type": "cppdbg",
      "request": "launch",
      "program": "${fileDirname}/${fileBasenameNoExtension}",
      "cwd": "${fileDirname}",
      "MIMode": "gdb",
      "preLaunchTask": "g++ build active file (C++17, warnings as errors)",
      "setupCommands": [
        { "text": "-enable-pretty-printing", "ignoreFailures": true }
      ]
    }
  ]
}
```

Now `Ctrl+Shift+B` builds the open file and `F5` builds and debugs it with breakpoints.
For real projects you will use CMake Tools instead (next module and 06_Linux_Toolchain_DevOps/04).

## 3. Code

Nothing to write yet; module 02 has the verification programs.

## 4. Under the hood (senior-level)

- WSL2 runs a Microsoft-built Linux kernel in a Hyper-V utility VM. Your processes are real Linux
  processes with real `pid`s, real `ptrace` (so gdb works fully), real `epoll`, real core dumps.
- `/mnt/c` is served by a 9P file server running in the Windows side; every `stat()` is a network
  round trip. That is why builds there are slow and why `inotify`-based file watching misses events.
- The VS Code Remote-WSL extension installs a small Node.js server inside Linux
  (`~/.vscode-server`). The Windows UI talks to it over a local socket. Extensions marked
  "workspace" run in Linux, "UI" extensions run in Windows. That is why you install C/C++ into WSL.
- Memory given to WSL shows up in Windows as `vmmem` / `Vmmem WSL`. `autoMemoryReclaim=gradual`
  in your `.wslconfig` returns idle pages to Windows.

## 5. In production

Most C++ backend jobs you will interview for build and run on Linux servers, usually Ubuntu or
RHEL, in Docker containers, in CI. Being fluent in a Linux shell, gdb and the GCC toolchain is
assumed at the senior level. Your Windows/MSVC experience is a plus, not a substitute. Interviewers
notice candidates who cannot type `ls -la`, `grep -rn`, or explain what `ulimit -c` does.

## 6. Try it

1. **Easy.** Open `~/dev/scratch` in a WSL window. Confirm the green badge. Done when a screenshot
   shows `WSL: Ubuntu-24.04` bottom-left.
2. **Easy.** Run `time (for i in $(seq 1 200); do stat /home/shakir > /dev/null; done)` and the same
   with `/mnt/c/Users/msi1`. Done when you can state the ratio.
3. **Medium.** Create a file from Windows Explorer at `\\wsl$\Ubuntu-24.04\home\shakir\dev\scratch\x.txt`
   and `cat` it from Linux. Then `touch` a file in Linux and find it from Explorer. Done when both work.

## 7. Interview questions

**Q: What is the difference between WSL1 and WSL2?**
WSL1 translated Linux syscalls to Windows calls; WSL2 runs a real Linux kernel in a lightweight VM,
so system call compatibility is complete (ptrace, epoll, Docker all work) at the cost of slower
cross-filesystem access.

**Q: Why are builds slow under /mnt/c?**
Files are served over the 9P protocol from Windows; every metadata call is an inter-VM round trip,
and compilers do thousands of them.

## 8. Recap

- All C and C++ work happens in WSL Ubuntu 24.04 with GCC 13. MinGW 6.3 is retired.
- Code lives in `/home/shakir/dev`, never under `/mnt/d`.
- VS Code connects through Remote-WSL; install C/C++ and CMake Tools *into WSL*.
- `cb`, `cbs`, `cbt` shell functions give you normal, ASan and TSan builds in one word.
- `ulimit -c unlimited` is set so core dumps are possible later.
- Raise WSL memory to 5 GB only when running YugabyteDB in Docker.
