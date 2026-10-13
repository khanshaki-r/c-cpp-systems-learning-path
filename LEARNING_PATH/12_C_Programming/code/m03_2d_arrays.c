/* m03_2d_arrays.c - true 2-D arrays, pointers to rows, and arrays of pointers (jagged arrays).
 *
 * Module: 12_C_Programming/03_pointers_from_the_ground_up.md
 * Build:  gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m03_2d_arrays.c -o m03_2d_arrays
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define ROWS 3
#define COLS 4

/* int m[][COLS] and int (*m)[COLS] are the same parameter type: pointer to an array of COLS ints.
 * The column count must be known, because m[r] has to skip r * COLS ints. */
static int sum_rows(const int (*m)[COLS], int rows)
{
    int s = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < COLS; ++c) s += m[r][c];
    return s;
}

/* The flat view: works for any width, you do the index arithmetic yourself. */
static int sum_flat(const int *m, int rows, int cols)
{
    int s = 0;
    for (int i = 0; i < rows * cols; ++i) s += m[i];
    return s;
}

int main(void)
{
    int grid[ROWS][COLS];
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) grid[r][c] = r * 10 + c;

    /* Row-major: row 1 starts immediately after the last element of row 0. */
    assert(&grid[1][0] == &grid[0][COLS - 1] + 1);
    printf("sizeof grid = %zu, sizeof grid[0] = %zu, sizeof grid[0][0] = %zu\n",
           sizeof grid, sizeof grid[0], sizeof grid[0][0]);

    int (*row)[COLS] = grid;        /* grid decays to a pointer to its first ROW */
    printf("row + 1 moves %td bytes (one whole row)\n", (char *)(row + 1) - (char *)row);
    assert(row[2][3] == 23);

    int expected = 0 + 1 + 2 + 3 + 10 + 11 + 12 + 13 + 20 + 21 + 22 + 23;
    assert(sum_rows((const int (*)[COLS])grid, ROWS) == expected);
    assert(sum_flat(&grid[0][0], ROWS, COLS) == expected);
    printf("sum_rows = sum_flat = %d\n", expected);

    /* Dynamic but CONTIGUOUS: one malloc, one free, still indexable as dyn[r][c]. */
    int rows = 5;
    int (*dyn)[COLS] = malloc((size_t)rows * sizeof *dyn);
    if (dyn == NULL) return 1;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < COLS; ++c) dyn[r][c] = 1;
    printf("contiguous dynamic %dx%d: sizeof *dyn = %zu, sum = %d\n", rows, COLS, sizeof *dyn,
           sum_rows((const int (*)[COLS])dyn, rows));
    free(dyn);

    /* Jagged: an array of row POINTERS, each row a separate allocation of its own length.
     * Same jag[r][c] syntax, completely different memory: jag[r] LOADS a pointer first. */
    int lens[ROWS] = {1, 3, 2};
    int **jag = malloc(ROWS * sizeof *jag);
    if (jag == NULL) return 1;
    for (int r = 0; r < ROWS; ++r) {
        jag[r] = malloc((size_t)lens[r] * sizeof **jag);
        if (jag[r] == NULL) return 1;
        for (int c = 0; c < lens[r]; ++c) jag[r][c] = r + c;
    }
    printf("jagged rows:");
    for (int r = 0; r < ROWS; ++r) {
        printf(" [");
        for (int c = 0; c < lens[r]; ++c) printf("%s%d", c ? " " : "", jag[r][c]);
        printf("]");
    }
    putchar('\n');
    /* sum_rows(jag, ROWS) does not compile: int ** is not int (*)[COLS]. A cast would make it
     * compile and then read the row pointers as if they were ints. */
    for (int r = 0; r < ROWS; ++r) free(jag[r]);
    free(jag);
    return 0;
}
