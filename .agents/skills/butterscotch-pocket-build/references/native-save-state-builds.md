# Native Memories and sleep builds

Use this for a card with the Pocket's native Memories and power-button
sleep/wake. Read `src/openfpga/README.md`'s save-state section first. The SDK
and runtime must implement the same save-state API; a successful link against
new headers does not prove that an older `os.bin` services requests.
Use the README's current minimum runtime revision and staging provenance rules;
the historical pair below is not evidence of current normal-save fixes.

## Stage a matching SDK and runtime

Keep the base SDK read-only. Use a separate staged SDK with the chosen
release's headers, build/linker support, and its exact paired `os.bin`,
`os25.rbf_r`, and loader. Verify published checksums and record the source
revision, runtime release, compiler/toolchain, and staged changes. Check
`of_savestate.h`, `OF_HW_SAVESTATE`, and the runtime's release guidance.

The Pocket-tested pair observed on 2026-10-15 was
[runtime 92202ab](https://github.com/zenibako/openfpgaCore/releases/tag/pocket-runtime-92202ab),
os25 at 100 MHz, seed 1. It is a historical known pair, not an instruction to
choose it over a newer verified release. Its reported slow-85°C WNS was
−0.777 ns: software tests and successful Pocket operation do not establish
FPGA timing closure. Preserve that limitation when shipping this pair.

## Build and identify the output

The `save-states` alias and `native-saves` target always build under
`src/openfpga/out/native-saves`, even when called with a different `ROOT`.
When changing compiler, SDK or untracked flags, avoid those convenience targets:
call `all` directly for each game with `SAVESTATES=1` and a fresh absolute root.
This isolates objects without deleting the previous release or its evidence.

From the checkout, after sourcing the parent workspace's `env.sh` on macOS,
the following builds both games. Check both assets first; for a one-game
release, omit the other game's asset check and build command.

```bash
CHECKOUT=$(pwd -P)
NATIVE_ROOT=$(mkdir -p "$CHECKOUT/src/openfpga/out" &&
  mktemp -d "$CHECKOUT/src/openfpga/out/native-release.XXXXXX")
(
set -eu
: "${NATIVE_ROOT:?Fresh release root creation failed}"
test -d "$NATIVE_ROOT"
test -f "$CHECKOUT/src/openfpga/games/undertale/data.win"
test -f "$CHECKOUT/src/openfpga/games/deltarune/data.win"
make -C "$CHECKOUT/src/openfpga" SHELL=/opt/homebrew/bin/bash \
  SDK_ROOT=/absolute/path/to/staged-sdk ROOT="$NATIVE_ROOT" \
  GAME=undertale SAVESTATES=1 FOCUS_BENCH=0 all
make -C "$CHECKOUT/src/openfpga" SHELL=/opt/homebrew/bin/bash \
  SDK_ROOT=/absolute/path/to/staged-sdk ROOT="$NATIVE_ROOT" \
  GAME=deltarune SAVESTATES=1 FOCUS_BENCH=0 all
)
```

Packaging requires Bash 4+; the example uses Homebrew Bash rather than macOS
`/bin/bash`. Set the appropriate Bash path on other hosts. Keep the chosen
`NATIVE_ROOT` value for every subsequent step; in a new shell, restore that
exact absolute path rather than creating another directory or using the default.
When both games were requested, verify both assets exist before building and
both new production ELFs exist afterwards; an Undertale-only success is incomplete.

- Card tree: `$NATIVE_ROOT/build/pocket/butterscotch/`.
- App ELFs: `$NATIVE_ROOT/.obj/<game>/app.elf`.
- `tools/mkcard.sh` sets the shared `sleep_supported` switch. Check that every
  game entry answers state requests, including explicit unsupported-scene
  refusals; the switch cannot selectively disable sleep for one game.
- Deltarune save-state cards include **Deltarune Sleep Test**, config
  `deltarune_sleep.ini`, argument `--bench-sleep`.

The build skill's no-rebuild-during-copy rule applies to this separate tree
too. Check for `rsync --server` before rebuilding it; leave the finished tree
unchanged while the user copies it.

Ordinary `copy`/`compare-copy` examples select the ordinary output tree. For
this native tree, adapt the build skill's authorized SD-copy route explicitly:

```bash
rsync -rc --exclude '._*' --exclude '.DS_Store' \
  "${NATIVE_ROOT:?Set the completed release root}/build/pocket/butterscotch/" "$CARD/"
```

Run on the machine with card access; then use the parent skill's cleanup,
verification, and unmount steps. Do not substitute the ordinary tree.

## Package evidence

Package the already-built native card tree explicitly. A bare `make package`
does not select it: its `all` prerequisite can rebuild ordinary outputs, and
the SDK dispatcher may resolve paths relative to the SDK instead of the port.
Do not rely on a `ROOT` override alone. Use the exact `NATIVE_ROOT` retained
from the successful builds above:

```bash
(
set -eu
native_tree="${NATIVE_ROOT:?Set the completed release root}/build/pocket/butterscotch"
package_dir=$(mktemp -d "$NATIVE_ROOT/package.XXXXXX")
archive="$package_dir/butterscotch-native-saves.zip"
(cd "$native_tree" && zip -r "$archive" Cores Assets Platforms \
  -x '*.DS_Store' '*/._*')
unzip -t "$archive"
shasum -a 256 "$archive"
)
```

The fresh ZIP path prevents entries from an older archive surviving an update.
This reads the completed tree without rebuilding it; copying to a card still
follows the parent skill's no-rebuild-during-copy rule.

Verify packaged game ELFs against built ELFs, every instance's slot paths
against actual files, runtime checksums, and `sleep_supported`. Record the
application commit, game/chapter data hash, runtime source/release, and final
ZIP SHA-256. Test ZIP integrity and read back the packaged bytes.

A small update ZIP may contain just the changed ELFs and benchmark entries
when the user already has the verified runtime and game assets. State that
dependency explicitly. Neither a full card nor an update should package the
user's progress saves; don't silently replace assets or runtime binaries.

Build the update inventory from the intended release changes, including an
updated logo and its platform/core metadata when requested. Distinguish both
production ELFs from any separately built diagnostic ELF. Verify every included
payload against its staged source, including the logo; inspect launch entries
and slot filenames so a focused binary cannot replace a normal game entry.
Record dependencies on unchanged installed runtime files with their hashes.
Preserve Memories/sleep metadata and include runtime changes as a matched set
when needed. Do not infer a bootloader/bitstream exception from a past release:
an authorized exception applies only to its stated compatible build and evidence.

Before handoff, use the verify skill's save-state reference for desktop
restore comparisons and the physical-device checklist. No harness substitutes
for the Pocket's real power-button sleep transaction.
