#!/usr/bin/env bash
# tools/decomp/decompile.sh OFFSET...: Ghidra's C for functions of the game image (image offsets), into
# out/decomp/c/<offset>.c (docs/DECOMPILATION.md). The first run imports out/eboot.elf into the Ghidra
# project out/decomp/ghidra. Needs Ghidra (brew install ghidra).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
export JAVA_HOME=${JAVA_HOME:-$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home}
headless=$(brew --prefix ghidra)/libexec/support/analyzeHeadless
project=out/decomp/ghidra
if [[ ! -d $project/bloodborne.rep ]]; then
    mkdir -p "$project"
    # Ghidra's ELF loader does not know the PS4 executable type (0xfe10): a copy marked ET_DYN.
    python3 -c "
import sys
d = bytearray(open('out/eboot.elf', 'rb').read())
d[16:18] = (3).to_bytes(2, 'little')
open('out/decomp/eboot_ghidra.elf', 'wb').write(d)"
    "$headless" "$project" bloodborne -import out/decomp/eboot_ghidra.elf -noanalysis \
        -processor x86:LE:64:default -cspec gcc > out/decomp/ghidra_import.log 2>&1
fi
"$headless" "$project" bloodborne -process eboot_ghidra.elf -noanalysis \
    -scriptPath tools/decomp/ghidra -postScript Decompile.java out/decomp/c "$@" 2>&1 |
    grep -E '^INFO  Decompile.java> ' | sed 's/^INFO  Decompile.java> //; s/ (GhidraScript) *$//'
