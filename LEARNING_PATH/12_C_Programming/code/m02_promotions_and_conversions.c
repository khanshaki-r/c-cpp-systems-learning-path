/* m02_promotions_and_conversions.c - the integer conversion rules behind real production bugs.
 *
 * Module: 12_C_Programming/02_types_integers_and_conversions.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m02_promotions_and_conversions.c -o m02_promotions_and_conversions
 *
 * Two blocks deliberately compare signed with unsigned. GCC flags that (-Wsign-compare, part of
 * -Wextra; MSVC says C4018) and -Werror would stop the build, so those blocks switch the warning
 * off locally with #pragma GCC diagnostic. In real code: fix the types, never silence the warning.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* 1. Integer promotion: anything narrower than int becomes int before arithmetic. */
    uint8_t a = 200, b = 100;
    int sum = a + b;                       /* computed in int: 300, no wrap */
    uint8_t stored = (uint8_t)(a + b);     /* wraps only when squeezed back into 8 bits: 44 */
    printf("1. uint8_t 200 + 100 = %d in int, %d when stored back into uint8_t\n", sum, stored);
    assert(sum == 300 && stored == 44);

    /* 2. ~ on a small unsigned type happens in int, so the high bits become 1. */
    uint8_t mask = 0x0F;
    int inverted = ~mask;                  /* ~(int)15 == -16, not 0xF0 */
    uint8_t inverted8 = (uint8_t)~mask;    /* cast back to get the 8-bit answer you meant */
    printf("2. ~(uint8_t)0x0F = %d as int, 0x%02X after casting back to uint8_t\n", inverted, inverted8);
    assert(inverted == -16 && inverted8 == 0xF0);

    /* 3. Plain char is signed on x86 (GCC and MSVC): bytes >= 0x80 turn negative. */
    char byte = (char)0xE9;                /* 0xE9 is a Latin-1 / Windows-1252 accented e */
    int as_int = byte;
    int as_uchar = (unsigned char)byte;
    printf("3. char holding 0xE9 -> %d as int, %d via unsigned char\n", as_int, as_uchar);
    assert(as_uchar == 233);               /* always; as_int is -23 only where char is signed */

    /* 4. Usual arithmetic conversions: int meets unsigned int -> the int becomes unsigned. */
    int minus_one = -1;
    unsigned int one = 1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    int lt = (minus_one < one);            /* -1 becomes 4294967295u, so this is FALSE */
#pragma GCC diagnostic pop
    printf("4. (-1 < 1u) is %s\n", lt ? "true" : "false");
    assert(!lt);

    /* 5. The length-check bug: a signed check passes, then memcpy sees a huge size_t. */
    char buf[64];
    int len = -5;                          /* e.g. a corrupt length field read from the wire */
    int passes_check = (len <= (int)sizeof buf);            /* -5 <= 64: true, check passes */
    size_t what_memcpy_sees = (size_t)len;                  /* 18446744073709551611 */
    printf("5. len=-5 passes 'len <= sizeof buf': %s; memcpy would copy %zu bytes\n",
           passes_check ? "yes" : "no", what_memcpy_sees);
    assert(passes_check && what_memcpy_sees == SIZE_MAX - 4);
    (void)buf;

    /* 6. size_t subtraction never goes negative. */
    size_t diff = strlen("abc") - strlen("abcdef");          /* 3 - 6 wraps */
    printf("6. strlen(\"abc\") - strlen(\"abcdef\") = %zu\n", diff);
    assert(diff == SIZE_MAX - 2);

    /* 7. Comparing a size_t with a negative int: the int is converted to size_t. */
    size_t count = 3;
    int wanted = -1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    int smaller = (wanted < count);        /* (size_t)-1 is SIZE_MAX: false */
#pragma GCC diagnostic pop
    printf("7. (-1 < (size_t)3) is %s\n", smaller ? "true" : "false");
    assert(!smaller);

    /* 8. Shifts: 1 << 31 overflows int (undefined). Shift an unsigned value of the right width. */
    uint32_t top_bit = UINT32_C(1) << 31;
    uint64_t bit40 = UINT64_C(1) << 40;    /* 1 << 40 would shift a 32-bit int by 40: undefined */
    printf("8. UINT32_C(1) << 31 = %u, UINT64_C(1) << 40 = %llu\n", (unsigned)top_bit,
           (unsigned long long)bit40);
    assert(top_bit == 0x80000000u && bit40 == 1099511627776ULL);

    /* 9. Narrowing conversions are silent without -Wconversion. */
    int64_t big_paise = INT64_C(5000000000);        /* 50,000,000.00 rupees */
    int32_t truncated = (int32_t)big_paise;         /* implementation-defined: GCC keeps low 32 bits */
    printf("9. int64_t 5000000000 squeezed into int32_t = %d\n", (int)truncated);
    assert(truncated != big_paise);
    return 0;
}
