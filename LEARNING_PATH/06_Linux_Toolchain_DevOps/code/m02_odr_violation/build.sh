#!/usr/bin/env bash
# m02_odr_violation/build.sh - a loud ODR violation, two fixes, then a silent one and -Wodr.
set -uo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -Wall -Wextra -Werror"

echo "== 1. loud: definition in a header included by two TUs"
$CXX $FLAGS -c a.cpp -o a.o && $CXX $FLAGS -c b.cpp -o b.o
$CXX a.o b.o -o prog 2>&1 | grep -m2 -E "multiple definition|first defined"
echo "   nm shows a strong data symbol (B/D) in BOTH objects:"
nm a.o | grep g_counter; nm b.o | grep g_counter

echo
echo "== 2. fix 1: extern in header, one definition in a.cpp"
$CXX $FLAGS -DUSE_FIX=1 -c a.cpp -o a.o && $CXX $FLAGS -DUSE_FIX=1 -c b.cpp -o b.o
$CXX a.o b.o -o prog_fix1 && ./prog_fix1
echo "   nm: defined (B) in a.o only, undefined (U) in b.o"
nm a.o | grep g_counter; nm b.o | grep g_counter

echo
echo "== 3. fix 2: C++17 inline variable"
$CXX $FLAGS -DUSE_FIX=2 -c a.cpp -o a.o && $CXX $FLAGS -DUSE_FIX=2 -c b.cpp -o b.o
$CXX a.o b.o -o prog_fix2 && ./prog_fix2
echo "   nm: weak/unique (u or V) in both; linker keeps one copy"
nm a.o | grep g_counter; nm b.o | grep g_counter

echo
echo "== 4. SILENT: two TUs, two layouts of struct Config, no linker error"
cd silent_odr
$CXX $FLAGS -DWITH_DEBUG_FIELDS -c writer.cpp -o writer.o
$CXX $FLAGS -c reader.cpp -o reader.o
$CXX writer.o reader.o -o silent && echo "   linked fine. Running:" && ./silent
echo "   (expected port=7000 max_connections=128; got garbage: fields shifted by 4 bytes)"

echo
echo "== 5. the same build with LTO and -Wodr"
$CXX $FLAGS -flto -DWITH_DEBUG_FIELDS -c writer.cpp -o writer_lto.o
$CXX $FLAGS -flto -c reader.cpp -o reader_lto.o
$CXX -flto -Wodr writer_lto.o reader_lto.o -o silent_lto 2>&1 | grep -A3 -m1 "Wodr\|odr" || \
    echo "   (no -Wodr output: GCC detects type mismatches of types used across TUs; see the module text)"
