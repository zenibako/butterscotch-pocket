---
name: pocket-benchmarking
description: Use when choosing Butterscotch Pocket platform profiles, planning benchmark cores, collecting Undertale or Deltarune reports, or comparing software and platform configurations with pocket-harness.
---

# Pocket benchmarking route

Read the maintained [Butterscotch integration guide](../../../src/openfpga/docs/BENCHMARKING.md)
and this repository's instructions first. Use the harness checkout established
in the session or `POCKET_HARNESS_ROOT`. An adjacent `pocket-harness` is only a
candidate: verify that `benchmarks/__main__.py` and
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
