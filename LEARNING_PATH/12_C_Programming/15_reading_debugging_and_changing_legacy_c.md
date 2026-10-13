# 15. Reading, debugging and changing legacy C

**Time:** ~3 hours   **Prerequisites:** modules 09, 10, 14; 05_Memory_Debugging_RCA/05_gdb_from_zero_to_fluent.md; 06_Linux_Toolchain_DevOps/06_git_properly.md

## 1. In plain words

Most professional C work is not writing new programs. It is changing old ones without breaking
them: code you did not write, often without tests, written by people who have left, running at
customers who cannot accept downtime. Banks, telecoms, embedded products, databases and
operating systems all run on C that is ten or twenty years old, and every one of those
organisations needs engineers who can work in it safely.

That is a distinct, senior skill set: **find your way** around a large unfamiliar codebase,
**understand** what a piece of code really does (including the parts nobody documented),
**reproduce** a bug, **fix** it with the smallest safe change, **prove** nothing else changed, and
**explain** what happened. Your 2.5 years of analysing and fixing bugs in an existing
multithreaded server are experience in exactly this. This module gives that experience a method
and a vocabulary, and the last part of section 2 shows how to present it honestly and strongly in
interviews, without claiming to have built things you did not build.

An analogy: a surgeon, not a robot builder. The surgeon does not redesign the patient. She studies
the scans first, makes the smallest incision that fixes the problem, monitors the patient before,
during and after, and writes up exactly what she did. A careless surgeon who "cleans up" healthy
organs on the way is not more senior; she is more dangerous.

## 2. The real thing

### 2.1 Navigating a large unfamiliar C codebase

**Step 0: build it yourself.** Until you can build and run it, you are reading fiction. While
building, produce a compilation database (`compile_commands.json`), the file every modern C tool
reads to know each file's flags and include paths:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON     # CMake projects
bear -- make -j8                                            # Makefile projects (sudo apt install bear)
```

**Step 1: a code index.** With `compile_commands.json`, clangd (the VS Code clangd extension)
gives go-to-definition, find-references and a **call hierarchy** that understands macros and
include paths. For terminals and very old code, the classic tools still work:

```bash
ctags -R .                    # universal-ctags: jump to definitions from vim/emacs
cscope -Rbq                   # build the cscope database
cscope -dL1 kv_put            # where is kv_put defined?
cscope -dL3 kv_put            # who calls kv_put?
cscope -dL2 kv_put            # what does kv_put call?
```

On Windows, Visual Studio's "Find All References" and "View Call Hierarchy" do the same for an
MSVC project.

**Step 2: grep, precisely.** ripgrep (`rg`) recipes that answer real questions:

```bash
rg -n '\bprocess_line\s*\('                     # every call and the definition
rg -n 'g_txn_count\s*([-+*/]?=|\+\+|--)'        # every WRITE to a global
rg -n '\.handler\s*=|->handler\s*='             # where a function pointer is assigned
rg -n 'pthread_create|_beginthreadex|CreateThread'   # thread entry points
rg -n 'signal\(|sigaction\('                    # signal handlers
rg -n '^static [a-z_ ]+\*?[a-z_]+\[?' --type c  # file-scope statics: hidden state
```

**Step 3: find the entry points.** A server rarely flows from `main` in a straight line. List
`main`, every thread start routine, signal handlers, `atexit` handlers, Windows service entry
points (`ServiceMain`) and `DllMain`, and above all the **dispatch tables**: arrays of function
pointers indexed by message type or opcode. In an RPC server, the dispatch table is the map of
the whole application.

**Step 4: follow one request end to end.** Pick one operation (a login, a balance enquiry) and
trace it: accept, read the frame, decode, dispatch, handler, database call, response, logging.
Use the debugger with a breakpoint in the handler and `bt` to see the real call chain, or
`perf record -g` and `strace -f` to see what it does at run time. Write a one-page map. Then the
next request is twice as fast to understand.

**Step 5: read the data structures before the code.** The structs and globals tell you what the
program believes about the world. Fred Brooks put it as "show me your tables, and I won't usually
need your flowchart".

### 2.2 Global state and hidden coupling

Legacy C couples distant code through shared state that no function signature mentions:

| Hidden channel | Example | How to find it |
|----------------|---------|----------------|
| Globals and file-scope statics | `int g_txn_count;` updated in five files | `nm obj.o \| grep -E ' [BbDd] '` (B/b = bss, D/d = data); the `rg` write-pattern above |
| Function-local statics and static result buffers | `char *fmt_amount(long)` returning `static char buf[]` | `rg -n 'static char .*\['` |
| Library calls with hidden state | `strtok`, `localtime`, `rand`, `setlocale` | module 14's list of non-reentrant calls |
| Order of initialisation | a config global read before `load_config()` ran | breakpoints on the first reads |
| Process-wide settings | `umask`, current directory, signal dispositions, environment variables | `rg -n 'chdir|umask|setenv|getenv'` |
| Files and shared memory | two processes that "communicate" through a temp file | `strace -f -e trace=file` |

Each of these causes "action at a distance": a change in one module breaks another that shares
the hidden state. Before changing a function, list what global state it reads and writes. That
list is also its thread-safety story.

### 2.3 Reproducing a bug

1. **Collect facts before theories.** Exact version and build, configuration, input, time, log
   lines around the failure, the crash dump. Ask "what changed?" (deploy, configuration, data
   volume, operating system patch).
2. **Reproduce in the smallest setting you can.** A unit-level call with the captured input is
   best; a single request replayed against a test server is next; a full system test is last.
   Capture the exact bytes of the failing request (a log hex dump, `tcpdump`) so you can replay it.
3. **Make it deterministic.** For timing bugs: stress (many threads, many iterations), sanitizers
   (TSan finds races without the race having to happen), and record-and-replay debuggers: `rr`
   on Linux records a run and lets you step **backwards** through it; WinDbg's Time Travel
   Debugging does the same on Windows.
4. **Write the reproduction down as a failing test** before fixing. It proves you found the bug,
   and it stays in the suite so the bug cannot return silently.

### 2.4 Characterisation tests before changing code

A **characterisation test** pins down what the code does **today**, right or wrong. The term
comes from Michael Feathers' *Working Effectively with Legacy Code* (2004), which defines legacy
code as code without tests. The recipe:

1. Pick the function you need to change.
2. Call it with many inputs, including edge cases (zero, negative, maximum, empty, malformed).
3. Record the outputs **from the running code** (a `--print` mode that prints table rows works
   well), and paste them in as expected values. Do not "correct" them: a known bug is recorded
   as it is, with a comment.
4. Now refactor. Any difference is a behaviour change you must justify.

C has few natural places to cut code apart for testing. Feathers calls those places **seams**:

| Seam | How it works in C |
|------|-------------------|
| Link seam | Compile the test with `fake_db.c` instead of `db.c`; same function names, test behaviour |
| `--wrap` seam | GNU ld `-Wl,--wrap=send` sends calls to `send` to your `__wrap_send`; `__real_send` reaches the original |
| Preprocessor seam | `#ifdef UNIT_TEST` or `#define fopen test_fopen` in a test-only header |
| Function-pointer seam | the code calls through a pointer (`ops->send`) that the test replaces |

The fault-injection wrappers in `m10_goto_cleanup.c` are a seam of the last two kinds.

### 2.5 The smallest safe fix

- **Fix the root cause at the right layer, with the smallest diff that does it.** A NULL check
  that hides a lifetime bug is not a fix; a rewrite of the module to fix a one-line bug is not
  safe.
- **Separate refactoring commits from behaviour changes.** Reviewers can check a pure refactor
  against the characterisation tests and a small fix by reasoning. Mixed together, neither is
  checkable.
- **Check every caller.** `rg` for the function and for every place its output is used. A changed
  return value or error code ripples.
- **Look for siblings.** The same mistake is usually copied elsewhere (the same `realloc` misuse,
  the same missing `while` around a wait). Fix or ticket them all.
- **Preserve compatibility.** Wire formats, file formats, configuration keys, error codes,
  log formats that monitoring parses, struct layouts shared with other binaries. In banking, a
  client's batch files may depend on a lenient parser that you consider a bug.
- **Guard risky changes.** A configuration switch that restores the old behaviour lets operations
  roll back without a redeploy.

### 2.6 Adding diagnostics

When you cannot attach a debugger (a customer machine, an intermittent fault), add evidence:

- **Contextual log lines** at decision points: request id, account, sizes, error codes, and the
  file and line (`m15_diagnostics.c`).
- **A trace ring**: a fixed in-memory array of the last N events per thread, cheap enough to
  leave on in production and dumped when something goes wrong. It answers "what happened just
  before?", which a single error line cannot.
- **Soft asserts** (`CHECK`) that log the violated condition with context and return an error,
  instead of aborting a production server.
- **Hex dumps of bad input**, so the exact bytes reach the bug report.
- **Counters** (requests, errors by code, pool usage) exposed in a status command.

Two cautions. Logging changes timing, so a race may disappear when you add logs (a "heisenbug");
prefer the trace ring, which is just memory writes. And rate-limit error logging, or the first
bad client fills the disk.

### 2.7 History as evidence: blame, log and bisect

```bash
git blame -w -C -C -C file.c                 # ignore whitespace, follow code moved between files
git log -L :process_line:legacy.c            # the full history of one function
git log -S 'atol(' --oneline -- src/         # commits that added or removed this string
git log -G 'fee.*1000' --oneline             # commits whose diff matches a regex
git bisect start BAD GOOD; git bisect run ./test.sh   # binary search for the breaking commit
```

The commit message and its ticket number often explain why strange code exists ("customer X sends
amounts with trailing spaces"). Read them before deleting anything. `git bisect run` needs a test
script that exits 0 for good, 1 to 127 (except 125) for bad, and 125 for "cannot test, skip" (for example a
commit that does not compile). With n commits it needs about log2(n) builds. Older codebases may
live in SVN or TFS; `svn blame` and `svn log` answer the same questions.

### 2.8 Reviewing C changes

A checklist that catches most of what reaches production:

- Every new acquisition is released on every path, including errors (module 10).
- Every return value is checked; `errno` is saved before any other call.
- Size arithmetic cannot overflow; buffer lengths include the NUL; `snprintf` truncation is handled.
- Ownership of every pointer passed or returned is documented.
- New globals or statics are thread-safe, or documented as single-threaded.
- Locks: taken in the documented order, never held across blocking I/O, released on every path.
- No undefined behaviour: signed overflow, aliasing casts, uninitialised reads (module 09).
- Format strings match their arguments; no user data used as a format string.
- Signed/unsigned comparisons and narrowing conversions are deliberate.
- Behaviour changes are called out; characterisation or regression tests are included.
- Wire, file and struct-layout compatibility is preserved, or the version is bumped.

### 2.9 Refactoring legacy C safely

Behaviour-preserving steps, each small and each behind the characterisation tests:

1. **Extract function**: pull a block out of a 600-line function into a named function with
   explicit parameters. The name documents intent; the parameters expose hidden inputs.
2. **Introduce a struct**: replace a cluster of globals with a struct passed by pointer
   (`struct ledger *lg`). State becomes explicit and two instances become possible (tests!).
3. **Replace static result buffers** with caller-provided buffers (`fmt_amount_r(v, buf, n)`), and
   keep the old function as a thin wrapper so old callers still compile.
4. **Opaque types**: the header declares `typedef struct ledger ledger_t;` and functions; only the
   `.c` file knows the fields. Callers can no longer reach inside, so the layout can change.
5. **Make it const-correct** from the leaves upward; the compiler then finds unintended writes.
6. **Strangler approach**: build the new implementation behind the old interface, move callers
   over one at a time, and delete the old code when nothing calls it. The system works at every
   step. (The name comes from Martin Fowler's "strangler fig" pattern.)
7. **Ratchet the warnings**: record today's warning count per file in CI and fail the build if it
   rises; turn on `-Wall -Wextra` for each file as it is cleaned.

`m15_refactored_report.c` performs steps 1 to 4 on the legacy code, then fixes a bug in a
separate, final step.

### 2.10 Static and dynamic analysis on old code

| Tool | Command | Good at |
|------|---------|---------|
| GCC analyzer | `gcc -fanalyzer -c file.c` (GCC 10+) | leaks, double free, use after free, file-handle leaks in C, with the path that leads there |
| clang-tidy | `clang-tidy file.c -checks='-*,clang-analyzer-*,bugprone-*,cert-*' -- -std=c17` or `run-clang-tidy -p build` | path-sensitive analysis plus style and API-misuse checks |
| cppcheck | `cppcheck --enable=warning,style --project=build/compile_commands.json` (`sudo apt install cppcheck`) | fast, few false positives, works without a full build setup |
| MSVC | `/analyze` | the Windows build of the same code |
| ASan / UBSan | rebuild with `-fsanitize=address,undefined` and run the tests | real memory errors on the paths you execute |
| Valgrind | `valgrind --leak-check=full ./prog` | binaries you cannot rebuild; uninitialised reads |

Real output for this ten-line function, written in a style common in old code:

```c
int load_cfg(const char *path, char **out)
{
    FILE *f = fopen(path, "r");
    char *buf = malloc(256);
    if (!f)
        return -1;
    if (!fgets(buf, 256, f))
        return -2;
    fclose(f);
    *out = buf;
    return 0;
}
```

```text
$ gcc -std=c17 -fanalyzer -c load_cfg.c
load_cfg.c:10:16: warning: leak of 'buf' [CWE-401] [-Wanalyzer-malloc-leak]
load_cfg.c:12:16: warning: leak of FILE 'f' [CWE-775] [-Wanalyzer-file-leak]
load_cfg.c:12:16: warning: leak of 'buf' [CWE-401] [-Wanalyzer-malloc-leak]

$ clang-tidy load_cfg.c -checks='-*,clang-analyzer-*,bugprone-*,cert-*' -- -std=c17
load_cfg.c:10:17: warning: Potential leak of memory pointed to by 'buf' [clang-analyzer-unix.Malloc]
load_cfg.c:13:5: warning: the value returned by this function should not be disregarded; neglecting it may lead to errors [cert-err33-c]
```

(GCC also printed a duplicate "leak of 'f'" line, omitted here; each warning comes with the full
path of events in the real output.) GCC found the `FILE` leak that clang-tidy's default checks
missed, and clang-tidy flagged the unchecked `fclose` that GCC did not: run more than one tool.
On `m15_legacy_report.c`, clang-tidy also flags `atol` (`cert-err34-c`: "will not report conversion
errors") and every `sprintf`, but it equally flags every `snprintf` as "insecure", which is noise.
On an old codebase the first run produces hundreds of findings. Do not try to fix them all:
record a baseline, fix by category starting with the real bug classes (leaks, overflows, use after
free), and fail CI only on new findings.

### 2.11 Describing maintenance work in interviews

Interviewers for systems roles respect maintenance experience when it is described precisely,
because it is what most of the job is. The two mistakes are underselling it ("I just fixed bugs")
and overselling it (claiming to have designed or built what you maintained). Both get found out:
the first in the screen, the second in the follow-up questions.

**Use verbs that match what you did.** Diagnosed, root-caused, reproduced, fixed, hardened,
added tests for, reduced, extended, maintained, analysed crash dumps of. Use "designed" or
"built" only for what you designed or built.

**Lead with impact and root cause, not activity.** For each story have: the symptom and its cost
(crashes per week, escalations, a stuck batch), how you found the cause (dump, logs, reproduction,
bisect), the root-cause class in precise words (use-after-free in connection teardown, lock-order
inversion between the session table and the connection table, signed overflow in a length check),
the fix, and **how you prevented a recurrence** (a test, a sanitizer run, a review rule). Use only
numbers you can defend if asked how you measured them; if you do not have one, describe the scale
in words instead of inventing a figure.

**Resume bullets, before and after (fill in your real facts):**

```text
Weak:    Worked on bug fixing for the core-banking application server.
Strong:  Diagnosed and fixed [N] production defects in a multithreaded C (Win32/MSVC) RPC server
         used by [bank customers], including [use-after-free / deadlock / memory leak] root causes
         found through crash-dump analysis in WinDbg.

Weak:    Responsible for maintenance of the migration engine.
Strong:  Maintained and extended a C data-migration engine (Berkeley DB to YugabyteDB):
         root-caused [a data-loss / performance] issue in [batch handling], added regression
         tests that [caught X before release].
```

**A STAR answer shape for "tell me about a hard bug":**

```text
Situation  The server crashed [how often] at [a customer]; no reproduction in house.
Task       I owned the RCA and the fix.
Action     From the dump: crash in [function], freed memory being read. I read the teardown
           path and found two threads could release the same connection. Reproduced it with
           [a stress test that closes connections during requests]. Fixed the ownership so only
           [one thread] frees, kept the change to [N lines], and added [a regression test / an
           ASan run] so it cannot come back.
Result     [No recurrence since], RCA shared with support; I added the pattern to our review
           checklist and found [one more] instance elsewhere.
```

**When asked "have you built something like this from scratch?"** answer straight: "Not in
production. At work my role has been maintaining and fixing a large existing server, which taught
me [X and Y]. To build design experience I built tcpkv from scratch; here is how its thread pool
and protocol parser work." The capstone in 10_Capstone_Projects exists precisely so that this
answer has a strong second half. Align the stories in 11_Interview_Prep_Job_Search/02_telling_your_story.md
with the same rule: each story should use the verbs that match your actual role in it.

## 3. Code

### 3.1 `m15_legacy_report.c`: pinning current behaviour

The legacy code, as found (abridged):

```c
char g_last_error[128];
int  g_txn_count;
long g_total_paise;

char *fmt_amount(long paise)
{
    static char buf[40];
    long rupees = paise / 100;
    long frac = paise % 100;
    if (frac < 0)
        frac = -frac;
    sprintf(buf, "%ld.%02ld", rupees, frac);   /* BUG #4711: -50 -> rupees 0 -> "0.50" */
    return buf;
}
```

The sign is lost for amounts between -0.99 and -0.01 because `-50 / 100` is `0` in C (division
truncates toward zero) and `0` has no sign. Running `./m15_legacy_report --print` prints the
current behaviour as table rows (real output):

```text
FMT_CASES:
    { 0, "0.00" },
    { 5, "0.05" },
    { 100, "1.00" },
    { 123456, "1234.56" },
    { -250, "-2.50" },
    { -50, "0.50" },
    { -1, "0.01" },
    { 9223372036854775807, "92233720368547758.07" },
    { -9223372036854775808, "-92233720368547758.08" },
LINE_CASES:
    { line 0: ret 0, count 1, total 2500, error "" },
    { line 1: ret 0, count 2, total 1500, error "" },
    { line 2: ret 0, count 3, total 1488, error "" },
    { line 3: ret -1, count 3, total 1488, error "bad type REFUND" },
    { line 4: ret -1, count 3, total 1488, error "bad format" },
    { line 5: ret -1, count 3, total 1488, error "bad format" },
    { line 6: ret 0, count 4, total 1488, error "bad format" },
```

Those rows became the expected values, with the two wrong rows marked `KNOWN BUG #4711`. Other
surprises the table pins down rather than "fixes": `"DEBIT|ACC-1001|12abc"` is accepted as 12
paise (`atol`), `"CREDIT||500"` is a format error because `strtok` merges empty fields, and a
successful line does not clear the previous error message. Each of these may be relied on by
someone. Normal run:

```text
9 fmt cases, 7 line cases, 0 failures
all checks passed
```

### 3.2 `m15_refactored_report.c`: refactor, then fix

```c
struct ledger {                         /* step 1: the globals, made explicit */
    int  txn_count;
    long total_paise;
    char last_error[128];
};

/* step 3 + step 5: caller buffer, and the sign handled separately */
static int fmt_amount_r(long paise, char *buf, size_t n)
{
    unsigned long mag = paise < 0 ? 0UL - (unsigned long)paise : (unsigned long)paise;
    int w = snprintf(buf, n, "%s%lu.%02lu", paise < 0 ? "-" : "", mag / 100, mag % 100);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

/* step 4: the old API kept for old callers (thread-safe now, still not reentrant) */
static char *fmt_amount(long paise)
{
    static _Thread_local char buf[32];
    fmt_amount_r(paise, buf, sizeof buf);
    return buf;
}
```

`0UL - (unsigned long)paise` is the safe way to get the magnitude: `-LONG_MIN` would overflow.
The same tables run against the new code, with exactly two rows changed by the fix, each marked
in the table:

```text
9 fmt cases, 7 line cases, 0 failures (2 fmt rows changed by the #4711 fix)
all checks passed
```

The parser still uses `atol`: tightening it to reject `"12abc"` is a behaviour change that needs
a product decision (a customer's batch file may contain such amounts), so it is deliberately not
part of a refactor.

### 3.3 `m15_diagnostics.c`: log, trace ring, soft assert, hex dump

```c
#define CHECK(cond, ...) \
    ((cond) ? 1 : (check_failed(#cond, __FILE__, __LINE__, __func__, __VA_ARGS__), 0))

static int process_message(const unsigned char *msg, size_t len)
{
    TRACE("recv", len, msg[0]);
    if (!CHECK(len >= 3, "len=%zu", len))
        return -1;
    size_t declared = (size_t)msg[1] | ((size_t)msg[2] << 8);
    TRACE("header", msg[0], declared);
    if (!CHECK(declared == len - 3, "type=%u declared=%zu actual=%zu", msg[0], declared, len - 3)) {
        hexdump(g_log, msg, len);
        return -1;
    }
    TRACE("ok", msg[0], declared);
    return 0;
}
```

Real output (your timestamps will differ):

```text
2026-10-13T15:38:50.402Z INFO  t1 m15_diagnostics.c:163 main(): server starting, 4 workers
2026-10-13T15:38:50.402Z ERROR t1 m15_diagnostics.c:148 process_message(): check failed: declared == len - 3 (type=2 declared=255 actual=8)
  last 8 trace events of t1 (oldest first):
    #0 recv       a=7 b=1
    #1 header     a=1 b=4
    #2 ok         a=1 b=4
    #3 recv       a=5 b=1
    #4 header     a=1 b=2
    #5 ok         a=1 b=2
    #6 recv       a=11 b=2
    #7 header     a=2 b=255
  00000000  02 ff 00 41 43 43 2d 31 30 30 31                 |...ACC-1001|
all checks passed
```

One error produced the violated condition, the values involved, the two good messages that came
before, and the exact bytes of the bad one: enough to write the reproduction test without another
round trip to the customer.

A true story from writing this file: the first draft defined `#define TRACE(what, a, b)` and
wrote `ev_->what = (what);` in the body. The preprocessor replaced the member names too
(`ev_->"recv"`), and the build failed with `expected identifier before string constant`. Macro
parameters are substituted everywhere in the body, including after `->`. The fix, now in the file,
is parameter names that cannot collide (`what_`, `a_`, `b_`). Module 08's rules on macro hygiene
exist because of exactly this.

### 3.4 `m15_bisect_demo.sh`: let git find the guilty commit

The script builds a throwaway repository with eight commits; commit 5, titled "avoid overflow in
fee formula", changes `(amount_paise * 5) / 1000` to `amount_paise / 1000 * 5`, which divides
first and loses precision. Real output (hashes differ on every run):

```text
== history (newest first)
6ecc8b7 v8: release 2.3.1
b7ec1b8 v7: update banner
cdfa196 v6: tidy includes
0ec5311 v5: avoid overflow in fee formula
b8e3d8c v4: bump version string
8fd833e v3: parenthesise for readability
0247ed6 v2: add usage comment
0a9bd79 v1: initial fee calculation
== bisect
running '/tmp/tmp.Ejl77rsl9D/test.sh'
running '/tmp/tmp.Ejl77rsl9D/test.sh'
running '/tmp/tmp.Ejl77rsl9D/test.sh'
0ec531191d14d405afff3cba8b61628f659160e2 is the first bad commit
    v5: avoid overflow in fee formula
== the guilty change
0ec5311 v5: avoid overflow in fee formula
-long fee_paise(long amount_paise) { return (amount_paise * 5) / 1000; }
+long fee_paise(long amount_paise) { return amount_paise / 1000 * 5; }
```

Three test runs to search eight commits. The commit message sounded harmless, which is normal: the
message describes the intent, the diff shows the effect.

## 4. Under the hood (senior-level)

**Why changing old C is risky: implicit contracts.** C interfaces carry contracts the compiler
never sees: who frees, how long a pointer stays valid, which lock must be held, what a return value
of 0 means, whether a buffer is NUL-terminated. Old code also has **binary** contracts: a struct
written to a file or shared memory, sent on the wire, or compiled into another module or plugin
that is not rebuilt with yours. Adding a field to such a struct changes its size and offsets and
silently breaks the other side. Before touching a struct, `rg` for `sizeof(struct name)`, `fwrite`,
`send`, `memcpy` and shared-memory mappings that involve it.

**How `--wrap` and interposition work.** With `-Wl,--wrap=send`, the GNU linker resolves every
undefined reference to `send` in the objects being linked to `__wrap_send`, and references to
`__real_send` to the original. It only affects objects linked in that step, which makes it a
precise test seam. `LD_PRELOAD` works at load time instead: the dynamic linker searches the
preloaded library first, so its `malloc` or `send` wins for every library in the process.

**How static analysers find a leak.** `-fanalyzer` and the Clang static analyzer perform
path-sensitive symbolic execution: they walk each path through a function with abstract values
("`buf` points to a fresh heap region; `f` may be NULL") and report a state where the last
reference to an allocation is lost. They do not run the code, so they are limited by how far they
follow calls and loops, which produces both false positives and missed bugs. That is why their
findings are triaged, not blindly fixed, and why they complement sanitizers, which see only the
paths you actually execute.

**Record and replay.** `rr` records a run by capturing the results of every system call and
other nondeterministic event, running the program on one core so thread interleaving is fixed.
Replaying reproduces the run exactly, and gdb can then run it backwards (`reverse-continue`), so
you can set a watchpoint on a corrupted field and run backwards to the write that corrupted it.
WinDbg's Time Travel Debugging records at the instruction level and offers the same on Windows.
Both turn "it happened once" into "it happens every time I replay".

**Bisect complexity.** Bisect halves the candidate range each step, so 1000 commits need about
10 builds. It assumes the property is monotonic (good, then bad). Flaky tests break that
assumption: make the test script run the scenario several times and fail if any run fails, or
skip (125) commits where the build is broken.

## 5. In production

- **Restore service first, then diagnose, and preserve evidence.** At 3 AM the order is: keep
  the crash dump and logs, restart or fail over, disable the triggering feature if a switch
  exists, and only then debug. A restart that deletes the core file loses the only evidence
  (05_Memory_Debugging_RCA/11_production_incident_triage.md).
- **"Works at customer A, fails at B" is usually configuration or data.** Diff the configuration
  files, the data volumes, the operating system and runtime versions, and the time zone and
  locale. Legacy C is full of fixed-size buffers that only overflow with one customer's longest
  branch name.
- **Hotfixes go to old versions.** A bank may run a release two years old. The fix is written on
  the main branch with its test, then cherry-picked to each maintenance branch, rebuilt and
  retested there. Keep fixes small precisely so that they backport cleanly.
- **Production data is sensitive.** Reproducing with real account data is usually not allowed.
  Build the reproduction from the shape of the data (lengths, character sets, counts), or use a
  masked extract.
- **Every fix gets a regression test and every incident gets an RCA.** The RCA states the root
  cause, why it was not caught earlier, and what now prevents the class of bug
  (05_Memory_Debugging_RCA/12_writing_a_formal_rca.md). Over two or three years, that list of RCAs
  is your strongest interview material.

## 6. Try it

1. **Easy - map an unfamiliar codebase.** Clone a real C project (SQLite's amalgamation is one
   file; Redis is a server like yours). Find `main`, the event loop and the function that handles
   one command (for Redis, `GET`). Done when you have written the call chain from socket read to
   reply in ten lines or fewer, with file names.
2. **Easy - read history.** In any git repository you use, run `git log -L` on one function and
   `git log -S` on one string. Done when you can name the commit that introduced the current
   behaviour and the reason given in its message.
3. **Medium - characterise.** Pick a function at work or in an open-source project that has no
   tests. Write a `--print` mode that prints its outputs for 20 inputs including edge cases, then
   turn that into a test table. Done when the test passes and you have marked any output you think
   is a bug instead of changing it.
4. **Medium - analyse.** Install cppcheck (`sudo apt install cppcheck`) and run it, clang-tidy and
   `gcc -fanalyzer` on `m15_legacy_report.c` and on one real legacy file. Done when you have a
   table of every finding with a verdict: real bug, risky but intended, or false positive.
5. **Hard - seam with `--wrap`.** Write a function that calls `send` in a loop, then test it
   without a network: link the test with `-Wl,--wrap=send` and make `__wrap_send` return short
   writes and `EINTR`. Done when the test proves the function handles partial sends and `EINTR`.
6. **Hard - your stories.** Write three STAR stories from your real work using section 2.11: one
   crash or memory bug, one concurrency bug, one change that needed backward compatibility. Use
   only numbers you can defend. Done when each fits in two minutes spoken and a friend can repeat
   the root cause after hearing it once.

## 7. Interview questions

**Q: You join a team with a 500 000-line C codebase. How do you get productive?**
A: Build and run it first, generating `compile_commands.json` so clangd or ctags/cscope can index
it. Read the main data structures and find the entry points: `main`, thread start routines and the
dispatch table. Then trace one real request end to end with a debugger and write a short map. Fixing
a small bug early is the fastest way to learn the build, test and release process.

**Q: How do you change code that has no tests?**
A: Write characterisation tests first: call the code with many inputs, record what it does today
(including known bugs) and make that the expected output. Use seams such as link substitution,
`--wrap` or function pointers to isolate it. Then refactor in small behaviour-preserving steps
and make behaviour changes in separate commits with their own tests.

**Q: What is the smallest safe fix, and why does it matter?**
A: The minimal change that removes the root cause at the right layer, without unrelated cleanups.
It is easy to review, easy to backport to maintenance branches, and limits the risk of regressions
in code nobody fully understands. Refactoring and improvements come in separate, test-protected
changes.

**Q: How do you find which commit introduced a bug?**
A: Write a script that builds and tests one commit and exits 0 for good, 1 for bad and 125 to
skip unbuildable commits, then run `git bisect start BAD GOOD` and `git bisect run script`. It
finds the commit in about log2(n) steps. `git log -S`, `git log -L` and `git blame -w -C` narrow
the search when you already suspect a function or string.

**Q: What hidden coupling do you look for in legacy C?**
A: Globals and file-scope statics written in several files, functions returning static buffers,
library calls with hidden state such as `strtok` and `localtime`, initialisation order, and
process-wide settings like the current directory, `umask` and signal handlers. Each one lets a
change in one place break code elsewhere, and each one is a thread-safety question.

**Q: Which static analysis tools would you run on old C code, and how do you deal with the noise?**
A: GCC's `-fanalyzer`, clang-tidy with the analyzer, bugprone and CERT checks, cppcheck, and MSVC
`/analyze` on Windows, plus ASan, UBSan and Valgrind runs of the tests. The first run produces
many findings, so record a baseline, fix by bug class starting with leaks and memory errors, and
fail CI only on new findings.

**Q: Most of your experience is maintenance. Why should we hire you for a role building systems?**
A: Maintenance at depth means I have debugged the failure modes that new systems must design out:
use-after-free in teardown paths, lock-order deadlocks, leaks on error paths, and compatibility
breaks. I can name the root causes I fixed and the tests I added to prevent them. I have also built
a server from scratch (tcpkv) to practise the design side, and I can walk you through its
decisions.

## 8. Recap

- Legacy work is a senior skill: navigate, understand, reproduce, fix minimally, prove, explain.
- Build first; generate `compile_commands.json`; use clangd or ctags/cscope; grep precisely for
  writes, function-pointer assignments and thread entry points.
- Find entry points and dispatch tables; trace one request end to end; read data structures first.
- Hidden coupling: globals, statics, static buffers, non-reentrant library calls, init order,
  process-wide settings.
- Reproduce with facts, the smallest setting, deterministic tools (sanitizers, `rr`, TTD), and a
  failing test.
- Characterisation tests pin current behaviour, bugs included; seams in C are link, `--wrap`,
  preprocessor and function pointers.
- Smallest safe fix; refactors and behaviour changes in separate commits; check callers and
  siblings; keep compatibility.
- Diagnostics: contextual logs, per-thread trace rings, soft asserts, hex dumps, counters.
- `git blame -w -C`, `git log -L`, `git log -S`, `git bisect run` turn history into evidence.
- Run several analysers (`-fanalyzer` found a FILE leak clang-tidy missed); baseline the noise.
- In interviews: precise verbs, root causes, impact you can defend, prevention; never claim to
  have built what you maintained; tcpkv covers the build-from-scratch question.
