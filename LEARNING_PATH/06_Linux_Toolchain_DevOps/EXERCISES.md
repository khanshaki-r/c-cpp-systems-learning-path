# 06_Linux_Toolchain_DevOps: all exercises

Every exercise from the twelve modules in one place. The full wording, including the exact
"done when" condition, is in section 6 of each module; this file adds a hint for each one.
Do the easy ones as you read the module, the medium ones the same week, and pick at least two hard
ones per module. The hard ones are what you show in interviews.

Difficulty: **E** easy (under 30 minutes), **M** medium (1 to 2 hours), **H** hard (half a day or more).

Suggested minimum path if time is short: 01.3, 02.4, 02.6, 03.4, 04.3, 04.6, 05.4, 05.6, 06.4,
06.5, 07.1, 07.5, 08.2, 08.5, 09.1, 09.4, 10.4, 11.5, 11.6, 12.6.

---

## 01. Linux for a Windows developer

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 1.1 | E | Time `ls -R` on `/usr/include` vs `/mnt/c/Windows/System32`; explain the ratio. | Prefix with `time`; run each twice so caches are warm. `/mnt/c` goes through the 9P file-sharing protocol to Windows, every `stat` is a round trip. |
| 1.2 | E | One pipeline printing the five largest files under `/usr/lib`, human-readable. | `find /usr/lib -type f -exec du -h {} + \| sort -rh \| head -5`. Explain why `-exec ... +` beats `\; `. |
| 1.3 | M | `sleep 1000 &`; inspect `/proc/PID/status`, `fd`, `maps`; SIGTERM it; explain exit 143. | `pgrep -n sleep`; `ls -l /proc/PID/fd`; after `kill PID`, `wait %1; echo $?`. 143 = 128 + 15. |
| 1.4 | M | Usernames with `/bin/bash` shell from `/etc/passwd`, with awk+sort and with grep+cut. | `awk -F: '$7=="/bin/bash"{print $1}'`; `grep ':/bin/bash$' \| cut -d: -f1`. Diff the two outputs with `diff <(...) <(...)`. |
| 1.5 | H | p50/p95/p99 from a file of `<timestamp> <latency_ms>` lines. | Generate data with `awk 'BEGIN{srand(); for(i=0;i<10000;i++) print i, int(rand()*100)}'`. Sort numerically on field 2, load into an awk array, index at `int(p*NR)` (clamp to at least 1). |
| 1.6 | H | SSH key, GitHub, clone over SSH into `~/projects`, `core.autocrlf input`. | `ssh-keygen -t ed25519`, paste `~/.ssh/id_ed25519.pub` into GitHub settings; `ssh -T git@github.com`. Clone into the Linux filesystem, not `/mnt/c`. |

## 02. How a C++ program is built

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 2.1 | E | `g++ -E` line counts for `<iostream>` vs `<cstdio>`. | `g++ -std=c++17 -E hello.cpp \| wc -l`. The difference is every header `<iostream>` pulls in transitively: templates, locales, strings. |
| 2.2 | E | Build `m02_static_lib`, find the `U` that becomes `T` with `nm -C`. | Run `build.sh`; compare `nm -C main.o \| grep add` and `nm -C prog_static \| grep add`. |
| 2.3 | M | Make `prog_norpath` run three ways; rank them for production. | `LD_LIBRARY_PATH=.`; `sudo cp libmathlib.so /usr/local/lib && sudo ldconfig`; relink with `-Wl,-rpath,'$ORIGIN'` (single quotes so the shell does not eat `$ORIGIN`). |
| 2.4 | M | Reproduce the three undefined-reference cases in `m02_undefined_ref`; diagnose each from `nm -C`. | Missing object: symbol absent everywhere. C vs C++ linkage: `nm` shows `add` in one file and `add(int, int)` in the other (use `nm` without `-C` to see `_Z3addii`). Signature mismatch: two different mangled names. |
| 2.5 | H | `libplugin.so` with one exported `extern "C"` function, loaded via `dlopen`/`dlsym`; `-fvisibility=hidden`. | Mark the function `__attribute__((visibility("default")))`; link the host with `-ldl` (not needed on glibc 2.34+, harmless). `nm -D --defined-only libplugin.so`. |
| 2.6 | H | `silent_odr` links and misbehaves; `-flto -Wodr` reports it; fix it. | The linker only sees symbol names, not class layouts, so two different `struct Config` definitions with the same name both "link". The fix is one shared header. |

## 03. GCC and Clang flags that matter

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 3.1 | E | `-S` at `-O0` vs `-O2`, diff the assembly for `sum_to`. | Add `-fno-asynchronous-unwind-tables -masm=intel` for readable output; look for a loop that disappeared (closed-form or vectorised). |
| 3.2 | E | Hello world with and without `-g`: `ls -l`, `size`, `readelf -S`. | `size` counts only loadable sections (text, data, bss); `.debug_*` sections are in the file but never loaded. |
| 3.3 | M | Compile your own C with `-Wshadow -Wconversion` etc., count warnings per category, fix shadows. | `2>&1 \| grep -o '\[-W[a-z-]*\]' \| sort \| uniq -c \| sort -rn`. Rename the inner variable; do not silence with casts. |
| 3.4 | M | `m03_asan_demo.cpp` with and without ASan; find the allocation site. | Without ASan the overrun lands in malloc padding or another chunk, so nothing visible happens. ASan's report has three stacks: the bad access, the allocation, and (for use-after-free) the free. |
| 3.5 | H | Signed overflow in a loop condition: terminates at `-O0`, infinite at `-O2`; then UBSan. | `for (int i = 1; i > 0; i *= 2)` or `i += big`. The optimiser assumes signed overflow never happens, so `i > 0` is "always true". `timeout 2 ./prog; echo $?` gives 124. |
| 3.6 | H | `-static-libstdc++ -static-libgcc`, then fully `-static`; run in a `FROM scratch` image. | Dockerfile: `FROM scratch`, `COPY hello /hello`, `ENTRYPOINT ["/hello"]`. `ldd` on the fully static binary says "not a dynamic executable". |

## 04. Make and CMake from scratch

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 4.1 | E | `code/Makefile`: header edit rebuilds both objects; show the `.d` file. | The `-MMD -MP` flags write `build/foo.d`; `-include $(DEPS)` pulls them in. `cat` one. |
| 4.2 | E | `ninja -C build/debug -t commands tcpkv`; map `-I`, `-pthread`, `-std=c++17` to CMake lines. | `target_include_directories(... PUBLIC ...)`, `Threads::Threads`, `CMAKE_CXX_STANDARD` + `CXX_EXTENSIONS OFF`. |
| 4.3 | M | Flip `Threads::Threads` and the include dir from PUBLIC to PRIVATE; explain what breaks. | glibc 2.34 moved pthread into libc, so the link may still succeed; the include change breaks `main.cpp` at compile time. Rule: PUBLIC if it appears in your headers, PRIVATE if only in your .cpp. |
| 4.4 | M | A `clang` preset; fix what Clang warns about. | The preset exists in the shipped `CMakePresets.json`; build it with `cmake --preset clang && cmake --build --preset clang`. Clang often adds `-Wshorten-64-to-32`-style findings. |
| 4.5 | H | Second library `tcpkv_net` depending on `tcpkv_core`; `tcpkv` depends on `tcpkv_net`. | `target_link_libraries(tcpkv_net PUBLIC tcpkv::core)`; `tcpkv` links only `tcpkv_net`; check `ninja -t commands tcpkv \| grep -o '\-I[^ ]*' \| sort \| uniq -c`. |
| 4.6 | H | `install(EXPORT)`, install to `/tmp/tcpkv-install`, consume with `find_package(tcpkv CONFIG)`. | You need a `tcpkvConfig.cmake` that `include()`s the exported targets file; `configure_package_config_file` + `write_basic_package_version_file` from `CMakePackageConfigHelpers`. |

## 05. Testing with GoogleTest and ctest

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 5.1 | E | SET with `:` and `-` in the key passes; a tab in the key fails. | Add to the parameterised `MalformedLines` table for the tab case; check `ctest -N \| grep -i tab`. |
| 5.2 | E | `--gtest_shuffle --gtest_repeat=50`; a shared static makes order matter. | Put `static int counter` in the test file, increment in test A, assert in test B. Shuffle exposes it within a few repeats; `--gtest_random_seed=N` reproduces. |
| 5.3 | M | `Store::require_key` aborts on empty key; `EXPECT_DEATH`. | Death tests fork; name the suite `*DeathTest` so they run first. Matcher is a regex on stderr. |
| 5.4 | M | Remove the lock in `Store::get`; read the TSan report. | `cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan -R Concurrency`. The report shows "Write of size ... by thread T2" and "Previous read ... by thread T1" with both stacks. |
| 5.5 | H | Extract a `LineReader` with tests for split lines, CRLF and overflow. | Keep the buffer and a start offset; `feed` appends, scans for `\n`, returns complete lines, erases the consumed prefix. Test overflow at exactly `kMaxLineLength` and one byte more. |
| 5.6 | H | ctest integration test with `FIXTURES_SETUP`/`FIXTURES_CLEANUP` and the Python client. | Setup test: a script that starts `tcpkv` on 7123 in the background and writes its PID to a file; cleanup test: `kill $(cat pidfile)`; main test: `tcpkv_client.py --selftest --port 7123 --wait 5`. Label all three `integration`. |

## 06. Git properly

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 6.1 | E | `git cat-file -p` from commit to tree to blob. | `git cat-file -p HEAD`, copy the `tree` hash, `git cat-file -p <tree>`, copy a blob hash. `git cat-file -t` prints the type. |
| 6.2 | E | Stage half a file's hunks with `git add -p`; two commits on one file. | In `add -p`, `s` splits a hunk, `e` edits it by hand. |
| 6.3 | M | `--fixup` plus `rebase -i --autosquash` non-interactively. | `git commit --fixup=<sha-of-first>`; `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash <base>`. |
| 6.4 | M | Conflict with `zdiff3`, resolve, then undo the whole rebase via reflog. | `git config merge.conflictstyle zdiff3`. The middle region is the common ancestor. Undo: `git reflog`, find the entry before `rebase (start)`, `git reset --hard HEAD@{n}`. |
| 6.5 | H | `scripts/bisect_test.sh` + `git bisect run` on a planted bug. | The finished script is in `code/tcpkv/scripts/bisect_test.sh`; copy it outside the repo first so old commits can run it. Plant the bug with `git commit` in a loop of trivial changes before and after. |
| 6.6 | H | tcpkv GitHub repo with LICENSE, `.gitattributes`, branch protection, `v0.1.0` tag, pinned. | `git tag -a v0.1.0 -m "..." && git push origin v0.1.0`. Required checks only appear in the protection settings after the workflow has run once. |

## 07. Debugging and profiling tools on Linux

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 7.1 | E | `strace -f -tt -T -e trace=network` on tcpkv with an `nc` session. | Expect `socket`, `setsockopt`, `bind`, `listen`, `accept4`, `recvfrom`, `sendto`. `recvfrom(...) = 0` is the client's FIN: orderly shutdown. |
| 7.2 | E | `ltrace -c` on the Store tests; count `malloc` calls. | `ltrace` only sees calls through the PLT, so static or inlined calls are invisible; filter with `-e malloc+free`. |
| 7.3 | M | Flame graph of tcpkv under `load_test.sh`. | Add `-fno-omit-frame-pointer` to the release preset; `perf record -g -p $(pgrep -n tcpkv) -- sleep 15` while `scripts/load_test.sh 7000 8 20000` runs; Brendan Gregg's FlameGraph scripts render the SVG. |
| 7.4 | M | callgrind on `ParseCommand.*`; allocation share of the parser. | `valgrind --tool=callgrind ./build/debug/tests/tcpkv_tests --gtest_filter='ParseCommand.*'`; `callgrind_annotate --inclusive=yes callgrind.out.*`; compare `parse_command` inclusive with `operator new` under it. |
| 7.5 | H | Lock-order deadlock: gdb `thread apply all bt`, `/proc/PID/task/*/wchan`, TSan report. | `gdb -batch -x scripts/dump_stacks.gdb -p PID` dumps all stacks non-interactively. In each blocked thread, `p mutex` shows `__owner` = the LWP holding it. |
| 7.6 | H | `tcpdump -w` a tcpkv session, read in Wireshark and with `tcpdump -A`. | `sudo tcpdump -i lo -w kv.pcap port 7000`; copy to `/mnt/c/...` for Windows Wireshark; `tcpdump -r kv.pcap -A` for the text view. Flags: `[S]`, `[S.]`, `[P.]`, `[F.]`. |

## 08. Docker for C++

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 8.1 | E | `docker build` and `docker history`; name the three largest layers. | `docker history --no-trunc --format '{{.Size}}\t{{.CreatedBy}}' tcpkv:dev`. The apt layers dominate. |
| 8.2 | E | `docker stop` shows exit 0; without `sigaction`, exit 137 after 10 s. | `docker inspect -f '{{.State.ExitCode}}' tcpkv`. With tini as PID 1, tini forwards SIGTERM and the unhandled default action kills tcpkv at once (143). You see 137 after 10 s when tcpkv itself is PID 1 (`--entrypoint /usr/local/bin/tcpkv`): the kernel does not apply default "terminate" actions to a namespace's PID 1, so Docker times out and sends SIGKILL. Try both. |
| 8.3 | M | Runtime stage on `debian:12-slim` and distroless; strace via a sidecar. | `docker run --rm -it --pid=container:tcpkv --cap-add SYS_PTRACE ubuntu:24.04` then `apt install strace; strace -p 1`. |
| 8.4 | M | HEALTHCHECK goes `(healthy)`, then `(unhealthy)` after `docker kill -s STOP`. | The shipped Dockerfile already has the healthcheck; `docker ps` shows the state; `docker inspect -f '{{json .State.Health}}'` shows the probe log. |
| 8.5 | H | Compose stack; `psql` and `redis-cli` from the host; OOM-kill tcpkv with large values. | `psql -h 127.0.0.1 -U tcpkv`; set the compose memory limit to 64M; `scripts/load_test.sh 7000 4 50000 65000`; `docker inspect -f '{{.State.OOMKilled}}' tcpkv`. |
| 8.6 | H | Fully static tcpkv in a `FROM scratch` image. | `-static` with glibc warns about `getaddrinfo` (it needs NSS modules at run time); tcpkv only binds to an IP, so it works. Remove the `nc`-based healthcheck: scratch has no shell. |

## 09. CI/CD with GitHub Actions and Azure DevOps

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 9.1 | E | Push tcpkv with the workflow; all green; badge renders. | The workflow is `code/tcpkv/.github/workflows/ci.yml`; the badge URL needs your user name and the workflow file name. |
| 9.2 | E | A PR with an unused variable is blocked by `-Werror`. | `gh pr create`, then `gh run view --log-failed` and search for `-Werror=unused-variable`. |
| 9.3 | M | `format` and `tidy` jobs with `.clang-format` and `.clang-tidy`. | `clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.hpp')`; `clang-tidy -p build/ci src/*.cpp`. Configure first so `compile_commands.json` exists. |
| 9.4 | M | Smoke-test the built image in the `docker` job. | Build with `load: true` in `build-push-action`, then `docker run -d -p 7000:7000`, loop on `printf 'PING\n' \| nc -N localhost 7000 \| grep -q PONG` with a timeout, `docker rm -f`. |
| 9.5 | H | Nightly workflow (full Valgrind, `-O3 -flto`) and a tag-triggered release with two assets. | `on: schedule: - cron: '0 3 * * *'`; release job `if: startsWith(github.ref, 'refs/tags/v')`; `gh release create "$GITHUB_REF_NAME" tcpkv tcpkv.debug.tar.gz`; `docker/metadata-action` generates semver tags. |
| 9.6 | H | Port the workflow to Azure DevOps; JUnit results visible in the UI. | The file is `code/tcpkv/azure-pipelines.yml`; a free Azure DevOps organisation includes Microsoft-hosted minutes for public projects (check current limits). Gate the Docker stage on a variable if you have no registry. |

## 10. Scripting: bash and a little Python

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 10.1 | E | Run `m10_bash_patterns.sh` with `-v`; ShellCheck it and one of your own scripts. | `sudo apt install shellcheck`. The `PS4` variable defines the `-x` prefix: file, line, function. |
| 10.2 | E | A CRLF script fails with `bash\r`; fix with `dos2unix`; prevent with `.gitattributes`. | `file hello.sh` reports "with CRLF line terminators"; `cat -A` shows `^M$`. Rule: `*.sh text eol=lf`. |
| 10.3 | M | Demonstrate each `set -e` hole (`if`, `local x=$(false)`, `((x++))`, function under `\|\|`). | Put an `echo "still running"` after each; the ones that print are holes. Fix: separate declaration from assignment, `x=$((x+1))`, explicit checks inside functions. |
| 10.4 | M | Ctrl-C during `run_tests.sh` leaves no server behind. | The `INT` trap calls `exit 130`, which runs the `EXIT` trap (`stop_server`) once. Check with `pgrep -a tcpkv`. |
| 10.5 | M | `tcpkv_client.py --mix ... --threads 8 --seconds 10` closed-loop benchmark. | One `TcpkvClient` per thread; `random.choices` with weights for the op type; per-op latency lists merged at the end. It is slower than `load_test.sh` because every request waits for its reply (no pipelining) and Python adds overhead per call. |
| 10.6 | H | ctest integration fixture using the Python client. | Same as 5.6. Write a small `scripts/start_server.sh` and register it with `add_test(NAME start_server COMMAND ${CMAKE_SOURCE_DIR}/scripts/start_server.sh $<TARGET_FILE:tcpkv> 7123)` so ctest passes the real binary path via a generator expression. |

## 11. Linux systems programming essentials

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 11.1 | E | Each `m11_syscalls` demo under `strace -f`; name the syscalls behind `open`, `fork`, `waitpid`. | `openat`, `clone` (or `clone3`), `wait4`. `clock_gettime` runs in the vDSO without entering the kernel. |
| 11.2 | E | `ulimit -n 64`, start tcpkv, open 100 connections; see `EMFILE`. | Python: `socks = [socket.create_connection(("127.0.0.1", 7000)) for _ in range(100)]` in a try block. `cat /proc/$(pgrep -n tcpkv)/limits`. |
| 11.3 | M | Measure the cost of `fsync`; write the atomic-replace version. | `hyperfine './w_nosync' './w_sync'`. Directory fsync: `int d = open(".", O_RDONLY \| O_DIRECTORY); fsync(d);`. WSL's virtual disk makes absolute numbers unrepresentative; compare ratios. |
| 11.4 | M | `mini_shell.c` with one pipe and one `>` redirect. | Fork twice for a pipeline; in each child `dup2` the right pipe end then close both ends; the parent closes both ends and waits for both children. Forgetting a close makes `wc` wait forever. |
| 11.5 | M | tcpkv as a systemd service; crash it; exceed `StartLimitBurst`. | `sudo kill -SEGV $(systemctl show -p MainPID --value tcpkv)`. After the burst: `systemctl status` says `start-limit-hit`; `sudo systemctl reset-failed tcpkv` clears it. |
| 11.6 | H | epoll-based tcpkv server in one thread with output buffers and `signalfd`. | Start from `m11_epoll_preview.c` (rename to `.cpp`, or link a C file with the C++ core through a small `extern "C"` layer). Per connection: `std::string in, out`; enable `EPOLLOUT` only while `out` is non-empty. See `08_Networking_Distributed/06_nonblocking_io_epoll_event_loops.md`. |

## 12. Windows toolchain reference

| # | Lvl | Exercise | Hint |
|---|---|---|---|
| 12.1 | E | `cl` build plus `dumpbin /headers /dependents /pdbpath`; the same with `g++`, `readelf`, `ldd`. | Use the "Developer PowerShell for VS 2022" so `cl` and `dumpbin` are on `PATH`. |
| 12.2 | M | tcpkv core and tests under MSVC `/W4 /WX`; fix warnings portably. | Configure with `-G "Visual Studio 17 2022" -A x64` and build only `tcpkv_tests`. Prefer `static_cast` and correct types (`std::size_t`) over `#pragma warning(disable)`. |
| 12.3 | M | Reproduce and fix LNK2038 (`/MT` library, `/MD` exe). | `set_property(TARGET lib PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)` on one target only reproduces it; set `CMAKE_MSVC_RUNTIME_LIBRARY` once at the top to fix it. |
| 12.4 | M | `vcpkg.json` with `fmt`, built on Windows and WSL. | Pass `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` before the first configure (it cannot be added to an existing cache). Triplets: `x64-windows` and `x64-linux`. |
| 12.5 | H | Port `main.cpp` to Winsock with the `socket_t` shim; same Python self-test passes on both. | Replace `poll` with `WSAPoll`, `accept4` with `accept`, `MSG_NOSIGNAL` with 0, `localtime_r` with `localtime_s` (argument order differs). `SetConsoleCtrlHandler` runs on its own thread: set an `std::atomic<bool>`. |
| 12.6 | H | Crash dump on both platforms: ProcDump + WinDbg, coredumpctl + GDB. | WinDbg: `.symfix; .sympath+ <dir with your pdb>; .reload; !analyze -v; .ecxr; kp`. Linux: `coredumpctl list`, `coredumpctl gdb tcpkv`, `bt full`. |
