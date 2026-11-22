# Butterscotch Pocket

Run Undertale v1.08 and Deltarune Chapter 1 on Analogue Pocket using the
[Butterscotch runner](https://github.com/ButterscotchRunner/Butterscotch) and openfpgaOS.
The runner, Pocket platform, preparation tools, Pocket Sync plugin and release
automation are maintained in this fork.

**You must supply your own supported game installation. No commercial game
data is included or downloaded.** Deltarune support is experimental; other
chapters and game versions are not established as compatible.

## Install

Follow **[Install Butterscotch Pocket](../GAME-DATA.md)**. It covers installing
the core with or without Pocket Sync, building your game files from your own
copy of Undertale or Deltarune, and fixing common errors such as
*"The game data is missing"* or *"Load error in Core"*.

The core on its own won't start a game: you also need three game files built
from your own copy, which the guide walks you through.

## Controls and current limits

D-pad moves; A confirms; B cancels; X/Y opens the game menu; Start maps to Enter.
Select opens the Butterscotch menu; its Performance Mode setting selects
Speed or Accuracy. L is unused. See the
[platform guide](../README.md)
for detailed controls, debugging and scene-specific save-state support.

Performance varies by scene and some rendering features remain incomplete.
Memories (save states) and sleep are experimental. Your ordinary progress
saves are separate, and the core ZIP never includes or replaces them.

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
