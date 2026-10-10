<h1 align="center">bloodborne-recomp</h1>

<p align="center">
  <strong>Bloodborne, running natively on Apple Silicon.</strong><br>
  A macOS port of the PS4 game, with a launcher app and an in-game settings menu. No Rosetta 2.<br>
  And a recompiler on its way to turning the game's code, from your own copy, into native code.
</p>

<p align="center">
  <sub>Built on <a href="https://github.com/deadinside28/bloodborne_pc"><strong>bbport</strong></a> by deadinside28, the native Linux port it started from, and on <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>.</sub>
</p>

<p align="center">
  <a href="https://github.com/PabloVSouza/bloodborne-recomp/releases/latest"><strong>Download</strong></a>
  ·
  <a href="docs/guide/installation.md">Installation</a>
  ·
  <a href="docs/README.md">Documentation</a>
  ·
  <a href="https://github.com/PabloVSouza/bloodborne-recomp/issues">Report a problem</a>
</p>

<p align="center">
  <img src="docs/screenshots/title.jpg" alt="Bloodborne's title screen on a Mac" width="98%">
</p>
<p align="center">
  <img src="docs/screenshots/hunters-dream.jpg" alt="The Hunter's Dream at 60 FPS with FSR 3.1 on an Apple M3 Pro" width="49%">
  <img src="docs/screenshots/game.jpg" alt="Bloodborne running on an Apple M3 Pro" width="49%">
</p>
<p align="center">
  <img src="docs/screenshots/game-menu.jpg" alt="The in-game settings menu" width="49%">
  <img src="docs/screenshots/launcher-home.png" alt="The launcher's home tab" width="49%">
</p>
<p align="center">
  <img src="docs/screenshots/launcher-graphics.png" alt="The launcher's graphics settings" width="49%">
  <img src="docs/screenshots/launcher-controls.png" alt="The launcher's controls tab" width="49%">
</p>

> [!IMPORTANT]
> **No game files are included.** You need your own dump of Bloodborne with the 1.09 update.
> See [Game files](docs/guide/game-files.md).

## Features

- **Native on Apple Silicon:** the game's x86-64 code is translated to arm64 as it runs.
- **One app, nothing else to install:** download, open, choose your game folder, play.
- **A launcher** with graphics, controls, game options, mods, patches and the game's log.
- **AMD FSR 3.1 upscaling,** the biggest performance gain on Apple GPUs.
- **An in-game settings menu** (F1, or L3 + R3 on a controller).
- **Controllers and keyboard,** with button mapping for both.
- **Mods and patches,** with load order. The game folder is never changed.
- **20 languages** for the launcher and the menu, the same as the PS4 game.

## Getting started

1. [Download the latest release](https://github.com/PabloVSouza/bloodborne-recomp/releases/latest)
   and drag **Bloodborne** to Applications.
2. The first time, right-click the app and choose **Open** (the app is not signed with an Apple
   Developer ID).
3. Choose your game folder and press **Play**.

The [installation guide](docs/guide/installation.md) has the details.

## Status

**Experimental, playable.** The game boots, loads saves and plays with sound, controllers and
saving. On an Apple M3 Pro it runs at about **44 FPS** at 1080p with FSR 3.1
([performance](docs/guide/performance.md)). Only one machine has been tested so far. See the
[known issues](docs/guide/troubleshooting.md#known-issues).

**In progress: recompilation.** A recompiler that turns the game's code, from the player's own
copy, into native code, checked against calls recorded from the original. Every one of the game's
functions is generated and the game runs on them; the work now is speed. Only the tool is
published, never the game's code ([plan](docs/RECOMPILATION.md),
[status](https://github.com/PabloVSouza/bloodborne-recomp/wiki/Recompilation-status)).

## Documentation

| | |
|---|---|
| [Installation](docs/guide/installation.md) | Download, first start, where your data lives |
| [Game files](docs/guide/game-files.md) | Preparing your dump, supported editions, The Old Hunters |
| [Settings](docs/guide/settings.md) | The launcher, the in-game menu, languages, controllers |
| [Mods and patches](docs/guide/mods-and-patches.md) | Installing mods and third-party patches |
| [Performance](docs/guide/performance.md) | What to expect, and the settings that matter |
| [Troubleshooting](docs/guide/troubleshooting.md) | Common problems and known issues |
| [Building from source](docs/guide/building.md) | For developers: build, run, package |
| [How it works](docs/guide/how-it-works.md) | The translator, the runtime and the renderer |
| [Recompilation](docs/RECOMPILATION.md) | Turning the game's code into native code (in progress) |

## Acknowledgements

This project (formerly the bloodborne_mac repository) started as a fork of [**bbport**](https://github.com/deadinside28/bloodborne_pc) by
deadinside28, a native Linux port of Bloodborne whose renderer is built on
[**shadPS4**](https://github.com/shadps4-emu/shadPS4), and it still builds on both: the loader, the
PS4 runtime, the renderer and its extensions, the upscalers and the patches come from them, and
bbport's releases are merged here as they come out (now 0.5), its history kept. This project adds
the macOS platform layer, its own x86-64 → arm64 translator, the macOS app and the recompiler. It
became a separate repository as those grew apart from a Linux port's; none of this would exist
without the projects it is built on. Full credits: [Credits and licenses](docs/CREDITS.md).

Licensed under the [GNU GPL v2 or later](LICENSE).

<sub>Not affiliated with shadPS4, Sony Interactive Entertainment, FromSoftware or AMD. Please do
not report problems with this project to shadPS4 or to bbport.</sub>
