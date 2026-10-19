# Building and releasing

The approved [repository split design](superpowers/specs/2026-10-18-install-entrypoint-design.md)
and [implementation plan](superpowers/plans/2026-10-18-install-entrypoint.md)
keep the runner in the fork while this repository owns installation, tools
and release packaging. The [game-data guide](../GAME-DATA.md) defines the
supported inputs and preparation contract. The next plugin release reads
originals under `Assets/butterscotch/common/undertale/` or `deltarune/`,
preserving native filenames and source layouts. It installs the selected game's
three converted files directly into `common/` and offers keep (default) or
discard of the entire selected source folder only after success. The other
game, installed game files and saves remain untouched. Work/recovery paths are
hidden under `Assets/butterscotch/common/.butterscotch-install-*`. CLI preparation
retains its separate-output/refuse-existing contract.
The stable v0.7.0 plugin predates direct installation; publish a new complete
release before presenting this workflow as available through Pocket Sync.

## Dependency lock

[`release/core.lock.json`](../release/core.lock.json) is the release input
contract. It pins full immutable commits and SHA-256 hashes; branches and
latest downloads are not build inputs.

| Input | Pinned source |
|---|---|
| Runner / Pocket platform | `zenibako/Butterscotch` at `f6261c51af5d2ed41719e7288fcb318a2918a433` |
| SDK base | `openfpgaOS/openfpgaSDK` at `a408ddc12aed0dfaa4aa22c06af82f829db77126` |
| Matched runtime and API source | `zenibako/openfpgaCore` at `902a8adc11309c50c8954af2864ddf893c8b1b0c` |
| Core compiler | GCC 13.2.0, Ubuntu 24.04 package `gcc-riscv64-unknown-elf=13.2.0-11ubuntu1+12` |

The checked-in `release/sdk.tar.gz` is the SDK snapshot used by this pipeline,
with the matching runtime's API headers. `release/runtime/` contains the
matched `os.bin`, `os25.rbf_r`, `loader.bin` and `SAVESTATES_RUNTIME.json`.
The lock covers every snapshot/runtime input; validation fails on any hash
or runtime provenance mismatch. Historical SDK trees elsewhere in this
repository are retained for history and do not supply the current build.

This is the previously tested experimental os25 sleep/clock runtime at
100 MHz, with -0.777 ns worst setup slack in the slow 85°C corner. It misses
that timing target. Do not describe it as a timing-clean FPGA build or swap
in another runtime merely because it is newer. Public ELFs omit private
compile-time game fingerprints; Memories from private test ELFs may not load
because identities differ. Ordinary game progress saves are separate.

To update dependencies, review the new core/SDK/API/runtime combination,
record complete revisions and hashes, rebuild both game programs and repeat
hardware compatibility checks before publishing. Keep the matching API,
firmware, loader and bitstream together. The provenance report records the
inputs and every packaged file hash.

## Tools and tests

Use Go 1.25 or newer, TinyGo 0.43.0 and Binaryen 133 (`wasm-opt` on PATH).
Python 3.12+ drives release assembly; the installed Go CLI/plugin needs no
Python or C toolchain. C reference tests additionally require a host C
compiler and Python. Use a clean dedicated checkout of the pinned fork:

```sh
git clone https://github.com/zenibako/Butterscotch.git out/core
git -C out/core checkout --detach f6261c51af5d2ed41719e7288fcb318a2918a433
git clone https://github.com/zenibako/openfpgaCore.git out/runtime-source
git -C out/runtime-source checkout --detach 902a8adc11309c50c8954af2864ddf893c8b1b0c
python3 release/pipeline.py validate

go test ./...
go vet ./...
go test -C third_party/vorbis ./...
BUTTERSCOTCH_SOURCE="$PWD/out/core" REQUIRE_REFERENCE_TESTS=1 REQUIRE_PLUGIN_TESTS=1 go test ./... -count=1
python3 -m unittest discover -s release -p 'test_*.py'
```

Run these commands from this repository root. Plugin integration tests use
TinyGo from PATH or `TINYGO`; `POCKET_PLUGIN_WASM` can select an already-built
plugin. Native, WASI and Extism tests use synthetic fixtures. Independent C
comparisons use the fork's retained Python/C converters. These checks do not
replace on-device testing with the user's own supported game installation.

## Assemble a complete release

Core compilation requires the pinned RISC-V GCC on Ubuntu 24.04. Build output
must stay outside the clean source checkout. No purchased game files are
needed: both Undertale and Deltarune programs are compiled without source
game data or a private game fingerprint.

```sh
python3 release/pipeline.py build-core --source "$PWD/out/core" --out "$PWD/out"
python3 release/pipeline.py package-core --source "$PWD/out/core" --programs "$PWD/out/programs" --out "$PWD/out/artifacts" --version vX.Y.Z
python3 release/pipeline.py build-tools --out "$PWD/out/artifacts"
python3 release/pipeline.py assemble-release --source "$PWD/out/core" --out "$PWD/out/artifacts"
```

Replace `vX.Y.Z` with the intended release version. Use a fresh artifact output
folder for each release. `assemble-release` requires the complete expected
asset set and refuses missing or unexpected files before writing `SHA256SUMS`.
It also includes source archives and license/provenance records. Review the
archive contents and test installation before publishing a stable release.
The GitHub workflows define the supported CI and explicit release dispatch;
regular pushes should validate artifacts without publishing a stable release.

Every release, including tool-only updates, must carry:

- `butterscotch-pocket.zip`: the installable core tree with both game programs.
- `plugin.wasm` and `plugin.json`: the Pocket Sync plugin pair.
- `prepare-data-{darwin,linux,windows}-{amd64,arm64}.tar.gz`: six native bundles.
- `prepare-data.wasm`: standard-Go WASI CLI, separate from the Extism plugin.
- `CORE-PROVENANCE.json`, `core.lock.json`, `RUNTIME-SUBMODULES.json` and `SHA256SUMS`.
- `butterscotch-core-source.tar.gz`, `pocket-tools-source.tar.gz`, `openfpga-runtime-source.tar.gz` and `licenses.tar.gz`.
- `GAME-DATA.md`: supported input and installation instructions.

The core package uses an explicit APF allowlist: `Cores`, `Assets` and
`Platforms` metadata, programs and matched runtime. It excludes commercial
inputs/packs, saves, benchmark instances, local logs and device metadata.
The ZIP installs both game entries, but game data remains a separate user
step. Never add a local staging folder to release assets.

## Distribution contract

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
git -C out/runtime-source -c url.https://github.com/.insteadOf=git@github.com: submodule update --init --recursive
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
