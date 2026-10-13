/* m06_unions_and_punning.c - tagged unions, and reading an object's bytes as another type safely.
 *
 * Module: 12_C_Programming/06_structs_unions_enums_and_layout.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m06_unions_and_punning.c -o m06_unions_and_punning
 *
 * DELIBERATE UNDEFINED BEHAVIOUR with -DSHOW_ALIASING_UB: calls a function with an int * and a
 * float * that point at the same object (a strict-aliasing violation). At -O2 GCC assumes they
 * cannot alias and returns a stale value. Build without -Werror:
 *   gcc -std=c17 -O2 -Wall -DSHOW_ALIASING_UB m06_unions_and_punning.c -o m06_alias && ./m06_alias
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A tagged union: the tag says which member is live. Reading another member is a bug. */
enum value_kind { V_INT, V_TEXT, V_AMOUNT };

struct value {
    enum value_kind kind;
    union {
        int64_t i;
        char    text[16];
        int64_t paise;
    } as;
};

static void print_value(const struct value *v)
{
    switch (v->kind) {           /* -Wswitch warns if a new enum value is not handled here */
    case V_INT:    printf("int %" PRId64 "\n", v->as.i); break;
    case V_TEXT:   printf("text \"%s\"\n", v->as.text); break;
    case V_AMOUNT: printf("amount %" PRId64 ".%02" PRId64 "\n", v->as.paise / 100, v->as.paise % 100); break;
    }
}

/* Three ways to get the bits of a float. */
static uint32_t bits_memcpy(float f)        /* always correct, in C and C++; compiles to one mov */
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static uint32_t bits_union(float f)         /* defined in C (C11 6.5.2.3 footnote 95), UB in C++ */
{
    union { float f; uint32_t u; } x = {.f = f};
    return x.u;
}

/* The third way, *(uint32_t *)&f, breaks the strict-aliasing rule: undefined behaviour. */

#ifdef SHOW_ALIASING_UB
static int alias_demo(int *i, float *f)
{
    *i = 1;
    *f = 2.0f;           /* GCC -O2 assumes this cannot change *i (different types) */
    return *i;           /* ...so it returns the constant 1 without reloading */
}
#endif

int main(void)
{
    struct value vals[3] = {
        {.kind = V_INT, .as.i = 42},
        {.kind = V_TEXT, .as.text = "NEFT"},
        {.kind = V_AMOUNT, .as.paise = 1234505},
    };
    printf("sizeof(struct value) = %zu (tag + padding + largest member)\n", sizeof(struct value));
    for (int k = 0; k < 3; ++k) print_value(&vals[k]);

    const float samples[] = {1.0f, -0.0f, 0.1f};
    for (int k = 0; k < 3; ++k) {
        uint32_t a = bits_memcpy(samples[k]), b = bits_union(samples[k]);
        printf("bits of %-5g = 0x%08" PRIX32 " (memcpy) 0x%08" PRIX32 " (union)\n", (double)samples[k], a, b);
        assert(a == b);
    }
    assert(bits_memcpy(1.0f) == 0x3F800000u);

    /* Looking at bytes through unsigned char is always allowed: the character-type exception. */
    uint32_t v = 0xA1B2C3D4;
    const unsigned char *bytes = (const unsigned char *)&v;
    printf("first byte of 0xA1B2C3D4 in memory: 0x%02X\n", bytes[0]);

#ifdef SHOW_ALIASING_UB
    int target = 0;
    int r = alias_demo(&target, (float *)&target);
    printf("UB: alias_demo returned %d, but target now holds 0x%08X (the float 2.0f)\n", r, (unsigned)target);
#endif
    return 0;
}
