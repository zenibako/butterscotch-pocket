---
name: butterscotch-pocket-perf
description: Use when investigating Butterscotch Undertale or Deltarune performance on the Analogue Pocket, interpreting benchmark logs, diagnosing battle text or flashing-tile slowdowns, comparing optimization candidates, or assessing os20/os25 timing claims.
argument-hint: "[card: SD mount path] [harness: path or host:path of a zenibako/pocket-harness checkout, if any]"
---

# Performance work on the Pocket

## Inputs

Take these from the arguments; if the task needs one that is missing, ask
the user once rather than guessing a machine or path:

- **card**: where the Pocket's SD card is mounted (written `$CARD` below).
- **harness**: where the user's checkout of the private
  [zenibako/pocket-harness](https://github.com/zenibako/pocket-harness)
  repository is, as a path or `host:path`, for simulated runs. Without it,
  say a simulated check is possible and ask; don't hunt for it.

## Reading the log off the card

Prefer the log file to screenshots: it is the whole log, not the last 21
lines. It is written to the game's spare save slot when the core halts
(benchmark end, fatal error), from the menu's "Save log to SD card" and on Select +
Y with the debug buttons on, and reaches
the card when the user leaves the core through the Analogue menu:

```bash
tr -d '\0' < "$CARD"/Saves/butterscotch/common/deltarune_1.sav   # or undertale_1.sav
```

Check the first line (`Butterscotch log, <game>`) and the timestamps: the
file stays on the card until the next dump overwrites it, so an old one can
be mistaken for a new run. A benchmark's report ends with ms-per-frame and
calls-per-frame tables by section; start there before theorising about
where a section's time goes.

## Speed and accuracy

L switches between two settings, and the core starts in **speed** (the
desktop build starts in accuracy, because frame comparisons are made against
what the game asks for; `BS_SMOOTH=1` gives it speed). The user's rule,
2026-10-09: accuracy is the target. A shortcut that changes the picture or
drops frames belongs under speed only, and whenever an optimisation brings
accuracy up to speed's frame rate in some domain, remove speed's shortcut
there so the two settings stop differing in it. Before adding a new
speed-only shortcut, look for an exact optimisation first; after an exact
one lands, check which speed shortcuts it has made unnecessary. `--bench`
measures accuracy and `--bench-smooth` speed, so the pair shows where the
two have reached parity.

## Measure first; this project has paid for guessing

During the port, three confident explanations for a slow load were each
wrong, and each cost the user a card trip to disprove:

1. "The file is opened with a buffer, so big reads are served as small
   ones." A buffered 1 MB read measured as fast as an unbuffered one.
2. "Reading into heap memory is slower than into static memory." Reading
   into a heap block measured just as fast.
3. Both rested on a benchmark that re-read the same 2 MB, so every pass
   after the first was a cache hit. The real cause was that first-time
   reads are slow at every request size.

The pattern to avoid: a plausible story, a fix built on it, and a request
for the user to test the fix. The pattern that worked: add a measurement
that separates the candidate causes, have the user run it once, then fix
what the numbers point at. When you do offer an explanation before
measuring, label it as a guess and say what number would confirm it.

Check that a test measures what you think it does. If two readings that
"should" agree don't, suspect the test before the system.

## What is known (measured on the user's Pocket, v0.7 runtime, os25)

Before explaining a slowdown or choosing an optimization, read [measured performance](references/measured-performance.md) completely. It records tested hypotheses, measurement limits and historical results.

## Findings from play sessions (2026-10-07)

- **Stacked fade overlays.** Undertale's door script creates a full-screen
  fade object on every frame the player overlaps the door: once if the
  player is at the doorway's edge (a wall pushes them back out), 13 times
  through the middle. This is the game's behaviour, not a port bug, and it
  reproduces on desktop (`650:R*26` against `650:R*30` in the door replay).
  Each full-screen blend is about 37 ms. Butterscotch now folds consecutive
  identical solid overlays into one blend (`swrOverlayFlush`).
- **Sound gaps.** Slow frames showed a few ms of mixing across multi-second
  loads, so the OS file idle hook was not feeding audio on the v0.7
  runtime (*inferred from those timings*). `platformBusyTick` now feeds the
  queue between read pieces and during drawing; the user confirmed the
  cutouts stopped.
- **Tile-heavy rooms.** The ruins room outside Toriel's house drew 437
  tiles for 26 ms and ran at 44-48 ms of work per frame. With the tile-run
  cache (`drawTileRun`) the user measured about 25 ms.
- **What a slow frame's draw time contains.** The held overlay and the
  tile picture are drawn outside or under different counters than before:
  an overlay's blend lands in whichever call flushes it, often none of the
  five kinds, so the kinds can add up to less than the draw phase.
- **Texture memory (2026-10-08).** The Pocket could not allocate the 8 MB
  that page 23 (2048x2048, the monsters) needs: battles showed no enemies,
  and each failed attempt threw out every other page, 1.3-2 s per frame.
  With pages loaded whole there was room for roughly 20-25 MB of them, so
  entering and leaving a battle re-read about 8 s of pages. The pack is now
  tiled and the renderer keeps one texture per TPAG item
  (`swrTextureForItem`); on desktop the same walk into a battle holds 2 MB.
  A page that fails to load is not retried for 150 frames.
- **Screenshots stall file access for 2-4 s.** fread waits it out. A
  direct read issued during one never completes, so those time out after
  1.5 s and fall back to fread for that piece.
- **Still open.** One frame of 150-250 ms on each room change (room load
  19-75 ms plus the first draw), and one unexplained 699 ms music read
  seen before reads were cut to 16 KB pieces.
- **The log overlay costs about 60 ms a frame** (average work 107 ms with
  it on against 44 ms off in the same room). Read the Select counters with
  the log off.

## Instruments

Before interpreting timings, adding instrumentation or requesting a benchmark, read [instruments](references/instruments.md) completely. It defines the counters, modes and measurement traps.

## Working through a report from the user

For battle letter-reveal or flashing-tile corridor slowdowns, use
[focused scene benchmarks](references/focused-benchmarks.md) before extending
the old whole-route benchmark. It records the corrected room, diagnostic modes,
returned-log checks and existing findings that should not be rediscovered.

1. Reproduce the scene on desktop if you can (see the
   butterscotch-pocket-verify skill) and read the log for texture loads,
   room changes and mode switches around the moment they describe. A
   freeze that lines up with `Loaded TXTR page` is a page load.
2. If the slow part is `step`, get script times for the scene: on desktop
   for the ranking by instructions (`BS_PROFILE=60`), and from the user
   (Select + X) for real milliseconds.
3. Profile the desktop build with `sample <pid> 5` while the scene runs.
   The game's own functions are a small share of samples next to the SDL
   display code, so read relative weights among `swr*` and VM functions
   only.
4. If the cause is still unclear, add a measurement to the benchmark or
   the log rather than a fix.
5. When you do change something, verify output is unchanged on desktop,
   then ask for one benchmark run and name the lines you need.

## Asking the user for a run

Card trips are the scarce resource. Batch changes that are independent and
individually verifiable, but keep a risky change (anything that alters how
frames reach the screen, video modes, or the OS runtime) separable so a
failure can be attributed. Say exactly which entry to run on which core and
which lines of the result you need. State predictions before the run
("load should drop to about 19 s") so the result can confirm or refute
them, and say plainly when a result refutes one.

## A clock read is a trap: 17 us (2026-10-10)

`nowNanos()` on the device is `clock_gettime`, an ecall into the kernel:
**16.8 us** per read, measured by the benchmark's "call costs" line.
VexiiRiscv here does not expose `rdcycle`/`rdtime` to user mode (they trap as
illegal instructions), so there is no cheaper clock. Consequences:

- A sprite draw that draws nothing costs 40 us, of which 6.5 us is the call
  and 33.5 us was the two clock reads timing it. A battle makes about 100
  draw calls a frame: 3 to 4 ms of timing, more with the mixing split.
- Draw calls and mixing are therefore only timed "in detail"
  (`ofPerfDetail` / `swrDrawTimed`): every frame while an overlay or debug
  mode is on, one frame in 8 during a benchmark (`OF_BENCH_DETAIL_EVERY`),
  never in plain play. The benchmark's spr/til/txt/bkg/rct columns, the
  mix/voices/write columns and the costliest-draws list are averages over
  the timed frames; frame times and step/draw/snd/out are of every frame.
- Benchmark numbers from before this change (through build `72eed8e`) carry
  that overhead in every section, and numbers before `946af8c` also carry
  about 0.15 ms per draw call of note formatting.
- Before adding any timing to a hot path, count the reads per frame. The
  script profiler (Select + X, `--scripts`) pays two per script and built-in
  call, about 18 us with its lookup: its times rank things, nothing more.
- `of_audio_free()` is 0.5 us, so the audio queue can be polled freely.

## Why reading the frame buffer is slow (2026-10-10)

*Measured on the device:* about 245 ns a pixel to read the frame buffer
against 27 ns to write. The first explanation was that app frame buffers
live at the **uncached** SDRAM alias (`0x50xxxxxx`), where every load is an
AXI round trip.

**Corrected 2026-10-11, from the OS source:** `target_platform.h` in
openfpgaOS has handed apps the **cached** alias (`FBn_BASE` at
`0x10xxxxxx`, flushed at the flip) since commit `ca5bb0a` (2026-04-16), and
both Pocket runtimes are built from OS revisions that contain it (v0.7
from `618a3eb`, v0.9 from v0.9.5 plus two commits). So the frame
buffer is most likely cached on the device, and the 245 ns read is
unexplained. Branch `claude/project-thread-quaq0g` logs the address at
boot (`Video: frame buffer N at 0x...`); one device run with it settles
the question. Until then, don't justify a change by "the frame buffer is
uncached". What still holds, whatever the cause:

- Anything that reads the frame buffer per pixel is the expensive kind of
  draw: translucent fills and sprites, text edges, a pass over another.
  Work out the colour without reading where the layers under it are known
  (`swrFillHold`, the held clear, `swrTiledHeldWrite`'s row buffer), or put
  the rows together in ordinary memory and copy them out.
- If reads must happen, two pixels per 32-bit load halves them (with 16-bit
  pixels, as the Pocket build uses).
- Scattered single-pixel stores were taken to be a round trip each, with
  about 2.5 us more per row a draw touches. Neither figure reproduced in
  the os25 harness (below): uncached cost there is flat per access, and
  back-to-back stores, the clear and fills, are what cost. Don't re-use
  the per-row or 1 us scattered-store figures without an isolated probe.

## Simulated runs: the os25 harness (2026-10-11)

The user has a cycle-level simulator of the os25 CPU and memory system,
the os25 harness, in the private repository zenibako/pocket-harness (the
**harness** input). The Butterscotch workload is its original one, at the
repository root. Read its `README.md` and its
`.agents/skills/os25-harness/SKILL.md` before running it. Game data,
captures and cases stay in its gitignored `inputs/` and `experiments/`;
never commit them there or here.
It answers cycle questions without a card trip:

- **Everything from it is simulated.** Say so with every number, and
  never present a desktop or cloud check as harness evidence.
- **It models what it is told.** Its OS model puts the frame buffer where
  the OS source says, so it cannot tell you which alias the device's
  runtime uses, or anything else about the device that the source would
  have to be right about. It does not model scanout contention,
  interrupts or the audio hardware.
- **It repeats exactly for the same build**, so differences between two
  builds are real for that model, but code layout alone moves results by
  a few percent. Use it for large wins and regressions and for "is this
  work wasted" tests, not 1 to 3% claims.
- **A cheap way to find wasted work:** replace a draw with a wrong colour.
  If the frames still match, everything it drew is overdrawn. That is how
  dodge's redundant full-screen clear was found (5.6 ms simulated).
- Record results as a closed issue in zenibako/butterscotch-pocket titled
  `Archived results (Butterscotch): <topic> (YYYY-MM-DD)`, with the window,
  frames and build, and date them here. Don't add results markdown to the
  repository.

Results so far (Butterscotch main `05b0eef`, three frames per window):

- An uncached frame buffer would cost 4 to 12 ms a frame (dodge 71.4 to
  83.6 ms, field 40.1 to 46.5, Flowey 11.5 to 15.5), most of it the
  full-screen clear (`swrClearSettle`, about 4.7 ms), then
  `swrTiledHeldWrite`, `swrDrawHLineInt` and row `memcpy`. Since the
  device is probably on the cached alias, treat these as an upper bound.
- Uncached reads cost 210 to 229 ns in the model, close to the device's
  245 ns. Spaced-out stores cost about 29 ns, but back-to-back stores 99
  to 122 ns.
- In a Deltarune battle the opaque full-screen backdrop dropped the held
  floor grid without taking over the held clear, so the clear was written
  in full and painted over. The fix is on `claude/project-thread-quaq0g`.

## Sprite-path layout experiment closed (2026-10-10)

*Measured on the Pocket, f42a23c, v0.7/os25, Deltarune Speed benchmark:*
the ordinary build's 40x40 sprite probe was 19.87 us outside the view and
73.48 us inside; the adjacent-function build was 19.56 us and 78.34 us.
Battle work changed from 67.8/80.2/62.8 to 67.5/80.0/62.7 ms; field work
stayed at 44.4 ms. The hot build booted and completed. One probe-bearing
run per build, so there is no run-to-run variance estimate.

The proposed criterion was a result well below 19.9 us outside the view;
this layout failed it and is dropped as a performance direction. It does
not establish that instruction-cache misses are absent elsewhere: the
probe is 2000 repeated renderer calls after warming the sprite, bypassing
the GML builtin and VM. No cache-miss counters are measured.

See the [result and exact device-report excerpts](https://github.com/zenibako/butterscotch-pocket-legacy/issues/30).

## Code layout is not it (2026-10-10)

*Measured, and it overturned a hypothesis:* a small sprite draw costs about
15 us on the Pocket between the renderer's door and finding itself out of
view, for some 150 instructions, and its functions are spread over 900 KB of
a 1.25 MB program with a 32 KB instruction cache. Linking those sixteen
functions side by side (`ld --section-ordering-file`, 14.7 KB in one
stretch) changed nothing: the sprite probe read 19.56 against 19.87 us out
of view and 78.3 against 73.5 in view, and every section's frame time was
within 0.5 ms. So instruction-cache conflicts among a draw's own functions
are not where that time goes. (Together with `-Os`, unaligned code and the
loop in block RAM, that is four layout experiments with nothing to show.)
The benchmark now ends with a staged probe instead (`swrSpriteCostProbe`):
the same draw cut short at six points, each timed over 2000 calls with
audio top-ups off, on a grid's own sprite when one was drawn.
