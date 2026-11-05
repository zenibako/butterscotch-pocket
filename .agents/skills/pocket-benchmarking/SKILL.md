---
name: pocket-benchmarking
description: Use when choosing Butterscotch Pocket profiles, preserving routine regression baselines, collecting Undertale or Deltarune reports, or comparing software and platform configurations.
---

# Pocket benchmarking route

## Shared repository identity

The canonical repository is <https://github.com/zenibako/pocket-dev-tools>,
formerly `pocket-harness`. For discovery use a root explicitly established in
the session, then `POCKET_DEV_TOOLS_ROOT`, then legacy `POCKET_HARNESS_ROOT`.
Resolve conflicting explicit roots before proceeding. Both sibling directory
names are candidates only; validate the canonical benchmark skill and collector
as described below. Existing local paths and CLI names remain supported.

Read the maintained [Butterscotch integration guide](../../../src/openfpga/docs/BENCHMARKING.md)
and this repository's instructions first. Use the shared checkout resolved
by the discovery rules above. Adjacent directories are only candidates: verify that `benchmarks/__main__.py` and
`.agents/skills/pocket-benchmarking/SKILL.md` exist there. Resolve and read that
canonical skill before following its relative references. Run collector commands
from its verified checkout root. If unavailable, report the missing dependency
and continue applicable project checks; do not copy the shared skill body or
create a cross-repository skill symlink.

Keep scene selection and counter interpretation in
[performance](../../../.agents/skills/butterscotch-pocket-perf/SKILL.md), output and
lifecycle checks in [verification](../../../.agents/skills/butterscotch-pocket-verify/SKILL.md),
and diagnostic construction in [build](../../../.agents/skills/butterscotch-pocket-build/SKILL.md).
The [release skill](../pocket-release/SKILL.md) still owns complete distributions.
Shortlist compatible profiles for this project; all four matrix cells are not
required. Register actual packaged artifacts and preserve raw private evidence.
Use the strict default for software changes, same-ELF platform mode for controlled
platform changes, or configuration mode with `--reason` for different ELFs.
Whole-configuration deltas cannot isolate a CPU, clock or OS effect. Collector
verification does not replace game correctness, hardware or release gates.

## Routine regression checks

For recurring software checks, require the shared `benchmarks/REGRESSIONS.md`
and read it with this project's benchmark guide. The contract was introduced in
`fd5fc7af6cb51b19bb800d6c2cd43b7c03cfcf58`; record the actual verified shared
checkout commit. Preserve the accepted reference, create independent candidates,
and require project correctness and negative controls before interpreting deltas.
A missing adapter or fixture is an explicit coverage gap. Collector `verify` or
`compare` is not an output-regression gate or a substitute for device checks.
For supported Butterscotch os25 cases, use `regression.py baseline/check` after
reading shared `docs/REGRESSION.md` and current CLI help. Never recapture or
promote over a reference; retain mismatch reports/images and explicit failures.
