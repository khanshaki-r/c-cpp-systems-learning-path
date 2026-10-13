#!/usr/bin/env bash
# build_debug.sh - build every example in this folder in each instrumentation variant and
# optionally run the Valgrind / sanitizer checks.
#
# Usage (from this directory, in WSL Ubuntu 24.04):
#   ./build_debug.sh                 # build all variants of all programs into build/
#   ./build_debug.sh debug           # only -g -O0 builds
#   ./build_debug.sh asan            # only ASan builds
#   ./build_debug.sh run-asan        # run every ASan build and show the first report lines
#   ./build_debug.sh run-valgrind    # run memcheck on the -O0 builds
#   ./build_debug.sh run-tsan        # run TSan on the concurrency examples
#   ./build_debug.sh run-ubsan       # run UBSan builds
#   ./build_debug.sh clean
#
# Variants and the exact flags (these are the ones to memorise):
#   debug   : -g -O0 -fno-omit-frame-pointer                 best for stepping; slowest
#   opt     : -g -O2 -fno-omit-frame-pointer                 what production looks like; <optimized out>
#   asan    : -g -O1 -fsanitize=address -fno-omit-frame-pointer   heap/stack/global overflow, UAF, double free, leaks
#   tsan    : -g -O1 -fsanitize=thread  -fno-omit-frame-pointer   data races, lock-order inversion (not with ASan)
#   ubsan   : -g -O1 -fsanitize=undefined -fno-sanitize-recover=all   signed overflow, bad shifts, null, misaligned, bounds
#   valgrind: run the "debug" build under valgrind; no special build flags (just -g)
#
# Intentionally buggy programs are compiled WITHOUT -Werror; the clean ones (m02, m11, m13) with it.

set -u
cd "$(dirname "$0")"
mkdir -p build

CXX=${CXX:-g++}
CC=${CC:-gcc}
COMMON_CXX="-std=c++17 -Wall -Wextra -pthread"
COMMON_C="-std=c11 -Wall -Wextra"

BUGGY_CPP="m04_null_deref m04_stack_overflow m04_heap_overflow m04_use_after_free m04_double_free \
           m04_uninit_read m04_integer_overflow_size m04_invalid_iterator m06_crash_example \
           m09_leak m09_growth_not_leak m10_data_race m10_deadlock"
CLEAN_CPP="m02_backtrace_frames m11_crash_handler m13_raii_fix m13_fuzz_target"
C_FILES="m01_memory_layout m03_chunk_inspect"

flags_for() {
  case "$1" in
    debug) echo "-g -O0 -fno-omit-frame-pointer" ;;
    opt)   echo "-g -O2 -fno-omit-frame-pointer" ;;
    asan)  echo "-g -O1 -fsanitize=address -fno-omit-frame-pointer" ;;
    tsan)  echo "-g -O1 -fsanitize=thread -fno-omit-frame-pointer" ;;
    ubsan) echo "-g -O1 -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" ;;
    *) echo "unknown variant $1" >&2; exit 2 ;;
  esac
}

build_variant() {
  local variant=$1 flags; flags=$(flags_for "$variant")
  echo "== building variant: $variant  ($flags)"
  for p in $BUGGY_CPP; do
    # m03/m01 are C; m13_fuzz_target needs no -DFUZZING for the plain build.
    $CXX $COMMON_CXX $flags -o "build/${p}_${variant}" "$p.cpp" || echo "   FAILED: $p ($variant)"
  done
  for p in $CLEAN_CPP; do
    local extra=""
    [ "$p" = m11_crash_handler ] && extra="-rdynamic"
    [ "$p" = m13_raii_fix ] && extra="-D_GLIBCXX_ASSERTIONS -Wshadow -Wconversion -Wsign-conversion"
    $CXX $COMMON_CXX -Werror $flags $extra -o "build/${p}_${variant}" "$p.cpp" || echo "   FAILED: $p ($variant)"
  done
  if [ "$variant" = debug ] || [ "$variant" = opt ]; then
    for p in $C_FILES; do
      $CC $COMMON_C $flags -o "build/${p}_${variant}" "$p.c" || echo "   FAILED: $p ($variant)"
    done
  fi
}

run_asan() {
  # LSan is on by default inside ASan on Linux. halt_on_error=1 stops at the first report.
  export ASAN_OPTIONS="detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=0:print_stats=0:symbolize=1"
  for p in m04_null_deref m04_heap_overflow m04_use_after_free m04_double_free m04_stack_overflow \
           m04_integer_overflow_size m04_invalid_iterator m06_crash_example m09_leak; do
    echo; echo "=================== ASan: $p"
    case $p in
      m04_null_deref) ./build/${p}_asan ACC-9999 2>&1 | head -25 ;;
      m04_integer_overflow_size) ./build/${p}_asan 1073741824 2>&1 | head -25 ;;
      *) ./build/${p}_asan 2>&1 | head -25 ;;
    esac
  done
}

run_valgrind() {
  command -v valgrind >/dev/null || { echo "valgrind not installed: sudo apt install valgrind"; exit 1; }
  VG="valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --num-callers=20 --error-exitcode=99"
  for p in m04_heap_overflow m04_use_after_free m04_double_free m04_uninit_read m09_leak; do
    echo; echo "=================== Valgrind memcheck: $p"
    $VG ./build/${p}_debug 2>&1 | grep -v '^==[0-9]*== *$' | head -40
  done
  echo; echo "=================== Valgrind helgrind: m10_data_race"
  valgrind --tool=helgrind ./build/m10_data_race_debug 2>&1 | head -40
}

run_tsan() {
  export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:detect_deadlocks=1"
  echo "=================== TSan: m10_data_race"
  ./build/m10_data_race_tsan 2>&1 | head -40
  echo; echo "=================== TSan: m10_deadlock (lock-order inversion report; kill after 10s)"
  timeout 10 ./build/m10_deadlock_tsan 2>&1 | head -40
  echo "(If you see 'FATAL: ThreadSanitizer: unexpected memory mapping', run with: setarch \$(uname -m) -R ./build/...)"
}

run_ubsan() {
  export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
  echo "=================== UBSan: m04_integer_overflow_size"
  ./build/m04_integer_overflow_size_ubsan 1073741824 2>&1 | head -20
}

case "${1:-all}" in
  all)   for v in debug opt asan tsan ubsan; do build_variant $v; done ;;
  debug|opt|asan|tsan|ubsan) build_variant "$1" ;;
  run-asan)     run_asan ;;
  run-valgrind) run_valgrind ;;
  run-tsan)     run_tsan ;;
  run-ubsan)    run_ubsan ;;
  clean)        rm -rf build core core.* batch.txt heaptrack.* massif.out.* ;;
  *) echo "usage: $0 [all|debug|opt|asan|tsan|ubsan|run-asan|run-valgrind|run-tsan|run-ubsan|clean]"; exit 2 ;;
esac
