#!/usr/bin/env bash
# scripts/build.sh - configure and build tcpkv with a CMake preset (module 10).
#
#   scripts/build.sh                  debug preset
#   scripts/build.sh release          any preset from CMakePresets.json: debug release asan tsan clang ...
#   scripts/build.sh --clean asan     delete build/asan first (fresh configure)
#   scripts/build.sh --list           list configure presets
#
# CI and developers run the same command, so a red pipeline reproduces with one line locally.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"     # repository root, wherever we are called from
cd "$ROOT"

log() { printf '[build] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

CLEAN=0
PRESET=debug
while (($#)); do
    case "$1" in
        --clean) CLEAN=1 ;;
        --list)  exec cmake --list-presets=configure ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        -*) die "unknown option $1" ;;
        *)  PRESET="$1" ;;
    esac
    shift
done

command -v cmake >/dev/null || die "cmake not found (sudo apt install cmake)"
command -v ninja >/dev/null || die "ninja not found (sudo apt install ninja-build)"

BUILD_DIR="build/$PRESET"                     # matches binaryDir in CMakePresets.json
if ((CLEAN)) && [[ -d "$BUILD_DIR" ]]; then
    log "removing $BUILD_DIR"
    rm -r -- "$BUILD_DIR"
fi

# Use every core, but leave the machine usable. nproc respects cgroup CPU limits in containers.
JOBS="${JOBS:-$(nproc)}"

start=$(date +%s)
log "configure: cmake --preset $PRESET"
cmake --preset "$PRESET"
log "build: cmake --build --preset $PRESET -j $JOBS"
cmake --build --preset "$PRESET" -j "$JOBS"

# Keep compile_commands.json at the root for clangd and clang-tidy (gitignored).
if [[ -f "$BUILD_DIR/compile_commands.json" ]]; then
    ln -sf "$BUILD_DIR/compile_commands.json" compile_commands.json
fi

log "done in $(( $(date +%s) - start )) s -> $BUILD_DIR/tcpkv"
