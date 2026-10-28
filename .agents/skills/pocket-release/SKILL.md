---
name: pocket-release
description: Use when validating Butterscotch Pocket distributions, preparing release candidates or drafts, publishing explicitly authorized releases, or updating matched runtime dependencies.
---

# Butterscotch Pocket releases

Read repository instructions and the [build and release guide](../../../src/openfpga/docs/BUILD.md) first.

## Read the shared workflow

Canonical owner: <https://github.com/zenibako/pocket-dev-tools> (formerly
`pocket-harness`). Use the root established in this session, then
`POCKET_DEV_TOOLS_ROOT`, then legacy `POCKET_HARNESS_ROOT`. Resolve conflicting
explicit roots before proceeding. A sibling `pocket-dev-tools` or `pocket-harness`
is only a candidate: verify `.agents/skills/pocket-release/SKILL.md` and
`docs/RELEASING.md`, confirm the Git remote identifies the canonical repository
(or its former name), and record its full commit. Resolve and read that canonical
skill before following its relative links. Apply it together with this project's
local release guide; stronger local checks remain required.

If the shared checkout is unavailable, report the missing dependency and continue
applicable local checks. Do not claim shared validation, copy the shared skill
body, or create cross-repository skill symlinks. A checkout predating the release
skill needs an explicit update; a matching directory name alone is insufficient.

## Project contract

Both games, pinned os25 runtime and the complete core/tools/source/license inventory remain required. Use this checkout's `release-pocket.yml` and `publish-pocket.yml`, with its explicit version and source provenance rules.

Read the preserved [project procedures](references/project-release.md) for
local commands, device checks and release recovery details.
