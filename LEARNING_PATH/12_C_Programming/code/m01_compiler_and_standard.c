/* m01_compiler_and_standard.c - which C standard and which compiler is this code REALLY built with?
 *
 * Module: 12_C_Programming/01_how_c_really_works.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m01_compiler_and_standard.c -o m01_compiler_and_standard
 * Then rebuild with -std=c99, -std=c11, -std=gnu17 and -std=c2x and compare the first line.
 * (With -std=c99 the final assert fires on purpose: this course assumes C17.)
 */
#include <assert.h>
#include <stdio.h>

static const char *std_name(long v)
{
    switch (v) {
    case 0L:      return "C89/C90 (no __STDC_VERSION__)";
    case 199409L: return "C95 (C90 + Amendment 1)";
    case 199901L: return "C99";
    case 201112L: return "C11";
    case 201710L: return "C17";
    case 202000L: return "C2x draft (what GCC 13 reports for -std=c2x)";
    case 202311L: return "C23";
    default:      return "unknown";
    }
}

int main(void)
{
#if defined(__STDC_VERSION__)
    long v = __STDC_VERSION__;
#else
    long v = 0L;                 /* C90 did not define the macro at all */
#endif
    printf("__STDC_VERSION__ = %ldL -> %s\n", v, std_name(v));
    printf("__STDC_HOSTED__  = %d (1 = full standard library available)\n", __STDC_HOSTED__);

#if defined(_MSC_VER)
    printf("compiler: MSVC, _MSC_VER = %d\n", _MSC_VER);
#elif defined(__clang__)
    printf("compiler: clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    printf("compiler: gcc %d.%d\n", __GNUC__, __GNUC_MINOR__);
#else
    printf("compiler: unknown\n");
#endif

    /* Optional features: C11 made these optional and gave each a "NOT available" macro. */
#ifdef __STDC_NO_VLA__
    puts("variable length arrays: NOT supported");
#else
    puts("variable length arrays: supported (optional since C11; MSVC never had them)");
#endif
#ifdef __STDC_NO_THREADS__
    puts("<threads.h>: NOT available");
#else
    puts("<threads.h>: available");
#endif
#ifdef __STDC_NO_ATOMICS__
    puts("<stdatomic.h>: NOT available");
#else
    puts("<stdatomic.h>: available");
#endif
#ifdef __STDC_IEC_559__
    puts("double is IEEE 754 binary64 (__STDC_IEC_559__ defined)");
#endif

    assert(v >= 201710L);        /* the whole course is written against C17 */
    return 0;
}
