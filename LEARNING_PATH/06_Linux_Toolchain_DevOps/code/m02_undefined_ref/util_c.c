/* m02_undefined_ref/util_c.c - case 3: C linkage vs C++ mangling.
 * Compiled with gcc as C, this defines the plain symbol  helper .
 * main.o (C++) wants  _Z6helperi .  Fix: declare it extern "C" in a header both sides include. */
int helper(int x) { return x * 2; }
