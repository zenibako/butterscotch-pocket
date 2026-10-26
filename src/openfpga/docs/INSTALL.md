# Butterscotch Pocket

Run Undertale v1.08 and Deltarune Chapter 1 on Analogue Pocket using the
[Butterscotch runner](https://github.com/ButterscotchRunner/Butterscotch) and openfpgaOS.
The runner, Pocket platform, preparation tools, Pocket Sync plugin and release
automation are maintained in this fork.

**You must supply your own supported game installation. No commercial game
data is included or downloaded.** Deltarune support is experimental; other
chapters and game versions are not established as compatible.

## Install

1. Open [Releases](https://github.com/zenibako/butterscotch-pocket/releases)
   and download **`butterscotch-pocket.zip`** from the release you want.
2. In Pocket Sync, connect your SD card and drag the core ZIP onto the window.
   Alternatively, extract it into the SD-card root, merging `Cores`, `Assets`
   and `Platforms` with the existing folders.
3. Follow [Add your own game files](../GAME-DATA.md) to prepare and install your
   Undertale or Deltarune data. Installing the core alone does not make either
   commercial game playable.

Release assets become available when a stable release is published. Source
archives, native preparation tools and the plugin assets are separate from
the core ZIP; do not import those into Pocket Sync as core packages.

## Prepare your games

Choose either method; both use the same Go conversion library:

| Method | What to use |
|---|---|
| Desktop CLI | Download `prepare-data-<os>-<arch>.tar.gz` from the same release. Supports macOS (`darwin`), Windows and Linux, each on `amd64` or `arm64`. Use `darwin-arm64` on Apple silicon Macs. |
| Pocket Sync plugin | In **Plugins → Add plugin via URL**, enter `https://github.com/zenibako/butterscotch-pocket`, then choose **Verify & install plugin**. Stage originals under `Assets/butterscotch/common/undertale/` or `deltarune/` on the connected card, then install directly from the plugin. |

The plugin's installer reads `plugin.wasm` and `plugin.json` from the latest
stable release. Installing the plugin and installing the core are separate
steps. No separate Pocket Sync development app is needed for published plugin
releases. See the [game-data guide](../GAME-DATA.md) for exact source layouts,
macOS `.app` resource copying, output paths and updating prepared data.

The desktop CLI writes a new output folder and refuses an existing one; copy
all three generated game files together. The plugin installs the
three files directly into `Assets/butterscotch/common/`, with a choice to keep
(default) or discard the selected original source folder after a successful
install. Preserve native filenames within the separate source folders.
These plugin instructions require v0.7.1 or newer; v0.7.0 uses the earlier
separate-output workflow. Neither method
changes your computer installation or writes saves. Ordinary game progress lives under
`/Saves/butterscotch/common/`; back up your SD card before updates.

## Controls and current limits

D-pad moves; A confirms; B cancels; X/Y opens the game menu; Start maps to Enter.
Select opens the Butterscotch menu, and L toggles speed/accuracy. See the
[platform guide](../README.md)
for detailed controls, debugging and scene-specific save-state support.

Performance varies by scene and some rendering features remain incomplete.
The pinned release runtime uses an experimental os25 sleep/clock configuration
at 100 MHz with -0.777 ns worst setup slack in the slow 85°C corner
(the build misses that timing target). Save states and sleep support
remain experimental. Private Memories from earlier test ELFs may not load with
public release ELFs because their binary identities differ. Ordinary progress
saves are separate and the installation package excludes saves.

## AI development disclosure

OpenAI Codex and Claude Code have provided substantial assistance with the
Pocket integration, game-data tools, Pocket Sync plugin, tests, documentation
and release packaging. The maintainer directs changes and tests on hardware.
This describes our development work; it does not attribute the inherited
Butterscotch runner, SDK or runtime wholesale to AI.

Releases include an explicit `declare_ai` list in the core's `updaters.json`.
Pocket Sync's numeric AI-ness score comes from the separate
[openFPGA AI report](https://github.com/openfpga-library/openfpga-ai-check),
not a measured percentage of AI-written source. A missing report is not a
claim that AI was unused.

## Source and releases

The shared Go module lives under `src/openfpga/tools/pocket-data`. Each
release builds both game programs and the tools from the same clean repository
commit, verifies its matched SDK/runtime inputs, and records source and file
hashes. Tool-only releases still include the complete core ZIP, plugin, native
tool bundles and source/provenance archives.

See [Building and releasing](BUILD.md) for the dependency and release process.
The Go tools retain their [AGPL-3.0 license](../tools/pocket-data/LICENSE-GO-TOOLS).
SDK and patched decoder notices are preserved in the
[release notices](../release/NOTICE) and the tools' third-party directories.

The intended openFPGA catalog distribution repository is this repository, with
asset filter `^butterscotch-pocket\.zip$`. Listing is a separate maintainer
submission; the source repository and plugin assets are not the core package.
