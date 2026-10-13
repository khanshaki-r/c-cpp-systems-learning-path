/* m06_serialize_record.c - why you never fwrite a raw struct, and the explicit encoding to use instead.
 *
 * Module: 12_C_Programming/06_structs_unions_enums_and_layout.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m06_serialize_record.c -o m06_serialize_record
 */
#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* In memory: natural types, natural layout. The compiler decides the padding. */
struct txn {
    char     type;              /* 'D' debit or 'C' credit */
    int64_t  amount_paise;
    uint32_t account_id;
    uint16_t branch;
};

/* On the wire / on disk, version 1. Little-endian, no padding, every offset written down:
 *   off  0  u8   version (= 1)
 *   off  1  u8   type
 *   off  2  u16  branch
 *   off  4  u32  account_id
 *   off  8  i64  amount_paise (two's complement)
 *   total 16 bytes                                                                         */
enum { TXN_V1 = 1, TXN_V1_SIZE = 16 };

static void put_u16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static void put_u32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static void put_u64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static uint16_t get_u16(const unsigned char *p) { return (uint16_t)(p[0] | (unsigned)p[1] << 8); }

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static uint64_t get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

/* uint64 -> int64 without implementation-defined conversion: values above INT64_MAX are
 * negative numbers in two's complement. */
static int64_t u64_to_i64(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX) return (int64_t)u;
    return -(int64_t)(~u) - 1;
}

static size_t txn_encode(const struct txn *t, unsigned char *out, size_t outsz)
{
    if (outsz < TXN_V1_SIZE) return 0;
    out[0] = TXN_V1;
    out[1] = (unsigned char)t->type;
    put_u16(out + 2, t->branch);
    put_u32(out + 4, t->account_id);
    put_u64(out + 8, (uint64_t)t->amount_paise);   /* int64 -> uint64 is defined: modulo 2^64 */
    return TXN_V1_SIZE;
}

/* Validates everything it reads. Returns 0 on success, -1 on a bad record. */
static int txn_decode(const unsigned char *in, size_t insz, struct txn *t)
{
    if (insz < 1) return -1;
    if (in[0] != TXN_V1) return -1;                 /* unknown version: quarantine, do not guess */
    if (insz < TXN_V1_SIZE) return -1;
    if (in[1] != 'D' && in[1] != 'C') return -1;
    t->type = (char)in[1];
    t->branch = get_u16(in + 2);
    t->account_id = get_u32(in + 4);
    t->amount_paise = u64_to_i64(get_u64(in + 8));
    return 0;
}

static void hexdump(const char *label, const void *p, size_t n)
{
    const unsigned char *b = p;
    printf("%-22s", label);
    for (size_t i = 0; i < n; ++i) printf("%s%02X", (i && i % 8 == 0) ? "  " : " ", b[i]);
    putchar('\n');
}

int main(void)
{
    struct txn t;
    memset(&t, 0xAA, sizeof t);                     /* make the padding visible */
    t.type = 'D';
    t.amount_paise = -250075;                       /* -2500.75 */
    t.account_id = 4000000001u;
    t.branch = 513;

    printf("sizeof(struct txn) = %zu, wire size = %d\n", sizeof t, TXN_V1_SIZE);
    hexdump("raw struct bytes:", &t, sizeof t);     /* what fwrite(&t, sizeof t, 1, f) would write */

    unsigned char wire[TXN_V1_SIZE + 1];
    size_t n = txn_encode(&t, wire + 1, sizeof wire - 1);   /* at offset 1: deliberately misaligned */
    assert(n == TXN_V1_SIZE);
    hexdump("encoded v1:", wire + 1, n);

    struct txn back;
    int rc = txn_decode(wire + 1, n, &back);
    assert(rc == 0);
    assert(back.type == t.type && back.amount_paise == t.amount_paise);
    assert(back.account_id == t.account_id && back.branch == t.branch);
    printf("decoded: type=%c amount=%" PRId64 " account=%" PRIu32 " branch=%u\n", back.type,
           back.amount_paise, back.account_id, (unsigned)back.branch);

    wire[1] = 9;                                    /* a record from a future version */
    printf("decode of version 9 -> %s\n", txn_decode(wire + 1, n, &back) == 0 ? "accepted?!" : "rejected");
    wire[1] = TXN_V1;
    assert(txn_decode(wire + 1, 5, &back) == -1);  /* short read */
    return 0;
}
