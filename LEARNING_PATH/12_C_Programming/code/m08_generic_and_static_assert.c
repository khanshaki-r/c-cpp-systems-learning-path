/* m08_generic_and_static_assert.c - _Generic (compile-time type switch) and _Static_assert.
 *
 * Module: 12_C_Programming/08_the_preprocessor_and_macros.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m08_generic_and_static_assert.c -o m08_generic_and_static_assert
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* _Generic picks an expression by the TYPE of its controlling expression. Nothing is evaluated
 * except the chosen branch, and the controlling expression itself is never evaluated. */
#define TYPE_NAME(x) _Generic((x),                                   \
    _Bool: "_Bool",                                                  \
    char: "char", signed char: "signed char", unsigned char: "unsigned char", \
    short: "short", unsigned short: "unsigned short",                \
    int: "int", unsigned int: "unsigned int",                        \
    long: "long", unsigned long: "unsigned long",                    \
    long long: "long long", unsigned long long: "unsigned long long",\
    float: "float", double: "double", long double: "long double",    \
    char *: "char *", const char *: "const char *",                  \
    default: "something else")

/* Type-generic max without double evaluation: dispatch to a real function. */
static int    max_i(int a, int b)       { return a > b ? a : b; }
static long   max_l(long a, long b)     { return a > b ? a : b; }
static double max_d(double a, double b) { return a > b ? a : b; }
#define MAX(a, b) _Generic((a) + (b), int: max_i, long: max_l, double: max_d)((a), (b))

/* A wire header whose layout other systems depend on. If someone edits it, the BUILD fails. */
struct wire_header {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t length;
};
_Static_assert(sizeof(struct wire_header) == 12, "wire_header must be exactly 12 bytes");
_Static_assert(offsetof(struct wire_header, length) == 8, "length must be at offset 8");
_Static_assert(sizeof(int64_t) == 8, "need a 64-bit integer type for money");

#define PAGE_SIZE 4096
_Static_assert((PAGE_SIZE & (PAGE_SIZE - 1)) == 0, "PAGE_SIZE must be a power of two");

/* <assert.h> in C11 also offers the lower-case spelling static_assert (a keyword in C23). */
static_assert(sizeof(void *) == 8, "this build assumes a 64-bit target");

int main(void)
{
    uint8_t small = 1;
    printf("TYPE_NAME('a')                  = %s   (character constants are int in C, char in C++)\n", TYPE_NAME('a'));
    printf("TYPE_NAME(sizeof(int))          = %s   (size_t on LP64)\n", TYPE_NAME(sizeof(int)));
    printf("TYPE_NAME(small + small)        = %s   (uint8_t promoted)\n", TYPE_NAME(small + small));
    printf("TYPE_NAME(1.0f * 2)             = %s\n", TYPE_NAME(1.0f * 2));
    printf("TYPE_NAME(\"abc\")                = %s\n", TYPE_NAME("abc"));
    printf("TYPE_NAME(1u + 1L)              = %s\n", TYPE_NAME(1u + 1L));
    printf("TYPE_NAME((struct wire_header){0}) = %s\n", TYPE_NAME((struct wire_header){0}));

    int i = 3;
    int r = MAX(i++, 2);                  /* i++ runs once: the function receives values */
    assert(r == 3 && i == 4);             /* never put side effects inside assert(): NDEBUG removes them */
    assert(MAX(2.5, 1.0) == 2.5);
    assert(MAX(10L, 20L) == 20L);
    printf("MAX(i++, 2) evaluated i++ once; MAX(2.5, 1.0) = %.1f; MAX(10L, 20L) = %ld\n",
           MAX(2.5, 1.0), MAX(10L, 20L));
    return 0;
}
