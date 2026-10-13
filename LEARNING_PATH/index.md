---
hide:
  - navigation
  - toc
---

# C, C++ Systems Engineering Learning Path

A 12-week curriculum that takes a working C programmer to production-depth knowledge of
**C, modern C++17, concurrency, debugging, Linux, databases and distributed systems**, built
around one real project: a multithreaded TCP key-value server.

Every module follows the same shape: **plain words first**, then the real mechanism, complete
code, what happens under the hood, how it fails in production, exercises, and interview questions.

[Start here: the 12-week plan](00_START_HERE.md){ .md-button .md-button--primary }
[Set up your machine](01_Environment_Setup/README.md){ .md-button }

<div class="lp-overall" data-lp-overall></div>

## The courses

<div class="grid cards" markdown>

-   :material-console: **Environment Setup**

    ---

    WSL, GCC 13, CMake, gdb, Valgrind and sanitizers working on day one.

    2 modules · <span data-lp-course="01_Environment_Setup" data-lp-total="2"></span>

    [:octicons-arrow-right-24: Open](01_Environment_Setup/README.md)

-   :material-language-c: **C Programming**

    ---

    Types, pointers, memory, strings, layout, UB, allocators, POSIX, pthreads, legacy code.

    16 modules · <span data-lp-course="12_C_Programming" data-lp-total="16"></span>

    [:octicons-arrow-right-24: Open](12_C_Programming/README.md)

-   :material-language-cpp: **Modern C++17**

    ---

    RAII, move semantics, smart pointers, STL internals, templates, vtables, UB.

    14 modules · <span data-lp-course="02_Modern_CPP" data-lp-total="14"></span>

    [:octicons-arrow-right-24: Open](02_Modern_CPP/README.md)

-   :material-graph: **Data Structures & Algorithms**

    ---

    From complexity to B+trees, graphs and DP, plus a 10-week interview problem plan.

    16 modules · <span data-lp-course="03_Data_Structures_Algorithms" data-lp-total="16"></span>

    [:octicons-arrow-right-24: Open](03_Data_Structures_Algorithms/README.md)

-   :material-call-split: **Concurrency**

    ---

    Threads, mutexes, condition variables, atomics and the memory model, thread pools, Win32.

    13 modules · <span data-lp-course="04_Concurrency" data-lp-total="13"></span>

    [:octicons-arrow-right-24: Open](04_Concurrency/README.md)

-   :material-bug: **Memory, Debugging & RCA**

    ---

    Process memory, malloc internals, gdb, core dumps, WinDbg, sanitizers, writing an RCA.

    13 modules · <span data-lp-course="05_Memory_Debugging_RCA" data-lp-total="13"></span>

    [:octicons-arrow-right-24: Open](05_Memory_Debugging_RCA/README.md)

-   :material-linux: **Linux, Toolchain & DevOps**

    ---

    The linker, GCC flags, CMake, GoogleTest, Git, Docker, CI, systems calls.

    12 modules · <span data-lp-course="06_Linux_Toolchain_DevOps" data-lp-total="12"></span>

    [:octicons-arrow-right-24: Open](06_Linux_Toolchain_DevOps/README.md)

-   :material-database: **Database Internals**

    ---

    Pages, B-trees, WAL, MVCC, EXPLAIN, PL/pgSQL, Berkeley DB, YugabyteDB, Redis.

    14 modules · <span data-lp-course="07_Databases_Internals" data-lp-total="14"></span>

    [:octicons-arrow-right-24: Open](07_Databases_Internals/README.md)

-   :material-lan: **Networking & Distributed Systems**

    ---

    TCP, sockets, epoll, RPC, HTTP, consistency, Raft, sharding, system design.

    14 modules · <span data-lp-course="08_Networking_Distributed" data-lp-total="14"></span>

    [:octicons-arrow-right-24: Open](08_Networking_Distributed/README.md)

-   :material-sitemap: **OOP, Patterns & Architecture**

    ---

    SOLID, ownership design, patterns in modern C++, server architecture, LLD walkthroughs.

    12 modules · <span data-lp-course="09_OOP_Design_Patterns_Architecture" data-lp-total="12"></span>

    [:octicons-arrow-right-24: Open](09_OOP_Design_Patterns_Architecture/README.md)

-   :material-hammer-wrench: **Capstone Projects**

    ---

    Build tcpkv step by step, sanitizer-clean and CI-green; optional minidb storage engine.

    10 steps · <span data-lp-course="10_Capstone_Projects" data-lp-total="10"></span>

    [:octicons-arrow-right-24: Open](10_Capstone_Projects/README.md)

</div>

## How to use this site

1. Follow the week-by-week table in [Start here](00_START_HERE.md). Study three or four courses
   in parallel, one module per sitting.
2. Type the code yourself. Every program is linked to GitHub and builds with
   `-Wall -Wextra -Werror` on GCC 13.
3. Press **Mark as done** at the top of a module when you can say its recap out loud without
   looking. Your progress is saved in this browser only.
4. Use the search box (press ++slash++) to find any topic across all courses.
