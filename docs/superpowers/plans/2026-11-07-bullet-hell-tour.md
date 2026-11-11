# Asriel Bullet-Hell Tour Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a separate, scene-validated Undertale benchmark that measures an Asgore control and two real Asriel projectile-heavy attack windows in Accuracy and Speed modes without changing the existing six-battle tour.

**Architecture:** Extend the existing benchmark definition with a third tour and optional per-section scene contracts. Drive Asriel through the existing numeric-global/object-creation jump mechanism, using battlegroup 256 and repeated HP replenishment; no story-state rewrite or runner optimization is part of this work. Add an additive packager for a separate os25 benchmark core with isolated save/log names and diagnostic launch entries.

**Tech Stack:** C and the Butterscotch runner/platform APIs, asset-free C fixtures, Python 3 standard-library packaging/tests, GNU Make, SDL2 desktop captures, pinned RISC-V GCC 13.2.0/os25 runtime.

**Spec:** `src/openfpga/docs/BENCHMARKING.md` plus the user-approved 2026-11-07 design: preserve the existing tour, measure a known Asgore control and two real Asriel attack windows, validate expected scenes, provide normal/no-render/no-audio/profile diagnostics, and leave physical-Pocket timing as a separate user-assisted step.

## Global Constraints

- Start from `c13ff8f6` on branch `perf/bullet-hell-tour`; PR #90 is already merged and must not be reimplemented.
- Accuracy is the primary target and 33.3 ms/frame is full speed. Speed is a separate report, never a substitute for Accuracy.
- Preserve `--bench-battles` and `--bench-battles-smooth` behavior and section boundaries.
- Use seed 7, uncapped frames, volatile saves, and the matched os25/100 MHz runtime from `release/core.lock.json`.
- Use real Asriel battle setup through `obj_battleblcon` and battlegroup 256. Do not benchmark only menus, dialogue, white transitions, game-over screens, or an opening turn with no projectile workload.
- Every Asriel measured section requires `room_battle` plus an observed attack object. A failed contract must print `BENCH FAIL` and halt instead of emitting a passing report.
- Use two complete 150-frame attack turns. Desktop discovery and Asriel's `global.turntimer = 150` establish that shorter natural turns are correct; never pad a section with menus, dialogue, transitions, or game-over frames to reach 300 frames. Setup and later-turn sections may provide context but do not count as attack windows.
- Keep private game data, captures, logs, packages, and device evidence out of Git and public packages. The local private asset source is `../butterscotch-pr20-fixes/src/openfpga/games/undertale` relative to this worktree.
- Do not change renderer, VM, audio, openfpgaCore, or openfpgaOS performance behavior in this plan.
- Desktop checks establish route correctness and deterministic output only. They do not establish Pocket timing, memory limits, SD behavior, or physical device correctness.

---

### Task 1: Scene contracts and the third-tour interface

**Files:**
- Create: `src/openfpga/platform/of_bench_scene.h`
- Create: `src/openfpga/platform/of_bench_scene.c`
- Create: `src/openfpga/tests/bench-scene.c`
- Modify: `src/openfpga/platform/of_game.h:40-91`
- Modify: `src/openfpga/platform/of_bench.h:16-22`
- Modify: `src/openfpga/platform/of_bench.c:17-73,202-225`
- Modify: `src/openfpga/platform/of_platform.c:665-728`
- Modify: `src/openfpga/main.c:123-131`
- Modify: `src/openfpga/Makefile:93-99,439-447`
- Include: `docs/superpowers/plans/2026-11-07-bullet-hell-tour.md`

**Interfaces:**
- Produces: `bool ofBenchSceneMatches(const Runner *runner, const OfBenchSection *section);`
- Produces: `bool ofBenchStartBulletTour(void);`
- Extends: `OfBenchSection` with optional `roomName` and `requiredObject` strings; NULL means no requirement.
- Extends: `OfGame` with `bulletTitle`, `bulletScript`, `bulletJumps`, `bulletJumpCount`, `bulletSections`, and `bulletSectionCount` following the existing tour fields.
- Changes: `ofBenchFrame(Runner *runner)` so each presented frame can mark the current section's scene contract as observed.

- [ ] **Step 1: Write the failing asset-free scene fixture**

Use a `Runner` with a named current room and a small `instances` array. Cover NULL requirements, wrong room, active matching object, inactive object, destroyed object, missing object, and out-of-range object indices. The central assertions are:

```c
OfBenchSection section = { "attack", 900, "atk1", "room_battle", "obj_star" };
assert(ofBenchSceneMatches(&runner, &section));
instances[0]->active = false;
assert(!ofBenchSceneMatches(&runner, &section));
section.roomName = "room_gameover";
assert(!ofBenchSceneMatches(&runner, &section));
section.requiredObject = NULL;
section.roomName = NULL;
assert(ofBenchSceneMatches(NULL, &section));
```

- [ ] **Step 2: Build the fixture and verify the missing interface fails**

Run: `make -C src/openfpga bench-scene-fixture`

Expected: compilation fails because `of_bench_scene.h` or `ofBenchSceneMatches` does not exist.

- [ ] **Step 3: Implement the matcher and third-tour plumbing**

The matcher scans only live instances and compares against `dataWin->objt.objects[instance->objectIndex].name`:

```c
bool ofBenchSceneMatches(const Runner *runner, const OfBenchSection *section) {
    if (section->roomName == NULL && section->requiredObject == NULL) return true;
    if (runner == NULL || runner->currentRoom == NULL || runner->dataWin == NULL) return false;
    if (section->roomName != NULL &&
        (runner->currentRoom->name == NULL || strcmp(runner->currentRoom->name, section->roomName) != 0)) return false;
    if (section->requiredObject == NULL) return true;
    for (ptrdiff_t i = 0; i < arrlen(runner->instances); ++i) {
        const Instance *instance = runner->instances[i];
        if (instance == NULL || !instance->active || instance->destroyed || instance->objectIndex < 0 ||
            (uint32_t) instance->objectIndex >= runner->dataWin->objt.count) continue;
        const char *name = runner->dataWin->objt.objects[instance->objectIndex].name;
        if (name != NULL && strcmp(name, section->requiredObject) == 0) return true;
    }
    return false;
}
```

Add one `g_sectionSeen[OF_BENCH_MAX_SECTIONS]` flag array. Reset it in `benchStart`, set it when `ofBenchSceneMatches` succeeds, and at each section boundary reject an unmet contract:

```c
if ((section->roomName != NULL || section->requiredObject != NULL) && !g_sectionSeen[g_section]) {
    ofLogPrint("BENCH FAIL %s: required scene not observed\n", section->name);
    ofDiagHalt("benchmark scene check failed");
}
```

Map `--bench-bullet-hell` to Accuracy and `--bench-bullet-hell-smooth` to Speed using the same seed and `ofPlatformSetSmoothLowres` pattern as the existing tour. Return false with a warning when a game has no bullet definition. Update both `ofBenchFrame` call sites to pass `g_runner`.

- [ ] **Step 4: Run the fixture and existing baselines**

Run:

```sh
src/openfpga/undertale_pc_bench_scene_fixture
src/openfpga/undertale_pc_sprite_clipping_fixture
src/openfpga/undertale_pc_solid_fill_fixture
src/openfpga/undertale_pc_orange_particle_fixture ../butterscotch-pr20-fixes/src/openfpga/games/undertale/data.win
make -C src/openfpga lint
```

Expected: scene assertions and all existing fixtures pass; lint prints `lint: clean`.

- [ ] **Step 5: Commit the independently testable interface**

```sh
git add docs/superpowers/plans/2026-11-07-bullet-hell-tour.md src/openfpga
agent-commit "feat(bench): add scene-checked bullet tour interface"
git log -1 --show-signature --format=fuller
```

Expected: a signed conventional commit with the automatically supplied AI co-author trailer.

### Task 2: Asgore control and two validated Asriel attack windows

**Files:**
- Modify: `src/openfpga/platform/game_undertale.c:159-252,290-317`
- Modify: `src/openfpga/README.md:357-373,421-424`
- Private evidence only: `src/openfpga/out/bullet-route/`

**Interfaces:**
- Consumes: the third-tour fields and `OfBenchSection.roomName` / `requiredObject` from Task 1.
- Produces: `g_bulletScript`, `g_bulletJumps`, and `g_bulletSections` registered in `ofGame` as the bullet tour.

- [ ] **Step 1: Establish the private real-data desktop route**

Link the existing private assets into the gitignored location and rebuild:

```sh
mkdir -p src/openfpga/games
ln -sfn "$(cd ../butterscotch-pr20-fixes/src/openfpga/games/undertale && pwd -P)" src/openfpga/games/undertale
make -C src/openfpga undertale_pc
mkdir -p src/openfpga/out/bullet-route
```

Start with the existing Ruins encounter sequence. Keep one 600-frame Asgore control using battlegroup 101 and the existing forced-win transition. Enter Asriel from the Ruins with:

```c
{ 1700, -1, "battlegroup=256,border=0,mercy=0,hp=99,maxhp=99,encounter=0,flag[500]=1,flag[501]=0,flag[505]=0,flag[506]=0,flag[507]=0,flag[508]=0", "obj_battleblcon" },
```

Replenish `hp=99` and `maxhp=99` at fixed 100-frame offsets through the discovery run. Use confirm presses to advance turns; directional holds must not leave ACT/ITEM/MERCY selected at menu boundaries. Do not force-clear Asriel at the end of the benchmark.

- [ ] **Step 2: Capture a long discovery run and select two nonoverlapping windows**

Run the Accuracy desktop route with `BS_FRAME_LOG=src/openfpga/out/bullet-route/frames.log`, `BS_DUMP_EVERY=100`, and `BS_DUMP_DIR=src/openfpga/out/bullet-route/frames`. Exclude frames whose room is not `room_battle` and frames showing menus, dialogue, game-over, or white transitions.

For each remaining 100-frame block, rank the sum of sprite, sprite-part, and rectangle calls in `frames.log`, then inspect the corresponding PPMs. Choose two distinct 150-frame windows with dense projectile rendering and no menu/dialogue/game-over contamination. Set `START`, `MID`, and `END` to each window's three probe frame numbers and run fresh same-seed captures with `BS_OBJECTS="$PROBE_FRAME"` in a loop over those three values; an object need only be observed once during the final section, but all three probes must remain in the intended attack sequence.

Encode the selected windows as separate `OfBenchSection` entries, each with `roomName="room_battle"` and the observed projectile controller/object in `requiredObject`. Keep the Asgore control as a regression comparison, not as evidence about Asriel.

- [ ] **Step 3: Add a deliberate negative scene control**

Temporarily change one Asriel `requiredObject` to `"obj_not_in_this_attack"` and rerun the desktop bullet tour.

Expected: the log contains `BENCH FAIL` and `benchmark scene check failed`, with no passing final report. Restore the observed object before continuing.

- [ ] **Step 4: Verify deterministic route behavior and the unchanged old tour**

Run both:

```sh
(cd src/openfpga/games/undertale && BS_NOFLIP=1 BS_UNCAPPED=1 BS_SEED=7 ../../undertale_pc --bench-bullet-hell)
(cd src/openfpga/games/undertale && BS_NOFLIP=1 BS_UNCAPPED=1 BS_SEED=7 BS_SMOOTH=1 ../../undertale_pc --bench-bullet-hell-smooth)
```

Expected: both complete with scene contracts satisfied, and repeated same-mode start/mid/end captures compare `identical` with `ppmtool.py cmp`. Also run the no-render, muted, and script-profile variants:

```sh
../../undertale_pc --bench-bullet-hell --draw-every 100000
../../undertale_pc --bench-bullet-hell --mute
../../undertale_pc --bench-bullet-hell --scripts
```

Expected: the scene contracts still pass; profile totals are explicitly treated as rankings, not baseline timings.

Capture the same seed-7 frames from `--bench-battles` before and after this task in Accuracy and Speed and compare with `ppmtool.py cmp`. Expected: every sampled old-tour frame is identical and its section names/boundaries are unchanged.

- [ ] **Step 5: Update usage documentation and commit the validated route**

Document the two new flags, the Asgore/Asriel scope, volatile saves, diagnostic flags, and the fact that desktop runs do not prove Pocket timing. Do not put measured frame times in the README.

```sh
git add src/openfpga/main.c src/openfpga/platform src/openfpga/tests src/openfpga/Makefile src/openfpga/README.md
agent-commit "feat(bench): cover Asriel bullet-hell attacks"
git log -1 --show-signature --format=fuller
```

### Task 3: Additive benchmark-core packaging and release verification

**Files:**
- Create: `src/openfpga/tools/mkbullet.py`
- Create: `src/openfpga/tests/test_bullet_package.py`
- Modify: `src/openfpga/docs/BENCHMARKING.md:30-46,73-110`
- Modify: `src/openfpga/README.md` benchmark-entry documentation
- Private output only: `artifacts/bullet-hell-$(git rev-parse --short HEAD)/`

**Interfaces:**
- Produces: `mkbullet.package(elf: Path, output: Path, version: str) -> None`
- Uses only tracked templates under `src/openfpga/dist` and matched runtime files under `src/openfpga/release/runtime`.
- Produces core ID `zenibako.BsBullet`, ELF/save prefix `bs_bullet`, and log path `Saves/butterscotch/common/bs_bullet_1.sav`.

- [ ] **Step 1: Write failing package tests**

Use a synthetic `b'\x7fELF...'` fixture and assert:

```python
self.assertEqual(core['core']['metadata']['shortname'], 'BsBullet')
self.assertFalse(core['core']['framework']['sleep_supported'])
self.assertEqual(slots[3], 'bs_bullet.elf')
self.assertEqual(slots[10], 'bs_bullet_0.sav')
self.assertEqual(slots[11], 'bs_bullet_1.sav')
self.assertFalse(any(name.endswith(('.sav', '.win', 'music.bin', 'textures.bin'))
                     for name in archive.namelist()))
```

Verify six unique instances with these exact argument sets:

```text
Accuracy:  --bench-bullet-hell
Speed:     --bench-bullet-hell-smooth
No Render: --bench-bullet-hell --draw-every 100000
No Audio:  --bench-bullet-hell --mute
Core Only: --bench-bullet-hell --draw-every 100000 --mute
Profile:   --bench-bullet-hell --scripts
```

Also assert os25 runtime/loader presence, source-template checksums unchanged, no duplicate paths, ZIP readback equality, and rejection of a non-ELF before creating output.

- [ ] **Step 2: Run the tests and confirm the packager is missing**

Run: `python3 -m unittest src/openfpga/tests/test_bullet_package.py -v`

Expected: import failure for `tools/mkbullet.py`.

- [ ] **Step 3: Implement the additive packager**

Follow `mkfocus.py`'s deterministic ZIP/readback pattern and the existing private battle packager's separate-core layout. Copy tracked core/platform templates, rename only the benchmark core metadata, install the matched `os.bin` as `bs_bullet_os.bin`, preserve slots 4–7, and remap slots 1–3 and 10–19 to the isolated `bs_bullet` names. Refuse existing output files and assert all instance filenames fit the APF 23-byte slot limit.

- [ ] **Step 4: Run package tests and update maintained instructions**

Run:

```sh
python3 -m unittest src/openfpga/tests/test_bullet_package.py src/openfpga/tests/test_focused_package.py -v
make -C src/openfpga lint
```

Expected: both package suites pass and lint remains clean. Document the six entries, report path, required Analogue-menu quit after each mode, and interpretation limits for no-render/no-audio/profile counters.

- [ ] **Step 5: Commit, build the pinned ELF, and package it**

```sh
git add src/openfpga/tools/mkbullet.py src/openfpga/tests/test_bullet_package.py \
  src/openfpga/docs/BENCHMARKING.md src/openfpga/README.md
agent-commit "test(bench): package isolated bullet-hell core"
python3 src/openfpga/release/pipeline.py validate
if pgrep -f 'rsync --server' >/dev/null; then
  echo 'Refusing to rebuild while an rsync server is active' >&2
  exit 1
fi
source ../env.sh
make -C src/openfpga GAME=undertale APP=bs_bullet SAVESTATES=1 FOCUS_BENCH=0 \
  ROOT="$PWD/src/openfpga/out/bullet-build" all
VERSION_WITH_V="$(python3 - <<'PY'
import importlib.util
from pathlib import Path
root = Path.cwd()
spec = importlib.util.spec_from_file_location('release_pipeline', root / 'src/openfpga/release/pipeline.py')
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)
print(pipeline.test_version(root))
PY
)"
python3 src/openfpga/tools/mkbullet.py \
  --elf src/openfpga/out/bullet-build/.obj/bs_bullet/app.elf \
  --version "${VERSION_WITH_V#v}" \
  --output "../artifacts/bullet-hell-$(git rev-parse --short HEAD)/bullet-hell.zip"
```

Expected: validation and build succeed, no rsync process is active during the build, and the package tests pass against the real ELF in addition to the synthetic fixture. Record source commit, compiler, runtime hashes, ELF hash, package hash, and flags in a private `BUILD-PROVENANCE.json` beside the ZIP. Do not deploy to an SD card or claim device results.

- [ ] **Step 6: Prepare the physical-Pocket handoff**

Report the exact six launch entries and request repeated paired Accuracy and Speed runs, followed by No Render, No Audio, Core Only, and Profile if attribution is needed. Ask for `bs_bullet_1.sav` after quitting each entry through the Analogue menu. State explicitly that route correctness, package integrity, and desktop determinism are verified locally, while Pocket timing, memory pressure, SD behavior, and hardware audio behavior remain unverified.
