/* m01_two_units.c - one file that can be built as ONE program or as TWO translation units.
 *
 * Module: 12_C_Programming/01_how_c_really_works.md
 *
 * Normal build (one translation unit, both halves switched on):
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g m01_two_units.c -o m01_two_units
 *
 * Split build (the point of the exercise): compile each half separately, then link.
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -DUNIT_A -c m01_two_units.c -o unit_a.o
 *   gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -g -DUNIT_B -c m01_two_units.c -o unit_b.o
 *   gcc unit_a.o unit_b.o -o m01_split && ./m01_split
 *   nm unit_a.o unit_b.o
 *     T = function defined here, B = zero-initialised data defined here (.bss),
 *     d/t lowercase = internal linkage (static), U = used here, defined elsewhere (linker must find it)
 *
 * Then try: link only unit_b.o  ->  "undefined reference to `next_id'"   (a LINKER error)
 */

#if !defined(UNIT_A) && !defined(UNIT_B)
#define UNIT_A
#define UNIT_B
#endif

/* ---- What a header would contain: DECLARATIONS both units agree on. No storage here. ---- */
extern int g_requests;          /* declaration: "an int called g_requests exists somewhere" */
int next_id(void);              /* declaration (prototype) of a function defined in unit A */
const char *unit_a_name(void);

#ifdef UNIT_A
/* ---- Unit A: the DEFINITIONS. Exactly one translation unit may define each external name. ---- */
int g_requests = 0;             /* definition: storage is reserved here, external linkage */
static int s_next = 1000;       /* internal linkage: unit B cannot name this object */

static int bump(int *p)         /* internal linkage function: invisible outside this unit */
{
    return (*p)++;
}

int next_id(void)
{
    ++g_requests;
    return bump(&s_next);
}

const char *unit_a_name(void) { return "unit A"; }
#endif

#ifdef UNIT_B
/* ---- Unit B: uses the names. The compiler trusts the declarations; the linker checks them. ---- */
#include <assert.h>
#include <stdio.h>

int main(void)
{
    int a = next_id();
    int b = next_id();
    assert(a == 1000 && b == 1001);
    assert(g_requests == 2);
    printf("ids %d and %d came from %s; g_requests = %d\n", a, b, unit_a_name(), g_requests);
    return 0;
}
#endif
