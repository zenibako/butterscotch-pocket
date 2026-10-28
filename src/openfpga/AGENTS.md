# Pocket development guidance

The runner, Pocket port, preparation tools, plugin and release pipeline share
this upstream fork. Read `README.md`, `GAME-DATA.md`, `docs/INSTALL.md` and
`docs/BUILD.md` before changes. The active tools module is
`tools/pocket-data`; release orchestration and pinned external inputs are
in `release`. Use the repository build and verification skills in
`.claude/skills`; `pocket-release` coordinates complete releases.

Preserve the runner's upstream layout, public core/platform/slot identifiers,
ordinary progress saves and user-owned game inputs. Keep CLI, WASI and plugin
conversion semantics together. Installation keeps originals by default;
explicit discard applies only after successful installation of the selected
game. Failure restores old installed files and preserves original inputs.

Build core programs and tools from the same clean repository commit. Keep
SDK/runtime/API/loader/bitstream pins and hashes matched; never substitute
legacy SDK trees. Public packages exclude commercial inputs, converted game
packs, progress saves, benchmarks and device logs. Preserve licenses and
source/provenance archives. Stable publication requires an explicit version;
CI validation alone does not authorize publication.

Run the Go module tests, vet, decoder regressions, mandatory C-reference and
Extism checks for tooling changes; run release unit tests and real complete
packaging for pipeline changes. Platform/VM/renderer/snapshot changes also
need the build/verify skills, both-game fixtures and deterministic frames.
For routine software checks, use the [preserved-baseline workflow](docs/BENCHMARKING.md#routine-regression-checks)
and local `pocket-benchmarking` router. Supported os25 cases use the shared
immutable `regression.py` gate; its simulated pass does not replace the affected
game, shipping-runtime or physical Pocket checks.
State explicitly which checks are desktop versus physical Pocket evidence.
Use signed conventional commits and preserve published history.
