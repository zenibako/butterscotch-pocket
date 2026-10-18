# Install entrypoint implementation plan

**Goal:** Make zenibako/butterscotch-pocket the tested install and release home while core source remains pinned in the fork.

**Spec:** ../specs/2026-10-18-install-entrypoint-design.md

## Tasks

- [x] Move complete Go module to root; update imports, make C oracle checkout explicit; preserve third-party licenses and verify native/WASI/Extism tests.
- [x] Add immutable core/SDK/runtime pins, verified runtime files, asset-free assembler and regression tests; build both core programs from pinned source.
- [x] Add CI and explicit complete-release workflow; source/provenance archives, six native bundles, plugin pair, WASI and checksums.
- [x] Replace install README and game-data guide paths; document legacy layout, core/plugin installs, supported games and experimental runtime limitations.
- [x] Remove active converter/workflow from fork and link replacement; verify no active old import paths or stale build directions.
- [ ] Run tests and release assembly, review resulting artifacts/workflows, sign commits and push migration for review; report any checks still running.
