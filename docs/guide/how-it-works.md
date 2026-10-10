# How it works

[← Documentation](../README.md)

Bloodborne is a PS4 game: its code is x86-64 machine code written for the PS4's operating system
and GPU. Running it on a Mac takes three things: running x86-64 code on an arm64 CPU, providing
the PS4 system functions the game calls, and translating its graphics to the Mac's GPU.

## CPU: `bbcpu`, an x86-64 → arm64 translator

The game's code has no source, so it is translated block by block into arm64 code while the game
runs ([`src/cpu/`](../../src/cpu)), with an interpreter as a fallback.

- Guest registers live in host registers.
- CPU flags are only computed where something reads them.
- x86's strong memory ordering is kept with arm64 acquire/release accesses.
- A fuzzer checks each translation against the interpreter.

This is what lets the game run natively, without Rosetta 2. Details:
[Native arm64](../ARM64_NATIVE.md).

## System: the PS4 runtime

The runtime ([`src/runtime_*.c`](../../src)) implements the PS4 operating system functions the
game uses: memory, threads, files, audio, controllers and saves.

## Graphics: a shadPS4-derived renderer on Metal

The renderer, derived from [shadPS4](https://github.com/shadps4-emu/shadPS4), translates the PS4
GPU's commands and shaders to Vulkan. [MoltenVK](https://github.com/KhronosGroup/MoltenVK) runs
that Vulkan on Metal.

On top of the game's own rendering, the port adds:

- **Upscaling:** AMD FSR 3.1 renders the game below the output resolution and reconstructs a
  sharp image at the output resolution.
- **An in-game settings menu** (Dear ImGui), drawn over the game.

## The app

`Bloodborne.app` contains the game program, its libraries, a bundled Python and bash for the
start-up scripts, and the launcher ([`launcher/app`](../../launcher/app), Tauri with React). The
launcher writes the settings and starts `run.sh`. `run.sh` checks the game folder, prepares
patches and mods, and starts the game.

## Where it is going: native code

Translating the game's code works, but costs CPU time, and every frame still goes through the PS4
GPU's command format, shadPS4 and MoltenVK. The [recompilation](../RECOMPILATION.md) work builds a
**recompiler**: a tool that runs on the player's Mac, reads their own `eboot.bin` and writes the
game's functions out as C, compiled into a native library the game loads.

1. Calls of game functions are recorded while the game runs: the memory they read and write, and
   the calls they make.
2. The recompiler generates C for the functions, and the generated code is checked against those
   recordings.
3. The generated functions replace the translated ones, behind a switch; anything else keeps
   running in the translator.

Only the recompiler and its tools are published, never the game's code.
The goal is a native port: the engine's graphics drawing with Metal directly. Progress:
[Recompilation status](https://github.com/PabloVSouza/bloodborne_mac/wiki/Recompilation-status).

## Where it comes from

Almost everything that makes the game run (the loader, the PS4 runtime, the renderer and its
extensions, the upscalers and the patches) comes from
[bbport](https://github.com/deadinside28/bloodborne_pc), the Linux port this project started from as
a fork, and shadPS4. bloodborne_mac adds the macOS platform layer, the arm64 translator, the macOS
app and the recompiler. Upstream's README is kept in
[upstream/README.md](../upstream/README.md).
