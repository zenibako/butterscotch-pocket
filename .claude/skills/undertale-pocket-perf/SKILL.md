---
name: undertale-pocket-perf
description: Measure and diagnose performance of the Undertale port on the Analogue Pocket — frame rate, load time, SD read speed, texture-load freezes, audio dropouts. Use this whenever the user reports something slow, choppy or frozen on the device, sends a benchmark table or overlay screenshot, asks whether a change helped, asks about os20/os25 or openfpgaOS versions, or before starting any optimisation work in src/undertale. Also use it when you are about to explain why something is slow: this skill records which explanations have already been tested and ruled out.
---

# Performance work on the Pocket

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

- **CPU:** roughly 100–170x slower than this Mac per frame of game work.
  Desktop work of 0.3 ms per frame is about 31 ms on the device.
- **SD reads:** about **1.1 MB/s for data read the first time**, the same
  for 4 KB, 64 KB and 1 MB requests. Repeat reads of the same region come
  back at about 13 MB/s from a cache below the app. Nothing about how a
  read is issued changes the cold speed; only reading fewer bytes helps.
- **Memory fill/copy:** tens of MB/s. A 640x480 16-bit frame is 614 KB, so
  every extra full-screen clear or copy costs on the order of 15 ms.
- **Heap:** about 51 MB for everything (game data, texture cache, audio).
- **Division is slow**; avoid per-pixel divides in the renderer.
- **OS version and CPU variant:** v0.9 on os25 is identical to v0.7 on
  os25 to within 0.1 ms. v0.9 on os20 is only 2–13% faster, and showed a
  garbage screen when the port used its 640x480 16-bit mode. Neither is a
  route to more speed.

Benchmark history, ms of work per frame (33.3 is full speed):

| Section | Start | + draw into display buffer | + skip redundant clear |
|---|---|---|---|
| Intro, menu, naming | 45.2 | 39.3 | 37.1 |
| First room 320x240 | 38.8 | 33.5 | 31.2 |
| Flowey dialogue 320x240 | 52.9 | 44.6 | 40.9 |
| Flowey battle 640x480 | 86.7 | 60.6 | 46.3 |

The battle at 320x240 with 2x2 smoothing (L button) runs at 28.3 ms. The
user chose crisp 640x480 as the default knowing that trade.

Load time went 31.6 s → 21.0 s (stop reading the 12 MB texture chunk that
is only needed lazily) → about 19 s (stop reading the string chunk twice).
What remains is about 11 s of unavoidable cold reading of roughly 12 MB,
1.4 s of parsing and a few seconds of VM start-up and first textures. The
startup chunks compress about 4.4:1 with zlib, which is the remaining
lever; the decompressor would have to be much faster than inflate to pay
off on this CPU.

Texture page loads take about 1.3 s for a 1024x2048 page: a cold read of
the run-length-encoded page plus the decode. That is the "brief freeze"
when a dialogue box or new room first appears.

## Instruments

**Slow-frame log lines.** Any frame over 150 ms of work logs three lines
that fit the overlay's 53 columns:

```
slow 555: step 300 draw 200 out 20 snd 10
  load: room 0 tex 0 sfx 0 mix 12 music 0
  draw: s5/380 p437/20 t12/40 b1/3 r0/0
```

Phases first (game code, drawing, presenting, audio update), then jobs
inside them (room, texture and sound-effect loads, audio mixing, streamed
music reads), then calls/ms per kind of draw call (sprites, sprite parts,
text, tiled backgrounds, rectangles; `-DSW_DRAW_PROFILE`). Writing a log
line to stdout costs about 20 ms on the Pocket, so the console is switched
off after the first frame; keep logging out of per-frame paths regardless. For a hitch, ask the user to press R straight afterwards and
screenshot the log. Two traps when reading these: a Pocket screenshot
freezes the core for 2-4 s and appears as one huge frame with nothing
attributed, and the log overlay only shows the first 53 characters of a
line, so anything longer is invisible on the device.

**Benchmark** (`--bench` in the OS config's `ARGS=`; `make compare` adds a
"Benchmark" entry to each core). It plays a fixed input script with a
fixed seed, no frame pacing and saves disabled, then draws a report:

```
OS 0.7.0, core variant 0, CPU 100 MHz
load to first frame: 19.1 s
slowest chunks: CODE 4.8s ROOM 3.8s SPRT 1.8s ...
phases: alloc 574 ms, read 10799 ms, parse 1384 ms, free ...
ms per frame:          work   total
intro, menu, naming    36.8   37.9
...
SD read, KB/s, 1 MB each from music.bin:
cold: 4K 1091, 64K 1102, 1M 1115
same 64K region again: 13441
```

"work" excludes the display flip. The report does not identify the build,
so ask which build a screenshot came from if it matters, and remember the
user may be one build behind what you last produced.

Variants: `--bench-smooth` (320x240 with smoothing), `--bench-lowres`
(320x240, point sampled). Edit `platform/ut_bench.c` to add a measurement;
keep the report within 20 lines of 53 characters, which is what fits on
the 320x240 report screen.

**Overlays during normal play:** Select shows three numbers (average
work, worst work, worst frame period over 30 frames, in ms). R shows the
last log lines over the game. L toggles crisp/smoothed 640x480 rooms. The
log overlay itself costs a lot of frame time, so read the numbers with it
off.

**Log lines worth knowing:** every line carries seconds since start.
`DataWin: NAME, n KB` marks each chunk as loading starts on it.
`SWR: Loaded TXTR page N (WxH, pack|PNG), cache K KB` and
`SWR: Unloaded TXTR page N` show the texture cache; pages reloading
repeatedly mean the cache is thrashing. `Video: 640x480, stride 1280`
marks a mode switch. `Audio: playing NAME` marks a streamed track.

## Working through a report from the user

1. Reproduce the scene on desktop if you can (see the
   undertale-pocket-verify skill) and read the log for texture loads,
   room changes and mode switches around the moment they describe. A
   freeze that lines up with `Loaded TXTR page` is a page load.
2. Profile the desktop build with `sample <pid> 5` while the scene runs.
   The game's own functions are a small share of samples next to the SDL
   display code, so read relative weights among `swr*` and VM functions
   only.
3. If the cause is still unclear, add a measurement to the benchmark or
   the log rather than a fix.
4. When you do change something, verify output is unchanged on desktop,
   then ask for one benchmark run and name the lines you need.

## Asking the user for a run

Card trips are the scarce resource. Batch changes that are independent and
individually verifiable, but keep a risky change (anything that alters how
frames reach the screen, video modes, or the OS runtime) separable so a
failure can be attributed. Say exactly which entry to run on which core and
which lines of the result you need. State predictions before the run
("load should drop to about 19 s") so the result can confirm or refute
them, and say plainly when a result refutes one.
