#!/usr/bin/env bash
# Runs tcpkv_server under Valgrind memcheck, drives it with load_gen, stops it with SIGINT and
# fails if memcheck reported any error or any definite/indirect leak.
#
#   bash tools/valgrind_check.sh build/relwithdebinfo
#
# Build with RelWithDebInfo: optimised enough to finish in CI, with symbols for readable reports.
set -euo pipefail

BUILD_DIR=${1:-build/relwithdebinfo}
PORT=${PORT:-7400}
CLIENTS=${CLIENTS:-8}
DURATION=${DURATION:-5}
VALGRIND_EXIT=42

if ! command -v valgrind >/dev/null; then
    echo "valgrind not installed (sudo apt install valgrind)" >&2
    exit 2
fi

valgrind \
    --quiet \
    --error-exitcode=$VALGRIND_EXIT \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    "$BUILD_DIR/tcpkv_server" --port "$PORT" --threads "$CLIENTS" --stats-interval 0 &
# --threads must be >= --clients: a connection owns its worker for its whole lifetime, so with
# fewer threads the extra load_gen clients wait in the queue and never run concurrently.
SERVER_PID=$!

# Wait until the server answers PING (Valgrind makes start-up slow).
for _ in $(seq 1 100); do
    if "$BUILD_DIR/tcpkv_cli" -p "$PORT" ping >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done

if ! "$BUILD_DIR/tcpkv_cli" -p "$PORT" ping >/dev/null 2>&1; then
    echo "server did not come up on port $PORT" >&2
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" || true
    exit 1
fi

set +e
"$BUILD_DIR/load_gen" --port "$PORT" --clients "$CLIENTS" --duration "$DURATION" --keyspace 1000
LOAD_STATUS=$?
set -e

kill -INT "$SERVER_PID"
set +e
wait "$SERVER_PID"
SERVER_STATUS=$?
set -e

echo "load_gen exit: $LOAD_STATUS, server exit: $SERVER_STATUS"
if [ "$SERVER_STATUS" -eq "$VALGRIND_EXIT" ]; then
    echo "valgrind reported errors" >&2
    exit 1
fi
if [ "$SERVER_STATUS" -ne 0 ]; then
    echo "server exited abnormally" >&2
    exit 1
fi
if [ "$LOAD_STATUS" -ne 0 ]; then
    echo "load generator reported errors or mismatches" >&2
    exit 1
fi
echo "valgrind: clean"
