# START HERE — The 12-week plan to a C, C++ systems job by December 2026

**Written for:** Shakir, 2.5 years of production C, moving to a senior-level C, C++ systems role.
**Dates:** Week 1 starts Tuesday 2026-10-06. Weeks run Tuesday to Monday. Week 12 ends Monday 2026-12-28.
**Rule of the plan:** every line on the resume must be backed by something you have built,
debugged or can explain at whiteboard depth. Nothing on the resume is allowed to stay a "keyword".

---

## 1. How this folder is organised

```
LEARNING_PATH/
  00_START_HERE.md                      <- this file: the plan and the schedule
  _STYLE_GUIDE.md                       <- how every course is written (you can ignore it)
  01_Environment_Setup/                 <- 1 evening. Do this first.
  12_C_Programming/                     <- C to senior depth: pointers, UB, POSIX (16 modules)
  02_Modern_CPP/                        <- C++17 for a C programmer          (14 modules)
  03_Data_Structures_Algorithms/        <- DSA to interview level            (16 modules)
  04_Concurrency/                       <- threads, locks, atomics, pools    (13 modules)
  05_Memory_Debugging_RCA/              <- gdb, WinDbg, cores, Valgrind, RCA (13 modules)
  06_Linux_Toolchain_DevOps/            <- Linux, GCC, CMake, Git, Docker, CI(12 modules)
  07_Databases_Internals/               <- PostgreSQL, YugabyteDB, BDB, Redis(14 modules)
  08_Networking_Distributed/            <- sockets, TCP, epoll, RPC, Raft    (14 modules)
  09_OOP_Design_Patterns_Architecture/  <- OOP, patterns, LLD interviews     (12 modules)
  10_Capstone_Projects/                 <- build tcpkv step by step (+ minidb)
  11_Interview_Prep_Job_Search/         <- question banks, stories, applications, negotiation
                                           (private: kept out of the public site and repo)
```

Every course has the same shape:
- `README.md` — syllabus, hours, "you are done when you can..." checklist.
- `01_*.md ... NN_*.md` — modules. Each module: plain words -> real thing -> code -> under the hood ->
  in production -> exercises -> interview questions -> recap. Study one module per sitting.
- `code/` — complete programs. Build them on WSL with `g++ -std=c++17 -Wall -Wextra -Werror -pthread`.
- `EXERCISES.md` and `INTERVIEW_QA.md` — all exercises and interview questions in one place.

## 2. How to study (read this once, then trust it)

1. **Read the module, then type the code yourself.** Do not copy-paste. Typing is where it sticks.
2. **Break the code on purpose.** Remove a `std::move`, drop a lock, overflow a buffer. Watch what
   gdb / ASan / TSan says. This is how production debugging skill is built.
3. **Say the recap out loud** at the end of every module as if an interviewer asked. If you cannot
   say it without looking, you are not done.
4. **One notebook file:** keep `MY_NOTES.md` in this folder. After every module write 5 lines:
   what surprised me, what I would say in an interview, what I still do not get.
5. **Parallel tracks, not serial.** You study 3 to 4 courses at once (see the schedule). Each gets a
   fixed slot. Do not "finish C++ first and then start concurrency" — the project needs both at once.
6. **The project is the spine.** Every week the tcpkv repo must have commits. The courses exist to
   make the project good and to make you able to defend every line of it.

## 3. Daily schedule (full-time job, about 3 h weekdays, 8 h weekend days)

| Slot | Weekday | Weekend |
|---|---|---|
| 06:00 - 07:00 | DSA: 1 problem fully (think, code, test, write the pattern down) | DSA: 2 problems + 1 review |
| Lunch (20 min) | Read one INTERVIEW_QA.md section, say answers in your head | — |
| 20:00 - 21:00 | Course module (the week's "primary" course) | Course modules (primary + secondary), 3 h |
| 21:00 - 22:00 | Project: tcpkv step of the week, commit | Project: 3 h, commit, CI green |
| — | — | Sunday 18:00: 1 h weekly review (11_Interview_Prep_Job_Search/12_weekly_checklists.md) |

Weekly total: about 31 hours. Over 12 weeks: about 370 hours. That is enough if you do not skip days.
If you miss a day, do not "catch up" by doubling; just continue. Consistency beats intensity.

## 4. The 12-week schedule

Legend: **P** = primary course that week (most hours), S = secondary, bg = background (1 to 2 h/week).
**C track** = two modules of 12_C_Programming a week, in the weekday lunch slot or on the weekend.
You already write C at work, so most of it is consolidation; it is also the revision source before
any C interview round.
Applications start **week 5** with the tcpkv repo public and green. Not earlier (resume would 404),
not later (December offers need November interviews).

| Week | Dates | DSA (daily) | Courses | Project (10_Capstone_Projects) | Job search (11_Interview_Prep) |
|---|---|---|---|---|---|
| 1 | Oct 6 - 12 | 03: modules 01-03 (complexity, arrays, strings) | **P:** 01_Environment_Setup then 02_Modern_CPP 01-04. **S:** 06_Linux_Toolchain 01-04 (Linux, build, flags, CMake). **C track:** 12_C_Programming 01-02 | tcpkv steps 01-02: skeleton, CMake, CI, RAII fd/socket wrappers | Read 11/01 (how interviews work). Set up tracking sheet. |
| 2 | Oct 13 - 19 | 03: 04-06 (hashing, lists, stacks/queues) | **P:** 02_Modern_CPP 05-08 (smart ptrs, STL, algorithms, templates). **S:** 04_Concurrency 01-03. bg: 06/05 GoogleTest, 06/06 Git. **C track:** 12_C_Programming 03-04 | tcpkv steps 03-04: protocol parser + tests, shared_mutex store | Write the 2-minute intro (11/02). |
| 3 | Oct 20 - 26 | 03: 07-09 (recursion, sorting, binary search) | **P:** 04_Concurrency 04-07 (shared_mutex, condvars, deadlocks, atomics). **S:** 08_Networking 01-04 (TCP, sockets, framing). bg: 06/11 Linux systems programming (signals). **C track:** 12_C_Programming 05-06 | tcpkv steps 05-06: blocking server, thread pool, Connection, graceful shutdown | Draft all STAR stories (11/02). Look up real numbers at work for them. |
| 4 | Oct 27 - Nov 2 | 03: 10-11 (trees, B-tree) | **P:** 04_Concurrency 08-13 (lock-free, thread pools, false sharing, patterns, debugging, Win32 reference). **S:** 08_Networking 05-06 (thread pool server, epoll). bg: 02_Modern_CPP 09-10. **C track:** 12_C_Programming 07-08 | tcpkv steps 07-08: client library + CLI, Valgrind/ASan/TSan clean under load | Resume final pass (11/08). LinkedIn + GitHub profile. |
| 5 | Nov 3 - 9 | 03: 12-13 (heaps, graphs) | **P:** 05_Memory_Debugging_RCA 01-06 (memory layout, malloc, gdb, cores). **S:** 02_Modern_CPP 11-14. bg: 06/07-10 profiling tools, Docker, CI, scripting. **C track:** 12_C_Programming 09-10 | tcpkv steps 09-10: metrics, logging, README, v1.0.0, **repo public and pinned by Monday Nov 9** | **Start applying: 10 to 15 targeted applications.** Recruiter outreach (11/09). |
| 6 | Nov 10 - 16 | 03: 14 (DP) + mixed review | **P:** 07_Databases_Internals 01-06 (engine, pages, B-tree indexes, WAL, MVCC, EXPLAIN). **S:** 05_Memory_Debugging_RCA 07-13 (WinDbg, sanitizers, RCA writing). bg: 06/12 Windows toolchain reference. **C track:** 12_C_Programming 11-12 | minidb step 1-2 (page file, page cache) or polish tcpkv issues | 10 to 15 applications. First mocks: DSA + C++ (11/11). |
| 7 | Nov 17 - 23 | 03: 15-16 (tries, bits, playbook) + timed sets | **P:** 07_Databases_Internals 07-11 (SQL, PL/pgSQL, schema, Berkeley DB, YugabyteDB). **S:** 09_OOP_Design 01-05. **C track:** 12_C_Programming 13-14 | minidb steps 3-4 (B+tree, WAL) | Interviews likely begin. Mocks: concurrency + database. |
| 8 | Nov 24 - 30 | Timed mock sets (2 x 45 min on weekend) | **P:** 07_Databases_Internals 12-14 (migration methodology, caching/Redis, operations). **S:** 09_OOP_Design 06-11 (patterns, server architecture, LLD walkthroughs). **C track:** 12_C_Programming 15-16 | minidb steps 5-6 (recovery, crash test) | Applications continue. System design lite mocks. |
| 9 | Dec 1 - 7 | Review weak patterns | **P:** 08_Networking 07-14 (RPC, HTTP, reliability, distributed, Raft, sharding, HA, system design). **S:** 09_OOP_Design 12 | minidb steps 7-8 or stop here and polish READMEs | Interview loops. Negotiation reading (11/10). |
| 10 | Dec 8 - 14 | Review | Revisit every course INTERVIEW_QA.md; fill gaps found in real interviews | Bug fixes from interview feedback | Interview loops. Keep 2 to 3 processes alive in parallel. |
| 11 | Dec 15 - 21 | Review | Gap-filling only | — | Offers, negotiation, closing. |
| 12 | Dec 22 - 28 | Light | — | — | Accept. Resignation plan. First-90-days plan. |

**This table is the single schedule for the whole curriculum.** The week-by-week tables in
`11_Interview_Prep_Job_Search` add interview-practice and application detail on top of it. Where
they mention the project, the step numbers here and in `10_Capstone_Projects` win.

**The project runs ahead of the theory on purpose.** Each tcpkv step guide starts with a
"Read first" list. When it names a module scheduled for a later week, read only that module's
sections 1 and 2 now (plain words and the real thing, about 30 minutes) and do the full module in
its scheduled week. Example: step 08 in week 4 needs 05_Memory_Debugging_RCA module 08 on
sanitizers, which is scheduled for week 6.

**Starting late?** If you begin after 2026-10-06, do not move December. Merge the missed week into
the next ones using the cut list below, and protect week 5: tcpkv public and applications started
by Monday 2026-11-09.

### If you fall behind, cut in this order
1. minidb (second project) — drop entirely. tcpkv alone is enough.
2. 08_Networking modules 11-13 (Raft, sharding, HA) — read the recaps only.
3. 09_OOP_Design modules 07-08, 12 — skip.
4. 03 DSA modules 15 (tries/bits) — skip.
Never cut: tcpkv, 02_Modern_CPP, 04_Concurrency, 05 modules 01-06, 07 modules 03-06, daily DSA.

## 5. Which course backs which resume line

| Resume line | Course that makes it true |
|---|---|
| C++17, STL, RAII, smart pointers, templates, OOP, memory & ownership | 02_Modern_CPP, 09_OOP_Design |
| C (language), multithreaded server-side C, memory corruption investigation | 12_C_Programming |
| Data structures & algorithms | 03_Data_Structures_Algorithms |
| Multithreading & concurrency; thread synchronisation layer; pooled request model | 04_Concurrency |
| Crash dump & core dump analysis, memory corruption, leaks, RCA | 05_Memory_Debugging_RCA |
| Linux, GCC, CMake, GDB, Git, Docker, CI/CD | 06_Linux_Toolchain_DevOps |
| PostgreSQL, YugabyteDB, Berkeley DB, Redis, internals, indexing, transactions, PL/pgSQL | 07_Databases_Internals |
| Sockets, TCP/IP, RPC, HTTP, distributed SQL, sharding, replication, HA | 08_Networking_Distributed |
| tcpkv project (every word of both bullets) | 10_Capstone_Projects |
| Performance optimisation | 02/12, 04/10, 07/06, 07/14 |

## 6. Definition of "senior-level understanding" used throughout

For each topic you should be able to do all four:
1. **Explain it to a junior in plain words** (the "In plain words" sections).
2. **Explain what happens under the hood** (compiler, OS, kernel, storage engine).
3. **Write it from scratch** (the code/ examples and the project).
4. **Debug it when it breaks in production** (the "In production" sections and course 05).

Interviewers for 2 to 4 years of experience test 1 and 3 always, 2 often, 4 for systems roles.
This plan targets all four.

## 7. Before you start (tonight)

1. Open `01_Environment_Setup/README.md` and do the two modules. About 2 hours.
2. Create `MY_NOTES.md` here.
3. Create a private GitHub repo `tcpkv` now (make it public at week 5 when it is green).
4. Put the daily schedule in your phone calendar with alarms.
5. Read `11_Interview_Prep_Job_Search/README.md` once so you know where this is going.
