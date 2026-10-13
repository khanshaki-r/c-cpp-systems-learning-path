# C, C++ Systems Engineering Learning Path

**Read it as a website: <https://khanshaki-r.github.io/c-cpp-systems-learning-path/>**

A 12-week, self-paced curriculum that takes a working C programmer to production-depth knowledge
of C, modern C++17, concurrency, debugging, Linux tooling, database internals and distributed
systems. It is built around one real project, **tcpkv**, a multithreaded TCP key-value server
that is sanitizer-clean and tested in CI.

Every module follows the same eight sections: plain words, the real mechanism, complete code,
under the hood, in production, exercises, interview questions, recap.

## Courses

| Course | What it covers |
|---|---|
| [00 Start here](LEARNING_PATH/00_START_HERE.md) | The 12-week schedule that ties everything together |
| [01 Environment Setup](LEARNING_PATH/01_Environment_Setup/) | WSL, GCC 13, CMake, gdb, Valgrind, sanitizers |
| [12 C Programming](LEARNING_PATH/12_C_Programming/) | Types, pointers, memory, strings, layout, UB, allocators, POSIX, pthreads, legacy code |
| [02 Modern C++](LEARNING_PATH/02_Modern_CPP/) | RAII, move semantics, smart pointers, STL internals, templates, vtables |
| [03 Data Structures & Algorithms](LEARNING_PATH/03_Data_Structures_Algorithms/) | Complexity to B+trees, graphs, DP, interview playbook |
| [04 Concurrency](LEARNING_PATH/04_Concurrency/) | Threads, locks, condition variables, atomics, memory model, thread pools |
| [05 Memory, Debugging & RCA](LEARNING_PATH/05_Memory_Debugging_RCA/) | Process memory, malloc, gdb, core dumps, WinDbg, sanitizers, RCA writing |
| [06 Linux, Toolchain & DevOps](LEARNING_PATH/06_Linux_Toolchain_DevOps/) | Linker, GCC flags, CMake, GoogleTest, Git, Docker, CI, system calls |
| [07 Database Internals](LEARNING_PATH/07_Databases_Internals/) | Pages, B-trees, WAL, MVCC, EXPLAIN, PL/pgSQL, Berkeley DB, YugabyteDB, Redis |
| [08 Networking & Distributed](LEARNING_PATH/08_Networking_Distributed/) | TCP, sockets, epoll, RPC, HTTP, Raft, sharding, system design |
| [09 OOP, Patterns & Architecture](LEARNING_PATH/09_OOP_Design_Patterns_Architecture/) | SOLID, ownership, patterns in modern C++, LLD walkthroughs |
| [10 Capstone Projects](LEARNING_PATH/10_Capstone_Projects/) | tcpkv in 10 steps (answer key included), optional minidb storage engine |

## Build the code

Everything targets Ubuntu 24.04 (native or WSL) with GCC 13:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread file.cpp -o prog   # C++ examples
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror file.c -o prog     # C examples
bash site_tools/check_code.sh                                     # compile-check all of them
```

## Build the site locally

```bash
python -m venv .venv && . .venv/bin/activate
pip install -r site_tools/requirements.txt
python site_tools/build_config.py   # regenerates mkdocs.yml navigation from the folders
mkdocs serve                        # http://127.0.0.1:8000
mkdocs gh-deploy --force            # publish to GitHub Pages (gh-pages branch)
```

CI workflows that publish automatically and compile-check every example are ready in
`site_tools/ci/`; see the README there to enable them.

Add a module by dropping a numbered `NN_topic.md` file into a course folder; the navigation
picks it up on the next build. Edit `site_tools/build_config.py`, not `mkdocs.yml`.

## Repository layout

```
LEARNING_PATH/        all course content (the site's docs folder)
  NN_Course/          README.md, NN_module.md files, code/, EXERCISES.md, INTERVIEW_QA.md
site_tools/           site config generator, link hook, code-check script, pinned requirements
site_tools/ci/        CI workflows, ready to enable (see its README)
*.cpp (root)          my own DSA practice files
```
