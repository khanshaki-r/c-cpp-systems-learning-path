# 04. The catalogue of memory bugs

**Time:** ~3 hours   **Prerequisites:** 01, 03

## 1. In plain words

There is a fixed, small list of ways C and C++ programs go wrong with memory. Every crash dump
you will ever read is one of these, or a combination. If you know the list by heart - what each
one looks like in the source, what symptom it produces in production, and which tool catches it -
then reading a dump becomes a matching exercise instead of a fishing expedition.

This module is the catalogue. One entry per bug class: a minimal reproduction, the typical
production symptom, how it shows up in a debugger, and which tool finds it fastest. The code
files in `code/` are the full versions with build and debug notes in the comments; this module
is the reference card you keep next to the dump.

## 2. The real thing

A compact index first; details follow.

| Bug | One-line cause | Crashes? | Best tool |
|-----|----------------|----------|-----------|
| Buffer overflow (heap) | write past end of a heap block | later, in free/malloc | ASan, Valgrind, PageHeap |
| Buffer overflow (stack) | write past end of a local array | at `ret` (canary) or garbage | ASan, `-fstack-protector`, `/GS` |
| Off-by-one | loop bound `<=` instead of `<` | often silent, sometimes as above | ASan, `-fsanitize=bounds`, `.at()` |
| Uninitialised read | use a variable/field never set | nondeterministic behaviour | Valgrind, MSan, `-Wmaybe-uninitialized` |
| Use-after-free | use memory after free/delete | silent corruption, sometimes crash | ASan, Valgrind |
| Double free | free the same block twice | abort in free, or later UAF | ASan, Valgrind, glibc |
| Dangling pointer/reference | pointer outlives its target | like UAF | ASan, `-Wdangling-pointer` |
| Stack-use-after-return | return address of a local | garbage reads/writes | ASan (`detect_stack_use_after_return`) |
| Invalid iterator | use an iterator after container mutates | silent or crash | `_GLIBCXX_DEBUG`, ASan |
| Memory leak | lose the last pointer to a block | growth, eventual OOM | LSan, Valgrind, heaptrack |
| Type confusion | reinterpret memory as the wrong type | garbage, crash on vtable | UBSan (`vptr`), review |
| Integer overflow -> bad size | size arithmetic wraps | tiny alloc then overflow | UBSan, `-ftrapv`, review |
| Null dereference | deref a null pointer | immediate crash at small addr | any; obvious in dump |

### 2.1 Buffer overflow (heap and stack)

**Cause.** Writing more bytes than the buffer holds. Heap version corrupts the next chunk's
header (module 03); stack version corrupts the saved frame pointer and return address.

```c
char *p = malloc(16);
strcpy(p, "this string is longer than sixteen bytes");   // heap overflow
char buf[16];
gets(buf);                                                 // stack overflow (gets: never use)
```

**Symptom.** Heap: a crash later in `free`/`malloc` with a glibc `free(): invalid next size`
message, or ASan `heap-buffer-overflow`. Stack: "*** stack smashing detected ***" + SIGABRT (if
the canary caught it), or SIGSEGV at `ret` with a garbage `pc` (the return address was
overwritten), or on Windows `0xC0000409` STATUS_STACK_BUFFER_OVERRUN. `code/m04_heap_overflow.cpp`.

**In the dump.** Stack overflow into the return address: `bt` shows `??` frames or a top frame
with a `pc` that is not in any function; the overflowed buffer's function is the one just below.
Heap: `pc` in `_int_free`; walk up to your `free` call; the chunk being freed is the overflowed
one or its neighbour.

### 2.2 Off-by-one

**Cause.** The most common overflow: `for (i = 0; i <= n; i++) a[i] = ...` writes `a[n]`, one
past a length-`n` array. Or `malloc(strlen(s))` forgetting the NUL terminator's byte.

```cpp
int *a = new int[n];
for (int i = 0; i <= n; ++i) a[i] = i;   // writes a[n], one past the end
```

**Symptom.** Usually silent: one element over often lands in padding or the next chunk's unused
space and nothing happens, until the input size or allocation pattern changes. Then it becomes a
heap overflow. The "works on my machine, fails at the client" bug. `code/m04_heap_overflow.cpp`
is an off-by-one.

**Tool.** ASan catches it at the write. `std::vector::at()` or `-D_GLIBCXX_ASSERTIONS` turns it
into a clean `std::out_of_range` or assertion. With compile-time-known bounds, GCC `-Warray-bounds`
(in `-Wall` at `-O2`) catches some.

### 2.3 Uninitialised read

**Cause.** Reading a variable, struct field, or heap block before writing it. Its value is
whatever was in that memory. For locals the compiler may leave a register; for heap, whatever the
allocator last left (often another object's old data - information disclosure risk).

```cpp
Record *r = new Record;   // no initialiser: members indeterminate
r->id = 7;                // but r->flags never set
if (r->flags & 1) ...     // reads garbage
```

**Symptom.** Nondeterministic: different behaviour run to run, works in debug (zeroed by luck or
by `MALLOC_PERTURB_`) fails in release, or the reverse. Security: leaked stale data. 
`code/m04_uninit_read.cpp`.

**Tool.** **Valgrind memcheck** is the tool (`--track-origins=yes` tells you where the
uninitialised value came from). ASan does **not** catch uninitialised reads;
**MemorySanitizer (MSan, clang only)** does. For locals, `-Wmaybe-uninitialized` / `-Wuninitialized`
(better at `-O2`) catch many at compile time. `MALLOC_PERTURB_=N` makes them reproducible.

### 2.4 Use-after-free and dangling pointers/references

**Cause.** Keeping and using a pointer (or reference, or iterator) after the object it points to
has been freed/deleted/gone out of scope. "Dangling" is the pointer; "use-after-free" is the act.

```cpp
Session *s = table[id];
table.erase(id);          // deletes the Session
audit(s);                 // s now dangles -> use-after-free
```
```cpp
const std::string &ref = make_name();   // binds to a temporary that dies at the semicolon
use(ref);                               // dangling reference
```

**Symptom.** Usually **silent corruption**, because freed memory stays mapped (module 01): you
read stale data or, worse, data belonging to a different object that reused the block, so one
feature's bug corrupts another feature's data. Sometimes a crash when the freed block's first
bytes (now a tcache pointer) are dereferenced. `code/m04_use_after_free.cpp`.

**Tool.** **ASan** is ideal: it quarantines freed memory and reports `heap-use-after-free` with
three stacks (use, free, allocation). Valgrind too. `-Wdangling-pointer` (GCC 12+, in `-Wall`)
and `-Wreturn-local-addr` catch the easy cases at compile time. The real fix is ownership
discipline: `unique_ptr`/`shared_ptr`, or ids instead of raw pointers (module 13).

### 2.5 Double free

**Cause.** Two owners both free the same block, or one path frees then an error path frees again.

```c
free(r->payload);   // handler frees it
free(r->payload);   // caller also frees it
```

**Symptom.** `free(): double free detected in tcache 2` + SIGABRT, if the block is still in the
tcache. If it was reallocated in between, the second free destroys a live object and you get a
UAF elsewhere - no double-free message, a mystery corruption instead. `code/m04_double_free.cpp`.

**Tool.** ASan (`attempting double-free` with both free stacks + alloc stack), Valgrind, glibc's
own check. Fix: set the pointer to `nullptr` after free (defensive), or use RAII so there is one
owner.

### 2.6 Stack-use-after-return

**Cause.** Returning the address of a local, or storing it somewhere that outlives the function.

```cpp
const char *name() { char buf[32]; sprintf(buf, "acc-%d", id); return buf; }  // returns dangling
int *p;
void set() { int local = 5; p = &local; }   // p dangles after set() returns
```

**Symptom.** The memory is reused by the next function call, so reads return garbage and writes
corrupt the next call's frame. Highly timing/layout dependent. `code/m04_stack_overflow.cpp` is
the recursion kind; the return-address kind is the snippet above.

**Tool.** ASan with `detect_stack_use_after_return=1` (default on recent versions) reports
`stack-use-after-return` with the function and variable. `-Wreturn-local-addr` /
`-Wdangling-pointer` catch the direct forms at compile time.

### 2.7 Invalid iterator / reference into a container

**Cause.** Mutating a container while holding an iterator, pointer or reference into it. `vector`
push_back/resize reallocates and moves elements; `erase` invalidates iterators at and after the
point; `unordered_map` rehash invalidates iterators (but not references to elements).

```cpp
for (auto it = v.begin(); it != v.end(); ++it)
    if (*it == x) v.erase(it);    // erase invalidates it; ++it is UB
```

**Symptom.** Skipped elements, duplicates, or a crash reading freed vector storage. Depends on
reallocation, so it may hide until the container grows past its capacity.
`code/m04_invalid_iterator.cpp`.

**Tool.** `-D_GLIBCXX_DEBUG` (libstdc++ debug mode) turns it into a clear abort at the point of
misuse ("attempt to increment a singular iterator"); ASan catches the reallocation UAF. The
`erase`/`remove` idiom (`v.erase(std::remove_if(...), v.end())`) and re-fetching iterators from
`erase`'s return value are the fixes.

### 2.8 Memory leak

**Cause.** Losing the last pointer to an allocation without freeing it: an early return that
skips `delete`, an exception between `new` and `delete`, overwriting a pointer, a container of
raw pointers cleared without deleting, or reference cycles with `shared_ptr`.

```cpp
void handle() {
    Request *r = new Request;
    if (!valid(r)) return;    // leak on the error path
    delete r;
}
```

**Symptom.** Slow RSS growth over hours/days, eventual OOM kill or allocation failure. Not a
crash at the leak site. `code/m09_leak.cpp` (covered fully in module 09).

**Tool.** LeakSanitizer (free with ASan), Valgrind `--leak-check=full`, heaptrack/massif for
growth over time. Fix: RAII (`unique_ptr`), and for cycles `weak_ptr`.

### 2.9 Type confusion

**Cause.** Interpreting memory as a type it is not: a bad `reinterpret_cast`, a union read
through the wrong member, a downcast to the wrong derived type (`static_cast` where `dynamic_cast`
was needed), deserialising into the wrong struct.

```cpp
struct A { virtual void f(); };
struct B : A { int extra; };
A *a = new A;
B *b = static_cast<B*>(a);   // a is not a B
b->extra = 1;                // writes past A; b->f() via a wrong vtable later
```

**Symptom.** Garbage field values, or a crash on a virtual call because the vtable pointer is
read from the wrong offset and jumped through. A `pc` that is 0 or points into data after a
virtual call is a classic sign.

**Tool.** UBSan `-fsanitize=vptr` catches bad polymorphic casts at the point of use; `-Wall`
catches some; `dynamic_cast` + checking for null is the safe pattern. Strict-aliasing violations
are a related UB class (`-fsanitize=alignment`, `-Wstrict-aliasing`).

### 2.10 Integer overflow leading to a bad size

**Cause.** Size or index arithmetic that wraps, producing a too-small allocation or a negative
value used as an unsigned size.

```c
int bytes = count * elem_size;        // count * elem_size overflows int -> small/negative
char *p = malloc(bytes);              // tiny allocation
memset(p, 0, count * elem_size);      // writes the intended (huge) size -> overflow
```

**Symptom.** A heap overflow whose size depends on input; often reachable from untrusted data,
so a security issue (CVE-shaped). `code/m04_integer_overflow_size.cpp`.

**Tool.** UBSan `-fsanitize=signed-integer-overflow` catches signed wrap; unsigned wrap is
defined so use `-fsanitize=unsigned-integer-overflow` deliberately or check with
`__builtin_mul_overflow`. Compute sizes in `size_t`, validate against a maximum, use
`std::vector`/`std::span`. `-Wconversion`/`-Wsign-conversion` flag the risky narrowings.

### 2.11 Null dereference

**Cause.** Dereferencing a pointer that is null because a lookup/allocation failed and the result
was not checked.

```cpp
Account *a = find(id);   // returns nullptr when not found
a->balance -= fee;       // crash when a == nullptr
```

**Symptom.** Immediate SIGSEGV at a small fault address (null + field offset). The easiest bug to
diagnose from a dump (module 06). `code/m04_null_deref.cpp`.

**Tool.** Obvious in any debugger. Prevent with checks, `std::optional`, references where null is
impossible, and `-fsanitize=null` (part of UBSan) to catch it precisely with a message.

### 2.12 Combinations

Real bugs combine: an integer overflow produces a small buffer (bug 10) that then overflows
(bug 1); a use-after-free (bug 4) reallocates the block as a different type, producing type
confusion (bug 9); a data race (module 10) frees an object twice (bug 5) or leaves a pointer
dangling (bug 4). The catalogue is the vocabulary; the skill is naming the combination from the
evidence.

## 3. Code

Each bug has a self-contained program in `code/`, with build and debug notes in the header
comment. They are the module-04 reproductions:

| File | Bug | Default behaviour |
|------|-----|-------------------|
| `m04_null_deref.cpp` | null dereference | SIGSEGV at 0x10 with `ACC-9999` |
| `m04_stack_overflow.cpp` | unbounded recursion (and the SUAR note) | SIGSEGV near the stack guard |
| `m04_heap_overflow.cpp` | off-by-one heap write | abort in free or ASan heap-buffer-overflow |
| `m04_use_after_free.cpp` | use-after-free | silent wrong output or ASan heap-use-after-free |
| `m04_double_free.cpp` | double free | SIGABRT "double free detected in tcache 2" |
| `m04_uninit_read.cpp` | uninitialised heap read | nondeterministic; Valgrind catches it |
| `m04_invalid_iterator.cpp` | erase-while-iterating + ref across realloc | garbage/crash; `_GLIBCXX_DEBUG` aborts |
| `m04_integer_overflow_size.cpp` | size wrap then overflow | UBSan signed-overflow report |

Build them all (none with `-Werror`, since they are intentionally buggy) and run the sanitizer
suite:

```bash
cd code
./build_debug.sh debug       # -g -O0 versions in build/
./build_debug.sh asan        # ASan versions
./build_debug.sh run-asan    # run each and show the first 25 lines of the report
./build_debug.sh run-valgrind
```

Example: the use-after-free under ASan gives you the three stacks that make it a five-minute fix:

```
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x... at pc 0x...
READ of size 8 at 0x... thread T0
    #0 audit_session(Session const*) m04_use_after_free.cpp:52
    #1 main m04_use_after_free.cpp:66
0x... is located 8 bytes inside of 48-byte region [...,...)
freed by thread T0 here:
    #0 operator delete(void*)
    #1 close_session(long) m04_use_after_free.cpp:39
previously allocated by thread T0 here:
    #0 operator new(unsigned long)
    #1 open_session(long, char const*) m04_use_after_free.cpp:30
```

## 4. Under the hood (senior-level)

**Why each class has the symptom it has** comes straight from modules 01 and 03:

- Overflows corrupt whatever is physically adjacent. Stack: saved registers and the return
  address (control-flow hijack, canary catches it). Heap: the next chunk's header (allocator
  aborts later) or the next object's fields (silent data corruption). Globals: the next global.
- UAF and double-free are silent because freed memory stays mapped and the allocator reuses the
  bytes for its free-list metadata (the tcache pointer you saw in module 03) or for a later
  allocation. Reading sees a pointer or another object; writing corrupts them.
- Uninitialised reads are nondeterministic because the value is literally leftover memory; it
  changes with allocation history, ASLR, and what ran before.
- Integer-overflow-to-small-buffer is dangerous because the two halves (the wrap and the
  overflow) are in different statements and the size often comes from untrusted input, making it
  a remote memory-corruption primitive.

**Undefined behaviour is the common thread.** Every bug here is UB in the language standard,
which is why the compiler is allowed to do surprising things: delete a null check *after* a
dereference (because UB means the pointer "cannot" be null), assume a signed add does not
overflow and optimise a bounds check away, or reorder around a use-after-free. This is why a bug
can appear only at `-O2`: the optimiser exploited a UB assumption. `-fsanitize=undefined` exists
precisely to make these assumptions observable. The senior mindset: a warning or sanitizer
finding about UB is a real bug even if the program "works today", because the next compiler
version may compile it differently.

**Exploitability.** Overflows, UAF and type confusion are not just crashes; they are the raw
material of exploits (control-flow hijack via overwritten return address or vtable, heap grooming
via tcache poisoning). For a banking vendor this elevates a "stability bug" to a "security
vulnerability" with disclosure obligations. Treat memory-safety findings as security findings.

## 5. In production

- **Match the symptom to the class before touching a debugger.** SIGSEGV at 0x10: null deref.
  SIGABRT with a glibc message: heap corruption or double free (read the message). SIGSEGV with
  `pc` garbage and `??` frames: stack overflow of a return address. Nondeterministic wrong
  answers: uninitialised read or data race. Steady RSS growth: leak. This triage (module 11) is
  faster than opening the core.
- **The "works here, fails there" bugs are the catalogue's uninitialised reads, off-by-ones and
  races.** They depend on memory contents, allocation order and timing, which differ between the
  developer laptop and the client's production box. Reproduce under ASan/Valgrind/TSan rather
  than on the developer machine.
- **Banking data corruption is worse than a crash.** A UAF or overflow that silently changes a
  balance and does not crash is the nightmare scenario: wrong numbers, no alarm. This is the
  argument for running the whole test suite under sanitizers in CI, and for arena allocators with
  guard bytes in the migration engine.
- **Your C background is a double edge.** C has no RAII, no bounds-checked containers, manual
  ownership - every bug in this catalogue is easy to write. Moving the new C++ code to
  `std::vector`/`std::string`/smart pointers removes whole classes (module 13). The existing C
  server benefits most from sanitizer CI and defensive asserts.

## 6. Try it

1. **Easy - classify by symptom.** For each of these, name the bug class without running
   anything: "SIGABRT, free(): invalid next size"; "SIGSEGV at 0x0"; "balance sometimes wrong,
   never crashes"; "RSS up 2 GB over a week"; "crash at ret, pc = 0x4141414141414141". Done when
   all five are named with one-line reasoning.
2. **Easy - run the suite.** Run `./build_debug.sh run-asan` and for each program write the one
   line of the report that names the bug. Done when you have eight lines.
3. **Medium - ASan vs Valgrind vs nothing.** Run `m04_uninit_read` three ways: plain, under
   ASan, under Valgrind. Done when you can explain why only Valgrind reports it.
4. **Medium - make the off-by-one bite reliably.** Modify `m04_heap_overflow` so it reliably
   aborts (hint: allocate, free a neighbour, allocate again to control layout; or just run with
   a size that makes the stray write hit a header). Done when it aborts every run and you can
   name the glibc message.
5. **Hard - combination bug.** Write a program that combines an integer overflow and a heap
   overflow (size wrap producing a small buffer, then a copy of the intended size). Catch it with
   UBSan and separately with ASan. Done when you can show both tools' reports and explain which
   half each caught.
6. **Hard - turn a crash into silent corruption.** Take `m04_double_free`, insert an allocation
   of the same size between the two frees so the second free destroys a live object instead of
   aborting. Show that the program now produces wrong output with no error. Done when you can
   explain why the glibc double-free check no longer fires.

## 7. Interview questions

**Q: Give the catalogue of memory bug classes.**
A: Buffer overflow (heap/stack), off-by-one, uninitialised read, use-after-free, double free,
dangling pointer/reference, stack-use-after-return, invalid iterator, memory leak, type
confusion, integer-overflow-to-bad-size, and null dereference. Each is undefined behaviour and
maps to a specific production symptom and a best-fit tool.

**Q: Which of these does AddressSanitizer NOT catch, and what do you use instead?**
A: ASan does not catch uninitialised reads (use Valgrind memcheck, or MemorySanitizer on clang),
and it does not catch most integer-overflow or type-confusion UB (use UBSan). ASan is for
spatial (overflow) and temporal (UAF, double free, leak) memory errors.

**Q: Why is use-after-free more dangerous than a null dereference?**
A: A null dereference crashes immediately and deterministically at a recognisable address, so
it is easy to find and it fails safe. A use-after-free usually does not crash - freed memory is
still mapped and may be reused by another object - so it silently corrupts data, can leak or
alter sensitive values, and is exploitable for control-flow hijack. It fails dangerously and is
hard to reproduce.

**Q: How can an integer overflow become a buffer overflow?**
A: If a size is computed with `count * element_size` in a type too small, the multiplication
wraps to a small value, `malloc` returns a tiny buffer, and a subsequent copy or loop writes the
intended (large) number of bytes past it. Because the count often comes from untrusted input,
this is a remotely triggerable memory-corruption primitive. Fix: compute in `size_t`, use
`__builtin_mul_overflow`, and cap sizes.

**Q: Why does a bug sometimes appear only in an optimised build?**
A: The optimiser is allowed to assume the program has no undefined behaviour, so it may remove a
null check after a dereference, assume a signed add does not overflow, or reorder around a
use-after-free - exposing a latent bug that the unoptimised build happened to tolerate. The bug
was always there; `-O2` just stopped hiding it. Build with `-O2 -Wall` and sanitizers to surface
these.

**Q: A balance is occasionally wrong but the service never crashes. Which classes are in play?**
A: Silent-corruption classes: use-after-free writing through a reused block, a heap/stack
overflow into a neighbouring object's fields, an uninitialised read, or a data race producing a
lost update. Reproduce under ASan (overflow/UAF), Valgrind (uninitialised) and TSan (race);
whichever fires names the class.

## 8. Recap

- There is a fixed catalogue of memory bugs; every dump is one or a combination of them.
- Overflows corrupt adjacent memory: stack -> return address (canary/0xC0000409), heap -> next
  chunk header (later free abort) or next object (silent corruption).
- UAF, double free and dangling pointers are usually silent because freed memory stays mapped;
  ASan quarantines freed memory and gives use/free/alloc stacks.
- Uninitialised reads are nondeterministic; Valgrind (not ASan) catches them; MSan on clang too.
- Integer-overflow-to-small-buffer is a security-grade overflow; compute sizes in `size_t`.
- Null deref is the easy one: immediate SIGSEGV at a small address.
- Tool fit: ASan = spatial+temporal memory errors + leaks; Valgrind = those + uninitialised;
  UBSan = integer/type/alignment UB; `_GLIBCXX_DEBUG` = iterator misuse; LSan/heaptrack = leaks.
- Every entry is undefined behaviour, so the optimiser can make it appear only at `-O2`; treat
  UB findings as real bugs and as potential security vulnerabilities.
