# Desktop capture tools

Run the `make` command from the checkout root. Run the `scripts/` examples
from the skill directory (`.agents/skills/butterscotch-pocket-verify`), not its
`references/` subdirectory. Invoke desktop binaries with the game-data directory
as the working directory, as described below.

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

From `src/openfpga/games/undertale/`, `../../undertale_pc --bench` runs the
built-in benchmark; desktop numbers are
meaningless for speed, but it exercises the whole scripted path and the
final report screen.
