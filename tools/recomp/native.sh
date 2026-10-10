#!/usr/bin/env bash
# tools/recomp/native.sh [SOURCE_DIR]: builds the native versions of game functions (default
# private/native: decompiled code, never in this repository) into out/recomp/libbbnative.dylib
# (docs/RECOMPILATION.md). The game loads it with BB_NATIVE_LIB; tools/recomp/replay.sh --native checks it.
# Floating point as on x86: no fused multiply-add (-ffp-contract=off).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source_dir=${1:-private/native}
mkdir -p out/recomp
clang -arch arm64 -O2 -g -std=gnu11 -Wall -Wextra -ffp-contract=off -fno-strict-aliasing -dynamiclib \
    -Isrc "$source_dir"/*.c -o out/recomp/libbbnative.dylib
echo "out/recomp/libbbnative.dylib"
