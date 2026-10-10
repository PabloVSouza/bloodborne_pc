#!/usr/bin/env python3
"""tools/recomp/imports.py EBOOT_BIN OUTDIR: the game's import stubs by name, OUTDIR/imports.tsv
(stub offset, import name): a stub is a function of tools/recomp/scan.c that is one jump through a
pointer slot (OUTDIR/slots.tsv) the loader fills with an import (the jump-slot relocations of
EBOOT_BIN; names from src/import_names.inc, else the NID). bbrecomp leaves the callers of
setjmp-like imports to the translator; the names also label the generated code."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import link_modules  # noqa: E402

R_X86_64_JUMP_SLOT = 7
R_X86_64_GLOB_DAT = 6


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[2])
    known = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (ROOT / 'src/import_names.inc').read_text()))
    m = link_modules.module(pathlib.Path(sys.argv[1]))
    slots = {}
    for target, kind, symid, _ in m['relocs']:
        if kind in (R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT):
            name = m['symbols'][symid]['name']
            slots[target] = known.get(name, name)
    sizes = {}
    for line in (out / 'functions.tsv').read_text().splitlines()[1:]:
        address, size, instructions, _ = line.split('\t')
        sizes[int(address, 16)] = (int(size), int(instructions))
    stubs = {}
    for line in (out / 'slots.tsv').read_text().splitlines()[1:]:
        caller, slot = (int(x, 16) for x in line.split('\t'))
        if slot in slots and sizes.get(caller, (0, 0))[1] <= 2:
            stubs[caller] = slots[slot]
    (out / 'imports.tsv').write_text('stub\tname\n' + ''.join(f'{a:#x}\t{n}\n' for a, n in sorted(stubs.items())))
    print(f'{len(stubs)} import stubs named ({sum(not n.endswith("#q#q") for n in stubs.values())} by name)')


if __name__ == '__main__':
    main()
