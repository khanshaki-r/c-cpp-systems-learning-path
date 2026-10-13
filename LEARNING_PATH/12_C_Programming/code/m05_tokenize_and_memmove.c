/* m05_tokenize_and_memmove.c - strtok vs strtok_r, a splitter that keeps empty fields, memmove.
 *
 * Module: 12_C_Programming/05_strings_and_buffers.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m05_tokenize_and_memmove.c -o m05_tokenize_and_memmove
 */
#define _POSIX_C_SOURCE 200809L  /* strtok_r is POSIX, not ISO C: -std=c17 hides it without this */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Parse "k=v;k=v;..." with NESTED strtok calls. strtok keeps ONE hidden static position for
 * the whole process, so the inner loop destroys the outer loop's state. */
static int count_pairs_strtok(char *s)
{
    int pairs = 0;
    for (char *item = strtok(s, ";"); item != NULL; item = strtok(NULL, ";")) {
        char *key = strtok(item, "=");          /* clobbers the outer position */
        char *val = strtok(NULL, "=");
        if (key && val) ++pairs;
    }
    return pairs;
}

/* Same with strtok_r: each loop keeps its own position in its own variable. */
static int count_pairs_strtok_r(char *s)
{
    int pairs = 0;
    char *outer, *inner;
    for (char *item = strtok_r(s, ";", &outer); item != NULL; item = strtok_r(NULL, ";", &outer)) {
        char *key = strtok_r(item, "=", &inner);
        char *val = strtok_r(NULL, "=", &inner);
        if (key && val) ++pairs;
    }
    return pairs;
}

/* A non-destructive splitter that KEEPS empty fields (CSV needs that; strtok merges "a,,b"
 * into two tokens). Calls cb(field, len) for each field; the input is never modified. */
static int split_keep_empty(const char *s, char delim, void (*cb)(const char *f, size_t n, void *ctx),
                            void *ctx)
{
    int fields = 0;
    char set[2] = {delim, '\0'};
    for (;;) {
        size_t n = strcspn(s, set);             /* length up to the next delimiter or the end */
        cb(s, n, ctx);
        ++fields;
        if (s[n] == '\0') break;
        s += n + 1;
    }
    return fields;
}

static void print_field(const char *f, size_t n, void *ctx)
{
    (void)ctx;
    printf(" [%.*s]", (int)n, f);               /* %.*s prints exactly n bytes, no NUL needed */
}

int main(void)
{
    char s1[] = "a=1;b=2;c=3";
    char s2[] = "a=1;b=2;c=3";
    int n1 = count_pairs_strtok(s1);
    int n2 = count_pairs_strtok_r(s2);
    printf("nested strtok   found %d pair(s)\n", n1);
    printf("nested strtok_r found %d pair(s)\n", n2);
    assert(n1 == 1 && n2 == 3);
    printf("after strtok, the input buffer reads \"%s\" (it was modified in place)\n", s1);

    char csv[] = "ACC1,,250.00,";
    int count = 0;
    for (char *t = strtok(csv, ","); t != NULL; t = strtok(NULL, ",")) ++count;
    printf("strtok on \"ACC1,,250.00,\" sees %d fields; split_keep_empty sees:", count);
    int fields = split_keep_empty("ACC1,,250.00,", ',', print_field, NULL);
    printf("  (%d fields)\n", fields);
    assert(count == 2 && fields == 4);

    /* memmove handles overlap: insert at the front of an array by shifting everything right.
     * memcpy here would be undefined behaviour because source and destination overlap. */
    int q[6] = {20, 30, 40, 50, 0, 0};
    size_t used = 4;
    memmove(&q[1], &q[0], used * sizeof q[0]);
    q[0] = 10;
    ++used;
    printf("after memmove insert at front:");
    for (size_t i = 0; i < used; ++i) printf(" %d", q[i]);
    putchar('\n');
    assert(q[0] == 10 && q[1] == 20 && q[4] == 50);
    return 0;
}
