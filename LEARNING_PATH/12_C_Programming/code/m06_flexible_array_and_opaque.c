/* m06_flexible_array_and_opaque.c - flexible array members, designated initializers, opaque types.
 *
 * Module: 12_C_Programming/06_structs_unions_enums_and_layout.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m06_flexible_array_and_opaque.c -o m06_flexible_array_and_opaque
 */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 1. flexible array member */
struct message {
    uint32_t      type;
    uint32_t      len;
    unsigned char payload[];      /* C99: must be last; contributes nothing to sizeof */
};

/* One allocation holds header and payload: one malloc, one free, good cache locality. */
static struct message *message_new(uint32_t type, const void *data, uint32_t len)
{
    struct message *m = malloc(sizeof *m + len);
    if (m == NULL) return NULL;
    m->type = type;
    m->len = len;
    memcpy(m->payload, data, len);
    return m;
}

/* ---------------------------------------------------------------- 2. designated initializers */
struct server_config {
    const char *bind_addr;
    int         port;
    int         max_connections;
    int         timeout_ms;
    bool        tls;
};

static const struct server_config k_defaults = {
    .bind_addr = "0.0.0.0",
    .port = 7000,
    .max_connections = 256,
    .timeout_ms = 5000,           /* .tls is not named: it is zero, i.e. false */
};

enum log_level { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_LEVEL_COUNT };
static const char *const k_level_names[LOG_LEVEL_COUNT] = {
    [LOG_ERROR] = "ERROR",        /* index by enum: reordering the enum cannot misalign names */
    [LOG_WARN] = "WARN",
    [LOG_INFO] = "INFO",
    [LOG_DEBUG] = "DEBUG",
};

static void print_config(const struct server_config *c)
{
    printf("config %s:%d max=%d timeout=%dms tls=%s\n", c->bind_addr, c->port, c->max_connections,
           c->timeout_ms, c->tls ? "on" : "off");
}

/* ---------------------------------------------------------------- 3. opaque type
 * What account.h would contain: an INCOMPLETE type. Callers can hold an account * and pass it
 * around, but cannot see the fields, cannot allocate one, and cannot take sizeof. */
typedef struct account account;
account *account_open(const char *id, int64_t opening_paise);
int      account_deposit(account *a, int64_t paise);
int64_t  account_balance(const account *a);
void     account_close(account *a);

/* What account.c would contain: the only place that knows the layout. You can add fields
 * here without recompiling any caller, which is how a stable C library ABI is kept. */
struct account {
    char     id[16];
    int64_t  balance_paise;
    uint32_t txn_count;
};

account *account_open(const char *id, int64_t opening_paise)
{
    if (opening_paise < 0 || strlen(id) >= sizeof ((account *)0)->id) return NULL;
    account *a = calloc(1, sizeof *a);
    if (a == NULL) return NULL;
    memcpy(a->id, id, strlen(id) + 1);
    a->balance_paise = opening_paise;
    return a;
}

int account_deposit(account *a, int64_t paise)
{
    if (paise <= 0 || a->balance_paise > INT64_MAX - paise) return -1;
    a->balance_paise += paise;
    a->txn_count++;
    return 0;
}

int64_t account_balance(const account *a) { return a->balance_paise; }

void account_close(account *a) { free(a); }

int main(void)
{
    const char text[] = "BAL?ACC-000042";
    struct message *m = message_new(7, text, (uint32_t)strlen(text));
    if (m == NULL) return 1;
    printf("sizeof(struct message) = %zu, offsetof payload = %zu, this message uses %zu bytes\n",
           sizeof(struct message), offsetof(struct message, payload), sizeof *m + m->len);
    printf("payload: %.*s\n", (int)m->len, (const char *)m->payload);
    assert(m->len == 14 && memcmp(m->payload, "BAL?", 4) == 0);
    free(m);

    print_config(&k_defaults);
    struct server_config c = k_defaults;      /* struct assignment copies every member */
    c.port = 9000;
    c.tls = true;
    print_config(&c);
    print_config(&(struct server_config){.bind_addr = "127.0.0.1", .port = 7001});   /* compound literal */
    printf("k_level_names[LOG_WARN] = %s\n", k_level_names[LOG_WARN]);
    assert(strcmp(k_level_names[LOG_DEBUG], "DEBUG") == 0);

    account *acc = account_open("ACC-000042", 100000);
    if (acc == NULL) return 1;
    assert(account_deposit(acc, 25050) == 0);
    assert(account_deposit(acc, -5) == -1);
    printf("opaque account balance: %" PRId64 " paise\n", account_balance(acc));
    assert(account_balance(acc) == 125050);
    account_close(acc);
    return 0;
}
