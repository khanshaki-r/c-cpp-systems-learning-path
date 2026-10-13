#!/usr/bin/env bash
# scripts/bisect_test.sh - the test script for `git bisect run` (module 06, exercise 5).
#
#   git bisect start <bad> <good>
#   git bisect run ./scripts/bisect_test.sh 'StoreTest.*'
#   git bisect reset
#
# Exit codes are the protocol git bisect understands:
#   0          this commit is good
#   1..124     this commit is bad
#   125        this commit cannot be tested (does not build): skip it
#   >= 128     abort the whole bisect (reserved; we never use it)
#
# Note: copy this file OUTSIDE the repo (or commit it before the 'good' commit), otherwise old
# commits checked out by bisect will not have it.
set -uo pipefail          # no -e: we decide the exit code ourselves

FILTER="${1:-ParseCommand.TrailingCarriageReturnIsStripped}"
BUILD_DIR="build/bisect"

# A dedicated build dir so bisect never clobbers your normal debug build.
if ! cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTCPKV_WERROR=OFF >/dev/null; then
    echo "configure failed: skip" >&2
    exit 125
fi
if ! cmake --build "$BUILD_DIR" >/dev/null; then
    echo "build failed: skip" >&2
    exit 125
fi

# timeout turns a hang (deadlock introduced by the bad commit) into "bad" instead of a stuck bisect.
timeout 120 ctest --test-dir "$BUILD_DIR" -R "$FILTER" --output-on-failure >/dev/null
rc=$?
if ((rc == 0)); then exit 0; fi
echo "tests '$FILTER' failed (ctest exit $rc): bad" >&2
exit 1
