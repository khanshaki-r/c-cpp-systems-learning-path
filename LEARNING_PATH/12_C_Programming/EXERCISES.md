# 12_C_Programming — all exercises

Every exercise from section 6 ("Try it") of modules 01 to 16, in one place: 96 exercises, each with
a difficulty, a short "done when" and a hint. The full statement (exact inputs, file names, flags)
is in the module; it is shortened here so you can tick exercises off without opening the module.

How to use this file:
- Do every **easy** exercise as you finish each module. Most take 10 to 30 minutes.
- Do at least two **medium** exercises per module. Writing the code yourself is where the rules
  stop being things you have read and become things you can predict.
- Pick the **hard** exercises that back a resume line or an interview story (see the minimum path
  at the end). Each one is something you can talk about for five minutes in an interview.
- Read a hint only after ten minutes of being stuck. Hints point at the mechanism; they do not give
  the answer.
- Work on a copy in the Linux file system (`~/dev/c12`, see README.md "Building the code"). Build
  every program twice: `gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g`, and again with
  `-fsanitize=address,undefined`. For Valgrind runs (3.3, 4.3, 10.3) build **without** sanitizers:
  ASan and Valgrind do not work together.
- Windows exercises (1.6, 10.6, 13.6, 14.6, and the MSVC branches of 7.3 and 8.5) need Visual
  Studio's "Developer Command Prompt". Build with `cl /std:c17 /W4 /WX`.

TSan note for WSL Ubuntu 24.04 (exercises 6.5, 11.6, 12.6 and module 14): if a TSan binary dies
immediately with `FATAL: ThreadSanitizer: unexpected memory mapping`, the kernel's ASLR entropy is
higher than TSan expects. Run it as `setarch $(uname -m) -R ./prog`, or once per boot
`sudo sysctl vm.mmap_rnd_bits=28`. This is an environment issue, not a bug in your code.

Legend: **E** = easy, **M** = medium, **H** = hard.

---

## Module 01 — How C really works (`01_how_c_really_works.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 1.1 | E | Split-build `m01_two_units.c` (`-DUNIT_A`, `-DUNIT_B`) and run `nm` on both objects. | You can name, for every `nm` line, the source line that produced it and why its letter is upper or lower case. | The letter gives the section (text, data, bss, undefined) and the case gives the linkage. Check which names carry `static`, and notice that a zero initialiser and a non-zero one land in different sections. |
| 1.2 | E | Make `s_next` non-`static` in unit A, add `int s_next = 5;` to unit B, rebuild split. | You have the `multiple definition` error, can explain it, and have fixed it two ways. | Both objects now export the same initialised symbol. Only the linker sees both objects, and either side can withdraw from external linkage. |
| 1.3 | M | In unit B only, declare `extern long g_requests;` (inside `#ifdef UNIT_B`) and build split. | It links with no error, and you can explain why the linker cannot notice and what `main` now reads. | Keep the shared `extern int` declaration out of unit B, or the compiler sees both and refuses. Then ask what an object file records about a symbol besides its name, and how many bytes a `long` load reads. |
| 1.4 | M | `gcc -std=c17 -save-temps -c m01_eval_order.c`; find your code in the `.i` and the call order of `note('x')`, `note('y')`, `note('z')` in the `.s`. | You can point at the instructions that prove the right-to-left order. | Your code is at the very bottom of the `.i`, after the expanded headers. In the `.s`, the characters appear as `$120`, `$121`, `$122` (ASCII `x`, `y`, `z`) loaded into `%edi` before each `call note`. |
| 1.5 | H | Call `char *make(void)` (returns `malloc(16)`) from another file with **no prototype**; build with `gcc -std=c89 -w`; then add the prototype. | You have seen the crash or truncated pointer, can explain `eax` versus `rax`, and the prototype fixes it. | Without a prototype C89 assumes the function returns `int`. Compare the instruction right after `call make` in the two `-S` listings: one of them keeps only 32 bits. |
| 1.6 | H | Build `m01_compiler_and_standard.c` with `cl /std:c17` and with plain `cl`. | Both outputs side by side, and every difference from the GCC output explained. | Without `/std`, MSVC compiles its older default C dialect. Compare which standard macros (`__STDC__`, `__STDC_VERSION__`) each build defines, then `sizeof(long)`. |

## Module 02 — Types, integers and conversions (`02_types_integers_and_conversions.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 2.1 | E | Predict the full output of `m02_data_model.c` on 64-bit Windows without running it. | You have the size table and the data-model line, and can name the one line that differs from Linux. | Look up what the letters in "LP64" and "LLP64" stand for: the name lists exactly which types are 64 bits. |
| 2.2 | E | Predict all nine lines of `m02_promotions_and_conversions.c`, then run it. | Every wrong prediction is explained with the rule from section 2.3 or 2.4. | Two steps, in order, for each expression: promote anything narrower than `int`, then apply the usual arithmetic conversions. When a signed and an unsigned type of the same rank meet, ask which one changes. |
| 2.3 | M | `bool mul_i64_checked(int64_t a, int64_t b, int64_t *out)` without builtins, covering all sign combinations and `INT64_MIN * -1`. | 100,000 random pairs plus the edge values (0, 1, -1, `INT64_MAX`, `INT64_MIN`) agree with `__builtin_mul_overflow`. | Split by the signs of `a` and `b`, and compare against a limit divided by `b` before multiplying. Handle `b == 0` and `b == -1` first: dividing by them is undefined or overflows itself. |
| 2.4 | M | Three correct "visit `n-1` down to 0" loops with a `size_t` index, and one deliberately broken one. | The broken one triggers `-Wtype-limits`; the three correct ones touch nothing when `n == 0`. | An unsigned value is always `>= 0`. Test before you decrement, or count something that can legitimately reach zero. `-Wtype-limits` is part of `-Wextra`. |
| 2.5 | M | Monthly compound interest on 1,00,000.00 rupees at 9 % a year for 12 months, rounded to the paisa monthly: once with `double`, once with `int64_t` paise and half-even rounding. | Both results printed side by side, any difference explained, and month 1 of the integer version matches a hand calculation. | 9 % a year is exactly 75 basis points a month. Multiply first, then divide by 10,000 and round the remainder yourself. Month 1 divides exactly; month 2 is the first month where the rounding rule matters. |
| 2.6 | H | `load_be64`, `store_be64` and a parser for a 16-byte big-endian header (`u32 magic, u16 version, u16 opcode, u64 length`). | The `-O2 -S` output shows `bswap` (or `movbe`), and 1000 random headers round-trip. | Cast each byte to the wide type **before** shifting: shifting a promoted `int` by 32 or more is undefined. Search the assembly for `bswap`. |

## Module 03 — Pointers from the ground up (`03_pointers_from_the_ground_up.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 3.1 | E | Draw memory for `int x = 5; int *p = &x; int **pp = &p;` with invented addresses, then check in gdb. | Your drawing matches `p &x`, `p p`, `p &p`, `p *pp`, `p **pp` and `x/4xb &x` (apart from the real address values). | Every variable is a box with an address and contents; a pointer's contents is another box's address. `x/4xb` shows the int's bytes lowest address first. |
| 3.2 | E | Predict `sizeof` of `int a[5]`, `&a`, `*&a`, `a + 0`, `"abc"`, `(char *)"abc"`, `a[0]` and `struct { char c; double d; }`. | All eight right, or every miss explained. | For each operand ask: is it still an array (arrays do not decay under `sizeof`) or already a pointer? A string literal is an array that includes its NUL. |
| 3.3 | M | `remove_all` twice: with a `prev` pointer and an explicit head case, and with the pointer-to-pointer technique. | Both pass the head, middle, tail, all-nodes and empty-list tests, and Valgrind reports no leaks. | In the second version, walk the links (the head pointer and every `next` field) rather than the nodes; then the head stops being a special case. |
| 3.4 | M | `int **matrix_new(int rows, int cols)` with **one** `malloc`: row pointers followed by the data. | `m[r][c]` works, one `free(m)` releases everything, and the data part is correctly aligned. | Total size = row-pointer array + data, with the pointer-array size rounded up to the data's alignment. Check `rows * cols` for overflow before computing it. |
| 3.5 | M | The extern trap: `a.c` defines `int table[4] = {1, 2, 3, 4};`, `b.c` declares `extern int *table;` and prints `table[0]`. | You have the crash, the faulting address from gdb, and can explain why it is `0x200000001`. | `b.c` believes the symbol's storage holds a pointer, so it loads 8 bytes from there and dereferences them. Write out the first 8 bytes of the array in little-endian order. |
| 3.6 | H | Generic `isort(void *base, size_t n, size_t size, cmp)` with the `qsort` contract, using only `unsigned char *` arithmetic and `memcpy`. | It sorts `int`s, `double`s and a 40-byte struct, and matches `qsort` on 1000 random arrays. | Element `i` starts at `(unsigned char *)base + i * size`. You need one element-sized temporary; shift the sorted part with `memmove`, never through a typed pointer. |

## Module 04 — Memory, storage, scope and lifetime (`04_memory_storage_scope_and_lifetime.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 4.1 | E | Annotate every object in `m04_storage_durations.c` with its storage duration, scope and linkage. | No blank cells, and you can say when each object is created and destroyed. | Three independent questions per object: where can the name be used, how long does the object live, and can another file name it? A `static` local answers them in an unusual combination. |
| 4.2 | E | Build `m04_returning_buffers.c` with `-DSHOW_BUG`, then rewrite `format_id_bug` into each of the four correct forms. | Each version passes the existing asserts, and you can name one disadvantage of each. | The four forms differ in who provides the storage. For each, ask: who frees it, is it safe from two threads, and is it safe to call twice inside one `printf`? |
| 4.3 | M | `void *test_realloc(void *p, size_t n)` that fails on the Nth call (N from an environment variable), used in `buf_reserve`. | Two Valgrind reports: the WRONG idiom leaks and leaves `data == NULL`, the RIGHT one does not. | Read N once with `getenv` and `strtol`, count calls in a `static` counter, and return NULL without calling `realloc` when the count reaches N. |
| 4.4 | M | `arena_mark`, `arena_release_to` and an overflow-checked `arena_calloc(a, count, size, align)`. | A test allocates in a nested scope, releases to the mark, and gets the same addresses again. | A mark is just the current offset. The overflow check must happen before `count * size` is computed: compare `count` against `SIZE_MAX / size`. |
| 4.5 | M | Ownership header comments for `cust_parse`, `cust_name`, `cust_free` and `cust_set_name` (which takes ownership of `name`). | Every pointer parameter and return value uses a phrase from the table in section 2.7, including what happens to `name` if `cust_set_name` fails. | For each pointer ask: who frees it, until when is it valid, and does ownership move? The failure case of a "takes ownership" function is the one people forget to write down. |
| 4.6 | H | Recursive function with a 4 KiB local array: find the crash depth on the main thread (8 MiB stack) and in a thread with a 256 KiB `pthread_attr_setstacksize`. | Both measured depths are within 10 % of your calculation, and you can explain the difference. | Build at `-O0`, make sure the array is really used, and print the depth to `stderr` (unbuffered) as you go. Each frame costs more than 4096 bytes, and a thread stack ends in a guard page. |

## Module 05 — Strings and buffers (`05_strings_and_buffers.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 5.1 | E | For `char d[6]`, predict the 6 bytes after `strncpy(d, s, sizeof d)` for `""`, `"abc"`, `"abcde"`, `"abcdef"`, `"abcdefgh"`. | All five predictions match a program that dumps the bytes, and you can say which results are terminated. | Read the contract word by word: `strncpy` always writes exactly `n` bytes, and copies at most `n` bytes from the source. |
| 5.2 | E | Rewrite `sprintf(buf, "%s-%06d-%s", branch, seq, suffix)` as a function on `snprintf` that reports truncation. | A 10-byte buffer reports truncation, a 64-byte buffer succeeds, and `buf` is never left unterminated. | `snprintf` returns the length it wanted to write. Check for a negative return as well as for "too long", and convert carefully before comparing with a `size_t`. |
| 5.3 | M | `int parse_amount_paise(const char *s, int64_t *out)` for `"1234.50"`, `"-0.75"`, `"12"`, with **no floating point**. | A table of at least 15 cases passes, including `"."`, `"1."`, `"-"` and `"9223372036854775807"`. | Parse sign, integer part and fraction separately into an unsigned accumulator. Decide up front what `"1."` and `"1.5"` mean, and check for overflow before every multiply-and-add. |
| 5.4 | M | Non-destructive CSV splitter with quoted fields and doubled quotes. | `a,"b,c","d""e"` gives `a`, `b,c`, `d"e`; 10 cases pass; the input is never written. | Model it as a small state machine (outside quotes, inside quotes, quote seen inside quotes). Return each field as pointer plus length, and unescape into a separate buffer. |
| 5.5 | M | `int utf8_next(const char **s, uint32_t *cp)` that decodes one code point, advances, and rejects every invalid case in section 2.9. | It agrees with `utf8_valid` on 100,000 random byte strings. | Check in a fixed order: lead-byte class, enough bytes left, each continuation byte is `10xxxxxx`, then the decoded value (overlong form, surrogate range, above U+10FFFF). |
| 5.6 | H | A `char buf[16]` overflowed by `strcpy` from `argv[1]`, run with `-O2` (FORTIFY), with `-O0 -fstack-protector-strong`, and with `-fsanitize=address`. | You have all three reports and can explain why FORTIFY stops a `strcpy` into a stack array but not into a heap buffer of unknown size. | Ubuntu's GCC turns `_FORTIFY_SOURCE` on whenever you optimise. FORTIFY swaps `strcpy` for a checking version only when the compiler can work out the destination's size at that call. |

## Module 06 — Structs, unions, enums and layout (`06_structs_unions_enums_and_layout.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 6.1 | E | Predict `sizeof` and every `offsetof` for `{char a; double b; char c;}`, `{char a; char c; double b;}`, `{int a; char b[3];}` and `{short s; struct {char c; int i;} in;}`. | A program using `offsetof` confirms all of them, or you can explain each miss. | Each member goes at the next multiple of its own alignment; the total rounds up to the largest alignment inside; a nested struct brings its own alignment and size. |
| 6.2 | E | Reorder a nine-field struct of mixed sizes for minimum size and lock it with `_Static_assert(sizeof(...) == N, ...)`. | The assert compiles, and fails if you swap two fields back. | Sort by alignment, largest first, then see whether the tail padding can be filled by moving the smallest members. |
| 6.3 | M | `container_of` plus an intrusive doubly linked list (`struct dlink`); one `struct account` in two lists at once (by branch, by status). | Removing it from one list leaves it in the other, with no allocation per list node. | Give the account one `struct dlink` member per list. A circular list with a sentinel head removes every NULL special case. |
| 6.4 | M | Decoder for the 35-byte `loan_rec_v2` record from 07_Databases_Internals/12_migrating_a_kv_store_to_relational.md, by byte offsets only (no packed struct). | Three hand-built golden records decode correctly; a wrong version and a short record are rejected. | Write tiny helpers (`get_le32(p, off)`, `get_be16(p, off)`, a BCD digit pair) and validate length and version before reading any field. Trim the padding only after copying. |
| 6.5 | M | Bit-field race: two threads toggle `unsigned a : 1` and `unsigned b : 1` one million times each; then fix with separate `unsigned char` fields. | You have seen a final state inconsistent with the counts (or a TSan report), and can explain why the fix works. | Look at the assembly for one bit-field toggle: how many bytes are loaded and stored? Then look up "memory location" in the C11 standard (section 3.14). |
| 6.6 | H | Version 2 of the format in `m06_serialize_record.c` with a 3-letter currency code; v1 still decodes (currency "INR"); fuzz `txn_decode` for a minute under ASan and UBSan. | Both versions round-trip in tests and the fuzzer finds nothing. | Check the version before any field and derive the expected length from it. A loop feeding random bytes of random length until a time limit is a sufficient fuzzer; save any input that crashes. |

## Module 07 — Functions, function pointers and callbacks (`07_functions_function_pointers_and_callbacks.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 7.1 | E | Say in English, then rewrite with typedefs: `int (*f[4])(double)`, `char *(*g)(const char *)`, `void (*signal(int, void (*)(int)))(int)`, `int (*(*h)(int))(int)`. | A test program declares each one, assigns a real function to it and calls it. | Start at the name and work outward: postfix `[]` and `()` bind tighter than prefix `*`, and parentheses override that. Typedef the innermost function type first. |
| 7.2 | E | Add `OP_TRANSFER` with a handler to `m07_function_pointers.c`, with tests for opcode 0, `OP_MAX - 1` and `OP_MAX`. | Every unknown opcode gives status 1 and the program never calls through NULL. | Range-check the opcode as an unsigned value before indexing, then check the slot for NULL. A designated initialiser `[OP_TRANSFER] = h_transfer` keeps the table aligned with the enum. |
| 7.3 | M | `sort_ctx(base, n, size, cmp(a, b, ctx), ctx)` on `qsort_r` (glibc), `qsort_s` (MSVC, via a trampoline), and your own merge sort elsewhere. | A sort by a field chosen at run time through `ctx` passes on WSL, and the MSVC branch compiles in your head line by line. | glibc's comparator is `(a, b, ctx)`, MSVC's `qsort_s` comparator is `(ctx, a, b)`, and BSD's `qsort_r` differs again. glibc needs `_GNU_SOURCE` defined before the first `#include`. |
| 7.4 | M | Re-implement the netstring parser with `struct state { struct state (*fn)(struct parser *, unsigned char); }`. | The same chunked and bad-input tests pass, and three sentences compare the two designs. | C cannot directly spell "a function that returns a pointer to a function of its own type"; wrapping the pointer in a struct breaks that recursion. Each state function returns the next state. |
| 7.5 | M | `log_msg(int level, const char *fmt, ...)` with the `format` attribute; trigger and fix the `%d`-for-`int64_t` error and the `-Wformat-security` warning. | The wrapper compiles cleanly with `-Wall -Wextra -Wformat=2 -Werror`. | In `format(printf, M, N)`, M is the position of the format and N of the first variadic argument. Forward with a `va_list` and `vfprintf`; never pass data as the format. |
| 7.6 | H | Event dispatcher: `subscribe(type, cb, ctx)` returns a handle, `unsubscribe(handle)`, `publish(type, data)`; callbacks may unsubscribe themselves or others during `publish`. | A test that unsubscribes during dispatch runs clean under ASan, and you can explain your deferred-removal design. | Never free or shift the subscriber array while iterating it. Mark entries dead and clean up when the **outermost** `publish` returns: publishes can nest. |

## Module 08 — The preprocessor and macros (`08_the_preprocessor_and_macros.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 8.1 | E | `gcc -std=c17 -E -P m08_macro_traps.c`; find the expansion of every macro use in `main`. | You can point at the exact text that makes `MAX_BAD(i++, 3)` increment twice and `BUMP_BOTH_BAD` escape the `if`. | Search the output for `i++` and count the occurrences. For the `if`, add the braces the compiler actually sees and check which statement it controls. |
| 8.2 | E | A `SWAP` macro with an internal `tmp`, broken by `SWAP(tmp, x)`; fix it with `__typeof__` and a safer name, and with a `static inline` function per type. | You can argue which fix you would ship and why. | After textual substitution the caller's `tmp` and the macro's `tmp` are the same name. Weigh type checking, debuggability and portability to MSVC. |
| 8.3 | M | One X-macro opcode table (`X(OP_BALANCE, 1, h_balance, "balance")`) generating the enum, the log names and a `const` dispatch table. | Adding an opcode is one line, and a `_Static_assert` fails if the table and `OP_COUNT` disagree. | Define the list once as `#define OPCODES(X) ...` and expand it three times with different `X` definitions. `sizeof table / sizeof table[0]` gives the count to assert on. |
| 8.4 | M | Logging macro with a monotonic timestamp and the thread id, built into one buffer and emitted with a single `fwrite`. | With `-DLOG_COMPILED_MIN=1`, `strings` finds no debug format strings in the binary. | `clock_gettime(CLOCK_MONOTONIC)` and `gettid()` (glibc 2.30 and later, needs `_GNU_SOURCE`). Compare the level against a compile-time constant inside the macro so the compiler can drop the call and its format string. |
| 8.5 | M | `plat_mutex` (`CRITICAL_SECTION` or `pthread_mutex_t`) and `plat_thread_create` in `m08_platform_conditionals.c`. | The POSIX branch compiles with `-Werror` and passes a two-thread counter test; every Windows call is checked against its documentation. | The two thread entry points have different signatures (`void *(*)(void *)` and `DWORD WINAPI (*)(LPVOID)`). Hide that behind a small heap struct holding your callback and context, passed to a per-platform trampoline. |
| 8.6 | H | `PRINT(x)` with `_Generic` over `int`, `unsigned`, `long`, `unsigned long`, `long long`, `unsigned long long`, `double`, `char *`, `const char *`; then try adding `int64_t`. | It compiles on LP64, you can explain why it would also compile on LLP64, and a test prints one value of each type. | Look up what `int64_t` is a typedef for in glibc's `<stdint.h>` and in MSVC's. `_Generic` rejects two associations with compatible types. |

## Module 09 — Undefined behaviour and the optimiser (`09_undefined_behaviour_and_the_optimizer.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 9.1 | E | Classify `sizeof(long)`; `f(g(), h())` call order; `-1 >> 1`; `INT_MAX + 1`; `(int)3000000000u`; `char *s = "hi"; s[0] = 'H';`. | All six classified as implementation-defined, unspecified or undefined, with one line of reasoning each. | Does the standard require the compiler to document the choice, allow a choice without documentation, or impose no requirement? Converting an out-of-range value is not the same as overflowing during arithmetic. |
| 9.2 | E | Build `m09_overflow_check.c` at `-O0`, `-O2`, `-O2 -fwrapv` and with UBSan; fill in the table from section 3.1 yourself. | Your numbers match, and you can explain the `-O0` result. | Look for an `add` instruction in `naive_add_overflows` in the `-O0` assembly. If it is missing, ask which part of the compiler removed it. |
| 9.3 | M | Assembly for `m09_null_check_deleted.c` at `-O2`, with and without `-fno-delete-null-pointer-checks`. | You can point at the null-check instruction in one listing and say why it is missing from the other. | Look for a `test` of the pointer register followed by a conditional jump. Then ask what the compiler had already learnt about that pointer before the check. |
| 9.4 | M | `uint32_t read_be32(const unsigned char *p)` three ways (pointer cast plus `ntohl`, `memcpy` plus `ntohl`, shifts), each called on `buf + 1`, built with `-O2 -fsanitize=undefined`. | You can say which version UBSan reports and why, and which one is portable without `ntohl`. | `buf + 1` is not 4-byte aligned. Which versions perform a 4-byte load through a `uint32_t *`, and which only ever load bytes? |
| 9.5 | H | `for (int i = 1; i > 0; i += i)` counting iterations, built at `-O0` and `-O2`, each run under `timeout 2`. | You have both outputs (or the timeout) and a three-sentence explanation based on section 2.3. | Ask what the compiler may conclude about `i > 0` when `i` starts positive and only positive values are ever added, given that overflow "cannot happen". |
| 9.6 | H | Build a legacy file (from work, or `m15_legacy_report.c`) with `-O2 -Wall -Wextra -Wstrict-aliasing=2 -fsanitize=undefined` and run its tests. | Every warning and report listed with a one-line verdict: real UB, false positive, or implementation-defined. | Change nothing yet. Run with `UBSAN_OPTIONS=print_stacktrace=1` and record the exact line and the operand values each report prints. |

## Module 10 — Error handling and resource cleanup (`10_error_handling_and_resource_cleanup.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 10.1 | E | Find and fix the three errors in the module's `malloc` + `fopen` + `fread` snippet. | Your version has no leak on any path and reports which call failed. | At every `return`, list what is still allocated or open. Also ask what happens when `malloc` itself fails, and what a short `fread` can mean. |
| 10.2 | E | `int read_int_file(const char *path, long *out)`: returns 0 or a positive errno, uses `strtol` correctly, leaves `*out` untouched on failure. | A missing file, an empty file, `"12abc"` and `"99999999999999999999"` each return the right code. | Save `errno` before any other call, including `fclose` and logging. Pick a code for "no digits" and "trailing junk" (`EINVAL` is the usual choice) and write `*out` as the very last step. |
| 10.3 | M | Rewrite a four-level nested function (two files, two buffers) into the goto-cleanup shape, with the fault-injection wrappers from `m10_goto_cleanup.c`. | The injection loop reports N failure points and zero leaks. | Initialise every resource variable to NULL at the top so one cleanup block can release whatever exists, in reverse order. Run once with no injection to learn N. |
| 10.4 | M | Add `kv_del`, a `KV_READONLY` code and `kv_set_readonly(kv, 1)` to `m10_error_model.c`. | `kv_put` on a read-only store returns `KV_READONLY`, the callback prints a contextual message, and `kv_strerror` knows the new code. | Put the new code in the library's reserved negative range and add it to `kv_strerror` in the same change. Decide whether deleting a missing key is an error or a quiet "not found". |
| 10.5 | H | `failmalloc.c`: an `LD_PRELOAD` `malloc` that fails the Nth call (real one via `dlsym(RTLD_NEXT, "malloc")`); run an unmodified program for N = 1..20. | You can name every N at which the program crashes instead of reporting an error. | Build with `-shared -fPIC` (plus `-ldl` on glibc older than 2.34). `dlsym` can itself allocate on first use, so guard against recursion with a flag and a small static buffer. |
| 10.6 | H | Port `copy_file` from `m10_goto_cleanup.c` to Win32 (`CreateFileW`, `ReadFile`, `WriteFile`, `CloseHandle`), returning a `DWORD` from `GetLastError()`. | It compiles with MSVC `/W4 /WX` and every handle is closed on every path. | `CreateFileW` fails with `INVALID_HANDLE_VALUE`, not NULL, so initialise handles to that. Save `GetLastError()` immediately, before `CloseHandle` can overwrite it. |

## Module 11 — Data structures in C (`11_data_structures_in_c.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 11.1 | E | `vec_shrink_to_fit`, and `vec_pop` halving the capacity when the length drops below a quarter of it. | Push 1000 then pop 990 ends with capacity at most 64, and alternating push and pop then causes no reallocation ping-pong. | Count reallocations in the test. The gap between the grow threshold (full) and the shrink threshold (a quarter) is what prevents thrashing; keep a minimum capacity. |
| 11.2 | E | `slist_reverse` and `slist_middle` for the singly linked list in `m11_lists.c`. | Both pass on lists of length 0, 1, 2 and 5. | Reversal turns one arrow around per step and must remember where it was going next. For the middle, use two pointers moving at different speeds, and decide which node is "the middle" of an even-length list. |
| 11.3 | M | O(1) LRU cache from `m11_hash_table.c` (account id to `struct session *`) plus the intrusive LRU list from `m11_lists.c`. | Every operation is O(1), and a capacity-3 test evicts in the right order. | The hash value points to the object that embeds the list link, so a hit can unlink and re-insert at the head without searching. Eviction must remove the tail from both structures. |
| 11.4 | M | `DEFINE_HEAP(T, name, less)` generating a typed heap; benchmark it against the generic heap on 1,000,000 ints. | You have both timings and can explain the difference with section 4. | Paste names with `##` to generate `static inline` functions per type. Time at `-O2` with `clock_gettime(CLOCK_MONOTONIC)`, and think about indirect calls and byte-wise swaps. |
| 11.5 | H | Replace tombstones in `m11_hash_table.c` with backward-shift deletion. | The churn test passes with zero tombstones and no same-size rebuilds. | Walk forward from the hole until an empty slot. Move an entry back into the hole only if that does not put it before its home slot, and remember that the table wraps around. |
| 11.6 | H | Make the ring buffer safe for one producer and one consumer thread with `_Atomic unsigned` head and tail and release/acquire ordering. | A 10,000,000-item test passes and TSan is clean. | Each index has exactly one writer. For each, decide which slot access its release store must publish, and where the other side's acquire load must go. |

## Module 12 — Memory allocators: arena, pool and malloc (`12_memory_allocators_arena_pool_and_malloc.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 12.1 | E | By hand: `align_up(x, a)` for (1, 8), (8, 8), (9, 16), (100, 64), (0, 32); explain why `align_up(5, 12)` is meaningless. | Your answers match a five-line C program. | Use the formula from section 2.2 and write `a - 1` in binary. What does it look like when `a` is a power of two, and when `a` is 12? |
| 12.2 | E | `arena_realloc(a, p, old, new)`: grow in place when `p` is the most recent allocation and there is room, otherwise allocate and copy. | A test shows the same pointer in the in-place case and correct copying in the other. | `p` is the most recent allocation exactly when `p + old` equals the arena's current bump position. Do not forget the shrink case. |
| 12.3 | M | Switch `tm_malloc` in `m12_teaching_malloc.c` to best fit and rerun the stress and fragmentation tests. | You report the out-of-memory count and the largest free block for both policies and explain the difference. | Best fit scans the whole free list and keeps the smallest block that fits (stop early on an exact fit). Use the same random seed for both runs. |
| 12.4 | M | `tm_realloc`: grow in place by absorbing a free next block (splitting off the excess), shrink in place by splitting, otherwise malloc-copy-free. | `tm_check` passes after every call in a random test. | The next block's header says whether it is free and how big it is. After merging, split off any excess of at least the minimum block size, and update both header and footer. |
| 12.5 | H | ASan-aware `m12_arena.c`: poison each chunk's unused tail and everything released by `arena_rewind` and `arena_reset`. | ASan reports `use-after-poison` on a read after reset, and the normal tests still pass. | `<sanitizer/asan_interface.h>` turns the poisoning macros into no-ops when ASan is off, so the normal build is unaffected. Unpoison exactly the bytes you hand out; poisoning works at 8-byte granularity. |
| 12.6 | H | Per-thread pools through a `_Thread_local` pointer, falling back to a shared, mutex-protected pool when the local one is empty. | A 4-thread test runs clean under TSan and you have measured it against plain `malloc`. | The fast path must touch only thread-local state and take no lock. Decide what happens when thread A frees a block that came from thread B's pool. |

## Module 13 — Files, I/O and POSIX system calls (`13_files_io_and_posix_system_calls.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 13.1 | E | Predict `printf("a\n"); write(1, "b\n", 2); printf("c\n");` on a terminal and piped to `cat`, then run both. | You can explain why the two orders differ, using section 2.2. | For each destination ask when stdio hands its buffer to the kernel; `write` bypasses that buffer entirely. |
| 13.2 | E | `strace -e trace=openat,read,write,close ./m13_binary_format_mmap`. | You can point at the `write` of 93 bytes and explain why no `read` of the file follows. | Add `mmap` to the trace list and look at what follows the `openat` that reads the file back. |
| 13.3 | M | Parent writes 1 MiB through a pipe with `O_NONBLOCK` to a slow child; count partial writes and `EAGAIN`; then add `poll`. | 1 MiB arrives intact and you report both counts. | A Linux pipe holds 64 KiB by default. Wait for `POLLOUT` instead of spinning, and verify the data on the reader side with a pattern or checksum. |
| 13.4 | M | Add a `u32 branch_code` to each record, bump the format to version 2, make the reader accept both. | A v1 file and a v2 file both decode; a v3 file is rejected. | The record size now depends on the version, so read and check the version before computing any offset. Keep a v1 file as a test fixture. |
| 13.5 | H | Crash-test `durable_replace`: alternate contents A and B while another script `kill -9`s it at random; then remove the `fsync` calls. | 1000 kills always leave exactly A or exactly B, and you can explain why the no-`fsync` version may still pass on WSL. | Make A and B large enough to need several `write` calls, and compare the whole content, not just the size. Ask what survives a process kill versus a power cut. |
| 13.6 | H | Port `m13_tcp_echo.c` to Winsock2 (`WSAStartup`, `SOCKET`, `closesocket`, `WSAGetLastError`) and build with MSVC `/W4 /WX`. | The Windows client talks to the Linux server running in WSL. | Link `ws2_32.lib`, compare against `INVALID_SOCKET` and `SOCKET_ERROR` instead of -1, and read errors with `WSAGetLastError`. WSL2 forwards `localhost` ports to Windows by default. |

## Module 14 — Multithreading in C: pthreads and C11 (`14_multithreading_in_c_pthreads_and_c11.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 14.1 | E | Pass `&i` from a `for` loop to four threads that print it; run it, run it under TSan, fix it with an array of ints. | You can explain the original output and TSan's report. | Count how many `i` objects exist, and who writes it while the threads read it. TSan's two stacks name the writer and the reader. |
| 14.2 | E | Lock a default mutex twice from one thread with a 1-second `pthread_mutex_timedlock`; then an `ERRORCHECK` mutex with plain `lock`. | `ETIMEDOUT` in the first case and `EDEADLK` in the second, printed with `strerror`. | pthread functions return the error number instead of setting `errno`. The timeout is an absolute `CLOCK_REALTIME` time, and the mutex type is set with `pthread_mutexattr_settype`. |
| 14.3 | M | Change one `while` to `if` in `m14_bounded_queue.c`; run the test 200 times, then under TSan. | You catch a failure, or can explain why it is still wrong even though you did not. | The bug needs two waiters woken for one item, or a spurious wakeup, so a light test may never show it. Write the interleaving down instead of relying on luck. |
| 14.4 | M | Two priority levels in `m14_thread_pool.c` (two queues; workers always drain the high one first). | Under a slow low-priority flood, high-priority tasks still finish quickly, and TSan is clean. | Keep one mutex and one condition ("either queue non-empty, or stopping"), and check the high queue first under that lock. Measure latency from submit to start. |
| 14.5 | H | Benchmark the rate table from `m14_pthreads_basics.c` with a rwlock and with a mutex: 1, 2, 4, 8 readers; 10 ns and 1 microsecond critical sections. | A table of results, and an explanation of where the rwlock starts to win. | Taking a read lock still writes a shared counter, so tiny critical sections can favour the mutex. Calibrate the busy loop against `clock_gettime` rather than guessing an iteration count. |
| 14.6 | H | Port `m14_bounded_queue.c` to Win32 `SRWLOCK` and `CONDITION_VARIABLE`. | It passes the same test on Windows; you have compared it with 04_Concurrency/code/m13_win32_srwlock_condvar.c. | `SleepConditionVariableSRW` takes a timeout (`INFINITE`) and a flags argument (0 for exclusive mode), and still needs the `while` loop. SRWLOCK and CONDITION_VARIABLE need no destroy call. |

## Module 15 — Reading, debugging and changing legacy C (`15_reading_debugging_and_changing_legacy_c.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 15.1 | E | Map a real C project (SQLite's amalgamation, or Redis): find `main`, the event loop, and the function that handles one command. | The call chain from socket read to reply, in ten lines or fewer, with file names. | Start from `main` and find where the event loop starts; then grep the command table for the command's name. In Redis, `processCommand` is a good waypoint. |
| 15.2 | E | `git log -L` on one function and `git log -S` on one string, in any repository you use. | You can name the commit that introduced the current behaviour and the reason given in its message. | `git log -L :function_name:file.c` follows one function through history; `-S` finds the commits that changed how many times a string appears. |
| 15.3 | M | Characterise an untested function: a `--print` mode for 20 inputs including edge cases, turned into a test table. | The test passes, and outputs you think are bugs are marked rather than changed. | Record what the code does today, including outputs that look wrong. Mark those with a comment and a ticket instead of "fixing" the expected value. |
| 15.4 | M | Run cppcheck, clang-tidy and `gcc -fanalyzer` on `m15_legacy_report.c` and on one real legacy file. | A table of every finding with a verdict: real bug, risky but intended, or false positive. | Give clang-tidy the same flags as the build (a `compile_commands.json`, or `--` followed by the flags). Record tool, location, check name and verdict for each row. |
| 15.5 | H | Test a function that calls `send` in a loop without a network: link with `-Wl,--wrap=send` and a scripted `__wrap_send`. | The test proves the function handles partial sends and `EINTR`. | With `--wrap=send`, calls to `send` from your objects go to `__wrap_send`, and `__real_send` reaches the real one. Drive the wrapper from a static array of planned return values and `errno`s. |
| 15.6 | H | Three STAR stories from real work: a crash or memory bug, a concurrency bug, a change that needed backward compatibility. | Each fits in two minutes spoken, uses only numbers you can defend, and a listener can repeat the root cause after hearing it once. | Keep Situation and Task to two sentences. Spend most of the time on how you found the root cause, and end with the result and what now prevents a recurrence. |

## Module 16 — Modern C and the C interview playbook (`16_modern_c_and_the_c_interview_playbook.md`)

| # | Diff | Exercise | Done when | Hint |
|---|------|----------|-----------|------|
| 16.1 | E | On paper, no references: `strlen`, `strcmp`, `memmove`, `reverse_iter`, `is_power_of_two`; then type them in and run the tests. | All tests pass on the first compile, or you know exactly which edge case you missed. | Say the edge cases before writing: the empty string, bytes above 127, both overlap directions, the empty list, zero. |
| 16.2 | E | Predict: `sizeof("abc")` and `strlen("abc")`; `-1 < 1u`; `c + c` for `unsigned char c = 200`; `&a[4] - &a[0]` for `int a[5]`. | Every prediction matches and you can give the rule behind each. | One rule each: what `sizeof` sees for a string literal, the usual arithmetic conversions, integer promotion, and the unit of pointer subtraction. |
| 16.3 | M | `parse_amount("1234.56", &paise)` without `strtod`: optional sign, digits, optional point with exactly two digits; reject everything else, including `long long` overflow. | A 20-input table test passes under UBSan. | Accumulate the magnitude in `unsigned long long` and check before each `acc * 10 + d`. The negative limit's magnitude is one larger than the positive limit. |
| 16.4 | M | Rewrite `m16_modern_c_features.c` for `-std=c2x`: `bool`, `nullptr`, `constexpr`, `typeof`, `[[nodiscard]]`, `static_assert` without a message. | It builds with GCC 13 `-std=c2x -Wall -Wextra -Wpedantic`, and you have listed which C23 features GCC 13 lacks. | Ignore a `[[nodiscard]]` result on purpose and read the warning. For missing features, try `#include <stdckdint.h>` and `#embed`, and read the GCC 13 release notes. |
| 16.5 | H | Word-at-a-time `strlen`: byte by byte to an 8-byte boundary, then `uint64_t` words read with `memcpy` and the zero-byte bit trick. | It matches `strlen` for every length 0 to 100 at every start alignment 0 to 7, and you can explain why the C version must not over-read while glibc's assembly may. | Look up the "has zero byte" trick (subtract 1 from every byte, then test the high bits). Then ask whether an aligned 8-byte load can ever cross a page boundary. |
| 16.6 | H | Mock interview: five questions from section 2.3 and three coding questions from 2.4, 45 minutes, plain editor, no compiling. | Done twice, with every edge case you forgot to say out loud written down. | State the contract and the edge cases before coding, and walk one normal and two edge inputs through by hand at the end. |

---

## Suggested minimum path if you are short of time

C interview rounds (module 16, section 2.5) are built from three things: "what does this print"
questions, small functions written without a compiler, and a discussion of a real bug. Spend your
time in that order.

1. **Prediction drills (about two hours in total).** 2.2, 3.2, 5.1, 6.1, 9.1, 13.1, 16.2. These
   are exactly the snippet questions of written tests and language rounds: promotions, `sizeof`,
   `strncpy`, padding, UB classes, buffering.
2. **Functions you must be able to write on paper (about four hours).** 16.1, 5.2, 10.2, 2.3, 3.3,
   11.2, 6.3. Between them: `strlen`/`strcmp`/`memmove`, a truncation-safe copy, `strtol` with
   `errno` checked properly, overflow-checked arithmetic, pointer-to-pointer list edits, list
   reversal and `container_of`.
3. **Robust C you will be asked to reason about (about four hours).** 4.3 (the realloc leak, proven
   with Valgrind), 10.3 (goto cleanup with fault injection), 9.3 (the deleted null check), 8.3
   (X-macro dispatch table), 14.1 and 14.3 (the two classic pthread bugs), 13.3 (partial writes).
4. **One hard exercise per resume line.** 6.4 for the migration engine (decoding packed records),
   12.5 for "arena allocator with explicit ownership contracts", 15.6 for "memory corruption & leak
   investigation" and the behavioural round, and 16.6 (twice) in the last week before interviews.

If you have a single evening before a C round: 16.2, 16.1, 2.2, 3.2, 5.2 and 10.2, then rehearse
the stories from 15.6 aloud.
