---
name: butterscotch-pocket-verify
description: Use when validating Butterscotch Pocket renderer, VM, native-script, string, audio, save-state or platform changes; reproducing game scenes; comparing pixels and PCM; testing restore chains; or interpreting supplied device logs before a Pocket test.
---

# Verifying on the desktop build

For archived benchmark evidence and platform/configuration comparisons, use the
[pocket-benchmarking router](../pocket-benchmarking/SKILL.md) and
[integration guide](../../../src/openfpga/docs/BENCHMARKING.md).
Collector verification preserves evidence; retain the independent game/output and
physical Pocket lifecycle checks below.

For routine supported os25 updates, follow the integration guide's
[preserved-baseline checks](../../../src/openfpga/docs/BENCHMARKING.md#routine-regression-checks)
before another device benchmark campaign. Keep an immutable accepted reference
and fresh candidates; simulated success retains all applicable device gates.

The Pocket can only be tested by the user: copy to SD, walk to the device,
report back. That loop is slow and they pay for it, so everything that can
be checked on the desktop should be, first. `undertale_pc` runs the same
renderer, loader, audio mixer and save code as the device through the
SDK's SDL shim, and it can be driven without a display or speakers.

Be straightforward about what desktop checks cannot show: timing, memory
limits, SD read speed, video-mode behaviour and anything else specific to
the hardware. Say which of your claims were verified here and which are
still untested on the device.

## Desktop tools and specialized checks

For Memories, sleep/wake, cutscene captures, or an attached device log, read
[Save-state verification](references/save-states.md). It covers fresh-process
comparisons, scene-policy checks, repeat wakes, and matching saved payloads to
received payloads. Ordinary screenshot comparisons do not exercise restoration.

For native-script, string, audio-tail or build-cache optimizations, also read
[optimization checks](references/optimization-checks.md). It maps the targeted
fixtures and distinguishes deterministic output evidence from desktop timing.

Before running desktop captures, input replay, tracing or environment controls,
read [desktop capture tools](references/desktop-tools.md). It contains the
working-directory requirements, frame helper commands and complete `BS_*`
reference. Keep seeds and initial saves matched; use `BS_PAD` for Pocket
controller behavior, since keyboard scripts exercise a different path.

## Scripts and landmarks

`assets/naming.script` gets through the title, instruction screen and name
entry (choosing "A"). `assets/flowey.script` continues: walks through the
first room, into Flowey's room, through his dialogue and into the battle.
With that script and seed 7:

| Frame | What is on screen | Size |
|---|---|---|
| 300, 1500 (no script) | intro story panels | 320x240 |
| 345 | "Is this name correct?" | 320x240 |
| 430 | fade to white after naming | 320x240 |
| 930 (naming script + `,900:C`) | first room with the menu open | 320x240 |
| 1300 | Flowey's dialogue, "Howdy!" | 320x240 |
| 1700 | Flowey's dialogue, later line | 320x240 |
| 2400 | Flowey battle | 640x480 |

To reach somewhere new, extend a script and look at a few frames; expect
two or three attempts to get movement distances right.

### Recordings instead of scripts

A script has to be guessed frame by frame. A recording is made by playing:
anyone at the Mac's own screen can play to the spot once, and the file
then replays exactly, as often as needed. Use one whenever the scene is
more than a short walk away, and to reproduce a bug the user can show on
the desktop build. Ask the user to record it; you cannot play
interactively.

This example records **from no progress**, matching the helper's reset before
each replay. From the checkout root:

```bash
CHECKOUT=$(pwd -P)
record_dir=$(mktemp -d "${TMPDIR:-/tmp}/butterscotch-record.XXXXXX")
for asset in data.win textures.bin music.bin; do
  ln -s "$CHECKOUT/src/openfpga/games/undertale/$asset" "$record_dir/$asset"
done
(cd "$record_dir" && BS_SEED=7 BS_RECORD="$record_dir/ruins.json" \
  "$CHECKOUT/src/openfpga/undertale_pc") # play, then close the window
BS_RUN_DIR="$record_dir" \
  "$CHECKOUT/.claude/skills/butterscotch-pocket-verify/scripts/bs-frames.sh" \
  -e BS_PLAYBACK="$record_dir/ruins.json" 5200 5300 # no -s
```

- Record and replay with the same seed (bs-frames.sh uses 7). The example's
  new directory contains only asset links; the helper clears generated progress
  there before each requested frame. Keep original saves and attachments intact.
- For a recording from existing progress, keep a pristine copy of every initial
  progress file before recording. Restore those copies into a fresh disposable
  directory before **each** replay and invoke the desktop binary directly with
  `BS_PLAYBACK`, `BS_DUMP_FRAME`, and `BS_DUMP_PATH`. The helper's default deletes
  progress; `-k` keeps it but does not rewind it between multiple requested frames.
- A recording is by frame number, so it replays the same uncapped. Checked
  with flowey.script: recorded through `BS_RECORD`, replayed without the
  script, frames 1300 and 2400 identical.
- To go further from the end of one: `BS_PLAYBACK=a.json BS_RECORD=b.json`.
- Turn a script into a recording by running it once with `BS_RECORD`.
- It records the game's keys, not Pocket buttons: Select chords, L and R
  are not in it.
- A change that alters game logic or timing in frames (not just speed)
  makes an old recording drift, like a script would.

## The standard check for a change

1. Before the change, dump reference frames covering the code you are about
   to touch (at least one 320x240 scene and the 640x480 battle; add the
   fade at 430 for blending work, the menu at 930 for texture-cache work).
2. Make the change, rebuild, dump the same frames with a different prefix.
3. `ppmtool.py cmp` each pair. An optimisation should report `identical`.
   For an intended visual change, look at the PNG and describe what changed.
4. For a renderer change, repeat one capture with `-e BS_SKIP=2`: it must
   match the unskipped one (frame skipping relies on a skipped frame
   leaving nothing behind that a later frame needs).
5. Report exactly which frames you compared. After a base refresh or conflict
   resolution, rerun affected checks and captures on that integrated code.
6. For user-visible changes, prepare reviewable PNGs with source commit, scene,
   resolution and replay/configuration details. Follow
   [PR preparation](../butterscotch-pocket-build/references/pr-preparation.md)
   for matched baselines, durable publication and description authorization.
   A mapping/disassembly check is not a screenshot of the requested scene.

For audio, `BS_AUDIO_DUMP` supports listening and inspection, but its wall-clock
sample demand does not establish deterministic PCM identity. Offset alignment
alone cannot correct unequal demand. Use deterministic decoder/mixer tests or
a separately validated fixed-clock capture for exact comparisons. Save a WAV
for listening when useful, and report device listening separately.

## Lint and sanitizers

```bash
make -C src/openfpga lint    # strict warnings on the port's own files; prints "lint: clean"
make -C src/openfpga asan    # builds undertale_pc_asan (ASan + UBSan)
```

Run `lint` after touching `main.c`, `platform/` or `tools/`. Run the frame
scripts through `undertale_pc_asan` after changing anything that indexes
memory (renderer, caches, audio buffers): it takes the same `BS_*`
variables and is a few times slower. On macOS it needs
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`, or SDL blocks on a dialog
that only shows on the Mac's own screen; run it under a watchdog. Use
`ASAN_OPTIONS=detect_leaks=0`. One finding is known and not ours:
`stb_ds.h:1132`, a left shift in the vendored hash function. A function
Butterscotch calls into the port needs its prototype in
`platform/of_hooks.h`, or lint reports it.

## Looking inside the game

When behaviour is wrong rather than slow, Butterscotch's own tracing is
there on the desktop build. Already wired: `BS_DUMP_STATE` (every instance
and variable at a frame) and `BS_DISASM` (a code entry's bytecode). The
rest of its `--trace-*` family (variable reads and writes, function calls,
alarms, events, collisions, instance creation, tiles, opcodes, stack) is
not wired to a variable, but each is one line in `main.c` next to
`BS_DISASM`: put the name to trace (or `*`) in the matching
`args.*ToBeTraced` map, as `src/cli/args.c` does. Add the one the bug
needs rather than all of them; `*` on a running game prints thousands of
lines a frame.

## Traps that have cost time

- **Saves leak between runs.** The game writes `undertale.ini` once you
  name the character, and Flowey's dialogue changes on a later run. Two
  renders that "should" match will differ for that reason alone.
  `bs-frames.sh` deletes the save before each frame unless given `-k`.
- **Unseeded runs are not repeatable** on screens with random motion (the
  shaking name on the confirm screen). Always pass a seed when comparing.
- **Rooms change size.** Battle and menu rooms are 640x480, overworld rooms
  320x240; a montage or crop that assumes one size will be garbage.
- **Uncapped audio dumps are time-compressed.** With `BS_UNCAPPED=1` game
  frames run faster than real time while audio advances by real elapsed
  time, so events bunch up. For a listenable capture, run without it.
- **The desktop hides memory pressure.** malloc never fails there and the
  texture cache never has to shrink, so cache behaviour on the device can
  only be inferred from the log lines ("Loaded TXTR page", "Unloaded").
- **Desktop mixer stubs.** The SDK's PC shim implements `of_audio_write`
  but stubs the hardware mixer (`of_mixer_*`), which is one reason the port
  mixes in software.
