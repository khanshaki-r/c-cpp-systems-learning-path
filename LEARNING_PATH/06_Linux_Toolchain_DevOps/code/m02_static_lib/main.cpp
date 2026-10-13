// m02_static_lib/main.cpp - consumer. After `g++ -c`, `nm -C main.o` shows:
//   U mathlib::add(int, int)     (U = undefined: needed from somewhere else)
//   U mathlib::mul(int, int)
//   T main
#include <cstdio>

#include "mathlib.h"

int main() {
    std::printf("add=%d mul=%d\n", mathlib::add(2, 3), mathlib::mul(4, 5));
    return 0;
}
