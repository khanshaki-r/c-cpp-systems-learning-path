#!/usr/bin/env bash
# build.sh — builds every C++ example in 04_Concurrency/code on WSL Ubuntu 24.04 (GCC 13).
#
#   ./build.sh            normal build (-O2, -Werror) AND ThreadSanitizer build of every .cpp
#   ./build.sh normal     normal build only
#   ./build.sh tsan       TSan build only
#   ./build.sh asan       AddressSanitizer build only (for m08_treiber_stack and m12_tsan_demo uaf)
#   ./build.sh clean      remove the build directory
#
# Output: build/<name> (normal), build/tsan/<name>, build/asan/<name>.
# The m13_win32_*.c files are Windows-only (MSVC or MinGW) and are skipped here on purpose.
# Several programs contain bugs ON PURPOSE (m02, m12, m06 hang mode); that is expected, and TSan
# reporting them is the lesson, not a build failure.
#
# Run from Windows PowerShell:  wsl -e bash -lc "cd /mnt/d/Cpp_DSA/LEARNING_PATH/04_Concurrency/code && ./build.sh"
set -euo pipefail

cd "$(dirname "$0")"

CXX="${CXX:-g++}"
STD_FLAGS="-std=c++17 -Wall -Wextra -Werror -pthread"
NORMAL_FLAGS="$STD_FLAGS -O2"
# TSan: -O1 keeps stacks readable; -g gives file:line in reports. -Werror kept so a warning that
# only appears at -O1 is still caught.
TSAN_FLAGS="$STD_FLAGS -O1 -g -fsanitize=thread -fno-omit-frame-pointer"
ASAN_FLAGS="$STD_FLAGS -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"

BUILD_DIR="build"
MODE="${1:-all}"

# Every .cpp in this directory is one program. Headers (.hpp) are included, not compiled alone.
mapfile -t SOURCES < <(ls m*.cpp | sort)

failures=0

build_one() {   # $1 = flags, $2 = out dir, $3 = source
    local flags="$1" outdir="$2" src="$3"
    local name="${src%.cpp}"
    mkdir -p "$outdir"
    # shellcheck disable=SC2086
    if $CXX $flags "$src" -o "$outdir/$name"; then
        printf '  ok    %-34s -> %s/%s\n' "$src" "$outdir" "$name"
    else
        printf '  FAIL  %s\n' "$src"
        failures=$((failures + 1))
    fi
}

build_set() {   # $1 = label, $2 = flags, $3 = out dir
    echo "== $1: $2"
    for src in "${SOURCES[@]}"; do
        build_one "$2" "$3" "$src"
    done
}

case "$MODE" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "removed $BUILD_DIR/"
        exit 0
        ;;
    normal)
        build_set "normal" "$NORMAL_FLAGS" "$BUILD_DIR"
        ;;
    tsan)
        build_set "tsan" "$TSAN_FLAGS" "$BUILD_DIR/tsan"
        ;;
    asan)
        build_set "asan" "$ASAN_FLAGS" "$BUILD_DIR/asan"
        ;;
    all)
        build_set "normal" "$NORMAL_FLAGS" "$BUILD_DIR"
        build_set "tsan" "$TSAN_FLAGS" "$BUILD_DIR/tsan"
        ;;
    *)
        echo "usage: $0 [all|normal|tsan|asan|clean]" >&2
        exit 2
        ;;
esac

# The race-point variant of the module 12 demo (deterministic lost wakeup).
if [[ "$MODE" == "all" || "$MODE" == "normal" ]]; then
    echo "== race points: m12_tsan_demo with -DRACE_POINTS"
    mkdir -p "$BUILD_DIR"
    # shellcheck disable=SC2086
    if $CXX $NORMAL_FLAGS -DRACE_POINTS m12_tsan_demo.cpp -o "$BUILD_DIR/m12_tsan_demo_rp"; then
        echo "  ok    m12_tsan_demo.cpp (RACE_POINTS)   -> $BUILD_DIR/m12_tsan_demo_rp"
    else
        echo "  FAIL  m12_tsan_demo.cpp (RACE_POINTS)"
        failures=$((failures + 1))
    fi
fi

echo "skipped (Windows-only, build with cl or MinGW): $(ls m13_win32_*.c 2>/dev/null | tr '\n' ' ')"

if [[ $failures -ne 0 ]]; then
    echo "$failures build(s) failed" >&2
    exit 1
fi
echo "all builds succeeded"
cat <<'EOF'

Suggested runs:
  build/m02_race_demo                      lost updates and torn reads (UB on purpose)
  build/tsan/m02_race_demo                 TSan names both races
  build/m06_deadlock_demo hang             real deadlock; gdb -p $(pgrep m06_deadlock) then: thread apply all bt
  build/tsan/m12_tsan_demo race|order|wakeup
  build/asan/m12_tsan_demo uaf             (after ./build.sh asan)
  RACE_POINTS=wakeup-gap build/m12_tsan_demo_rp wakeup
  build/m10_false_sharing                  close other programs first; numbers are noisy on WSL
  build/m11_rcu_snapshot_cache 3           fewer readers than cores gives the honest publish time

TSan on Ubuntu 24.04: if a TSan binary dies at start with
  "FATAL: ThreadSanitizer: unexpected memory mapping"
the kernel's ASLR entropy is too high for TSan. Either run it as
  setarch $(uname -m) -R build/tsan/<prog>
or once per boot:  sudo sysctl vm.mmap_rnd_bits=28
EOF
