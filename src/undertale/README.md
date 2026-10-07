# Undertale for openfpgaOS (Analogue Pocket / MiSTer)

[Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), an
open-source GameMaker: Studio runner, built as an openfpgaOS app. It uses
Butterscotch's software renderer (draft PR #429) drawing 320x240 RGB555
straight into the openfpgaOS framebuffer.

You must supply your own `data.win` from Undertale v1.08.

## Layout

| Path | What |
|------|------|
| `main.c` | Entry point: registers `data.win` on data slot 4 and starts the runner |
| `platform/of_platform.c` | Butterscotch platform hooks on the `of_*` API (video, pad, timing) |
| `butterscotch/` | Butterscotch checkout, `sw-renderer` PR branch plus two small patches (not tracked here) |
| `../../dist/undertale/` | Pocket core definition (core, data slots, instance JSON) |

Getting `butterscotch/`:

```bash
git clone https://github.com/ButterscotchRunner/Butterscotch.git butterscotch
cd butterscotch && git fetch origin pull/429/head:sw-renderer && git checkout sw-renderer
```

Then apply the two local patches on the `openfpga` branch of that checkout
(build-time overrides for `PIXEL_SIZE` / `TEXTURE_LRU_LENGTH`, and a typo fix
in the 16-bit colour blend).

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

## Music

Undertale streams its music from external `.ogg` files. Decoding Vorbis live
is too heavy for the 100 MHz CPU, so `make` runs `tools/mkmusic` over
`music/` (a directory or symlink holding the game's `.ogg` files) to produce
`music.bin` (data slot 6): every track as 32 kHz mono IMA ADPCM, about
130 MB. `platform/of_audio_system.c` streams tracks from the pack, applies
pitch and gain, mixes up to four at once and writes 48 kHz output.

Sounds embedded in `data.win` (short effects) are not played yet.

Desktop: `UT_AUDIO_DUMP=out.raw` captures the mixed output (48 kHz stereo
s16le) for checking without speakers.

## Benchmark and OS comparison

`--bench` in the app arguments (the `ARGS=` line of the OS config) runs a
fixed, scripted play-through of the opening with a fixed seed and no frame
pacing, then prints milliseconds per frame for four sections and halts.

`make compare` adds a Benchmark instance to this core and two more cores
built against the v0.9 SDK and runtime (`SDK09`, by default the Diablo
port's checkout next to this repo): `Undertale09os25` and
`Undertale09os20`. All three share the game data. `make compare-copy`
puts the lot on the SD card.

## Button prompts

The game's on-screen key prompts ("[Z or ENTER]", "[C]", the instruction
screen) are renamed to the Pocket's buttons in memory after loading; see
`platform/ut_strings.c`. `data.win` itself is not modified.

## On-device diagnostics

The boot log stays on screen while loading, and every Butterscotch log line
is prefixed with seconds since start, so load stages can be timed by eye.
If the app exits or aborts it halts with the log visible instead of
rebooting.

## Status

- Runs on an Analogue Pocket (firmware 2.7, os25 bitstream): boots, plays
  the intro, name entry works, and the first room and menu are playable.
- Use the os25 bitstream. The SDK's runtime `os.bin` paired with os20
  reboot-looped before the OS banner appeared.
- Music plays on desktop (verified against a reference decode); not yet
  heard on hardware. Sound effects are not implemented.
- Saves are not mapped to save slots yet.
- Loading `data.win` takes about 28 s on the Pocket before the first frame.
