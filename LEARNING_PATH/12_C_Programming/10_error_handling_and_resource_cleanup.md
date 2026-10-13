# 10. Error handling and resource cleanup

**Time:** ~3 hours   **Prerequisites:** modules 04 (memory, storage and lifetime), 05 (strings), 09

## 1. In plain words

C has no exceptions and no destructors. A function that can fail tells you so through its
return value, and every resource you acquire (memory, a file, a socket, a lock, a database
handle) you must release yourself, on **every** path out of the function: the normal path and
each error path.

That second part is where mature C servers keep most of their bugs. The normal path runs a
million times a day and is well tested. The path where the third `malloc` fails, or the second
`fopen` fails, runs almost never, so nobody notices that it leaks the first buffer or closes a
file twice. Then a disk fills up at a customer site, every request takes the error path, and the
process leaks itself to death.

Think of closing a bank branch at night. Whatever was opened during the day (vault, safe,
cash drawer, lights) must be closed in the reverse order, whether the staff leave at six as
usual or at three because the fire alarm went off. The goto-cleanup idiom in this module is that
checklist written in code: one place at the bottom of the function, closing things in reverse,
reached from every exit.

This module covers how C reports errors (return codes, `errno`), how to clean up correctly, who
owns what, when to use `assert`, how to log an error so that someone can act on it, and how to
design the error model of a C library the way Berkeley DB did.

## 2. The real thing

### 2.1 The ways C code reports failure

| Convention | Example | How you check | Trap |
|------------|---------|---------------|------|
| `-1` (or `NULL`) plus `errno` | POSIX `open`, `read`, `fopen`, `malloc` | `if (fd < 0) { use errno }` | `errno` is only valid right after the failure |
| Negative errno as return value | Linux kernel: `return -ENOMEM;` | `if (ret < 0)` | mixing it with the `-1` style in one codebase |
| Positive error number as return value | `pthread_*`, Berkeley DB, `getaddrinfo` (its own codes) | `if (rc != 0)` | `perror` is wrong here: `errno` was not set |
| Sentinel value | `EOF` from `fgetc`, `NULL` from `strchr`, `0` from `fread` | compare, then ask why (`ferror`, `feof`) | `0` from `fread` can be EOF or an error |
| Out-parameter error object | `struct err *e` filled by the callee | inspect after the call | forgetting to initialise it |
| Per-thread "last error" | `errno`, Win32 `GetLastError()`, `WSAGetLastError()` | read immediately | any other call in between may overwrite it |

Pick **one** convention per library or module and write it in the header. Most bugs here come
from a caller assuming one convention while the callee uses another, for example calling
`perror` after `pthread_mutex_lock` fails (pthreads return the error number and leave `errno`
alone).

### 2.2 The rules of `errno`

`errno` is a per-thread integer (C11 and POSIX make it thread-local; glibc defines it as
`(*__errno_location())`). Its rules are stricter than most people remember:

1. It is meaningful **only** when the function's return value said "failed".
2. No library function ever sets it to 0. A successful call may still **change** it (glibc's
   `printf` can leave `ENOTTY` behind the first time it probes whether stdout is a terminal).
3. Any call between the failure and your read of `errno` may overwrite it. Logging is the usual
   culprit: your logger calls `fopen`, `write` or `localtime`, and the reason is lost.
4. `strtol`, `strtoll` and friends are the exception that needs `errno = 0` **before** the call,
   because they report overflow only through `errno == ERANGE`.
5. `strerror(e)` returns text but is not guaranteed thread-safe. Use `strerror_r`. Careful: with
   `_POSIX_C_SOURCE` you get the XSI version that returns `int` and fills your buffer; with
   `_GNU_SOURCE` you get the GNU version that returns a `char *` that may not be your buffer.

On Windows the equivalent is `GetLastError()`, with the same overwrite problem, plus
`WSAGetLastError()` for Winsock. `FormatMessage` turns the number into text.

### 2.3 The goto-cleanup idiom

Without `goto`, a function that acquires three resources needs either deeply nested `if`s (the
"arrow" shape) or copies of the cleanup code at every `return`, and the copies drift apart. With
`goto`, every failure jumps forward to a cleanup block at the bottom. Two shapes are common.

**Shape 1, Linux kernel style: one label per resource, in reverse order of acquisition.**

```c
in = fopen(src, "rb");
if (!in) { err = errno; goto out; }
out = fopen(dst, "wb");
if (!out) { err = errno; goto close_in; }
buf = malloc(N);
if (!buf) { err = ENOMEM; goto close_out; }
/* ... work ... */
free_buf:
    free(buf);
close_out:
    if (fclose(out) != 0 && err == 0) err = errno;
close_in:
    fclose(in);
out:
    return err;
```

A failure jumps to the label that releases exactly what has been acquired so far, and falls
through the rest. The labels read like a stack being unwound.

**Shape 2: one `cleanup:` label, every resource starts as NULL.**

```c
FILE *f = NULL;
char *line = NULL;
struct table *t = NULL;
/* ... every failure: err = ...; goto cleanup; ... */
*out = t;      /* success: ownership moves to the caller ... */
t = NULL;      /* ... so cleanup must not free it */
cleanup:
    table_free(t);         /* accepts NULL, like free() */
    free(line);
    if (f) fclose(f);      /* fclose(NULL) is UB: guard it */
    return err;
```

Shape 2 is easier to keep correct when code is edited later, because the order of labels cannot
get out of step with the order of acquisition. It needs release functions that accept NULL, or a
guard for the ones that do not: `fclose(NULL)` is undefined, `close(-1)` merely fails with
`EBADF`, and Win32 `CloseHandle` on an invalid handle fails (and raises an exception under a
debugger).

Why `goto` is accepted here: the jumps go only forward and only to cleanup, so the control flow
stays readable. The Linux kernel coding style recommends it, and the SEI CERT C rule MEM12-C
("consider using a goto chain when leaving a function on error when using and releasing
resources") describes it. The alternatives have their own costs: `do { ... } while (0)` with
`break` works for one level only; GCC and Clang's `__attribute__((cleanup(fn)))` (used by
systemd's `_cleanup_free_`) runs a function when a variable leaves scope, which is close to RAII
but not portable to MSVC.

### 2.4 Single exit discipline

"One return at the bottom" makes cleanup and logging easy to verify and is required by some
standards (MISRA C 2012 rule 15.5 is advisory). Enforced dogmatically, it produces flag
variables and deep nesting. The pragmatic rule most senior C programmers use:

- **Early returns are fine before you have acquired anything** (argument validation).
- **After the first acquisition, every exit goes through the cleanup block.**
- Never `return` from the middle of a function that holds a lock.

### 2.5 Ownership, written down

C has no type that says "I own this". So you say it in names and in comments, and you keep the
same convention everywhere:

| Convention | Meaning |
|------------|---------|
| `x_create()` / `x_destroy()`, `x_open()` / `x_close()`, `x_init()` / `x_fini()` | pairs: whoever called the first calls the second |
| "Returns a pointer the caller must free with `x_free()`" | ownership transferred to the caller |
| "Returns a pointer into `t`, valid until the next call that modifies `t`" | borrowed; do not free, do not keep |
| `const char *` parameter | the function reads it and does not keep it after returning |
| "Takes ownership of `buf`" | after the call, the caller must not touch or free `buf` |
| Out-parameter "set only on success" | on failure the caller's variable keeps its previous value |

The idiom `*out = t; t = NULL;` before the cleanup label is how ownership transfer looks in code:
the cleanup frees whatever the function still owns, and after the transfer it owns nothing.
`free(NULL)` being a no-op is what makes this pattern cheap. Setting a pointer to `NULL` after
freeing it turns a later double free into a harmless no-op and a later use into an immediate,
easy-to-diagnose null dereference.

### 2.6 `assert` versus runtime checks

| | `assert(cond)` | runtime check |
|---|---|---|
| For | conditions that can only be false if the **program** is wrong (invariants, preconditions between your own functions) | conditions that depend on **input or environment** (user data, files, network, memory, disk) |
| In release builds | removed when `NDEBUG` is defined | always present |
| On failure | prints the expression, file, line, then `abort()` (core dump) | returns an error the caller handles |
| Example | `assert(acc != NULL)` in an internal function | `if (amount > balance) return WD_INSUFFICIENT;` |

Rules that follow:

- Never put work inside `assert`: `assert(fd = open(...))` disappears in release builds.
- Never use `assert` to validate input. A malformed message must not kill a banking server.
- `_Static_assert` (C11) checks layout assumptions at compile time for free:
  `_Static_assert(sizeof(struct hdr) == 16, "wire header layout");`.
- Some server teams keep asserts **on** in production on purpose: if an internal invariant is
  broken, stopping with a core dump is better than continuing and writing wrong balances. That
  is a deliberate decision; write it down, and still never assert on input.

### 2.7 Logging an error so someone can act on it

A useful error line answers five questions: **what** was being attempted, on **which** object,
**what went wrong** (code and text), **where** in the code, and **which request** it belonged to.

```text
ERROR m10_error_model.c:261 main(): loading accounts for branch 42: No such file or directory (code 2)
```

Two rules keep logs useful at scale:

- **Log once, at the layer that decides what to do.** Lower layers return the code plus a detail
  string; the request handler logs it with the request id. If every layer logs, one failure
  becomes five lines and nobody can count incidents.
- **Save the error code before calling the logger.** The logger itself may change `errno` or
  `GetLastError()` (section 2.2 and the real bug in `m10_errno_conventions.c`).

### 2.8 Designing a C library's error model, Berkeley DB style

Berkeley DB, which you meet in 07_Databases_Internals/10_berkeley_db_and_embedded_kv_stores.md,
has a clean model worth copying:

1. Every function returns `int`; `0` is success.
2. **Positive** values are system `errno` codes passed through unchanged (`ENOENT`, `ENOMEM`).
3. **Negative** values in a reserved range are the library's own codes (`DB_NOTFOUND`,
   `DB_KEYEXIST`, `DB_LOCK_DEADLOCK`, ...), chosen far from zero (near -30000) so they never
   collide with an errno.
4. One function, `db_strerror(int)`, turns any of them into text.
5. **Expected outcomes are not errors.** `DB_NOTFOUND` from a `get` is a normal answer. It is
   returned quietly, not logged.
6. Detailed messages go to an application-supplied callback (`DB_ENV->set_errcall`) with a
   prefix (`set_errpfx`), so the application decides where messages are written.

`m10_error_model.c` implements the same model for a tiny key-value store and adds two habits:
the handle keeps the detailed message for the last failure (like `sqlite3_errmsg(db)` in
SQLite), and out-parameters are written only on success. Other well-known models: SQLite returns
`SQLITE_OK` or a code and keeps a per-connection message; OpenSSL keeps a per-thread queue of
errors. Whatever you choose, a library must **never** call `exit()` or `abort()` on a runtime
error: that decision belongs to the application.

### 2.9 What C++ changes (and what it does not)

In C++, destructors run on every exit path, including exceptions, so the goto chain becomes a
set of RAII objects and the cleanup code disappears (02_Modern_CPP/03_raii_the_core_idea.md).
Errors can travel as exceptions or as values (`std::optional`, `std::expected` in C++23; see
02_Modern_CPP/10_error_handling_exceptions_optional_expected.md). But as soon as C++ calls a C
library (POSIX, Win32, Berkeley DB, libpq), the C conventions of this module are back: you check
the return code, read `errno` or `GetLastError()` immediately, and usually wrap the handle in a
small RAII class whose destructor calls the C close function.

## 3. Code

### 3.1 Both cleanup shapes, proved by fault injection (`m10_goto_cleanup.c`)

The file wraps `malloc`, `realloc`, `free`, `fopen` and `fclose` in counting wrappers. Setting
`g_fail_at = N` makes the Nth wrapped call fail. The test first runs a function successfully
and counts its calls, then reruns it once per call with that call failing, and after each run
checks that no allocation is live and no file is open:

```c
static void *t_malloc(size_t n)
{
    if (should_fail()) return NULL;
    void *p = malloc(n);
    if (p) g_live_allocs++;
    return p;
}

/* in main(): */
for (int n = 1; n <= load_points; n++) {
    struct account_table *never = (struct account_table *)&never;   /* sentinel */
    reset_injection(n);
    int err = load_accounts(acc, &never, NULL);
    assert(err == ENOMEM || err == EMFILE);
    assert(never == (struct account_table *)&never);   /* *out untouched on failure */
    assert(g_live_allocs == 0 && g_open_files == 0);
}
```

Real output:

```text
copy_file: 3 failure points injected, 0 leaks, 0 open files
load_accounts: 6 failure points injected, 0 leaks, 0 open files
load_accounts(bad file): Invalid argument at line 3
load_accounts(missing file): No such file or directory
all checks passed
```

Six failure points for `load_accounts`: the `fopen`, two `malloc`s and the three `realloc`s that
grow the table from 4 to 8 to 16 entries. Every one of them is now a tested path. This technique
is how you test error handling that never happens on a developer machine; it works the same for
Win32 (`HeapAlloc`, `CreateFileW`) wrappers.

Note the `realloc` handling inside `load_accounts`:

```c
struct account *grown = t_realloc(t->items, ncap * sizeof *grown);
if (!grown) {                    /* t->items is still valid and is freed below */
    err = ENOMEM;
    goto cleanup;
}
t->items = grown;
```

Writing `t->items = realloc(t->items, ...)` directly would overwrite the only pointer to the old
block with `NULL` on failure: a leak that only happens when memory is already short.

### 3.2 A library error model (`m10_error_model.c`)

```c
#define KV_OK        0
#define KV_NOTFOUND  (-30500)   /* key not present: a normal outcome, not a failure */
#define KV_KEYEXIST  (-30501)
#define KV_FULL      (-30502)
#define KV_BADARG    (-30503)
#define KV_CORRUPT   (-30504)

typedef struct kv kv_t;         /* opaque: callers never see the fields */

const char *kv_strerror(int err)
{
    switch (err) {
    case KV_NOTFOUND: return "KV_NOTFOUND: key not found";
    /* ... */
    }
    if (err > 0)
        return strerror(err);   /* system error passed through */
    return "KV: unknown error code";
}

/* Records a contextual message on the handle, reports it through the callback, and
 * returns err so call sites can write: return kv_fail(kv, KV_FULL, "...", ...); */
static int kv_fail(kv_t *kv, int err, const char *fmt, ...);
```

Real output (the first three lines come from the application's error callback, the fourth from
the `LOG_ERR` macro that adds file, line and function):

```text
[kv-test] kv_put: key 'ACC-1': KV_KEYEXIST: key already exists
[kv-test] kv_put: key 'ACC-3', capacity 2: KV_FULL: store is full
[kv-test] kv_load: open '/no/such/file.kv': No such file or directory
ERROR m10_error_model.c:261 main(): loading accounts for branch 42: No such file or directory (code 2)
all checks passed
```

The test also checks that `kv_get` of a missing key returns `KV_NOTFOUND` **without** calling
the error callback, and that a failed `kv_open` leaves the caller's pointer untouched.

### 3.3 errno done right and done wrong (`m10_errno_conventions.c`)

```c
static int parse_long(const char *s, long *out)
{
    char *end;
    errno = 0;                         /* strtol sets errno only on failure; clear it first */
    long v = strtol(s, &end, 10);
    if (end == s)       return EINVAL; /* no digits */
    if (errno == ERANGE) return ERANGE;
    /* ... skip trailing spaces ... */
    if (*end != '\0')   return EINVAL; /* "12abc": atoi would silently return 12 */
    *out = v;
    return 0;
}

static int open_for_write_buggy(const char *path)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_to_file("open failed\n");  /* BUG: clobbers errno before the caller reads it */
        return -1;
    }
    return fd;
}
```

Real output. Opening the directory `/` for writing fails with `EISDIR`; the buggy version's
logger tries to open a log file in a missing directory, and that `ENOENT` is what reaches the
caller:

```text
parse_long("12abc") -> Invalid argument; atoi("12abc") -> 12
buggy: errno says 'No such file or directory'
fixed: errno says 'Is a directory'
all checks passed
```

Imagine that second line in a production log. The support engineer spends a day looking for a
missing file. The real problem was a path that pointed at a directory.

## 4. Under the hood (senior-level)

**What `goto` compiles to.** Nothing special: a jump. GCC produces essentially the same machine
code for the goto chain as for the equivalent nested `if`s. The cleanup block is ordinary code
that the optimiser can share between paths. With `__builtin_expect` (or the kernel's
`unlikely()` macro) on the failure tests, GCC lays out the error blocks away from the hot path,
and with `-freorder-blocks-and-partition` (on by default at `-O2` on x86) it moves blocks it
believes are cold, from `__builtin_expect` or profile data, to a separate `.text.unlikely` section, so error handling costs almost nothing in
instruction-cache space.

**Why C has no exceptions, and what `setjmp`/`longjmp` really do.** `setjmp` saves the
callee-saved registers, stack pointer and program counter into a `jmp_buf`; `longjmp` restores
them, which makes `setjmp` appear to return a second time. Nothing in between is unwound: no
`free`, no `fclose`, no `pthread_mutex_unlock`. Local variables modified after `setjmp` have
indeterminate values after the jump unless they are `volatile`. That is why `longjmp` is used
only in narrow places (some parsers, Lua's error handling, libpng's error callback) and why
C++ exceptions needed compiler-generated unwind tables (`.eh_frame`) to run destructors.

**How `errno` is per-thread.** glibc declares `extern int *__errno_location(void)` and defines
`errno` as `(*__errno_location())`. The function returns the address of a thread-local variable
(in the TLS block that the `%fs` register points to on x86-64). A system call wrapper stores the
negated kernel return value there when the kernel returns a value in the range -4095 to -1. So
`errno` is cheap to read, but every library call is free to write it.

**`__attribute__((cleanup))`.** GCC and Clang call the named function with a pointer to the
variable when the variable goes out of scope, by any path (return, `break`, `goto`), but not on
`longjmp`. It is the closest C gets to RAII, and code like systemd relies on it heavily. MSVC
does not support it, which rules it out for a Win32 codebase.

**Fault injection at scale.** The wrapper technique in 3.1 needs source changes. Alternatives:
an `LD_PRELOAD` library that replaces `malloc` and fails the Nth call (no recompilation), the
Linux kernel's fault-injection framework (`failslab`, `fail_page_alloc`) for kernel code, and on
Windows Application Verifier's low-resource simulation. Running the full test suite with each
injected failure point is expensive; teams usually run it nightly rather than per commit.

## 5. In production

- **Error-path leaks are slow incidents.** A leak in the error path of request parsing costs
  nothing until a misbehaving client sends a burst of bad requests. Memory then grows by
  (leak size x bad requests) and the process is eventually killed. The RSS graph shows steps that
  line up with error-log bursts, which is the clue (05_Memory_Debugging_RCA/09_leaks_and_memory_growth.md).
- **Win32 handles are resources too.** `CreateFileW`, `CreateEventW`, `OpenProcess` and
  `RegOpenKeyExW` return handles that must be closed (`CloseHandle`, `RegCloseKey`) on every
  path. Task Manager's "Handles" column growing over hours is the Windows version of an fd leak.
  Note the inconsistent failure values: `CreateFileW` returns `INVALID_HANDLE_VALUE` (-1), while
  `CreateEventW` returns `NULL`.
- **Half-updated state is worse than a leak.** If a function updates two fields of a shared
  structure and fails between them, the next request sees an inconsistent object. Either
  validate everything first and mutate last, or build the new state on the side and swap it in
  at the end. In money-moving code this rule is not optional.
- **Map internal errors to wire errors at the boundary.** A client of the RPC server should get a
  stable error code ("insufficient funds", "invalid account", "internal error, reference
  REQ-81723"), not `strerror` text or a file path. Log the internal detail with the same reference
  so support can join the two.
- **Out of memory is real on Windows.** Linux usually overcommits, so `malloc` rarely returns
  `NULL` and the OOM killer acts later. Windows commits memory at allocation time, so a 32-bit
  or commit-limited process sees `NULL` from `malloc` or `HeapAlloc` under load. Code that
  never checks crashes at a random dereference far from the cause.

## 6. Try it

1. **Easy - spot the bug.** Find three errors in this code and fix them:
   `char *buf = malloc(n); FILE *f = fopen(p, "r"); if (!f) { perror("fopen"); return -1; }
   if (fread(buf, 1, n, f) != n) return -1; fclose(f); return 0;`. Done when your version has no
   leak on any path and reports which call failed.
2. **Easy - errno discipline.** Write `int read_int_file(const char *path, long *out)` that
   returns 0 or a positive errno, uses `strtol` correctly, and leaves `*out` untouched on
   failure. Test it on a missing file, an empty file, `"12abc"` and `"99999999999999999999"`.
   Done when all four return the right code.
3. **Medium - convert a nested function.** Rewrite a function with four nested `if` levels that
   opens two files and allocates two buffers into shape 2 of the goto idiom. Add the fault
   injection wrappers from `m10_goto_cleanup.c`. Done when the injection loop reports N failure
   points and zero leaks.
4. **Medium - extend the error model.** Add `kv_del` and a `KV_READONLY` code to
   `m10_error_model.c`, plus a `kv_set_readonly(kv, 1)`. Done when `kv_put` on a read-only store
   returns `KV_READONLY`, the callback prints a contextual message, and `kv_strerror` knows the
   new code.
5. **Hard - LD_PRELOAD fault injection.** Write `failmalloc.c` that defines `malloc` and fails
   the Nth call (read N from an environment variable; call the real one via
   `dlsym(RTLD_NEXT, "malloc")`). Run an unmodified program under it for N = 1..20. Done when you
   can name every N where the program crashes instead of reporting an error.
6. **Hard - Win32 version.** Port `copy_file` from `m10_goto_cleanup.c` to Win32 with
   `CreateFileW`, `ReadFile`, `WriteFile`, `CloseHandle`, returning a `DWORD` from
   `GetLastError()`. Done when it compiles with MSVC `/W4 /WX` and every handle is closed on every
   path.

## 7. Interview questions

**Q: How do you handle errors and cleanup in C without exceptions?**
A: Each function returns a status (0 or an error code, or -1 with `errno`), and callers check
every call. Resources are released through a single cleanup section at the end of the function,
reached with forward `goto`s from each failure point, releasing in reverse order of acquisition.
Ownership of returned resources is documented and transferred explicitly.

**Q: Why is `goto` acceptable for cleanup?**
A: The jumps only go forward to one cleanup block, so control flow stays readable, and the
alternative is either deep nesting or duplicated cleanup code that drifts out of sync. The Linux
kernel coding style recommends it and CERT C rule MEM12-C describes it. It is the C equivalent
of a destructor running on every exit path.

**Q: What are the pitfalls of `errno`?**
A: It is only meaningful immediately after a call that reported failure; successful calls may
change it and nothing resets it to 0; any intervening call, especially logging, can overwrite
it, so save it at once. `strtol` needs `errno = 0` before the call. pthreads and many libraries
return the error number instead of setting `errno`.

**Q: When do you use `assert` and when a runtime check?**
A: `assert` is for conditions that can only fail if the program itself is wrong, such as an
internal invariant or a precondition between your own functions; it is removed with `NDEBUG`, so
it must have no side effects. Anything that depends on input, files, network or resources gets a
runtime check that returns an error, in every build.

**Q: How would you design the error model for a C library?**
A: Return `int` from every function, 0 for success, positive values for passed-through errno
codes and negative library-specific codes in a reserved range, like Berkeley DB. Provide one
`lib_strerror`, keep a detailed per-handle message and an optional error callback, treat
expected outcomes such as "not found" as quiet return values, write out-parameters only on
success, and never call `exit` or `abort` from the library.

**Q: What goes wrong with `p = realloc(p, n)`?**
A: If `realloc` fails it returns `NULL` and leaves the old block allocated, so the assignment
loses the only pointer to it: a leak that only happens under memory pressure. Assign to a
temporary, check it, then update `p`.

**Q: How do you test error paths that almost never run?**
A: Fault injection: wrap allocation and I/O functions so the Nth call fails, count the calls on
a successful run, then rerun once per call with that call failing and assert no leaks, no open
handles and a correct error code. Without source changes, an `LD_PRELOAD` failing `malloc` or
Windows Application Verifier can do the same.

## 8. Recap

- C reports errors through return values; pick one convention per module and document it.
- `errno` is valid only right after a reported failure; save it before calling anything else,
  set it to 0 before `strtol`; pthreads return the error number instead.
- The goto-cleanup idiom releases resources in reverse order from one place; shape 2
  (NULL-initialised resources, one label) is the easiest to keep correct.
- Transfer ownership with `*out = t; t = NULL;`; write out-parameters only on success; release
  functions should accept NULL.
- Never `p = realloc(p, n)`; never put work inside `assert`.
- `assert` for program bugs, runtime checks for input and environment; `_Static_assert` for
  layout assumptions.
- Log once, at the layer that decides, with what/which/code/where/request id.
- Berkeley DB model: 0 ok, positive errno passthrough, negative library codes, one strerror,
  "not found" is not an error, an error callback for detail.
- Fault injection turns rare error paths into tested paths; `m10_goto_cleanup.c` exercises 9
  failure points.
- C++ RAII removes the cleanup code, but C conventions return at every C API boundary.
