# 10. Scripting: bash for build, test and deploy, and a little Python

**Time:** ~2 hours   **Prerequisites:** 01, 04, 05

## 1. In plain words

A script is a text file of commands you would otherwise type. On Windows you used `.bat` files
or PowerShell for the same job: "build, copy the DLLs, run the tests, zip the output". On Linux
the default scripting language is **bash**, the same shell you type into. Every CI step, Docker
`RUN` line and deployment hook in this course is a small bash program.

Bash is very good at one thing: gluing other programs together (run `cmake`, pipe into `grep`,
check the exit code, start a server in the background, kill it at the end). It is bad at
everything else: arithmetic beyond integers, data structures, string parsing, error handling. The
senior skill is knowing where the line is. Write bash that fails loudly and cleans up after itself;
switch to Python the moment you need real logic, real data or a real protocol.

Analogy: bash is the wiring in a building. It connects the appliances (compilers, servers, test
runners) and it must be safe: a bad connection should trip the breaker (`set -e`), not smoulder.
You do not build the appliances out of wiring. When you need an appliance, you use Python.

## 2. The real thing

### 2.1 The first three lines of every script

```bash
#!/usr/bin/env bash
# what this script does, how to call it, what it exits with
set -euo pipefail
```

- `#!/usr/bin/env bash` (the **shebang**): when you run `./build.sh`, the kernel reads the first
  two bytes `#!`, and executes the named interpreter with the script path as an argument.
  `/usr/bin/env bash` finds bash on `PATH` (works on macOS and NixOS too). `#!/bin/sh` is a
  different, smaller language (dash on Ubuntu): no arrays, no `[[ ]]`, no `pipefail` before
  POSIX 2024. If you use bash features, say bash.
- `chmod +x build.sh` sets the execute bit. Without it: `Permission denied`. Git records the bit
  (`git update-index --chmod=+x` from Windows).
- **Line endings.** A script saved with CRLF fails with `/usr/bin/env: 'bash\r': No such file or
  directory`. The `\r` became part of the interpreter name. Fix with `.gitattributes`
  (`*.sh text eol=lf`, module 06) and `dos2unix`.

The three `set` options:

| Option | Meaning | What it prevents |
|---|---|---|
| `-e` (errexit) | exit when any command returns non-zero | a failed `cmake` followed by "tests passed" |
| `-u` (nounset) | expanding an unset variable is an error | `rm -r "$BUILD_DIR/"` with `BUILD_DIR` unset becomes `rm -r /` |
| `-o pipefail` | a pipeline's status is the last non-zero stage | `ctest \| tee log` returning tee's 0 when ctest failed |

`set -e` has well-known holes, and you must know them for interviews:

- A command whose status is **tested** never triggers it: in `if cmd`, `cmd || x`, `cmd && x`,
  `! cmd`, `while cmd`. That is deliberate: you are handling the failure.
- `local x=$(cmd)` and `export x=$(cmd)`: the status is that of `local`/`export`, which succeeded.
  Declare first, assign second.
- `((count++))` returns status 1 when the expression evaluates to 0, so it kills the script on the
  first increment from zero. Write `count=$((count + 1))`.
- Inside a function called from an `if` (or `||`), `set -e` is disabled for the whole function body.
- Command substitution in older bash did not inherit `-e`; bash 5.0+ has `shopt -s inherit_errexit`.

`set -e` is a safety net, not error handling. For anything that may legitimately fail, test it
explicitly.

### 2.2 Variables, quoting, arrays

```bash
name="tcpkv"                   # no spaces around '='
echo "$name"                   # always double-quote expansions
echo "${name}_tests"           # braces when followed by name characters
port="${1:-7000}"              # first argument, default 7000
: "${BUILD_TYPE:=Debug}"       # set to Debug if unset or empty
dir="$(dirname "$0")"          # command substitution; quotes nest inside $( )
n=$((3 * 4 + 1))               # integer arithmetic only
args=(--preset debug -j 8)     # array
cmake "${args[@]}"             # each element one argument, spaces preserved
```

**Why quoting matters.** An unquoted `$var` undergoes word splitting (on spaces, tabs, newlines)
and glob expansion (`*` becomes file names). `rm $file` with `file="my report.txt"` deletes `my`
and `report.txt`. Quote every expansion unless you specifically want splitting. `"$@"` expands to
all arguments, each one quoted: the correct way to forward arguments.

`[[ ... ]]` is bash's test command: `[[ -f path ]]` file exists, `-d` directory, `-x` executable,
`-z "$s"` empty string, `-n "$s"` non-empty, `==` glob match, `=~` regex match. Numbers compare
with `(( a < b ))`. Prefer `[[ ]]` over `[ ]`: no word splitting inside, `&&` and `||` work.

### 2.3 Functions, return values, output

```bash
log() { printf '%s %s\n' "$(date +%T)" "$*" >&2; }   # logs to stderr
die() { log "ERROR: $*"; exit 1; }

count_lines() {
    local file="$1"                     # 'local' or it leaks into the caller
    [[ -r "$file" ]] || return 1        # status: 0 success, 1-255 failure
    wc -l < "$file"                     # "return" data by printing it
}
n="$(count_lines access.log)" || die "cannot read access.log"
```

Functions return a status code (0 to 255), like a process. Data comes out on stdout and is
captured with `$( )`. Keep logs on **stderr** (`>&2`) so `result="$(myfunc)"` gets only data.

### 2.4 Loops and reading input

```bash
for f in src/*.cpp; do clang-format -i "$f"; done        # glob; use shopt -s nullglob for "no match = no loop"
for ((i = 1; i <= 8; i++)); do ...; done                  # C-style
while IFS= read -r line; do echo "[$line]"; done < cmds.txt   # line by line, exactly
some_cmd | while read -r a b rest; do ...; done            # split fields; runs in a subshell!
```

`IFS= read -r`: `IFS=` keeps leading and trailing spaces, `-r` keeps backslashes. The pipe form
runs the loop in a subshell, so variables set inside are gone afterwards; redirect from a file or
use process substitution `done < <(some_cmd)` when you need them.

### 2.5 Exit codes and traps

Every process exits with a status 0 to 255. 0 is success. Conventions that matter:

| Status | Meaning |
|---|---|
| 1 | generic failure |
| 2 | usage error (bash builtins, most CLI tools) |
| 124 | `timeout` killed the command |
| 125 | `git bisect run`: skip this commit |
| 126 | found but not executable |
| 127 | command not found |
| 128 + N | killed by signal N: 130 = SIGINT (Ctrl-C), 137 = SIGKILL (OOM killer, `docker kill`), 143 = SIGTERM |

`$?` is the status of the last command. `PIPESTATUS[@]` holds each stage of the last pipeline.

A **trap** runs code when the shell receives a signal or exits:

```bash
WORKDIR="$(mktemp -d)"
cleanup() {
    local rc=$?                         # read first: anything else overwrites it
    kill $(jobs -p) 2>/dev/null || true # stop background servers we started
    rm -r -- "$WORKDIR"
    exit "$rc"                          # preserve the original status
}
trap cleanup EXIT                       # runs on normal exit, on set -e failure, on 'exit N'
trap 'exit 130' INT                     # Ctrl-C -> exit -> EXIT trap runs once
trap 'exit 143' TERM                    # docker stop / CI cancel -> same
trap 'log "failed at line $LINENO: $BASH_COMMAND"' ERR    # with set -E, also inside functions
```

`EXIT` is the one to remember: it is bash's `finally`. Converting `INT` and `TERM` into `exit`
makes cleanup run exactly once with the right status. SIGKILL cannot be trapped by anything.

### 2.6 Background jobs, wait, timeout

```bash
./build/debug/tcpkv 7123 > server.log 2>&1 &   # '&' runs in the background
SERVER_PID=$!                                   # PID of the last background job
for i in 1 2 3; do worker "$i" & pids+=($!); done
for p in "${pids[@]}"; do wait "$p" || failed=$((failed + 1)); done   # wait PID returns its status
timeout 30 ./long_test || echo "status $?"      # 124 if it ran out of time
kill -0 "$SERVER_PID" 2>/dev/null               # "is it still alive?" without sending a signal
```

### 2.7 The tcpkv scripts

Four bash scripts live in `code/tcpkv/scripts/`. They are the commands CI runs, so any red
pipeline reproduces locally with one command.

| Script | Does |
|---|---|
| `build.sh [--clean] [preset]` | `cmake --preset` + `cmake --build --preset`; links `compile_commands.json` to the root |
| `run_tests.sh [preset] [ctest args]` | build, `ctest --preset`, then start a server on a free port, run the Python self-test and a short load burst, stop the server via trap |
| `load_test.sh [port] [clients] [ops] [value_bytes]` | N parallel pipelined clients through `nc`, counts replies, reports throughput, fails on any missing or `ERROR` reply |
| `bisect_test.sh [gtest filter]` | the `git bisect run` script from module 06: exit 125 on build failure |

### 2.8 When to switch to Python

Switch when any of these is true:

- You need a data structure beyond a flat list: maps, nested records, JSON, CSV with quoting.
- You need floating point (latency percentiles, averages).
- You are parsing a protocol or a structured format (sockets, HTTP, JSON, YAML).
- You need real error handling: retry this, catch that, clean up those three things in order.
- The script is over about 100 lines, or has more than two levels of nested `if`/loops.
- It must run on Windows too.

Keep bash when the script is a sequence of commands with exit-code checks: build, test, package,
deploy hooks, Docker `RUN` lines, CI steps. A useful rule many teams use: bash for orchestration,
Python for logic.

Python's standard library covers what you need without `pip`: `socket`, `subprocess`,
`argparse`, `json`, `threading`, `time`, `statistics`, `pathlib`. Ubuntu 24.04 ships Python 3.12.

### 2.9 Python driving tcpkv over a socket

`code/tcpkv/scripts/tcpkv_client.py` is a small client library plus four modes:

```bash
python3 scripts/tcpkv_client.py SET greeting hello world      # one command, prints the reply
python3 scripts/tcpkv_client.py --port 7123 --wait 5 --selftest   # assertions, exit 0/1
python3 scripts/tcpkv_client.py --bench 20000 --value-size 100    # p50/p95/p99 latency
python3 scripts/tcpkv_client.py --repl                            # interactive
```

The self-test checks basic commands, error replies, CRLF tolerance, a 64 KiB value, pipelined
requests, a request split across two TCP segments, eight concurrent clients and `QUIT`. It is
what `run_tests.sh` and the ctest integration test (module 05, exercise 6) run.

## 3. Code

### 3.1 `code/m10_bash_patterns.sh`

Every pattern from section 2 in one runnable file: `set -euo pipefail`, `IFS`, readonly constants,
logging to stderr, `getopts`, `mktemp` + `trap cleanup EXIT`, an `ERR` trap with line numbers,
functions with `local`, quoting and arrays, the four `set -e` gotchas, `PIPESTATUS`, background
jobs with `wait`, `timeout`, a `retry` function with exponential backoff, here-documents.

```bash
chmod +x m10_bash_patterns.sh
./m10_bash_patterns.sh          # run all demos
./m10_bash_patterns.sh -v       # with set -x and a PS4 showing file:line:function
shellcheck m10_bash_patterns.sh # sudo apt install shellcheck
```

The retry helper, which you will copy into many scripts:

```bash
# Retry DOWNLOADS and connections, never tests (a flaky test is a bug to fix).
retry() {
    local attempts="$1"; shift
    local delay=1 n
    for ((n = 1; n <= attempts; n++)); do
        if "$@"; then return 0; fi
        log "attempt $n/$attempts failed: $*; sleeping ${delay}s"
        sleep "$delay"
        delay=$((delay * 2))
    done
    return 1
}
retry 5 curl -fsSL -o deps.tar.gz "$URL"
```

### 3.2 `code/tcpkv/scripts/run_tests.sh` (the integration part)

```bash
# Pick a free port by asking the kernel for one (bind to port 0), so parallel CI jobs never collide.
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
SERVER_PID=""

stop_server() {
    local rc=$?
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID"                       # graceful: tcpkv drains and exits 0
        for _ in $(seq 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$SERVER_PID" 2>/dev/null || true   # same policy as systemd TimeoutStopSec
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if ((rc != 0)) && [[ -f "$SERVER_LOG" ]]; then tail -n 20 "$SERVER_LOG" >&2; fi
    exit "$rc"
}
trap stop_server EXIT

"$BUILD_DIR/tcpkv" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
python3 "$ROOT/scripts/tcpkv_client.py" --port "$PORT" --wait 5 --selftest
"$ROOT/scripts/load_test.sh" "$PORT" 4 2000
```

Points worth saying in an interview: the free-port trick avoids collisions between parallel CI
jobs (there is a tiny race between closing the probe socket and the server binding, acceptable
for tests); `--wait` polls instead of `sleep 1` (fast when the server is fast, robust when it is
slow); the trap prints the server log only on failure, which is when you need it.

### 3.3 `code/tcpkv/scripts/load_test.sh` (the generator)

```bash
# awk is far faster than a bash loop for 100k lines. srand(client) makes runs reproducible.
for ((c = 1; c <= CLIENTS; c++)); do
    awk -v c="$c" -v n="$OPS" -v vb="$VALUE_BYTES" 'BEGIN {
        srand(c)
        val = ""; for (j = 0; j < vb; j++) val = val "x"
        for (i = 0; i < n; i++) {
            k = "c" c "_k" int(rand() * 1000)
            r = rand()
            if      (r < 0.30) print "SET " k " " val
            else if (r < 0.90) print "GET " k
            else               print "DEL " k
        }
    }' > "$WORKDIR/req_$c.txt"
done

start_ns=$(date +%s%N)
for ((c = 1; c <= CLIENTS; c++)); do
    # -N: send FIN after the last request; the server answers, sees EOF, closes; nc exits.
    nc -N -w 30 "$HOST" "$PORT" < "$WORKDIR/req_$c.txt" > "$WORKDIR/resp_$c.txt" &
    pids+=("$!")
done
for p in "${pids[@]}"; do wait "$p" || failed=$((failed + 1)); done
```

Then it counts reply lines per client (`wc -l`) and `ERROR` lines (`grep -c ... || true`, because
`grep -c` exits 1 when the count is zero), prints one summary line and fails unless every request
got a reply. Usage from module 07: `./scripts/load_test.sh 7001 8 20000 &` while `perf record`
runs. Usage from module 08: `./scripts/load_test.sh 7000 4 50000 65000` to grow memory until the
container's limit is hit.

What this measures, honestly: throughput for **pipelined** requests (each client sends all its
requests without waiting). That is the server's best case. It says nothing about per-request
latency, which is what users feel. For latency, use the Python benchmark.

### 3.4 `code/tcpkv/scripts/tcpkv_client.py`

The core of the client:

```python
class TcpkvClient:
    def __init__(self, host: str, port: int, timeout: float = 5.0) -> None:
        # Timeout applies to connect() and every later recv()/send(). No timeout = a hung server hangs us.
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # readline() over a buffered reader handles a reply split across TCP segments.
        self.reader = self.sock.makefile("rb")

    def request(self, line: str) -> str:
        self.sock.sendall(line.encode() + b"\n")      # sendall loops over partial sends
        raw = self.reader.readline(MAX_VALUE + 64)
        if not raw:
            raise TcpkvError("server closed the connection")
        return raw[:-1].decode(errors="replace")

    def get(self, key: str) -> Optional[str]:
        reply = self.request(f"GET {key}")
        if reply == "NOT_FOUND":
            return None
        if reply.startswith("VALUE "):
            return reply[len("VALUE "):]
        raise TcpkvError(f"GET {key}: unexpected reply {reply!r}")
```

The latency benchmark sends requests one at a time and records each round trip with
`time.perf_counter_ns()`, then sorts and reads percentiles:

```python
lat_us.sort()
def pct(p: float) -> float:
    return lat_us[min(len(lat_us) - 1, int(p / 100.0 * len(lat_us)))]
print(f"p50={pct(50):.0f} p95={pct(95):.0f} p99={pct(99):.0f} us")
```

Note that Python's own overhead (tens of microseconds per request, typical) is included in the
number. For tcpkv on loopback that overhead can dominate; for comparing two server versions with
the same client it is fine. A C++ client or a tool like `memtier_benchmark` removes it.

Run everything:

```bash
cd ~/projects/tcpkv
chmod +x scripts/*.sh scripts/*.py
scripts/run_tests.sh                       # debug: unit + integration
scripts/run_tests.sh asan                  # the same under ASan + UBSan
./build/debug/tcpkv 7000 &
scripts/load_test.sh 7000 8 20000
python3 scripts/tcpkv_client.py --bench 20000
kill %1
```

## 4. Under the hood (senior-level)

**How bash runs a command.** For `cmake --build build`, bash parses the line, performs expansions
in a fixed order (brace, tilde, parameter and arithmetic and command substitution, word splitting,
glob), then searches `PATH` (with a hash cache, `hash -r` clears it), `fork()`s, and in the child
applies redirections with `open` + `dup2` and calls `execve`. The parent `waitpid`s for the child
unless the command ends in `&`. Builtins (`cd`, `export`, `read`, `[[`) run inside the shell
process; that is why `cd` cannot be an external program. `strace -f -e trace=execve,clone,dup2
bash -c 'ls | wc -l'` shows all of it (module 11 covers fork and exec).

**Pipelines are concurrent.** `a | b | c` creates two pipes and three processes at once; data
flows through 64 KiB kernel buffers (default Linux pipe capacity). When `c` exits early (`head`),
the next write by `b` gets SIGPIPE and `b` dies with status 141. With `pipefail` that 141 fails
the pipeline: `yes | head -1` under `set -o pipefail` returns 141. That is the one place
pipefail surprises people; `|| true` or `set +o pipefail` locally is the fix.

**Subshells.** `( ... )`, `$( ... )`, every stage of a pipeline except (with `shopt -s lastpipe`)
the last, and `&` jobs run in a forked copy of the shell. Variable changes inside never reach the
parent. Traps are reset in subshells unless `set -E` (for ERR) is used.

**Signals and background jobs.** A non-interactive script's background jobs ignore SIGINT and
SIGQUIT. When the terminal sends Ctrl-C to the foreground process group, the script gets it but
its `&` children do not, which is why scripts without a cleanup trap leave servers running. While
bash is waiting for a foreground child, it does not run trap handlers until the child exits; with
`wait` (a builtin) the trap runs immediately. That is why long-running scripts use `cmd & wait $!`
when they need responsive signal handling.

**`$(date +%s%N)` and timing.** Each `$( )` is a fork + exec, about a millisecond (typical); a
bash loop doing 100,000 of them takes minutes. That is why the load generator uses one `awk`
process to write 20,000 lines in milliseconds. In bash 5, `$EPOCHREALTIME` gives microsecond
time without forking.

**Python's socket layer.** `socket.create_connection` is `getaddrinfo` + `socket` + `connect` with
`SO_` options; `settimeout` puts the fd in non-blocking mode internally and uses `poll` to
implement the timeout, so `strace` shows `poll` before every `recvfrom`. `makefile("rb")` wraps the
socket in a `BufferedReader` which issues `recv` of 8 KiB (typical buffer size) and serves
`readline` from the buffer. The GIL is released during blocking socket calls, so eight threads of
I/O-bound clients do overlap, which is why the concurrency part of the self-test is meaningful.

## 5. In production

- **Every CI step is a bash script.** GitHub Actions runs `run:` blocks with `bash -e {0}` by
  default, which has `-e` but not `-o pipefail`. `ctest | tee log.txt` in a `run:` step passes when
  tests fail. Add `shell: bash` (which GitHub runs as `bash --noprofile --norc -eo pipefail {0}`)
  or put the logic in `scripts/*.sh` with its own `set -euo pipefail`.
- **The deploy script that deleted the wrong directory.** A classic incident pattern:
  `rm -rf "$PREFIX/$APP_DIR"` with both variables unset on one host. `set -u` turns it into an
  error. Also use `${VAR:?message}` for variables that must be set: `rm -r -- "${INSTALL_DIR:?}"`.
- **Leaked background processes.** A test script that starts a server and fails before `kill`
  leaves the port bound; the next CI run on a self-hosted agent fails with "Address already in
  use". `trap cleanup EXIT` plus `kill $(jobs -p)` prevents it.
- **Hangs.** A script waiting on a dead server waits forever and burns CI minutes until the
  6-hour job limit (GitHub default). Put `timeout` on anything that talks to the network, and
  timeouts on every socket in Python.
- **Your Windows background.** The equivalent of a `.bat` that ignores `%ERRORLEVEL%` is a bash
  script without `set -e`. If your current team's build scripts are PowerShell, the same rules
  apply: `$ErrorActionPreference = 'Stop'`, check `$LASTEXITCODE` after native commands,
  `try/finally` for cleanup. Say so in interviews: you know the failure modes, not just the syntax.
- **ShellCheck in CI.** `shellcheck scripts/*.sh` finds unquoted variables, `local x=$(cmd)`,
  unused variables, and portability issues. Add it as a CI job next to clang-format.
- **3 AM:** the nightly backup script reported success for a week while writing empty files. It
  ran `pg_dump ... | gzip > backup.gz`; `pg_dump` failed on authentication after a password
  rotation, `gzip` happily compressed zero bytes and returned 0. No `pipefail`. The fix is one
  line; the lesson is to verify outputs (size, `gzip -t`, a test restore), not just exit codes.

## 6. Try it

1. **(easy)** Run `code/m10_bash_patterns.sh` and then with `-v`. Then run `shellcheck` on it and
   on a script of your own. Done when you can explain every line of the `-v` trace prefix and fix
   every ShellCheck warning in your script.
2. **(easy)** Write `hello.sh` with CRLF line endings (save it from Notepad on `/mnt/c`), run it in
   WSL, read the error, fix it with `dos2unix`, and add a `.gitattributes` rule that prevents it.
   Done when you can explain why the error message mentions `bash\r`.
3. **(medium)** Demonstrate each `set -e` hole from section 2.1 in a tiny script: the `if`, the
   `local x=$(false)`, the `((x++))`, and a function called from `||`. Done when the script prints
   which ones did and did not abort, and you can say how to fix each.
4. **(medium)** Run `scripts/run_tests.sh` and then press Ctrl-C during the integration part. Done
   when `pgrep tcpkv` shows no leftover server, and you can explain the order in which the `INT`
   trap and the `EXIT` trap ran.
5. **(medium)** Extend `tcpkv_client.py` with `--mix get=60,set=30,del=10 --threads 8 --seconds 10`
   that runs a closed-loop workload and prints throughput and p50/p99 latency per operation type.
   Done when it runs against the release build and its throughput is within the same order of
   magnitude as `load_test.sh` (it will be lower; explain why).
6. **(hard)** Add the ctest integration test from module 05 exercise 6 using this client: a
   `FIXTURES_SETUP` test that starts the server with a small wrapper script, the `--selftest` test,
   and a `FIXTURES_CLEANUP` test that stops it. Done when `ctest --preset debug -L integration`
   passes, and a deliberately broken `Store::del` makes it fail with a readable message.

## 7. Interview questions

**Q: What does `set -euo pipefail` do and why use it?**
`-e` exits on the first failing command, `-u` makes unset variables an error, `-o pipefail` makes a
pipeline fail if any stage fails rather than only the last. Together they turn silent failures
(a build that failed followed by "deploy OK", an `rm` with an empty variable, `cmd | tee` hiding
cmd's failure) into immediate, visible exits.

**Q: When does `set -e` not stop the script?**
When the failing command's status is being tested: in an `if` or `while` condition, on the left
of `&&` or `||`, after `!`, and anywhere inside a function called from such a context. Also
`local x=$(cmd)` because the status is `local`'s, and `((x++))` evaluating to 0 kills the script
unexpectedly. It is a net, not a substitute for explicit checks.

**Q: How do you make sure a script cleans up temporary files and background processes?**
`mktemp -d` for the directory, a `cleanup` function registered with `trap cleanup EXIT` that saves
`$?`, kills `$(jobs -p)`, removes the directory, and exits with the saved status; plus `trap 'exit
130' INT` and `trap 'exit 143' TERM` so signals take the same path. Nothing handles SIGKILL, so
temp dirs live under a location that is cleaned anyway.

**Q: What do exit codes 127, 137 and 143 mean?**
127: command not found. 137 = 128 + 9: killed by SIGKILL, typically the OOM killer or `docker
kill`/a stop timeout. 143 = 128 + 15: terminated by SIGTERM, typically a normal `docker stop` or
`systemctl stop` of a program with no handler.

**Q: Why quote `"$var"`?**
Unquoted expansions are word-split on IFS and glob-expanded, so a value with spaces becomes several
arguments and a `*` becomes file names. Quoting passes the value as exactly one argument; `"$@"`
forwards all arguments intact.

**Q: When would you write a script in Python instead of bash?**
When it needs data structures, floating point, structured formats (JSON, CSV, a socket protocol),
non-trivial error handling, or grows past roughly a hundred lines. Bash stays for orchestration:
running tools in sequence and checking exit codes.

**Q: How did you load-test tcpkv?**
A bash generator writes per-client request files with `awk` and streams them through parallel `nc`
connections, verifying that every request got a reply and none was an error; that gives pipelined
throughput. A Python client measures per-request round-trip latency percentiles on one
connection. I ran the server under `perf record` while the generator ran to find the hot spots.

## 8. Recap

- First lines: `#!/usr/bin/env bash`, a usage comment, `set -euo pipefail`; LF line endings, `chmod +x`.
- Quote every expansion; `"$@"` forwards arguments; arrays for argument lists; `[[ ]]` and `(( ))`.
- `set -e` holes: tested commands, `local x=$(...)`, `((x++))`, functions called from `if`/`||`.
- Functions return status; data on stdout; logs on stderr; `local` everything.
- `trap cleanup EXIT` is bash's `finally`; map `INT`/`TERM` to `exit 130`/`exit 143`; read `$?` first.
- Exit codes: 0 ok, 1 fail, 2 usage, 124 timeout, 125 bisect skip, 127 not found, 128+N signal N.
- `&`, `$!`, `wait PID`, `kill -0`, `timeout`; never leave a server running after a script.
- Retry downloads, never tests; put `timeout` on network operations.
- Switch to Python for data, floats, protocols, real error handling, or more than ~100 lines.
- tcpkv: `build.sh`, `run_tests.sh` (unit + live integration), `load_test.sh` (pipelined throughput), `tcpkv_client.py` (self-test, latency).
