---
name: butterscotch-pocket-build
description: Use when building, packaging or deploying the Butterscotch Analogue Pocket port, preparing Undertale and Deltarune update ZIPs, staging dependencies or save-state runtimes, committing Pocket changes, handling Pocket Sync plugin icons, or handling the port's build and CI review feedback.
argument-hint: "[checkout: path or host:path with game data + toolchain] [card: SD mount path, and which machine]"
---

# Building and deploying the Undertale Pocket port

For benchmark packages and platform/profile experiments, follow the local
[pocket-benchmarking router](../pocket-benchmarking/SKILL.md) and
[integration guide](../../../src/openfpga/docs/BENCHMARKING.md).
Preserve matched runtime inputs, diagnostic isolation and the release gates below.

## Inputs

Use session/handoff paths and hosts; ask once for missing required inputs.
Do not guess machines.

- **checkout** (`$CHECKOUT`): working checkout, game data and Pocket toolchain;
  a local path or SSH `host:path`.
- **card** (`$CARD`): Pocket SD mount path and machine, e.g. `/Volumes/Pocket`
  on a Mac.

## Local development builds

Read `src/openfpga/README.md` first; the external SDK is read-only.
Before locating a checkout, building either game, or assembling a local card,
read [local build procedures](references/local-builds.md) for workspace layout,
host/toolchain checks, cache isolation and the per-game commands.
Use a fresh named `ROOT` when changing compiler, SDK, layout or other build definitions;
preserve existing evidence. For Memories or sleep/wake, also read
[native save-state builds](references/native-save-state-builds.md): the convenience
target overrides `ROOT`. Keep production `FOCUS_BENCH=0` and diagnostic ELFs separate.

## Public distributions

For public packages use `src/openfpga/docs/BUILD.md` and `pocket-release`.
Runner and nested tools build from one clean commit with matched external
SDK/runtime pins. The linked local build procedures cover development.

## Pocket Sync plugin icons

Before changing plugin artwork or investigating a huge plugin row, read
[plugin icon sizing and delivery](references/plugin-icons.md). It covers
intrinsic dimensions, both Pocket Sync views, catalog discovery, and published
asset verification. Apply this to each discovered plugin logo, not unrelated
core `icon.bin`, banner, or game artwork.

## Keeping `src/openfpga/README.md` true

That README says what is here and how to use it, and nothing that a day of
work changes. It went out of date twice by carrying measurements and
walk-throughs of the renderer.

- **No measurements in it**: no frame times, load times, sizes or counts
  read off a run. Those go in the perf skill, dated and with the commit, or
  nowhere. A number may appear only as a definition (a frame is 33 ms) or
  next to the name that sets it (`MUSIC_RATE`, `TEXTURE_RESERVE_MB`).
- **No account of how the renderer does something**: that belongs in the
  comment over the code, and the README points at the file.
- **Update it in the same commit** as a change to anything a reader acts on:
  controls and debug chords, `make` targets, environment variables, file and
  slot names on the card, benchmark entries, what a log line means.
- Its outline comments and the sections marked "To be written" are the
  user's to write. Leave their wording alone; say so if one has gone stale.

## The cardinal rule: don't rebuild under a copy

`make` and `make compare` delete and recreate `src/openfpga/out/build/pocket/butterscotch/`. The
user often copies that tree to the SD card from another machine with rsync,
which takes a minute or more. Rebuilding mid-copy hands them a mixed tree.

- Use `make undertale_pc` for desktop iteration; it never touches `build/`.
- Before a device rebuild, check nobody is pulling:
  `pgrep -f 'rsync --server' | wc -l` must be 0.
- After rebuilding, tell the user the tree is ready and that you will leave
  it alone. If they said they are about to copy, do not rebuild until they
  confirm the copy is done.

## SD-card deployment

Before copying, verifying or troubleshooting a card, read
[SD-card procedures and layout](references/sd-card.md). Use the supplied checkout
and card hosts; keep the tree frozen through the copy and verification.
Progress saves are outside the build tree; replace them only when asked, and
preserve newer progress during migration. Unmount the card after copying.
Keep the SDK runtime and its os25 bitstream matched; switching that runtime to
os20 boot-loops before the OS banner.

## Committing

One repository holds both the runner and the port:

```bash
git add -A src .claude && git ... commit      # a branch off main
```

- Conventional commit messages; commits must be GPG signed. In Codex, run
  `gpg-warm-agent` before the first commit and verify the resulting signature.
  If warming fails, ask for an unlock rather than disabling signing.
- For Codex commits, supply `AI_CO_AUTHOR='Codex <noreply@openai.com>'`
  and/or an explicit `Co-authored-by: Codex <noreply@openai.com>` trailer.
  The inherited `prepare-commit-msg` hook otherwise inserts the placeholder
  `AI Model <ai@example.com>`, even when Codex attribution is configured.
  Verify `git log -1 --format=%B` as well as the signature after committing.
  Correcting published commit trailers requires the user's approval to rewrite
  history; preserve the old commits and use an explicit `--force-with-lease`.
- `main` is upstream `main` merged with upstream's draft PR #429
  (`sw-renderer`) and this port. Bring upstream in with a merge, not a
  rebase: the branch is published.
- Check `git remote -v` before pushing: a local checkout may name the
  fork (`zenibako/Butterscotch`) `fork` and upstream ButterscotchRunner
  `origin`, while a cloud clone's `origin` is the fork. Push to the fork
  only when asked, never to upstream, and never open PRs or comment upstream on
  the user's behalf.
- A push from a remote checkout can be stopped by a permission check in
  the session driving it. Then give the user the exact `git push -u
  <fork remote> <branch>` to run there rather than looking for another route.
- Commits use the GitHub noreply address set in each repo's local config;
  leave it as it is.

## Pull requests on the fork

- Work goes to `main` through PRs on `zenibako/Butterscotch`, one topic per
  PR. A follow-up topic gets its own branch from `main` and its own PR,
  rather than riding on an open one.
- **Check the PR is still open before pushing to its branch.** PR #4 was
  merged while follow-up fixes were being pushed to it, and they never
  reached `main`. After a merge, put further work on a new branch from
  `main`.
- Read [PR preparation](references/pr-preparation.md) when creating or refreshing
  a PR, especially after fork history changes or for visible UI changes. It
  covers the current base, signed replay, post-conflict checks, screenshot
  provenance and durable image links, and user-controlled description wording.
- Follow the user's authorship instructions and supplied summary. Explicit
  authorization to add screenshots/technical visual details covers those
  additions; preserve other prose and do not publish unrelated comments.
- CI runs about 33 checks: desktop gcc and clang, C++98, MSVC 4.0,
  GCC 2.95, Web, the consoles and the Analogue Pocket build. They build
  with warnings as errors, so the usual failures are:
  - a helper or variable only used under one configuration (profiling, a
    platform): mark it `MAYBE_UNUSED` (`src/common.h`) rather than
    deleting it;
  - `ull` suffixes on 64-bit constants, which MSVC 4.0 cannot read;
  - two `for (int i ...)` loops in one function, which MSVC 4.0 treats as
    one variable declared twice;
  - a platform hook the Web build does not provide.
  Reproduce with the desktop CMake build and `-DWERROR=ON` before pushing.
- For review fixes, thread resolution, CI monitoring and an authorized merge,
  read [PR follow-through](references/pr-follow-through.md). It covers existing
  authorization, rate-limited reviews and verification of external mutations.

## Syncing with upstream

First identify remotes by URL: `origin` may be the fork, not the original
runner. Refreshing a topic onto current fork main and merging the runner's
upstream are different operations; use [PR preparation](references/pr-preparation.md)
for baseline selection and rewritten fork history. The steps below apply to an
authorized integration of the original upstream. Preserve published history
with a signed local merge. Do not use GitHub's "Sync fork" button: it can merge
without your signature or offer to discard the fork's own commits.

1. **Capture reference frames first**, with the `undertale_pc` built from
   the tree as it is (see the butterscotch-pocket-verify skill): a new game,
   the user's save in an overworld room, and a battle. They are the only
   way to tell afterwards whether the merge changed what is drawn.
2. **Merge:** fetch the identified upstream remote, then
   `git merge -S --no-ff <upstream-remote>/<branch>`. Inspect ancestry first.
   If the clone is shallow, deepen that remote as needed; do not mistake
   rewritten history for a shallow checkout.
3. **Make it build.** The port's Makefile lists Butterscotch's sources
   itself and does not read `CMakeLists.txt`, so read that file's diff for
   new source directories, vendored libraries and options, and mirror them
   in `src/openfpga/Makefile`. The first sync (83 commits, 2026-10-08)
   needed `vendor/miniz` added and the physics stubs
   (`src/physics/physics.c` and `src/physics/disabled/physics_disabled.c`,
   since `ENABLE_PHYSICS` stays off).
4. **Expect the renderer interface to have moved.** The software renderer
   comes from draft PR #429, which upstream does not build, so a change to
   `src/renderer.h` reaches `src/sw/` as a compile error for us to fix.
   That first sync changed `drawSpritePart` to take a fractional source
   rectangle. When #429 itself merges upstream, `src/sw/` will conflict
   with this branch's changes to it throughout; treat that as its own job.
5. **Check:** `make -C src/openfpga undertale_pc`, then compare against
   the reference frames. Identical is the expected result; a difference is
   either an upstream behaviour change (find the commit and say so) or a
   bad merge. Then `make -C src/openfpga lint`, and run the frame scripts
   through `make asan` if anything in `src/sw/` or the loaders changed.
6. **Build for the device and have it run once** before pushing: the
   desktop cannot show memory, timing or card behaviour, and upstream code
   has not been through the Pocket before.
7. **Push** only when asked, to the identified fork remote's topic branch,
   and open the PR against the intended fork base. Prepare visual evidence
   using [PR preparation](references/pr-preparation.md).
