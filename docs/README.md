# Documentation

[← Back to the project](../README.md)

## Playing

1. [Installation](guide/installation.md): download, the first start, where your data lives.
2. [Game files](guide/game-files.md): preparing your dump, supported editions, The Old Hunters.
3. [Settings](guide/settings.md): the launcher, the in-game menu, languages, controllers.
4. [Mods and patches](guide/mods-and-patches.md): installing mods, load order, third-party
   patches.
5. [Performance](guide/performance.md): what to expect, and the settings that matter.
6. [Troubleshooting](guide/troubleshooting.md): common problems and known issues.

## Developing

- [Building from source](guide/building.md): build, run, package, environment variables, tools.
- [How it works](guide/how-it-works.md): the translator, the runtime, the renderer and the app.
- [Native arm64](ARM64_NATIVE.md): the x86-64 → arm64 translator in depth.
- [macOS performance](MACOS_PERFORMANCE.md): measurements, what was changed, what could still be
  gained.
- [Recompilation](RECOMPILATION.md): turning the game's code into native code with a recompiler
  that runs on the player's own copy (in progress,
  [status](https://github.com/PabloVSouza/bloodborne-recomp/wiki/Recompilation-status)).
- [Launcher](../launcher/app/README.md): the launcher's stack and code conventions.

## Project

- [Credits and licenses](CREDITS.md)

## Upstream notes

Design notes inherited from [bbport](https://github.com/deadinside28/bloodborne_pc), the Linux port
this project is based on. Most of them are in Russian, and they describe the Linux version.

- [Upstream README](upstream/README.md) ([Русский](upstream/README.ru.md))
- [Roadmap](ROADMAP.md), [development log](DEVELOPMENT_LOG.ru.md), [mods](MODS.md)
- Renderer: [upscaling](upscaler.md), [motion vectors](motion_vectors.md),
  [depth-adaptive TAA](DEPTH_ADAPTIVE_TAA.md), [parallel GPU processing](parallel_gpu.md),
  [TAA review](TEMPORAL_REVIEW_2026-10-01.md)
- Change notes: [2026-10-02](CHANGES_2026-10-02.md), [2026-10-03](CHANGES_2026-10-03.md),
  [2026-10-06](CHANGES_2026-10-06.md), [history/](history)
