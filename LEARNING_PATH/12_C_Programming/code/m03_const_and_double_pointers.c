/* m03_const_and_double_pointers.c - const placement, and pointers to pointers.
 *
 * Module: 12_C_Programming/03_pointers_from_the_ground_up.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_const_and_double_pointers.c -o m03_const_and_double_pointers
 *
 * Read a declaration right to left, starting at the name:
 *   const int *p         p is a pointer to an int that is const    (move p: yes, write *p: no)
 *   int const *p         exactly the same
 *   int *const p         p is a const pointer to an int            (move p: no,  write *p: yes)
 *   const int *const p   neither
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct node {
    int          value;
    struct node *next;
};

/* The function must change the CALLER's head pointer, so it needs the address of that pointer. */
static int push_front(struct node **head, int value)
{
    struct node *n = malloc(sizeof *n);
    if (n == NULL) return -1;
    n->value = value;
    n->next = *head;
    *head = n;                /* with "struct node *head" this would update a local copy only */
    return 0;
}

/* Pointer-to-pointer removal: `link` points at whichever pointer leads to the current node
 * (the head variable, or some node's next field), so the head needs no special case. */
static void remove_all(struct node **link, int value)
{
    while (*link != NULL) {
        if ((*link)->value == value) {
            struct node *dead = *link;
            *link = dead->next;
            free(dead);
        } else {
            link = &(*link)->next;
        }
    }
}

static void free_list(struct node *n)
{
    while (n != NULL) {
        struct node *next = n->next;
        free(n);
        n = next;
    }
}

/* Out-parameter allocation. On success returns 0 and *out owns a malloc'd string (caller frees).
 * On failure returns -1 and *out is untouched. */
static int make_greeting(const char *name, char **out)
{
    size_t n = strlen("Hello, ") + strlen(name) + 1;
    char *s = malloc(n);
    if (s == NULL) return -1;
    snprintf(s, n, "Hello, %s", name);
    *out = s;
    return 0;
}

/* const char *s: a promise not to write through s. Moving s itself is fine. */
static size_t count_char(const char *s, char c)
{
    size_t n = 0;
    for (; *s != '\0'; ++s)
        if (*s == c) ++n;
    return n;
}

int main(void)
{
    int value = 1, other = 2;
    const int *pc = &value;   /* *pc = 5;  -> error: assignment of read-only location */
    pc = &other;              /* allowed: the pointer itself is not const */
    int *const cp = &value;   /* cp = &other; -> error: assignment of read-only variable */
    *cp = 10;                 /* allowed: the int is not const */
    assert(value == 10 && *pc == 2);

    struct node *head = NULL;
    const int input[] = {7, 2, 7, 4, 7};
    for (size_t i = 0; i < sizeof input / sizeof input[0]; ++i)
        if (push_front(&head, input[i]) != 0) return 1;

    printf("list before:");
    for (const struct node *n = head; n != NULL; n = n->next) printf(" %d", n->value);
    remove_all(&head, 7);
    printf("\nlist after removing every 7:");
    for (const struct node *n = head; n != NULL; n = n->next) printf(" %d", n->value);
    putchar('\n');
    assert(head != NULL && head->value == 4 && head->next->value == 2 && head->next->next == NULL);
    free_list(head);

    char *greeting = NULL;
    if (make_greeting("Shakir", &greeting) == 0) {
        printf("%s (%zu letters 'a')\n", greeting, count_char(greeting, 'a'));
        assert(strcmp(greeting, "Hello, Shakir") == 0);
        free(greeting);       /* we own it: the contract of make_greeting says so */
    }
    return 0;
}
