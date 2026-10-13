# Step 10. Polish, benchmark, release v1.0.0, go public, write the resume bullets

**Time:** ~7 hours   **Week:** 5 (Nov 03 – Nov 09). Deadline: tagged and public by Monday Nov 09.
**Read first:** 06_Linux_Toolchain_DevOps: `06_git_properly.md` (annotated tags, clean
history, what never to commit), `09_ci_cd_github_actions_and_azure_devops.md` (badges,
release triggers), `07_debugging_and_profiling_tools_on_linux.md` (`perf stat`, `taskset`).
11_Interview_Prep_Job_Search: `08_resume_linkedin_github.md` (projects section, pinned repos),
`02_telling_your_story.md`. 08_Networking_Distributed: `05_thread_per_connection_and_thread_pools.md`
(so you can explain the shape of your numbers).

## 1. Goal

Turn a working repository into evidence.

* A README a reviewer understands in 30 seconds and can verify in 5 minutes: badges,
  demo, architecture diagram, protocol table, build commands, **your** benchmark numbers with
  the machine they came from, "what I would do next".
* `docs/PROTOCOL.md` complete; formatting enforced in CI; version 1.0.0 in CMake.
* An annotated tag `v1.0.0` and a GitHub release with notes.
* The repository public at `github.com/khanshaki-r/tcpkv`, with a description and topics,
  pinned on your profile.
* Resume bullets rewritten so every word is true of the tagged code, with the ops/s and p99
  numbers filled in from your own benchmark.

In plain words: steps 01 to 09 built the thing. This step is the shop window and the price
tag. A recruiter gives the repo two seconds (is it real? is CI green?), a hiring engineer
gives it five minutes (does the README make sense? do the tests exist? is the code clean?),
and an interviewer gives it an hour of questions. This step is for all three.

## 2. What a reviewer actually checks

| time | who | what they look at | what must be true |
|------|-----|-------------------|-------------------|
| 2 s | recruiter | repo exists, pinned, green badge, description | public, CI green on `main`, one-line description |
| 30 s | engineer | README top: what it is, demo, diagram | first screen explains the project without scrolling |
| 5 min | engineer | `src/server.cpp`, `src/store.cpp`, `tests/`, `.github/workflows/ci.yml`, commit history | no `TODO` in hot paths, tests named after behaviour, sanitizer jobs real, commits with messages |
| 1 h | interviewer | any line they pick | you can explain why that line exists |

The last row is why you typed every line yourself (README of this course, section 5).

## 3. Files to create or change

### 3.1 `README.md` (final)

Start from `code/README.md`; it follows the outline in `00_spec.md` section 9. Keep its
structure, rewrite the prose in your own words (an interviewer may quote it back to you),
and fill in the benchmark section from section 4 below. The parts that carry the most weight:

**Badges** (line 3 onwards). The CI badge URL is derived from the workflow file name, so it
works as soon as the repo is public:

```markdown
[![CI](https://github.com/khanshaki-r/tcpkv/actions/workflows/ci.yml/badge.svg)](https://github.com/khanshaki-r/tcpkv/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-informational)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey)
```

The CI badge shows the status of the latest run on the default branch. A red badge on a
pinned repo is worse than no badge. If CI goes red after you are public, fixing it is the
first task of the day.

**Demo session** directly under the description: ten lines of `tcpkv_cli` output, copied
from a real run (not typed from memory; the output format must match the code).

**Architecture diagram.** ASCII, so it renders everywhere and diffs in git:

```
                 +-----------------------------------------------------------+
   clients       |                      tcpkv_server                         |
                 |                                                           |
  nc / cli ----->|  acceptor thread                    worker pool (N)       |
  load_gen ----->|  poll(listener, wake_pipe)          +-----------------+   |
  Client lib --->|    accept4()                        | Connection      |   |
                 |    Connection{Socket}  --submit-->  |  poll/read      |   |
                 |    pool.submit(move)                |  Parser.feed    |   |
                 |                                     |  execute -------+---+---> Store
                 |  every 1 s: store.purge_expired()   |  write_all      |   |     unordered_map
                 |  every 10 s: log ops/s, p99         +-----------------+   |     + shared_mutex
                 |                                                           |
   SIGINT/TERM ->|  request_stop(): atomic flag + write(wake_pipe)           |
                 +-----------------------------------------------------------+
```

followed by the five design bullets (one connection per worker, `shared_mutex` store, RAII,
`MoveOnlyFunction`, self-pipe shutdown). Each bullet names a trade-off, not just a feature:
"simple, debuggable, fast enough for a few thousand connections; `epoll` would be next".

**Protocol table** and a link to `docs/PROTOCOL.md`. If you added the `EXPIRE` bound from
step 09, add it to the limits sentence.

**Build, test, sanitizers, Valgrind**: copy-pasteable commands, exactly the ones CI runs.

**Benchmark**: section 4 of this step.

**What I would do next**: epoll event loop, persistence (point to minidb if it exists by
then; otherwise say "append-only log with snapshots"), replication, sharded locks *if
profiling shows writer contention*. Four bullets, each one a conversation you are ready to
have (see `INTERVIEW_TALKING_POINTS.md`).

### 3.2 `docs/PROTOCOL.md`

Identical to `code/docs/PROTOCOL.md`: transport, encoding, request grammar, every command
with its reply, error list split into fatal and non-fatal, limits, a session example. Read
it once against `src/protocol.cpp` line by line. Every error message string in the doc must
appear verbatim in the code and in a test. A protocol document that disagrees with the code
is worse than none.

### 3.3 `CMakeLists.txt`

```cmake
project(tcpkv
    VERSION 1.0.0
    DESCRIPTION "Multithreaded TCP key-value server"
    LANGUAGES CXX)
```

### 3.4 `.github/workflows/ci.yml`: formatting becomes required

Format the whole tree once, commit that alone, then delete the `continue-on-error: true`
line (and the comment above it) from the `format` job:

```bash
find include src apps tools tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i
git diff --stat            # whitespace only; read it anyway
cmake --build --preset debug && ctest --preset debug
git commit -am "Apply clang-format to the whole tree (no functional change)"
```

A formatting-only commit keeps `git blame` useful: reviewers can skip it
(`git blame --ignore-rev <sha>`), and it never hides a logic change.

### 3.5 Check the repository for things that must not be public

Before the visibility switch, not after:

```bash
git log --all --oneline | wc -l                         # history you are about to publish
git log --all -p | grep -inE 'password|secret|token|api[_-]?key|BEGIN (RSA|OPENSSH)' | head
git log --all --format='%an <%ae>' | sort -u            # author identities
git ls-files | grep -vE '^(include|src|apps|tools|tests|docs|\.github)/|^(CMakeLists.txt|CMakePresets.json|README.md|LICENSE|\.gitignore|\.clang-format)$'
```

* **Nothing from your employer.** No Extrieve code, file names, table names, client names,
  hostnames or log excerpts, not even in a commit message or a test fixture. tcpkv is
  written from scratch; keep it provably so. This is also a contractual point: read your
  employment agreement's IP clause, and if in doubt keep the repo strictly generic (it is).
* **Author identity.** Commits should carry the name and email you want recruiters to see
  (`git config user.name`, `git config user.email`). GitHub can hide your email with the
  `@users.noreply.github.com` address (Settings, Emails).
* The last command lists tracked files outside the expected layout: build directories,
  core dumps, `.vscode/` or `vgcore.*` files must not be there (`.gitignore` from step 01
  covers them; check anyway).

If a secret ever was committed, deleting it in a new commit is not enough: rewrite history
(`git filter-repo`) *and* rotate the secret. For this project there should be nothing to find.

## 4. Benchmark: the procedure and the table

Numbers on a resume must be reproducible by you on demand and labelled with where they came
from. The procedure below is what you will describe if asked "how did you measure that?".

### 4.1 Record the machine

```bash
lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket'
nproc
free -h | head -2
uname -r                      # a "-microsoft-standard-WSL2" suffix means WSL2
cat /etc/os-release | grep PRETTY_NAME
g++ --version | head -1
```

Your WSL is limited to 4 CPUs and 3 GB by `.wslconfig`. Say so in the README: "WSL2 on
Windows 11, 4 vCPUs (Intel/AMD <model>), 3 GB RAM, kernel <x>, GCC 13.3, release build".
Numbers from a VM on a laptop are fine for a portfolio as long as they are labelled; they
are wrong only when presented as something else.

### 4.2 Run

```bash
cd ~/dev/tcpkv
cmake --preset release && cmake --build --preset release

# Terminal 1: server on cores 0-1, logging quiet
taskset -c 0,1 ./build/release/tcpkv_server --port 7379 --threads 8 --stats-interval 0 --log-level warn

# Terminal 2: load generator on cores 2-3; 5 s warm-up run first, discard it
taskset -c 2,3 ./build/release/load_gen --port 7379 --clients 8 --duration 5
for run in 1 2 3; do
  taskset -c 2,3 ./build/release/load_gen --port 7379 --clients 8 --duration 20 \
      --value-size 64 --read-ratio 0.8 | tee -a bench_8c_4t_64b.txt
done
./build/release/tcpkv_cli stats           # server-side latency, for the note under the table
```

* **`taskset`** gives server and load generator separate cores. On 4 vCPUs, sharing cores
  means the load generator steals CPU from the server and you measure both. Note the split
  in the README.
* **Three runs, report the median** of ops/s and of each percentile. If the three differ by
  more than about 10%, something else is running (Windows Update, the IDE indexer); find it
  and rerun.
* **Repeat for each row** of the table, restarting the server with that row's `--threads`
  (the commands above are for the `8 clients / 8 threads` row). Restarting also resets
  `STATS` to that row. For 8 or 16 threads on
  a 2-core server allocation, say so: more threads than cores is a legitimate configuration
  for a blocking server (threads mostly wait on I/O), but the reader should know.
* **Do not tune for the benchmark.** No `-march=native`, no special flags beyond the
  `release` preset. The point is "this is what the code does".

### 4.3 The table (template)

Copy into the README and fill from your runs. Leave a row out rather than invent it.

```markdown
## Benchmark

Measured on: WSL2 on Windows 11, <CPU model>, 4 vCPUs, 3 GB RAM, kernel <uname -r>, GCC 13.3,
`release` preset. Server pinned to 2 vCPUs, `load_gen` to the other 2, loopback TCP.
Each row is the median of three 20-second runs after a 5-second warm-up. Value size 64 B
unless stated. Latencies are client-observed round trips from `load_gen`, reported as
histogram bucket upper bounds ("p99 <= X us").

| clients | server threads | value size | read ratio | ops/s | GET p50 | GET p99 | SET p99 |
|--------:|---------------:|-----------:|-----------:|------:|--------:|--------:|--------:|
|       1 |              4 |       64 B |       0.80 |       |         |         |         |
|       4 |              4 |       64 B |       0.80 |       |         |         |         |
|       8 |              8 |       64 B |       0.80 |       |         |         |         |
|      16 |             16 |       64 B |       0.80 |       |         |         |         |
|      16 |             16 |      4 KiB |       0.80 |       |         |         |         |
|      64 |              8 |       64 B |       0.50 |       |         |         |         |

Server-side command execution time (`STATS`, row 4): p50 <= <x> us, p99 <= <y> us.
The last row deliberately has more clients than worker threads; see the note below the table.
```

**Why clients <= threads in every row but the last.** A connection holds a worker for its
whole life, and a `load_gen` client stays connected until its deadline. With 16 clients on 8
threads, 8 clients would sit in the pool queue for the entire run and the row would really
measure 8 clients. (The benchmark template in `code/README.md` has `16 clients / 8 threads`
rows; replace them with the rows above.) The same applies to every `load_gen` run in this
course: when you want N truly concurrent clients, start the server with `--threads N` or more.

How to read your own numbers, because you will be asked:

* **Row 1 is latency-bound.** One client waits for each reply before sending the next
  (closed loop), so ops/s is roughly `1 / round-trip time`. It measures loopback plus two
  syscalls each way plus the store, not throughput.
* **Rows 2 to 4 should scale** with clients until the server's cores saturate. If they do
  not, the bottleneck is elsewhere: the load generator's cores, or the store lock under
  writes. `perf top -p $(pgrep tcpkv_server)` tells you which.
* **Row 6 has more clients than threads.** With 64 clients and 8 workers, 56 connections
  wait in the pool queue. Their *connect* succeeds (the kernel completed the handshake and
  the acceptor accepted them) but no worker reads from them. And a `load_gen` client never
  leaves before its deadline, so a worker frees up only when one of the first 8 clients
  finishes at the end of the run. Predict the result before you run it: ops/s about the same
  as 8 clients, and most of the 56 queued clients completing exactly one operation, at the
  very end, after waiting roughly the whole run, which puts the tail percentiles in the top
  buckets. That row is the honest demonstration of the thread-per-connection limit, and the
  reason epoll is first in "what I would do next". Do not delete it because it looks bad; it
  is the most interesting row. (If your numbers do not match the prediction, find out why
  before publishing.)
* **Server p99 tiny, client p99 large** means the time is in the network stack, scheduling
  and queueing, not in `Store`. Say that sentence with your numbers in it.
* **Closed-loop load hides queueing delay** (coordinated omission): a slow reply delays the
  next request, so the slow period is under-sampled. Open-loop generators (wrk2-style,
  constant arrival rate) avoid this. Knowing the term is a senior signal.

### 4.4 The two numbers for the resume

Pick **one configuration** and use its numbers everywhere (resume, LinkedIn, interviews):
the 16-client, 16-thread, 64-byte, 80%-read row is a good default. You need:

* **ops/s**: the median from that row. Round *down* to two significant figures
  (e.g. 41,730 becomes "~41k"). Never round up.
* **p99**: the GET p99 bucket bound from the same row, "p99 <= 200 us" style. If you prefer
  a single p99 for all operations, run `load_gen` with the same settings and use the larger of
  the GET and SET p99 values.

Write them, with the machine description, into a file in your notes, not just the README:
when an interviewer asks in March, you want the exact numbers and conditions.

## 5. Tag, release, publish, pin

### 5.1 Tag and release

```bash
git switch main && git pull
git status                                    # clean
cmake --preset release && cmake --build --preset release && ctest --preset release

git tag -a v1.0.0 -m "tcpkv 1.0.0

Multithreaded TCP key-value server in C++17.
- Text protocol with length-prefixed values: PING GET SET DEL EXISTS KEYS EXPIRE TTL STATS QUIT
- Fixed thread pool, move-only Connection handles, graceful SIGINT/SIGTERM shutdown
- In-memory store behind std::shared_mutex with lazy + periodic TTL expiry
- Client library, tcpkv_cli, load_gen (C++ and Python) with per-GET verification
- GoogleTest suites; CI: debug, release, ASan, TSan, UBSan, Valgrind memcheck under load"
git push origin v1.0.0
```

An **annotated** tag (`-a`) stores the tagger, date and message; a lightweight tag is just a
name for a commit. Releases use annotated tags. The `tags: ["v*"]` trigger from step 08 runs
CI on the tag. Wait for it to be green, then create the release:

```bash
gh auth status                                # once: gh auth login
cat > /tmp/release_notes.md <<'EOF'
First stable release. See README for architecture, protocol and benchmark numbers
(<CPU>, WSL2, 4 vCPUs): ~<N> ops/s with 16 clients, GET p99 <= <X> us.

Build: cmake --preset release && cmake --build --preset release
EOF
gh release create v1.0.0 --title "tcpkv 1.0.0" --notes-file /tmp/release_notes.md
```

(Use a `mktemp` file instead of `/tmp/release_notes.md` if you prefer; delete it afterwards.)

If you find a bug after tagging, do not move the tag. Fix it on `main` and tag `v1.0.1`.
Moving a published tag breaks everyone who fetched it, and it looks like hiding something.

### 5.2 Make it public, describe it, pin it

```bash
gh repo edit khanshaki-r/tcpkv --visibility public --accept-visibility-change-consequences
gh repo edit khanshaki-r/tcpkv \
  --description "Multithreaded TCP key-value server in C++17: thread pool, shared_mutex store, RAII sockets, GoogleTest, ASan/TSan/UBSan/Valgrind CI" \
  --add-topic cpp17 --add-topic tcp-server --add-topic key-value-store \
  --add-topic multithreading --add-topic linux --add-topic cmake --add-topic googletest
```

(The web route: repo Settings, General, Danger Zone, "Change repository visibility".)

Pinning has no `gh` command. On github.com: your profile page, "Customize your pins", tick
`tcpkv`, save. Put it first. Then open your profile in a private browser window, logged
out, and check what a stranger sees: the pin, the description, the green badge on the
README.

### 5.3 Fresh-clone check

The ultimate test of "builds clean": a clone on a machine state that is not yours.

```bash
dir=$(mktemp -d) && cd "$dir"
git clone https://github.com/khanshaki-r/tcpkv && cd tcpkv
git checkout v1.0.0
cmake --preset release && cmake --build --preset release && ctest --preset release
./build/release/tcpkv_server --port 7390 & sleep 1
./build/release/load_gen --port 7390 --clients 4 --duration 3
kill -INT %1; wait
```

If this fails, the repo depends on something only your machine has (an untracked file, a
locally installed package). Fix and tag `v1.0.1`.

## 6. The resume bullets: true, specific, checkable

### 6.1 The rule

Every noun in a bullet must point at code on `main` at the tag. Every number must come from
section 4 with its conditions written down. Anything else is a claim the interviewer can
disprove in thirty seconds, and after that every other line on the resume is in doubt too.

### 6.2 Check the current bullets against the repo

| claim on the resume (from the course README) | where it is true | verdict |
|---|---|---|
| Concurrent TCP server with a fixed thread pool | `ThreadPool`, `Server::run` | true |
| text protocol | `Parser`, `docs/PROTOCOL.md` | true |
| in-memory store guarded by shared_mutex | `Store` | true |
| RAII socket and file-descriptor wrappers | `FileDescriptor`, `Socket` | true |
| move-only connection handles | `Connection`, `MoveOnlyFunction` | true |
| Builds clean under -Wall -Wextra -Werror with CMake | `tcpkv_warnings`, every CI job | true (and stricter: also `-Wpedantic -Wshadow`) |
| GoogleTest suite for protocol parsing and store operations | `tests/protocol_test.cpp`, `tests/store_test.cpp` | true (understated: also fd, thread pool, integration) |
| Valgrind-clean under concurrent load | `tools/valgrind_check.sh`, CI `valgrind` job | true |

### 6.3 Claims you must not make about v1.0.0

Other parts of this curriculum sketch a bigger tcpkv. In particular
`11_Interview_Prep_Job_Search/02_telling_your_story.md` and `08_resume_linkedin_github.md`
mention **sharded locking / lock striping, an append-only log with replay, LRU eviction with a
memory cap, and an epoll variant**. None of those is in v1.0.0. Until one is merged, tested
and tagged (v1.1.0 and later), it does not go on the resume, the LinkedIn profile, or into
your spoken story. Edit your copy of those story paragraphs to match what you built. In an
interview they become "what I would do next", which is a strong answer when you can explain
the design and its trade-offs.

Also avoid: "high-performance" (compared to what?), "production-ready" (it has no
persistence, auth or TLS), "Redis-compatible" (the protocol is RESP-*like*, not RESP),
"lock-free store" (the histogram is lock-free; the store is not).

### 6.4 The bullets

Project heading line:

> **tcpkv — Multithreaded TCP Key-Value Server** · C++17, Linux, CMake, GoogleTest ·
> github.com/khanshaki-r/tcpkv

Bullets (fill the brackets from section 4.4; delete the bracket text if you have no number):

> - Concurrent TCP server with a fixed thread pool and a length-prefixed text protocol over an
>   in-memory store guarded by `std::shared_mutex`, with per-key TTL; RAII socket and
>   file-descriptor wrappers, move-only connection handles, and graceful SIGINT/SIGTERM
>   shutdown via the self-pipe trick.
> - Sustains ~[N]k ops/s with 16 concurrent clients at GET p99 <= [X] µs (80% reads, 64 B
>   values, 4-vCPU WSL2 VM, loopback), measured with a load generator that verifies every
>   read against the last write.
> - Builds clean under -Wall -Wextra -Wpedantic -Werror with CMake; GoogleTest suites for
>   protocol parsing, store operations, the thread pool and end-to-end server behaviour; CI
>   runs them under ASan, TSan and UBSan and runs the server Valgrind-clean under concurrent load.

Why each change from the original two bullets:

* The **metric bullet** is new and carries the numbers. It states the conditions in the
  same line, so it cannot be read as a production claim.
* "length-prefixed" and "per-key TTL" are true and are better interview hooks than "text
  protocol" alone: each invites a question you have a good answer to.
* The CI clause names the three sanitizers, which are exactly the keywords Linux C++ job
  descriptions use, and each is backed by a green job anyone can click.

Three bullets is the maximum for a side project on a 2.5-years-experience resume. If space is
tight, merge the first and third and keep the metric bullet.

LinkedIn "Projects" entry: the same three bullets plus the repo link. GitHub profile README
(optional): one line, "tcpkv: multithreaded TCP KV server in C++17, [N]k ops/s, ASan/TSan/
Valgrind-clean CI", linking to the repo.

## 7. Build and run (WSL Ubuntu 24.04): the release checklist in order

```bash
cd ~/dev/tcpkv
# 1. everything green locally
for p in debug release asan ubsan; do cmake --preset $p && cmake --build --preset $p && ctest --preset $p || break; done
setarch $(uname -m) -R cmake --preset tsan && setarch $(uname -m) -R cmake --build --preset tsan && setarch $(uname -m) -R ctest --preset tsan
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo && bash tools/valgrind_check.sh build/relwithdebinfo
# 2. format, commit; 3. benchmark (section 4); 4. README + PROTOCOL + version, commit, push
# 5. CI green on main; 6. tag, push tag, CI green on tag; 7. release; 8. public, describe, pin
# 9. fresh-clone check; 10. resume bullets
```

## 8. Test by hand

1. Logged-out browser: profile shows `tcpkv` pinned first with description; README badge
   green; the Actions tab shows the tag run green; the Releases sidebar shows `v1.0.0`.
2. Click every link in the README (LICENSE, PROTOCOL.md, badges). None 404s.
3. Fresh-clone check (section 5.3) passes.
4. Read the README aloud. Every sentence is something you can defend for two minutes.
5. Give the 2-minute pitch from `INTERVIEW_TALKING_POINTS.md` to someone (or a recording)
   with the README open and no notes. Time it.
6. Ask a friend who programs to clone and run it from the README alone. Where they hesitate,
   the README is wrong.

## 9. Commit

```bash
git add -A
git commit -m "Release 1.0.0: README with architecture and benchmarks, protocol doc, required format check

- README: badges, demo, architecture diagram, protocol table, build/sanitizer/Valgrind
  commands, benchmark table with machine description, what I would do next
- docs/PROTOCOL.md checked against the parser and tests
- CMake project version 1.0.0
- CI: clang-format job is now required"
git push
```

(The formatting commit from section 3.4 comes before this one, on its own.)

## 10. What you learned, and what to say in an interview

* **A portfolio project is evidence, not code.** Public, pinned, green, documented,
  reproducible, honestly labelled numbers.
* **Benchmarks need conditions.** Machine, build, pinning, warm-up, repetitions, what the
  latency includes. Without them a number is noise.
* **Closed-loop vs open-loop load**, and why your p99 under closed loop is optimistic.
* **Annotated tags and immutable releases.** Fix forward with a patch version.
* **Resume claims are a contract.** Every one maps to a file and a test; the ones that do
  not exist yet are "what I would do next".

Interview sentence: "It is public at github.com/khanshaki-r/tcpkv, tagged 1.0.0. On a 4-vCPU
WSL2 VM with the server and load generator pinned to separate cores, it does about [N]
thousand operations a second with sixteen clients at a GET p99 of [X] microseconds, and the
load generator checks every read, so that is correct operations per second. Server-side
execution is [Y] microseconds at p99, so nearly all the latency is the network stack and
scheduling, not the store. With 64 clients on 8 threads you can see the thread-per-connection
limit in the tail, which is why epoll is the first item in my next-steps list."
