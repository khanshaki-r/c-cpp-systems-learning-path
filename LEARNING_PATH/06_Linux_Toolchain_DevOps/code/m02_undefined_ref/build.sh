#!/usr/bin/env bash
# m02_undefined_ref/build.sh - reproduce "undefined reference" three ways and diagnose with nm.
set -uo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
FLAGS="-std=c++17 -Wall -Wextra -Werror"

$CXX $FLAGS -c main.cpp -o main.o
$CXX $FLAGS -c util.cpp -o util.o
$CC  -std=c11 -Wall -Wextra -Werror -c util_c.c -o util_c.o
$CXX $FLAGS -c util_long.cpp -o util_long.o

echo "== what main.o needs:"
nm -C main.o | grep helper          # U helper(int)
echo "   raw mangled name:"
nm main.o | grep helper             # U _Z6helperi

echo
echo "== case 1: forgot to link the object that defines it"
$CXX main.o -o prog 2>&1 | grep -m1 "undefined reference"
echo "   fix: add util.o"
$CXX main.o util.o -o prog && ./prog

echo
echo "== case 2: signature mismatch (helper(long) defined, helper(int) wanted)"
$CXX main.o util_long.o -o prog2 2>&1 | grep -m1 "undefined reference"
echo "   diagnose: compare demangled symbols side by side"
echo "   main.o:      $(nm -C main.o | grep helper)"
echo "   util_long.o: $(nm -C util_long.o | grep helper)"

echo
echo "== case 3: C definition, C++ caller, no extern \"C\""
$CXX main.o util_c.o -o prog3 2>&1 | grep -m1 "undefined reference"
echo "   diagnose: raw names differ in mangling"
echo "   main.o:   $(nm main.o | grep helper)"
echo "   util_c.o: $(nm util_c.o | grep helper)"
echo "   fix: in util.h wrap the declaration:  extern \"C\" int helper(int x);"
