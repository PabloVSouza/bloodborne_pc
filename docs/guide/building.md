# Building from source

[← Documentation](../README.md)

## Requirements

- A Mac with Apple Silicon. macOS 15 or newer is recommended.
- Xcode Command Line Tools and [Homebrew](https://brew.sh):

  ```bash
  brew install bash pkgconf glslang nasm cmake ninja
  ```

- For the app: Node 22+ and Rust (the launcher is a [Tauri](https://tauri.app) app).
- A game folder prepared as in [Game files](game-files.md).

## Build and run

```bash
git clone --recursive https://github.com/PabloVSouza/bloodborne-recomp.git
cd bloodborne-recomp
bash build.sh                                  # the first run also builds the libraries (deps/)
BB_GAME_DIR=/path/to/your/game bash run.sh
```

- `build.sh` builds the native arm64 program. `BB_ARCH=x86_64` builds the older Rosetta 2 one.
- The first build compiles the dependencies (MoltenVK, SDL3, FFmpeg, ...) into `deps/` and takes
  a while.
- From a checkout, saves and the shader cache go to `user/` and settings to `bbport.ini`.

## The app and the DMG

```bash
bash packaging/macos.sh                        # Bloodborne.app and its DMG in dist/
```

The launcher lives in [`launcher/app`](../../launcher/app). Its
[README](../../launcher/app/README.md) covers development (`npm run dev` previews it in a browser
with sample data) and its code conventions.

Releases are built by GitHub Actions (`.github/workflows/macos.yml`): pushing a `v*` tag builds the
DMG and publishes a release.

## Environment variables

`run.sh` reads its options from the environment. The launcher sets them from its settings.

| Variable | Effect |
|---|---|
| `BB_GAME_DIR` | The game folder |
| `BB_RENDER_RES=WxH`, `BB_OUTPUT_RES=WxH` | Render and output size |
| `BB_UPSCALER=fsr3\|off` | Upscaler |
| `BB_FRAME_STATS=1` | Frame statistics in the log |
| `BB_LANGUAGE` | The game's language (PS4 language code) |
| `BB_UI_LANGUAGE` | The in-game menu's language (`en`, `pt-BR`, ...) |
| `BB_MODS_DIR`, `BB_MODS_ENABLED=0` | Mods folder, or no mods |
| `BB_PATCHES_DIR` | Third-party patches folder |
| `BB_VK_DRIVER=kosmickrisp` | Mesa's KosmicKrisp Vulkan driver instead of MoltenVK (macOS 26+, slower today) |
| `BB_SKIP_GAME_CHECK=1` | Skip the game folder check |

## Developer tools

- `tools/mac_bench.sh NAME [VAR=VALUE...]`: starts the game in the background, enters the level
  and prints frame-time statistics.
- `tools/mst_summary.py`, `tools/mst_passes.py`: read Metal System Traces.
- `tools/grab.sh [--screen] NAME`: saves the game's own frame as a PNG (`--screen` includes the
  in-game menu, which `touch out/menu.trigger` opens).
- `python3 -m unittest discover -s tests`: the Python tests.

Upstream's GTK4 launcher (`bash launcher/bb-launcher.sh`) also works, with
`brew install gtk4 libadwaita pygobject3`.
