#!/usr/bin/env bash
# tools/cpu/test.sh: builds and runs the bbcpu differential test (tests/test_bbcpu.c) on x86-64.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
D=deps/macos-x86_64
clang -arch x86_64 -O2 -std=gnu11 -mavx -msse4.1 -mpopcnt -mbmi -mlzcnt -I$D/include \
    tests/test_bbcpu.c src/cpu/cpu.c src/cpu/decode.c src/cpu/interp.c src/cpu/interp_vec.c \
    src/cpu/interp_x87.c src/cpu/record.c src/cpu/hostcall.S $D/lib/libZydis.a -o out/test_bbcpu
ROSETTA_ADVERTISE_AVX=1 out/test_bbcpu
