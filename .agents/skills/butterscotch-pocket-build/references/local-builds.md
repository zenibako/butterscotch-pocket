# Local development builds

## Where things are

```
undertale-pocket/                 not a git repo; holds the pieces below
├── env.sh                        source before any RISC-V build on macOS
├── butterscotch-pocket/          THIS REPOSITORY: fork of ButterscotchRunner/Butterscotch; trunk is `main` (it was `pocket` until 2026-10-10)
│   ├── src/                      Butterscotch itself; src/sw is the software renderer the port draws with
│   └── src/openfpga/             THE PORT
│       ├── main.c, Makefile, README.md
│       ├── platform/             of_* glue: video, audio, saves, log, bench
│       ├── tools/                mktexpack.c, mkmusic.c, mkcompare.sh, sdcopy.sh
│       ├── dist/                 Pocket core definition (core.json, data slots, instance JSON)
│       ├── platform/game_<game>.c   what differs per game; the rest of platform/ is shared
│       ├── games/<game>/         the user's own game data (data.win, music/) and the
│       │                         packs built from it (textures.bin, music.bin); gitignored.
│       │                         Undertale's was in src/openfpga/ itself until 2026-10-13
│       └── out/                  everything the build writes (gitignored):
│           └── build/pocket/butterscotch/   assembled SD card tree
├── openfpgaSDK/                  the openfpgaOS SDK, read only; SDK_ROOT points here by default
├── Diablo/                       reference port; also the source of the v0.9 SDK + runtime
└── screenshots/                  captures sent to the user
```

**Which machine.** Build in `$CHECKOUT`; the card may be on a different
machine from it. If `$CHECKOUT` is a `host:path`, run builds there over SSH
using the host name the user gave, since a `.local` name may not resolve
from every machine. A
cloud session has neither the game data nor a device toolchain, so it can
lint and build `undertale_pc` against a test game but cannot build the
Pocket ELF or check the real game's output.

Read `src/openfpga/README.md` first; it is kept current and describes each
subsystem. The SDK is a dependency, not part of this repository: never edit
it. Until 2026-10-08 the port lived inside a fork of the SDK
(`openfpgaSDK/src/undertale`, branch `undertale`) with Butterscotch cloned
inside it; that tree is superseded by this one.

## Building

```bash
# On the build host, CHECKOUT is its local checkout path.
cd "$CHECKOUT" && source ../env.sh # env.sh is in the parent workspace shown above
make -C src/openfpga undertale_pc   # desktop binary only; never touches build/
make -C src/openfpga                # RISC-V ELF + packs + SD tree (v0.7 core only)
make -C src/openfpga compare        # the above + Benchmark entries + v0.9 cores
```

Before importing dependency images or building, check free space on the host
and output volume. If containers will be used, also check Docker's actual
backing filesystem and available capacity, and VM memory. Archives on an
external disk do not move Docker's backing store.
Inspect existing containers, projects and local changes before proposing cleanup
or VM changes; do not prune images or relocate/restart the VM without permission.
Read a supplied dependency manifest and verify its checksums before using it.

`env.sh` supplies GNU sed (the SDK uses GNU-only `sed -i`) and normally selects
Homebrew's `riscv64-elf-gcc` with `USE_SDK_CONTAINER=0`. Inspect its actual settings
and the staged runtime's requirements; a previous Docker/keychain failure does
not establish that containers are unavailable now. Record the compiler actually
used rather than assuming host and container builds are equivalent.

A change to any header under `src/`, `src/*/` or `platform/` empties the
object directories a run builds into (the Makefile keeps a checksum of the
headers in each one's `.headers`), so objects are never left built against an
old struct layout: that links without complaint and crashes on the device.
The key also tracks `SAVESTATES`, `GAME`, `WADS`,
`FOCUS_BENCH` and `FOCUS_BUILD_ID` for native and PC objects. It is not a
complete compiler/flag fingerprint: use a fresh named `ROOT` when changing
other definitions, compiler, SDK or layout. Preserve existing build evidence.
The `save-states`/`native-saves` convenience target hardcodes its recursive
`ROOT`; passing a fresh `ROOT` to that target does not isolate its builds.
Use the explicit per-game `SAVESTATES=1 ... all` workflow in
[Native save-state builds](native-save-state-builds.md) for a fresh
release root, and keep that root through packaging and copy verification.
For a production release, explicitly select `FOCUS_BENCH=0`; build diagnostic
ELFs separately with an identifying `FOCUS_BUILD_ID`. A build-ID string alone
does not prove which ELF was packaged. The shared-root transition regression is
`tests/test_focus_build_cache.py`, run by `tools/test-snapshots.sh`.

The device build prints many warnings and notes; they are expected and not
a problem. What matters is that there are no errors. `make compare` output
should end with `Comparison cores added: ...`, and a build that prints
nothing after that line succeeded. To see only the lines that matter:

```bash
make -C src/openfpga compare 2>&1 | grep -E " error|undefined reference|Comparison|mkcompare:|\*\*\*"
make -C src/openfpga GAME=deltarune 2>&1 | grep -E " error|undefined reference|\*\*\*"
```

The first should print only the `Comparison cores added` line and the
second nothing.

## Two games, one core

For Memories or sleep/wake cards, read
[Native save-state builds](native-save-state-builds.md). The ordinary
build commands above do not select the save-state runtime or output tree.

The core is `zenibako.Butterscotch`, platform `butterscotch`, with one
entry per game on the Pocket. `GAME=deltarune` on any make target builds
Deltarune's program instead of Undertale's (WAD 17, data in
`games/deltarune/`, binary `deltarune_pc`); run
`tools/deltarune-setup.sh <game Resources folder> [chapter]` once first.
Every device build ends by reassembling the one SD tree from all games
built so far (`tools/mkcard.sh`), so a full card is `make && make
GAME=deltarune`. `compare` adds Undertale benchmark entries and v0.9
cores to that same tree.

Until 2026-10-08 the platform folder was `undertale`. A card set up before
then has `Assets/undertale/`, `Saves/undertale/` and possibly
`Cores/zenibako.Deltarune` with `Assets/deltarune/`; the save belongs in
`Saves/butterscotch/common/` now. Before the first launch of the new
core, copy the save files under `Saves/undertale/` there, or the game
starts as if nothing had been saved. Don't copy over a file that is
already there and newer: that one is progress made since.
