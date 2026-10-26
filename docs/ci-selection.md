# Domain-based CI

`Build` is the entry point for pull requests and pushes to `main`. It always
runs the routing tests and a changed-file classifier, then invokes the existing
build/test workflows only for selected domains. Its final **CI result** job
always runs and fails if selection failed, a selected workflow did not succeed,
or any invoked workflow failed or was cancelled.

When configuring branch protection or a ruleset, require **CI result** instead
of individual platform/matrix jobs that may legitimately be absent. This change
does not alter repository settings. Filtering whole required workflows with
`paths` can leave checks pending; conditional jobs plus an always-running verdict
avoid that problem. See [GitHub's required-check guidance](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks).

## Routing policy

The policy lives in `scripts/ci/selection.py` and has no third-party Python
dependencies. Domains combine when a change touches multiple areas.

| Changed area | Selected checks |
| --- | --- |
| Pocket glue, configuration, assets and fixtures under `src/openfpga/` | Pocket builds for both games and existing port fixtures |
| `src/runner_snapshot.c`, `.h`, and explicitly listed Pocket snapshot/cache fixtures | Pocket |
| `src/openfpga/tools/pocket-data/`, bundled `GAME-DATA.md`, root `LICENSE` | Complete Pocket distribution: native/WASI/Extism/C-reference tools, both public programs, package/source/provenance validation |
| Pocket platform/build/release inputs, Python/C reference converters, or Pocket platform headers | Pocket and complete distribution |
| `src/ps2/`, `src/audio/ps2/`, `src/ps3/`, `src/vita/`, `src/switch/` | Corresponding console only |
| Web/Android paths and dedicated Web/AppKit backends | Corresponding port |
| Dedicated Windows/macOS artifacts and mapped toolchain files | Corresponding platform |
| Game regression tests | Core tests; the shared `try-catch-finally-test/data.win` also selects Pocket |
| General documentation and skill documents | Routing tests and final verdict, without heavy builds |
| Shared runtime/renderer/headers, shared backends, vendor code, build system, or unknown paths | All domains |

Docs routing is limited to document/image extensions in known documentation
locations. For example, a new Python file under `docs/` still gets broad coverage.
The Go module is checked before general docs rules because its data and licenses
are bundled/tested. CI entry-point, classifier, and unknown workflow changes also
select all domains. Changes to a known reusable workflow select the domains it
defines; console and Web/Android workflows retain all of their leaves for such
changes.

This preserves the [existing Pocket-only snapshot codec boundary](https://github.com/zenibako/butterscotch-pocket-legacy/issues/18).
It does not narrow ordinary `src/*.c` or `src/sw/` changes based on their original
motivation: a Pocket renderer optimization still selects the shared builds.

Go parity tests invoke `prepare_game.py --build-tools`, which compiles C logging,
image and audio converters. Those sources and their shared headers/vendor inputs
therefore retain Go coverage. Pocket platform headers are conservatively routed
to both Pocket and Go to cover transitive includes from `of_log.c`.

The independent `Update Coverage` workflow is existing wiki-publishing automation,
not a CI build/test gate. Its publishing policy is unchanged.

The `PocketTools` domain calls the read-only `release-pocket.yml` distribution
workflow through `build-pocket-data-tools.yml`. Both programs and the nested
Go module are built from the same checkout. The separate `publish-pocket.yml`
workflow requires an explicit version and draft action; routine CI cannot
publish. SDK/runtime hashes remain pinned external inputs.

## Pocket build reuse

The Pocket workflow runs make with two jobs per invocation. Build/test steps
remain sequential so configuration changes cannot race make's object-directory
invalidation. The builtin, clipping, font/menu and Undertale battle fixtures share
one Undertale sanitizer object directory because their compiler flags are
identical. Deltarune, ordinary desktop builds, the default profiler and the
generic profiler retain separate directories. All existing checks still run.

`ccache` wraps host make compilation through `PC_CC`; device make commands select
the cached RISC-V compiler explicitly. No cross compiler or multiword `CC` is
exported to the standalone snapshot shell suite. That suite retains clang and
its existing sanitizer checks.

Only the 500 MB compiler cache is saved between runs, keyed by schema, runner
OS/architecture, pinned SDK revision, actual host/cross compiler and ccache binary
contents, and commit. Restore prefixes share compatible compiler results across
source changes. Ccache checks compiler contents and individual compilation
inputs; raw object directories are not restored. Cache hits never skip builds,
tests, BRAM checks or artifact upload. Per-run cache statistics are printed even
if a later test fails, provided build-tool installation succeeded.

`scripts/ci/test_pocket_workflow.py` runs the workflow's shell commands against
recording tools to check fixture coverage, configuration isolation, failure
propagation and compiler fingerprints without commercial assets or an SDK. This
does not measure real compiler-cache hit rates or GitHub runner speed; inspect
the workflow's statistics and step timings for cold/warm comparisons.

## Manual full matrix

In GitHub Actions, choose **Build → Run workflow**. Every manual invocation
selects all domains and expands the Unix architecture matrix. From an authorized
local checkout, the equivalent command is:

```sh
gh workflow run build.yml --ref main
```

Automatic pull requests retain the existing reduced Unix architecture matrix;
selected Unix builds on main pushes retain the expanded matrix. No platform's
existing compiler, WAD, OS or artifact matrix is removed. A PS2-only change skips
the PS3/Vita/Switch leaves, while a console-workflow change selects all four.

## Diff and failure behavior

Pull requests compare the merge base of the event's base/head SHAs against its
head; pushes compare their before/after SHAs. Checkout fetches full history.
Rename detection is disabled so a move between ports checks both the old and new
locations. NUL-delimited paths preserve spaces, tabs, Unicode and newlines.

An initial push, unavailable revision, invalid event, or failed Git diff selects
all domains and the expanded matrix. An empty successful diff selects no heavy
jobs. Changed paths are data, never interpolated into shell commands or emitted
as output keys. Only static domain names and JSON booleans reach `GITHUB_OUTPUT`.

## Local validation and changes

Run the same behavior tests used by the always-running selector job:

```sh
python3 -m unittest discover -s scripts/ci -p 'test_*.py'
```

Preview a set of changed paths without running builds:

```sh
printf '%s' '["src/openfpga/platform/of_menu.c"]' |
  python3 scripts/ci/selection.py select --paths-json -
```

When adding a port or workflow, update the domain/job maps, dispatcher outputs,
conditions, reusable inputs, verdict dependencies and regression cases together.
Check workflow syntax with `actionlint`. Shared or unrecognized inputs should
stay broad until their dependency boundaries are established.
