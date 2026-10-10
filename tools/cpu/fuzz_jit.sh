#!/usr/bin/env bash
# tools/cpu/fuzz_jit.sh FILE [trials] [max reports]: builds tests/fuzz_jit.c (arm64) and fuzzes the
# JIT against the interpreter with the instructions in FILE (a game run with BB_JIT_DUMP=FILE).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
D=deps/macos-arm64
clang -arch arm64 -O2 -std=gnu11 -I$D/include tests/fuzz_jit.c src/cpu/cpu.c src/cpu/decode.c \
    src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/record.c src/cpu/jit_arm64.c \
    src/cpu/hostcall.S $D/lib/libZydis.a $D/lib/libZycore.a -o out/fuzz_jit
out/fuzz_jit "$@"
