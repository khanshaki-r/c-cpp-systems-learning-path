#!/usr/bin/env bash
# scripts/run_tests.sh - build, run unit tests, then an end-to-end test against a live server.
#
#   scripts/run_tests.sh                  debug preset: unit + integration
#   scripts/run_tests.sh asan             same under ASan + UBSan
#   scripts/run_tests.sh tsan -R Store    extra arguments go to ctest
#   UNIT_ONLY=1 scripts/run_tests.sh      skip the live-server part
#
# Exit status: 0 only if every stage passed. That is the contract CI relies on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

log() { printf '[test] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

PRESET="${1:-debug}"
if (($#)); then shift; fi
BUILD_DIR="build/$PRESET"

# ---------------------------------------------------------------- 1. build
"$ROOT/scripts/build.sh" "$PRESET"

# ---------------------------------------------------------------- 2. unit tests
log "ctest --preset $PRESET $*"
ctest --preset "$PRESET" --output-junit "$ROOT/$BUILD_DIR/junit.xml" "$@"

if [[ "${UNIT_ONLY:-0}" == 1 ]]; then
    log "UNIT_ONLY=1: skipping integration"
    exit 0
fi

# ---------------------------------------------------------------- 3. integration against a live server
command -v python3 >/dev/null || die "python3 not found"

# Pick a free port by asking the kernel for one (bind to port 0), so parallel CI jobs never collide.
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
SERVER_LOG="$BUILD_DIR/integration-server.log"
SERVER_PID=""

stop_server() {
    local rc=$?
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID"                       # graceful: tcpkv drains and exits 0
        # Give it 5 s, then insist. Same policy as systemd TimeoutStopSec and docker stop.
        for _ in $(seq 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if ((rc != 0)) && [[ -f "$SERVER_LOG" ]]; then
        log "server log (last 20 lines):"
        tail -n 20 "$SERVER_LOG" >&2
    fi
    exit "$rc"
}
trap stop_server EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

log "starting $BUILD_DIR/tcpkv on port $PORT"
"$BUILD_DIR/tcpkv" "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

# The client waits for the port to accept connections, then runs its self-test.
python3 "$ROOT/scripts/tcpkv_client.py" --port "$PORT" --wait 5 --selftest

# A short load burst catches crashes that only show up with concurrent clients.
"$ROOT/scripts/load_test.sh" "$PORT" 4 2000

kill -0 "$SERVER_PID" 2>/dev/null || die "server died during the integration run"
log "all tests passed"
