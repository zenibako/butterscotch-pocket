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

For Memories, sleep/wake, cutscene captures, or an attached device log, read
[Save-state verification](references/save-states.md). It covers fresh-process
comparisons, scene-policy checks, repeat wakes, and matching saved payloads to
received payloads. Ordinary screenshot comparisons do not exercise restoration.

For native-script, string, audio-tail or build-cache optimizations, also read
[optimization checks](references/optimization-checks.md). It maps the targeted
fixtures and distinguishes deterministic output evidence from desktop timing.

Desktop binaries open `data.win` in the current directory, so run them from
`src/openfpga/games/<game>/` (`scripts/bs-frames.sh -g <game>` does this for you).
Build with `make -C src/openfpga undertale_pc` (this never touches the
SD-card tree). Then, from this skill's directory:

```bash
scripts/bs-frames.sh -s assets/flowey.script 1700 2400      # dump frames 1700 and 2400
scripts/bs-frames.sh -s assets/flowey.script -e BS_SMOOTH=1 -p s 2400
python3 scripts/ppmtool.py cmp before2400.ppm f2400.ppm     # "identical" or what differs
python3 scripts/ppmtool.py png f2400.ppm battle.png 2       # PNG, enlarged 2x, to look at
python3 scripts/ppmtool.py crop f2400.ppm text.png 380 120 240 120 3
```

`bs-frames.sh` runs each frame from a clean save state with seed 7 and no
frame pacing, in `$BS_RUN_DIR` (default `/tmp/butterscotch-pocket-run-<game>`), and
leaves the last run's log in `<prefix>last.log`. Use the Read tool on a PNG
to look at it.

Environment variables the desktop build understands (all are ignored on the
device):

| Variable | Effect |
|---|---|
| `BS_DUMP_FRAME=n`, `BS_DUMP_PATH=f.ppm` | write frame n and exit |
| `BS_PAD="30:A,700:R*400"` | physical Pocket gameplay buttons through the GameMaker gamepad API; U D L R, A B X Y, E (Start); `*N` holds N frames. Use this for controller behavior; keyboard scripts/recordings exercise a different input path |
| `BS_SCRIPT="30:Z,700:R*400"` | press keys on given frames; `*N` holds for N frames. Keys: U D L R, Z X C, E (Enter) |
| `BS_SEED=n` | fix the game's random seed |
| `BS_UNCAPPED=1` | no frame pacing (runs as fast as the display flip allows) |
| `BS_SMOOTH=1` | speed mode, as L sets it: 640x480 rooms at 320x240 with 2x2 averaging, faint mirrored layers left out. (Frame skipping needs frame pacing, so it never happens with `BS_UNCAPPED`.) |
| `BS_SKIP=n` | draw only every nth frame, whatever the mode. A frame captured this way must be identical to one from a run without it: a difference means something drawn on a skipped frame mattered to a later one |
| `BS_MIRROR_FAINT=a`, `BS_NO_MIRROR=1` | fix the faint-layer threshold (of 256) whatever the mode; draw mirrored layers the ordinary way |
| `BS_OVERLAY=1` | turn on the frame-time and log overlays |
| `BS_DEBUG=1` | debug mode, as the menu's "Enable debug buttons" sets it on the device: the Accuracy/Speed mark in 640x480 rooms, and the runner's debug hotkeys. Script them by key code: `!` next room, `"` previous room, `w` pause, `O` step, `y` clear `global.interact` |
| `BS_AUDIO_DUMP=f.raw` | write the mixed output, 48 kHz stereo s16le |
| `BS_AUDIO_LOG=1` | log every sound effect as it starts |
| `BS_DUMP_STATE=n` | print every instance and its variables at frame n |
| `BS_DISASM=name` | print that code entry's bytecode at start (`*` for all, about a million lines) |
| `BS_DUMP_EVERY=n`, `BS_DUMP_DIR=d` | also write every nth frame to `d/f<frame>.ppm` on the way to `BS_DUMP_FRAME` |
| `BS_NOFLIP=1` | never present to the window; without it a run is capped at the display's 60 fps even with `BS_UNCAPPED` |
| `BS_RECORD=f.json` | write every key press and release, by frame, when the run ends (Butterscotch's `--record-inputs`) |
| `BS_PLAYBACK=f.json` | replay a recording (`--playback-inputs`). With `BS_RECORD` as well it replays, then records what follows, so a recording can be extended |
| `BS_PROFILE=n` | log the heaviest game scripts every n frames; the report the menu's "Show script times in log" gives on the device (see the perf skill) |
| `BS_MENU="frame:buttons"` | open the Select menu on that frame and press the buttons in it, one a frame (U D L R, A B, S for Select). With `BS_DUMP_PATH` the menu as they leave it is written there and the run ends; without, the game goes on once the menu is closed (end with S; B closes from the first page and goes back from the Debug page). The first page is Resume, Performance Mode, Debug: `DDA` opens Debug with Save log to SD card chosen; `DDADD` selects Audio mode, and each `R` there moves it on through Normal, Disabled, Music only, Sound only |
| `BS_EXIT_FRAME=n` | leave the main loop at frame n, the ordinary way out (a frame dump exits on the spot) |
| `BS_NO_NATIVE=1` | run every game script in the interpreter, leaving out the native stand-ins of `platform/of_native.c`; frames must match with and without |
| `BS_CODEHASH=name` | print a code entry's length and bytecode hash, for a row of the table in `of_native.c` |
| `BS_GOTO="frame:room"` | jump to a room index on that frame; the game's own state is left as it was |
| `BS_SET="frame:name=1,arr[2]=3"` | set numeric globals (or elements of existing global arrays) on that frame |

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
