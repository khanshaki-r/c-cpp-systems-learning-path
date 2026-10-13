/* m09_strict_aliasing.c - the strict aliasing rule, what breaks when you violate it, and the
 * three legal ways to look at the bytes of an object as another type.
 *
 * DELIBERATE UB: main() calls store_both(&x, (float *)&x), which writes an int object through
 * a float lvalue. The compiler is allowed to assume an int* and a float* never point to the
 * same object, so at -O2 it returns the cached 1 without reloading.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m09_strict_aliasing m09_strict_aliasing.c
 * Run:    ./m09_strict_aliasing
 * Compare:
 *   -O0                          -> store_both returns 0 (the store through f is reloaded)
 *   -O2 -fno-strict-aliasing     -> store_both returns 0 (the Linux kernel builds this way)
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The compiler reasons: "*f = 0.0f stores a float; a float store cannot modify an int;
 * so *i is still 1". With strict aliasing on (default at -O2), it returns 1 directly. */
__attribute__((noinline)) int store_both(int *i, float *f)
{
    *i = 1;
    *f = 0.0f;
    return *i;
}

/* LEGAL 1: memcpy. The standard way; GCC and Clang compile it to a single register move. */
static uint32_t float_bits_memcpy(float f)
{
    uint32_t u;
    _Static_assert(sizeof u == sizeof f, "float must be 32 bits here");
    memcpy(&u, &f, sizeof u);
    return u;
}

/* LEGAL 2 (C only, not C++): read a different union member than the one last written.
 * C99 TC3 and C11 footnote 95 say the bytes are reinterpreted as the new type. */
static uint32_t float_bits_union(float f)
{
    union { float f; uint32_t u; } pun = { .f = f };
    return pun.u;
}

static float float_from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* LEGAL 3: character types may alias anything. This is why memcpy, hexdumps and checksum
 * code can read any object through unsigned char *. */
static int is_little_endian(void)
{
    unsigned int one = 1;
    return *(const unsigned char *)&one == 1;
}

/* Parsing a wire header from a byte buffer. The tempting version is
 *     const struct wire_hdr *h = (const struct wire_hdr *)(buf + off);
 * which is an aliasing violation AND possibly a misaligned access (buf + 1 is not 4-aligned).
 * It "works" on x86 and traps on some ARM/SPARC builds. memcpy into a local is correct. */
struct wire_hdr {
    uint32_t magic;
    uint16_t type;
    uint16_t len;
};

static struct wire_hdr parse_hdr(const unsigned char *buf)
{
    struct wire_hdr h;
    memcpy(&h, buf, sizeof h);   /* host byte order; see module 13 for explicit endianness */
    return h;
}

int main(void)
{
    int x = 5;
    int r = store_both(&x, (float *)&x);
    printf("store_both returned %d, x now holds bits 0x%08x\n", r, (unsigned)x);
    /* No assert on r: its value depends on the optimisation level - that is the point. */

    assert(float_bits_memcpy(1.0f) == 0x3F800000u);
    assert(float_bits_memcpy(-2.0f) == 0xC0000000u);
    assert(float_bits_union(1.0f) == float_bits_memcpy(1.0f));
    assert(float_from_bits(0x40490FDBu) > 3.14159f && float_from_bits(0x40490FDBu) < 3.14160f);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    assert(is_little_endian());
#elif defined(__BYTE_ORDER__)
    assert(!is_little_endian());
#endif

    /* A header placed at an odd offset inside a buffer, as it would be inside a TCP stream. */
    unsigned char buf[1 + sizeof(struct wire_hdr)];
    struct wire_hdr src = { .magic = 0xCAFEF00Du, .type = 7, .len = 512 };
    memcpy(buf + 1, &src, sizeof src);
    struct wire_hdr got = parse_hdr(buf + 1);
    assert(got.magic == 0xCAFEF00Du && got.type == 7 && got.len == 512);

    printf("1.0f has bits 0x%08x; this machine is %s-endian\n",
           (unsigned)float_bits_memcpy(1.0f), is_little_endian() ? "little" : "big");
    puts("all checks passed");
    return 0;
}
