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

`data.win` goes in `Assets/undertale/common/` on the SD card.

Desktop extras: `UT_DUMP_FRAME=<n> UT_DUMP_PATH=out.ppm ./undertale_pc`
writes frame `n` of the 320x240 output and exits. `make test WADS="14 16"`
enables older bytecode versions for testing other games.

## Controls

D-pad = arrows, A = Z (confirm), B = X (cancel), X/Y = C (menu),
Start = Enter, Select = Esc.

## Status

- Builds for RISC-V and desktop; desktop output verified with Butterscotch's
  bundled test games.
- Not yet run on hardware, and not yet run against Undertale's `data.win`.
- No audio yet (Butterscotch's no-op audio backend).
- Saves are not mapped to save slots yet.
