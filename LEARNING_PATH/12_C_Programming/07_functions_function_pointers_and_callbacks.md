# 07. Functions, function pointers and callbacks

**Time:** ~2.5 hours   **Prerequisites:** 03, 04

## 1. In plain words

When you call a C function, C **copies** every argument into the function. The function works on
its own copies. If you want a function to change something of yours, you give it the *address* of
that thing (module 03). That is the whole story of "pass by value" in C, and it explains
out-parameters, the "return a status code, deliver results through pointers" style of every C API
you have used (Win32, POSIX, Berkeley DB), and why passing a big struct by value is a quiet
performance bug.

A **function pointer** is a phone number for a function. You can write it in a table, hand it to
someone else, and they can call it later. Handing someone your number so they can call you when
something happens is a **callback**. Because the caller does not know anything about you, you also
hand over a note saying who you are and what you were doing: that is the `void *` **context**
argument you see in `CreateThread`, `pthread_create`, `qsort_r` and every event library.

Analogy: the reception desk of your bank's head office. A directory maps request types to extension
numbers (a **dispatch table**: opcode to handler function). The receptionist does not know how to
handle a loan query; she looks up the extension and transfers the call. Your RPC server does exactly
this with every request it reads from a socket. This module covers that machinery, state machines
built from function pointers, `qsort`/`bsearch`, variadic functions such as `printf`, and the
format-string vulnerability that comes with them.

## 2. The real thing

### 2.1 Pass by value, always

- Scalars and pointers: the value is copied. Changing the parameter changes the copy.
- Arrays: cannot be passed; the array decays to a pointer to its first element, and the pointer is
  copied. So the callee *can* change the caller's elements, which makes arrays look like "by
  reference". They are not; the pointer was passed by value.
- Structs: the **whole struct** is copied. A 4000-byte struct passed by value copies 4000 bytes per
  call (section 3.1). Pass large structs as `const struct x *`; return small structs by value freely
  (it is cheap and clear, see module 06 section 4 for the ABI).

### 2.2 Out-parameters and the status-code convention

The dominant shape of a C API: the **return value is a status**, results go out through pointers.

```c
/* Contract: *new_balance is written only when the result is ST_OK. */
static status withdraw(struct account *acct, int64_t paise, int64_t *new_balance);
```

Decide, and document, one of two policies for out-parameters on failure: "untouched" (as above) or
"always set to a defined value (0, NULL) at entry". Callers that read an out-parameter after a
failure are reading garbage under the first policy, so the comment matters.

The conventions you meet in practice:

| API family | Success / failure | Where the error detail is |
|---|---|---|
| POSIX system calls | `-1` (or NULL) on failure | `errno` (thread-local) |
| pthreads | `0` on success | the **return value** is the error number; `errno` is not set |
| Win32 | `BOOL` FALSE or invalid `HANDLE` | `GetLastError()` (thread-local) |
| COM | `HRESULT` (`SUCCEEDED(hr)`) | the value itself |
| Berkeley DB | `0` on success | return value (`DB_NOTFOUND`, ...), `db_strerror()` |
| your own | an `enum` status | the value, plus a log line |

10_error_handling_and_resource_cleanup.md is dedicated to error handling, `errno` and cleanup paths.

### 2.3 Prototypes and `()` versus `(void)`

In C17, `int f();` declares a function with **unspecified** parameters, not "no parameters". The
compiler checks nothing at call sites. This compiles cleanly with `-Wall -Wextra`:

```c
int f();
int g(void) { return f(1, 2, 3); }   /* no diagnostic */
int f(int a) { return a; }           /* the real definition takes one int: undefined behaviour */
```

Always write `int f(void)` for "no parameters", and `-Wstrict-prototypes` will find the old style.
C23 finally changes `()` to mean `(void)`.

### 2.4 Function pointer syntax

```c
int add(int a, int b);

int (*op)(int, int) = add;     /* op is a pointer to a function (int, int) returning int */
int *op2(int, int);            /* WITHOUT the parentheses: a function returning int *     */

op = &add;                     /* & is optional: a function name decays to a pointer */
int r1 = op(2, 3);             /* call */
int r2 = (*op)(2, 3);          /* identical */
```

Typedefs make everything else readable. Two styles, both common:

```c
typedef int (*binop_fn)(int, int);    /* a pointer type:   binop_fn op;   */
typedef int binop(int, int);          /* a function type:  binop *op;     */
```

Reading hard declarations: start at the name, go right while you can, then left, and jump out of
each parenthesis level. The classic test is the standard `signal` function:

```c
void (*signal(int sig, void (*func)(int)))(int);
/* signal is a function taking (int, pointer to function(int) returning void)
   that returns a pointer to function(int) returning void. With a typedef:   */
typedef void (*sig_handler)(int);
sig_handler signal(int sig, sig_handler func);
```

A function pointer can be NULL; calling a NULL function pointer jumps to address 0 and faults with
the **instruction pointer** at 0 (in a crash dump: `rip = 0`, often with a garbage stack trace,
because the call pushed a return address and nothing else ran). ISO C does not allow converting
function pointers to `void *` (module 03); converting between different function pointer types is
allowed, but **calling** through the wrong type is undefined.

### 2.5 Callbacks with a `void *` context

A callback needs state. Globals work until there are two users or two threads. The C solution is a
`void *` that the registering code passes in and the library hands back untouched:

```c
typedef void (*txn_visitor)(const struct txn *t, void *ctx);

static void for_each_txn(const struct txn *arr, size_t n, txn_visitor fn, void *ctx)
{
    for (size_t i = 0; i < n; ++i) fn(&arr[i], ctx);
}

static void accumulate(const struct txn *t, void *ctx)
{
    struct totals *tot = ctx;    /* the caller promised ctx is a struct totals * */
    ...
}

for_each_txn(book, n, accumulate, &tot);
```

The price of `void *` is type safety: nothing checks that `ctx` really is a `struct totals *`. Keep
the registering call and the callback next to each other in the code, and remember the lifetime rule
from module 04: the context must outlive every call to the callback.

`qsort` has **no** context parameter, which is why every platform added one, incompatibly:

| Function | Comparator signature | Where |
|---|---|---|
| `qsort_r(base, n, size, cmp, arg)` | `int cmp(const void *a, const void *b, void *arg)` | glibc |
| `qsort_r(base, n, size, arg, cmp)` | `int cmp(void *arg, const void *a, const void *b)` | BSD, macOS |
| `qsort_s(base, n, size, cmp, ctx)` | `int cmp(void *ctx, const void *a, const void *b)` | MSVC |
| `qsort_s(base, n, size, cmp, ctx)` | `int cmp(const void *a, const void *b, void *ctx)` | C11 Annex K (not in glibc) |

Portable code wraps one of them behind an `#ifdef` (module 08), or ships its own sort.

### 2.6 `qsort` and `bsearch`

The comparator contract: return a negative value, zero or a positive value, and be a **consistent
total order** (if a < b and b < c then a < c; the same pair always gives the same answer). Breaking
it is undefined behaviour for `qsort`, not just "a wrong order": in 2024 a memory-corruption bug was
reported in glibc's `qsort` that an inconsistent comparator could trigger.

The classic broken comparator subtracts:

```c
return (int)(x->amount_paise - y->amount_paise);   /* WRONG: truncates to int, can flip sign;
                                                      with int fields the subtraction itself can overflow */
return (x->amount_paise > y->amount_paise) - (x->amount_paise < y->amount_paise);   /* RIGHT */
```

Other rules:

- `qsort` is **not stable**: equal elements may change their relative order. Sort by a tie-breaker
  key (an id) if order matters.
- The comparator receives **pointers to elements**. For an array of `char *`, each argument is a
  `char **`; passing them straight to `strcmp` compares pointer bytes as text.
- `bsearch(key, base, n, size, cmp)` needs the array sorted by the same key, calls
  `cmp(key, element)` (key first), and the key need not have the element's type: search a
  `struct txn[]` with a `uint32_t` id.

### 2.7 Dispatch tables

An array of function pointers indexed by a small integer replaces a long `switch` and makes the
mapping data instead of code:

```c
typedef void (*handler_fn)(const struct request *req, struct response *resp);

static const handler_fn k_handlers[OP_MAX] = {
    [OP_BALANCE] = h_balance,
    [OP_DEPOSIT] = h_deposit,
    [OP_WITHDRAW] = h_withdraw,
};

static void dispatch(const struct request *req, struct response *resp)
{
    if (req->opcode >= OP_MAX || k_handlers[req->opcode] == NULL) {   /* from the network: check */
        resp->status = 1;
        snprintf(resp->text, sizeof resp->text, "unknown opcode %u", (unsigned)req->opcode);
        return;
    }
    k_handlers[req->opcode](req, resp);
}
```

Three rules: **bounds-check** the index (it came from a socket), **NULL-check** the entry (gaps in
the table are NULL), and make the table **`const`** so it lives in read-only memory. A writable table
of function pointers is a favourite exploitation target: one heap overflow that reaches it turns into
control of the instruction pointer. For sparse or large opcode spaces use a sorted array of
`{opcode, handler}` and `bsearch`.

### 2.8 State machines with function pointers

A parser that receives bytes from TCP never gets a whole message at once (08_Networking_Distributed/04_framing_and_protocol_design.md).
It must remember where it was between reads: that memory is its **state**. A clean C design gives
each state a handler function and keeps a table from state to handler:

```c
typedef enum { ST_LENGTH, ST_PAYLOAD, ST_COMMA, ST_ERROR, ST_COUNT } state_id;
typedef state_id (*state_fn)(struct parser *p, unsigned char c);

static const state_fn k_states[ST_COUNT] = {
    [ST_LENGTH] = on_length,
    [ST_PAYLOAD] = on_payload,
    [ST_COMMA] = on_comma,
    [ST_ERROR] = on_error,
};

static int parser_feed(struct parser *p, const char *data, size_t n)
{
    for (size_t i = 0; i < n && p->state != ST_ERROR; ++i)
        p->state = k_states[p->state](p, (unsigned char)data[i]);
    return p->state == ST_ERROR ? -1 : 0;
}
```

Each handler consumes one byte and returns the next state. Limits live in the handlers (maximum
length digits, maximum payload), so a hostile peer cannot make the parser allocate or loop without
bound. The error state is **sticky**: once the stream is invalid, the connection must be closed,
because there is no reliable way to find the next message boundary.

An alternative design makes the state itself a function pointer. A function cannot return its own
type in C, so you wrap it: `struct state { struct state (*fn)(struct parser *, unsigned char); };`.
The table-driven version is easier to log ("state 2"), test and extend.

### 2.9 Variadic functions

`printf` takes a variable number of arguments through `<stdarg.h>`:

```c
static int64_t sum_i64(int count, ...)
{
    va_list ap;
    va_start(ap, count);              /* start after the last named parameter */
    int64_t s = 0;
    for (int i = 0; i < count; ++i) s += va_arg(ap, int64_t);   /* you state the type */
    va_end(ap);                       /* every va_start needs a va_end */
    return s;
}
```

The function receives **no information** about how many arguments there are or their types. Caller
and callee must agree by convention: a count (`sum_i64`), a sentinel (a trailing NULL), or a format
string (`printf`). If they disagree, the behaviour is undefined and the compiler cannot tell:

- `sum_i64(3, 100, 250, -50)` passes three `int`s while the function reads three `int64_t`s.
  GCC 13 gives **no warning**. Write `INT64_C(100)` or cast.
- **Default argument promotions** apply to the `...` part: `float` becomes `double`, `char` and
  `short` become `int`. So `va_arg(ap, float)` or `va_arg(ap, char)` is undefined: read `double` or
  `int`.
- A sentinel must be a **pointer**: `NULL` may be the plain integer 0, which is 4 bytes where a
  pointer is 8. Pass `(char *)NULL`. GCC's `__attribute__((sentinel))` checks it.
- A `va_list` can be walked once. To walk twice (measure with `vsnprintf(NULL, 0, ...)`, then
  format), `va_copy` it first.
- To forward arguments, call the `v` version (`vprintf`, `vsnprintf`, `vfprintf`) with your
  `va_list`. You cannot pass `...` on to another variadic function.
- Mark printf-like functions with `__attribute__((format(printf, fmt_index, first_arg_index)))` so
  `-Wformat` checks every call site exactly like `printf`. (MSVC has `_Printf_format_string_` for its
  code analysis instead.)

Prefer non-variadic APIs where you can: an array plus a count is type-checked; `...` is not.

### 2.10 The format-string vulnerability

```c
printf(user_text);          /* WRONG: the data is used as the format */
printf("%s", user_text);    /* RIGHT: the data is an argument */
```

If `user_text` contains conversion specifiers, `printf` reads arguments that were never passed:
`%x` prints stack contents (leaking pointers and data), `%s` dereferences stack garbage (crash), and
`%n` **writes** the count of printed characters to an address taken from the stack, which is how
format-string bugs become arbitrary memory writes. The same applies to `fprintf`, `snprintf`,
`syslog`, and to every logging wrapper you write. Ubuntu's GCC enables `-Wformat-security` by
default, even without `-Wall`:

```
fs.c:2:1: warning: format not a string literal and no format arguments [-Wformat-security]
    2 | void p(const char *user) { printf(user); }
```

### 2.11 `static`, `inline` and `static inline`

- `static` on a function: internal linkage. The compiler knows every call site, so it can inline
  it, specialise it, or delete it if unused. Make every helper `static`.
- `inline` in C99 does **not** mean what it means in C++. A plain `inline` function defined in a
  header is an *inline definition* only; if the compiler decides not to inline a call (for example at
  `-O0`), it emits a call to an **external** symbol that must be defined in exactly one `.c` file
  with `extern inline`. Verified with GCC 13: the same program links at `-O2` and fails at `-O0`:

```
a.c:(.text+0xe): undefined reference to `twice'
collect2: error: ld returned 1 exit status
```

- The simple rule for headers: **`static inline`**. Each translation unit gets its own private copy
  if one is needed; there is nothing to link.
- `_Noreturn` (C11; `[[noreturn]]` in C23) marks functions like a `fatal()` that calls `abort`, which
  helps both warnings and code generation.

## 3. Code

### 3.1 Pass by value and out-parameters: `code/m07_pass_by_value_and_out_params.c`

```
after try_to_change: 1, after change(&n): 99
sum_by_value == sum_by_pointer == 499500 (sizeof(struct big) = 4000 bytes copied by value)
split_amount(123456) -> 1234 rupees 56 paise
withdraw 200.00 -> status 0, new balance 30000
withdraw 999.99 -> status 2 (insufficient funds), out-param untouched: -1
```

`split_amount` returns a small struct by value (two results, no out-parameters), the same idea as the
standard `div()` returning a `div_t`.

### 3.2 Syntax and an RPC dispatch table: `code/m07_function_pointers.c`

```c
/* A function RETURNING a function pointer.
 * Without typedef:  int (*pick(char c))(int, int)
 * With typedef:     binop_fn pick(char c)                                                     */
static binop_fn pick(char c)
{
    switch (c) {
    case '+': return add;
    case '-': return sub;
    case '*': return mul;
    default:  return NULL;
    }
}
```

```
table[i](6, 3) = 9 3 18
opcode   1 -> status 0, "balance of ACC-000042 is 1500.00"
opcode   2 -> status 0, "deposited 250000 paise"
opcode   3 -> status 2, "insufficient funds"
opcode   7 -> status 1, "unknown opcode 7"
opcode 500 -> status 1, "unknown opcode 500"
```

Opcode 7 is inside the table but has no handler (NULL entry); opcode 500 is outside it. Both are
rejected without crashing.

### 3.3 Comparators, `bsearch`, context callbacks: `code/m07_qsort_bsearch_callbacks.c`

```c
/* Sorting an array of char *: the comparator receives POINTERS TO the elements, i.e. char **. */
static int cmp_str(const void *a, const void *b)
{
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}
```

```
cmp_amount_bad(30000000.00, 0) = -1294967296  (negative: claims big < zero)
cmp_amount    (30000000.00, 0) = 1
by amount: -120000 -9900 0 500000 3000000000
bsearch id 105 -> amount -9900
sorted codes: IMPS NEFT RTGS UPI
debits -129900, credits 3000500000, 3 over the 1000.00 limit
```

The first line is the subtraction bug: an amount of 30,000,000.00 rupees (3,000,000,000 paise)
compared with 0 gives a difference that does not fit in an `int`, and the truncated value is
negative. A sort using it would put the largest transaction first.

### 3.4 A table-driven state machine: `code/m07_state_machine.c`

The parser reads netstrings (`<length>:<payload>,`, for example `5:hello,`), fed in deliberately
awkward chunks:

```
fed 5:he                 -> frames so far 0
fed llo,1                -> frames so far 1
fed 2:BAL?ACC-0042,0     -> frames so far 2
fed :,                   -> frames so far 3
payloads: hello|BAL?ACC-0042|
bad input 5:hello;   -> rc -1, error: missing ',' after payload
bad input 99999:x    -> rc -1, error: length has too many digits
bad input 2:ab,x:    -> rc -1, error: bad character in length
```

The second frame's length (`12`) is split across two reads, and the third frame is empty (`0:,`).
`99999:x` is rejected at the fifth digit, before any memory is committed: limits belong in the
parser, not after it.

### 3.5 Variadic functions: `code/m07_variadic_functions.c`

```c
__attribute__((format(printf, 1, 2)))
static char *format_alloc(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);                         /* a va_list can be walked ONCE: copy it first */
    int n = vsnprintf(NULL, 0, fmt, ap);      /* pass 1: measure */
    va_end(ap);
    char *s = NULL;
    if (n >= 0 && (s = malloc((size_t)n + 1)) != NULL)
        vsnprintf(s, (size_t)n + 1, fmt, ap2);  /* pass 2: format */
    va_end(ap2);
    return s;                                 /* caller frees */
}
```

```
sum_i64(3, 100, 250, -50) = 300
join_words -> "NEFT batch 07 settled"
format_alloc -> "acct=ACC-000042 amount=1234.05" (31 bytes allocated)
[INFO] server listening on port 7000
100% done %s %n
[WARN] 100% done %s %n
```

The last two lines print hostile-looking text (`%s %n`) safely, because it is passed as an argument
to `"%s"`.

## 4. Under the hood (senior-level)

### Calling conventions

On Linux x86-64 (System V ABI) the first six integer or pointer arguments travel in `rdi`, `rsi`,
`rdx`, `rcx`, `r8`, `r9`, floating-point ones in `xmm0` to `xmm7`, and the result comes back in
`rax` (or `xmm0`). On Windows x64 the first four go in `rcx`, `rdx`, `r8`, `r9` (or `xmm0` to
`xmm3`), and the caller must reserve 32 bytes of "shadow space" on the stack. 32-bit Windows had
several conventions (`__cdecl`, `__stdcall`, `__fastcall`): a callback declared with the wrong one
(missing `CALLBACK`/`WINAPI`) corrupted the stack pointer on return. On x64 there is one convention,
so that class of bug disappeared. Full detail and how debuggers use it:
05_Memory_Debugging_RCA/02_stack_frames_and_calling_conventions.md.

### Indirect calls

A call through a function pointer compiles to `call *%rax`, and a table dispatch to something like
`call *k_handlers(,%rax,8)`. The CPU predicts the target with its branch target buffer; a dispatch
that always goes to the same few handlers predicts well. Two security features make indirect calls
more expensive or stricter: Spectre v2 mitigations (retpolines, `-mindirect-branch=thunk`) on some
builds, and on Windows **Control Flow Guard** (`/guard:cf`), which checks every indirect call target
against a bitmap of valid function starts. With CFG on, a corrupted function pointer does not jump
into the weeds; the process is terminated with a fast-fail (exception code `0xC0000409`), which is a
clue worth recognising in a dump.

### How `va_list` works

On System V x86-64, a variadic callee cannot know which registers hold its arguments, so its prologue
saves all six integer argument registers (and, if the caller set `al` non-zero, the vector registers)
into a "register save area". `va_list` is a small struct (offsets into that area plus a pointer to
the stack-passed arguments), declared as an array of one struct. That is why you cannot copy a
`va_list` with `=` (use `va_copy`) and why a `va_list` passed to another function is consumed by it.
On Windows x64, `va_list` is just a `char *` walking the stack, because the caller always spills
arguments into the shadow space; code that "worked" by copying `va_list`s on Windows breaks on Linux.

### Why `qsort` is slower than `std::sort`

`qsort` calls the comparator through a pointer for every comparison and moves elements with a
size-generic copy. C++ `std::sort` with a lambda is a template instantiated for the exact type: the
comparison is inlined and elements are moved as their real type. The algorithm is similar; the
abstraction cost is not (02_Modern_CPP/07_stl_algorithms_lambdas_iterators.md). In C, if a sort is
hot, write a type-specific sort or use a macro-generated one.

## 5. In production

- **Unchecked opcode dispatch.** `handlers[msg->opcode](msg)` with an opcode from the wire and no
  bounds check reads a "function pointer" from beyond the table and calls it. In a dump: the
  instruction pointer is garbage or a data address, and the previous frame is the dispatcher. Always
  bounds-check and NULL-check, and keep the table `const`.
- **Callbacks under a lock.** A library calls a user callback while holding its internal mutex; the
  callback calls back into the library, which takes the same mutex: deadlock (or, with a recursive
  lock, corrupted internal state). Rule: release internal locks before invoking callbacks, or
  document that callbacks must not re-enter
  (04_Concurrency/06_deadlocks_livelocks_starvation.md).
- **Dangling callback context.** `register_timer(cb, &local_state)` followed by the function
  returning; the timer fires later and writes into a dead stack frame. Heap-allocate contexts and
  free them on unregister (module 04).
- **Wrong comparator.** Subtraction comparators work in tests with small values and misorder large
  amounts in production. Grep for `return a - b` style comparators.
- **Logging wrappers without the format attribute.** `log_msg("%d", amount_int64)` prints garbage on
  Windows (where `int64_t` is wider than `int`), and `log_msg(customer_name)` is a format-string
  vulnerability. Add `__attribute__((format(printf, ...)))` to every printf-like function and fix
  every warning it produces.
- **Out-parameters read after failure.** A function fails early without writing its out-parameter;
  the caller logs it anyway and prints stack garbage, or uses it as a pointer. Initialise
  out-parameters at the top of the function or initialise the variable at the call site.
- **Win32 thread start routines.** `CreateThread(NULL, 0, worker, ctx, 0, &tid)` follows exactly
  the callback-with-context pattern; the same lifetime and ownership rules apply to `ctx`.

## 6. Try it

1. **(easy)** Write each declaration in English, then rewrite it with typedefs:
   `int (*f[4])(double)`, `char *(*g)(const char *)`, `void (*signal(int, void (*)(int)))(int)`,
   `int (*(*h)(int))(int)`. Done when a test program declares each one, assigns a real function to
   it and calls it.
2. **(easy)** Add `OP_TRANSFER` with a handler to `m07_function_pointers.c` and tests for opcode 0,
   `OP_MAX - 1` and `OP_MAX`. Done when all unknown opcodes produce status 1 and the program never
   calls through NULL.
3. **(medium)** Write `void sort_ctx(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *ctx)`
   that uses `qsort_r` on glibc, `qsort_s` on MSVC (adapting the argument order with a small
   trampoline), and your own merge sort elsewhere. Done when the same test (sort accounts by a
   field chosen at run time through `ctx`) passes on WSL, and the MSVC branch at least compiles in
   your head line by line.
4. **(medium)** Re-implement the netstring parser with the alternative design, where the state is a
   `struct state { struct state (*fn)(struct parser *, unsigned char); }`. Done when it passes the
   same chunked and bad-input tests, and you have written three sentences comparing the two designs.
5. **(medium)** Write `log_msg(int level, const char *fmt, ...)` with the format attribute. Pass an
   `int64_t` with `%d` and read the compile error; fix it with `PRId64`. Then call
   `log_msg(LOG_INFO, user)` and read the `-Wformat-security` warning. Done when your wrapper compiles
   cleanly with `-Wall -Wextra -Wformat=2 -Werror`.
6. **(hard)** Build an event dispatcher: `subscribe(event_type, cb, ctx)` returns a handle,
   `unsubscribe(handle)`, `publish(event_type, data)` calls every subscriber. A callback may
   unsubscribe itself or another subscriber during `publish`. Done when a test that unsubscribes during
   dispatch runs clean under ASan, and you can explain your deferred-removal design.

## 7. Interview questions

**Q: Does C have pass by reference?**
No. Every argument is passed by value; to let a function modify an object you pass its address,
which is itself copied. Arrays look like they are passed by reference only because they decay to a
pointer; structs are copied whole, so large ones should be passed as `const` pointers.

**Q: Declare a pointer to a function that takes an `int` and returns a `char *`, and an array of four of them.**
`char *(*fp)(int);` and `char *(*table[4])(int);`. With a typedef: `typedef char *(*fn_t)(int); fn_t
table[4];`. The parentheses around `*fp` are what make it a pointer to a function rather than a
function returning a pointer.

**Q: Why do C callback APIs take a `void *` context?**
So the callback can reach caller-specific state without globals, which would break with multiple
users or threads. The library stores the pointer and hands it back unchanged; the callback casts it to
the real type. The costs are lost type checking and a lifetime obligation: the context must outlive all
callback invocations.

**Q: What is wrong with `return a - b;` in a `qsort` comparator?**
The subtraction can overflow (undefined for signed `int`) or, for wider types, be truncated to `int`,
flipping the sign, so the comparator is no longer a consistent order. Use
`(a > b) - (a < b)`. An inconsistent comparator is undefined behaviour for `qsort`.

**Q: How does a variadic function know how many arguments it got?**
It does not; the caller and callee must agree through a convention such as a count, a sentinel or a
format string. The callee also states each argument's type in `va_arg`, and default argument
promotions apply (`float` to `double`, `char`/`short` to `int`), so reading the wrong type is
undefined behaviour. The `format` and `sentinel` attributes let GCC check call sites.

**Q: What is a format-string vulnerability?**
Passing untrusted data as the format argument of a printf-family function. Conversion specifiers in
the data make the function read nonexistent arguments: `%x` leaks stack memory, `%s` crashes, `%n`
writes memory. Always pass data as an argument to a literal format such as `"%s"`, and compile with
`-Wformat-security`.

**Q: What should you put in a header: `inline`, `static`, or `static inline`?**
`static inline`. In C99, a plain `inline` definition in a header requires one external definition
elsewhere; when the compiler does not inline a call (often at `-O0`) the link fails with an undefined
reference. `static inline` gives each translation unit its own copy, and unused copies are discarded.

**Q: In C17, what is the difference between `int f();` and `int f(void);`?**
`int f(void)` declares a function with no parameters and the compiler checks calls. `int f();` is an
old-style declaration with unspecified parameters, so calls with any arguments compile without a
diagnostic and a mismatch with the definition is undefined behaviour. C23 makes `()` mean `(void)`.

## 8. Recap

- C passes everything by value; pass addresses to modify, `const` pointers for large read-only
  structs, return small structs by value.
- Return a status and deliver results through out-parameters; document what out-parameters hold on
  failure.
- `int f(void)`, never `int f()`, before C23.
- `int (*fp)(int, int)`: parentheses make it a pointer; typedefs make the rest readable.
- Callbacks take a `void *` context; it must outlive every callback call. `qsort_r`/`qsort_s` differ
  per platform.
- Comparators: consistent total order, `(a > b) - (a < b)`, element pointers (not elements) as
  arguments; `qsort` is not stable.
- Dispatch tables: `const`, bounds-checked, NULL-checked.
- State machines: one handler per state, limits inside the handlers, sticky error state.
- Variadic: no count or types are passed; promotions apply; `(char *)NULL` sentinels; `va_copy` to
  walk twice; `format` attribute on printf-like functions.
- Never pass data as a format string. Use `static inline` in headers.
