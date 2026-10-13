# 10. Capstone Projects: build tcpkv (and minidb) so the resume is true

**Time:** tcpkv ~60 hours over 5 weeks; minidb ~50 hours over 4 weeks (optional)
**Prerequisites:** C at a professional level (you have it). Modern C++, concurrency, Linux
toolchain and networking are studied *in parallel*; every step says which modules to read first.

## 1. What this course is

The other nine courses teach you things. This one makes you ship something.

Your resume lists this project:

> **tcpkv — Multithreaded TCP Key-Value Server.** C++17, Linux, CMake, GoogleTest.
> - Concurrent TCP server with a fixed thread pool and a text protocol over an in-memory store
>   guarded by shared_mutex; RAII socket and file-descriptor wrappers and move-only connection handles.
> - Builds clean under -Wall -Wextra -Werror with CMake; GoogleTest suite for protocol parsing
>   and store operations; Valgrind-clean under concurrent load.

Today (2026-10-03) that project does not exist. Every claim in those two bullets is checkable
in thirty seconds by anyone with the GitHub link. So the project must exist, be public, and be
exactly what the bullets say, before the resume leaves your hands.

The second project, **minidb**, is optional. It is a durable key-value storage engine with a
write-ahead log and a B+tree index. It exists so that when an interviewer asks about WAL,
pages, crash recovery or B-trees (all things in 07_Databases_Internals), you can answer from
code you wrote rather than from a textbook. Do it only if tcpkv is finished and tagged.

## 2. The one rule

**The repository is public at github.com/khanshaki-r/tcpkv, pinned on your profile, with a
green CI badge, before you send the resume to anyone.**

Not "almost done". Not "private until I clean it up". A recruiter who clicks a dead link, or a
link to a repo whose Actions tab is red, forms an opinion in two seconds and you never hear
about it. A senior reviewer who opens the repo and finds `// TODO` in the server loop forms the
same opinion more slowly. The definition of done below is the bar.

## 3. Why these projects, specifically

* **They match the job.** Your background is a multithreaded RPC-over-TCP application server in
  C at a banking vendor. tcpkv is that system reduced to its skeleton, rewritten in modern C++
  with the tooling a Linux shop expects (CMake, GoogleTest, sanitizers, CI). An interviewer can
  draw a straight line from your past work to this code to their job.
* **They are small enough to finish and large enough to be judged.** About 3,500 lines
  including tests. Every design decision (thread pool vs epoll, shared_mutex vs sharded locks,
  text protocol vs binary) is one you can defend in an interview because you made it.
* **They force the courses to stick.** RAII, move semantics, `std::shared_mutex`, condition
  variables, `poll()`, `EINTR`, partial writes, sanitizers, Valgrind, CMake presets, GitHub
  Actions: each shows up here as a thing you must get right, not a thing you read about.

## 4. Schedule (12 weeks from Tuesday 2026-10-06, weeks run Tuesday to Monday)

This matches section 4 of `../00_START_HERE.md`, the master schedule.

Each tcpkv step is designed for 5 to 8 hours. Two steps a week alongside the other courses.

| Week | Dates             | Work                                                            | Checkpoint |
|-----:|-------------------|-----------------------------------------------------------------|------------|
|  1   | Oct 06 – Oct 12   | tcpkv step 01 (skeleton, CMake, CI) and 02 (RAII fd and socket) | repo exists, CI green on a hello test, fd tests pass |
|  2   | Oct 13 – Oct 19   | step 03 (protocol parser) and 04 (store with shared_mutex)      | 40+ unit tests pass under ASan |
|  3   | Oct 20 – Oct 26   | step 05 (blocking server, test with nc) and 06 (thread pool, move-only Connection, graceful shutdown) | integration tests pass; Ctrl-C shuts down cleanly |
|  4   | Oct 27 – Nov 02   | step 07 (client library and CLI) and 08 (load generator; ASan, TSan, Valgrind clean) | `valgrind_check.sh` passes; TSan job green |
|  5   | Nov 03 – Nov 09   | step 09 (metrics, logging, KEYS prefix, TTL) and 10 (README, benchmark table, badges, tag v1.0.0, pin, resume bullets) | **v1.0.0 tagged, repo public and pinned, resume sent from Nov 10** |
|  6–9 | Nov 10 – Dec 07   | minidb steps 1 to 8 (optional; two steps a week)                | crash test passes; tag v0.1.0 |
| 10–12| Dec 08 – Dec 28   | interview preparation, buffer for slips                         | |

If you slip a week on tcpkv, take it from minidb, never from tcpkv's step 08 (sanitizer and
Valgrind cleanliness) or step 10 (public, documented, tagged). Those two steps are the resume.

## 5. How to work through a step

1. Read the listed prerequisite modules first. The step assumes them.
2. Read the whole step before typing. Understand what the files are for.
3. Type the code yourself. Do not copy the files from `tcpkv/code/` into your repo in one go.
   You will be asked in interviews why a line exists; typing it is how you find out.
   Use `tcpkv/code/` as the answer key when something will not compile or a test fails.
4. Build, run the tests, run the manual check, commit with the given message.
5. Write down, in your own words, the "what to say in an interview" paragraph. If you cannot,
   you are not done with the step.

## 6. Definition of done

### tcpkv (required)

- [ ] Public repository `github.com/khanshaki-r/tcpkv`, pinned on your GitHub profile.
- [ ] `cmake --preset release && cmake --build --preset release` succeeds with zero warnings
      (the build uses `-Wall -Wextra -Wpedantic -Wshadow -Werror`).
- [ ] `ctest --preset debug` passes: fd, protocol, store, thread pool and integration suites.
- [ ] `ctest --preset asan`, `ctest --preset tsan`, `ctest --preset ubsan` pass.
- [ ] `bash tools/valgrind_check.sh build/relwithdebinfo` reports "valgrind: clean" while
      `load_gen` runs 8 clients for 5 seconds.
- [ ] GitHub Actions: debug, release, asan, tsan, ubsan and valgrind jobs green on `main`;
      badge in the README.
- [ ] README with architecture diagram, protocol table, build instructions, a benchmark table
      filled with *your* numbers and the machine they came from, and "what I would do next".
- [ ] Tag `v1.0.0` with release notes.
- [ ] The resume bullets describe exactly what the repo does (see step 10). Nothing more.
- [ ] You can give the 2-minute and the 15-minute walkthrough in `tcpkv/INTERVIEW_TALKING_POINTS.md`
      without notes.

### minidb (optional)

- [ ] Public repository `github.com/khanshaki-r/minidb`.
- [ ] Single-file storage with 4 KiB pages, page cache, B+tree index, WAL with LSNs and
      checkpoints, recovery on open.
- [ ] A crash test that kills the process mid-write (SIGKILL) and verifies on reopen that
      every acknowledged write is present and no unacknowledged write is visible.
- [ ] Tests, sanitizers and CI as for tcpkv. Tag `v0.1.0`.
- [ ] You can explain WAL-before-data, LSNs, checkpoints and B+tree splits from your own code.

## 7. Files in this course

```
README.md                          this file
tcpkv/00_spec.md                   the complete specification: protocol, architecture, NFRs, testing, layout
tcpkv/01_step_project_skeleton_and_cmake.md
tcpkv/02_step_raii_fd_and_socket.md
tcpkv/03_step_protocol_parser.md
tcpkv/04_step_store_with_shared_mutex.md
tcpkv/05_step_blocking_single_threaded_server.md
tcpkv/06_step_thread_pool_connection_and_shutdown.md
tcpkv/07_step_client_library_and_cli.md
tcpkv/08_step_sanitizers_and_load_generator.md
tcpkv/09_step_metrics_logging_and_extras.md
tcpkv/10_step_polish_release_and_resume.md
tcpkv/INTERVIEW_TALKING_POINTS.md  2-minute and 15-minute pitches, design decisions, likely questions
tcpkv/code/                        the complete final source tree, as the repo should look at v1.0.0
minidb/00_spec.md                  specification for the second project
minidb/01_build_plan.md            8 steps with key classes written out
minidb/reference_code/             answer key: the minidb library and its 21 tests (CLI and bench are yours to write)
```

## 8. Resume lines this course backs up

> "Concurrent TCP server with a fixed thread pool" — `ThreadPool` (step 06), `Server::run` (step 06)
> "text protocol" — `Parser` state machine (step 03), `docs/PROTOCOL.md` (step 10)
> "in-memory store guarded by shared_mutex" — `Store` (steps 04, 09)
> "RAII socket and file-descriptor wrappers" — `FileDescriptor`, `Socket` (step 02)
> "move-only connection handles" — `Connection`, `MoveOnlyFunction` (step 06)
> "Builds clean under -Wall -Wextra -Werror with CMake" — `CMakeLists.txt` (step 01), CI (steps 01, 08)
> "GoogleTest suite for protocol parsing and store operations" — `tests/` (steps 02 to 06, 09)
> "Valgrind-clean under concurrent load" — `tools/valgrind_check.sh`, `load_gen` (step 08)
