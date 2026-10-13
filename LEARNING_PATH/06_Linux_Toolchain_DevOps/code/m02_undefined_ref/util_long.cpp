// m02_undefined_ref/util_long.cpp - case 2: signature mismatch.
// The header declares helper(int); this file defines helper(long). Different mangled names:
//   main.o wants  _Z6helperi   (helper(int))
//   this defines  _Z6helperl   (helper(long))
// It compiles fine (no header included, so no conflict is visible) and fails at link.
long helper(long x) { return x * 2; }
