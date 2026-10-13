/* m07_function_pointers.c - function pointer syntax, typedefs, and an RPC-style dispatch table.
 *
 * Module: 12_C_Programming/07_functions_function_pointers_and_callbacks.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m07_function_pointers.c -o m07_function_pointers
 */
#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

/* Raw syntax: the * and the name go in parentheses, or it declares a function returning int *. */
int (*g_op)(int, int) = add;

/* With a typedef, everything below reads like ordinary C. */
typedef int (*binop_fn)(int, int);

/* A function RETURNING a function pointer.
 * Without typedef:  int (*pick(char c))(int, int)
 * With typedef:     binop_fn pick(char c)                                                     */
static binop_fn pick(char c)
{
    switch (c) {
    case '+': return add;
    case '-': return sub;
    case '*': return mul;
    default:  return NULL;
    }
}

/* ---- A dispatch table: how an RPC server maps an opcode on the wire to a handler. ---- */
struct request {
    uint16_t    opcode;
    int64_t     amount_paise;
    const char *account;
};

struct response {
    int  status;                 /* 0 ok, else an error code */
    char text[48];
};

typedef void (*handler_fn)(const struct request *req, struct response *resp);

static void h_balance(const struct request *req, struct response *resp)
{
    resp->status = 0;
    snprintf(resp->text, sizeof resp->text, "balance of %s is 1500.00", req->account);
}

static void h_deposit(const struct request *req, struct response *resp)
{
    resp->status = 0;
    snprintf(resp->text, sizeof resp->text, "deposited %" PRId64 " paise", req->amount_paise);
}

static void h_withdraw(const struct request *req, struct response *resp)
{
    resp->status = req->amount_paise > 150000 ? 2 : 0;
    snprintf(resp->text, sizeof resp->text, "%s", resp->status ? "insufficient funds" : "ok");
}

enum { OP_BALANCE = 1, OP_DEPOSIT = 2, OP_WITHDRAW = 3, OP_MAX = 8 };

/* Indexed by opcode. Unlisted entries are NULL. const: the table lives in read-only memory, so
 * a stray write cannot redirect a handler (an attacker's favourite target in writable tables). */
static const handler_fn k_handlers[OP_MAX] = {
    [OP_BALANCE] = h_balance,
    [OP_DEPOSIT] = h_deposit,
    [OP_WITHDRAW] = h_withdraw,
};

static void dispatch(const struct request *req, struct response *resp)
{
    /* Bounds check AND null check: the opcode came from the network. */
    if (req->opcode >= OP_MAX || k_handlers[req->opcode] == NULL) {
        resp->status = 1;
        snprintf(resp->text, sizeof resp->text, "unknown opcode %u", (unsigned)req->opcode);
        return;
    }
    k_handlers[req->opcode](req, resp);
}

int main(void)
{
    assert(g_op(2, 3) == 5);
    assert((*g_op)(2, 3) == 5);  /* calling through *g_op is identical */
    g_op = &sub;                 /* & is optional: a function name decays to a pointer */
    assert(g_op(2, 3) == -1);

    binop_fn table[] = {add, sub, mul};
    int results[3];
    for (int i = 0; i < 3; ++i) results[i] = table[i](6, 3);
    printf("table[i](6, 3) = %d %d %d\n", results[0], results[1], results[2]);
    assert(results[0] == 9 && results[1] == 3 && results[2] == 18);

    binop_fn f = pick('*');
    assert(f != NULL && f(7, 6) == 42);
    assert(pick('/') == NULL);   /* always check before calling: calling NULL jumps to address 0 */

    const struct request reqs[] = {
        {OP_BALANCE, 0, "ACC-000042"},
        {OP_DEPOSIT, 250000, "ACC-000042"},
        {OP_WITHDRAW, 999999, "ACC-000042"},
        {7, 0, "ACC-000042"},
        {500, 0, "ACC-000042"},
    };
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; ++i) {
        struct response resp = {0};
        dispatch(&reqs[i], &resp);
        printf("opcode %3u -> status %d, \"%s\"\n", (unsigned)reqs[i].opcode, resp.status, resp.text);
    }
    return 0;
}
