# Verifying Memories, cutscenes, and sleep/wake

Use `src/openfpga/README.md` for current supported scenes and format revision;
inspect `runner_snapshot.c` and `platform/game_<game>.c` when that changes.
Game-specific rules stay behind `of_game.h`. A player-control/cutscene marker
alone is not evidence of an unserializable resource.

## Desktop evidence: distinguish capture from policy

Build the desktop game and use an isolated working directory with game-asset
links and disposable progress copies. Keep supplied saves/attachments intact.
Fix the seed and input script for all compared runs.

```bash
make -C src/openfpga GAME=deltarune deltarune_pc
```

From `src/openfpga`:

```bash
tools/state-coverage.sh -g deltarune -b
tools/state-coverage.sh -g deltarune -n 1 -x 2600 -s /path/to/route.script
tools/state-coverage.sh -g deltarune -x 850 -s /path/to/route.script 600:800
make SDK_ROOT=/absolute/path/to/staged-sdk snapshot-tests
make lint
```

The coverage table separates scene policy from serializer capture and lists
`Runner_snapshotBlockers` by room. Its `save:compare` pairs currently set
`BS_STATE_PROBE_BYPASS=1`: they prove serializer restoration, **not** that
production policy permits the scene. For production acceptance, repeat with
the desktop save/load variables below and bypass **unset**, and check policy
and capture both succeed. Inspect the tool if this behavior changes.

For each pair, run three separate processes:

1. Run uninterrupted to frame `compare`, dumping its reference image.
2. Start fresh; capture at frame `save` using `BS_STATE_SAVE_FRAME` and
   `BS_STATE_SAVE_PATH`; exit after capture. Require a nonempty payload.
3. Start fresh; restore that payload with `BS_STATE_LOAD_FRAME=40` and
   `BS_STATE_LOAD_PATH`; dump frame `compare`. Require `Save state load: ok`
   before comparing pixels. The runner restores its frame count to the saved
   frame; comparison must follow both capture and the initial load boundary.

Use `BS_SEED=7`, the same `BS_SCRIPT`, and `BS_DUMP_FRAME`/`BS_DUMP_PATH`.
Input recordings are unsuitable for restore pairs: the playback cursor is
not snapshotted. Use frame scripts. To prove successive restores, recapture
after the first restore, restore that new blob in another fresh process, and
compare both results with the uninterrupted reference. Two independent
single-restore tests do not prove a consecutive restore chain.

Probe around transitions as well as waiting screens: opening/closing dialogue,
advancing a line, Draw/GUI object destruction, room changes, and battle turns.
In the tested Chapter 1 Toriel route, frame 600 caught a dialogue-advance
failure that waiting at checkpoint B did not exercise.

### Reproduce the Chapter 1 dialogue-advance pair

The [Toriel input script](../assets/deltarune-cutscene.script) presses Start
at frame 200 and advances dialogue from frame 600. It requires
`--bench-sleep`, seed 7, and the Chapter 1 data supported by the benchmark;
it starts with volatile progress, not a supplied save. Coverage's `-b` selects
the different performance benchmark. For this route use the direct binary.

Set `CHECKOUT` to the checkout's absolute path. From a disposable working
directory with links to the Chapter 1 game assets, run these as separate
processes. Each invocation allocates new output paths, so a refused capture
cannot reuse an older `state.bin` or screenshot:

```bash
(
set -eu
pair_out=$(mktemp -d "$PWD/restore-pair.XXXXXX")
route=$(cat "$CHECKOUT/.claude/skills/butterscotch-pocket-verify/assets/deltarune-cutscene.script")
run_pair() {
  env -u BS_STATE_PROBE_BYPASS BS_SEED=7 BS_UNCAPPED=1 BS_NOFLIP=1 \
    BS_SCRIPT="$route" "$@" "$CHECKOUT/src/openfpga/deltarune_pc" --bench-sleep
}
run_pair BS_DUMP_FRAME=800 BS_DUMP_PATH="$pair_out/ref.ppm" > "$pair_out/ref.log" 2>&1
run_pair BS_STATE_SAVE_FRAME=600 BS_STATE_SAVE_PATH="$pair_out/state.bin" \
  BS_EXIT_FRAME=602 > "$pair_out/capture.log" 2>&1
rg 'Save state save: ok' "$pair_out/capture.log"
test -s "$pair_out/state.bin"
run_pair BS_STATE_LOAD_FRAME=40 BS_STATE_LOAD_PATH="$pair_out/state.bin" \
  BS_DUMP_FRAME=800 BS_DUMP_PATH="$pair_out/restored.ppm" > "$pair_out/restore.log" 2>&1
rg 'Save state load: ok' "$pair_out/restore.log"
cmp "$pair_out/ref.ppm" "$pair_out/restored.ppm"
)
```

Stop on any failed command. On macOS set
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` if SDL cannot find its runtime.

## Snapshot lifecycle regressions

Draw/GUI runs after Step's destroyed-instance cleanup. A dialogue instance
destroyed there may still be in the runner's ordered lists at capture. A valid
pending instance is inactive and has already fired Cleanup. Preserve its
identity, values, shared-array ownership, flags and index membership until
ordinary cleanup; eagerly freeing or omitting it changes restored behavior.
Test alive–dead–alive ordering, indexes, surviving shared references, normal
next-Step cleanup, and rejection of impossible lifecycle flags.

For serializer changes, also test prior readable revisions and wrong
game/chapter hashes. Bump the format only for a wire change. Revision 8 already
carries the instance lifecycle flags, so the pending-instance fix required no
new layout; revisions 3–8 remained loadable.

Inventory mutable parsed data as well as runtime objects: tilemap setters
modify loaded rooms' `tileData`. Test mutated tile cells after a fresh-process
restore, not just an unchanged room. Structs and arrays need identity and
ordering preservation; methods, VM static-block flags, and runtime layers
also need coverage when the game creates them. An "unused" vertex buffer
with a partial attribute mask or pending format-release pointer is not empty.

## Physical Pocket checklist

Use the matching runtime/card from the build skill. **Deltarune Sleep Test**
(`--bench-sleep`) is interactive and uses volatile progress storage:

1. At A, outside the cutscene, close Select and sleep/wake. Confirm A resumes.
2. Press Start to enter the real Toriel yard cutscene at B. Check Select →
   Debug → Save-state status permits capture, then close Select.
3. Sleep/wake at B twice consecutively. Each wake should return to the same
   dialogue rather than A. Record the capture and received payload IDs.
4. Advance dialogue after waking; continue through the car ride and into
   school. Check controls and audio by observation/listening, not log alone.
5. Separately test a scene the current build genuinely refuses: save/sleep
   somewhere allowed, wake, move to that refused scene, and sleep again.
   A refusal does not prevent power-off. Record where wake lands and whether
   its received ID matches the previous allowed payload. B is supported by
   the cutscene-enabled build, so it is no longer a refusal test.

The existing runtime exposes one sleep/Memories flag per core, not per scene.
Check resource refusals and application notices without claiming they can
veto physical sleep. Different-game and changed-data Memories should fail
clearly without replacing the live game. Notices should use the game/menu
font and remain readable at both game resolutions.

## Read an attached device log as evidence

Select → Debug → Save log to SD card uses the spare `.sav` slot, so the extension alone does not
identify a progress archive. Inspect bytes before decoding; retain the
attachment unchanged. Text logs can contain NUL padding, benchmark history,
previous-boot tails, and current-boot output. Split text at the first NUL for
inspection, retain boot order, and locate the relevant checkpoint/request.

Match `State save prepared: ok` size/ID to `State load receive` and require
the subsequent `State result: load ok`. `State request` alone proves only an
attempt. A zero-byte refusal produced no new restorable payload. A wake that
receives the older A ID is fallback to A, not proof of a cold restart.

Report evidence in this order: checkpoint → capture ID/size → received
ID/size → successful result → later observed landmarks. Scope the claim:

- One B capture/load in each of two separate runs proves two independent
  B restores, not two consecutive wakes at B in one run.
- Room changes into school after the B load establish later progression.
  A music-start line establishes a request to play music, not audible quality.
- Raw whole-game `BS_AUDIO_DUMP` output advances by wall-clock delta. Uncapped
  frame runs do not produce a fixed number of audio samples per game frame.
  Do not claim PCM identity from pixel matches or mismatched-length dumps;
  use deterministic decoder/queue/mixer tests and device listening separately.

For a Continue crash, test the supplied progress slots independently of
Memories. Deltarune's Continue calls `audio_group_set_gain`; verify the Pocket
vtable and mixer, not only desktop generic audio. Existing regression tests
cover group isolation, fade/pause behavior, and a snapshot mid-fade.
