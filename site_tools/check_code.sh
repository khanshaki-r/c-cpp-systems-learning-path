#!/usr/bin/env bash
# Compile-check every C and C++ example in the curriculum (syntax and semantics, no linking).
# Used by .github/workflows/code-check.yml and runnable locally on Ubuntu 24.04 / WSL:
#
#   bash site_tools/check_code.sh
#
# C++: g++ -std=c++17 -Wall -Wextra -Werror (the course standard).
# C:   gcc -std=c17 -Wall -Wextra (-Werror for the C course itself).
# Programs that are buggy on purpose (05_Memory_Debugging_RCA) are checked without -Werror.
# Windows-only sources (#include <windows.h>) are skipped on Linux.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)/LEARNING_PATH"
ERRLOG="$(mktemp)"
pass=0
fail=0
skip=0

check() {  # check <compiler> <file> <flags...>
    local cc="$1" f="$2"
    shift 2
    local dir up up2
    dir="$(dirname "$f")"
    up="$(dirname "$dir")"
    up2="$(dirname "$up")"
    if "$cc" "$@" -fsyntax-only -I"$dir" -I"$dir/include" -I"$up/include" -I"$up2/include" \
        -I"$up" -I"$up/tests" -I/usr/include/postgresql "$f" 2>"$ERRLOG.one"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        { echo "=== $f"; head -n 15 "$ERRLOG.one"; } >>"$ERRLOG"
    fi
}

while IFS= read -r -d '' f; do
    case "$f" in
        */05_Memory_Debugging_RCA/code/*)
            check g++ "$f" -std=c++17 -Wall -Wextra -pthread '-DTCPKV_VERSION="check"' ;;
        *)
            check g++ "$f" -std=c++17 -Wall -Wextra -Werror -pthread '-DTCPKV_VERSION="check"' ;;
    esac
done < <(find "$ROOT" \( -name build -o -name 'build-*' \) -prune -o -name '*.cpp' -print0)

while IFS= read -r -d '' f; do
    if grep -q '#include <windows.h>' "$f"; then
        skip=$((skip + 1))
        continue
    fi
    case "$f" in
        */12_C_Programming/code/*)
            if grep -qi 'deliberate\|undefined behaviour on purpose\|without -Werror' "$f"; then
                check gcc "$f" -std=c17 -Wall -Wextra -pthread
            else
                check gcc "$f" -std=c17 -Wall -Wextra -Wpedantic -Werror -pthread
            fi ;;
        *)
            check gcc "$f" -std=gnu17 -Wall -Wextra -pthread ;;
    esac
done < <(find "$ROOT" \( -name build -o -name 'build-*' \) -prune -o -name '*.c' -print0)

echo "checked: $pass passed, $fail failed, $skip skipped (Windows-only)"
if [ "$fail" -gt 0 ]; then
    cat "$ERRLOG"
    exit 1
fi
