# Butterscotch Pocket: Undertale on openfpgaOS (Analogue Pocket / MiSTer)

[Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), an
open-source GameMaker: Studio runner, built as an openfpgaOS app. It uses
Butterscotch's software renderer (draft PR #429) drawing RGB555 straight
into the openfpgaOS framebuffer, at 320x240 or 640x480 depending on the room.

You must supply your own `data.win` from Undertale v1.08.

## Layout

| Path | What |
|------|------|
| `main.c` | Entry point: registers `data.win` on data slot 4 and starts the runner |
| `platform/of_platform.c` | Butterscotch platform hooks on the `of_*` API (video, pad, timing) |
| `butterscotch/` | Butterscotch checkout, `openfpga` branch of the fork below (not tracked here) |
| `../../dist/undertale/` | Pocket core definition (core, data slots, instance JSON) |
| `tools/mkart.py` | Draws the menu banner and core icon into `dist/` (run by hand after changing the art; needs Pillow) |

Getting `butterscotch/`:

```bash
git clone -b openfpga https://github.com/zenibako/Butterscotch.git butterscotch
```

That branch is upstream's `sw-renderer` (draft PR #429) plus this port's
renderer, loader and platform-hook changes.

## Build

On macOS, `source ../../../env.sh` first (GNU sed + host RISC-V toolchain).

```bash
make            # RISC-V ELF + Pocket SD tree in ../../build/pocket/undertale/
make test       # desktop binary ./undertale_pc (run it next to a data.win)
make copy       # copy the core to a mounted Pocket SD card
```

Put your `data.win` in this directory (on macOS it is `game.ios` inside
`UNDERTALE.app/Contents/Resources/`, renamed), and link the folder with the
game's `.ogg` files as `music/`. The build copies it to
`Assets/undertale/common/` in the SD tree.

Desktop extras: `UT_DUMP_FRAME=<n> UT_DUMP_PATH=out.ppm ./undertale_pc`
writes frame `n` of the 320x240 output and exits. `UT_UNCAPPED=1` disables
frame pacing for timing runs. `UT_SEED=<n>` fixes the game's RNG and
`UT_SCRIPT="300:Z,340:D"` presses keys on given frames (U D L R, Z X C,
E = Enter), which together make runs repeatable for pixel comparisons. `make test WADS="14 16"`
enables older bytecode versions for testing other games.

## Controls

D-pad = arrows, A = Z (confirm), B = X (cancel), X/Y = C (menu),
Start = Enter. Select toggles a frame-time overlay: three numbers in
milliseconds over the last 30 frames (average work, worst work, worst frame
period; 33 means full speed). R shows the last log lines over the game.
L switches 640x480 rooms between native resolution and smoothed 320x240.

## Resolution

Undertale's window is 640x480. Overworld rooms show a 320x240 view scaled
2x, so they are rendered at 320x240. Battles and menus use the full 640x480
with small fonts, so those rooms are rendered at 640x480 and the display
mode is switched to match (`of_video_set_mode`). If the OS refuses the mode
everything stays at 320x240.

L switches to the alternative: every room at 320x240, with the renderer
averaging each 2x2 block of texels when it shrinks a 640x480 screen. Small
text is slightly soft but readable, and those screens cost about a quarter
of the pixels.

## Texture pack

`make` runs `tools/mktexpack` on your `data.win` to produce `textures.bin`
(data slot 5): every texture page already converted to the renderer's
16-bit format and run-length encoded. The device then loads pages without
decoding PNGs or allocating an RGBA intermediate. Without the pack the
renderer falls back to the PNGs inside `data.win`. Decoded pages are kept
in a least-recently-used cache that shrinks whenever less than
`TEXTURE_RESERVE_MB` (default 4) of heap would be left for the game.

## Sound

Decoding Vorbis live is too heavy for the 100 MHz CPU, so `make` runs
`tools/mkmusic` to produce `music.bin` (data slot 6): every sound as 32 kHz
mono IMA ADPCM, about 134 MB. It combines the external `.ogg` files the game
streams (from `music/`, a directory or symlink) with the effects embedded in
`data.win`.

`platform/of_audio_system.c` plays them: long tracks stream through a
read-ahead buffer, short ones are loaded whole on first use and cached. It
applies pitch and gain, mixes up to 16 voices and writes 48 kHz output.

Desktop: `UT_AUDIO_DUMP=out.raw` captures the mixed output (48 kHz stereo
s16le) and `UT_AUDIO_LOG=1` logs every effect, for checking without speakers.

## Saves

Undertale keeps its progress in a few small files (`file0`, `file9`,
`undertale.ini`, ...). `platform/ut_save_fs.c` stores them all as one small
archive in the first save slot (`undertale_0.sav`). The Pocket writes save
slots back to the SD card when the core is closed from its menu. The
benchmark neither reads nor writes saves.

To carry a save over from the desktop game, `make import-save
SAVE_DIR=<folder>` packs a desktop save folder into `undertale_0.sav`
(`SAVE_DIR` defaults to the macOS location, `~/Library/Application
Support/com.tobyfox.undertale`; on Windows it is `%LOCALAPPDATA%\UNDERTALE`,
on Linux `~/.config/UNDERTALE`). Copy that file to
`Saves/undertale/common/` on the SD card; it replaces any progress made on
the Pocket. `make export-save EXPORT_DIR=<folder>` goes the other way, and
`tools/mksave list <file>` shows what a slot file holds. The slot file is
deliberately not part of the SD tree, so copying a build never touches saves.

## Button prompts

The game's on-screen key prompts ("[Z or ENTER]", "[C]", the instruction
screen) are renamed to the Pocket's buttons in memory after loading; see
`platform/ut_strings.c`. `data.win` itself is not modified.

## On-device diagnostics

The boot log stays on screen while loading, and every Butterscotch log line
is prefixed with seconds since start, so load stages can be timed by eye.
If the app exits or aborts it halts with the log visible instead of
rebooting.

Any frame that takes longer than 150 ms logs a line such as
`Perf: slow frame 1840 ms: room 390, textures 1240 (1), sounds 90 (2)`:
the frame's work time, then the parts of it spent loading the room, texture
pages and sound effects (with counts). Whatever is left over is game code
and drawing. Press R after a hitch to read it.

## Status

- Runs on an Analogue Pocket (firmware 2.7, os25 bitstream): boots, plays
  the intro, name entry works, and the first room and menu are playable.
- Use the os25 bitstream. The SDK's runtime `os.bin` paired with os20
  reboot-looped before the OS banner appeared.
- Music, sound effects and saves work on hardware, including a save
  imported from the desktop game.
- Loading `data.win` takes about 19 s on the Pocket before the first frame.
