# Add your own game files

Butterscotch Pocket includes the core and game programs. You supply the game
data from your own computer installation. The desktop preparation tool converts
its textures and audio into a new folder ready to copy to your SD card. The
Pocket Sync plugin can install converted files directly on the card, as
described below. Neither downloads games, changes your computer installation
or writes saves.

The supported targets are **Undertale v1.08** and **Deltarune Chapter 1**.
Checking a data file's bytecode version does not establish compatibility with
other game releases or chapters.

## 1. Install the core

Download **`butterscotch-pocket.zip`** from the [latest release](https://github.com/zenibako/butterscotch-pocket/releases/latest), then choose either installation method:

- **Pocket Sync:** Connect your Pocket or insert its SD card, click
  **Connect to Pocket**, and select the card's root folder (the one containing
  `Assets`, `Cores` and `Saves`). Drag the Butterscotch Pocket **core ZIP** onto
  the Pocket Sync window and follow its install prompts. This works before
  Butterscotch appears in its catalog. Once listed, you can also install it
  from Pocket Sync's core browser.
- **Manual:** Extract the core ZIP into the root of your Pocket SD card,
  merging its `Cores`, `Assets` and `Platforms` folders with the existing folders.

Installing the core does not install your commercial game data. Complete the
preparation steps below for either installation method. The game-data
preparation bundle is a separate computer-side tool; extract it on your
computer, not on the SD card. Do not drag that tools archive into Pocket Sync.

Download the preparation bundle from the same release: `prepare-data-<os>-<arch>.tar.gz`.
Use `darwin` for macOS, `windows` for Windows or `linux` for Linux, and
`arm64` for an Apple silicon Mac or ARM computer, or `amd64` for an Intel/AMD
64-bit computer. It contains a standalone `prepare-data` executable
(`prepare-data.exe` on Windows). Python, a C compiler and the openfpgaOS SDK
are not required. The same Go conversion library also runs in WASM.

## 2. Select your installed game

For Undertale, select the installed game folder containing `data.win`, or
select `UNDERTALE.app` on macOS. Selecting its exact `game.ios` or Linux
`game.unx` file also works. Keep the game's original `.ogg` music alongside it
or in its `music` folder. An installer executable or a ZIP is not a data file.

For Deltarune, select the installed game folder containing `chapter1_windows`,
`chapter1_mac` or `chapter1_linux` and the shared `mus` folder. On macOS you
can select `DELTARUNE.app`. A Chapter 1 folder or its exact data file also works
when the shared `mus` folder is beside the chapter folder.

Deltarune needs all of these original files:

- Chapter 1's `data.win` or `game.ios`.
- `audiogroup1.dat` in the chapter folder.
- `lang/lang_en.json` in the chapter folder.
- Chapter music and the shared `mus` folder's `.ogg` files.

The tool gathers the chapter's referenced music and includes its language
and additional sound data in the generated audio pack. You do not need to
rename or manually copy those extra files to the SD card.

## 3. Prepare a new output folder

Open a terminal in the extracted preparation bundle. Replace the example
paths with your own. Keep paths containing spaces inside quotes.

macOS or Linux:

```sh
./prepare-data --game undertale --source "/path/to/UNDERTALE.app" --output "prepared-undertale"
./prepare-data --game deltarune --source "/path/to/DELTARUNE.app" --output "prepared-deltarune"
```

Windows, in PowerShell:

```powershell
.\prepare-data.exe --game undertale --source "C:\Games\UNDERTALE" --output "prepared-undertale"
.\prepare-data.exe --game deltarune --source "C:\Games\DELTARUNE" --output "prepared-deltarune"
```

Run only the command for the game you want to install. Choose an output folder
that does not already exist and is outside the original game installation.
If preparation fails, fix the reported missing or invalid file and rerun it.
A failed conversion does not publish a partial output folder. To prepare again,
choose a new output folder; the tool does not overwrite an existing folder.

## 4. Install the prepared data

After the tool says `Ready`, choose either method:

- **Pocket Sync:** In the prepared output folder, compress the **Assets**
  folder into a ZIP using your computer's file manager. Drag this new ZIP onto
  the connected Pocket Sync window, review the selected `Assets` paths, and
  click **Confirm**. Include all three generated game files.
- **Manual:** Copy the output folder's **Assets** folder into the root of
  your Pocket SD card and merge folders.

For Pocket Sync, check that opening the ZIP shows `Assets` immediately, with
`butterscotch/common` inside it. Do not compress the enclosing
`prepared-undertale` or `prepared-deltarune` folder: that would add an extra
directory above `Assets`. The game-data ZIP contains no core, tools or saves.
It is a private archive for your own card; do not upload it to a release.

The resulting paths on the card are:

| Game | Files under `/Assets/butterscotch/common/` |
|---|---|
| Undertale | `data.win`, `textures.bin`, `music.bin` |
| Deltarune Chapter 1 | `deltarune.win`, `dr_textures.bin`, `dr_music.bin` |

These generated texture and audio packs belong together with that game's data
file. Copy all three files. When changing the game data, prepare all three
again. Your core's matching app, OS and FPGA runtime come from the core ZIP;
preparing game files does not replace them.

Keep the `PREPARED-DATA.json` report on your computer if you need to identify
the data hash and generated files later. It is not needed on the card.

Unmount the card after copying, put it in the Pocket, and open Butterscotch.
Select only a game whose data you installed. Your progress is under
`/Saves/butterscotch/common/`; the preparation output contains no `Saves`
folder. Keep your original games and generated data private.

Pocket Sync installs the core and copies prepared files. Its optional plugin
can also run the converter, as described below; neither method obtains your purchased game data. Installing an updated
core does not convert a different game version. If a release asks you to
prepare the data again, repeat steps 2–4 with a new output folder.

Pocket Sync's [installation and ZIP import instructions](https://github.com/neil-morrison44/pocket-sync#installation)
describe connecting a card and importing archives. Butterscotch's catalog
listing must point to the **core ZIP**, keeping the computer-side tools and
private game-data archives separate.

## Pocket Sync plugin

The optional plugin runs the same conversion library inside regular Pocket Sync.
In **Plugins → Add plugin via URL**, enter:

```text
https://github.com/zenibako/butterscotch-pocket
```

Choose **Verify & install plugin**. This requires a published stable release
containing both `plugin.wasm` and `plugin.json`; a source checkout or a draft
release is not installable through this dialog. The core and plugin use the
same repository URL, but installing one does not install the other.

**Release availability:** The direct-install workflow below is for the next
plugin release. The currently published v0.7.0 plugin still uses the earlier
separate-input/output workflow. Adding the repository URL installs the latest
published stable plugin; this source change is not available there until a new
version is published. The desktop CLI steps above remain available.

The plugin reads only files under the connected SD-card root. It cannot open
an arbitrary path on your computer. Put original files directly into
`/Assets/butterscotch/common/`; do not create a per-game source folder or copy
an unexpanded `.app` bundle.

1. Install the core. Copy Undertale's original data file into `common/` as
   **`data.win`**, including when its original name is `game.ios` or `game.unx`.
   Copy original `.ogg` music beside it or into `common/music/`.
   For Deltarune Chapter 1, copy its original data file as **`deltarune.win`**,
   its `audiogroup1.dat` into `common/`, its `lang/lang_en.json` into
   `common/lang/`. Copy shared `mus/` into `common/mus/` and put **all**
   chapter `.ogg` files into that same `common/mus/` folder. Keep Deltarune
   audio out of the common root: filenames such as `audio_intronoise.ogg`
   also occur in Undertale and would overwrite its originals. Rename
   Deltarune's data file **before copying** so it
   cannot overwrite Undertale's `data.win`.
2. Run **Butterscotch Game Data** in Pocket Sync and select the game. Choose
   **Install and keep originals** (the first, default option) or
   **Install and discard originals**. Review the selected game and the
   confirmation's explanation of which redundant source files may be removed.
3. Keep Pocket Sync open until installation finishes. The plugin installs all
   three game files directly into `/Assets/butterscotch/common/` using the
   filenames in the table above. No extra copy or merge is needed. The game's
   original data file is used unchanged by the core; the converter creates
   the texture and audio packs alongside it.

For a default Steam installation on macOS, replace `/Volumes/POCKET` below
with your connected card's path. Run only the block for the game you own.
These commands copy individual data files under distinct names rather than
merging an entire app's Resources folder over the other game's files.

```sh
# Undertale: copy data and music directly into common.
card_common="/Volumes/POCKET/Assets/butterscotch/common"
ut_resources="$HOME/Library/Application Support/Steam/steamapps/common/Undertale/UNDERTALE.app/Contents/Resources"
mkdir -p "$card_common"
cp "$ut_resources/game.ios" "$card_common/data.win"
find "$ut_resources" -maxdepth 1 -type f -name '*.ogg' -print | while IFS= read -r file; do
    if [ -f "$file" ]; then cp "$file" "$card_common/"; fi
done
if [ -d "$ut_resources/music" ]; then
    ditto "$ut_resources/music" "$card_common/music"
fi
```

```sh
# Deltarune Chapter 1: distinct data name; all audio goes into mus.
card_common="/Volumes/POCKET/Assets/butterscotch/common"
dr_resources="$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app/Contents/Resources"
dr_chapter="$dr_resources/chapter1_mac"
mkdir -p "$card_common/mus"
cp "$dr_chapter/game.ios" "$card_common/deltarune.win"
cp "$dr_chapter/audiogroup1.dat" "$card_common/audiogroup1.dat"
ditto "$dr_chapter/lang" "$card_common/lang"
ditto "$dr_resources/mus" "$card_common/mus"
find "$dr_chapter" -maxdepth 1 -type f -name '*.ogg' -print | while IFS= read -r file; do
    if [ -f "$file" ]; then cp "$file" "$card_common/mus/"; fi
done
```

On Windows or Linux, copy the installed files into the same flat layout with
your file manager. Use `data.win` for Undertale and `deltarune.win` for
Deltarune, regardless of the original platform's data-file name. Deltarune's
chapter folder itself does not belong on the card: its data, bank, language
and music files go into the locations above. Put both chapter and shared
Deltarune audio in `common/mus/`; Undertale audio stays in `common/` or
`common/music/`.

**Keep originals** preserves the source files, so you can install again
without moving or removing a previous output folder. **Discard originals**
means removing redundant source files only after successful installation:
identified audio for the selected game, plus its Deltarune bank and language
file when applicable. The plugin retains `data.win` and `deltarune.win` because
the core requires them. It also retains audio needed by the other game and
unrecognized files. It never deletes `common/` or an entire music directory.
Your computer's Steam installation and saves are untouched. Keep enough free
space for originals, conversion work and any previous installed files during
replacement.

Before deleting a source file, the plugin checks its hash again. A changed
file is kept and reported as a cleanup warning. If conversion or installation
fails, the plugin preserves originals and restores the previously installed
files. If restoration also fails, it keeps recovery backups and reports their
location; preserve those files for recovery. If installation succeeds but
removing redundant originals or temporary files fails, the plugin reports a
cleanup warning; the converted game remains installed. Review any reported
remnants before removing them manually.

Temporary work and recovery remnants are hidden under
`/Assets/butterscotch/.butterscotch-install-*`, beside `common/`; there are no
separate user-facing input or prepared-output folders. The plugin keeps its
installation report at `/Assets/butterscotch/common/.butterscotch-<game>.json`
(for example, `.butterscotch-undertale.json`). The desktop CLI continues to
write `PREPARED-DATA.json` in its separate output folder and refuses existing
output folders. Keep all original and converted game data private; never
upload it to a release.

## Building from source

From this repository root, with Go 1.25 or newer:

```sh
go test ./...
go build -trimpath -o prepare-data ./cmd/prepare-data
```

Windows users can choose `-o prepare-data.exe`. To cross-compile, set `GOOS`
and `GOARCH` for the intended host and `CGO_ENABLED=0`. No C toolchain is used.

WASI CLI (run in a WASI host with the input/output directories mounted):

```sh
GOOS=wasip1 GOARCH=wasm go build -trimpath -o prepare-data.wasm ./cmd/prepare-data
```

Pocket Sync / Extism plugin, using TinyGo 0.43.0 and Binaryen 133:

```sh
tinygo build -p 2 -trimpath -target wasip1 -buildmode=c-shared -o plugin.wasm ./cmd/pocket-plugin
```

Use TinyGo for the plugin: standard Go's WASI filesystem handling does not work
reliably with Pocket Sync's relative preopen names. The CLI WASM uses standard Go.
Place `plugin.json` beside the plugin WASM. `go test ./...` executes native and
WASI conversions against synthetic fixtures, compares their game files, and
runs plugin exports in an Extism host when `tinygo` is on PATH (or set `TINYGO`
to its executable). Set `REQUIRE_PLUGIN_TESTS=1` to fail instead of skipping when
TinyGo is missing. Run `go test -C third_party/vorbis ./...` for the patched
decoder's own regressions. To run independent C converter comparisons, set `BUTTERSCOTCH_SOURCE` to a checkout of the pinned fork and `REQUIRE_REFERENCE_TESTS=1`; Python and a host C compiler are required for that check.
The old `prepare_game.py`, `mktexpack.c` and `mkmusic.c` remain developer references in [the fork](https://github.com/zenibako/Butterscotch/tree/main/src/openfpga/tools);
they are not dependencies of the Go executable or plugin.

The Vorbis decoder is pure Go. The generated synthetic Vorbis fixture matches
the old decoder in current checks; this does not prove byte-identical decoding
for every commercial track. Full-game compatibility and resource use still need
on-device tests with the user's own supported game files.

See [Building and releasing](docs/BUILD.md) for the pinned core, SDK and runtime contract.
