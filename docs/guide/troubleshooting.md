# Troubleshooting

[← Documentation](../README.md)

## "Bloodborne can't be opened"

The app is not signed with an Apple Developer ID. Right-click it in Applications and choose
**Open**, or allow it in **System Settings → Privacy & Security**. See
[Installation](installation.md#the-first-time-you-open-it).

## The launcher says the game folder has a problem

The message says what is wrong. The common cases (update not merged, old `eboot.bin`, another
edition) are explained in [Game files](game-files.md#when-the-launcher-reports-a-problem).

## The first start takes a long time

The first start compiles the game's shaders, which takes a few minutes. It happens again after an
update that changes the shader cache format.

## The game stops or crashes

1. Open the launcher's **Log** tab, or `~/Library/Application Support/bloodborne_mac/logs/last.log`.
2. Start the game again. Some known crashes are intermittent (below).
3. If it keeps happening, [open an issue](https://github.com/PabloVSouza/bloodborne-recomp/issues)
   with the log, your Mac model and macOS version.

Please do not report problems with this port to shadPS4 or to the upstream project.

## Known issues

- **Crash while a save loads**, roughly 1 load in 10 (a corrupted GPU command buffer). Starting
  again works.
- **Switching FSR on and off while playing with character motion vectors on** can crash the GPU
  ("Invalid Resource"). It does not happen with them off, the default.
- Two of the game's pipelines fail to compile in MoltenVK; their draws are skipped.
- *The Old Hunters* is only available with the Game of the Year editions: add-on packages are not
  loaded yet.
- FSR 4 / 4.1.1 and upstream's experimental PC memory model are not available on macOS.

## Starting fresh

To reset the settings, quit the launcher and delete `bbport.ini` and `launcher.json` in
`~/Library/Application Support/bloodborne_mac`. Your saves in `user/` are kept.
