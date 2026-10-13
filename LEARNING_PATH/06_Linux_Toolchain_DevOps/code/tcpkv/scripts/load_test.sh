#!/usr/bin/env bash
# scripts/load_test.sh - concurrent load generator for tcpkv (module 10; used in modules 07 and 08).
#
#   scripts/load_test.sh [PORT] [CLIENTS] [OPS_PER_CLIENT] [VALUE_BYTES]
#   scripts/load_test.sh 7000 8 20000            8 clients x 20,000 requests, 16-byte values
#   scripts/load_test.sh 7000 4 50000 65000      big values: memory growth / OOM exercise (module 08)
#   HOST=10.0.0.5 scripts/load_test.sh 7000      another host
#
# Each client is one TCP connection that pipelines its requests: a request file is generated
# up front, streamed through nc, and every reply line is counted. Workload per client:
# 30 % SET, 60 % GET, 10 % DEL over a per-client key space, so clients never share keys and the
# result is checkable. Pass condition: replies == requests and zero ERROR replies.
#
# This measures the server's throughput for pipelined traffic, not per-request latency.
# For latency percentiles use scripts/tcpkv_client.py --bench.
set -euo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${1:-7000}"
CLIENTS="${2:-4}"
OPS="${3:-10000}"
VALUE_BYTES="${4:-16}"

log() { printf '[load] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

[[ "$PORT$CLIENTS$OPS$VALUE_BYTES" =~ ^[0-9]+$ ]] || die "all arguments must be positive integers"
((CLIENTS >= 1 && OPS >= 1)) || die "CLIENTS and OPS must be >= 1"
((VALUE_BYTES >= 1 && VALUE_BYTES <= 65536)) || die "VALUE_BYTES must be 1..65536 (kMaxValueLength)"
command -v nc >/dev/null || die "nc not found (sudo apt install netcat-openbsd)"
# -N (shut down the write side at EOF) is a netcat-openbsd flag; the traditional nc lacks it.
nc -h 2>&1 | grep -qE -- '(^|[[:space:]])-N' || die "need netcat-openbsd for nc -N"

WORKDIR="$(mktemp -d)"
cleanup() {
    local rc=$?
    local pids
    pids="$(jobs -p)"
    if [[ -n "$pids" ]]; then kill $pids 2>/dev/null || true; fi
    rm -r -- "$WORKDIR"
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Fail fast with a clear message if nothing is listening.
printf 'PING\n' | nc -N -w 2 "$HOST" "$PORT" | grep -q '^PONG' \
    || die "no tcpkv answering PING on $HOST:$PORT"

# ---------------------------------------------------------------- generate request files
# awk is far faster than a bash loop for 100k lines. srand(client) makes runs reproducible.
log "generating $CLIENTS x $OPS requests (value $VALUE_BYTES bytes)"
for ((c = 1; c <= CLIENTS; c++)); do
    awk -v c="$c" -v n="$OPS" -v vb="$VALUE_BYTES" 'BEGIN {
        srand(c)
        val = ""; for (j = 0; j < vb; j++) val = val "x"    # a value of vb x characters
        keys = 1000                                         # per-client key space
        for (i = 0; i < n; i++) {
            k = "c" c "_k" int(rand() * keys)
            r = rand()
            if      (r < 0.30) print "SET " k " " val
            else if (r < 0.90) print "GET " k
            else               print "DEL " k
        }
    }' > "$WORKDIR/req_$c.txt"
done

# ---------------------------------------------------------------- run clients in parallel
log "running $CLIENTS clients against $HOST:$PORT"
start_ns=$(date +%s%N)
pids=()
for ((c = 1; c <= CLIENTS; c++)); do
    # -N: send FIN after the last request; the server answers everything, sees EOF, closes,
    # and nc exits. -w 30: give up if the server stops responding for 30 s.
    nc -N -w 30 "$HOST" "$PORT" < "$WORKDIR/req_$c.txt" > "$WORKDIR/resp_$c.txt" &
    pids+=("$!")
done

failed=0
for p in "${pids[@]}"; do
    wait "$p" || failed=$((failed + 1))
done
end_ns=$(date +%s%N)

# ---------------------------------------------------------------- verify and report
total_req=$((CLIENTS * OPS))
total_resp=0
errors=0
for ((c = 1; c <= CLIENTS; c++)); do
    n=$(wc -l < "$WORKDIR/resp_$c.txt")
    e=$(grep -c '^ERROR' "$WORKDIR/resp_$c.txt" || true)   # grep -c exits 1 when the count is 0
    total_resp=$((total_resp + n))
    errors=$((errors + e))
    ((n == OPS)) || log "client $c: $n replies for $OPS requests"
done

elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
((elapsed_ms > 0)) || elapsed_ms=1
rate=$(( total_resp * 1000 / elapsed_ms ))

printf 'clients=%d requests=%d replies=%d errors=%d nc_failures=%d elapsed=%d ms throughput=%d req/s\n' \
    "$CLIENTS" "$total_req" "$total_resp" "$errors" "$failed" "$elapsed_ms" "$rate"

((failed == 0))               || die "$failed client(s) failed"
((total_resp == total_req))   || die "missing replies: $total_resp of $total_req"
((errors == 0))               || die "$errors ERROR replies"
log "PASS"
