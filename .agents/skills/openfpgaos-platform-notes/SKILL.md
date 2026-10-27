---
name: openfpgaos-platform-notes
description: Hard-won facts about openfpgaOS and the Analogue Pocket that are not in the SDK documentation, or contradict it. Use this before writing or changing any platform code in src/openfpga/platform (video modes, display, file reads, saves, audio, input, memory), when choosing a bitstream variant or OS version, when a core boot-loops, shows a black, frozen or garbage screen, or when the SDK README says one thing and the device does another. Check here before trusting the SDK docs on anything listed.
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
- **Where Pocket runtimes come from (checked 2026-10-11).** The
  openfpgaOS/openfpgaOS repo has the OS source but no Pocket binaries; its
  tags are MiSTer releases. Pocket runtimes ship only inside
  openfpgaOS/openfpgaSDK (v0.7, `a408ddc`, which this repo pins) and the
  Diablo port's releases (v0.9; Diablo 1.1.0 was built from OS v0.9.5
  plus two commits). To answer "is there a newer OS", compare those two
  against what `build-pocket.yml` and `SDK_ROOT` use, not the OS repo's
  tags. Moving to v0.9 would bring fixes for audio re-arm races and for
  flips timing out while the Pocket's menu is open, at no speed gain
  (see the perf skill).
- **The frame buffer is on the cached alias** (`0x10xxxxxx`) in the OS
  source since `ca5bb0a` (2026-04-16). Both runtimes are built from OS
  revisions that contain it: the SDK's v0.7 runtime from `618a3eb`
  (2026-08-09) and Diablo 1.1.0's from v0.9.5 plus two commits (checked
  with `git merge-base --is-ancestor`). Not yet read
  back on the device; see the perf skill's frame-buffer section.

## Display

- Video modes above 320x240 exist (`of_video_set_mode`, up to 800x600,
  1 MB per frame) even though the README says 320x240 is the only one.
  640x480 RGB555 works on os25.
- **The OS text terminal does not display once the app is in its own
  16-bit mode**, in either `OF_DISPLAY_TERMINAL` or `OF_DISPLAY_OVERLAY`.
  It works at boot, before the first frame. Anything the user must read
  after that (log overlay, benchmark report, crash screen) has to be
  drawn into the framebuffer by the app; see `ofPerfDrawLogScreen` and
  `ofPlatformShowLogAndHalt`.
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

## The Pocket's menu

- A core whose platform the Pocket does not know is left out of the core
  list with no error. The Pocket caches platforms in
  `System/platforms_cache.bin` and did not rebuild it when a platform was
  renamed on the card (2026-10-08, firmware 2.7): `Platforms/` held the
  same number of files before and after, which is the suspected reason.
  Renaming the cache away made the core appear at the next start.
  `strings` on the cache shows which platform ids it holds; `sdcopy.sh`
  now sets it aside when it copies a platform that is new to the card.
- Things that were suspected and are not the cause of a missing core:
  a `description` over 63 characters (listed cores on the same card have
  up to 92), and `._` sidecar files beside the core folder.

## Saves

### Native Memories and sleep (observed 2026-10-15)

- On the save-state runtime, `core.json`'s `sleep_supported` is one per-core
  switch for both Memories and sleep, shared by Undertale and Deltarune. It
  needs the matching runtime service; there is no extra Deltarune sleep flag.
- A refused sleep capture does **not** keep Pocket awake. Hardware testing
  showed power-off followed by wake loading the previous successful sleep
  payload. Match capture and receive IDs before diagnosing a cold restart.
- Games launched under the same core can share a Memories list. Bind each
  payload to the game/chapter's exact data hash and show a game-font notice
  when a different game's or version's state is refused.
- These observations used the paired fork runtime `92202ab` (os25). Its
  SDK `of_savestate.h` documents the contract; this does not establish native
  save-state support in the older v0.7 runtime described elsewhere here.
  Use the build skill's native-save-state reference and the verify skill's
  save-state reference for staging, tests, and interpreting device logs.

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

## The Pocket's core menu (interact.json)

- A core cannot add menu options of its own that reach the app. The SDK's
  README says app options start at index 4 of `of_interact_get`, but that
  call reads a memory page nothing writes: in the hardware source
  (`core_top.v`, same at the v0.7 tag) menu writes land only in the
  Analogizer registers at 0xF7000000-0xF700000C and in 0xF7000010, which
  is the app id that an instance file's `memory_writes` sets at launch.
  An entry pointed at 0xF7000010 showed in the menu and could not be
  changed (tried 2026-10-08).
- What does reach the app from the Pocket's own screens: an instance
  file's OS config (`ARGS=`), read at launch, and nothing live. Live
  switches have to be buttons.

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
