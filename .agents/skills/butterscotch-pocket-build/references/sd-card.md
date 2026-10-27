# SD-card deployment and layout

## Getting it onto the SD card

Three routes, in order of preference:

1. **Card in the machine that has the checkout:** `make -C src/openfpga compare-copy`
   (or `copy` for the single core). This runs `tools/sdcopy.sh`, which waits
   for the card, mounts it, copies, removes `._*` sidecar files, verifies,
   and unmounts. It prints a specific message on each failure.
2. **Card in a second Mac:** SSH from the build machine
   cannot touch the card (macOS blocks removable volumes for remote
   logins; that restriction is the user's and is not to be worked around).
   Give the user these to run in a Terminal on that Mac:

   ```bash
   rsync -rc --exclude '._*' --exclude '.DS_Store' "$CHECKOUT/src/openfpga/out/build/pocket/butterscotch/" "$CARD/"
   dot_clean -m "$CARD/Assets/butterscotch" "$CARD/Cores" "$CARD/Platforms"
   diskutil unmount "$CARD"
   ```

   - `dot_clean -m "$CARD"` on the whole card prints "Operation
     not permitted" for `.Spotlight-V100`, `.Trashes` and
     `.TemporaryItems`. Those are macOS's own folders and the lines are
     harmless; the narrower paths above avoid them.
   - If the unmount is "dissented by" a `com.apple.Virtualization`
     process, the Claude desktop app has the card open for a session that
     was reading it. Once the copy has finished,
     `diskutil unmount force "$CARD"` is safe.
   - The rsync never deletes, so old benchmark ELFs and instance files
     stay on the card. That is harmless.
   - If `$CHECKOUT` is on a work branch, the user builds main
     with `git fetch <fork remote> && git switch --detach <fork remote>/main` after checking `git status` is
     clean, and switches back afterwards.
3. **Manual:** `cp -R src/openfpga/out/build/pocket/butterscotch/{Cores,Assets,Platforms} "$CARD/"`

**Checking what is on a card.** Every core's `core.json` says version
0.1.0, so it cannot tell builds apart. Compare file times against when the
change merged, `cmp` the ELF against a fresh build, or look for a string
the change added (`strings "$CARD/Assets/butterscotch/common/undertale.elf" | grep "Music only"`).

Plain `copy` after a `compare-copy` leaves stale v0.9 cores on the card; use
`compare-copy` again if those cores should stay current.

Unmount, never eject (`diskutil unmount`, as `sdcopy.sh` does): after an
eject a Mac does not see the card again until it is reseated, and
sometimes not then, and the user asked on 2026-10-09 for unmount to be
used so the card is found next time.

A card reader on an idle, headless Mac can fail to notice a card inserted
while the machine is idle. `sdcopy.sh` declares user activity to
wake it; if no disk appears at all, the card needs reseating.

## What is on the card

| File (Assets/butterscotch/common) | Slot | Source |
|---|---|---|
| `os.bin`, `undertale_os.ini`, `undertale.elf` | 1, 2, 3 | SDK runtime, dist, build |
| `data.win` | 4 | the user's copy (`game.ios` on macOS, renamed) |
| `deltarune.elf`, `deltarune_os.ini`, `deltarune.win`, `dr_textures.bin`, `dr_music.bin` | 3, 2, 4, 5, 6 | the same for the Deltarune entry |
| `textures.bin` | 5 | `tools/mktexpack` from data.win |
| `music.bin` | 6 | `tools/mkmusic` from data.win + `music/*.ogg` |
| `undertale_0.sav` | 10 | written by the game (save archive) |

`undertale_0.sav` lives under `Saves/butterscotch/common/` on the card and is
never part of the build tree. `make import-save SAVE_DIR=<desktop save
folder>` builds one from a desktop save (`tools/mksave`); copying it to the
card replaces the Pocket's own progress, so only do that when asked.

Only slots 4–6 are available for data, and file names are limited to 23
characters. The OS config must not be called `undertale.ini`: the game
reads and writes a file of that name itself.

The core uses the **os25** bitstream from the SDK's own runtime (v0.7). Do
not switch it to os20 with that runtime; it boot-loops before the OS banner.
