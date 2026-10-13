// m02_static_lib/mathlib.h - public interface of the library.
// Declarations only: a declaration tells the compiler the signature; the linker finds the body.
#pragma once

namespace mathlib {
int add(int a, int b);
int mul(int a, int b);
}  // namespace mathlib
