#!/usr/bin/env bash
# m10_bash_patterns.sh - the bash patterns every build, test and deploy script needs (module 10).
#
#   ./m10_bash_patterns.sh                 run every demo
#   ./m10_bash_patterns.sh -v              same, with a trace of each command (set -x)
#   bash -n m10_bash_patterns.sh           syntax check only
#   shellcheck m10_bash_patterns.sh        lint (sudo apt install shellcheck)
#
# Read it top to bottom: each section is one pattern, with the reason in the comment.

# -e: exit when a command fails. -u: using an unset variable is an error, not "".
# -o pipefail: a pipeline fails if ANY stage fails, not just the last one.
set -euo pipefail

# Word splitting only on newline and tab, never on spaces inside file names.
IFS=$'\n\t'

# Constants in UPPER case, readonly so nothing reassigns them by accident.
# Assign first, then mark readonly: 'readonly X=$(cmd)' would hide a failure of cmd.
SCRIPT_NAME="$(basename "$0")"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly SCRIPT_NAME SCRIPT_DIR

# ------------------------------------------------------------------ logging
# Logs go to stderr so stdout stays clean for data that callers pipe somewhere.
# "$*" joins arguments with the first character of IFS; set IFS locally so that is a space.
log()  { local IFS=' '; printf '%s [%s] %s\n' "$(date +%H:%M:%S)" "$SCRIPT_NAME" "$*" >&2; }
die()  { log "ERROR: $*"; exit 1; }

# ------------------------------------------------------------------ argument parsing
VERBOSE=0
while getopts ':vh' opt; do
    case "$opt" in
        v) VERBOSE=1 ;;
        h) echo "usage: $SCRIPT_NAME [-v]"; exit 0 ;;
        \?) die "unknown option -$OPTARG" ;;
    esac
done
shift $((OPTIND - 1))
if (( VERBOSE )); then
    # PS4 is the prefix printed by set -x: file, line and function make traces readable.
    export PS4='+ ${BASH_SOURCE##*/}:${LINENO}:${FUNCNAME[0]:-main}: '
    set -x
fi

# ------------------------------------------------------------------ temp dir + trap cleanup
# mktemp gives a unique, private directory. The trap removes it however the script ends:
# normal exit, failure under set -e, or Ctrl-C.
WORKDIR="$(mktemp -d)"
cleanup() {
    local rc=$?                  # exit status of whatever ended the script; must be read first
    rm -r -- "$WORKDIR"
    # Kill any background jobs we started so nothing outlives the script.
    local pids
    pids="$(jobs -p)"
    if [[ -n "$pids" ]]; then kill $pids 2>/dev/null || true; fi
    log "cleanup done, exit $rc"
    exit "$rc"
}
trap cleanup EXIT
# Convert signals into a normal exit so the EXIT trap runs once with a meaningful status.
trap 'exit 130' INT
trap 'exit 143' TERM

# ERR trap: say where it failed. set -E makes functions and subshells inherit it.
set -E
trap 'log "failed at line $LINENO: $BASH_COMMAND"' ERR

# ------------------------------------------------------------------ functions, locals, return values
# Functions return a status (0 = success). They "return" data by printing it.
# 'local' keeps variables from leaking into the caller.
count_lines() {
    local file="$1"
    [[ -r "$file" ]] || return 1
    wc -l < "$file"
}

demo_functions() {
    log "== functions"
    printf 'a\nb\nc\n' > "$WORKDIR/abc.txt"
    local n
    n="$(count_lines "$WORKDIR/abc.txt")"      # command substitution captures stdout
    echo "abc.txt has $n lines"
    if ! count_lines "$WORKDIR/missing.txt" >/dev/null; then
        echo "missing.txt: count_lines returned non-zero, handled with if !"
    fi
}

# ------------------------------------------------------------------ quoting
demo_quoting() {
    log "== quoting"
    local name="file with spaces.txt"
    touch "$WORKDIR/$name"                     # quoted: one argument
    ls "$WORKDIR" | grep -c 'spaces'           # prints 1
    # Unquoted $name would be three arguments: "file" "with" "spaces.txt". Always quote.
    local -a args=(--preset debug "--target=tcpkv tests")
    printf '[%s] ' "${args[@]}"; echo          # arrays keep elements intact: 3 items
}

# ------------------------------------------------------------------ loops
demo_loops() {
    log "== loops"
    local i
    for i in 1 2 3; do printf 'item %s ' "$i"; done; echo
    for ((i = 0; i < 3; i++)); do printf 'c-style %d ' "$i"; done; echo

    # Read a file line by line. IFS= keeps leading spaces; -r keeps backslashes literal.
    printf 'GET a\nSET b 2\n  PING\n' > "$WORKDIR/cmds.txt"
    local line
    while IFS= read -r line; do
        printf 'cmd=[%s]\n' "$line"
    done < "$WORKDIR/cmds.txt"

    # Glob loop: nullglob makes an empty match expand to nothing instead of the literal '*.log'.
    shopt -s nullglob
    local f
    for f in "$WORKDIR"/*.log; do echo "never printed: $f"; done
    shopt -u nullglob
}

# ------------------------------------------------------------------ set -e traps for the unwary
demo_set_e_gotchas() {
    log "== set -e gotchas"
    # 1. A failing command in an 'if', '||', '&&' or '!' does NOT trigger set -e. Intended.
    if grep -q 'nomatch' "$WORKDIR/cmds.txt"; then echo "found"; else echo "grep failed, script continues"; fi
    # 2. 'local x=$(cmd)' hides cmd's failure: 'local' itself succeeds. Declare, then assign.
    local out
    out="$(echo ok)"
    echo "out=$out"
    # 3. Arithmetic ((x++)) returns 1 when the result is 0 and would kill the script. Use x=$((x+1)).
    local x=0
    x=$((x + 1))
    echo "x=$x"
    # 4. Commands that may legitimately fail: be explicit.
    rm -- "$WORKDIR/not-there" 2>/dev/null || true
    # 5. Default values with -u: ${VAR:-default} is safe when VAR is unset.
    echo "BUILD_TYPE=${BUILD_TYPE:-Debug}"
}

# ------------------------------------------------------------------ pipefail in action
demo_pipefail() {
    log "== pipefail"
    # Without pipefail the status is sort's (0) and the failure of 'false' is lost.
    if false | sort; then echo "pipeline ok"; else echo "pipeline failed (pipefail saw 'false')"; fi
    # PIPESTATUS holds every stage's status. Copy it immediately: the next command overwrites it.
    local -a status=()
    false | true | true || status=("${PIPESTATUS[@]}")
    printf 'PIPESTATUS: '; printf '%s ' "${status[@]}"; echo   # [*] would join with IFS's first char
}

# ------------------------------------------------------------------ background jobs, wait, timeouts
demo_jobs() {
    log "== background jobs"
    local -a pids=()
    local i
    for i in 1 2 3; do
        ( sleep 0.$i; echo "worker $i done" ) &
        pids+=("$!")                           # $! = PID of the last background job
    done
    local failed=0 p
    for p in "${pids[@]}"; do
        wait "$p" || failed=$((failed + 1))    # wait PID returns that job's exit status
    done
    echo "workers failed: $failed"

    # timeout(1): kill a command that hangs. Exit status 124 means "timed out".
    local rc=0
    timeout 1 sleep 5 || rc=$?
    echo "timeout exit status: $rc"
}

# ------------------------------------------------------------------ retry with backoff
# Retry DOWNLOADS and connections, never tests (a flaky test is a bug to fix).
retry() {
    local attempts="$1"; shift
    local delay=1 n
    local IFS=' '                              # so "$*" in the message joins with spaces
    for ((n = 1; n <= attempts; n++)); do
        if "$@"; then return 0; fi
        log "attempt $n/$attempts failed: $*; sleeping ${delay}s"
        sleep "$delay"
        delay=$((delay * 2))
    done
    return 1
}

demo_retry() {
    log "== retry"
    local marker="$WORKDIR/ready"
    ( sleep 1.5; touch "$marker" ) &           # something becomes ready after a while
    retry 4 test -e "$marker" && echo "ready after retries"
}

# ------------------------------------------------------------------ here-docs
demo_heredoc() {
    log "== here-doc"
    local port=7000
    # Unquoted delimiter: variables expand. Quoted 'EOF': taken literally.
    cat > "$WORKDIR/tcpkv.env" <<EOF
TCPKV_PORT=$port
TCPKV_LOG_LEVEL=info
EOF
    cat "$WORKDIR/tcpkv.env"
    cat <<'EOF'
literal: $HOME is not expanded here
EOF
}

main() {
    log "running in $SCRIPT_DIR, scratch dir $WORKDIR"
    demo_functions
    demo_quoting
    demo_loops
    demo_set_e_gotchas
    demo_pipefail
    demo_jobs
    demo_retry
    demo_heredoc
    log "all demos finished"
}

main "$@"
