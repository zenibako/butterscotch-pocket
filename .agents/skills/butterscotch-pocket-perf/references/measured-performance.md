## What is known (measured on the user's Pocket, v0.7 runtime, os25)

- **CPU:** roughly 100–170x slower than the desktop build on a recent Mac per frame of game work.
  Desktop work of 0.3 ms per frame is about 31 ms on the device.
- **SD reads:** about **1.1 MB/s for data read the first time**, the same
  for 4 KB, 64 KB and 1 MB requests. Repeat reads of the same region come
  back at about 13 MB/s from a cache below the app. Nothing about how a
  read is issued changes the cold speed; only reading fewer bytes helps.
- **Where the 1.1 MB/s comes from** (benchmark, 2026-10-08, same on v0.7
  and v0.9; OS source read at openfpgaOS 8318f25): `fread` is served from
  the OS's 4 MB cache in 32 KB blocks, one host command per block, with
  two copies on the way. A single non-blocking read (`of_file_read_async`)
  takes 6 / 13 / 23 / 42 ms for 4 / 16 / 32 / 64 KB: about 3.5 ms fixed
  plus 0.6 ms per KB, so the host delivers about 1.7 MB/s and that is the
  ceiling. A megabyte in 64 KB commands runs at 1.5 MB/s into the OS's
  staging memory and 1.3 MB/s into ordinary memory. The per-command round
  trip is NOT the bottleneck (a guess that the benchmark refuted). The
  OS's file idle hook is deliberately not called during an app's reads on
  the Pocket; feed audio from the app.
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

### Reads and multiplies both cost; the compiler does not help (2026-10-09)

*Measured:* a full-screen translucent fill at 320x240 takes 15 to 19 ms even
when it only reads each pixel and compares it with the last (no arithmetic,
no write): about 245 ns a pixel just to read the framebuffer. A fill that
only writes is about 2 ms. Copying a cached tile picture to the screen costs
about 110 ns a pixel. `-O3` and LTO changed nothing (within 2%).

*Measured, and it overturned two conclusions in a row:* a tiled pass that
put down about 6,000 scattered pixels (one per row per copy of a tile of
thin lines) took 6 ms. Taking the buffer reads out of it (the colour under
each pixel known) left it at 6.4 ms. Taking the blend's arithmetic out of it
as well (the last answer kept) left it at 5.9 ms. Two divisions a pixel, by
contrast, made it 15 ms, so divisions are dear. *Inferred, not yet confirmed
on the device:* what is left is the store itself: each pixel is on a cache
line of its own, and a store to a line not in the cache costs about a
microsecond, where a fill of consecutive pixels costs about 27 ns each. The
change made on that inference writes such a pass together with the clear
under it, row by row, so that its pixels land on lines the fill has just
brought in (`swrTiledHold`).

*Measured:* with the drawing left out altogether (`--draw-every 100000`,
the "no drawing" benchmark entry) a Deltarune battle frame still takes 46 to
50 ms: draw-event scripts about 24 to 30, step 7 to 21, sound 6 to 12. So
battles cannot reach full speed by drawing less or skipping frames; the
field (24 ms undrawn) can. Making the code smaller does not help: `-Os` (768 KB of
code against 1.27 MB) is 16 to 22% slower and unaligned code 2 to 4% slower.
That does not settle whether fetching code from SDRAM is what holds the
scripts back: `-Os` also inlines less, and neither build moved the hot code
out of SDRAM. The direct test was the interpreter's loop (8.9 KB) in the
app's 14 KB of uncached block RAM (`of_fastram.h`, `-DOF_FAST_LOOP`).
*Measured:* 2 to 4% of an undrawn frame in every section (battle rows 46.7,
47.2, 47.5 became 45.3, 46.2, 46.4). Kept as the default, but it shows the
loop itself is not where a script operation's time goes; the desktop
profile, which put more than half in the loop, does not carry over.
A pool for script call frames in place of calloc and free
gained 0.2 to 0.3 ms, within noise, and was dropped. With the script
profiler on and nothing drawn, scripts are 35 to 39 ms of a battle frame;
plain scripts run at about 1.9 microseconds an instruction and `scr_charbox`
at 4.8, about 10 ms a frame, so its time is not all interpretation. The
profiler now times built-in functions under their own names, with calls per
frame, to say what the rest is.
*Measured the same day:* the collision-threshold fix took 3.4 ms off the
dodging section's step time; the other three double-precision fixes showed
nothing.

*Found by reading the device build's disassembly, effect not yet measured:*
places that fall into software double precision on this single-precision
FPU. `riscv64-elf-objdump -dlr` on an object built with `-g`, looking for
calls to `__*df*` and `__*di*` helpers, names the source lines. The ones on
a per-frame path were the collision thresholds (three compares for every
pixel a precise test looks at), the image_index sum for instances that are
not animating, double constants pushed by scripts, and `div`.

So, before optimising a draw, find out which it is. The benchmark report's
"costliest draws" list (`sw_call_notes.h`) names the calls and times parts
of them; three guesses at the tiled background were wrong before that list
existed, and one after. What has worked: not drawing at all (held layers
dropped under an opaque full-screen fill), fewer passes over the screen, and
no division per pixel where a running value will do.
