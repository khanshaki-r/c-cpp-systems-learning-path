#!/usr/bin/env bash
# m13_redis_cli_demo.sh : Redis primitives from the shell (module 13).
#
# Needs Redis on localhost:6379 (cd code && docker compose up -d redis) and redis-cli:
#     sudo apt install -y redis-tools
# or run every command inside the container instead: REDIS_CLI="docker exec -i redis redis-cli" bash m13_redis_cli_demo.sh
#
# Each step prints what it does; read the output next to the comments.

set -euo pipefail
read -r -a CLI <<< "${REDIS_CLI:-redis-cli -p 6379}"
rc() { "${CLI[@]}" "$@"; }
step() { echo; echo "=== $*"; }

step "0. Connectivity and the server's role"
rc PING
rc INFO server | grep -E "redis_version|process_id|tcp_port" || true

step "1. A lock with SET NX PX: the second SET NX fails while the first holder's key exists"
rc DEL lock:branch:BR0001 >/dev/null
rc SET lock:branch:BR0001 token123 NX PX 5000      # OK: acquired, expires in 5 s
rc SET lock:branch:BR0001 token456 NX PX 5000      # (nil): someone holds it
rc PTTL lock:branch:BR0001                          # remaining milliseconds

step "2. Owner-checked release with a Lua script (atomic compare-and-delete)"
RELEASE_LUA='if redis.call("GET", KEYS[1]) == ARGV[1] then return redis.call("DEL", KEYS[1]) else return 0 end'
rc EVAL "$RELEASE_LUA" 1 lock:branch:BR0001 token456   # 0: not the owner, nothing deleted
rc EVAL "$RELEASE_LUA" 1 lock:branch:BR0001 token123   # 1: the owner released it
# Why not GET then DEL from the client: between the two calls the key can expire and be taken
# by another client, and your DEL would delete THEIR lock. The script runs atomically.

step "3. Cache entry with TTL, and the negative-cache sentinel"
rc SET branch:v1:BR0007 '{"code":"BR0007","name":"Branch 7"}' EX 300
rc TTL branch:v1:BR0007
rc SET branch:v1:BR9999 __NONE__ EX 30
rc MGET branch:v1:BR0007 branch:v1:BR9999 branch:v1:BR0008     # last one: (nil) = a real miss

step "4. Pipelining: 100k SETs in one stream with --pipe (no per-command round trip)"
if [[ -z "${REDIS_CLI:-}" ]]; then
    time (seq 1 100000 | awk '{print "SET k:"$1" v"$1}' | rc --pipe)
else
    time (seq 1 100000 | awk '{print "SET k:"$1" v"$1}' | docker exec -i redis redis-cli --pipe)
fi
rc DBSIZE

step "5. SCAN versus KEYS: iterate without blocking the single-threaded server"
# KEYS k:* walks the whole keyspace in ONE command: every other client waits meanwhile.
# SCAN returns a cursor and a small batch per call; repeat until the cursor is 0.
cursor=0; total=0; calls=0
while :; do
    mapfile -t out < <(rc SCAN "$cursor" MATCH 'k:*' COUNT 1000)
    cursor="${out[0]}"
    total=$(( total + ${#out[@]} - 1 ))
    calls=$(( calls + 1 ))
    [[ "$cursor" == "0" ]] && break
done
echo "SCAN found $total keys in $calls calls"
# Delete them the same way in production (UNLINK frees memory in a background thread):
rc EVAL "for _, k in ipairs(redis.call('KEYS', ARGV[1])) do redis.call('UNLINK', k) end return 1" 0 'k:*' >/dev/null
# (KEYS inside a script is acceptable on a dev box with 100k keys; on production use SCAN + UNLINK.)
rc DBSIZE

step "6. Optimistic transaction: WATCH / MULTI / EXEC on a counter"
rc SET counter:logins 10 >/dev/null
# In ONE connection: WATCH the key, read it, queue the write, EXEC. If any other client changes the
# key between WATCH and EXEC, EXEC returns (nil) and the client retries.
printf 'WATCH counter:logins\nGET counter:logins\nMULTI\nSET counter:logins 11\nEXEC\n' | rc
# Now a conflicting writer between WATCH and EXEC. redis-cli may read all of stdin before
# sending, so we hold one raw TCP connection open with bash's /dev/tcp and speak Redis's inline
# protocol on it, while a second connection (redis-cli) writes the key in the middle.
if [[ -z "${REDIS_CLI:-}" ]]; then
    exec 3<>/dev/tcp/127.0.0.1/6379
    printf 'WATCH counter:logins\r\nMULTI\r\nINCR counter:logins\r\n' >&3
    sleep 0.5
    rc SET counter:logins 100 >/dev/null       # the other client
    printf 'EXEC\r\nQUIT\r\n' >&3
    cat <&3                                    # +OK +OK +QUEUED *-1 +OK : "*-1" is the aborted EXEC
    exec 3>&-
    echo "-> *-1 means EXEC returned nil: the queued INCR was discarded because the WATCHed key changed"
    rc GET counter:logins                      # 100: the other client's write stands, no lost update
fi
# For a plain counter you do not need any of this: INCR is atomic on its own.
rc INCR counter:logins

step "7. Persistence: what survives a restart?"
rc INFO persistence | grep -E "aof_enabled|aof_last_write_status|rdb_last_save_time|rdb_changes_since_last_save" || true
rc CONFIG GET appendfsync
# appendfsync everysec: up to ~1 s of acknowledged writes can be lost on a crash. That is fine
# for a cache, and the reason Redis is not the system of record for balances.

step "8. Memory policy: what happens when the cache is full?"
rc CONFIG GET maxmemory
rc CONFIG GET maxmemory-policy
rc INFO memory | grep -E "used_memory_human|maxmemory_human|mem_fragmentation_ratio" || true

step "9. Keyspace stats: the hit ratio your dashboards should show"
rc INFO stats | grep -E "keyspace_hits|keyspace_misses|evicted_keys|expired_keys" || true

rc DEL branch:v1:BR0007 branch:v1:BR9999 counter:logins >/dev/null
echo; echo "done"
