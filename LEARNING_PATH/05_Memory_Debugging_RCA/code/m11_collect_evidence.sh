#!/usr/bin/env bash
# m11_collect_evidence.sh - minute-6 evidence capture for a Linux server incident (module 11).
#
# Collects everything needed to analyse a crash or hang later, on another machine, into ONE
# tarball with a SHA-256 for chain of custody:
#   - hung process (--pid): two gcores 30 s apart (deadlock vs slow), per-thread CPU, limits,
#     maps, open fds, status, the exact binary, its debug file if found, every shared library
#   - crashed process (--core): the core, the binary, the libraries listed in the core
#   - both: last N lines of the log, host info, glibc version, core_pattern, ulimit
#
# Usage:
#   sudo ./m11_collect_evidence.sh --pid 4120 [--log /var/log/app/app.log] [--out DIR] [--gap 30]
#   sudo ./m11_collect_evidence.sh --core /var/crash/core.app.4120 --bin /opt/app/bin/app [--log ...]
#
# Try it on the course programs:
#   ./build/m10_deadlock_debug &  ./m11_collect_evidence.sh --pid $! --out /tmp/inc
#
# The output contains customer data in a real system: write it only to an approved, encrypted,
# access-controlled location, and delete it when the investigation closes.

set -u
PID="" CORE="" BIN="" LOG="" OUT="." GAP=30 LOGLINES=20000

while [ $# -gt 0 ]; do
  case "$1" in
    --pid)  PID=$2; shift 2 ;;
    --core) CORE=$2; shift 2 ;;
    --bin)  BIN=$2; shift 2 ;;
    --log)  LOG=$2; shift 2 ;;
    --out)  OUT=$2; shift 2 ;;
    --gap)  GAP=$2; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ -z "$PID" ] && [ -z "$CORE" ]; then
  echo "need --pid (hung/live) or --core (crashed)" >&2; exit 2
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME="inc-${STAMP}${PID:+-pid$PID}"
WORK=$(mktemp -d)/"$NAME"
mkdir -p "$WORK"/libs "$OUT"
say() { echo "[collect] $*"; echo "$(date -u +%FT%TZ) $*" >> "$WORK/collect.log"; }

# ---- host facts: the first questions of every RCA ----------------------------------------
say "host info"
{
  echo "collected_utc: $STAMP"
  uname -a
  cat /etc/os-release 2>/dev/null | head -4
  ldd --version 2>&1 | head -1
  echo "core_pattern: $(cat /proc/sys/kernel/core_pattern)"
  echo "ulimit -c (this shell): $(ulimit -c)"
  uptime
  free -m
  df -h /var /tmp 2>/dev/null
} > "$WORK/host.txt" 2>&1

copy_libs_from_maps() {  # $1 = maps file; copy every mapped .so so gdb can use set sysroot
  awk '$6 ~ /\.so/ {print $6}' "$1" | sort -u | while read -r lib; do
    [ -f "$lib" ] && mkdir -p "$WORK/libs$(dirname "$lib")" && cp -L "$lib" "$WORK/libs$lib"
  done
}

copy_binary_and_debug() {  # $1 = binary path
  local b=$1
  [ -f "$b" ] || { say "binary $b not found"; return; }
  cp -L "$b" "$WORK/"
  readelf -n "$b" 2>/dev/null | grep -i 'build id' > "$WORK/build_id.txt"
  # the debug file via build-id, the standard location
  local id; id=$(awk '{print $3}' "$WORK/build_id.txt" | head -1)
  if [ -n "$id" ] && [ -f "/usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug" ]; then
    cp "/usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug" "$WORK/"
    say "debug file found for build id $id"
  else
    say "no debug file found locally for build id ${id:-unknown} - fetch it from the symbol store"
  fi
}

# ---- live process (hang, spin, growth) --------------------------------------------------
if [ -n "$PID" ]; then
  [ -d "/proc/$PID" ] || { echo "no such process $PID" >&2; exit 1; }
  BIN=$(readlink -f "/proc/$PID/exe")
  say "process $PID binary $BIN"

  # cheap, non-intrusive data first, while the state is untouched
  cp "/proc/$PID/maps"    "$WORK/maps.txt"
  cp "/proc/$PID/status"  "$WORK/status.txt"
  cp "/proc/$PID/limits"  "$WORK/limits.txt"
  cat "/proc/$PID/smaps_rollup" > "$WORK/smaps_rollup.txt" 2>/dev/null
  ls -l "/proc/$PID/fd" > "$WORK/fds.txt" 2>&1
  tr '\0' ' ' < "/proc/$PID/cmdline" > "$WORK/cmdline.txt"
  top -H -b -n 1 -p "$PID" > "$WORK/top_threads.txt" 2>&1
  for t in /proc/"$PID"/task/*; do   # where each thread is blocked in the kernel
    echo "$(basename "$t") $(cat "$t/comm") $(cat "$t/wchan" 2>/dev/null)"
  done > "$WORK/thread_wchan.txt"

  # two gcores: identical stacks = deadlock, different = slow or livelock
  say "gcore 1 of 2 (the process is paused while this runs)"
  gcore -o "$WORK/core1" "$PID" > "$WORK/gcore1.log" 2>&1 || say "gcore 1 failed - see gcore1.log"
  say "waiting $GAP s for second gcore (hang analysis needs two)"
  sleep "$GAP"
  say "gcore 2 of 2"
  gcore -o "$WORK/core2" "$PID" > "$WORK/gcore2.log" 2>&1 || say "gcore 2 failed - see gcore2.log"

  copy_binary_and_debug "$BIN"
  copy_libs_from_maps "$WORK/maps.txt"
fi

# ---- crashed process ------------------------------------------------------------------
if [ -n "$CORE" ]; then
  [ -f "$CORE" ] || { echo "core $CORE not found" >&2; exit 1; }
  cp "$CORE" "$WORK/"
  say "core $CORE copied"
  if [ -z "$BIN" ]; then  # the core's NT_PRPSINFO names the executable
    BIN=$(file "$CORE" | sed -n "s/.*execfn: '\([^']*\)'.*/\1/p")
  fi
  copy_binary_and_debug "$BIN"
  # NT_FILE note lists every mapped file at crash time
  eu-unstrip -n --core="$CORE" 2>/dev/null > "$WORK/core_modules.txt"
  awk '{print $NF}' "$WORK/core_modules.txt" | grep '\.so' | sort -u | while read -r lib; do
    [ -f "$lib" ] && mkdir -p "$WORK/libs$(dirname "$lib")" && cp -L "$lib" "$WORK/libs$lib"
  done
fi

# ---- logs ----------------------------------------------------------------------------
if [ -n "$LOG" ] && [ -f "$LOG" ]; then
  tail -n "$LOGLINES" "$LOG" > "$WORK/app_log_tail.txt"
  say "last $LOGLINES log lines copied"
fi
journalctl -n 500 --no-pager > "$WORK/journal_tail.txt" 2>/dev/null

# ---- package, hash ---------------------------------------------------------------------
TAR="$OUT/$NAME.tar.gz"
tar -C "$(dirname "$WORK")" -czf "$TAR" "$NAME"
SUM=$(sha256sum "$TAR" | awk '{print $1}')
echo "$SUM  $(basename "$TAR")" > "$TAR.sha256"
say "wrote $TAR (sha256 $SUM)"
rm -rf "$(dirname "$WORK")"
echo
echo "Analyse elsewhere with:"
echo "  tar xzf $(basename "$TAR") && cd $NAME"
echo "  gdb -q ./$(basename "${BIN:-app}") core1.<pid>   -ex 'set sysroot ./libs' -ex 'thread apply all bt'"
