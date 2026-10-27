## Instruments

**Slow-frame log lines.** Any frame over 150 ms of work logs three lines
that fit the overlay's 53 columns:

```
slow 555: step 300 draw 200 out 20 snd 10
  load: rm 0 tx 0 fx 0 mix 12 mus 0
  draw: s5/380 p1/9 t12/40 b1/3 r0/0
```

Phases first (game code, drawing, presenting, audio update), then jobs
inside them (room, texture and sound-effect loads, audio mixing, streamed
music reads), then calls/ms per kind of draw call (sprites, sprite parts,
text, tiled backgrounds, rectangles; `-DSW_DRAW_PROFILE`). Writing a log
line to stdout costs about 20 ms on the Pocket, so the console is switched
off after the first frame; keep logging out of per-frame paths regardless. For a hitch, ask the user to press R straight afterwards and
screenshot the log. Two traps when reading these: a Pocket screenshot
freezes the core for 2-4 s and appears as one huge frame, attributed to
whatever was running (a 3 s "music read" or "texture load" right after a
screenshot is the screenshot), and the log overlay only shows the first 53 characters of a
line, so anything longer is invisible on the device.

**Script times** (the menu, or Select + X with the debug buttons on;
`BS_PROFILE=n` on desktop).
The slow-frame line says "step 300"; this says which scripts the step
went to. It is Butterscotch's GML profiler (`--profile-gml-scripts`),
reported every 60 frames in a form that fits the log overlay:

```
scripts 6.4 ms 5210 ops /frame (41, 60 fr)
  2.1  1830   1.0 obj_mainchara_Step_0
  1.2  7218   3.0 obj_base_writer_Draw_0
  ...
```

First line: all game code together per frame, how many scripts ran, the
window. Then the heaviest twenty: ms per frame, VM instructions per frame,
calls per frame, name (without `gml_Object_`/`gml_Script_`). Reading it:
- Built-in functions are entries of their own since 2026-10-09 (0 ops),
  so a time is an entry's own: neither the scripts nor the built-ins it
  calls. A drawing built-in's time is the renderer's.
- ms divided by ops is a script's cost per instruction, now without its
  built-ins; look for those by name further down the list.
- Desktop times are near zero and mean nothing; the ops column is the
  same on both, so the desktop can rank scripts by instructions for a
  scene before asking for a device run.
- Timing every script call costs time itself. Take frame-time numbers
  with it off. The cost of having it compiled in but off (a test per
  instruction) has not been measured on the device.
- The report is up to 21 log lines every two seconds, more than the
  overlay shows: read it from the log file, not a screenshot.
When asking the user for it: go to the scene, Select, "Show script times in log" (the
log comes up with it), close the menu, wait a few seconds, screenshot.

**Opcode ranking** (desktop only: `make ops`, then run `undertale_pc_ops`
with `BS_EXIT_FRAME=n`, usually with `BS_PLAYBACK` or `BS_SCRIPT`). Prints
how often each bytecode instruction ran, split by operand types, when it
leaves the main loop. Counts are the same as on the device. Use it before
hand-tuning the interpreter, to see which instruction and type
combinations a scene is made of; it does not say what each costs.

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
(320x240, point sampled). Edit `platform/of_bench.c` to add a measurement;
keep the report within 20 lines of 53 characters, which is what fits on
the 320x240 report screen.

**Overlays** (switched on in the menu: press Select, then Debug → "Show frame times"
or "Show log overlay"): frame times are four numbers over 30 frames (average work,
worst work and worst frame period in ms, then frames skipped). The menu's
footer explains the selected item instead of showing timings. The log shows the last log lines over the game; R toggles it too with
the debug buttons on. The log overlay itself
costs a lot of frame time, so read the numbers with it off.

**L, speed or accuracy.** Always ask which the user was in; numbers from
the two are not comparable. Speed does three things: 640x480 rooms at
320x240 smoothed, faint mirrored blend layers left out
(`swrMirrorFaintAlpha` 8, against 0 in accuracy), and frame skipping. With
skipping on, "average work" falls because skipped frames are cheap, and
the worst period can go under 33 (a frame shortened to pay back time), so
judge a scene in speed mode by the skipped count: 0 means it holds full
speed drawn, 15 means every other frame is being dropped. To measure what
a scene costs to draw, use accuracy mode or the benchmark, which never
skips. A new fidelity-for-speed trade-off belongs under this toggle (see
the comment above `decideFrameSkip` in `platform/of_platform.c`), not
behind a new button; one that cannot be switched at run time (colour
depth, mono audio, float reals) is a known difference for the README.

**Log lines worth knowing:** every line carries seconds since start.
`DataWin: NAME, n KB` marks each chunk as loading starts on it.
`SWR: Loaded TXTR page N (WxH, pack|PNG), cache K KB` and
`SWR: Unloaded TXTR page N` show the texture cache; pages reloading
repeatedly mean the cache is thrashing. `Video: 640x480, stride 1280`
marks a mode switch. `Audio: playing NAME` marks a streamed track.
