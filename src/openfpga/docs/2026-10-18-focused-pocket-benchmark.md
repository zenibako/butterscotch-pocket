# Focused Pocket benchmark: observation text and flashing tiles

This diagnostic extends the current writer/castle candidate. It measures the two
reported Speed-mode slowdowns on the Pocket before choosing another optimization.
The [existing optimization design](https://github.com/zenibako/butterscotch-pocket-legacy/issues/27)
requires separate host and device evidence; desktop route checks do not establish
Pocket performance. The [sequential investigation](https://github.com/zenibako/butterscotch-pocket-legacy/issues/28)
also requires measured work removal before retaining a new optimization.

## Install and run

Use the current Butterscotch installation with Deltarune Chapter 1 assets. Extract
the diagnostic ZIP into the SD root, merging its `Assets` folder. It adds one ELF,
five INIs and five launch entries. It contains no game assets, saves, OS or FPGA
files and does not replace the normal launch entries. Existing Memories/sleep
support remains installed; use fresh launches for these measurements.

Launch these entries in order:

1. **Deltarune Tiles 1 Normal**
2. **Deltarune Tiles 2 No Render**
3. **Deltarune Tiles 3 No Audio**
4. **Deltarune Tiles 4 Core Only**
5. **Deltarune Tiles 5 Profile**

Each entry runs automatically from seed 7, enters the same battle, samples the
initial encounter and later observation text while revealing and complete, then
samples walking, a full hazard/reset cycle, and standing in the first flashing-tile
checkerboard corridor (room 65, `room_field_checkers4`). Do not press buttons
or change settings. The no-render entries may show a frozen screen until the
report appears. Wait for **Focus PASS; log saved**, then **Quit through the Analogue
menu after every entry**. This commits the log before the next launch.

After all five, return `Saves/butterscotch/common/deltarune_tiles_1.sav`. This single
file retains the five reports. If any entry reports FAIL or WRITE FAILED, send the
log and identify that entry. The earlier `Deltarune Focus` diagnostic and its log also remain separate.
Normal progress uses different save/log filenames;
the diagnostic also uses a volatile filesystem and starts without saved progress.
It does not create persistent gameplay progress. Diagnostic Memories slots use
separate names as well.

## Interpretation

All modes use Speed rendering, uncapped execution and fixed input-frame windows.
Each accepted sample verifies the expected room, battle objects, exact displayed
sentence and writer position, or the corridor's player and tile-generator objects.
The reset window retains moving and frozen frames and must contain both lit and
dark tile phases, a white flash, and an observed return to x=340. The walking and
standing windows require the corresponding movement state outside a white flash. Missing scenes or
insufficient samples fail the run. This route measures the identified workloads;
it does not replay the user's exact save or every possible battle sentence.

`work` is this input frame's wall time minus its own display-flip time. Mean,
median, nearest-rank p95 and maximum are reported in microseconds per frame,
alongside flip and other presentation time. Ten-frame block means show variation
among correlated samples; they are not independent trials. Step, Draw and audio
update clocks are separate supporting counters. The old output phase is omitted
because it belongs to the preceding frame.

Compare Normal with No Render, No Audio and Core Only to locate expensive work.
These are counterfactual measurements, not directly additive components:

- No Render retains Draw scripts and renderer setup while suppressing raster work.
- No Audio removes reads, decode, mixing and output, and advances muted voices by
  game time. It does not isolate just the mixer.
- Draw includes scripts, raster work and opportunistic audio work. Detailed mix
  clocks can overlap Draw and must not be added to it.
- Profile enables per-frame script and draw instrumentation. Its totals include
  substantial profiling overhead; use its rankings to explain the uninstrumented
  modes, not as baseline frame times. Script entries report exclusive time; nested calls have their own entries.
- Draw descriptions are a bounded summary; discarded call counts are reported.

No performance conclusion is established until the physical Pocket log returns.

## Rebuild and verify

Use a fresh `ROOT` (make does not encode compiler flags in object dependencies),
the paired immutable SDK and the shipping compiler. Build with `GAME=deltarune`,
`SAVESTATES=1`, `FOCUS_BENCH=1`, and `FOCUS_BUILD_ID=<source commit>`. Ordinary builds
leave the diagnostic entry points inert and keep their existing save filenames.

Run `make ... focus-stats-fixture` and its produced executable for frame/flip
accounting, percentile, capacity and rejection cases. Run
`python3 src/openfpga/tests/test_focused_package.py` for additive packaging and save
slot isolation. Validate all five fresh-process routes on desktop, a wrong-scene
negative control, normal/profile raw frame equality, and preservation of all five
report blocks through the actual diagnostic log file. Check native snapshot
symbols, fixed interpreter placement, fsync syscall and original runtime/asset
hashes before release. Desktop sanitizer diagnostics from vendored code must be
distinguished from benchmark failures.

Package with `python3 src/openfpga/tools/mkfocus.py --elf <app.elf> --output <zip>`.
The script reads the existing Deltarune instance as the slot template and changes
only the diagnostic INI/ELF and save slots 10–19.
