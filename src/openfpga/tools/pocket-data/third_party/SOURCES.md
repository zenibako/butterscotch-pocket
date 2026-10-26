# Source provenance and licenses

The Go CLI, shared conversion packages, Pocket Sync plugin and associated tests
originated together at `src/openfpga/tools/pocket-data` in
[zenibako/Butterscotch](https://github.com/zenibako/Butterscotch), commit
`f6261c51af5d2ed41719e7288fcb318a2918a433` (2026-10-18). The complete module
is maintained at that path in the consolidated Pocket fork. Its original
AGPL-3.0 license is preserved in [`LICENSE-GO-TOOLS`](../LICENSE-GO-TOOLS).
The legacy SDK license at the repository root does not relicense these files.

The patched Vorbis and Ogg readers retain their upstream MIT licenses and
patch descriptions in `vorbis/LICENSE`, `vorbis/PATCHES.md`,
`oggvorbis/LICENSE`, and `oggvorbis/PATCHES.md`.

Toolchain notices used for release builds are preserved in `licenses/`.
Go dependency versions and corresponding module sums remain in `go.mod` and
`go.sum`; these dependencies keep their own upstream licenses.
