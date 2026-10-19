---
name: pocket-release
description: Refresh Butterscotch Pocket's immutable core pin, verify complete distribution CI, or prepare a versioned draft release from the Pocket release repository. Use for core updates and release coordination, not runner implementation or game-data conversion.
---

The Pocket release repository (`zenibako/butterscotch-pocket`) owns this
process. Core code stays in `zenibako/Butterscotch`. Read `CLAUDE.md` and
`docs/BUILD.md` in the release checkout first. Work in its active `release/`
pipeline, not its historical SDK/runtime trees.

## Refresh a core pin

1. Inspect local changes and the current `release/core.lock.json`. Use a clean,
   separate core checkout; never switch a user's development tree underneath
   them. Resolve the requested ref (default `main`) once and record its full SHA.
2. Either dispatch **Prepare core pin update** (`update-core.yml`, `core_ref`)
   on the Pocket release repository and download `core-pin-candidate`, or fetch
   the candidate and previous pinned commits into the separate core checkout
   and run locally:

   ```sh
   python3 release/update_core.py --source /absolute/path/to/clean-core
   ```

3. Review `out/core-update/REVIEW.md` and `core-pin.patch`. Compare exact trees:
   rewritten history can produce a huge merge-base diff even when code is
   identical. Review build-script changes too, not only `src/` and `vendor/`.
   The helper leaves SDK/runtime pins and hashes alone. A changed API/runtime
   requirement needs a separate, matched dependency update, not new hashes for
   whatever binaries happen to be available.
4. Apply in the release checkout with `--apply`, or use `git apply --check`
   followed by `git apply` on the downloaded patch at its recorded release
   commit. An already-current pin produces an empty patch; do not invent a
   commit. Run release unit tests and `python3 release/pipeline.py validate`.
5. Make a signed conventional commit on a topic branch. Respect repository
   signing and Sphinx checkpoints. Push only with session authorization. Use
   the existing **Build and verify distribution** workflow on that exact commit
   to test native platforms, build both games, run mandatory plugin/C-reference
   checks, assemble the full asset set and verify checksums. Do not use the
   preparation workflow's success as build or hardware evidence.
6. Report the exact pin, changed content, CI link and remaining device checks.
   Provide an outline for the maintainer's PR description; do not publish one
   on their behalf without explicit authorization.

## Prepare a release

Require complete distribution CI on the intended release commit and the
hardware compatibility checks required by `docs/BUILD.md`. Record Undertale
and Deltarune launch/progress-save checks plus applicable Memory/sleep checks
using the user's own game data. State unperformed checks explicitly. Preserve
the documented experimental runtime/timing limits; CI is not hardware proof.

Obtain the intended version and explicit release authorization before a
version/tag action. For an authorized draft, dispatch **Build and verify
distribution** on the reviewed ref with `version=vX.Y.Z`, `create_draft=true`.
That rebuilds and gates the complete draft on all required jobs. Never create
a tools-only latest release: core ZIP, plugin/icon, six native bundles, WASI,
source/provenance/license archives and checksums belong together.

Inspect the resulting assets/checksums and report the draft link. Stable
publication is a separate, explicitly authorized action. Never upload game
data, saves, local staging folders or private logs.

For a hosted-runner acquisition failure, inspect the job annotations and retry
only with authorization; keep retries bounded (at most two). Never convert an
abandoned/cancelled required job into success or retry test failures blindly.
