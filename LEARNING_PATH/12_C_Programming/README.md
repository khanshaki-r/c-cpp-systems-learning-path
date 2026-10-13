# 12. C programming: the language under everything else

## What this course covers

C from the ground up to senior and interview depth. It starts with what a C compiler and linker
actually do with your files, then rebuilds the fundamentals one at a time: integer types and the
conversion rules, pointers drawn as memory, storage duration and lifetime, strings and buffers,
struct layout, function pointers and callbacks, and the preprocessor. The second half is about
writing C that survives production: undefined behaviour and what the optimiser does with it,
error handling and cleanup, data structures and allocators written in C, files and sockets,
threads, working safely in legacy code, and a final module for C interview rounds.

Every module explains each idea in plain words first, then goes down to what the compiler, the
linker, the C library and the CPU really do, then to how the idea shows up in a multithreaded
banking server at 3 AM. Every module has complete programs in `code/` that build with
`-Wall -Wextra -Wpedantic -Werror` and check themselves with `assert`.

## Why it is on the resume

Your resume says **"Languages: C, C++17"**. For 2.5 years your C work has been maintenance:
analysing and fixing bugs in an existing multithreaded Win32 server, rather than designing C code
from an empty file. That is real and valuable experience, but it can leave gaps in the
fundamentals: you fix what is in front of you without always being able to say *why* the language
behaves that way. Interviewers for C, C++ systems roles probe exactly those fundamentals:

- "What does `-1 < 1u` evaluate to?" "Why is signed overflow undefined?" (module 02)
- "What is the difference between an array and a pointer?" (module 03)
- "What happens if `realloc` fails?" "Write `strlcpy`." (modules 04, 05)
- "Why is this struct 24 bytes?" "Is union type punning legal?" (module 06)
- "Why `do { } while (0)`?" (module 08)
- "The compiler deleted my NULL check. Why?" (module 09)

This course turns 2.5 years of fixing C into the ability to explain C, write it from scratch, and
defend every C line on the resume:

> "Languages: C, C++17"

> "multithreaded server-side C"

> "C migration engine ... arena allocator with explicit ownership contracts"

> "memory corruption & leak investigation"

## How to study it: a parallel track

This course is not studied in one block. It runs as the **C track** next to the main weekly plan
in `00_START_HERE.md`: **two modules a week, one every three or four days**, in the weekday lunch
slot or on the weekend. Most of it consolidates what you already do at work, so it moves faster
than a new subject.

| Week (00_START_HERE.md) | C track modules |
|---|---|
| 1 (Oct 6 - 12) | 01, 02 |
| 2 (Oct 13 - 19) | 03, 04 |
| 3 (Oct 20 - 26) | 05, 06 |
| 4 (Oct 27 - Nov 2) | 07, 08 |
| 5 (Nov 3 - 9) | 09, 10 |
| 6 (Nov 10 - 16) | 11, 12 |
| 7 (Nov 17 - 23) | 13, 14 |
| 8 (Nov 24 - 30) | 15, 16 |

It is also the **revision source before any C interview round**: see "Revising before a C round"
below.

## Prerequisites

- None beyond having written some C. You have far more than that.
- The WSL setup from `01_Environment_Setup` (Ubuntu 24.04, GCC 13, gdb, Valgrind). Optional:
  `sudo apt install dwarves` for `pahole` (struct layout, module 06) and `clang` for a second compiler.
- Modules list their own prerequisites. Modules 01 to 08 assume only the modules before them.

## Building the code

All code targets **C17** on WSL Ubuntu 24.04 with GCC 13. Work on a copy in the Linux file system,
not under `/mnt/d` (01_Environment_Setup explains why: file system speed and permissions):

```bash
mkdir -p ~/dev && cp -r /mnt/d/Cpp_DSA/LEARNING_PATH/12_C_Programming/code ~/dev/c12 && cd ~/dev/c12

# one program
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_pointer_basics.c -o m03_pointer_basics
./m03_pointer_basics

# every C program in the folder (-pthread is needed only by the threading modules, harmless elsewhere)
for f in m*.c; do
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -pthread "$f" -o "${f%.c}" || echo "BUILD FAILED: $f"
done

# the same with sanitizers: the best way to run the exercises
gcc -std=c17 -Wall -Wextra -g -fsanitize=address,undefined m05_string_traps.c -o m05_string_traps_asan
```

Conventions used by every file in `code/`:

- Named `mNN_short_name.c`, where `NN` is the module number.
- `main()` checks its own results with `assert` and prints a short report; exit status 0 means
  every check passed. The module text shows the real output, captured from these builds.
- The first comment gives the exact build line, and any extra flags or arguments the file needs.
- Files that **deliberately** contain undefined behaviour say so in that first comment, and give the
  build line (without `-Werror` where GCC would refuse). In modules 01 to 08 these are
  `m02_overflow.c` (one call overflows `int` on purpose), `m04_returning_buffers.c` (with
  `-DSHOW_BUG`) and `m06_unions_and_punning.c` (with `-DSHOW_ALIASING_UB`).
- Windows: the portable files also build with MSVC (`cl /std:c17 /W4 file.c`), which is a useful
  exercise for module 01's "MSVC versus GCC" section. GCC-only features (`__builtin_*`,
  `__attribute__`) are pointed out where they are used.

## Modules

| # | File | Topic | Hours |
|---|---|---|---|
| 01 | `01_how_c_really_works.md` | The abstract machine, translation units, preprocess/compile/assemble/link, declarations versus definitions, linkage, object files and symbols, C89 to C23, MSVC C versus GCC C | 2.5 |
| 02 | `02_types_integers_and_conversions.md` | Data models (LP64 versus Windows LLP64), two's complement, integer promotions, usual arithmetic conversions, signed/unsigned bugs, overflow, `size_t`, `<stdint.h>`, printf specifiers, floating point and why money is never a `double`, endianness | 3 |
| 03 | `03_pointers_from_the_ground_up.md` | Addresses, `*` and `&`, pointer arithmetic, arrays versus pointers and decay, `sizeof` traps, pointer-to-pointer, `const` placement, `void *`, NULL, 2-D arrays, `restrict` | 3 |
| 04 | `04_memory_storage_scope_and_lifetime.md` | Storage durations, scope versus lifetime, `static`/`extern`, stack frames, `malloc`/`calloc`/`realloc`/`free` contracts, the realloc-leak idiom, ownership comments, alignment, VLAs and `alloca`, an arena allocator | 3 |
| 05 | `05_strings_and_buffers.md` | NUL-terminated strings, `<string.h>` traps, `strncpy`, `snprintf`, `memcpy` versus `memmove`, `strtol` versus `atoi`, `sscanf`, `strtok_r`, UTF-8, safe helpers, buffer overflows | 3 |
| 06 | `06_structs_unions_enums_and_layout.md` | Layout and padding, `offsetof`, packing, bit-fields, unions and type punning, strict aliasing, flexible array members, designated initializers, opaque types, explicit binary formats | 3 |
| 07 | `07_functions_function_pointers_and_callbacks.md` | Pass by value, out-parameters, function pointer syntax, callbacks with `void *` context, `qsort`/`bsearch`, dispatch tables, state machines, variadic functions, format-string bugs, `static inline` | 2.5 |
| 08 | `08_the_preprocessor_and_macros.md` | `#include`, include guards, macro traps, `do { } while (0)`, `#` and `##`, X-macros, platform conditionals, `_Static_assert`, `_Generic`, logging macros | 2.5 |
| 09 | `09_undefined_behaviour_and_the_optimizer.md` | Implementation-defined, unspecified and undefined; the UB catalogue; how the optimiser deletes overflow and NULL checks; strict aliasing; `-fwrapv` and friends; UBSan and ASan | 3 |
| 10 | `10_error_handling_and_resource_cleanup.md` | How C reports failure, the rules of `errno`, goto-cleanup and single exit, ownership, `assert` versus runtime checks, a library error model in the Berkeley DB style, fault injection | 3 |
| 11 | `11_data_structures_in_c.md` | Dynamic arrays, singly and intrusive linked lists with `container_of`, an open-addressing hash table, ring buffer, binary heap, three ways to be generic in C | 3 |
| 12 | `12_memory_allocators_arena_pool_and_malloc.md` | Why custom allocators, alignment math, the arena and its ownership contract, a fixed-size pool, a teaching `malloc` with splitting and coalescing, a debug allocator with guards and leak reports | 3 |
| 13 | `13_files_io_and_posix_system_calls.md` | stdio buffering, `open`/`read`/`write` with partial transfers and `EINTR`, durability (`fsync`, atomic replace), binary formats read through `mmap`, a TCP echo server in C, Win32 equivalents | 3 |
| 14 | `14_multithreading_in_c_pthreads_and_c11.md` | pthreads, mutexes, condition variables, read-write locks, `pthread_once`, C11 `<threads.h>` and `<stdatomic.h>`, thread-safe versus reentrant versus async-signal-safe, a thread pool, TSan | 3 |
| 15 | `15_reading_debugging_and_changing_legacy_c.md` | Navigating an unfamiliar code base, global state, reproducing bugs, characterisation tests, the smallest safe fix, diagnostics, blame/log/bisect, reviewing and refactoring C, describing maintenance work in interviews | 3 |
| 16 | `16_modern_c_and_the_c_interview_playbook.md` | C standards in one table, C versus C++ differences, classic conceptual and coding questions with edge cases, how C interview rounds run | 3 |

Total: about 47 hours of study plus exercises.

Modules 01 to 08 are the **language model**: after them you can predict what any line of C does
and why. Modules 09 to 12 are **robust C**: the rules and techniques that keep a long-running server
alive. Modules 13 to 16 are **systems C and the job**: I/O, threads, legacy code, and interviews.

## How this course connects to the others

- **05_Memory_Debugging_RCA** is the other side of the same coin: modules 03, 04, 05 and 09 here
  explain the bugs whose crash dumps that course teaches you to read. Do modules 01 to 06 here
  before 05_Memory_Debugging_RCA modules 03 and 04.
- **02_Modern_CPP** shows how C++ encodes what C leaves to discipline: RAII instead of goto-cleanup
  (module 10 here), smart pointers instead of ownership comments (module 04 here), templates
  instead of `void *` and macros (modules 07 and 08 here).
- **04_Concurrency** teaches threads with C++17; module 14 here is the same material in C with
  pthreads and C11 `<threads.h>`, which is closer to your Win32 server.
- **07_Databases_Internals/12_migrating_a_kv_store_to_relational.md** applies module 06 (decoding
  packed records) and module 02 (amounts as integers) to your migration work.
- **08_Networking_Distributed/04_framing_and_protocol_design.md** applies modules 02, 06 and 07
  (byte order, explicit wire formats, state-machine parsers).
- **06_Linux_Toolchain_DevOps/02_how_a_cpp_program_is_built.md** and
  **06_Linux_Toolchain_DevOps/03_gcc_and_clang_flags_that_matter.md** go deeper into the linker and
  the warning flags used in module 01 and throughout; module 13 here pairs with
  06_Linux_Toolchain_DevOps/11_linux_systems_programming_essentials.md.

## Revising before a C round

The evening before an interview that includes C:

1. Read section 8 (Recap) of all sixteen modules aloud. About an hour.
2. Answer section 7 (Interview questions) of modules 02, 03, 04, 05, 06 and 09 aloud without
   looking, then check. These six cover most of what C rounds ask.
3. Do module 16's questions on paper, with a timer.
4. Pick two programs from `code/`, predict their output line by line, then run them.

## You are done when you can...

- [ ] Explain what the preprocessor, compiler, assembler and linker each do, read `nm` output, and
      tell an "undefined reference" from a "multiple definition" and fix both.
- [ ] Name the differences between MSVC C and GCC C that break ported code (`long`, VLAs,
      `_snprintf`, `_s` functions, Debug fill patterns).
- [ ] Predict the result of any mixed signed/unsigned expression, on Linux and on Windows, using the
      promotion and conversion rules.
- [ ] Write overflow-checked addition and multiplication, and explain why `x + 1 < x` gets deleted.
- [ ] Explain why money is never a `double` and implement interest in integer paise with explicit
      rounding.
- [ ] Draw any pointer, array, pointer-to-pointer or 2-D array situation as memory, and explain array
      decay and every `sizeof` trap.
- [ ] State the four storage durations, the exact contracts of `malloc`/`calloc`/`realloc`/`free`,
      write the realloc idiom, and document ownership of every pointer in an API.
- [ ] Write an arena allocator with alignment and explain its ownership contract.
- [ ] Explain why `strncpy`, `strcat`, `sprintf`, `atoi` and `strtok` are traps, and write a safe
      copy, a safe builder on `snprintf`, and a fully checked `strtol` wrapper.
- [ ] Predict any struct's size and offsets, decide when packing is acceptable, use unions and
      `memcpy` without breaking strict aliasing, and design a versioned binary record format.
- [ ] Read and write any function pointer declaration, build a dispatch table and a state-machine
      parser, and write a printf-like function with the `format` attribute.
- [ ] Write macros that survive every classic trap, an X-macro table, `_Static_assert` checks and a
      logging macro with file, line and function.
- [ ] Recognise the main kinds of undefined behaviour and catch them with UBSan and ASan.
- [ ] Write C functions that release every resource on every error path.
- [ ] Implement a vector, a hash table and a thread pool in C, and explain how each would look in C++.
- [ ] Walk into a C interview round and answer the questions in module 16 without notes.

## Resume lines this course backs up

> "Languages: C, C++17"

The C half of that line: all sixteen modules, and module 16 for the interview itself.

> "multithreaded server-side C"

Module 07 (callbacks, dispatch tables, state machines: how a server routes requests), module 10
(cleanup on every path), module 14 (threads in C) and module 15 (working safely in an existing
server code base).

> "C migration engine ... arena allocator with explicit ownership contracts"

Module 04 (storage, ownership comments, a first arena), module 06 (decoding packed records, explicit
formats), module 12 (allocators in depth), module 13 (binary files and I/O), with module 02 for
amounts and byte order.

> "memory corruption & leak investigation"

Modules 03, 04, 05 and 09 (how corruption and leaks happen in C, and how to make them impossible or
detectable), module 12 (a debug allocator with guard bytes and leak reports), together with
05_Memory_Debugging_RCA for the debugging tools and the RCA side.
