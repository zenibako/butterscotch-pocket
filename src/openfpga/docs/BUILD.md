# Building and releasing

The runner, Pocket platform, data tools and release pipeline share the
`zenibako/butterscotch-pocket` upstream fork. The Pocket integration lives in
`src/openfpga`; commands below run from the repository root unless stated
otherwise. The earlier [split design](https://github.com/zenibako/butterscotch-pocket-legacy/issues/15)
and [split implementation plan](https://github.com/zenibako/butterscotch-pocket-legacy/issues/16)
are historical: the approved consolidation replaces their separate core
checkout and self commit pin with one clean checkout.
The [game-data guide](../GAME-DATA.md) defines the
supported inputs and preparation contract. The plugin (v0.7.1 or newer) reads
originals under `Assets/butterscotch/common/undertale/` or `deltarune/`,
preserving native filenames and source layouts. It installs the selected game's
three converted files directly into `common/` and offers keep (default) or
discard of the entire selected source folder only after success. The other
game, installed game files and saves remain untouched. Work/recovery paths are
hidden under `Assets/butterscotch/common/.butterscotch-install-*`. CLI preparation
retains its separate-output/refuse-existing contract.
The v0.7.0 plugin predates direct installation.

For diagnostic packages, platform profile selection and benchmark evidence,
follow [Pocket benchmark integration](BENCHMARKING.md). Benchmark evidence
supplements, but does not replace, the matched-input, clean-source, correctness
and complete-release gates below.

## Dependency lock

[`release/core.lock.json`](../release/core.lock.json) is the release input
contract. Its schema 2 selects the current clean repository checkout for runner and
tools, and pins external SDK/runtime commits and SHA-256 hashes. Branches and
latest downloads are not external build inputs.

| Input | Pinned source |
|---|---|
| Runner / Pocket platform / tools | Current clean `zenibako/butterscotch-pocket` checkout; actual full commit recorded in provenance |
| SDK base | `openfpgaOS/openfpgaSDK` at `a408ddc12aed0dfaa4aa22c06af82f829db77126` |
| Matched runtime and API source | `zenibako/openfpgaCore` at `902a8adc11309c50c8954af2864ddf893c8b1b0c` |
| Core compiler | GCC 13.2.0, Ubuntu 24.04 package `gcc-riscv64-unknown-elf=13.2.0-11ubuntu1+12` |

The checked-in `release/sdk.tar.gz` is the SDK snapshot used by this pipeline,
with the matching runtime's API headers. `release/runtime/` contains the
matched `os.bin`, `os25.rbf_r`, `loader.bin` and `SAVESTATES_RUNTIME.json`.
The lock covers every snapshot/runtime input; validation fails on any hash
or runtime provenance mismatch. Historical SDK trees remain in the preserved release history and original
repository; they are not active build inputs in this fork.

This is the previously tested experimental os25 sleep/clock runtime at
100 MHz, with -0.777 ns worst setup slack in the slow 85°C corner. It misses
that timing target. Do not describe it as a timing-clean FPGA build or swap
in another runtime merely because it is newer. Local/private and public ELFs
use the same metadata-and-bytecode Memory identity; local game files do not
alter it. Use matching game data and a compatible snapshot format. Changes
outside metadata and bytecode are not whole-file verified. Older private-build
Memories with a compile-time fingerprint are not migrated. Ordinary game
progress saves are separate.

To update dependencies, review the new core/SDK/API/runtime combination,
record complete revisions and hashes, rebuild both game programs and repeat
hardware compatibility checks before publishing. Keep the matching API,
firmware, loader and bitstream together. The provenance reports record the
actual source commit, external inputs and every packaged file hash.

## Update runner or tools

Commit runner, Pocket platform, tools and release changes together in this
repository. There is no separate core pin or `update_core.py` workflow.
`verify_source` requires the source argument to resolve to this pipeline's
repository root, rejects tracked or untracked changes, and records its actual
HEAD. Ignored `src/openfpga/out/` output does not dirty the checkout.

The complete distribution workflow builds both core programs and all tools
from the same commit. `PROGRAM-PROVENANCE.json` binds compiled programs to that
source before packaging; `CORE-PROVENANCE.json` and `TOOLS-PROVENANCE.json`
record the source identity used for the release. Assembly rejects artifacts
from another commit, checks tool artifact hashes, and verifies the core's
complete ZIP manifest, every entry hash and current dependency pins. Core
metadata/version, static files and runtime bytes must match the current
checkout and locked dependency set. External SDK/runtime
changes still require matched dependency review and hardware checks.

The repository's [`pocket-release` skill](../../../.agents/skills/pocket-release/SKILL.md),
also shared with Claude through a symlink, coordinates validation and explicit
versioned draft releases. Stable publication remains a separate maintainer
action.

## Tools and tests

Use Go 1.25 or newer, TinyGo 0.43.0 and Binaryen 133 (`wasm-opt` on PATH).
Python 3.12+ drives release assembly; the installed Go CLI/plugin needs no
Python or C toolchain. C reference tests additionally require a host C
compiler and Python. Use a clean checkout of this repository:

```sh
git clone https://github.com/zenibako/openfpgaCore.git src/openfpga/out/runtime-source
git -C src/openfpga/out/runtime-source checkout --detach 902a8adc11309c50c8954af2864ddf893c8b1b0c
python3 src/openfpga/release/pipeline.py validate
python3 -m unittest discover -s src/openfpga/release -p 'test_*.py'

(cd src/openfpga/tools/pocket-data && go test ./... && go vet ./... && go test -C third_party/vorbis ./...)
(cd src/openfpga/tools/pocket-data && BUTTERSCOTCH_SOURCE="$PWD/../../../.." REQUIRE_REFERENCE_TESTS=1 REQUIRE_PLUGIN_TESTS=1 go test ./... -count=1)
```

Run these commands from this repository root. Plugin integration tests use
TinyGo from PATH or `TINYGO`; `POCKET_PLUGIN_WASM` can select an already-built
plugin. Native, WASI and Extism tests use synthetic fixtures. Independent C
comparisons use the fork's retained Python/C converters. These checks do not
replace on-device testing with the user's own supported game installation.

## Assemble a complete release

Core compilation requires the pinned RISC-V GCC on Ubuntu 24.04. Build output defaults to the ignored
`src/openfpga/out/` directory; keep custom output directories ignored or outside
the checkout so source verification stays meaningful. No purchased game files are
needed: both Undertale and Deltarune programs are compiled without source
game data or a private game fingerprint.

```sh
python3 src/openfpga/release/pipeline.py build-core --out src/openfpga/out/build
python3 src/openfpga/release/pipeline.py package-core --programs src/openfpga/out/build/programs --version vX.Y.Z
python3 src/openfpga/release/pipeline.py build-tools
python3 src/openfpga/release/pipeline.py assemble-release
```

Replace `vX.Y.Z` with the intended release version. Use a fresh artifact output
folder for each release. `assemble-release` requires the complete expected
asset set and refuses missing or unexpected files before writing `SHA256SUMS`.
It also includes source archives and license/provenance records. The core
source archive starts at the repository root. The tools source archive keeps
the complete nested Go module (including future packages), Pocket docs,
release inputs and notices at their repository paths. `licenses.tar.gz` retains
`src/openfpga/release/NOTICE`, the historical SDK license notices and all tool
decoder/runtime license files. Native bundle notices and the license archive
read only tracked HEAD content, excluding ignored local logs even in a clean
checkout. Runtime source archives require the locked repository root so root
notices cannot be omitted by selecting a subdirectory. Review the
archive contents and test installation before publishing a stable release.
The GitHub workflow `.github/workflows/release-pocket.yml` defines the
complete pipeline and is reused by the main CI's Pocket tools domain. Tagged
builds validate artifacts; reusable validation has read-only repository
permission. Draft creation uses **Prepare complete Pocket draft release**
(`.github/workflows/publish-pocket.yml`) with an explicit `vMAJOR.MINOR.PATCH`
version and `create_draft=true`. That workflow reuses the complete distribution
on the same commit/version before its separate publishing job obtains write
permission. Regular pushes and workflow calls only validate artifacts.

Commit the version's release notes at `src/openfpga/docs/release-notes/vX.Y.Z.md` before
starting its release build. Draft creation uses that file as the GitHub release
body and refuses missing or empty notes. Include installation/Memory compatibility,
known issues, and the distinction between automated and physical Pocket checks.

Every release, including tool-only updates, must carry:

- `butterscotch-pocket.zip`: the installable core tree with both game programs.
- `plugin.wasm`, `plugin.json` and `plugin_icon.png`: the Pocket Sync plugin
  and its paper/heart icon. The manifest's `logo_url` uses the latest release's
  icon asset.
- `prepare-data-{darwin,linux,windows}-{amd64,arm64}.tar.gz`: six native bundles.
- `prepare-data.wasm`: standard-Go WASI CLI, separate from the Extism plugin.
- `CORE-PROVENANCE.json`, `TOOLS-PROVENANCE.json`, `core.lock.json`, `RUNTIME-SUBMODULES.json` and `SHA256SUMS`.
- `butterscotch-core-source.tar.gz`, `pocket-tools-source.tar.gz`, `openfpga-runtime-source.tar.gz` and `licenses.tar.gz`.
- `GAME-DATA.md`: supported input and installation instructions.

The core package uses an explicit APF allowlist: `Cores`, `Assets` and
`Platforms` metadata, programs and matched runtime. It excludes commercial
inputs/packs, saves, benchmark instances, local logs and device metadata.
The ZIP installs both game entries, but game data remains a separate user
step. Never add a local staging folder to release assets.

## Distribution contract

### Advertised core capabilities

Release assembly owns the published description, hardware flags and
[`info.txt`](../release/core-info.txt), rather than inheriting generic SDK
defaults. The information page fits Analogue's 32-line limit.

| Metadata | Audited value and evidence |
|---|---|
| Cartridges | `cartridge_adapter: -1`: no cartridge-game support; leaves cartridge power off. |
| Link port | `link_port: false`: pinned runtime `variants/os25.mk` omits `INCLUDE_LINK`; `core_top.v` disables the link peripheral. Debug UART is separate. |
| Dock | Digital Dock support remains enabled; runtime handles Dock notifications and controllers. This audit did not physically test a Dock. |
| Analog Dock | Disabled. Runtime Analogizer support is a separate external interface and does not establish analog Dock support. |
| Save states / sleep | Enabled by the matched os25 runtime and both game builds; experimental and subject to scene eligibility. Check **Select > Debug > Save-state status** before sleeping. |
| Firmware minimum | Retains `2.2`; the exact minimum has not been independently established by a firmware compatibility matrix. |
| Games | Undertale **v1.08**, experimental Deltarune **Chapter 1**; user-owned game data required. |

Core/platform IDs, author attribution, slot IDs and save addresses remain
unchanged. Input labels match the pinned runner's controls; extra generic
loader slot extensions are runtime file filters, not claims of support for
additional games. Release assembly supplies its version, repository URL and
source-commit date in UTC. It retains the pinned scaler modes, loader and
runtime filenames.

See Analogue's [core metadata contract](https://www.analogue.co/developer/docs/openfpga/core-definition-files/core-json)
and [information-page limit](https://www.analogue.co/developer/docs/openfpga/core-definition-files).

### AI disclosure

Release assembly installs `release/updaters.json` at
`Cores/zenibako.Butterscotch/updaters.json`. Its `declare_ai` list names the
AI tools used in the maintained Pocket integration and companion tooling.
The README explains scope and inherited sources. Do not invent a numeric
AI-authorship percentage: Pocket Sync obtains a heuristic score from the
separate openfpga-ai-check report service, which currently does not calculate
its score from this declaration alone.

### Catalog and plugin installation

The openFPGA catalog should point at `zenibako/butterscotch-pocket` and filter
assets with `^butterscotch-pocket\.zip$`. The catalog listing requires a
separate maintainer submission. Pocket Sync's plugin installer uses the same
repository URL and downloads the two plugin assets from its latest stable
release. Drafts and prereleases are not a substitute for that stable release.

Keep the full asset set in every latest stable release so either installer
continues to work after an update. Verify the generated checksums, source
provenance, license notices and installation instructions before making a
release public. Ordinary saves must never be bundled or replaced.

## Runtime source and rebuilding the FPGA

`openfpga-runtime-source.tar.gz` contains the locked runtime repository tree,
including its notices and licenses. Git archive does not embed submodule
working trees: `RUNTIME-SUBMODULES.json` records the top-level gitlinks.
Retrieve their exact nested revisions through the pinned runtime checkout:

```sh
git -C src/openfpga/out/runtime-source -c url.https://github.com/.insteadOf=git@github.com: submodule update --init --recursive
```

The runtime source commit pins VexiiRiscv at
`580b76c3868512c8316bb7a3d3add81cad49a0dc`; that repository pins its own
nested dependencies. See the runtime repository build instructions for
Quartus 25.1 and CPU generation. This release pipeline rebuilds the game
programs, not the FPGA. It reuses the complete previously built runtime set,
whose source, hashes and timing limitation are recorded in the lock.
The SDK archive includes the exported API sources and prebuilt musl 1.2.5
libraries; musl source is available from its official release archive at
https://musl.libc.org/releases/musl-1.2.5.tar.gz and its copyright notice is
in `release/licenses/musl.txt`.
