# Butterscotch Pocket install entrypoint

Approved in chat on 2026-10-18: use zenibako/butterscotch-pocket as the installation repository, move the complete shared Go converter/CLI/plugin there, and build public core releases from a pinned zenibako/Butterscotch commit.

## Existing decisions

The fork README preserves upstream Butterscotch documentation and delegates Pocket documentation to src/openfpga/README.md. Its src/openfpga/GAME-DATA.md and docs/superpowers/specs/2026-10-18-go-game-data-design.md define conversion formats, source preservation, staging and independent native/WASI/Extism checks. Those contracts remain in force. The catalog Add Core form supports a repository plus an asset filter; Pocket Sync plugins consume plugin.wasm and plugin.json from the latest release.

## Plugin staging contract update

The later user-approved direct-install change supersedes the plugin's original
staging/output contract referenced above: complete originals live under
`Assets/butterscotch/common/undertale/` or `deltarune/`; the plugin installs the
selected game's three files directly into `common/`, with keep (default) or
explicit discard of only the selected source folder after success. Installation
failure restores previous files and keeps originals; deletion failure is a
warning after success. Work/recovery paths and the plugin manifest are hidden.
CLI preparation continues to preserve sources and refuse existing output
folders. See [GAME-DATA.md](../../../GAME-DATA.md) for current layouts and release
availability.

## Architecture

Preserve this repository history and legacy SDK/port directories. The root becomes the shared Go module, installation documentation, release pins and orchestration. The runner and platform source remain in the fork. The complete Go module moves together, including patched dependencies and tests, with imports updated wholesale. C reference tests use an explicit checkout of the pinned fork.

A checked-in lock records immutable core, SDK and runtime source revisions and hashes of the matched, already-tested runtime binaries. Build both game programs without commercial inputs and assemble an explicit allowlist of APF files, including both game entries. Never publish games, saves, debug logs or local SD metadata. Keep experimental save-state/runtime limitations documented.

Every release has the complete core ZIP, plugin.wasm, plugin.json, native tool bundles, WASI CLI, checksums and source/provenance. Catalog filter: ^butterscotch-pocket\.zip$. Do not automatically publish a stable release while introducing this migration; build and validate assets, with release publication requiring an explicit version/tag workflow.

The fork removes the active Go module/workflow and links users/developers here; Python/C reference tools remain there. Historical spec documents are retained. Legacy SDK files in this repo are preserved but are not used to build current releases.

## Verification

Run moved Go tests, nested decoder tests, vet, mandatory Extism and C oracle integration. Exercise package assembly against synthetic fixtures including missing inputs, runtime corruption, exclusion of game/save files and correct APF runtime paths. Run real pinned core builds and validate the complete generated ZIP in CI. Review release workflow permissions and ensure both build jobs gate publication. Preserve original source licenses and provide corresponding source archives for the compiled programs/runtime.
