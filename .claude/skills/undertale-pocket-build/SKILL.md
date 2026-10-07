---
name: undertale-pocket-build
description: Build, package and deploy the Undertale port for the Analogue Pocket (Butterscotch on openfpgaOS) in this repo. Use this whenever the task involves building src/undertale, running make here, producing the SD card tree, copying to the Pocket's SD card (from this Mac or another one), regenerating textures.bin or music.bin, the v0.9 comparison cores, or committing work in this repo — even if the user only says "rebuild it", "copy it" or "is it ready to copy?".
---

# Building and deploying the Undertale Pocket port

## Where things are

```
undertale-pocket/                 not a git repo; holds the pieces below
├── env.sh                        source before any RISC-V build on macOS
├── openfpgaSDK/                  git repo (fork of openfpgaOS/openfpgaSDK), branch `undertale`
│   ├── src/undertale/            THE PORT (everything we own)
│   │   ├── main.c, Makefile, README.md
│   │   ├── platform/             of_* glue: video, audio, saves, log, bench
│   │   ├── tools/                mktexpack.c, mkmusic.c, mkcompare.sh, sdcopy.sh
│   │   ├── butterscotch/         SEPARATE git repo, branch `openfpga` (gitignored here)
│   │   ├── data.win, music/      the user's own game data (gitignored)
│   │   └── textures.bin, music.bin   generated packs (gitignored)
│   ├── dist/undertale/           Pocket core definition (core.json, data slots, instance JSON)
│   └── build/pocket/undertale/   assembled SD card tree (generated)
├── Diablo/                       reference port; also the source of the v0.9 SDK + runtime
└── screenshots/                  captures sent to the user
```

Read `src/undertale/README.md` first; it is kept current and describes each
subsystem. The SDK's own `CLAUDE.md` says not to edit `src/sdk/`, `scripts/`
or `runtime/`; keep changes inside `src/undertale/` and `dist/undertale/`.

## Building

```bash
cd undertale-pocket && source env.sh
make -C openfpgaSDK/src/undertale undertale_pc   # desktop binary only; never touches build/
make -C openfpgaSDK/src/undertale                # RISC-V ELF + packs + SD tree (v0.7 core only)
make -C openfpgaSDK/src/undertale compare        # the above + Benchmark entries + v0.9 cores
```

`env.sh` matters: it puts GNU sed on the PATH (the SDK scripts use GNU-only
`sed -i`) and sets `USE_SDK_CONTAINER=0` so the build uses Homebrew's
`riscv64-elf-gcc` instead of the SDK's Docker image, which cannot be built
here because Docker's keychain access fails in non-interactive sessions.

The Makefile does not track header or flag changes. After editing a header,
or changing `-D` flags, remove the affected objects first:

```bash
rm -rf openfpgaSDK/.obj/undertale-pc/butterscotch/src/sw     # desktop, renderer only
rm -rf openfpgaSDK/.obj/undertale openfpgaSDK/.obj/undertale-v09   # device, everything
```

`make compare` output should end with `Comparison cores added: ...`. A build
that prints nothing after that line succeeded; filter the noise with
`| grep -E " error|undefined reference|Comparison|\*\*\*"`.

## The cardinal rule: don't rebuild under a copy

`make` and `make compare` delete and recreate `build/pocket/undertale/`. The
user often copies that tree to the SD card from another machine with rsync,
which takes a minute or more. Rebuilding mid-copy hands them a mixed tree.

- Use `make undertale_pc` for desktop iteration; it never touches `build/`.
- Before a device rebuild, check nobody is pulling:
  `pgrep -f 'rsync --server' | wc -l` must be 0.
- After rebuilding, tell the user the tree is ready and that you will leave
  it alone. If they said they are about to copy, do not rebuild until they
  confirm the copy is done.

## Getting it onto the SD card

Three routes, in order of preference:

1. **Card in this Mac mini:** `make -C openfpgaSDK/src/undertale compare-copy`
   (or `copy` for the single core). This runs `tools/sdcopy.sh`, which waits
   for the card, mounts it, copies, removes `._*` sidecar files, verifies,
   and unmounts. It prints a specific message on each failure.
2. **Card in a second Mac:** SSH from here cannot touch the card
   (macOS blocks removable volumes for remote logins; that restriction is
   the user's and is not to be worked around). Give the user these to run in
   a Terminal on that Mac:

   ```bash
   rsync -rc --exclude '._*' --exclude '.DS_Store' <user>@<build-mac>:<path to>/openfpgaSDK/build/pocket/undertale/ /Volumes/Pocket/
   dot_clean -m /Volumes/Pocket/Assets/undertale /Volumes/Pocket/Cores /Volumes/Pocket/Platforms
   diskutil eject /Volumes/Pocket
   ```
3. **Manual:** `cp -R build/pocket/undertale/{Cores,Assets,Platforms} /Volumes/Pocket/`

Plain `copy` after a `compare-copy` leaves stale v0.9 cores on the card; use
`compare-copy` again if those cores should stay current.

A card reader on an idle, headless Mac can fail to notice a card inserted
while the machine is idle. `sdcopy.sh` declares user activity to
wake it; if no disk appears at all, the card needs reseating.

## What is on the card

| File (Assets/undertale/common) | Slot | Source |
|---|---|---|
| `os.bin`, `undertale_os.ini`, `undertale.elf` | 1, 2, 3 | SDK runtime, dist, build |
| `data.win` | 4 | the user's copy (`game.ios` on macOS, renamed) |
| `textures.bin` | 5 | `tools/mktexpack` from data.win |
| `music.bin` | 6 | `tools/mkmusic` from data.win + `music/*.ogg` |
| `undertale_0.sav` | 10 | written by the game (save archive) |

`undertale_0.sav` lives under `Saves/undertale/common/` on the card and is
never part of the build tree. `make import-save SAVE_DIR=<desktop save
folder>` builds one from a desktop save (`tools/mksave`); copying it to the
card replaces the Pocket's own progress, so only do that when asked.

Only slots 4–6 are available for data, and file names are limited to 23
characters. The OS config must not be called `undertale.ini`: the game
reads and writes a file of that name itself.

The core uses the **os25** bitstream from the SDK's own runtime (v0.7). Do
not switch it to os20 with that runtime; it boot-loops before the OS banner.

## Committing

Two repositories change in a typical task, and both need commits:

```bash
git -C openfpgaSDK/src/undertale/butterscotch add -A && git ... commit   # branch openfpga
git -C openfpgaSDK add -A src/undertale dist/undertale && git ... commit # branch undertale
```

- Conventional commit messages; commits are GPG signed automatically.
- `butterscotch/` is a nested clone that the outer repo ignores. Its
  `openfpga` branch sits on top of upstream's draft PR #429 (`sw-renderer`).
- Both repos have a `fork` remote on GitHub (`zenibako/openfpgaSDK` branch
  `undertale`, `zenibako/Butterscotch` branch `openfpga`); `origin` is
  upstream. Push to `fork` only when asked, and never open PRs or comment
  upstream on the user's behalf.
- Commits use the GitHub noreply address set in each repo's local config;
  leave it as it is.
