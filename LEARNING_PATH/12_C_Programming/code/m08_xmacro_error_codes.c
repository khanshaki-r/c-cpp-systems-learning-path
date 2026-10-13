/* m08_xmacro_error_codes.c - X-macros: one table generates the enum, the names, the messages,
 * and a struct with its printer, so they can never drift apart.
 *
 * Module: 12_C_Programming/08_the_preprocessor_and_macros.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m08_xmacro_error_codes.c -o m08_xmacro_error_codes
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The single source of truth. To add an error you add ONE line here. */
#define ERROR_TABLE(X)                                                    \
    X(ERR_OK,                 0, "success")                               \
    X(ERR_NOT_FOUND,          1, "account not found")                     \
    X(ERR_INSUFFICIENT_FUNDS, 2, "insufficient funds")                    \
    X(ERR_ACCOUNT_FROZEN,     3, "account is frozen")                     \
    X(ERR_DB_UNAVAILABLE,     4, "database unavailable, retry later")     \
    X(ERR_PROTOCOL,           5, "malformed request")

/* Expansion 1: the enum. */
enum err_code {
#define X(name, value, text) name = value,
    ERROR_TABLE(X)
#undef X
};

/* Expansion 2: the count, computed by adding 1 per row. */
enum {
#define X(name, value, text) +1
    ERR_COUNT = 0 ERROR_TABLE(X)
#undef X
};
_Static_assert(ERR_COUNT == 6, "error table changed: update the client documentation");

/* Expansion 3: the symbolic name, for logs ("ERR_ACCOUNT_FROZEN" beats "3" at 3 AM). */
static const char *err_name(enum err_code e)
{
    switch (e) {
#define X(name, value, text) case name: return #name;
        ERROR_TABLE(X)
#undef X
    }
    return "ERR_UNKNOWN";
}

/* Expansion 4: the human message. */
static const char *err_text(enum err_code e)
{
    switch (e) {
#define X(name, value, text) case name: return text;
        ERROR_TABLE(X)
#undef X
    }
    return "unknown error";
}

/* A second X-macro: one field list generates the struct AND its debug printer. */
#define TXN_FIELDS(X)                       \
    X(uint32_t, id,           PRIu32)       \
    X(int64_t,  amount_paise, PRId64)       \
    X(uint32_t, branch,       PRIu32)

struct txn {
#define X(type, name, fmt) type name;
    TXN_FIELDS(X)
#undef X
};

static void txn_print(const struct txn *t)
{
#define X(type, name, fmt) printf("  %-13s = %" fmt "\n", #name, t->name);
    TXN_FIELDS(X)
#undef X
}

int main(void)
{
    printf("%d error codes:\n", ERR_COUNT);
    for (int e = 0; e < ERR_COUNT; ++e)
        printf("  %d %-24s %s\n", e, err_name((enum err_code)e), err_text((enum err_code)e));
    assert(strcmp(err_name(ERR_ACCOUNT_FROZEN), "ERR_ACCOUNT_FROZEN") == 0);
    assert(strcmp(err_text(ERR_NOT_FOUND), "account not found") == 0);
    assert(strcmp(err_name((enum err_code)42), "ERR_UNKNOWN") == 0);

    struct txn t = {.id = 9001, .amount_paise = -250075, .branch = 7};
    puts("txn:");
    txn_print(&t);
    return 0;
}
