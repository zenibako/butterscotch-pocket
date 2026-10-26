# Get started with Butterscotch Pocket

1. Follow [installation](docs/INSTALL.md) to download and install the core ZIP.
2. [Prepare your own supported game files](GAME-DATA.md), using the native tool
   or the Pocket Sync plugin from the same release.
3. Read the [controls and current limits](README.md) before playing.

For development, clone this repository and read [building and releasing](docs/BUILD.md).
The runner is in `src/`, the Pocket port in `src/openfpga/`, and the shared Go
preparation library, CLI and plugin in `src/openfpga/tools/pocket-data/`.
External SDK/runtime dependencies are pinned separately. User game data and
progress saves are never included in source or public release packages.
