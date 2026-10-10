#!/usr/bin/env bash
# tools/decomp/replay.sh RECORDS [options]: builds tools/decomp/replay.c with the CPU core and replays
# recorded calls (docs/DECOMPILATION.md).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
D=deps/macos-arm64
mkdir -p out/decomp
clang -arch arm64 -O1 -g -std=gnu11 -Wall -I$D/include tools/decomp/replay.c src/cpu/cpu.c \
    src/cpu/decode.c src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/jit_arm64.c \
    src/cpu/record.c src/cpu/native.c src/cpu/hostcall.S $D/lib/libZydis.a $D/lib/libZycore.a -o out/decomp/replay
BB_JIT=0 exec out/decomp/replay "$@"
