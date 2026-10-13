/* m10_error_model.c - designing the error model of a C library, Berkeley DB style.
 *
 *   - Every function returns int: 0 on success.
 *   - Positive values are errno codes passed through from the OS (ENOENT, ENOMEM, ...).
 *   - Negative values in a reserved range are the library's own codes (KV_NOTFOUND, ...),
 *     the same idea as Berkeley DB's DB_NOTFOUND and DB_KEYEXIST, which are negative
 *     numbers near -30000 so they can never collide with a positive errno.
 *   - kv_strerror(code) turns ANY of those into text, like db_strerror().
 *   - The handle remembers a detailed message for the last failure and can call an
 *     application-supplied error callback, like DB_ENV->set_errcall().
 *   - Out-parameters are written only on success.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m10_error_model m10_error_model.c
 * Run:    ./m10_error_model
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ======================= what would be kv.h ======================= */
#define KV_OK        0
#define KV_NOTFOUND  (-30500)   /* key not present: a normal outcome, not a failure */
#define KV_KEYEXIST  (-30501)   /* put with KV_NOOVERWRITE on an existing key */
#define KV_FULL      (-30502)   /* fixed capacity reached */
#define KV_BADARG    (-30503)   /* caller passed something invalid */
#define KV_CORRUPT   (-30504)   /* input data failed validation */

#define KV_NOOVERWRITE 0x1u

typedef struct kv kv_t;         /* opaque: callers never see the fields */
typedef void (*kv_errcall_fn)(const char *prefix, const char *msg);

int         kv_open(kv_t **out, size_t capacity);
void        kv_close(kv_t *kv);
int         kv_put(kv_t *kv, const char *key, long long value, unsigned flags);
int         kv_get(const kv_t *kv, const char *key, long long *value);
int         kv_load(kv_t *kv, const char *path);
void        kv_set_errcall(kv_t *kv, kv_errcall_fn fn, const char *prefix);
const char *kv_last_error(const kv_t *kv);
const char *kv_strerror(int err);

/* ======================= what would be kv.c ======================= */
#define KV_KEY_MAX 32

struct kv_entry {
    char      key[KV_KEY_MAX];
    long long value;
};

struct kv {
    struct kv_entry *entries;
    size_t           count;
    size_t           capacity;
    kv_errcall_fn    errcall;
    const char      *errpfx;
    char             last_error[160];
};

const char *kv_strerror(int err)
{
    switch (err) {
    case KV_OK:       return "KV_OK: success";
    case KV_NOTFOUND: return "KV_NOTFOUND: key not found";
    case KV_KEYEXIST: return "KV_KEYEXIST: key already exists";
    case KV_FULL:     return "KV_FULL: store is full";
    case KV_BADARG:   return "KV_BADARG: invalid argument";
    case KV_CORRUPT:  return "KV_CORRUPT: input failed validation";
    }
    if (err > 0)
        return strerror(err);   /* system error passed through. strerror is not guaranteed
                                 * thread-safe; a multithreaded library would use strerror_r
                                 * into a caller buffer (see m10_errno_conventions.c). */
    return "KV: unknown error code";
}

/* Record a detailed, contextual message on the handle and report it through the callback.
 * Returns err so call sites can write: return kv_fail(kv, KV_FULL, "...", ...); */
static int kv_fail(kv_t *kv, int err, const char *fmt, ...)
{
    char detail[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    snprintf(kv->last_error, sizeof kv->last_error, "%s: %s", detail, kv_strerror(err));
    if (kv->errcall)
        kv->errcall(kv->errpfx ? kv->errpfx : "kv", kv->last_error);
    return err;
}

int kv_open(kv_t **out, size_t capacity)
{
    if (out == NULL || capacity == 0 || capacity > 1000000)
        return KV_BADARG;                 /* no handle yet, so no detailed message */
    kv_t *kv = calloc(1, sizeof *kv);
    if (!kv)
        return ENOMEM;
    kv->entries = calloc(capacity, sizeof *kv->entries);
    if (!kv->entries) {
        free(kv);
        return ENOMEM;
    }
    kv->capacity = capacity;
    *out = kv;                            /* written only on success */
    return KV_OK;
}

void kv_close(kv_t *kv)
{
    if (!kv) return;
    free(kv->entries);
    free(kv);
}

void kv_set_errcall(kv_t *kv, kv_errcall_fn fn, const char *prefix)
{
    kv->errcall = fn;
    kv->errpfx = prefix;
}

const char *kv_last_error(const kv_t *kv)
{
    return kv->last_error;
}

static struct kv_entry *kv_find(const kv_t *kv, const char *key)
{
    for (size_t i = 0; i < kv->count; i++)
        if (strcmp(kv->entries[i].key, key) == 0)
            return &kv->entries[i];
    return NULL;
}

int kv_put(kv_t *kv, const char *key, long long value, unsigned flags)
{
    if (!kv || !key)
        return KV_BADARG;
    size_t len = strlen(key);
    if (len == 0 || len >= KV_KEY_MAX)
        return kv_fail(kv, KV_BADARG, "kv_put: key length %zu not in 1..%d", len, KV_KEY_MAX - 1);

    struct kv_entry *e = kv_find(kv, key);
    if (e) {
        if (flags & KV_NOOVERWRITE)
            return kv_fail(kv, KV_KEYEXIST, "kv_put: key '%s'", key);
        e->value = value;
        return KV_OK;
    }
    if (kv->count == kv->capacity)
        return kv_fail(kv, KV_FULL, "kv_put: key '%s', capacity %zu", key, kv->capacity);
    memcpy(kv->entries[kv->count].key, key, len + 1);
    kv->entries[kv->count].value = value;
    kv->count++;
    return KV_OK;
}

int kv_get(const kv_t *kv, const char *key, long long *value)
{
    if (!kv || !key || !value)
        return KV_BADARG;
    const struct kv_entry *e = kv_find(kv, key);
    if (!e)
        return KV_NOTFOUND;   /* expected outcome: no message, no callback, no log spam */
    *value = e->value;
    return KV_OK;
}

/* File format: one "key=value" per line. */
int kv_load(kv_t *kv, const char *path)
{
    int ret = KV_OK;
    char line[96];
    int lineno = 0;

    FILE *f = fopen(path, "r");
    if (!f)
        return kv_fail(kv, errno, "kv_load: open '%s'", path);   /* positive errno */

    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *eq = strchr(line, '=');
        char *end;
        if (!eq) {
            ret = kv_fail(kv, KV_CORRUPT, "kv_load: %s:%d: missing '='", path, lineno);
            goto out;
        }
        *eq = '\0';
        errno = 0;
        long long v = strtoll(eq + 1, &end, 10);
        if (errno != 0 || end == eq + 1 || (*end != '\n' && *end != '\0')) {
            ret = kv_fail(kv, KV_CORRUPT, "kv_load: %s:%d: bad number", path, lineno);
            goto out;
        }
        ret = kv_put(kv, line, v, 0);
        if (ret != KV_OK)
            goto out;         /* kv_put already recorded the detail */
    }
    if (ferror(f))
        ret = kv_fail(kv, EIO, "kv_load: read '%s'", path);
out:
    fclose(f);
    return ret;
}

/* ======================= the application side ======================= */
static int g_errcall_count;

static void app_errcall(const char *prefix, const char *msg)
{
    g_errcall_count++;
    printf("[%s] %s\n", prefix, msg);   /* a real server sends this to its log */
}

/* Logging an error WITH CONTEXT: what we were doing, on which object, the code, and where. */
#define LOG_ERR(err, ...)                                                       \
    do {                                                                        \
        char ctx_[128];                                                         \
        snprintf(ctx_, sizeof ctx_, __VA_ARGS__);                               \
        printf("ERROR %s:%d %s(): %s: %s (code %d)\n", __FILE__, __LINE__,      \
               __func__, ctx_, kv_strerror(err), (err));                        \
    } while (0)

int main(void)
{
    kv_t *kv = NULL;
    long long v = -1;
    int rc;

    rc = kv_open(&kv, 0);
    assert(rc == KV_BADARG && kv == NULL);          /* out-param untouched on failure */
    rc = kv_open(&kv, 2);
    assert(rc == KV_OK && kv != NULL);
    kv_set_errcall(kv, app_errcall, "kv-test");

    rc = kv_put(kv, "ACC-1", 100, 0);
    assert(rc == KV_OK);
    rc = kv_put(kv, "ACC-2", 200, 0);
    assert(rc == KV_OK);

    rc = kv_get(kv, "ACC-9", &v);
    assert(rc == KV_NOTFOUND && v == -1);           /* *value not written */
    assert(g_errcall_count == 0);                   /* NOTFOUND is not reported as an error */

    rc = kv_put(kv, "ACC-1", 111, KV_NOOVERWRITE);
    assert(rc == KV_KEYEXIST);
    rc = kv_put(kv, "ACC-3", 300, 0);
    assert(rc == KV_FULL);
    assert(strstr(kv_last_error(kv), "capacity 2") != NULL);
    assert(g_errcall_count == 2);

    rc = kv_load(kv, "/no/such/file.kv");
    assert(rc == ENOENT);                           /* positive: an errno passed through */
    assert(strcmp(kv_strerror(rc), strerror(ENOENT)) == 0);
    LOG_ERR(rc, "loading accounts for branch %d", 42);

    rc = kv_get(kv, "ACC-2", &v);
    assert(rc == KV_OK && v == 200);

    assert(strcmp(kv_strerror(KV_NOTFOUND), "KV_NOTFOUND: key not found") == 0);
    assert(strcmp(kv_strerror(-1), "KV: unknown error code") == 0);

    kv_close(kv);
    kv_close(NULL);                                  /* like free(NULL): allowed */
    puts("all checks passed");
    return 0;
}
