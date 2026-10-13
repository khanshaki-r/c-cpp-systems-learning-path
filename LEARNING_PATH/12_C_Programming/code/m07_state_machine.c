/* m07_state_machine.c - a table-driven state machine: an incremental netstring parser.
 *
 * Module: 12_C_Programming/07_functions_function_pointers_and_callbacks.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m07_state_machine.c -o m07_state_machine
 *
 * A netstring frames a payload as  <length>:<payload>,   e.g.  "5:hello,"
 * Bytes arrive from TCP in arbitrary chunks, so the parser must keep its place between calls:
 * that "place" is the state. Each state is a function; the table maps state -> function.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { MAX_LEN_DIGITS = 4, MAX_PAYLOAD = 1024 };   /* refuse lengths an attacker could abuse */

typedef enum { ST_LENGTH, ST_PAYLOAD, ST_COMMA, ST_ERROR, ST_COUNT } state_id;

struct parser;
typedef void (*frame_cb)(const char *payload, size_t len, void *ctx);
typedef state_id (*state_fn)(struct parser *p, unsigned char c);

struct parser {
    state_id    state;
    size_t      expected;      /* length announced by the peer */
    int         digits;
    size_t      have;          /* payload bytes collected so far */
    char        buf[MAX_PAYLOAD];
    frame_cb    on_frame;
    void       *ctx;
    const char *error;
};

static state_id fail(struct parser *p, const char *why)
{
    p->error = why;
    return ST_ERROR;
}

static state_id on_length(struct parser *p, unsigned char c)
{
    if (c >= '0' && c <= '9') {
        if (++p->digits > MAX_LEN_DIGITS) return fail(p, "length has too many digits");
        p->expected = p->expected * 10 + (size_t)(c - '0');
        return ST_LENGTH;
    }
    if (c == ':' && p->digits > 0) {
        if (p->expected > MAX_PAYLOAD) return fail(p, "length above limit");
        p->have = 0;
        return p->expected == 0 ? ST_COMMA : ST_PAYLOAD;
    }
    return fail(p, "bad character in length");
}

static state_id on_payload(struct parser *p, unsigned char c)
{
    p->buf[p->have++] = (char)c;
    return p->have == p->expected ? ST_COMMA : ST_PAYLOAD;
}

static state_id on_comma(struct parser *p, unsigned char c)
{
    if (c != ',') return fail(p, "missing ',' after payload");
    p->on_frame(p->buf, p->have, p->ctx);
    p->expected = 0;               /* reset for the next frame */
    p->digits = 0;
    return ST_LENGTH;
}

static state_id on_error(struct parser *p, unsigned char c)
{
    (void)p;
    (void)c;
    return ST_ERROR;               /* sticky: the connection must be closed */
}

static const state_fn k_states[ST_COUNT] = {
    [ST_LENGTH] = on_length,
    [ST_PAYLOAD] = on_payload,
    [ST_COMMA] = on_comma,
    [ST_ERROR] = on_error,
};

static void parser_init(struct parser *p, frame_cb cb, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->state = ST_LENGTH;
    p->on_frame = cb;
    p->ctx = ctx;
}

/* Feed any number of bytes. Returns 0, or -1 once the stream is invalid. */
static int parser_feed(struct parser *p, const char *data, size_t n)
{
    for (size_t i = 0; i < n && p->state != ST_ERROR; ++i)
        p->state = k_states[p->state](p, (unsigned char)data[i]);
    return p->state == ST_ERROR ? -1 : 0;
}

struct collected {
    int  frames;
    char joined[64];
};

static void collect(const char *payload, size_t len, void *ctx)
{
    struct collected *c = ctx;
    c->frames++;
    size_t used = strlen(c->joined);
    snprintf(c->joined + used, sizeof c->joined - used, "%s%.*s", used ? "|" : "", (int)len, payload);
}

int main(void)
{
    struct parser p;
    struct collected got = {0};
    parser_init(&p, collect, &got);

    /* Three frames split across four reads at awkward places. */
    const char *chunks[] = {"5:he", "llo,1", "2:BAL?ACC-0042,0", ":,"};
    for (size_t i = 0; i < 4; ++i) {
        int rc = parser_feed(&p, chunks[i], strlen(chunks[i]));
        printf("fed %-20s -> frames so far %d\n", chunks[i], got.frames);
        assert(rc == 0);
    }
    printf("payloads: %s\n", got.joined);
    assert(got.frames == 3 && strcmp(got.joined, "hello|BAL?ACC-0042|") == 0);

    const char *bad[] = {"5:hello;", "99999:x", "2:ab,x:"};
    for (size_t i = 0; i < 3; ++i) {
        struct collected g2 = {0};
        parser_init(&p, collect, &g2);
        int rc = parser_feed(&p, bad[i], strlen(bad[i]));
        printf("bad input %-10s -> rc %d, error: %s\n", bad[i], rc, p.error);
        assert(rc == -1);
    }
    return 0;
}
