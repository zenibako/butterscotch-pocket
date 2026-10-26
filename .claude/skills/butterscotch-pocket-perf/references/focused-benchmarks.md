# Focused battle text and flashing-tile measurements

Use the maintained [diagnostic instructions](../../../../src/openfpga/docs/2026-10-18-focused-pocket-benchmark.md)
for build/launch commands and the [corrected route validation](https://github.com/zenibako/butterscotch-pocket-legacy/issues/50)
for semantic windows. The reported corridor is room 65 `room_field_checkers4`,
with flashing floor tiles that return the player to the start. Outdoor room 104
is a different workload; it cannot supply a corridor before/after baseline.

Measure encounter and observation text separately, while letters reveal and
after the line completes. Measure corridor walking, the entire hazard/reset
cycle and standing. Require accepted scene/sample checks, lit/dark phases,
white flash and an actual return to the start; frozen reset frames belong in
that cycle rather than being discarded as timing outliers.

The five modes are Normal, No Render, No Audio, Core Only and Profile. Compare
uninstrumented work excluding the same frame's flip. No Render retains Draw
scripts; No Audio removes decoding and output as well as mixing. Profile changes
timing and audio demand. These counterfactuals and overlapping script/draw/audio
counters are not independent components to sum or subtract as isolated costs.

Ask for all five entries to reach PASS and Quit through the Analogue menu after
each. Inspect the returned `deltarune_tiles_1.sav` locally, preserving the original
attachment. Require matching build IDs, seed/mode, complete PASS reports and
sample counts; retain report history and distinguish exact duplicates from new
measurements. Diagnostic saves use a separate namespace and volatile progress;
they do not validate ordinary gameplay saves, Memories or sleep/wake.

Before proposing another optimization, read the [physical silent-tail results](https://github.com/zenibako/butterscotch-pocket-legacy/issues/54)
and [background lookup results](https://github.com/zenibako/butterscotch-pocket-legacy/issues/49).
The former measured observation reveal falling from 100.131 to 63.219 ms/frame
in one physical run per mode; it did not establish a corridor speedup. The latter
documents the guarded live lookup and borrowed string search already implemented.
Keep remaining work and device validation scoped to the exact candidate and scene.

For changes to audio or native scripts, use the verify skill's optimization
checks before another device run. Keep normal production and identified diagnostic
ELFs separate when using the build skill's native-save-state packaging workflow.
