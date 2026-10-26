---
name: pocket-release
description: Verify complete Butterscotch Pocket distribution CI or prepare an explicitly versioned draft release from the single upstream fork. Use for release coordination and matched external SDK/runtime dependency updates.
---

The upstream fork `zenibako/butterscotch-pocket` owns runner, Pocket platform,
tools and releases. Read repository instructions and `src/openfpga/docs/BUILD.md`
first. Active release inputs live in `src/openfpga/release`; the Go module is
`src/openfpga/tools/pocket-data`. Historical SDK trees are not build inputs.

## Verify a candidate

1. Inspect local changes and `src/openfpga/release/core.lock.json`. Schema 2
   selects the current checkout for runner and tools. Never add a self commit
   pin or clone a separate core checkout. Record the intended full commit.
2. Preserve the immutable SDK/runtime revisions and every binary hash unless
   the task explicitly updates a reviewed, matched dependency set. API headers,
   firmware, loader and bitstream must remain compatible; never recertify
   arbitrary binaries by replacing their hashes.
3. Use a clean checkout of the candidate. Run:

   ```sh
   python3 src/openfpga/release/pipeline.py validate
   python3 -m unittest discover -s src/openfpga/release -p 'test_*.py'
   (cd src/openfpga/tools/pocket-data && go test ./... && go vet ./... && go test -C third_party/vorbis ./...)
   ```

4. Follow the build guide to run mandatory plugin/C-reference checks, compile
   both public games and assemble the complete distribution. Outputs default
   to ignored `src/openfpga/out/`. Source verification rejects another checkout
   or dirty source. Keep the generated program/source provenance together;
   assembly requires matching core/tool source commits, exact dependency pins,
   the complete core ZIP manifest and matching entry/runtime bytes. Preserve
   tracked HEAD notices; ignored local logs must never enter native bundles or
   license archives. Runtime source must be the locked checkout's root.
5. Make a signed conventional commit and verify the complete distribution CI
   on that exact commit. CI's Pocket tools domain reuses `release-pocket.yml`;
   don't add duplicate builds or a core-pin preparation workflow. Push only
   with session authorization. Report the commit, CI link and remaining
   device checks. Provide an outline for the maintainer's PR description;
   do not publish external descriptions/comments without authorization.

## Prepare a release

Require complete distribution CI on the intended commit and the hardware
compatibility checks required by the build guide. Record Undertale and
Deltarune launch/progress-save checks plus applicable Memory/sleep checks
using the user's own data. State unperformed checks explicitly. Preserve
experimental runtime/timing limits; CI is not hardware proof.

Obtain the intended version and explicit release authorization before a
version/tag action. For an authorized draft, dispatch **Prepare complete Pocket draft
release** (`publish-pocket.yml`) on the reviewed ref with `version=vX.Y.Z`,
`create_draft=true`. It calls the read-only complete distribution workflow
with that version, then grants write permission only to the draft job after
validation passes. Commit its notes first at
`src/openfpga/docs/release-notes/vX.Y.Z.md`. Draft creation refuses missing or
empty notes and invalid versions. Reusable workflow calls and normal tags
only validate/build; they do not create a draft or publish a stable release.

Never create a tools-only latest release. The core ZIP, plugin/icon, six
native bundles, WASI, both source provenance records, complete source/license
archives, game-data guide and checksums belong together. Inspect assets,
source identities and checksums and report the draft link. The publisher
checks that the provenance version matches the explicit version input and
that both core/tools identify the workflow's exact source commit. Stable publication
is a separate, explicitly authorized action. Never upload game data, saves,
local staging folders or private logs.

For hosted-runner acquisition failures, inspect annotations and retry only
with authorization, at most twice. Never convert a cancelled required job to
success or retry test failures blindly.
