---
name: openfpgaos-platform-notes
description: Hard-won facts about openfpgaOS and the Analogue Pocket that are not in the SDK documentation, or contradict it. Use this before writing or changing any platform code in src/undertale/platform (video modes, display, file reads, saves, audio, input, memory), when choosing a bitstream variant or OS version, when a core boot-loops, shows a black, frozen or garbage screen, or when the SDK README says one thing and the device does another. Check here before trusting the SDK docs on anything listed.
---

# openfpgaOS on the Pocket: what the docs don't tell you

Everything here was observed on the user's Analogue Pocket (firmware 2.7)
with the SDK's v0.7 runtime unless marked otherwise. Items marked
*inferred* fit the evidence but were not confirmed directly.

## Runtime and bitstreams

- The SDK repo ships runtime v0.7. v0.9 exists for the Pocket only inside
  game ports; `../Diablo/` carries a v0.9 SDK tree and runtime, which
  `make compare` uses (`SDK09`).
- **Pair os25 with the SDK's `os.bin`.** os20 with the v0.7 `os.bin`
  boot-loops ("Booting..." / "Loading..." alternating) before the OS
  banner. The OS binary's own text says bitstream and `os.bin` must come
  from one build.
- v0.9 os20 boots and runs the game at 320x240, but produced a garbage
  blue screen once the port had used 640x480 in 16-bit colour. Diablo
  uses 640x480 on os20 in 8-bit mode. *Inferred:* os20 mishandles
  640x480 at 16 bpp or the switch back from it.
- The app-facing API barely changed between v0.7 and v0.9; the same
  sources build against either.

## Display

- Video modes above 320x240 exist (`of_video_set_mode`, up to 800x600,
  1 MB per frame) even though the README says 320x240 is the only one.
  640x480 RGB555 works on os25.
- **The OS text terminal does not display once the app is in its own
  16-bit mode**, in either `OF_DISPLAY_TERMINAL` or `OF_DISPLAY_OVERLAY`.
  It works at boot, before the first frame. Anything the user must read
  after that (log overlay, benchmark report, crash screen) has to be
  drawn into the framebuffer by the app; see `utPerfDrawLogScreen` and
  `utPlatformShowLogAndHalt`.
- `of_video_surface()` returns the back buffer and changes after
  `of_video_flip()`. Rendering straight into it works and removes a
  full-frame copy; fetch the pointer again every frame, including for
  anything that clears before drawing.
- An app that returns from `main`, calls `exit` or aborts is restarted by
  the OS, which looks like a boot loop. The port installs handlers that
  halt with the log on screen instead (`platform/of_diag.c`).

## Files

- First-time reads run at about 1.1 MB/s regardless of request size or
  destination; repeat reads of recently read data are about 13 MB/s.
  Design loaders around bytes read, not around call patterns. Do not
  benchmark by re-reading one region.
- Three data slots (4, 5, 6) are all there is for game data. Slot 7 is the
  OS's SoundFont slot and slot 8 is reserved. Names are limited to 23
  characters and each slot's extensions are listed in the core's
  `data.json`.
- A file a slot names is opened with plain `fopen` by that name.
  `of_file_slot_register(slot, name)` at startup is what the ports do for
  data slots.
- The game opens its own `undertale.ini`; an OS config with the same name
  on slot 2 would be read as the game's settings. The OS config is
  `undertale_os.ini` for that reason.
- The OS config is `[os]` with `ELF=`, `ARGS=` and `VARIANT=` lines.
  `ARGS` reaches `main` as `argv`; scan from index 0.
- `of_file_set_idle_hook` runs a callback while a blocking read waits, and
  the callback must not read files itself. The audio backend uses it to
  keep music going during loads.

## Saves

- Save slots 10–19 are 256 KB files named in the instance JSON
  (`undertale_0.sav` ...), read and written with `fopen`/`fwrite`.
- The Pocket writes save slots back to the SD card when the core is left
  through the Analogue menu (per the Diablo port's documentation; powering
  off without that may lose the save).
- A slot that has never been written may open successfully and read as
  blank, so validate a header rather than relying on `fopen` failing.

## Memory

- About 51 MB of heap, not the 3 MB the README's memory-map diagram
  suggests. `of_get_caps()->heap_size` reports it at boot.
- malloc can fail, unlike on desktop. The texture cache gives up pages
  when an allocation fails and keeps a reserve free
  (`TEXTURE_CACHE_RESERVE_BYTES`).
- The FPU is single precision. The port builds Butterscotch with
  `USE_FLOAT_REALS` and `NO_RVALUE_INT64`, as the PS2 port does.

## Audio

- `of_audio_write` takes stereo pairs at 48 kHz into a ring; on the Pocket
  the ring is about 2.7 s deep, so queue depth is a latency decision, not
  a capacity one. The port keeps about 100 ms queued.
- The hardware mixer (`of_mixer_*`) is stubbed in the desktop shim, so
  code using it cannot be tested off the device.

## Input

- D-pad, A, B, X, Y, Start, Select, L1 and R1 all reach the app on the
  Pocket. The port maps A/B/X/Y/Start to Z/X/C/C/Enter and keeps Select,
  L and R for its own overlays.
- Do not map anything to Escape: holding Esc quits Undertale, which
  restarts the core.

## Building on macOS

- The SDK scripts need GNU sed, and its Docker build fails without
  keychain access; `env.sh` selects Homebrew's GNU sed and
  `riscv64-elf-gcc` with `USE_SDK_CONTAINER=0`.
- The SDK's own desktop test rule (`app_pc`) does not build on macOS
  (`sys/auxv.h`); the port's Makefile has its own `undertale_pc` target.
- The SDK's `copy.sh` fails silently when no card is mounted and would try
  to mount any unmounted FAT partition, including a fixed disk's EFI
  partition. The port uses `tools/sdcopy.sh` instead.
- macOS writes `._*` sidecar files onto the card, which the Pocket can
  list as junk entries; remove them with `dot_clean -m`.

## Reference material

- `../Diablo/` is a complete, released port on the same SDK and the best
  source of working patterns (video mode setup, out-of-memory reporting,
  per-game packaging). Read it before inventing an approach.
- The SDK's `CLAUDE.md` covers the GPU, hardware mixer and input APIs and
  the rule against editing SDK-owned directories.
