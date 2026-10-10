# Recompilation

> [!NOTE]
> This work happens on the [`recomp`](https://github.com/PabloVSouza/bloodborne_mac/tree/recomp)
> branch: the tools named below are there, not in `main` yet. Progress is tracked on the wiki:
> [Recompilation status](https://github.com/PabloVSouza/bloodborne_mac/wiki/Recompilation-status).

Goal: the game's own code running as native arm64 code, compiled ahead of time instead of
translated while the game runs, and in the end the engine's graphics drawing with Metal directly
instead of building PS4 command buffers for shadPS4 and MoltenVK to translate.

The way there is a **static recompiler**: a tool that runs on the player's Mac, reads *their own*
`eboot.bin` and writes the game's functions out as C, which clang compiles into a native library
the game loads. This project publishes the recompiler, never the game's code, as *Unleashed
Recompiled* and N64Recomp do.

## What is published

| Published (this repository) | Never published |
|---|---|
| The recompiler, its runtime, the translator | The game's code |
| Record and replay, profiling and analysis tools | |
| Annotations: function names, signatures, struct layouts | |

The recompiled code is produced on the player's machine from their copy, and stays there.

## Principles

1. **Only FromSoftware's code first.** Havok, FMOD, Scaleform, YEBIS, Lua and zlib are licensed
   libraries; they can be recompiled like the rest later, the game's and engine's code comes first.
2. **Always playable.** Recompiled functions replace the translated ones one by one; anything not
   recompiled runs in the translator, which is also the fallback inside a generated function.
3. **The game's own data layouts.** Guest memory is host memory and x86-64 and arm64 share the
   LP64 layout rules: generated code works on the game's live objects.
4. **Checked by behaviour.** Calls recorded during play are replayed against the generated code;
   results, memory writes and calls must match exactly.
5. **Behind switches.** Generated functions can be turned off one by one, so a wrong one is found
   by bisection.

## Milestones

| | Goal | Status |
|---|---|---|
| R0 | Groundwork: function table, library labels, CPU profile, record and replay, native functions loaded into the game and called from translated code | Done |
| R1 | Recompiler prototype: generated C for the recorded functions, checked by replay | Done |
| R2 | Coverage: every function generated; the game runs recompiled, the translator as fallback | In progress |
| R3 | Speed: integer, flags and vector instructions in C; memory ordering relaxed where safe | |
| R4 | Annotations: names, signatures and structs make the generated C readable | |
| R5 | The engine's graphics on Metal, at the libGnm boundary | |

## Progress

*2026-10-10.* The game's executable has 241,821 functions, about 11.3 million instructions.

| | Progress | |
|---|---|---|
| Functions recompiled, running in the game | `████░░░░░░░░░░░░░░░░` 20.3% | 49,071 of 241,821 |
| Instructions recompiled | `█████░░░░░░░░░░░░░░░` 27.2% | 3,063,245 of 11,268,132 |
| Instructions in plain C (of those recompiled) | `████████████████████` 99.8% | 3,057,104 of 3,063,245 |
| Functions checked call by call (recordings) | `░░░░░░░░░░░░░░░░░░░░` 0.4% | 933 of 241,821 |
| Functions labelled by library | `█████████░░░░░░░░░░░` 44.1% | 106,659 of 241,821 |

The recompiled functions are all those a 3-minute route through the Hunter's Dream runs; the rest
are generated the same way once a route reaches them, or all at once. Every recompiled function
runs in the game; the call-by-call checks cover the busiest, and the fuzzing covers every
instruction form the generator handles.

## The recompiler

`tools/recomp/bbrecomp.c` reads `eboot.bin`, takes functions from the function table
(`tools/recomp/scan.c`), and writes one C function per game function against `src/recomp/rc.h`;
`tools/recomp/recomp.sh` compiles them into a library, one file per 128 KiB of the image, compiling
again only the files whose C changed:

- The code is decoded by following its control flow from the entry. Jumps become `goto`s (labels
  only where something jumps); returns become C returns. Calls of other recompiled functions are
  direct C calls; other calls go through the runtime.
- Guest registers live in a local array; the flags are computed lazily (clang drops what nothing
  reads). Vector registers stay in the CPU state, so vector instructions need no spill.
- Memory accesses keep x86's ordering between threads (acquire/release), as the translator's do,
  the stack's too: the game's jobs write into their creator's stack.
- In C: moves, integer arithmetic and logic (with carry), shifts, multiplies, bit tests,
  conditional sets and moves, locked operations (compare-and-exchange loops), and the common
  SSE/AVX moves, arithmetic, dot products, blends, inserts and extracts, conversions, bitwise
  operations, compares, shifts and shuffles, each as the interpreter does it. Anything else runs in the interpreter, one instruction at a
  time, so the output is complete from the start.
- An indirect jump to an unknown place goes back to the translator; a jump out of the function is
  a tail call.
- Left to the translator: the import stubs (one jump each to the PS4 libraries) and the functions
  that call `setjmp` (its return point must be guest code that can be resumed).
- A call the runtime runs in the translator gets a return address of its own; when a `longjmp` or
  an exception leaves several of them at once, the runtime goes back to the right one on the host
  as well.

The game loads the library (`BB_RECOMP_LIB`) after its patches: a function whose code changed since
it was generated (a game patch, a hook: `bbcpu_recomp_hash`) stays with the translator.
`BB_RECOMP_OFF=OFFSET,...` (or `@FILE`) switches chosen ones off, `BB_RECOMP_ONLY` keeps only the
listed ones (bisecting), `BB_RECOMP_THREADS` / `BB_RECOMP_NOT_THREADS`
limit them to some threads (by name), `BB_RECOMP_NODIRECT=1` sends every call through the
translator. Translated code calls recompiled functions directly.

Two checks: the **recordings** (whole functions, real calls) and **per-instruction fuzzing**
(`tools/recomp/fuzz.sh`: every distinct instruction encoding in the game, at most 30 per mnemonic,
run from random states through the interpreter and through its generated C).

R1 results (2026-10-10): the 24 recorded functions (2,508 instructions, 99% in C) match every
recorded call exactly as the originals do; in the game they run at the translator's speed (61.4 FPS
against 61.9, up to 1.7 million calls a second). Fuzzing: 3,443 instruction forms, 200 trials each,
no differences (a planted mistake in one flag fails 40).

R2 so far (2026-10-10): the 933 busiest functions (222,285 instructions, 99.4% in C) run in the
game on every thread at the translator's speed (62.1 FPS against 61.9). Fuzzing: 4,892 instruction
forms, 200 trials each, no differences. Recordings, 40 calls per function: 911 of 933 functions
match every call; the other 22 also fail when the original runs (they read the time stamp counter
or memory other threads change). What the checks found along the way:

- The interpreter's `lock cmpxchg` reported the value of a separate load after a failed exchange:
  when another thread had changed the value back, a failed exchange looked like a successful one,
  and two threads took the same lock. In the game this was a heap panic during save load
  (`DLRegularHeap.cpp(710)`); recompiled code, faster on some threads, made it frequent.
- The recorder missed `vmaskmovdqu`'s write (its address is implicit, in rdi).
- Speed: a shared counter of recompiled calls (every thread adding to it) and the runtime between
  recompiled functions cost 11 FPS; the instructions left to the interpreter, 7 more.

Coverage (2026-10-10): the unwind tables list 162,959 functions; leaf functions often have none.
The scanner now also follows direct calls, jumps, code addresses taken with `lea` and the code
pointers relocations put into data (vtables): 241,821 functions, and every block the game ran on a
3-minute route through the Hunter's Dream (walking, fighting, menus) is inside one. All 49,095
functions that route ran (3.06 million instructions, 99.8% in C) build in 3.5 minutes into a 98 MB
library; 49,071 of them run recompiled in the game (the rest changed by its patches) at the
translator's speed, 59.1 FPS, about 12 million recompiled calls a second. On the way:

- Recompiled import stubs put the runtime's return address on the stack, which `setjmp` saved; a
  later `longjmp` (the game's PNG reader) came back into a frame that was gone. The stubs and the
  40 functions that call `setjmp` stay with the translator.
- Every call the runtime ran in the translator returned to the same address: after a `longjmp` the
  innermost one stopped where an outer one should have.

## Tools

All write to `out/recomp/`.

| Tool | What it does |
|---|---|
| `tools/recomp/scan.c` | Every function: the unwind tables (exact start and size), then what calls, jumps and code pointers reach (decoded by following control flow); `functions.tsv`, `calls.tsv`, `slots.tsv`, `strings.tsv` |
| `tools/recomp/pointers.py` | The code addresses relocations put into data (`pointers.txt`), for `scan.c` |
| `tools/recomp/imports.py` | The import stubs by name (`imports.tsv`) |
| `tools/recomp/label.py` | The library of each function (`modules.tsv`), from source paths and messages, spread to neighbours |
| `tools/recomp/profile.py` | CPU time per function and library from a macOS `sample` (`profile.tsv`) |
| `tools/recomp/inspect.sh` | Ghidra's view of chosen functions (`c/<offset>.c`, `.s`), for analysis and annotations |
| `BB_RECORD` (`src/cpu/record.c`) | Calls of chosen functions recorded in the game (`records/<offset>.rec`) |
| `tools/recomp/replay.sh` | Recorded calls replayed and checked (`--recomp LIB`: the generated code) |
| `tools/recomp/recomp.sh` | Generated C for chosen functions, compiled (`librecomp.dylib`; `BB_RECOMP_LIB`) |
| `tools/recomp/fuzz.sh` | The generator's C for every instruction form, fuzzed against the interpreter |

```sh
D=deps/macos-arm64
clang -arch arm64 -O2 -I$D/include tools/recomp/scan.c $D/lib/libZydis.a $D/lib/libZycore.a \
    -o out/recomp/scan
GAME=/path/to/CUSA03173  # the game folder (1.09)
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
python3 tools/recomp/label.py out/recomp

# A profile: 5 s of every thread, then the main thread's functions.
SAMPLE=1 tools/mac_bench.sh prof BB_JIT_MAP=$PWD/out/jit.map
python3 tools/recomp/profile.py out/bench_prof_*.sample.txt out/jit.map out/bench_prof_*.log Thread_<id>

brew install ghidra
tools/recomp/inspect.sh 0x21b8710

# Record calls (OFFSET:SIZE from functions.tsv) and replay them.
tools/mac_bench.sh rec BB_RECORD=0x22928a0:247,0x227e230:579 BB_RECORD_EVERY=7 \
    BB_RECORD_DIR=$PWD/out/recomp/records
tools/recomp/replay.sh out/recomp/records/0x22928a0.rec
```

## Record and replay

With `BB_RECORD`, the translator leaves the chosen functions' first instruction to the
interpreter, which runs each recorded call in a tracer (`src/cpu/record.c`): one instruction at a
time, reading the memory each one accesses before and after it runs. The record holds the
registers at entry, the memory the function read (the first time in each epoch), the memory it
wrote, and the calls it made with their registers. Calls run at full speed; after each one the
memory the function reads is recorded again, so what the callee changed is in the record.

`tools/recomp/replay.sh` replays each record in a child process: the game image is mapped where it
was, memory is rebuilt from the record, and the function runs in the interpreter. Its calls are not
run: each must match the recorded target and argument registers, and is answered with the
recorded registers and memory. The registers at the return and every byte written must match.

Replaying the original function checks the record; generated code is checked the same way.
A record is flagged when another thread changed memory the function read during the call; such
calls (locks, job queues) cannot be replayed alone. The game's code must only touch guest memory
for a record to be replayable: the variables the game imports (the stack canary) live in guest
memory for that reason.

First results (2026-10-10): 8 functions, 50 calls each, recorded during the Hunter's Dream
cutscene with no visible slowdown. The 7 ordinary functions replay exactly on all 350 calls. The
job loop (`0x21b8710`) matches on 31 of 50; 18 of the others had other threads' writes.


## What the image contains

162,959 functions with unwind information (median 80 bytes), about 10 million instructions;
241,821 functions in all with those found through calls, jumps and code pointers (about 11.3
million instructions). No
C++ RTTI. About 500 source paths in assertion and log messages name the libraries:

| Library | Functions | Code | Known by |
|---|---:|---:|---|
| Game (`SPRJ\Source\SPRJ\Source\Game`, `Sys`) | ~31,000 | 6.5 MiB | source paths |
| Havok 2014.1 (physics, AI, cloth) | ~24,700 | 9.2 MiB | source paths, names |
| Dantelion2 (FromSoftware's core library) | ~17,400 | 4.7 MiB | source paths |
| Scaleform GFx (menus, HUD) | ~12,300 | 3.2 MiB | messages |
| FD4 (FromSoftware's engine) | ~9,600 | 2.9 MiB | source paths, names |
| FMOD Ex 4.44.50 | ~6,400 | 1.3 MiB | source paths |
| Lua (LuaPlus) | ~2,200 | 0.9 MiB | source paths |
| YEBIS (post-processing) | ~2,200 | 0.8 MiB | source paths |
| Not labelled yet | ~56,300 | 10.4 MiB | |

The labels are a first pass: a library's code is found by the paths in its own messages and the
functions between them, so the boundaries between game, FD4 and Dantelion2 are approximate.

## First profile (2026-10-10, cutscene in the Hunter's Dream)

The game's main thread is about 95% busy; 85% of its samples are in translated code. 287 of its
functions ran; the busiest 69 take half of the time. The busiest one (`0x21b8710`, 13.8%) runs
jobs and updates shared counters with atomic operations: the main thread waiting on, or helping,
worker threads.

## Gameplay profile (2026-10-10, 20 s in the Hunter's Dream)

The main thread's time is spread thin: 933 functions ran, and the busiest 145 take half of it. The
busiest one is the job loop (7.3%); no other function takes more than 1.5%. No small set of
functions dominates: the CPU side gets faster by recompiling all of them.

A batch of 16 of the hottest small and medium functions (46 to 1,125 bytes; game, FD4 and
Dantelion2 code), 60 calls each, recorded across loading, the cutscene and play: 15 replay exactly
on every call; one (`0x1d37400`) on 8 of 60, not explained yet.
