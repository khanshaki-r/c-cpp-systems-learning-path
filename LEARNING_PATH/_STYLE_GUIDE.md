# Style guide for every course in LEARNING_PATH (read before writing any module)

## Who the learner is
- Name: Shakir. 2.5 years of production **C** at a core-banking vendor (Extrieve Technologies).
  Windows / MSVC / Win32 background. Wrote a Berkeley DB -> YugabyteDB migration engine in C,
  maintains a multithreaded RPC-over-TCP application server, debugs crash dumps, writes PL/pgSQL.
- Knows C well. Knows **modern C++ only at beginner level** (currently doing arrays/binary search).
- Goal: a **senior-level, production-grade** understanding of every item on his resume, and a
  new job **by December 2026**. Today is 2026-10-03. ~12 weeks. Studies several courses in parallel.
- Machine: Windows 11. MinGW GCC 6.3 (too old for C++17). **WSL Ubuntu 24.04 is installed** ->
  ALL Linux/C++ code in the courses targets `g++ -std=c++17` on WSL Ubuntu 24.04 (GCC 13).
  Windows-specific topics (Win32 threads, WinDbg, MSVC) are covered where the resume lists them.

## Voice and language
- **Easy language first, depth second.** Every concept is introduced with a one-paragraph
  plain-English explanation and, where it helps, a real-world analogy. Then go deep.
- Short sentences. No fluff, no motivational filler. Assume an intelligent reader who is new to the topic.
- Prefer "you" and concrete examples over abstract statements.
- When a term is first used, define it in one line.
- Depth target: what a **senior engineer** would be expected to know and be able to explain in an
  interview and apply in production. Include under-the-hood mechanics (what the compiler/OS/DB does).
- Be honest about trade-offs. Say "it depends" only when followed by what it depends on.

## File layout for a course folder `NN_Course_Name/`
- `README.md` — syllabus: what the course covers, why it is on the resume, prerequisites,
  module list with estimated hours, a "You are done when you can..." checklist, and a
  "Resume line this course backs up" section quoting the exact resume phrases.
- `01_topic.md`, `02_topic.md`, ... — modules in learning order. 8 to 14 modules per course.
- `code/` — complete, compilable example programs referenced by the modules
  (one file per example, named `mNN_short_name.cpp` / `.c` / `.sql` / `.sh`). Every C++ file must
  compile with `g++ -std=c++17 -Wall -Wextra -Werror -pthread` on GCC 13 (do not actually need to
  run them, but write them as if they will be).
- `EXERCISES.md` — one place listing every exercise across modules with difficulty (easy/medium/hard)
  and a hint for each.
- `INTERVIEW_QA.md` — 25 to 40 interview questions for this topic with crisp model answers
  (2 to 6 sentences each), ordered from basic to senior-level.

## Fixed section order inside every module file
```
# <Module number>. <Title>

**Time:** ~X hours   **Prerequisites:** <modules or "none">

## 1. In plain words
<Easy-language explanation + analogy. 1 to 3 paragraphs.>

## 2. The real thing
<Precise technical explanation. Sub-headings allowed. Diagrams in ASCII where useful.>

## 3. Code
<Complete, runnable examples with line comments explaining WHY, not what.
 Reference files in code/ by name and also inline the key parts.>

## 4. Under the hood (senior-level)
<What actually happens in the compiler / OS / kernel / database engine. Memory layout, syscalls,
 cache lines, locks, pages, WAL, etc. This is the section that separates mid from senior.>

## 5. In production
<How this shows up in real systems. Pitfalls, war stories, how it maps to Shakir's banking-server
 work where relevant. What goes wrong at 3 AM and how you debug it.>

## 6. Try it
<3 to 6 exercises, graded easy -> hard. Each exercise states exactly what "done" looks like.>

## 7. Interview questions
<5 to 8 questions with short model answers.>

## 8. Recap
<5 to 10 bullet points you should be able to say from memory.>
```

## Rules
- Markdown only. Code in fenced blocks with language tags.
- Never write "C/C++". Write "C, C++" or name the language.
- No invented statistics, no fake quotes. If you give a number (latency, size), say where it comes from
  or label it "typical".
- Every module must be self-contained enough to study in one sitting (1 to 3 hours).
- Cross-reference other courses by folder name, e.g. "see 04_Concurrency/03_mutexes_lock_guard_unique_lock.md".
- Use UTF-8, no emoji.
