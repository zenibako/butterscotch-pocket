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
shared [pocket-dev-tools](https://github.com/zenibako/pocket-dev-tools)
checkout, formerly `pocket-harness`, alongside its implementation.

Use a root established in the session, then `POCKET_DEV_TOOLS_ROOT`, then legacy
`POCKET_HARNESS_ROOT`; resolve conflicting explicit roots before choosing.
An adjacent checkout is only a candidate. Verify that it contains
`benchmarks/__main__.py` and `.agents/skills/pocket-benchmarking/SKILL.md`, resolve
the latter's canonical path, then read it and its linked `benchmarks/README.md`,
`PLATFORMS.md` and `SELECTION.md`. Run the commands below from that harness root.
Do not copy the harness skill body here. Missing access limits collection;
applicable local correctness checks can continue. RV32 capture/replay/profiling
routes separately to the harness's canonical `os25-harness` skill.

## Routine regression checks

Use the [shared preserved-baseline contract](https://github.com/zenibako/pocket-dev-tools/blob/fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58/benchmarks/REGRESSIONS.md)
(introduced in shared commit `fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58`) from the verified
`pocket-dev-tools` checkout. Require its `benchmarks/REGRESSIONS.md` and record the
actual shared commit; do not silently substitute an older checkout. Keep private
source exports, accepted evidence, indices and candidate/failure records under
that checkout's ignored directories. Do not ship them in this project's releases.

For supported Butterscotch os25 sources, follow shared `docs/REGRESSION.md` and
`python3 regression.py --help`. Establish a verified reference once from an
explicit accepted source revision. The standard roster is Undertale Flowey and
Deltarune field/menu/dodge, each with trace and timing builds (eight cases).
Use the central guide's fixed-audio/configuration controls; a declared subset
covers only that subset. Every registered case needs repeat QEMU agreement,
positive RTL replay and a rejected corrupted oracle. Demonstrate separately that
a known bad candidate fails the cross-build comparison, not only its own replay.

From the shared checkout, use actual printed CASE paths and an unused name:

```sh
python3 regression.py baseline --name accepted-reference --traffic quiet \
  --cases BASELINE_CASE_1 BASELINE_CASE_2
python3 regression.py check --baseline BASELINE_INDEX \
  --repo /absolute/path/to/butterscotch-pocket --revision CANDIDATE_REVISION --jobs 1
```

Replace the two example paths with the complete selected case roster; use the
printed BASELINE path. Git export reads committed objects, not uncommitted port
edits. The gate creates fresh independent candidates, validates the exact roster,
compiler/ordered flags/tooling/platform and negative evidence, and compares raw
pixels/geometry plus fixed PCM/counts. It never rebuilds or promotes the reference.
Output mismatch exits 2 and keeps both images; stale/incomplete/incompatible or
failed evidence exits nonzero. Preserve those failures and the index digest.
Changed tooling or compiler needs newly verified cases and a new baseline name.

These timings are simulated, with no automatic slowdown threshold. A routine
pass supplements the desktop both-game and scene-specific checks; it does not
qualify shipping runtime/API/allocator/storage behavior or actual SD, audio,
progress saves, Memories or sleep/wake. Keep the applicable physical Pocket and
release gates below. The collector's `verify`/numeric `compare` is archival work,
not this deterministic output gate.

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
progress, result slots and Memories identity. The current focused package adds
diagnostic launch entries under the gameplay core; it is not a dedicated core.
Preserve its existing save isolation. Match relevant production paths and record
instrumentation, renderer, audio and pacing differences. Do not overwrite shared
runtime/assets while constructing side-by-side packages.

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
menu after each focused mode to flush save RAM, then mount the card. Standard
logs are `Saves/butterscotch/common/undertale_1.sav` or `deltarune_1.sav`;
focused tiles reports accumulate in `deltarune_tiles_1.sav`. Preserve the original
NUL-padded file; text ends at the first NUL. Old/duplicate reports can remain.
Inspect the chosen file and explicitly select the intended complete block:

```sh
python3 -m benchmarks inspect "$LOG"
python3 -m benchmarks import --format butterscotch-device \
  --source "$LOG" --metadata "$RUN_METADATA" --report-index "$REPORT_INDEX" \
  --profile build/platforms/candidate.json
python3 -m benchmarks verify build/benchmarks/RUN_ID
```

Use the printed bundle path in place of `RUN_ID`. Bind each import to its known
matching variant and clock; the device's numeric core variant does not identify
os20/os25. Import each focused mode separately with its actual metadata and the
route's seed 7, seven windows and completion markers. A complete `FOCUS FAIL`
report can be imported and remains failed evidence. Keep incomplete raw reports
privately too; the collector refuses incomplete imports. Standard logs lack an
embedded source commit and independent correctness oracle; their build
attribution remains declared.

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
cache, clock, OS or compiler effect. Overlapping counters and counterfactual
no-render/no-audio modes are not additive. Select the best eligible tested
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
