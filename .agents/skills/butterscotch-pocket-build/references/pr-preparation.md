# Prepare a current, visually reviewable Pocket PR

Use this for a requested PR or branch refresh. Read repository/user instructions
and the existing design and results relevant to the change. Creating a PR does
not authorize merging, deployment, unrelated upstream integration or external
comments.

## Establish the actual baseline

Inspect `git status`, remotes by URL, default branch, write permission and any
existing PR before changing branches. Remote names are not identities: `origin`
may be the user's fork or the original runner. Record which remote is the fork
and which is upstream. Fetch the intended PR base, then inspect ancestry and
`git log --left-right HEAD...<fork-remote>/<base>` before selecting a strategy.

For normal fork advancement, prepare the topic against current fork main and
preserve newer menu, renderer, native-script and platform behavior while
resolving conflicts. A capture from the old checkout is not a baseline for the
refreshed fork; unrelated changes can otherwise appear to belong to the PR.

If fetched history was rewritten, preserve the old signed commit/branch and
local changes. Prepare a fresh topic branch from the current fork base in an
isolated worktree and replay only the intended topic commits with GPG signing.
Inspect the resulting diff and commit list: the PR should contain the topic,
not the old lineage. Do not reset the original checkout, rebase published
history or force-push to make it fit. If safe replay is ambiguous, preserve the work, explain the blocking choice
and continue independent validation. Follow existing permission requirements
for history changes.

Integrating the **original runner's upstream** is a separate operation. When
requested, fetch the identified upstream remote and make a signed merge rather
than rebasing published port history. Follow the build skill's upstream checks;
refreshing a PR against the fork does not implicitly request this merge.

## Revalidate the integrated code

Use a fresh named build root after changing SDK/compiler/flags. Read the current
base's verification guidance and choose its relevant fixtures. New renderer or
native optimizations may interact with otherwise unchanged topic code. After
conflict resolution or another source edit, rebuild affected games, run the
fixtures (not just their build targets), lint, and recapture affected scenes.
For an integration defect, add a focused failing regression before the fix.

Compare unchanged scenes against the matching current baseline; inspect intended
visual changes directly. Check both 320x240 and 640x480 where affected, including
an in-game dialogue rather than only an instruction screen. For input prompts,
trace the displayed physical button to the actual input handler. Text-table,
generated-text and sprite prompts can use different paths. Report a script or
mapping check separately from an actual capture of that scene.

## Prepare visual evidence

For user-visible changes, prepare a small set of useful screenshots while
validating, even if publication has not yet been authorized. Prefer a readable
before/after pair when it explains the change; label after-only evidence clearly.
Use the same game/version, initial save, seed, route and mode for comparisons.
Never substitute a different scene or old-history baseline without labeling it.

Record the source commit, game/version, frame or scene, resolution, seed,
input script/recording and relevant build/runtime flags with the captures.
Use a small manifest beside committed images when those details are too long
for captions. Keep desktop, emulator and physical Pocket evidence distinct.
Inspect every final PNG. Preserve the native pixels; use nearest-neighbor
integer enlargement for small pixel-art captures and label that enlargement.

Use GitHub attachments when a supported upload route is available. A durable
fallback is a small set of PNGs plus capture metadata under a topic directory
such as `docs/visuals/<topic>/`, committed and signed with the work. Include only
review evidence, never game data, texture/audio packs or progress saves. Link to
a pushed immutable commit so later branch changes do not silently replace the
images. Local paths, chat-only attachments and temporary filesystem URLs are
not usable PR image links.

## Publish within the user's authorization

Preserve user-supplied title/summary wording and existing description content.
When prose is reserved for the user, offer an outline and use their supplied
wording. If they explicitly authorize screenshots or technical visual details,
append those images and factual captions within that scope without asking for
the same permission again. This does not authorize unrelated prose or comments.

A visual description consists of the user's summary, labeled screenshots,
brief scene/configuration captions and explicit verification limits. State
which physical button is highlighted when that is the change. A desktop image
does not establish Pocket timing, hardware behavior or SD-card correctness.

When updating an existing PR, verify it is still open before pushing. After
pushing, check the
expected head, published image URLs and final description; verify that images
are accessible, not just that Markdown was accepted. Attach a created PR to the
current Codex task. Follow [PR follow-through](pr-follow-through.md) for CI,
review feedback and a separately authorized merge.
