# Installation

[← Documentation](../README.md)

## Requirements

- A Mac with Apple Silicon (M1 or newer).
- macOS 13 or newer. macOS 15 or newer is recommended: it lets the game keep its GPU memory
  resident, which is faster.
- Your own decrypted dump of Bloodborne with the 1.09 update. See [Game files](game-files.md).

Nothing else is needed: the app includes everything it uses to run the game.

## Install

1. Download the latest DMG from the
   [Releases page](https://github.com/PabloVSouza/bloodborne-recomp/releases/latest)
   (`bloodborne_mac-<version>-arm64.dmg`).
2. Open the DMG and drag **Bloodborne** to **Applications**.
3. Open **Bloodborne** from Applications.

### The first time you open it

The app is not signed with an Apple Developer ID, so macOS blocks the first launch. To allow it:

- right-click (or Control-click) **Bloodborne** in Applications, choose **Open**, then **Open**
  again; or
- open it once, then go to **System Settings → Privacy & Security** and choose **Open Anyway**.

macOS remembers the choice. You only need to do this once per version.

## First start

1. On the **Home** tab, choose your game folder (the folder that holds `eboot.bin` and
   `sce_sys`).
2. The launcher checks the folder and shows the game's edition, region and version. If something
   is wrong, it tells you what and how to fix it. See [Game files](game-files.md).
3. Press **Play**.

The first start compiles the game's shaders and takes a few minutes. Later starts are faster.
After an update that changes the shader cache format, the shaders are compiled again.

## Where your data lives

Everything the app writes is kept in `~/Library/Application Support/bloodborne_mac`:

| Path | Contents |
|---|---|
| `user/` | Saves and the shader cache |
| `bbport.ini` | Graphics and control settings (shared with the in-game menu) |
| `launcher.json` | Launcher settings (game folder, language, start-up options) |
| `mods/`, `mods.json` | Mods and their load order |
| `patches/`, `patches.json` | Third-party patches and which are enabled |
| `logs/last.log` | The log of the last run |

The game folder is never modified.

## Updating

Download the new DMG and replace the app in Applications. Your saves and settings stay in the data
folder above.

## Uninstalling

Delete **Bloodborne** from Applications. To remove your saves and settings too, delete
`~/Library/Application Support/bloodborne_mac`.
