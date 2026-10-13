/* m06_layout_and_padding.c - where the compiler puts struct members, and the padding between them.
 *
 * Module: 12_C_Programming/06_structs_unions_enums_and_layout.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m06_layout_and_padding.c -o m06_layout_and_padding
 * See it from outside: pahole ./m06_layout_and_padding   (apt install dwarves) prints every hole.
 */
#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct txn_bad {           /* declared in "logical" order */
    char    type;          /* 'D' or 'W' */
    int64_t amount_paise;
    char    channel;       /* 'B'ranch, 'A'TM, 'N'et */
    int32_t branch;
};

struct txn_good {          /* same fields, largest alignment first */
    int64_t amount_paise;
    int32_t branch;
    char    type;
    char    channel;
};

#define SHOW_FIELD(T, f) \
    printf("  %-14s offset %2zu size %zu\n", #f, offsetof(T, f), sizeof(((T *)0)->f))

static void dump(const char *label, const void *p, size_t n)
{
    const unsigned char *b = p;
    printf("%s", label);
    for (size_t i = 0; i < n; ++i) printf("%s%02X", i % 8 ? " " : "  ", b[i]);
    putchar('\n');
}

int main(void)
{
    printf("struct txn_bad:  sizeof %zu, alignof %zu\n", sizeof(struct txn_bad), alignof(struct txn_bad));
    SHOW_FIELD(struct txn_bad, type);
    SHOW_FIELD(struct txn_bad, amount_paise);
    SHOW_FIELD(struct txn_bad, channel);
    SHOW_FIELD(struct txn_bad, branch);
    printf("struct txn_good: sizeof %zu, alignof %zu\n", sizeof(struct txn_good), alignof(struct txn_good));
    SHOW_FIELD(struct txn_good, amount_paise);
    SHOW_FIELD(struct txn_good, branch);
    SHOW_FIELD(struct txn_good, type);
    SHOW_FIELD(struct txn_good, channel);
    assert(sizeof(struct txn_bad) == 24 && sizeof(struct txn_good) == 16);

    /* Arrays: element i starts at i * sizeof(T). Trailing padding keeps element 1 aligned. */
    struct txn_good arr[2];
    printf("array stride = %td bytes\n", (char *)&arr[1] - (char *)&arr[0]);

    /* Padding bytes have unspecified values. Here they hold whatever memset left behind, so two
     * structs with EQUAL fields can still differ byte-for-byte: never memcmp or hash raw structs. */
    struct txn_bad x, y;
    memset(&x, 0xAA, sizeof x);
    memset(&y, 0x00, sizeof y);
    x.type = y.type = 'D';
    x.amount_paise = y.amount_paise = 150000;
    x.channel = y.channel = 'A';
    x.branch = y.branch = 7;
    int fields_equal = x.type == y.type && x.amount_paise == y.amount_paise &&
                       x.channel == y.channel && x.branch == y.branch;
    int bytes_equal = memcmp(&x, &y, sizeof x) == 0;
    printf("fields equal: %s, memcmp equal: %s\n", fields_equal ? "yes" : "no", bytes_equal ? "yes" : "no");
    dump("x bytes:", &x, sizeof x);
    dump("y bytes:", &y, sizeof y);
    assert(fields_equal);
    return 0;
}
