// m02_shared_lib/mathlib.h - public interface, identical to the static demo.
// MATHLIB_API hides every symbol except the ones we mark when built with -fvisibility=hidden.
#pragma once

#if defined(_WIN32)
#  if defined(MATHLIB_BUILDING)
#    define MATHLIB_API __declspec(dllexport)
#  else
#    define MATHLIB_API __declspec(dllimport)
#  endif
#else
#  define MATHLIB_API __attribute__((visibility("default")))
#endif

namespace mathlib {
MATHLIB_API int add(int a, int b);
MATHLIB_API int mul(int a, int b);
// Not exported: internal helper. With -fvisibility=hidden, `nm -D` will not list it.
int internal_helper(int x);
}  // namespace mathlib
