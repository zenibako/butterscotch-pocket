---
name: undertale-pocket-verify
description: Verify changes to the Undertale Pocket port on the desktop build before the user spends an SD-card trip on them. Use this for any change to the renderer, loader, audio, saves, input or platform layer in src/undertale — to check frames are pixel-identical, reproduce a scene the user describes, drive the game to a specific point with scripted input, capture audio, or produce a screenshot to show the user. Use it even when the change "obviously" can't affect output; a card trip costs the user several minutes and a desktop check costs seconds.
---

# Verifying on the desktop build

The Pocket can only be tested by the user: copy to SD, walk to the device,
report back. That loop is slow and they pay for it, so everything that can
be checked on the desktop should be, first. `undertale_pc` runs the same
renderer, loader, audio mixer and save code as the device through the
SDK's SDL shim, and it can be driven without a display or speakers.

Be straightforward about what desktop checks cannot show: timing, memory
limits, SD read speed, video-mode behaviour and anything else specific to
the hardware. Say which of your claims were verified here and which are
still untested on the device.

## The tools

Build with `make -C src/undertale undertale_pc` (this never touches the
SD-card tree). Then, from this skill's directory:

```bash
scripts/ut-frames.sh -s assets/flowey.script 1700 2400      # dump frames 1700 and 2400
scripts/ut-frames.sh -s assets/flowey.script -e UT_SMOOTH=1 -p s 2400
python3 scripts/ppmtool.py cmp before2400.ppm f2400.ppm     # "identical" or what differs
python3 scripts/ppmtool.py png f2400.ppm battle.png 2       # PNG, enlarged 2x, to look at
python3 scripts/ppmtool.py crop f2400.ppm text.png 380 120 240 120 3
```

`ut-frames.sh` runs each frame from a clean save state with seed 7 and no
frame pacing, in `$UT_RUN_DIR` (default `/tmp/undertale-pocket-run`), and
leaves the last run's log in `<prefix>last.log`. Use the Read tool on a PNG
to look at it.

Environment variables the desktop build understands (all are ignored on the
device):

| Variable | Effect |
|---|---|
| `UT_DUMP_FRAME=n`, `UT_DUMP_PATH=f.ppm` | write frame n and exit |
| `UT_SCRIPT="30:Z,700:R*400"` | press keys on given frames; `*N` holds for N frames. Keys: U D L R, Z X C, E (Enter) |
| `UT_SEED=n` | fix the game's random seed |
| `UT_UNCAPPED=1` | no frame pacing (runs as fast as the display flip allows) |
| `UT_SMOOTH=1` | render 640x480 rooms at 320x240 with 2x2 averaging |
| `UT_OVERLAY=1` | turn on the frame-time and log overlays |
| `UT_AUDIO_DUMP=f.raw` | write the mixed output, 48 kHz stereo s16le |
| `UT_AUDIO_LOG=1` | log every sound effect as it starts |

`./undertale_pc --bench` runs the built-in benchmark; desktop numbers are
meaningless for speed, but it exercises the whole scripted path and the
final report screen.

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

## The standard check for a change

1. Before the change, dump reference frames covering the code you are about
   to touch (at least one 320x240 scene and the 640x480 battle; add the
   fade at 430 for blending work, the menu at 930 for texture-cache work).
2. Make the change, rebuild, dump the same frames with a different prefix.
3. `ppmtool.py cmp` each pair. An optimisation should report `identical`.
   For an intended visual change, look at the PNG and describe what changed.
4. Report exactly which frames you compared.

For audio, compare a new `UT_AUDIO_DUMP` against an earlier one. Captures
can start a few dozen samples apart, so search for the shift that makes
them match rather than expecting byte equality. The user's ears are the
real test; save a WAV into `undertale-pocket/screenshots/` and send it.

## Traps that have cost time

- **Saves leak between runs.** The game writes `undertale.ini` once you
  name the character, and Flowey's dialogue changes on a later run. Two
  renders that "should" match will differ for that reason alone.
  `ut-frames.sh` deletes the save before each frame unless given `-k`.
- **Unseeded runs are not repeatable** on screens with random motion (the
  shaking name on the confirm screen). Always pass a seed when comparing.
- **Rooms change size.** Battle and menu rooms are 640x480, overworld rooms
  320x240; a montage or crop that assumes one size will be garbage.
- **Uncapped audio dumps are time-compressed.** With `UT_UNCAPPED=1` game
  frames run faster than real time while audio advances by real elapsed
  time, so events bunch up. For a listenable capture, run without it.
- **The desktop hides memory pressure.** malloc never fails there and the
  texture cache never has to shrink, so cache behaviour on the device can
  only be inferred from the log lines ("Loaded TXTR page", "Unloaded").
- **Desktop mixer stubs.** The SDK's PC shim implements `of_audio_write`
  but stubs the hardware mixer (`of_mixer_*`), which is one reason the port
  mixes in software.
