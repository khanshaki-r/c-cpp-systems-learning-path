# 01. Linux for a Windows developer

**Time:** ~3 hours   **Prerequisites:** none

## 1. In plain words

Linux is an operating system kernel plus a set of small programs that each do one job. Windows gives
you one big integrated environment (Explorer, Visual Studio, the registry, services.msc). Linux gives
you a toolbox: a shell to type in, hundreds of tiny commands, and a convention that everything is a
file and text is the universal interface. Once you learn to chain those tiny commands together, you
will do in one line what used to take a PowerShell script.

WSL2 (Windows Subsystem for Linux, version 2) is a real Linux kernel running inside a lightweight
virtual machine managed by Windows. It is not an emulator and not a compatibility layer. When you
type `g++` in your Ubuntu terminal, a real Linux process runs on a real Linux kernel and the output
binary is a real Linux ELF executable. The only "magic" is that Windows and Linux can see each
other's files and that the VM starts in under a second.

Analogy: think of WSL2 as a second PC under your desk that shares your monitor and your `C:` drive
over a very fast network share. Anything you do on "that PC" is real Linux. Anything you do across
the share is slower.

## 2. The real thing

### 2.1 WSL2 mental model

```
+---------------------------- Windows 11 ---------------------------------+
|  Explorer, Visual Studio, Docker Desktop, VS Code                         |
|                                                                          |
|  +------------------ Lightweight utility VM (Hyper-V) -----------------+ |
|  |  Real Linux kernel (Microsoft-built, e.g. 5.15 / 6.6)               | |
|  |  Ubuntu 24.04 userland: bash, apt, gcc, cmake, systemd (optional)   | |
|  |  Linux filesystem: ext4 inside a .vhdx file on your C: drive        | |
|  |     /home/shakir/...   <- FAST: native ext4                         | |
|  |     /mnt/c/Users/...   <- SLOW: 9P network filesystem to Windows    | |
|  +---------------------------------------------------------------------+ |
|  \\wsl.localhost\Ubuntu-24.04\home\shakir  <- Windows view of Linux FS  |
+--------------------------------------------------------------------------+
```

Three rules that follow from this picture:

1. **Keep source code in the Linux filesystem** (`/home/<you>/...`). Compiling a project under
   `/mnt/c/` is routinely 5-20x slower (typical, from Microsoft's own WSL documentation and
   common experience) because every `stat()` and `open()` crosses the 9P file-protocol boundary.
   `git status` on a large repo under `/mnt/c` can take seconds instead of milliseconds.
2. **Edit from Windows if you like.** Open the folder in VS Code with the "WSL" extension, or browse
   `\\wsl.localhost\Ubuntu-24.04\home\...` in Explorer. The editor talks to the Linux side; the
   compiler runs natively.
3. **Networking:** WSL2 has its own IP (`ip addr show eth0`), but `localhost` is forwarded both
   ways by default. A server listening on port 7000 in WSL is reachable from Windows at
   `localhost:7000`. The reverse (Linux reaching a Windows service) uses the Windows host IP from
   `/etc/resolv.conf` (nameserver line) or `$(hostname).local`.

Useful WSL commands from PowerShell:

```powershell
wsl -l -v                 # list distros and WSL version
wsl --shutdown            # stop the VM (frees RAM, resets networking)
wsl -d Ubuntu-24.04       # start a shell
wsl --update              # update the kernel
```

Memory: WSL2 grows to 50% of host RAM by default. Cap it in `C:\Users\<you>\.wslconfig`:

```ini
[wsl2]
memory=8GB
processors=6
swap=2GB
```

### 2.2 The shell

Your default shell is **bash**. The prompt `shakir@LAPTOP:~$` shows user, host, current directory
(`~` is your home, `/home/shakir`). Core moves:

| Action | Command |
|---|---|
| Where am I | `pwd` |
| List | `ls -la` (`-l` long, `-a` include dotfiles, `-h` human sizes, `-t` by time) |
| Change dir | `cd dir`, `cd ..`, `cd -` (previous), `cd` (home) |
| Make dir | `mkdir -p a/b/c` (`-p` creates parents, no error if exists) |
| Copy / move / delete | `cp -r src dst`, `mv a b`, `rm -rf dir` (no recycle bin: it is gone) |
| Show file | `cat f`, `less f` (q to quit, / to search), `head -n 20 f`, `tail -f log` (follow) |
| Find text | `grep -rn "pattern" src/` |
| Find files | `find . -name "*.cpp"` |
| Help | `man ls`, `ls --help`, `tldr ls` (after `sudo apt install tldr`) |
| History | `history`, `Ctrl-R` (reverse search), `!!` (last command), `sudo !!` |
| Tab completion | `Tab` completes files, commands, and (with bash-completion) subcommands |

Quoting: single quotes `'...'` are literal; double quotes `"..."` expand `$VAR` and `$(cmd)`.
`*` and `?` are **globs** expanded by the shell before the program sees them. `rm *.o` is
expanded by bash into `rm a.o b.o c.o`; the program `rm` never sees the star.

### 2.3 Filesystem layout

There are no drive letters. One tree rooted at `/`.

```
/            root of everything
/home/shakir your home directory ($HOME, ~)
/etc         system configuration (text files): /etc/passwd, /etc/hosts, /etc/ssh/
/bin, /usr/bin      programs (ls, gcc, cmake). On Ubuntu 24.04 /bin is a symlink to /usr/bin
/usr/lib, /lib      shared libraries (.so). /usr/lib/x86_64-linux-gnu/libstdc++.so.6
/usr/include        system headers (stdio.h, c++/13/vector)
/usr/local          things you install by hand (make install) - keeps apt and you separate
/opt                third-party bundles (/opt/microsoft/...)
/var                variable data: /var/log (logs), /var/lib (databases), /var/run
/tmp                scratch, cleared on reboot
/proc               virtual: kernel and process info as files (/proc/1234/status)
/sys                virtual: devices and kernel parameters
/dev                devices: /dev/null, /dev/zero, /dev/urandom, /dev/sda
/mnt/c              your Windows C: drive (WSL only)
```

Paths use `/`. Case-sensitive: `Main.cpp` and `main.cpp` are different files. Hidden files start
with `.` (`.bashrc`, `.git/`). Extensions are convention only; the kernel decides executability by
a permission bit, not by `.exe`.

Important Windows difference: a file that is open can be deleted or replaced on Linux. The inode
stays alive until the last descriptor closes. This is why you can upgrade a running server's binary
and why `rm` of a log file does not free disk until the process closes it (see `lsof` in module 07).

### 2.4 Users, groups, permissions

Every file has an owner, a group, and nine permission bits: read/write/execute for
owner/group/others.

```
$ ls -l tcpkv
-rwxr-xr-x 1 shakir shakir 48216 Oct  3 10:12 tcpkv
 ^^^ ^^^ ^^^
 |   |   +-- others: r-x (read, execute)
 |   +------ group:  r-x
 +---------- owner:  rwx
first char: - regular file, d directory, l symlink, c/b device, p pipe, s socket
```

`chmod` changes bits: `chmod +x build.sh` (add execute for all), `chmod 644 file` (rw-r--r--),
`chmod 755 dir` (rwxr-xr-x). Octal: r=4, w=2, x=1. `chown shakir:shakir file` changes owner.
For a directory, `x` means "may enter", `r` means "may list", `w` means "may create/delete entries".

`root` is the administrator (uid 0). You are not root. `sudo cmd` runs one command as root; your
password is cached for 15 minutes. Never run your normal shell as root; one typo in `rm -rf` and
the VM is gone. Services should run as a dedicated non-root user (module 11, systemd `User=`).

Windows ACLs are richer; Linux also has ACLs (`setfacl`) but almost nobody uses them for dev work.
There is no "Run as administrator" dialog: `sudo` is the equivalent.

### 2.5 Processes

A process has a PID, a parent PID, a user, a state, and open file descriptors. PID 1 is `init`
(systemd on Ubuntu; in WSL it is a small init unless you enable systemd in `/etc/wsl.conf`).

```bash
ps aux                 # all processes, BSD style
ps -ef                 # all processes, with parent PID
ps -o pid,ppid,stat,cmd -p 1234
pgrep -a tcpkv         # find PIDs by name
top                    # live view; press 1 for per-core, M sort by memory, P by CPU, q quit
htop                   # nicer top; F5 tree view, F9 kill
kill 1234              # sends SIGTERM (15): "please exit"
kill -9 1234           # SIGKILL: kernel removes it, no cleanup, no core dump
kill -INT 1234         # SIGINT, same as Ctrl-C
killall tcpkv          # by name
nohup ./server &       # run in background, survive terminal close
jobs; fg; bg; Ctrl-Z   # job control inside one shell
```

Signals are the Linux equivalent of Windows console control events plus structured exceptions in
one mechanism. The ones you will meet:

| Signal | Number | Default action | When |
|---|---|---|---|
| SIGINT | 2 | terminate | Ctrl-C |
| SIGTERM | 15 | terminate | `kill`, systemd stop; your server should handle this gracefully |
| SIGKILL | 9 | terminate, cannot be caught | `kill -9`, OOM killer |
| SIGSEGV | 11 | core dump | invalid memory access (your access violation) |
| SIGABRT | 6 | core dump | `abort()`, failed `assert`, uncaught C++ exception |
| SIGPIPE | 13 | terminate | write to a socket/pipe whose reader closed. Servers ignore it. |
| SIGHUP | 1 | terminate | terminal closed; daemons reuse it for "reload config" |
| SIGCHLD | 17 | ignore | a child exited |

Exit status: `echo $?` after a command. 0 is success. 1-125 program-defined. 128+N means killed by
signal N (139 = 128+11 = SIGSEGV). Shell scripts and CI depend on this.

### 2.6 File descriptors, redirection, pipes

Every process starts with three open file descriptors (small integers indexing the kernel's
open-file table): 0 stdin, 1 stdout, 2 stderr. Redirection rewires them before the program runs:

```bash
cmd > out.txt          # stdout to file (truncate)
cmd >> out.txt         # append
cmd 2> err.txt         # stderr to file
cmd > all.txt 2>&1     # both to one file (order matters: redirect 1 first, then point 2 at 1)
cmd &> all.txt         # bash shorthand for the above
cmd < input.txt        # stdin from file
cmd > /dev/null 2>&1   # discard everything
cmd1 | cmd2            # stdout of cmd1 becomes stdin of cmd2 (a kernel pipe buffer, 64 KiB)
cmd1 |& cmd2           # pipe both stdout and stderr
cmd | tee log.txt      # write to file and also pass through
```

Why `2>&1` order matters: redirections are applied left to right, and `2>&1` means "make fd 2 a
copy of what fd 1 is **right now**". `cmd 2>&1 > f` sends stderr to the terminal and stdout to `f`.

Here-doc and here-string:

```bash
cat > config.ini <<'EOF'
port=7000
EOF
grep port <<< "port=7000"
```

### 2.7 The text-processing toolbox

These five commands cover 90% of daily log and code work.

**grep** — find lines matching a pattern.

```bash
grep -rn "TODO" src/                 # recursive, show line numbers
grep -rni "error" /var/log/syslog    # case-insensitive
grep -E "fail(ed|ure)" log           # extended regex
grep -v "^#" config | grep -v "^$"   # -v inverts: strip comments and blank lines
grep -c "GET" access.log             # count matching lines
grep -l "include <thread>" -r .      # list files only
grep -A3 -B1 "segfault" log          # 3 lines after, 1 before
grep -rn --include="*.cpp" "mutex" . # restrict by glob
```

**find** — walk a tree and select files by attributes.

```bash
find . -name "*.o" -delete
find src -type f -name "*.cpp" | wc -l
find . -type f -size +10M
find . -mmin -30                     # modified in last 30 minutes
find . -name "*.cpp" -exec clang-format -i {} \;
find . -name "*.cpp" -print0 | xargs -0 grep -l "pthread"   # safe with spaces in names
```

**xargs** — turn stdin lines into command arguments.

```bash
pgrep tcpkv | xargs kill
find . -name "*.log" | xargs rm -f
cat hosts.txt | xargs -I{} ssh {} uptime      # -I{} substitutes each line
find . -name "*.cpp" | xargs -P 8 -n 1 g++ -c  # -P parallel, -n args per invocation
```

**sed** — stream editor; substitutions.

```bash
sed 's/foo/bar/' file                # first occurrence per line, print to stdout
sed 's/foo/bar/g' file               # all occurrences
sed -i 's/\bint32\b/std::int32_t/g' *.cpp   # -i edit in place
sed -n '10,20p' file                 # print lines 10-20
sed '/^#/d' file                     # delete comment lines
sed -E 's/([0-9]+)ms/\1 ms/g' log    # -E extended regex with capture groups
```

**awk** — a tiny language for column-oriented text. Fields are `$1 $2 ...`, `$0` is the line,
`NF` field count, `NR` line number.

```bash
awk '{print $1}' access.log                       # first column
awk -F: '{print $1, $7}' /etc/passwd              # -F sets separator
awk '$9 >= 500 {count++} END {print count}' access.log   # count 5xx responses
awk '{sum += $3} END {print sum/NR}' latencies.txt       # average of column 3
ps aux | awk '$3 > 50.0 {print $2, $11}'                # PIDs using >50% CPU
awk '/ERROR/ && !/timeout/' app.log                      # pattern logic
```

Also: `sort` (`-n` numeric, `-k2` by column 2, `-u` unique, `-r` reverse), `uniq -c` (count
duplicates; input must be sorted), `cut -d, -f2` (column by delimiter), `wc -l`, `tr a-z A-Z`,
`diff -u a b`, `head`, `tail`, `tee`, `column -t`.

A real pipeline: top 10 IPs by request count from an access log.

```bash
awk '{print $1}' access.log | sort | uniq -c | sort -rn | head -10
```

### 2.8 Environment variables and PATH

Environment variables are inherited by child processes. `PATH` is a colon-separated list of
directories the shell searches for commands, left to right.

```bash
echo $PATH                       # /usr/local/bin:/usr/bin:/bin:...
which g++                        # /usr/bin/g++ - first match in PATH
type cd                          # cd is a shell builtin
export CC=gcc CXX=g++            # export: visible to children
MY_VAR=1 ./prog                  # set only for that one command
env | sort                       # show all
unset MY_VAR
```

Persistent settings go in `~/.bashrc` (run for every interactive shell) or `~/.profile` (login
shells). After editing: `source ~/.bashrc`. Example additions:

```bash
export PATH="$HOME/.local/bin:$PATH"
export CMAKE_GENERATOR=Ninja
export EDITOR=vim
alias ll='ls -lah'
alias gs='git status'
```

Difference from Windows: there is no registry; there is no system-wide "Environment Variables"
dialog that updates running programs. A variable set in one shell does not appear in another.

### 2.9 Package management with apt

`apt` is the Ubuntu package manager (think winget/chocolatey with a central, signed repository).

```bash
sudo apt update                       # refresh package index (does not install anything)
sudo apt install -y gdb               # install
sudo apt remove gdb                   # remove (keep config); purge removes config too
apt search ninja                      # search
apt show cmake                        # details, version
apt list --installed | grep clang
dpkg -L cmake | head                  # which files a package installed
dpkg -S /usr/bin/g++                  # which package owns a file
sudo apt autoremove                   # remove orphaned dependencies
```

Dev packages end in `-dev` and contain headers and link-time `.so` symlinks:
`libpq-dev` (PostgreSQL client), `libssl-dev`, `libhiredis-dev`. The runtime package (`libpq5`)
has only the shared object. A Docker runtime image needs only the runtime package (module 08).

### 2.10 SSH keys

SSH (Secure Shell) gives you a remote shell and is how Git talks to GitHub without passwords.

```bash
ssh-keygen -t ed25519 -C "rajaman.ar3@gmail.com"      # accept defaults; set a passphrase
cat ~/.ssh/id_ed25519.pub                             # paste this into GitHub -> Settings -> SSH keys
ssh -T git@github.com                                 # "Hi <user>! You've successfully authenticated"
eval "$(ssh-agent -s)" && ssh-add ~/.ssh/id_ed25519   # cache passphrase for this session
ssh user@host                                         # remote shell
scp file user@host:/path/                             # copy over ssh
ssh -L 5432:localhost:5432 user@host                  # port forward local 5432 -> remote 5432
```

`~/.ssh/config` saves typing:

```
Host prod
    HostName 10.0.0.5
    User deploy
    IdentityFile ~/.ssh/id_ed25519
```

Permissions matter: `chmod 700 ~/.ssh; chmod 600 ~/.ssh/id_ed25519`. SSH refuses keys that are
world-readable.

### 2.11 Windows concept -> Linux concept

| Windows | Linux | Notes |
|---|---|---|
| `C:\Users\you` | `/home/you` (`~`) | |
| `C:\Program Files` | `/usr/bin`, `/usr/lib`, `/opt` | programs and libs split by type, not by vendor |
| `C:\Windows\System32` | `/usr/lib`, `/lib` | |
| `%TEMP%` | `/tmp` | |
| Registry | text files in `/etc`, `~/.config` | |
| Services (services.msc) | systemd units (`systemctl`) | module 11 |
| Event Viewer | `journalctl`, `/var/log` | |
| Task Manager | `top`, `htop`, `ps` | |
| `taskkill /PID n /F` | `kill -9 n` | |
| `dir`, `type`, `copy`, `del` | `ls`, `cat`, `cp`, `rm` | |
| `where cmd` | `which cmd` | |
| `findstr` | `grep` | |
| `set VAR=x` | `export VAR=x` | |
| `%PATH%` with `;` | `$PATH` with `:` | |
| `.exe` | no extension; execute bit | |
| `.dll` / `.lib` | `.so` / `.a` | module 02 |
| `.pdb` | DWARF sections inside the ELF (`-g`) | module 03 |
| `cl.exe`, `link.exe` | `g++`, `ld` (via `g++`) | |
| MSBuild, `.sln`, `.vcxproj` | CMake, `CMakeLists.txt`, Ninja or Make | module 04 |
| WinDbg, Visual Studio debugger | `gdb`, `lldb` | module 07, 05_Memory_Debugging_RCA |
| Process Monitor | `strace`, `ltrace` | module 07 |
| PerfView, VTune | `perf`, `valgrind --tool=callgrind` | module 07 |
| Dependency Walker, `dumpbin /DEPENDENTS` | `ldd`, `readelf -d` | module 02 |
| `dumpbin /SYMBOLS` | `nm`, `objdump -t` | module 02 |
| Access violation (0xC0000005) | SIGSEGV | |
| Minidump `.dmp` | core file (`core`, `coredumpctl`) | 05_Memory_Debugging_RCA |
| `CreateProcess` | `fork` + `exec` | module 11 |
| `WaitForSingleObject(hProcess)` | `waitpid` | |
| HANDLE | file descriptor (int) | |
| `CloseHandle` | `close` | |
| IOCP | `epoll` | 08_Networking |
| `CreateThread` | `pthread_create` / `std::thread` | 04_Concurrency |
| Critical section | `pthread_mutex_t` / `std::mutex` | |
| `GetLastError()` | `errno` | module 11 |
| Administrator | `root` via `sudo` | |
| `\r\n` line endings | `\n` | `git config core.autocrlf input` on WSL |
| Backslash paths, case-insensitive | forward slash, case-sensitive | |
| Winget / Chocolatey | `apt` | |
| PowerShell | bash (or zsh, fish) | module 10 |
| Scheduled Tasks | `cron`, systemd timers | |
| vcpkg / NuGet | vcpkg, Conan, or `apt install lib*-dev` | module 12 |

## 3. Code

No C++ in this module; the "code" is shell. Create a scratch project and exercise the tools.

```bash
mkdir -p ~/scratch/m01 && cd ~/scratch/m01
cat > hello.cpp <<'EOF'
#include <cstdio>
int main(int argc, char** argv) {
    std::printf("args=%d\n", argc);     // prints to fd 1
    std::fprintf(stderr, "to stderr\n"); // prints to fd 2
    return argc > 1 ? 3 : 0;            // exit status visible with $?
}
EOF
g++ -std=c++17 -Wall -Wextra -Werror -o hello hello.cpp
./hello; echo "status=$?"
./hello x y; echo "status=$?"
./hello > out.txt 2> err.txt; cat out.txt; cat err.txt
./hello 2>&1 | grep stderr            # only stderr line, because the pipe carries both
ls -l hello                            # note the x bits
chmod -x hello; ./hello                # "Permission denied": the bit, not the extension, decides
chmod +x hello
```

Watch a process and its file descriptors:

```bash
sleep 300 &                            # background job, prints its PID
PID=$!
ps -o pid,ppid,stat,cmd -p $PID
ls -l /proc/$PID/fd                    # 0,1,2 point at your terminal (/dev/pts/N)
cat /proc/$PID/status | head -12
kill $PID; wait $PID; echo "status=$?"  # 143 = 128 + 15 (SIGTERM)
```

Log analysis pipeline on synthetic data:

```bash
for i in $(seq 1 1000); do
  echo "$((RANDOM % 5 + 1)).example.com GET /api/$((RANDOM % 7)) $((RANDOM % 3 == 0 ? 500 : 200)) $((RANDOM % 900 + 10))ms"
done > access.log
awk '{print $1}' access.log | sort | uniq -c | sort -rn
awk '$4 == 500' access.log | wc -l
sed -E 's/([0-9]+)ms$/\1/' access.log | awk '{s += $5} END {printf "avg %.1f ms\n", s/NR}'
```

See `code/m10_bash_patterns.sh` for the scripting side (module 10).

## 4. Under the hood (senior-level)

**Everything is a file descriptor.** The shell implements `cmd > out.txt` with three syscalls:
`fork()` to create a child, in the child `open("out.txt", O_WRONLY|O_CREAT|O_TRUNC)` then
`dup2(fd, 1)` to make descriptor 1 refer to that file, then `execve("cmd", ...)`. The program
never knows; it just writes to fd 1. A pipe is the same trick with `pipe(fds)` creating a kernel
buffer, `dup2(fds[1], 1)` in the writer and `dup2(fds[0], 0)` in the reader. Module 11 shows the C.

**Permission bits are checked at `open()`, not at `read()`.** Once you hold a descriptor, changing
the file's mode does not revoke it. This is why a server can open a privileged port or file as root
and then drop privileges with `setuid()`.

**Signals are delivered on return to user space.** The kernel sets a bit in the target's pending
mask; the next time that thread transitions kernel -> user (syscall return, interrupt return), the
kernel runs the handler on the thread's stack. That is why a thread blocked in `read()` wakes up
with `EINTR`, and why signal handlers must be async-signal-safe (module 11).

**`/proc` is a filesystem interface to the kernel.** `cat /proc/1234/maps` is how `gdb`, `perf`
and `top` learn memory layout; `/proc/1234/fd` is how `lsof` works. Reading `/proc/meminfo`
costs one syscall and a few microseconds; no API to learn.

**WSL2 file performance.** The Linux ext4 filesystem lives in a `.vhdx` file; the Linux kernel
drives it with its own page cache and inode cache, so `stat()` is a few hundred nanoseconds. For
`/mnt/c`, each metadata operation becomes a 9P request to a Windows-side server (Plan 9 protocol
over a VM socket), context switch included; measured latencies are in the tens to hundreds of
microseconds. A `cmake` configure that stats thousands of files feels it immediately.

**apt and shared libraries.** `apt install libpq-dev` places `/usr/include/postgresql/libpq-fe.h`
and the symlink `/usr/lib/x86_64-linux-gnu/libpq.so -> libpq.so.5`. The link editor needs the
unversioned name; the dynamic loader at run time needs the versioned soname `libpq.so.5`. This
split is why a runtime image needs only `libpq5` (module 02, module 08).

## 5. In production

- **Logs are text, and the toolbox is the log viewer.** On an incident you will do
  `journalctl -u tcpkv --since "10 min ago" | grep -c ERROR` and
  `tail -f /var/log/app.log | grep --line-buffered "txn=4471"` long before you open any GUI.
  Learn `grep -A/-B`, `awk` sums, and `sort | uniq -c` to reflex level.
- **Exit codes are the contract.** CI treats a non-zero exit as failure. A test runner that
  prints "FAILED" but returns 0 is a silent green build. `set -e` in scripts (module 10) exists
  because of this.
- **`kill -9` loses data.** A banking server with an in-flight transaction needs SIGTERM, a
  handler that stops accepting, drains, flushes, and exits 0. systemd gives 90 seconds by default
  (`TimeoutStopSec`), then sends SIGKILL. Design for it (module 11).
- **Disk full with no big files.** A deleted-but-open log file still consumes space.
  `lsof +L1` lists them; restarting the writer frees the space. Classic 3 AM page.
- **Line endings.** A bash script written in Notepad has `\r\n`; bash reports
  `/bin/bash^M: bad interpreter`. Fix with `sed -i 's/\r$//' script.sh` or `dos2unix`, and set
  `git config --global core.autocrlf input` so Git normalises on commit.
- **Mapping to your current work:** your RPC server on Windows runs as a service, logs to files
  and you read dumps in WinDbg. On Linux the same server is a systemd unit, logs to journald, and
  dumps to a core file read by gdb. The engineering is identical; only the tool names change.
  Interviewers like hearing that comparison.

## 6. Try it

1. **(easy)** Create `~/projects` in the Linux filesystem. Time `ls -R /usr/include | wc -l` and then
   time the same on `/mnt/c/Windows/System32` with `time`. Done when you can state the ratio and
   explain why the second is slower.
2. **(easy)** Write a one-line pipeline that prints the five largest files under `/usr/lib` with
   human-readable sizes. Done when it runs and you can explain each stage. Hint: `find -type f`,
   `du -h`, `sort -rh`, `head`.
3. **(medium)** Start `sleep 1000 &`, then from a second terminal find its PID with `pgrep`, inspect
   `/proc/<pid>/status`, `/proc/<pid>/fd`, and `/proc/<pid>/maps`. Send SIGTERM and verify the exit
   status is 143 in the first terminal. Done when you can explain the 143.
4. **(medium)** Given `/etc/passwd`, print only usernames whose login shell is `/bin/bash`, sorted,
   using `awk` and `sort`. Then do it again with `grep` and `cut`. Done when both give identical output.
5. **(hard)** Write a pipeline that, given a file of lines `<timestamp> <latency_ms>`, prints the
   p50, p95 and p99 latency. Done when it works on 10,000 random lines and you can explain how you
   computed the percentile index. Hint: `sort -n`, `awk` with `NR` and `int()`, or collect into an
   array and index in `END`.
6. **(hard)** Set up an SSH key, add it to GitHub, clone one of your repos over SSH into `~/projects`,
   and set `core.autocrlf input`. Done when `ssh -T git@github.com` greets you and `git status` is
   clean after a `git clone`.

## 7. Interview questions

**Q: What is WSL2 and how does it differ from WSL1?**
WSL2 runs a real Linux kernel in a lightweight Hyper-V VM; syscalls hit a genuine kernel, so
Docker, `perf` and `strace` work. WSL1 translated Linux syscalls to NT calls in a compatibility
layer; faster for `/mnt/c` file access, but incomplete. For C++ development WSL2 is the right choice,
with sources kept in the Linux filesystem.

**Q: Explain `2>&1` and why `cmd 2>&1 > f` behaves differently from `cmd > f 2>&1`.**
Redirections apply left to right. `2>&1` copies the current target of fd 1 into fd 2. In
`cmd > f 2>&1`, fd 1 is already the file, so stderr follows it. In `cmd 2>&1 > f`, fd 2 copies the
terminal first, then fd 1 moves to the file; stderr stays on the terminal.

**Q: What does exit status 139 mean?**
128 + 11: the process was killed by signal 11, SIGSEGV. 134 is SIGABRT (128+6), 137 is SIGKILL
(128+9), often the OOM killer in containers.

**Q: Difference between SIGTERM and SIGKILL?**
SIGTERM can be caught: the process gets a chance to flush, close sockets and exit cleanly.
SIGKILL cannot be caught or ignored; the kernel tears the process down immediately. systemd and
Docker send SIGTERM, wait a grace period, then SIGKILL.

**Q: Why can you delete a file that a process has open on Linux but not on Windows?**
Linux separates the directory entry (name) from the inode (data). `unlink` removes the name; the
inode lives until the link count and open-descriptor count both reach zero. Windows by default takes
a sharing lock on open files. Consequence: on Linux a deleted log can still eat disk space.

**Q: How does the shell find the program when you type `g++`?**
It walks `$PATH` left to right and executes the first directory containing an executable named
`g++`. `which g++` shows the winner. Builtins (`cd`, `export`) are handled by the shell and
never searched.

**Q: What is the difference between `libpq-dev` and `libpq5`?**
`libpq5` is the runtime: the versioned shared object `libpq.so.5`. `libpq-dev` adds headers and
the unversioned `libpq.so` symlink the linker needs. Build images need `-dev`; runtime images need
only the runtime package.

## 8. Recap

- WSL2 is a real Linux kernel in a lightweight VM. Keep code under `/home`, not `/mnt/c`.
- One tree rooted at `/`; `/etc` config, `/usr/bin` programs, `/usr/lib` libraries, `/var/log` logs,
  `/proc` kernel view, `/tmp` scratch.
- Permissions are nine bits (rwx for owner/group/others) plus owner and group; execute bit decides
  what runs, not the extension.
- `sudo` for one privileged command; never a root shell.
- Processes: `ps`, `top`, `htop`, `kill` sends SIGTERM by default; `kill -9` is SIGKILL.
- Exit status 0 is success; 128+N means killed by signal N.
- fd 0/1/2 are stdin/stdout/stderr; `>`, `2>`, `2>&1`, `|` rewire them before the program starts.
- `grep`, `find`, `xargs`, `sed`, `awk`, `sort | uniq -c` are the daily log and code toolbox.
- `$PATH` is searched left to right; persistent environment goes in `~/.bashrc`.
- `apt install` for tools; `-dev` packages for headers; `ssh-keygen -t ed25519` for GitHub.
