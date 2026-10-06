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
`UNDERTALE.app/Contents/Resources/`, renamed). The build copies it to
`Assets/undertale/common/` in the SD tree.

Desktop extras: `UT_DUMP_FRAME=<n> UT_DUMP_PATH=out.ppm ./undertale_pc`
writes frame `n` of the 320x240 output and exits. `UT_UNCAPPED=1` disables
frame pacing for timing runs. `make test WADS="14 16"`
enables older bytecode versions for testing other games.

## Controls

D-pad = arrows, A = Z (confirm), B = X (cancel), X/Y = C (menu),
Start = Enter, Select = Esc.

## Status

- Runs on an Analogue Pocket (firmware 2.7, os25 bitstream): boots, plays
  the intro, name entry works, and the first room and menu are playable.
- Use the os25 bitstream. The SDK's runtime `os.bin` paired with os20
  reboot-looped before the OS banner appeared.
- No audio yet (Butterscotch's no-op audio backend).
- Saves are not mapped to save slots yet.
- The texture cache is a count-limited ring. Undertale has four 2048x2048
  pages (8 MB each at 16 bpp, 16 MB more while decoding), so later rooms
  need a byte budget or pre-converted textures. The app has about 51 MB of
  heap on the Pocket.
