/* m06_packed_and_bitfields.c - packing (and what it costs), bitfields, and portable bit masks.
 *
 * Module: 12_C_Programming/06_structs_unions_enums_and_layout.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m06_packed_and_bitfields.c -o m06_packed_and_bitfields
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* #pragma pack: understood by MSVC, GCC and Clang. push/pop restores the previous setting so
 * the packing cannot leak into structs declared later (or into headers included later). */
#pragma pack(push, 1)
struct rec_pragma {
    uint8_t  version;
    uint32_t customer_id;
    int64_t  amount_paise;
    uint16_t branch;
};
#pragma pack(pop)

/* GCC and Clang spelling. Same layout. MSVC does not understand this one. */
struct rec_attr {
    uint8_t  version;
    uint32_t customer_id;
    int64_t  amount_paise;
    uint16_t branch;
} __attribute__((packed));

struct rec_natural {
    uint8_t  version;
    uint32_t customer_id;
    int64_t  amount_paise;
    uint16_t branch;
};

/* Bitfields: compact, readable, and NOT portable as a storage or wire format. The order of
 * fields inside the unit, and how fields straddle units, are implementation-defined. */
struct acct_flags {
    unsigned int active    : 1;
    unsigned int frozen    : 1;
    unsigned int kyc_level : 3;   /* 0..7 */
    unsigned int reserved  : 27;
};

/* The portable alternative: one fixed-width integer with explicit masks and shifts. */
#define ACCT_ACTIVE      (UINT32_C(1) << 0)
#define ACCT_FROZEN      (UINT32_C(1) << 1)
#define ACCT_KYC_SHIFT   2
#define ACCT_KYC_MASK    (UINT32_C(0x7) << ACCT_KYC_SHIFT)

static uint32_t set_kyc(uint32_t word, unsigned level)
{
    return (word & ~ACCT_KYC_MASK) | (((uint32_t)level << ACCT_KYC_SHIFT) & ACCT_KYC_MASK);
}

static unsigned get_kyc(uint32_t word) { return (unsigned)((word & ACCT_KYC_MASK) >> ACCT_KYC_SHIFT); }

int main(void)
{
    printf("natural: sizeof %zu, amount_paise at offset %zu\n", sizeof(struct rec_natural),
           offsetof(struct rec_natural, amount_paise));
    printf("pragma : sizeof %zu, amount_paise at offset %zu\n", sizeof(struct rec_pragma),
           offsetof(struct rec_pragma, amount_paise));
    printf("attr   : sizeof %zu, amount_paise at offset %zu\n", sizeof(struct rec_attr),
           offsetof(struct rec_attr, amount_paise));
    assert(sizeof(struct rec_pragma) == 15 && sizeof(struct rec_attr) == 15);
    assert(sizeof(struct rec_natural) == 24);

    /* Accessing a packed member BY NAME is safe: the compiler knows it may be misaligned and
     * emits code that copes. Taking its ADDRESS is not: an int64_t * promises 8-byte alignment.
     *   int64_t *p = &ra.amount_paise;  (struct rec_attr) -> GCC 13: "taking address of packed
     *       member of 'struct rec_attr' may result in an unaligned pointer value"
     *   int64_t *p = &r.amount_paise;   (struct rec_pragma) -> GCC 13 says NOTHING. Same bug.
     * Copy the value out instead. */
    struct rec_pragma r = {2, 1001, -250000, 7};
    int64_t amount;
    memcpy(&amount, (const unsigned char *)&r + offsetof(struct rec_pragma, amount_paise), sizeof amount);
    printf("packed amount read via member = %lld, via memcpy = %lld\n", (long long)r.amount_paise,
           (long long)amount);
    assert(amount == r.amount_paise);

    struct acct_flags f = {0};
    f.active = 1;
    f.kyc_level = 5;
    uint32_t raw;
    memcpy(&raw, &f, sizeof raw);
    printf("bitfield struct: sizeof %zu, active=1 kyc=5 stored as 0x%08X (GCC on x86: first field in the lowest bit)\n",
           sizeof f, (unsigned)raw);

    uint32_t word = ACCT_ACTIVE;
    word = set_kyc(word, 5);
    printf("mask version   : 0x%08X, active=%d frozen=%d kyc=%u\n", (unsigned)word,
           (word & ACCT_ACTIVE) != 0, (word & ACCT_FROZEN) != 0, get_kyc(word));
    assert(word == raw && get_kyc(word) == 5);

    /* A bitfield silently truncates out-of-range values: kyc_level is 3 bits. */
    unsigned level = 9;
    f.kyc_level = level & 0x7u;      /* written explicitly; a plain "= level" stores 9 mod 8 = 1 */
    assert(f.kyc_level == 1);
    return 0;
}
