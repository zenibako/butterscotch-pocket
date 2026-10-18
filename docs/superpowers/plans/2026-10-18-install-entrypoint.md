# Install entrypoint implementation plan

**Goal:** Make zenibako/butterscotch-pocket the tested install and release home while core source remains pinned in the fork.

**Spec:** ../specs/2026-10-18-install-entrypoint-design.md

## Tasks

- [x] Move complete Go module to root; update imports, make C oracle checkout explicit; preserve third-party licenses and verify native/WASI/Extism tests.
- [x] Add immutable core/SDK/runtime pins, verified runtime files, asset-free assembler and regression tests; build both core programs from pinned source.
- [x] Add CI and explicit complete-release workflow; source/provenance archives, six native bundles, plugin pair, WASI and checksums.
- [x] Replace install README and game-data guide paths; document legacy layout, core/plugin installs, supported games and experimental runtime limitations.
- [x] Remove active converter/workflow from fork and link replacement; verify no active old import paths or stale build directions.
- [x] Run tests and release assembly, review resulting artifacts/workflows, sign commits and push migration for review; report any checks still running.

## Verification results

- Full native/WASI/Extism/C-reference suite passed after migration; nested Vorbis regressions and go vet passed.
- Five release assembler regressions passed; actionlint passed.
- Both game programs built without private files using the locked GCC 13.2 SDK/runtime combination.
- Local complete release assembled: 19 assets, all SHA-256 checks passed; release plugin matched the tested binary.
- GitHub run 39162040663 passed Linux, macOS, Windows, core, tools and final distribution checks.
- Fork cleanup pushed as chore/move-pocket-tools; its main branch remains unchanged.
