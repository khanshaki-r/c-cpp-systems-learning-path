// m02_shared_lib/mathlib.cpp - compiled with -fPIC so the code can live at any address.
#include "mathlib.h"

namespace mathlib {
int internal_helper(int x) { return x + 1; }
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b + internal_helper(0) - 1; }  // uses the hidden helper
}  // namespace mathlib
