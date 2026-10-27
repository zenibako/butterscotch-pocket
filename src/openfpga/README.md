# Butterscotch Pocket: Undertale and Deltarune on openfpgaOS (Analogue Pocket / MiSTer)

<p align="center"><img src="docs/banner.png" alt="Butterscotch Pocket menu banner: bold white pixel lettering with a gray heart inside the R of BUTTERSCOTCH, above POCKET" width="521"></p>

<!--
Introduction: one short paragraph, then the game-data notice.
- What is this, in one sentence? (Whose runner is it a port of, and to what device?)
- Which game and version does it run today?
- What is this repository in relation to Butterscotch, and where does the port live in it?
  (A fork, whose `main` carries the port since 2026-10-10; src/openfpga/ is the port and src/openfpga/dist/ the core definition.
  The openfpgaOS SDK is a separate checkout the build reads.)
- Upstream lists out-of-tree ports under "Community Ports" in its README; the two there open with a
  short note about the port and then carry the original README below. This file follows that shape.
- The notice a reader must not miss: no game data is included; what do they have to supply?
- The sentence that stood in the root README: "This is a port of Butterscotch for the Analogue Pocket, targeted at running Undertale v0.8."
  (The paragraph below says v1.08; which is right?)
-->

Butterscotch built as an openfpgaOS app: this directory is the platform
layer and the core around it, which lists Undertale and Deltarune (one
chapter). It uses Butterscotch's software
renderer (draft PR #429, carried on this branch) drawing RGB555 straight
into the openfpgaOS framebuffer, at 320x240 or 640x480 depending on the room.

This file says what is here and how to use it. It carries no measurements:
frame times, load times and sizes change from build to build, and the
benchmark (see On-device diagnostics) reports them for the build in hand.

You must supply your own game data: `data.win` from Undertale v1.08, or a
Deltarune chapter (see Deltarune below).

## Installing your own game data

For a prebuilt core, follow [Add your own game files](GAME-DATA.md). The
Standalone Go preparation tool converts textures and audio and produces the
exact SD-card filenames without rebuilding the core or installing Python.
The guide covers installing both the core and your prepared data through
Pocket Sync, as well as copying the files manually.

For installation, follow [Install](docs/INSTALL.md) and [Prepare your game files](GAME-DATA.md).
The runner, port, nested Go tools and release pipeline share this repository.
Public builds and dependency provenance are described in [Build and release](docs/BUILD.md).

## Layout

| Path | What |
|------|------|
| `main.c` | Entry point: registers `data.win` on data slot 4 and starts the runner |
| `platform/of_platform.c` | Butterscotch platform hooks on the `of_*` API (video, pad, timing) |
| `platform/of_*.c` | The rest of the platform layer; the same for every game |
| `platform/game_undertale.c`, `platform/game_deltarune.c` | What differs per game (prompt patches, native scripts, benchmark route); the build compiles the one `GAME` names (see `platform/of_game.h`) |
| `games/<game>/` | Your own game data (`data.win`, `music/`) and the packs built from it; not tracked |
| `../` | Butterscotch itself; `../sw/` is the software renderer |
| `dist/` | Pocket core definition (core, data slots, instance JSON) |
| `out/` | Everything the build writes; not tracked |
| `tools/mkart.py` | Draws the menu banner and core icon into `dist/` (run by hand after changing the art; needs Pillow). `docs/banner.png` is its preview of the banner, shown at the top of this file |

The openfpgaOS SDK is a dependency, not part of this repository. Clone
https://github.com/openfpgaOS/openfpgaSDK next to this repository, or pass
`SDK_ROOT=<its path>` to `make`. The build only reads it.

## Build

<!--
- Which host have you built on, and what does it need installed?
- Why are GNU sed and USE_SDK_CONTAINER=0 needed here? (One line each; env.sh has the reasons.)
- What does the reader need to supply, and where does each file go?
- Which commit of the openfpgaOS SDK have you built against? (The port was developed on a408ddc.)
- Where should they look for the rest? (src/openfpga/README.md covers controls, packs, saves,
  the benchmark and diagnostics.)
The commands below match the layout as of 2026-10-08; check they still match how you build.
-->

From nothing, with the SDK cloned next to this repository:

```bash
git clone https://github.com/zenibako/butterscotch-pocket.git butterscotch-pocket
git clone https://github.com/openfpgaOS/openfpgaSDK.git
cd butterscotch-pocket/src/openfpga

# Your own game data: data.win (named game.ios inside the macOS app) and
# the folder holding the game's .ogg files.
mkdir -p games/undertale
cp /path/to/data.win games/undertale/data.win
ln -s /path/to/folder-with-ogg-files games/undertale/music

make            # builds the core and its data packs into out/build/pocket/butterscotch/
make copy       # copies it to a mounted Pocket SD card
```

On macOS, put Homebrew's GNU sed first on `PATH` and set
`USE_SDK_CONTAINER=0` first (the SDK's scripts need GNU sed, and this builds
with the host's `riscv64-elf-gcc` instead of the SDK's Docker image).

```bash
export PATH="$(brew --prefix gnu-sed)/libexec/gnubin:$PATH"
export USE_SDK_CONTAINER=0
```

```bash
make            # RISC-V ELF + Pocket SD tree in out/build/pocket/butterscotch/
make test       # desktop binary ./undertale_pc (run it from games/undertale/)
make lint       # strict compiler warnings on this port's own files
make tile-motion-fixture  # builds the asset-free translated tile-cache regression
make builtin-compat-fixture  # builds the asset-free date/audio builtin regression
make gamepad-input-fixture   # checks controller actions, edges, axes and restore/menu rebasing
make button-prompts-fixture  # builds the asset-free prompt, font and icon regression
make deltarune-button-prompts-fixture  # builds generated-label and fallback checks
make string-reads-fixture    # builds the asset-free string read/ownership regression
make collision-work-fixture  # builds asset-free collision order/mutation checks
make collision-precise-fixture GAME=undertale WADS=16 # checks precise pairs against scalar sampling
make collision-precise-fixture GAME=deltarune WADS=17 # checks the Chapter 1 configuration
make font-sampling-fixture   # checks doubled fonts and unchanged fallback filters
make menu-fixture            # checks menu help, scrolling and navigation without game data
make battle-contrast-fixture # checks shared speech, battle labels and buttons
make solid-fill-fixture      # builds exact RGB555 fill/blending and boundary checks
make spatial-grid-fixture    # builds asset-free grid recovery/update checks
make layer-lookup-fixture    # checks layer lookup order, mutation and repeated work
make vm-call-args-fixture    # checks argument bounds, ownership and nested calls
make call-later-fixture WADS="16 17" # checks deferred script/method/builtin callback context
make physics-contact-fixture WADS="16 17" # checks collision callback self/other and inheritance
make tile-shift-fixture GAME=deltarune # compares the guarded native with supplied real bytecode
make asan       # desktop binary with ASan + UBSan: ./undertale_pc_asan
                # (GAME=deltarune: ./deltarune_pc, ./deltarune_pc_asan)
make copy       # copy the core to a mounted Pocket SD card
```

The collision fixture is `./<game>_pc_collision_work_fixture`. Build with one
explicit `WADS=16` or `WADS=17`; add `PC_EXTRA_CFLAGS=-DOF_COLLISION_WORK_PROFILE`
to also check sort, comparison, target-length and bounding-box counters. Use a
fresh `PC_OBJ_DIR` when changing flags. These counters are diagnostic counts,
not timings, and are absent from ordinary builds.

The precise-pair fixture is `./<game>_pc_collision_precise_fixture`. Build
`collision-precise-fixture` with one explicit `WADS=16` or `WADS=17` and the
matching `GAME`. It compiles the real collision header directly, without game
assets or platform objects. Literal edge cases and a scalar sampling oracle
cover compatibility sampling, signed/degenerate scales, tiny/negative angles,
frame masks, cropped rows, fractional positions and lazy setup. Fixture-only
function instrumentation and math wrappers bound repeated setup work; they do
not measure time and add no production diagnostics. `--behavior-only` skips
work-bound failures for characterizing an unoptimized baseline. For sanitizers,
use `PC_CC=clang` and
`PC_EXTRA_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize=alignment,function -fno-omit-frame-pointer"`.
On macOS run with `ASAN_OPTIONS=detect_leaks=0` and, when needed,
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`. Use fresh named `ROOT`,
`PC_OBJ_DIR` and `PC_BIN` paths when changing configurations; header checksum
invalidation applies to those selected directories.

The string-read fixture is `./<game>_pc_string_reads_fixture`. It checks UTF-8,
malformed byte groups, embedded NULs, index boundaries, non-string conversions
and owned results through the real builtin registry. A scalar oracle covers
short byte sequences; fixture-only `strlen` accounting verifies that
`string_char_at` no longer traverses the whole input to determine its length.
`--behavior-only` skips allocation/work assertions when comparing an older
implementation. Run both games with separate object directories and the
sanitizer flags above.

The spatial-grid fixture is `./<game>_pc_spatial_grid_fixture`. It checks
ordinary updates, stale/partial membership recovery and grid replacement; use
a fresh `PC_OBJ_DIR` for each game or changed build flags.

The layer lookup fixture is `./<game>_pc_layer_lookup_fixture`. It checks ordered
ID lookup, missing IDs, structural mutations and bounded repeated-hit work using
the actual runner and layer builtins. Use a separate object directory per game.

The VM call fixture is `./<game>_pc_vm_call_args_fixture`. It exercises actual
CALL bytecode and WAD17 CALLV bytecode, stack/heap argument boundaries, value
ownership and nested script/event execution without game assets. Run it once
for each game with a separate object directory.

The callback fixtures are `./<PC_BIN>_call_later_fixture` and
`./<PC_BIN>_physics_contact_fixture` (`PC_BIN` defaults to `undertale_pc`).
Build both with `WADS="16 17"` and run the produced executables. The deferred
fixture checks idle and nested calls, bound-instance fallback, diagnostics
without code entries, and returned-value ownership. The physics fixture checks
collision self/other, target and handler inheritance, and caller restoration.
Both are asset-free and run under ASan/UBSan in CI.

The Forest sentry fixture is `./<game>_pc_forest_sentry_fixture`. Build with
`GAME=deltarune WADS=17 forest-sentry-fixture`, then pass your Chapter 1
`data.win` to the executable. Real bytecode is the differential oracle; no game
data is embedded. It checks active-state builtin order/arguments/results,
numeric value types, bullet state, array aliases/owner tokens, spatial dirty
order, RNG, disabled/mismatched natives, unsupported-state rejection and fresh
VM variable-ID resolution. Instrumentation requires native acceptance and zero
interpreter entries for supported cases. `--behavior-only` characterizes an
unoptimized baseline without enforcing those work bounds. Run with the
sanitizer flags above and also build `GAME=undertale WADS=16` to check exclusion.
Creation and warm-up retain the interpreter; the native handles only bounded,
finite active-state values behind the exact event length/hash guard.

The tile-shift fixture is `./deltarune_pc_tile_shift_fixture`. Pass the path to
your own Chapter 1 `data.win` as its argument. It compares the interpreted
script and the guarded native, including ordered builtin calls, coordinates,
caller state and fallback controls; no game bytecode is included in the test.
Its interpreter-work bound is fixture-only instrumentation. The Undertale
configuration checks that the Deltarune-only script is not installed there.

The translated tile-cache fixture and its recorded harness/Pocket comparisons
are described in [the dated results](https://github.com/zenibako/butterscotch-pocket-legacy/issues/31).

### Save states and sleep (experimental; Undertale and Deltarune Chapter 1)

The save-state build requires a matching os25 runtime with
`INCLUDE_SAVESTATE`. The ordinary SDK runtime does not support it. Build the
os25 variant from our openfpgaOS fork,
[zenibako/openfpgaCore](https://github.com/zenibako/openfpgaCore) (`52abf78bde0446bc4af8e23631f41ff950e5a535`,
which includes the reviewed save-slot `fsync` fixes, or a descendant), then
stage it into a separate SDK. The staging script refuses an OS checkout that does not contain
that fsync-capable revision.

There os25 runs the CPU and RAM at 100 MHz with the save-state controller
(seed 1). That build misses setup timing at the slow 85C corner (WNS
-0.777 ns, about 92.8 MHz) but has been verified on a Pocket. Staging refuses
a build with negative slack unless you pass `--allow-timing-miss`, and prints
the worst slack either way:

```bash
tools/stage-savestate-sdk.sh --allow-timing-miss /path/to/built-openfpgaCore /path/to/base-SDK /path/to/new-SDK
make SDK_ROOT=/path/to/new-SDK save-states
```

Staging requires committed firmware/build inputs and a built Pocket `os.bin`
no older than its tracked Pocket compile/link inputs (tests and docs are
excluded). It verifies that SDK export copied that exact OS, then records SHA-256 hashes for `os.bin`, `os25.rbf_r`, and `loader.bin` in
`SAVESTATES_RUNTIME.json`. Save-state packaging validates this marker and all
three files before image assembly; legacy text markers require restaging.
Native card assembly requires `VARIANT=os25`. This is a local provenance and corruption guard, not a signed build attestation.

After an OS source change, rebuild its firmware and patch the matching bootloader into
the bitstream before staging. The output is
`out/native-saves/build/pocket/butterscotch/`, with an entry for each game
whose data is in place (Deltarune's only when `games/deltarune/data.win` is).
The base SDK is read-only. The standard build still uses its existing SDK.
The fork's runtime prioritizes a pending save over an unfinished asynchronous
read and provides a conservative heap-capacity query. Install the complete
package when updating it: the app, OS, and bitstream must match.
Native container builds use the firmware image built by openfpgaOS; the app
wrapper mounts both the SDK and this repository. Packaging needs Bash 4 or later
on the host (Homebrew's Bash on macOS).

Use the Pocket's Memories controls to save or load, or its power button to
sleep. Exact snapshots are accepted in supported overworld scenes, the
game's own menus, its dialogue, battles, and the intro, title and naming
screens, and cutscenes. A battle state also carries the room the battle was entered from,
which the game keeps as persistent, so finishing the battle returns there.
The port's Select menu, unsupported runtime resources, live dynamic sprites, and a debug-paused runner
produce an on-screen refusal. Select → Debug → Save-state status
checks the current scene's eligibility; the Pocket's controls perform the save
or load. A refusal stays visible until a
fresh game-button press, including after the Pocket menu closes. A snapshot that exceeds the
Pocket's blob limit is also refused. Ordinary game progress saves remain
separate; loading a save state rewinds its captured progress files.

Both games run under the one core, and the Pocket's sleep and Memories
switch (`sleep_supported` in `core.json`) belongs to the core, so each game's
program answers every save and load request. Both games accept cutscenes
when the runner can preserve their live resources; the player-control marker
(`obj_mainchara.cutscene`) does not itself prevent a snapshot. A state is bound
to the game and the exact `data.win` it was taken with. The Pocket lists
every game's Memories together, so loading one into the other game, or after
the game's data has changed, is refused with "This Memory is from another
game or version".

Memories from older builds that omitted the whole-file fingerprint are also
readable when their original metadata-and-code identity matches the loaded
game. Those legacy states cannot verify changes elsewhere in `data.win`, so
use the original game data. Loading one keeps all format, checksum, and resource
validation; subsequent captures use the current whole-file fingerprint.

The Pocket can still power off after a refused save. In the hardware test, wake
then replayed an older sleep payload. A refusal therefore means the current
position will not be restored; check eligibility before sleeping.

Routine saves and loads use the Pocket's normal interface; the app shows
refusals and failures without extra stage overlays. For troubleshooting,
Select → Debug → Save log to SD card writes the log to the spare save slot; leave through the
Analogue menu to keep it. The log records capture sizes, elapsed time,
transfer return codes, core capture phases, and the Pocket's read-word count.
Optional streamed-music refills are deferred while the Pocket menu or a state
request is active, preserving buffered audio until normal reads can resume.
Core format revision 10 preserves renderer depth and texture-coordinate settings
without saving renderer pointers. It also saves the VM's configurable comparison tolerance, uses
lossless Zstandard compression, references strings
already present in the matching game data, carries persistent rooms' saved
state, and holds GameMaker 2.3 values (methods, structs and GMS 2 arrays), the
VM's static-block flags, the current room's runtime layers and every loaded
room's legacy tiles and tilemap cells; states from revisions 3 through 9 remain
readable, including revision-9 states from before and after the upstream renderer
update. Revisions 3 through 8 restore the original default comparison tolerance.
New revision-10 states require an updated build. Vertex formats
and empty vertex buffers a game makes at start are not
saved: a load keeps the game's own and is refused if they differ from the state's.
Native builds avoid the renderer's large allocation probe
when the OS can prove the reserve fits, otherwise retaining the original probe.
The first reserve check logs the allocation and query timings for comparison.
State-request logs include refusal reasons and separate load receive,
preparation, application and cleanup times. Successful loads notify the Pocket
after committing restored state, before freeing the state they replace.
Native builds automatically write the log to the existing spare log slot before
publishing a captured or refused save. This records sleep
capture attempts before power-down; a failed diagnostic write does not change
save completion. `State service` records the live CPU clock, registration and
request registers, and `State save audit` reports the diagnostic write time.
Capture and receive logs include the envelope checksum as an ID to match blobs
across repeated sleep/wake cycles.
`State apply phases` separates progress-archive writes, runner replacement,
audio restoration and input/clock reset.
`State prepare phases` separates decode, rooms, values, indexes and temporary
buffer cleanup. Individual `State room payload` lines time asset reads for
missing room definitions.
Save-state builds retain the raw ROOM chunk already read at startup, so lazy
room reloads use memory during restoration. Room parsing and unloading remain
lazy. The cache uses additional heap space and is released with the game data.

The runtime fault tests use a checkout of the fork:

```bash
OPENFPGAOS_SOURCE=/path/to/openfpgaCore python3 tests/test_async_save_abort.py
OPENFPGAOS_SOURCE=/path/to/openfpgaCore python3 tests/test_heap_reserve.py
```

Desktop verification uses `BS_STATE_SAVE_FRAME=<n>` with
`BS_STATE_SAVE_PATH=<file>`, and `BS_STATE_LOAD_FRAME=<n>` with
`BS_STATE_LOAD_PATH=<file>`. These run the same serializer at completed frame
boundaries. `make snapshot-tests` checks the runner, audio, archive and envelope
code with sanitizers, including the desktop SDK's audio queue, native close
failure handling, notice bounds, and request completion. It also checks
container-supplied game fingerprints. Native hardware
behavior still requires Pocket tests.

To see where a state can be taken, `BS_STATE_PROBE=<n>` makes the desktop build
try every n frames and print a tab-separated `State probe` line on stderr:
frame, room index and name, what the scene policy says, whether a capture
works with the policy set aside, its size, and every refusal the serializer
has (`Runner_snapshotBlockers`), not only the first. It looks behind the
Deltarune gate too, without lifting it. `BS_STATE_PROBE_BYPASS=1` lets
`BS_STATE_SAVE_FRAME` save in a scene the policy refuses, so that a restore
there can be compared. Neither exists in the device build.
`tools/state-coverage.sh` runs a route with the probe and prints the table;
given `save:compare` frame pairs it also saves, restores in a fresh process
and checks the later frame is identical to an uninterrupted run:

```bash
tools/state-coverage.sh -g undertale -s route.script -x 2700 1500:1700 900:1100
tools/state-coverage.sh -g deltarune -b          # the benchmark's route
```

Each comparison frame must follow both the saved frame and the fresh process's
restore at frame 40. The tool requires a successful load before comparing pixels.
`-p recording.json` accepts relative paths and can probe a recorded route, but
cannot be combined with frame pairs: snapshots do not yet preserve the recorded
input playback cursor. Use `-s route.script` for repeatable restore comparisons.

Put your `data.win` in `games/undertale/` (on macOS it is `game.ios` inside
`UNDERTALE.app/Contents/Resources/`, renamed), and link the folder with the
game's `.ogg` files there as `music/`. (Until 2026-10-13 they went in this
directory; the build stops and says so if it finds them here.) The build copies it to
`Assets/butterscotch/common/` in the SD tree.

Desktop extras: `BS_DUMP_FRAME=<n> BS_DUMP_PATH=out.ppm ../../undertale_pc` (from `games/undertale/`)
writes frame `n` of the 320x240 output and exits. `BS_UNCAPPED=1` disables
frame pacing for timing runs. `BS_SEED=<n>` fixes the game's RNG and
`BS_SCRIPT="300:Z,340:D"` presses keys on given frames (U D L R, Z X C,
E = Enter), which together make runs repeatable for pixel comparisons.
`BS_PAD="300:A,340:D,400:X*5"` uses the same frame/hold notation for physical
Pocket gameplay input through the GameMaker gamepad API (U D L R, A B X Y,
E = Start). `BS_SCRIPT` and keyboard recordings still exercise keyboard input;
use `BS_PAD` to verify controller behavior. `make test WADS="14 16"`
enables older bytecode versions for testing other games. These settings were
named `UT_*` until 2026-10-13; the old names still work and the log says so.

`make profiler-fixture WADS="16 17"` builds the asset-free
`./<PC_BIN>_profiler_fixture` check (CI names it `bs_pc`) for Script times. It exercises nested
script/builtin counts, native success/fallback accounting, report/reset, and
off/on/off toggles between executions. Pass the same `PC_EXTRA_CFLAGS` sanitizer
flags as an ASan build to check memory safety too.

`make sprite-clipping-fixture WADS="16 17"` builds the asset-free
`./<PC_BIN>_sprite_clipping_fixture`. It compares clipped sprites against
crops of unclipped draws, including horizontal/vertical flips, scaling,
transparency and an offset viewport. Pass sanitizer flags in `PC_EXTRA_CFLAGS`
to check texture-read bounds as well.

`make solid-fill-fixture WADS="16 17"` builds the asset-free
`./<PC_BIN>_solid_fill_fixture`. Run it to compare constant translucent fills
against the general pixel compositor, including every 16-bit destination,
partial opacity, clipped rows, odd pitches and repeated pixel pairs. Passing
`--negative` deliberately corrupts a pixel and must exit with failure.

## Deltarune (experimental)

The core lists two games on the Pocket, Undertale and Deltarune. They are
the same code built twice (`GAME=deltarune` enables the newer bytecode
version and compiles `platform/game_deltarune.c` in place of
`platform/game_undertale.c`), and the SD tree is assembled from every game built so far:

```bash
tools/deltarune-setup.sh "<...>/DELTARUNE.app/Contents/Resources" 1   # links your files into games/deltarune/
make && make GAME=deltarune         # both programs; out/build/pocket/butterscotch/ holds both games
make GAME=deltarune deltarune_pc    # desktop binary
```

One chapter at a time. Each game has its own program, data, object files
and save file; on the card they share `Assets/butterscotch/common/`, where
Deltarune's files are `deltarune.win`, `dr_textures.bin` and
`dr_music.bin`. The program still opens `data.win`, `textures.bin` and
`music.bin`: those names are bound to data slots 4 to 6, and the game's
entry (`dist/Assets/butterscotch/zenibako.Butterscotch/Deltarune.json`)
decides which files the Pocket puts in the slots. `tools/mkcard.sh` does
the assembly and leaves out the entry of a game that is not built.

Two things differ from Undertale. The music folder is shared by all
chapters, so the setup script links only the `.ogg` files the chapter's
data file names. And the game reads its text from `lang_en.json`, which
has no data slot to live in, so it is stored in `music.bin` and the file
layer reads it from there (`ofAudioReadPackFile`).

`--bench` works for Deltarune too, and `make compare` adds its benchmark
entries to the card (`tools/mkcompare.sh` names them; the route they play is
in `platform/of_bench.c`).

**Deltarune Sleep Test** is an interactive benchmark for Chapter 1, started
with `--bench-sleep` (its own entry on save-state cards). It keeps progress in memory,
leaving the game's save archive alone. Checkpoint A waits in the yard with
saving allowed: sleep, wake, then press Start. The yard reloads into Toriel's
real cutscene and waits at its dialogue. At checkpoint B, Select → Debug → Save-state status
should allow capture. Close Select, sleep again, and check that wake returns
to B in the same dialogue. Repeat sleep/wake at B, then advance the dialogue
to check that the car ride continues. The benchmark keeps running so
Memories and sleep requests can be serviced at both checkpoints.

What the software renderer does for Deltarune's rooms and screens (tile
layers, stacks of mirrored translucent layers, the battle background) is
described where it is done, in the comments of `../sw/sw_renderer.c` and
`../sw/sw_drawing.c`.

## Controls

The built-in controls are exposed as one GameMaker gamepad, including the
legacy joystick API used by Undertale. Instruction glyphs mark physical
Pocket button positions; Deltarune's generated prompts follow the active
controller bindings. Select and the shoulders remain port controls.

The right face button confirms, the bottom cancels, and either the top or
left opens the game's menu. The D-pad moves. The right small button below
the screen (Start) also confirms; the left (Select) opens the port's menu
(below). The left shoulder toggles speed/accuracy and shows the new setting in
the top right corner for two seconds. The core starts in speed (the desktop
build in accuracy, since frames are compared against what the game asks
for). Accuracy is the target: when an optimisation brings accuracy up to
speed's frame rate somewhere, the shortcut speed takes there is removed, so
the two differ only where they still have to. Accuracy draws everything the
game asks for. Speed gives up these for time: 640x480 rooms are drawn at
320x240 and smoothed (see Resolution), blend layers too faint to change a
16-bit colour by more than one step are left out, and frames are skipped
when the game is running behind (see Frame skipping).

### The menu

Select opens the port's menu over the paused game, drawn in the game's own
font (the first of `fnt_main`, `fnt_maintext` and `fnt_small` the game has,
copied out of its texture page the first time; see `platform/of_font.c`).
Up/Down choose a row and A or Left/Right change it. The debug settings are
on a page of their own: A on "Debug" opens it and B goes back. B on the
first page, or Select or Start on either, closes the menu. A two-line area
below the rows explains the selected item, with text wrapped to the menu width. The sound stops
while it is open. Everything it sets lasts until
the core is left. It is not in the Pocket's own core menu because the
hardware gives a core no menu variables of its own.

| Row | What it does |
|---|---|
| Resume | closes the menu |
| Performance Mode | Speed or Accuracy, as L sets it |
| Debug | opens the rows below |
| Save log to SD card | first row in Debug; writes the log to the spare save slot; leave through the Analogue menu to keep it |
| Save-state status | shows whether the current scene is eligible, and displays a refusal reason when selected |
| Audio mode | Normal, Disabled, Music only or Sound only, to see what sound costs. Music is what the game streams or keeps in a file of its own, and sound is the effects embedded in its data, however long either is. What is muted is not decoded, mixed or read from the card, and still runs its course so that the game is not left waiting for it. Something that was playing unheard is rough until it next starts. `--mute` in `ARGS=` starts with Disabled |
| Show frame times | four numbers in the top left corner over the last 30 frames. The first three are milliseconds (average work, worst work, worst frame period; 33 means full speed); the fourth is how many of the 30 frames were skipped, which is always 0 in accuracy mode. The menu footer describes the selected item; frame times appear only when this overlay is on |
| Show log overlay | the last log lines over the game. The log is kept whether or not this is on, so it can be switched on after a hitch to read it |
| Show script times in log | every two seconds the log gets the twenty game scripts and built-in functions that took the most time, as milliseconds, VM instructions and calls per frame (Butterscotch's GML profiler). The log comes up with it. Measuring slows the game a little while it is on |
| Enable debug buttons | the button chords below (debug mode) |
| Next room, Previous room | goes there as the menu closes |
| Clear interact | sets `global.interact` to 0, for when a cutscene has left the player stuck |

With the debug buttons on (or after starting with `--debug` in the OS
config's `ARGS=`, or `BS_DEBUG=1` on desktop):

- R toggles the log.
- "Accuracy" or "Speed" stays in the top right corner, the way L's toggle
  is set.
- Butterscotch's own debug hotkeys are reached with Select held, since the
  Pocket has no keyboard: Select + Right/Left goes to the next/previous
  room, Select + Start pauses, Select + A steps one frame while paused,
  Select + B clears `global.interact`, Select + X turns script times on or
  off, and Select + Y saves the log. A Select used for one of these does not
  open the menu. The state dumps (F11, F12) are not mapped: they print far
  more than the device can show.
- Select + Down turns sound off or on ("Sound off", "Sound on"): the
  menu's audio mode set to Disabled, or back to Normal from any other.

Anything that would otherwise leave the screen unchanged says what it did
in the top right corner for two seconds, in the game's font: "Next room",
"Previous room", "interact = 0", "Sound off" and "on", "Script times on" and
"off", "Log saved", and "Resumed". A paused game shows "Paused, frame N"
for as long as it is paused, and each step advances N.

## Resolution

Undertale's window is 640x480. Overworld rooms show a 320x240 view scaled
2x, so they are rendered at 320x240. Battles and menus use the full 640x480
with small fonts, so those rooms are rendered at 640x480 and the display
mode is switched to match (`of_video_set_mode`). If the OS refuses the mode
everything stays at 320x240.

L switches to the alternative: every room at 320x240, with the renderer
averaging each 2x2 block of texels when it shrinks a 640x480 screen. Fonts
verified to contain doubled pixels use sharp sampling at exact half size,
keeping dialogue and menu text crisp. Smaller fonts retain averaging so thin
strokes remain readable. Those screens cost about a quarter of the pixels.

`make font-sampling-fixture` builds `./<game>_pc_font_sampling_fixture` without
game assets. It checks either pixel phase, clipping, tint, alpha, cache reuse
and texture reloads, plus unchanged filtering for other fonts, odd glyph
sizes, alternate scales, mirrored text and rotation.

Speed mode also adjusts selected battle names, buttons and enemy speech for
readability at half resolution. Both games use the shared sampler in
`../sw/sw_drawing.c`; `game_undertale.c` and `game_deltarune.c` select the
assets and active battle context. Accuracy and non-battle screens keep their
ordinary sampling.

`make battle-contrast-fixture` builds `./<game>_pc_battle_contrast_fixture`
without game assets. Run it once for each `GAME` to check game-specific
selection as well as shared coverage, clipping, cache isolation and fallback
modes. It also checks exact button-cache output, warm renderer-state restoration,
cold rebuilding and page eviction. `make snapshot-tests` includes allocation,
budget and texture-ownership checks. Use separate object directories when
changing `GAME` or compiler flags.

## Frame skipping

In accuracy mode a scene that cannot be drawn in a frame's time (33 ms)
runs the whole game slow. In speed mode the time lost to slow frames is
kept as a debt; when it reaches half a frame, one frame is run without
drawing to the screen and the time that saves pays the debt, so the game
keeps its pace and loses smoothness instead. The game's Draw events still
run on a skipped frame, because Undertale keeps logic in them; only the
pixel work is left out, and drawing to surfaces still happens. No two
frames in a row are skipped, and the debt is capped at one frame so a long
stall (a room load) is not chased with a burst of fast frames. The
benchmark never skips.

## Texture pack

`make` runs `tools/mktexpack` on your `data.win` to produce `textures.bin`
(data slot 5): every texture page already converted to the renderer's
16-bit format, cut into 128x128 tiles and run-length encoded tile by tile.
The renderer then reads only the part of a page each sprite, tileset or
font occupies, and keeps one small texture per item; whole pages would not
fit in the Pocket's memory side by side. Without the pack the renderer falls
back to the PNGs inside `data.win`, a whole page at a time. Textures are
kept in a least-recently-used cache that shrinks whenever less than
`TEXTURE_RESERVE_MB` (default 4) of heap would be left for the game.

The pack format changed when tiles were introduced (`UTX2`); a build
ignores an older `textures.bin`, so copy the new one with the core.

## Sound

Decoding Vorbis live is too heavy for the 100 MHz CPU, so `make` runs
`tools/mkmusic` to produce `music.bin` (data slot 6): every sound as mono
IMA ADPCM at `MUSIC_RATE` (32 kHz unless set otherwise). It combines the external `.ogg` files the game
streams (from `music/`, a directory or symlink) with the effects embedded in
`data.win`.

`platform/of_audio_system.c` plays them: long tracks stream through a
read-ahead buffer, short ones are loaded whole on first use and cached. It
applies pitch and gain, mixes the voices in software and writes the OS's
output rate.

A short stretch of output is kept queued. It is topped up once per frame
and also from `platformBusyTick`, which the loaders call between read pieces
and the renderer calls while drawing; without that a frame longer than the
queue leaves a gap in the music. The read-ahead is refilled a piece per
frame, and a track that starts is first read on the following frame, so
room changes do not also pay for music start-up. On the Pocket the pieces
are read with `of_file_read_async` and waited for, which is much quicker
there than `fread`; a read is never left in flight, because the OS fails
ordinary reads while one is. The comments in `platform/of_audio_system.c`
have the sizes and the reasons.

Desktop: `BS_AUDIO_DUMP=out.raw` captures the mixed output (48 kHz stereo
s16le) and `BS_AUDIO_LOG=1` logs every effect, for checking without speakers.

## Saves

Undertale keeps its progress in a few small files (`file0`, `file9`,
`undertale.ini`, ...). `platform/of_save_fs.c` stores them all as one small
archive in the first save slot (`undertale_0.sav`, or `deltarune_0.sav`).
Native Memories builds flush ordinary progress saves directly to SD with
`fsync`; they require the runtime with save-slot `fsync` support shipped
alongside the game update. Older runtimes report `SD sync failed` and still
rely on the Pocket's exit writeback. A Memories restore rewinds the captured
progress in save RAM; it does not itself flush to SD. The benchmark neither
reads nor writes saves.

To carry a save over from the desktop game, `make import-save
SAVE_DIR=<folder>` packs a desktop save folder into `undertale_0.sav`
(`SAVE_DIR` defaults to the macOS location, `~/Library/Application
Support/com.tobyfox.undertale`; on Windows it is `%LOCALAPPDATA%\UNDERTALE`,
on Linux `~/.config/UNDERTALE`). Copy that file to
`Saves/butterscotch/common/` on the SD card; it replaces any progress made on
the Pocket. `make export-save EXPORT_DIR=<folder>` goes the other way, and
`tools/mksave list <file>` shows what a slot file holds. The slot file is
deliberately not part of the SD tree, so copying a build never touches saves.

## Button prompts

The game's keyboard prompts use positional button icons: four circles in
a diamond, with the matching face button filled. Confirm highlights the
right button, cancel the bottom, and menu the top (the left also works).
Start/Select use a pair of small buttons, shoulders a pair of shoulder
shapes, and directions a D-pad. These appear in the port menu too.

Strings and font glyphs are changed in memory after loading; see
`platform/of_strings.c` and `platform/of_buttons.c`. Deltarune Chapter 1's
generated input names and Undertale v1.08's graphical attack prompt have
version-checked draw/script replacements. Other game versions keep their
original generated/graphical prompts when the code differs. `data.win`
and the texture packs are not modified.

## On-device diagnostics

For platform shortlisting, actual-artifact profiles and private report collection,
follow [Pocket benchmark integration](docs/BENCHMARKING.md). It distinguishes
software, same-ELF platform and different-ELF configuration comparisons while
preserving the game's correctness and physical Pocket lifecycle checks.

**The log as a file.** A screenshot of the log overlay holds only the last
few lines. The whole log, from start-up on, is also written as text to the
game's second save slot, `undertale_1.sav` or `deltarune_1.sav`: when the
core halts (the end of a benchmark, a fatal error), from the menu's "Save
log", and on Select + Y with the debug buttons on. A core can only write to its save slots, so that is where it
goes; the Pocket copies the slot to `Saves/butterscotch/common/` on the
card when the core is left through the Analogue menu. The text ends at the
first NUL byte. Preserve the original file and use the shared collector's
`inspect` to select the intended complete report; removing NULs can expose a
stale tail.

**The benchmark.** The Benchmark entries (`make compare`) play a fixed
route with scripted input and end by writing a report to the log: time per
frame for each section of the route, then tables of where it went (game
code, drawing by kind of call, sound, presenting) and the draw calls that
cost the most. `platform/of_bench.c` has the route and says what each
table's columns are. Quote numbers from a report together with the commit
it was built from.

The boot log stays on screen while loading, and every Butterscotch log line
is prefixed with seconds since start, so load stages can be timed by eye.
If the app exits or aborts it halts with the log visible instead of
rebooting.

A frame that takes much longer than it should (`OF_PERF_SLOW_FRAME_MS`)
logs three lines, kept short enough to fit the log overlay:

```
slow 555: step 300 draw 200 out 20 snd 10
  load: rm 0 tx 0 fx 0 mix 12 mus 0
  draw: s5/380 p1/9 t12/40 b1/3 r0/0
```

The first splits the frame's work time (ms) into game code, drawing,
presenting (overlays, copy, flip) and the audio update. The second gives
time spent within those on particular jobs: loading the room, texture pages
and sound effects, mixing audio, and reading streamed music. The third
gives calls/ms for each kind of draw call: sprites, sprite parts (tiles),
text, tiled backgrounds and rectangles. Reading the clock is costly on the
Pocket, so mixing and draw calls are only timed while debug mode or an
overlay is on (and on some frames of a benchmark); otherwise those
milliseconds read 0 and the draw line ends "(not timed)". Press R after
a hitch to read it. Taking a Pocket screenshot freezes the core for a few
seconds, which shows up here as one very slow frame.

Log lines stop going to the OS console after the first frame: the console
is hidden by then and writing to it is slow.

## Status

<!--
- What works on a real Pocket today? What have you actually played through yourself?
- No numbers here: they go out of date within a day of work. Point at the benchmark instead.
- What is still rough? (Which scenes run below full speed? What happens on a room change now?)
- What is untested? (MiSTer; anything past the opening hours.)
-->

- Runs on an Analogue Pocket with the os25 bitstream: boots, plays the
  intro, name entry works, and the first room and menu are playable.
- Use the os25 bitstream. The SDK's runtime `os.bin` paired with os20
  reboot-looped before the OS banner appeared.
- Music, sound effects and saves work on hardware, including a save
  imported from the desktop game.
- Undertale save-state test builds passed Pocket save/load and repeated
  sleep/wake checks in supported overworld scenes, including the quieter UI.
  The final integration with main passed desktop restore checks and native
  compilation; it has not had a separate Pocket run. Build prerequisites and
  supported scenes are listed under “Save states and sleep” above.
- Not everything runs at full speed. Which scenes do, and by how much the
  others miss, is what the benchmark is for.
- Deltarune Chapter 1 plays on the desktop build through the opening and a
  Dark World battle. Fog (the hit flash) is not implemented in the software
  renderer.

## Download

<!--
- Is there a build to download yet, and where? Upstream links nightly builds of each platform from
  its CI; the community ports attach a binary to a GitHub release. Neither is set up here yet.
- What can a download contain, given that no game data may be included? (The core, and the two
  tools that build textures.bin and music.bin from the reader's own data.win.)
-->

_To be written._

## Licences and credits

<!--
- Who wrote Butterscotch, and under what licence? What does that licence mean for a core built
  from this repository?
- What does this branch add to Butterscotch? (The software renderer from draft PR #429, this port's
  changes to it, and src/openfpga.)
- What licence is the openfpgaOS SDK under, and does any of it ship in a built core?
- Is there anyone else to credit?
-->

_To be written._

## Disclaimer

<!--
- Butterscotch's own README has a disclaimer about having no association with the software it
  runs and not providing it. Do you want to follow its wording or write your own?
- What must a reader understand about game files before they start?
-->

_To be written._
