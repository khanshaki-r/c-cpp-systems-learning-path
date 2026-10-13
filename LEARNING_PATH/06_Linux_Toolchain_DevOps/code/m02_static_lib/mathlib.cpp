// m02_static_lib/mathlib.cpp - definitions. After `g++ -c`, `nm -C mathlib.o` shows:
//   T mathlib::add(int, int)     (T = defined in .text, global)
//   T mathlib::mul(int, int)
#include "mathlib.h"

namespace mathlib {
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
}  // namespace mathlib
