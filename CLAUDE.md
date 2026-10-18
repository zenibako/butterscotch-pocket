# Agent guidance — Butterscotch Pocket

`AGENTS.md` is a symlink to this file. This repository is the install and
release entrypoint for Butterscotch Pocket. The active Go module is at the
root; runner and Pocket platform source remain in
https://github.com/zenibako/Butterscotch.

## Orient before changing code

Read `README.md`, `GAME-DATA.md`, `docs/BUILD.md` and the relevant approved
spec/plan under `docs/superpowers/`. The repository was repurposed from an
openfpgaOS SDK checkout with the user's authorization. Historical `src/sdk`,
`src/apps`, `runtime`, `scripts` and SDK Makefiles remain for history; they are
not the current release dependency source. Do not refresh or edit those
legacy trees as part of routine tools/release work.

## Active layout

- `cmd/prepare-data`: native and WASI CLI.
- `cmd/pocket-plugin`, `plugin`, `plugin.json`: Pocket Sync Extism plugin.
- `prep`: shared conversion implementation.
- `integration`, `internal/testgame`: synthetic native/WASI/Extism checks.
- `third_party`: patched pure-Go decoders, original licenses and patch notes.
- `release`: immutable dependency lock, verified runtime inputs and pipeline.
- `.github/workflows`: checks and explicit complete release publication.

## Contracts

No commercial game data, user saves, debug logs, SD-card/device metadata or
private staging folders may enter source control or release assets. Supported
input versions are Undertale v1.08 and Deltarune Chapter 1. Preserve source
files, refuse existing output folders and keep failed partial conversions
unpublished. The plugin sees only the connected card; do not add arbitrary
host-path access or network hosts.

Keep CLI, WASI and plugin conversion semantics together in `prep`. Update all
imports/usages for module changes. Preserve third-party attribution and explain
decoder changes in their patch notes.

Pin core/SDK/runtime revisions and verify runtime hashes. Never silently
substitute an SDK runtime or use the historical copies for a current release.
Build/package without purchased inputs; use the explicit package allowlist.
Every published release must contain the complete asset set described in
`docs/BUILD.md`, even for a tools-only update. Publication requires an explicit
version/tag action; do not publish a stable release merely to test CI.

## Verification and version control

Run `go test ./...`, `go vet ./...` and `go test -C third_party/vorbis ./...`.
Require the plugin host checks for plugin changes; C oracle checks use
`BUTTERSCOTCH_SOURCE` pointed at the pinned fork. Run release pipeline tests
and validate/package both game programs for packaging changes. Report checks
that could not run rather than declaring unverified behavior working.

Use this repository's existing Git history and signed conventional commits.
Do not rewrite history, replace it with a fresh repository, force-push, move a
bookmark or discard local changes without explicit user authorization. Do not
publish external comments/descriptions on the user's behalf unless explicitly
requested. Follow applicable shared workflow skills and keep scope focused.
