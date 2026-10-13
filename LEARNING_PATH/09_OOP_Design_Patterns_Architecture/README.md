# 09. OOP, Design Patterns and Software Architecture in Modern C++

## What this course covers

You already design software. You do it in C: structs that hold state, functions that act on
them, function-pointer tables when you need to swap behaviour, and a head full of rules about
who `free()`s what. This course teaches the same job in C++17, with names for the things you
already do and tools you do not yet have.

Three layers, in order:

1. **OOP as a tool, not a religion.** Classes, invariants, interfaces, virtual dispatch. What
   each buys you, what each costs (in bytes, cycles, compile time and readability), and when a
   plain function or a template is the better design.
2. **Design patterns, each shown three ways.** The classic class-hierarchy form, the modern
   C++17 form (`std::function`, `std::variant`, templates, value semantics), and the C form you
   already know (a `struct` of function pointers, a `switch` on an enum, a `void*` context).
   Patterns click when you see that you have been writing them for 2.5 years.
3. **Architecture.** Ownership design, layered server codebases, API design, refactoring
   legacy C business logic into modules, and the low-level-design (LLD) interview round where
   you are asked to design a parking lot or a rate limiter on a whiteboard.

Everything targets `g++ -std=c++17 -Wall -Wextra -Werror -pthread` on WSL Ubuntu 24.04.

## Why it is on the resume

Your resume says you re-engineered legacy C business logic into a modular layer, that you
understand "OOP, design patterns, memory and ownership design", and that you maintain a
client-server application server. Interviewers for senior C++ roles will probe all three:

- "Why did you choose a virtual interface here instead of a template?"
- "Who owns the connection object? What happens if the client disconnects mid-request?"
- "Design an LRU cache with TTL. Now make it thread-safe. Now make it testable."

This course is the preparation for those conversations and for the LLD round, which many
companies run as a separate 45 to 60 minute interview.

## Prerequisites

- C at a professional level (you have it).
- `02_Modern_CPP` through classes, constructors, destructors, references and
  `const`. Modules here assume you can read a class definition and a range-for loop.
- `02_Modern_CPP` module 05 (at least `unique_ptr` and `shared_ptr` basics). Module 03
  of this course builds on it.
- `04_Concurrency` for Module 07 (mutex, condition_variable, threads). You can read Module 07
  later if you are doing the courses in parallel.

## Modules

| #  | File | Topic | Est. hours |
|----|------|-------|-----------|
| 01 | `01_oop_for_a_c_programmer.md` | From `struct` + function pointers to classes with virtual functions; encapsulation and invariants; what OOP costs | 2.5 |
| 02 | `02_solid_in_plain_words.md` | The five SOLID principles with bad/good C++; dependency injection; mapping onto a server codebase | 2.5 |
| 03 | `03_ownership_and_object_lifetime_design.md` | Who owns what; `unique_ptr` trees; shared ownership as a smell; handles, pools, arenas; ownership of connections and sessions | 3 |
| 04 | `04_creational_patterns.md` | Factory, abstract factory, builder, singleton (and why to avoid it), prototype, object pool | 2 |
| 05 | `05_structural_patterns.md` | Adapter over C APIs, facade, decorator around a Store, composite, caching proxy, bridge and pimpl | 2.5 |
| 06 | `06_behavioural_patterns.md` | Strategy, observer, command, state machine, template method vs NVI, visitor via `std::variant`, chain of responsibility, iterator | 3 |
| 07 | `07_concurrency_patterns.md` | Active object, monitor, producer-consumer, thread pool, reactor vs proactor, RCU snapshots, immutable messages, actors | 3 |
| 08 | `08_modern_cpp_alternatives_to_classic_patterns.md` | Type erasure, CRTP, policy-based design, tag dispatch, variant state machines, pipelines; templates vs virtual cost model | 3 |
| 09 | `09_designing_a_server_codebase.md` | Layered architecture, dependency direction, boundaries, config, logging, errors; full architectures for tcpkv and a loan-origination server; refactoring legacy C | 3.5 |
| 10 | `10_api_design_and_code_quality.md` | API design rules, strong types, `[[nodiscard]]`, testable seams, fakes vs mocks, code review checklist, safe refactoring | 2.5 |
| 11 | `11_low_level_design_interview_walkthroughs.md` | The LLD method plus seven full walkthroughs with ASCII class diagrams and code | 5 |
| 12 | `12_uml_and_communicating_design.md` | Just enough UML; presenting a design in 10 minutes; design document template; trade-off tables | 1.5 |

Total: roughly 34 hours. At 6 to 8 hours a week alongside other courses, this is 5 weeks.

Suggested order if time is short: 01, 03, 06, 08, 09, 11. Those six cover the interview
surface. Read 02, 04, 05, 07, 10, 12 as you have time; they fill in vocabulary and polish.

## Code

`code/` contains 39 complete programs, one per pattern or walkthrough, named `mNN_name.cpp`
where `NN` is the module number. Build any of them with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread -o out code/m05_decorator_store.cpp && ./out
```

Every file is self-contained (no external libraries). Where a real program would use a C
library like libpq or Berkeley DB, the file includes a small fake C API so the pattern is
visible without a dependency.

## You are done when you can...

- [ ] Rewrite a `struct` of function pointers as a class hierarchy, and explain the vtable
      layout and the cost of a virtual call in cycles and in lost inlining.
- [ ] State each SOLID principle in one sentence and give a C++ example of violating it.
- [ ] Draw the ownership diagram of a server (listener, connections, sessions, requests,
      storage) with only `unique_ptr`, references and raw observer pointers, and defend why
      `shared_ptr` is not needed.
- [ ] For any of the 20 classic patterns, show the class version, the `std::function` or
      `std::variant` version, and the C version, and say which one you would pick and why.
- [ ] Explain reactor vs proactor, active object vs monitor object, and when a thread pool
      should be bounded.
- [ ] Explain type erasure well enough to hand-roll a `function_ref` or a small `any_shape`.
- [ ] Lay out a layered server codebase on paper: directories, headers, dependency direction,
      what is an interface and what is concrete, where tests plug in.
- [ ] Describe the strangler-fig approach to replacing legacy C business logic, using your
      stored-procedure re-engineering project as the example.
- [ ] Walk through an LLD interview for an LRU cache with TTL, a rate limiter, a logger, a
      parking lot, a transactional key-value store, a workflow engine and a job scheduler,
      within 45 minutes each, with a class diagram and working C++.
- [ ] Write a one-page design document with a trade-off table and present it in 10 minutes.

## Resume lines this course backs up

- "OOP, design patterns, memory & ownership design"
- "client-server architecture"
- "Re-engineered legacy C business logic into a modular ... layer"
- The low-level design (LLD) interview round that most senior C++ positions include.

## Files in this course

```
09_OOP_Design_Patterns_Architecture/
  README.md
  01_oop_for_a_c_programmer.md
  02_solid_in_plain_words.md
  03_ownership_and_object_lifetime_design.md
  04_creational_patterns.md
  05_structural_patterns.md
  06_behavioural_patterns.md
  07_concurrency_patterns.md
  08_modern_cpp_alternatives_to_classic_patterns.md
  09_designing_a_server_codebase.md
  10_api_design_and_code_quality.md
  11_low_level_design_interview_walkthroughs.md
  12_uml_and_communicating_design.md
  EXERCISES.md
  INTERVIEW_QA.md
  code/   (39 .cpp files, see each module's section 3)
```
