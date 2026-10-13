#!/usr/bin/env bash
# m02_static_lib/build.sh - build a static library by hand and watch the symbols move.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -Wall -Wextra -Werror"

echo "== 1. compile library object"
$CXX $FLAGS -c mathlib.cpp -o mathlib.o
echo "--- nm -C mathlib.o (expect T for add and mul)"
nm -C mathlib.o | grep mathlib

echo "== 2. archive it"
ar rcs libmathlib.a mathlib.o        # r: insert/replace, c: create, s: write symbol index
echo "--- ar t libmathlib.a (members)"
ar t libmathlib.a

echo "== 3. compile the consumer (does not need the library yet)"
$CXX $FLAGS -c main.cpp -o main.o
echo "--- nm -C main.o (expect U for add and mul: undefined here)"
nm -C main.o | grep mathlib

echo "== 4. link, correct order: objects first, then libraries"
$CXX main.o -L. -lmathlib -o prog_static
echo "--- nm -C prog_static (expect T: resolved)"
nm -C prog_static | grep mathlib
echo "--- run"
./prog_static

echo "== 5. link, WRONG order: library before the object that needs it"
if $CXX -L. -lmathlib main.o -o prog_wrong 2> wrong.log; then
    echo "(this linker resolved it anyway - lld and mold do; GNU ld does not)"
    ./prog_wrong
else
    echo "--- link failed as expected; first lines of the error:"
    head -3 wrong.log
fi

echo "== 6. size: the library code is copied into the executable"
size prog_static
echo "--- ldd prog_static (no libmathlib: it is inside)"
ldd prog_static
