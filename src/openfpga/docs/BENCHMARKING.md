# Pocket benchmark integration

This fork owns the runner, Pocket platform, game-data tools, plugin and release
pipeline together. Follow [Build and release](BUILD.md), the current
[`release/core.lock.json`](../release/core.lock.json) and the
[game-data contract](../GAME-DATA.md) before constructing a diagnostic. The
[focused scene benchmark](2026-10-18-focused-pocket-benchmark.md) defines its
existing route, correctness checks and packaging isolation; this guide connects
that evidence to the shared collector.

## Find the shared workflow

Codex discovers the local [pocket-benchmarking router](../../../.agents/skills/pocket-benchmarking/SKILL.md).
All six project skills live in the physical `.agents/skills/` collection,
including the existing release, build, performance, verification and platform
notes. Claude discovers that same collection through the `.claude/skills`
root symlink; individual skill-directory aliases are not used.
Keep this thin route local; the maintained collector workflow lives in the
private `zenibako/pocket-harness` checkout, alongside its implementation.

Use a harness path already established in the session or `POCKET_HARNESS_ROOT`.
An adjacent checkout is only a candidate. Verify that it contains
`benchmarks/__main__.py` and `.agents/skills/pocket-benchmarking/SKILL.md`, resolve
the latter's canonical path, then read it and its linked `benchmarks/README.md`,
`PLATFORMS.md` and `SELECTION.md`. Run the commands below from that harness root.
Do not copy the harness skill body here. Missing access limits collection;
applicable local correctness checks can continue. RV32 capture/replay/profiling
routes separately to the harness's canonical `os25-harness` skill.

## Plan this project's evaluation

Choose representative Undertale v1.08 and experimental Deltarune Chapter 1
scenes, metrics and acceptance targets before collecting. Start with the matched
shipping runtime. Shortlist only compatible alternatives after reviewing ISA,
features, memory, SDK/API and packaging requirements. In a private selection note
under the harness's `build/`, record each candidate's profile ID/digest and why
it is selected, incompatible, deferred or not considered. The os20/os25 × 90/100
MHz matrix inventories evidence; this project need not evaluate all four cells.
Variant labels do not determine clock or cache sizes.

Prefer a separate benchmark core beside gameplay, with isolated settings,
progress, result slots and Memories identity. The additive bullet-hell package
uses `zenibako.BsBullet`, `bs_bullet.elf`, `bs_bullet_os.bin` and isolated
`bs_bullet_0.sav` through `bs_bullet_9.sav`; it preserves asset slots 4–7 and
leaves the gameplay core and saves unchanged. The current focused package adds
diagnostic launch entries under the gameplay core instead; preserve its existing
save isolation. Match relevant production paths and record instrumentation,
renderer, audio and pacing differences. Do not overwrite shared runtime/assets
while constructing side-by-side packages.

Build the diagnostic ELF from clean committed source with the matched SDK and
`GAME=undertale APP=bs_bullet SAVESTATES=1 FOCUS_BENCH=0` using the `elf`
target. Set `OF_BUILD_NOTE=<version>-<short-commit>` to identify each A/B build
in returned report titles. The packager checks that the RISC-V ELF uses the
benchmark's save and log slot names. Pass that ELF and an explicit test version.

```sh
python3 src/openfpga/tools/mkbullet.py \
  --elf src/openfpga/out/bullet-build/.obj/bs_bullet/app.elf \
  --version "$VERSION_WITHOUT_V" \
  --output "../artifacts/bullet-hell-$(git rev-parse --short HEAD)/bullet-hell.zip"
```

The ZIP contains no game data or saves. It has entries for the full bullet-hell
tour, the short Chaos Saber benchmark and the Photoshop Flowey checkpoint tour:

| Entry | Exact `ARGS=` value |
| --- | --- |
| Bullet Hell - Accuracy | `--bench-bullet-hell` |
| Bullet Hell - Speed | `--bench-bullet-hell-smooth` |
| Bullet Hell - No Render | `--bench-bullet-hell --draw-every 100000` |
| Bullet Hell - No Audio | `--bench-bullet-hell --mute` |
| Bullet Hell - Core Only | `--bench-bullet-hell --draw-every 100000 --mute` |
| Bullet Hell - Profile | `--bench-bullet-hell --scripts` |
| Chaos Saber - Accuracy | `--bench-chaos-saber` |
| Chaos Saber - Speed | `--bench-chaos-saber-smooth` |
| Flowey - Accuracy | `--bench-flowey` |
| Flowey - Speed | `--bench-flowey-smooth` |
| Flowey - Speed (Live Pacing) | `--bench-flowey-live` |
| Flowey - No Render | `--bench-flowey --draw-every 100000` |
| Flowey - No Audio | `--bench-flowey --mute` |
| Flowey - Core Only | `--bench-flowey --draw-every 100000 --mute` |
| Flowey - Profile | `--bench-flowey --scripts` |

For the short Chaos Saber benchmark, compare the 150-frame `saber` rows
in Accuracy and Speed. Both use the same direct Asriel entry and 82-frame
setup. Use a separate baseline from the full tour, which enters via Asgore
and consumes a different sequence of random numbers. The short benchmark
skips the standalone SD and renderer probes.

For Flowey, compare the 160-frame `vines` and `eyes` rows. Both are natural
attack windows in `room_floweyx`, reached via its phase 2 and phase 6
checkpoints. The `vset` and `fset` rows include entry and warmup and should
be kept separate from attack timing. Flowey uses the same `bs_bullet_1.sav`
report slot as the Asriel tour.

Use Live Pacing to measure gameplay cadence with the Speed limiter active.
It reports completed game updates, adaptive screen skips, display
submissions, and elapsed wall time, including the limiter's wait. The
`other` column includes updates during room transitions that do not submit
a picture. Separate `drawn` and `skip` rows report the sample count and
average work, limiter wait, and total milliseconds per update. An update
with no eligible swap stays in the wall-clock rate and out of both timing
rows. A drawn update can count without a display submission, for example
when the desktop flip is disabled. The uncapped Speed entry measures the
cost of drawing every frame. Compare timings within the same entry.

The Flowey opening table uses scripted update numbers so it also includes the
room transition, which can occur without a display submission. `cold` and
`return` cover entry through the first backdrop draw. Each is followed by
separate fade, hold, and TV laugh windows. The second entry is a later
checkpoint in the same process and can reuse textures from the first.
`tx-ms` sums texture load time in each window, and `max-ms` is the longest
update's work time. `warm-ms` averages only drawn updates with no texture
loads. A dash means there were no such samples. Audio work is still included.

The texture window read table separates successful direct reads, declined
direct attempts, and the ensuing stdio attempts for the same windows. A
decline includes an unavailable direct backend. Counts are renderer read
windows, and the time columns are totals in milliseconds for each backend.

The `45x31 raster` lines show the number of rotated sprite calls on the timed
frames and the mean microseconds per call. `warm` reuses a cached raster,
`build` creates and blits one, `pivot` bypasses the cache because the pivot
is outside the supported range or off its half-pixel grid, and `other` covers
remaining fallbacks. These calls also appear in the ordinary sprite totals.

Start with repeated paired Accuracy and Speed runs on the same Pocket. Add No
Render, No Audio, Core Only and Profile only when attribution is needed. Record
the actual ELF, version, flags and package hash privately with the collected
reports; do not publish this diagnostic package as a stable release.

Prepare a version-1 platform definition using the harness's `PLATFORMS.md` shape.
Declare the actual CPU/ISA/issue/cache/features and OS/SDK source identities.
Use the exact packaged OS image, RBF, Chip32 loader and CPU configuration used
to build that platform, including local changes. A generic archived runtime or
source revision alone is insufficient. With paths explicitly selected for that
package (shell variables below), create and verify its immutable profile:

```sh
python3 -m benchmarks profile create "$DEFINITION" \
  --os-bin "$OS_BIN" --bitstream "$RBF" --loader "$CHIP32_LOADER" \
  --cpu-config "$CPU_CONFIG" --output build/platforms/candidate.json
python3 -m benchmarks profile verify build/platforms/candidate.json \
  --os-bin "$OS_BIN" --bitstream "$RBF" --loader "$CHIP32_LOADER" \
  --cpu-config "$CPU_CONFIG"
```

Select each actual packaged artifact independently. This package's 199-byte
Chip32 `loader.bin` is a selector and does not contain the OS. The RV32
`firmware.mif` is embedded in the RBF; `os.bin` is a separate packaged image.
Hash verification does not inspect embedding or prove source correspondence,
flashed identity, fit, timing closure or device correctness. A changed build or
corrected declaration needs a new profile. Do not change release pins to fill a
matrix cell. The current release's experimental os25/100 MHz runtime misses its
stated slow-corner timing target; retain that qualification from the build guide.

## Collect and preserve evidence

Record the actual source commit/build ID, ELF/compiler/flags, profile, device ID,
firmware, asset-manifest SHA-256, route/seed/mode, initial state, warmup, sample
roster, audio and pacing. Set `conditions.core_role` to `benchmark` or `gameplay`;
optional `deployment` describes package/core/save identities. Use the harness's
metadata schema rather than inventing missing measurements or identities.

Finish sampling before screenshots or report writes. Quit through the Analogue
menu after each focused or bullet-hell mode to flush save RAM, then mount the
card. Standard logs are `Saves/butterscotch/common/undertale_1.sav` or
`deltarune_1.sav`; focused tiles reports accumulate in `deltarune_tiles_1.sav`.
Each bullet-hell entry reports to `bs_bullet_1.sav` in that same directory:
collect a fresh, private snapshot after the Analogue-menu quit and before
launching the next entry. Preserve the NUL padding in the snapshot. Use
`pocket_card.py collect` with the local mount or the authorized SSH host.
[Card collection commands](../../../.agents/skills/butterscotch-pocket-build/references/sd-card.md#collect-a-benchmark-save)

Use the local [log history check](../../../.agents/skills/butterscotch-pocket-perf/SKILL.md#reading-the-log-off-the-card)
to identify complete report blocks and exact copies across files.

For formats recognized by the shared collector, inspect the chosen file and
explicitly select the intended complete block.

```sh
python3 -m benchmarks inspect "$LOG"
python3 -m benchmarks import --format butterscotch-device \
  --source "$LOG" --metadata "$RUN_METADATA" --report-index "$REPORT_INDEX" \
  --profile build/platforms/candidate.json
python3 -m benchmarks verify build/benchmarks/RUN_ID
```

Use the printed bundle path in place of `RUN_ID`. Bind each import to its known
matching variant and clock; the device's numeric core variant does not identify
os20/os25. Import each supported mode separately with its actual metadata,
seed 7, route-specific section/window roster and completion markers. Use the
local history check for report titles the collector does not recognize.
A complete `FOCUS FAIL` report can be imported and remains failed evidence.
Keep incomplete raw reports privately too. Use actual package provenance for
build attribution. A legacy report without a build label needs independent
provenance.

Keep raw logs, profiles, metadata, screenshots, bundles and selection notes
private under the harness's ignored `build/`; never add game inputs, progress,
Memories or device evidence to a public package. The collector reads explicitly
selected files and archives them locally; it does not deploy or write the card.
Host and simulated evidence stay in separate timing domains. The Butterscotch RTL
adapter remains the pinned os25/100 MHz model; profiles do not extend it.

## Compare and decide

Use `matrix` on explicitly selected bundles/store directories and relevant
registered profiles. Inspect failures, unknown identities and synthetic
exclusions. Counts are imported records, not independent trials; registration
is not device observation, and retained source PASS is not independent validation.
Use repeated paired runs on the same Pocket and fixed scenes/assets/settings.

| Question | Collector command | Required interpretation |
| --- | --- | --- |
| Software regression on one platform | `compare BASELINE CANDIDATE` | Default keeps platform/profile, toolchain, workload and sampling fixed |
| Intentional platform change with the same ELF | `compare BASELINE CANDIDATE --mode platform --vary clock` | Same known application source/ELF, compiler/flags, device, assets and full workload controls; declare every actual factor |
| Different SDK/platform requires another ELF | `compare BASELINE CANDIDATE --mode configuration --reason "Each SDK requires its own ELF."` | Complete working-configuration delta; preserve both identities and all differences |

Prefix those commands with `python3 -m benchmarks` and use printed bundle paths.
Platform factors are `clock`, `variant` and `runtime`; add each actual
`--vary` according to the harness's factor rules. Configuration mode requires a
nonempty actual reason, complete profile-bound device evidence and known
ELF/source/toolchain identities. Device, assets, case/conditions, adapter and
sample/metric rosters still match. Do not weaken identity checks or relabel ELFs
to force a platform comparison. Bound/unbound default comparisons are rejected;
legacy unbound comparisons remain provisional.

Preserve literal units and every source limitation. Multiple changed factors
are a combined experiment; different-ELF configurations cannot isolate a CPU,
cache, clock, OS or compiler effect. Bullet-hell No Render skips ordinary frame
presentation except at the configured draw interval, No Audio disables audio
work, and Core Only combines those counterfactuals rather than isolating a pure
CPU/VM measurement: their overlapping savings are not additive, and none
predicts an ordinary gameplay configuration by subtraction. Profile adds script
instrumentation and changes timing; use its counters for workload attribution
and rankings, never as baseline frame times. Select the best eligible tested
configuration against this project's targets and observed variability, or report
a tie/insufficient evidence. One candidate shows suitability, not superiority
over untested profiles. There is no automatic ranking command.

The [performance skill](../../../.agents/skills/butterscotch-pocket-perf/SKILL.md)
retains scene/counter interpretation. The
[verification skill](../../../.agents/skills/butterscotch-pocket-verify/SKILL.md)
retains both-game deterministic pixels/PCM, scene oracles and relevant launch,
progress-save, audio, Memory and sleep/wake checks. Desktop checks cannot prove
Pocket timing or lifecycle behavior. The [build skill](../../../.agents/skills/butterscotch-pocket-build/SKILL.md)
and [pocket-release](../../../.agents/skills/pocket-release/SKILL.md) retain clean
same-commit builds, matched pins, provenance, complete packaging and explicit
version/publication gates. Collector acceptance replaces none of these checks.
