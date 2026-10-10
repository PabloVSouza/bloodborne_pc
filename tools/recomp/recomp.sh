#!/usr/bin/env bash
# tools/recomp/recomp.sh OFFSET... | --file LIST: recompiles game functions (image offsets) from out/eboot.elf
# into out/recomp/librecomp.dylib (docs/RECOMPILATION.md): tools/recomp/bbrecomp.c writes the C
# (out/recomp/gen/recomp*.c), clang compiles the files in parallel. The game loads it with
# BB_RECOMP_LIB; replay.sh --recomp checks it. Needs out/recomp/functions.tsv (tools/recomp/scan.c).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
D=deps/macos-arm64
G=out/recomp/gen
mkdir -p "$G"
offsets=("$@")
if [[ ${1:-} == --file ]]; then mapfile -t offsets < "$2"; fi
clang -arch arm64 -O1 -g -std=gnu11 -Wall -I$D/include tools/recomp/bbrecomp.c src/cpu/cpu.c \
    src/cpu/decode.c src/cpu/interp.c src/cpu/interp_vec.c src/cpu/interp_x87.c src/cpu/jit_arm64.c \
    src/cpu/record.c src/cpu/native.c src/cpu/recomp.c src/cpu/hostcall.S $D/lib/libZydis.a \
    $D/lib/libZycore.a -o out/recomp/bbrecomp
rm -f "$G"/recomp*.c "$G"/recomp*.o
BB_JIT=0 out/recomp/bbrecomp out/eboot.elf out/recomp/functions.tsv "$G/recomp.c" "${offsets[@]}"
# Floating point as on x86: no fused multiply-add.
ls "$G"/recomp*.c | xargs -P "$(sysctl -n hw.ncpu)" -I{} clang -arch arm64 -O2 -g -std=gnu11 -Wall \
    -Wno-unused-label -Wno-unused-variable -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden \
    -I$D/include -Isrc -c {} -o {}.o
clang -arch arm64 -dynamiclib "$G"/recomp*.o -o out/recomp/librecomp.dylib
echo "out/recomp/librecomp.dylib"
