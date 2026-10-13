#!/usr/bin/env bash
# m02_shared_lib/build.sh - build a shared library, fail to load it, then fix it three ways.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -Wall -Wextra -Werror"

echo "== 1. compile with -fPIC and hidden visibility"
$CXX $FLAGS -fPIC -fvisibility=hidden -DMATHLIB_BUILDING -c mathlib.cpp -o mathlib.o

echo "== 2. link the shared object with a soname"
$CXX -shared -Wl,-soname,libmathlib.so.1 -o libmathlib.so.1.0.0 mathlib.o
ln -sf libmathlib.so.1.0.0 libmathlib.so.1     # soname: what consumers record in NEEDED
ln -sf libmathlib.so.1 libmathlib.so            # dev name: what -lmathlib resolves at link time
echo "--- nm -D (exported symbols only: add and mul, NOT internal_helper)"
nm -D -C libmathlib.so | grep -i mathlib || true
echo "--- readelf -d libmathlib.so | grep SONAME"
readelf -d libmathlib.so | grep SONAME

echo "== 3. compile and link the consumer"
$CXX $FLAGS -c main.cpp -o main.o
$CXX main.o -L. -lmathlib -o prog_norpath
echo "--- readelf -d prog_norpath | grep NEEDED  (records the soname, not the path)"
readelf -d prog_norpath | grep NEEDED

echo "== 4. run without help: the loader cannot find libmathlib.so.1"
set +e
./prog_norpath
echo "exit status $?  (127 = loader failure)"
set -e
echo "--- ldd prog_norpath"
ldd prog_norpath | grep mathlib || true

echo "== 5a. fix 1: LD_LIBRARY_PATH (testing only)"
LD_LIBRARY_PATH=. ./prog_norpath

echo "== 5b. fix 2: bake a RUNPATH relative to the executable"
$CXX main.o -L. -lmathlib -Wl,-rpath,'$ORIGIN' -o prog_rpath
./prog_rpath
echo "--- readelf -d prog_rpath | grep -E 'RUNPATH|RPATH'"
readelf -d prog_rpath | grep -E 'RUNPATH|RPATH'
echo "--- ldd prog_rpath"
ldd prog_rpath | grep mathlib

echo "== 5c. fix 3 (commented out: needs sudo): install system-wide and run ldconfig"
echo "    sudo cp libmathlib.so.1.0.0 /usr/local/lib/ && sudo ldconfig && ./prog_norpath"

echo "== 6. watch the loader search"
LD_DEBUG=libs ./prog_rpath 2>&1 | grep -E 'find library|trying file' | head -8

echo "== 7. a shared lib is a complete ELF with its own dependencies"
ldd libmathlib.so
