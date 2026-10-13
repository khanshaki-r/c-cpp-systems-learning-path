/* m02_endianness.c - byte order in memory, and endian-independent loads and stores.
 *
 * Module: 12_C_Programming/02_types_integers_and_conversions.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m02_endianness.c -o m02_endianness
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int host_is_little_endian(void)
{
    uint32_t one = 1;
    unsigned char first;
    memcpy(&first, &one, 1);         /* look at the byte stored at the lowest address */
    return first == 1;
}

static void dump(const char *label, const void *p, size_t n)
{
    const unsigned char *b = p;      /* unsigned char may inspect the bytes of any object */
    printf("%-34s", label);
    for (size_t i = 0; i < n; ++i) printf(" %02X", b[i]);
    putchar('\n');
}

/* Endian-INDEPENDENT code: build values from bytes with shifts. Works on any host, needs no
 * #ifdef, and GCC -O2 turns each one into a single mov (plus bswap for the foreign order).
 * The (uint32_t) casts matter: p[0] << 24 alone promotes p[0] to int, and 0x80 << 24 overflows int. */
static uint32_t load_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint32_t load_le32(const unsigned char *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | (uint32_t)p[0];
}

static void store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void store_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

int main(void)
{
    printf("this host is %s-endian\n", host_is_little_endian() ? "little" : "big");

    uint32_t v = 0x11223344;
    unsigned char wire[4];
    dump("0x11223344 as stored in memory:", &v, sizeof v);
    store_be32(wire, v);
    dump("store_be32 (network byte order):", wire, sizeof wire);
    assert(load_be32(wire) == v);
    store_le32(wire, v);
    dump("store_le32:", wire, sizeof wire);
    assert(load_le32(wire) == v);

    /* The classic bug: a peer sends length 256 in network (big-endian) order and the receiver
     * memcpy's it straight into a uint32_t on an x86 box. */
    const unsigned char msg[4] = {0x00, 0x00, 0x01, 0x00};
    uint32_t wrong;
    memcpy(&wrong, msg, sizeof wrong);
    printf("length 256 sent big-endian: memcpy reads %u, load_be32 reads %u\n", wrong, load_be32(msg));
    assert(load_be32(msg) == 256);

    /* High bit set: proves the casts in load_be32 keep the arithmetic unsigned. */
    const unsigned char high[4] = {0xF0, 0x00, 0x00, 0x01};
    assert(load_be32(high) == 0xF0000001u);
    return 0;
}
