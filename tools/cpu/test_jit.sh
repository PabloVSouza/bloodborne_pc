#!/usr/bin/env bash
# tools/cpu/test_jit.sh: bbcpu on arm64 (interpreter and JIT) against native x86-64 results.
# Builds tests/jit_guest.c as an x86-64 executable linked at 3 TiB (free in an arm64 process), runs
# it under Rosetta for the expected results, then tests/test_jit.c with BB_JIT=0 and BB_JIT=1.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
D=deps/macos-arm64
clang -arch x86_64 -O2 -mavx -msse4.1 -mpopcnt -fno-builtin -fno-stack-protector -Wl,-no_pie -Wl,-image_base,0x30000000000 \
    tests/jit_guest.c -o out/jit_guest
ROSETTA_ADVERTISE_AVX=1 out/jit_guest > out/jit_guest.expected
clang -arch arm64 -O2 -std=gnu11 -I$D/include tests/test_jit.c src/cpu/cpu.c src/cpu/decode.c \
    src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/record.c src/cpu/native.c src/cpu/recomp.c src/cpu/jit_arm64.c \
    src/cpu/hostcall.S $D/lib/libZydis.a $D/lib/libZycore.a -o out/test_jit
echo "== interpreter"; BB_JIT=0 out/test_jit out/jit_guest out/jit_guest.expected
echo "== JIT"; out/test_jit out/jit_guest out/jit_guest.expected
