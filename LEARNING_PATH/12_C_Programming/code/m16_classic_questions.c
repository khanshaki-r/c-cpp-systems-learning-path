/* m16_classic_questions.c - the classic non-string C coding questions, each solved and tested:
 *
 *   linked list: reverse (iterative and recursive), detect a loop, find where the loop starts,
 *                find the middle
 *   bits:        detect endianness, byte swap, count set bits (3 ways), power of two
 *   stack:       a growable int stack, used to check balanced brackets
 *   arrays:      rotate right by k (reversal algorithm), XOR swap and its aliasing trap
 *   sizeof:      an array parameter is a pointer; a pointer-to-array keeps the size
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m16_classic_questions m16_classic_questions.c
 * ASan:   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -o m16_classic_asan m16_classic_questions.c
 * Run:    ./m16_classic_questions
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ======================= linked lists ======================= */
struct node {
    int          val;
    struct node *next;
};

/* Builds the list into a caller-provided array of nodes: no malloc, nothing to free. */
static struct node *list_from(struct node *pool, const int *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pool[i].val = v[i];
        pool[i].next = i + 1 < n ? &pool[i + 1] : NULL;
    }
    return n ? &pool[0] : NULL;
}

static int list_equals(const struct node *h, const int *v, size_t n)
{
    for (size_t i = 0; i < n; i++, h = h->next)
        if (!h || h->val != v[i])
            return 0;
    return h == NULL;
}

/* Three pointers: prev, cur, next. Each step turns one arrow around. O(n) time, O(1) space. */
static struct node *reverse_iter(struct node *head)
{
    struct node *prev = NULL;
    while (head) {
        struct node *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

/* Recursive: elegant, but O(n) stack depth - a million-node list can overflow the stack. */
static struct node *reverse_rec(struct node *head)
{
    if (!head || !head->next)
        return head;
    struct node *new_head = reverse_rec(head->next);
    head->next->next = head;
    head->next = NULL;
    return new_head;
}

/* Floyd: slow moves 1, fast moves 2. If there is a loop they must meet inside it. */
static int has_loop(const struct node *head)
{
    const struct node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast)
            return 1;
    }
    return 0;
}

/* After the meeting, a pointer from the head and one from the meeting point, both moving
 * one step, meet exactly at the first node of the loop. */
static const struct node *loop_start(const struct node *head)
{
    const struct node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            for (slow = head; slow != fast; slow = slow->next, fast = fast->next)
                ;
            return slow;
        }
    }
    return NULL;
}

static const struct node *middle(const struct node *head)   /* second middle for even n */
{
    const struct node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

/* ======================= bits ======================= */
static int is_little_endian(void)
{
    union { uint32_t u; unsigned char b[4]; } x = { .u = 1 };
    return x.b[0] == 1;
}

static uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

static int popcount_naive(uint32_t v)
{
    int c = 0;
    for (; v; v >>= 1)
        c += (int)(v & 1u);
    return c;                       /* loops once per bit position up to the highest set bit */
}

static int popcount_kernighan(uint32_t v)
{
    int c = 0;
    for (; v; v &= v - 1)           /* v & (v - 1) clears the LOWEST set bit */
        c++;
    return c;                       /* loops once per SET bit */
}

static int popcount_builtin(uint32_t v)
{
    return __builtin_popcount(v);   /* one POPCNT instruction with -mpopcnt or -march=native */
}

static int is_power_of_two(uint64_t x)
{
    return x != 0 && (x & (x - 1)) == 0;   /* a power of two has exactly one set bit */
}

/* ======================= stack ======================= */
struct int_stack {
    int   *data;
    size_t len, cap;
};

static int stack_push(struct int_stack *s, int v)
{
    if (s->len == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 8;
        int *p = realloc(s->data, ncap * sizeof *p);
        if (!p)
            return -1;
        s->data = p;
        s->cap = ncap;
    }
    s->data[s->len++] = v;
    return 0;
}

static int stack_pop(struct int_stack *s, int *out)
{
    if (s->len == 0)
        return -1;                  /* underflow is reported, never undefined */
    *out = s->data[--s->len];
    return 0;
}

static int stack_peek(const struct int_stack *s, int *out)
{
    if (s->len == 0)
        return -1;
    *out = s->data[s->len - 1];
    return 0;
}

static void stack_free(struct int_stack *s)
{
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

static int balanced(const char *s)
{
    struct int_stack st = { NULL, 0, 0 };
    int ok = 1;
    for (; *s && ok; s++) {
        char c = *s;
        if (c == '(' || c == '[' || c == '{') {
            if (stack_push(&st, c) != 0)
                ok = 0;
        } else if (c == ')' || c == ']' || c == '}') {
            int open;
            char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (stack_pop(&st, &open) != 0 || open != want)
                ok = 0;
        }
    }
    if (st.len != 0)
        ok = 0;
    stack_free(&st);
    return ok;
}

/* ======================= arrays ======================= */
static void reverse_range(int *a, size_t i, size_t j)   /* inclusive i, exclusive j */
{
    while (i + 1 < j) {
        int t = a[i];
        a[i++] = a[--j];
        a[j] = t;
    }
}

/* Rotate right by k: reverse all, reverse the first k, reverse the rest. O(n), O(1) space. */
static void rotate_right(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    k %= n;
    reverse_range(a, 0, n);
    reverse_range(a, 0, k);
    reverse_range(a, k, n);
}

static void xor_swap(int *x, int *y)
{
    if (x == y)
        return;                     /* without this, xor_swap(&a, &a) sets a to 0 */
    *x ^= *y;
    *y ^= *x;
    *x ^= *y;
}

/* ======================= sizeof and arrays ======================= */
/* "int a[10]" as a parameter IS "int *a": sizeof a would be 8, and GCC warns
 * (-Wsizeof-array-argument). A pointer to an array keeps the length in the type. */
static size_t size_via_array_pointer(int (*a)[10])
{
    return sizeof *a;               /* 40: the whole array */
}

int main(void)
{
    /* lists */
    struct node pool[8];
    int v[] = { 1, 2, 3, 4, 5 };
    int r[] = { 5, 4, 3, 2, 1 };
    struct node *h = list_from(pool, v, 5);
    h = reverse_iter(h);
    assert(list_equals(h, r, 5));
    h = reverse_rec(h);
    assert(list_equals(h, v, 5));
    assert(reverse_iter(NULL) == NULL);
    assert(middle(h)->val == 3);

    assert(!has_loop(h) && loop_start(h) == NULL);
    pool[4].next = &pool[2];        /* 1 -> 2 -> 3 -> 4 -> 5 -> back to 3 */
    assert(has_loop(h));
    assert(loop_start(h) == &pool[2]);
    pool[4].next = &pool[0];        /* whole list is the loop */
    assert(loop_start(h) == &pool[0]);
    pool[4].next = NULL;

    /* bits */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    assert(is_little_endian());
#endif
    assert(bswap32(0x11223344u) == 0x44332211u);
    assert(bswap32(0x11223344u) == __builtin_bswap32(0x11223344u));
    uint32_t x = 0x9E3779B9u;
    for (int i = 0; i < 100000; i++, x = x * 1664525u + 1013904223u) {
        int a = popcount_naive(x), b = popcount_kernighan(x), c = popcount_builtin(x);
        assert(a == b && b == c);
    }
    assert(popcount_kernighan(0) == 0 && popcount_kernighan(0xFFFFFFFFu) == 32);
    assert(is_power_of_two(1) && is_power_of_two(1024) && is_power_of_two(1ULL << 63));
    assert(!is_power_of_two(0) && !is_power_of_two(6) && !is_power_of_two(UINT64_MAX));

    /* stack */
    struct int_stack st = { NULL, 0, 0 };
    int out;
    assert(stack_pop(&st, &out) == -1);
    for (int i = 0; i < 100; i++)
        assert(stack_push(&st, i) == 0);
    assert(stack_peek(&st, &out) == 0 && out == 99);
    for (int i = 99; i >= 0; i--)
        assert(stack_pop(&st, &out) == 0 && out == i);
    stack_free(&st);
    assert(balanced("{[()()]}") && balanced("") && balanced("f(a[1], {b})"));
    assert(!balanced("(]") && !balanced("((") && !balanced(")("));

    /* rotate: compare against a naive copy for every k */
    for (size_t n = 0; n <= 7; n++) {
        for (size_t k = 0; k <= 2 * n + 1; k++) {
            int a[8], want[8];
            for (size_t i = 0; i < n; i++)
                a[i] = (int)i;
            for (size_t i = 0; i < n; i++)
                want[(i + k) % n] = (int)i;
            rotate_right(a, n, k);
            assert(n == 0 || memcmp(a, want, n * sizeof a[0]) == 0);
        }
    }

    /* xor swap */
    int p = 3, q = 7;
    xor_swap(&p, &q);
    assert(p == 7 && q == 3);
    xor_swap(&p, &p);
    assert(p == 7);

    /* sizeof */
    int arr[10];
    assert(sizeof arr == 40 && sizeof arr / sizeof arr[0] == 10);
    assert(size_via_array_pointer(&arr) == 40);
    assert(sizeof(&arr[0]) == sizeof(int *));

    printf("endianness: %s; popcount(0x9E3779B9) = %d\n",
           is_little_endian() ? "little" : "big", popcount_kernighan(0x9E3779B9u));
    puts("all checks passed");
    return 0;
}
