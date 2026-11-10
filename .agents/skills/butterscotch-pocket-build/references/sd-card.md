# SD-card deployment and layout

## Local or remote card access

Run [pocket_card.py](../../../../src/openfpga/tools/pocket_card.py) from the
checkout with Python 3.10 or later. Use the absolute mount on the card host,
such as `/Volumes/Pocket SD`. Omit `--host` for a card mounted on the
session host. For another machine, use its established SSH alias with
`--host`. The helper sends its code for the operation, so that host needs
Python, noninteractive SSH authentication and authorized access to the
mounted card. If its login PATH lacks Python, pass the existing absolute
executable with `--remote-python`.

Reuse the host and mount already supplied in the conversation. Ask only for
missing details. Perform the requested transfer and verification when access
works. A build ZIP request alone does not authorize writing to the card.

If macOS denies removable-volume access over SSH, use an already authorized
interactive agent session on the card host or an existing authorized mount.
For a card-side session, transfer the finished ZIP through ordinary host
storage first, or transfer the privately collected save back through it.
Ask for a session with card access if neither route exists. Leave OS access
settings and privileges alone.

## Install a finished build

Use the specific packaged ZIP the user asked to install. For a ZIP on a
separate build host, first copy it to private storage the session can read
and compare it with the build's SHA-256. The install command accepts standard
Butterscotch and Bullet Hell core ZIPs.

```sh
python3 src/openfpga/tools/pocket_card.py install \
  --host "$CARD_HOST" --card "$CARD" --zip "$ZIP" --sha256 "$BUILD_SHA256"
```

Pass the build's published checksum when available, otherwise omit
`--sha256`. The helper verifies the same ZIP at both ends, stages the
overlay, and checks file contents on the card. It reports the package hash,
core versions and verified file count. It preserves Saves, Memories, game
data and unrelated cores. Use the build label and hash to identify the
exact ZIP when versions are reused.

When asked to collect and install together, collect and verify the snapshot
before installing. Use `--unmount` when the user is done with the card. A normal unmount keeps
the card discoverable by the Mac next time. If verification fails, keep the
card mounted. If an interrupted install leaves `.pocket-card-install`,
inspect the staging directory and its `recovery.json`, if present, before
retrying. If verification
succeeds but unmount fails, report both outcomes. The verified files are
already installed.

The legacy `make copy` and `compare-copy` targets assemble a tree before
copying and can remove stale core folders or move the platform cache. Use
them for a requested build-and-copy workflow. An idle Mac reader may need
the user to reseat the card when the OS cannot see it.

## Collect a benchmark save

After the user finishes the run and quits through the Analogue menu to flush
save RAM, collect the selected log into a fresh private directory. Use
`bs_bullet` for all Bullet Hell, Chaos Saber and Flowey modes,
`deltarune_tiles` for the focused tiles log, or `undertale` or `deltarune`
for standard game logs.

```sh
SNAPSHOT_DIR=$(mktemp -d)
python3 src/openfpga/tools/pocket_card.py collect \
  --host "$CARD_HOST" --card "$CARD" --log bs_bullet --out "$SNAPSHOT_DIR/bs_bullet_1.sav"
python3 src/openfpga/tools/inspect_bench_save.py "$SNAPSHOT_DIR/bs_bullet_1.sav"
```

The snapshot keeps every byte, including NUL padding, and prints the
verified size and SHA-256. It leaves the source save in place and refuses
an existing output path. Pass earlier snapshots to the history checker to
separate new runs from older accumulated reports.

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
