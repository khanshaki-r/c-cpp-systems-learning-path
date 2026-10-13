/* m11_lists.c - linked lists the way C programmers actually write them.
 *
 *   Part 1: an owning singly linked list, using a pointer-to-pointer so that inserting or
 *           deleting at the head needs no special case ("the good taste" version).
 *   Part 2: an intrusive circular doubly linked list in the style of the Linux kernel's
 *           struct list_head, with container_of(). One object sits on two lists at once.
 *
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 -o m11_lists m11_lists.c
 * Run:    ./m11_lists
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#error "this file's self-checks use assert(); build it without -DNDEBUG"
#endif

/* ======================= Part 1: singly linked, owning ======================= */
struct snode {
    int           value;
    struct snode *next;
};

static int slist_push_front(struct snode **head, int value)
{
    struct snode *n = malloc(sizeof *n);
    if (!n)
        return -1;
    n->value = value;
    n->next = *head;
    *head = n;
    return 0;
}

/* pp always points at "the link that points to the current node": first the head
 * variable itself, then some node's next field. Writing *pp relinks either one. */
static int slist_insert_sorted(struct snode **head, int value)
{
    struct snode **pp = head;
    while (*pp && (*pp)->value < value)
        pp = &(*pp)->next;
    struct snode *n = malloc(sizeof *n);
    if (!n)
        return -1;
    n->value = value;
    n->next = *pp;
    *pp = n;
    return 0;
}

static size_t slist_remove_all(struct snode **head, int value)
{
    size_t removed = 0;
    struct snode **pp = head;
    while (*pp) {
        if ((*pp)->value == value) {
            struct snode *dead = *pp;
            *pp = dead->next;          /* unlink: works for the head and the middle alike */
            free(dead);
            removed++;
        } else {
            pp = &(*pp)->next;
        }
    }
    return removed;
}

static void slist_free(struct snode **head)
{
    struct snode *n = *head;
    while (n) {
        struct snode *next = n->next;  /* read next BEFORE free: n->next after free is UAF */
        free(n);
        n = next;
    }
    *head = NULL;
}

static size_t slist_to_array(const struct snode *n, int *out, size_t max)
{
    size_t i = 0;
    for (; n && i < max; n = n->next)
        out[i++] = n->value;
    return i;
}

/* ======================= Part 2: intrusive, circular, doubly linked ======================= */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Given a pointer to a member, get the pointer to the struct that contains it. */
#define container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe against removal of pos: the next pointer is read before the body runs. */
#define list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); (pos) = (tmp), (tmp) = (pos)->next)

static void list_init(struct list_head *h)
{
    h->next = h;          /* an empty list points at itself: no NULL checks anywhere */
    h->prev = h;
}

static void list_insert_between(struct list_head *n, struct list_head *prev, struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

static void list_add(struct list_head *n, struct list_head *head)       /* push front */
{
    list_insert_between(n, head, head->next);
}

static void list_add_tail(struct list_head *n, struct list_head *head)  /* push back */
{
    list_insert_between(n, head->prev, head);
}

static void list_del(struct list_head *e)
{
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->next = e->prev = NULL;   /* the kernel uses poison values so a stale use faults loudly */
}

static int list_empty(const struct list_head *h)
{
    return h->next == h;
}

/* A session that is on TWO lists at once with no extra allocation:
 *   all_link - every open session, in creation order
 *   lru_link - most recently used first, so eviction takes from the tail */
struct session {
    int              id;
    char             user[16];
    struct list_head all_link;
    struct list_head lru_link;
};

struct session_table {
    struct list_head all;
    struct list_head lru;
    size_t           count;
};

static struct session *session_open(struct session_table *t, int id, const char *user)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->id = id;
    snprintf(s->user, sizeof s->user, "%s", user);
    list_add_tail(&s->all_link, &t->all);
    list_add(&s->lru_link, &t->lru);
    t->count++;
    return s;
}

static void session_touch(struct session_table *t, struct session *s)
{
    list_del(&s->lru_link);
    list_add(&s->lru_link, &t->lru);      /* O(1) move to front: no search */
}

static struct session *session_evict_lru(struct session_table *t)
{
    if (list_empty(&t->lru))
        return NULL;
    struct session *s = list_entry(t->lru.prev, struct session, lru_link);
    list_del(&s->lru_link);
    list_del(&s->all_link);               /* O(1) removal from the other list too */
    t->count--;
    return s;                             /* caller owns it now */
}

static size_t lru_ids(const struct session_table *t, int *out, size_t max)
{
    size_t n = 0;
    const struct list_head *pos;
    list_for_each(pos, &t->lru) {
        if (n == max) break;
        out[n++] = list_entry(pos, const struct session, lru_link)->id;
    }
    return n;
}

int main(void)
{
    /* ---- Part 1 ---- */
    struct snode *head = NULL;
    int vals[] = { 5, 1, 4, 1, 3, 1 };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        int rc = slist_insert_sorted(&head, vals[i]);
        assert(rc == 0);
    }
    int got[16];
    size_t n = slist_to_array(head, got, 16);
    assert(n == 6 && got[0] == 1 && got[3] == 3 && got[5] == 5);

    size_t removed = slist_remove_all(&head, 1);   /* removes the head three times in a row */
    assert(removed == 3);
    n = slist_to_array(head, got, 16);
    assert(n == 3 && got[0] == 3 && got[1] == 4 && got[2] == 5);
    int rc = slist_push_front(&head, 0);
    assert(rc == 0 && head->value == 0);
    slist_free(&head);
    assert(head == NULL);

    /* ---- Part 2 ---- */
    struct session_table t;
    list_init(&t.all);
    list_init(&t.lru);
    t.count = 0;

    struct session *s[4];
    const char *users[] = { "asha", "bilal", "chen", "divya" };
    for (int i = 0; i < 4; i++) {
        s[i] = session_open(&t, 100 + i, users[i]);
        assert(s[i] != NULL);
    }
    int ids[8];
    n = lru_ids(&t, ids, 8);                       /* newest first: 103 102 101 100 */
    assert(n == 4 && ids[0] == 103 && ids[3] == 100);

    session_touch(&t, s[0]);                       /* 100 becomes most recent */
    n = lru_ids(&t, ids, 8);
    assert(ids[0] == 100 && ids[1] == 103 && ids[3] == 101);

    struct session *victim = session_evict_lru(&t);
    assert(victim == s[1] && t.count == 3);        /* 101 was least recently used */
    printf("evicted session %d (%s); container_of recovered it from &lru_link\n",
           victim->id, victim->user);
    free(victim);

    /* the "all" list still holds the others in creation order */
    struct list_head *pos, *tmp;
    int expect_all[] = { 100, 102, 103 };
    size_t k = 0;
    list_for_each(pos, &t.all)
        assert(list_entry(pos, struct session, all_link)->id == expect_all[k++]);
    assert(k == 3);

    /* tear down with the _safe iterator because the body frees pos's container */
    list_for_each_safe(pos, tmp, &t.all) {
        struct session *x = list_entry(pos, struct session, all_link);
        list_del(&x->all_link);
        list_del(&x->lru_link);
        free(x);
    }
    assert(list_empty(&t.all) && list_empty(&t.lru));

    printf("offsetof(struct session, lru_link) = %zu\n", offsetof(struct session, lru_link));
    puts("all checks passed");
    return 0;
}
