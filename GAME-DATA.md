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

The direct-install workflow below requires **v0.7.1 or newer**. Update the
plugin if you installed v0.7.0, which used separate input/output folders.

1. Install the core, then copy original resources into the connected card:
   - **Undertale:** `/Assets/butterscotch/common/undertale/`, with its native
     `data.win`, `game.ios` or `game.unx` and original music beside it or in
     `music/`.
   - **Deltarune:** `/Assets/butterscotch/common/deltarune/`, with
     `chapter1_mac`, `chapter1_windows` or `chapter1_linux` alongside shared
     `mus/`. Keep the chapter's data, `audiogroup1.dat`, `lang/lang_en.json`
     and music together. Preserve native filenames; no data-file renaming is
     needed. Separate source folders prevent music filename collisions.
2. Run **Butterscotch Game Data**, select the game, then choose
   **Install and keep originals** (first/default) or
   **Install and discard originals**. Review the source folder shown in the
   confirmation.
3. Keep Pocket Sync open until installation finishes. All three game files
   are installed directly into `/Assets/butterscotch/common/` with the names
   in the table above. No manual copy or merge is needed.

The plugin reads only the connected card, so copy resources out of any `.app`
bundle first. For default Steam installations on macOS, replace
`/Volumes/POCKET` with your card's path and run only the commands you need:

```sh
# Undertale: copy Resources contents into the dedicated source folder.
mkdir -p "/Volumes/POCKET/Assets/butterscotch/common/undertale"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/Undertale/UNDERTALE.app/Contents/Resources" "/Volumes/POCKET/Assets/butterscotch/common/undertale"

# Deltarune: preserve the chapter and shared music directory arrangement.
mkdir -p "/Volumes/POCKET/Assets/butterscotch/common/deltarune"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app/Contents/Resources/chapter1_mac" "/Volumes/POCKET/Assets/butterscotch/common/deltarune/chapter1_mac"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app/Contents/Resources/mus" "/Volumes/POCKET/Assets/butterscotch/common/deltarune/mus"
```

On Windows or Linux, copy the equivalent installed resources into those
source folders with your file manager. Do not copy an installer or ZIP.

**Keep originals** preserves the source folder for another install.
**Discard originals** removes the entire selected `undertale/` or `deltarune/`
source folder only after successful installation. It leaves the other game's
source folder, installed game files, your computer's Steam installation and
saves untouched. Keep enough free space for originals, conversion work and
previous installed files during replacement.

If conversion or installation fails, originals are kept and previous installed
files are restored. If restoration fails, preserve the recovery backups at
the reported location. A failure to discard originals or remove temporary
files produces a cleanup warning after a successful install; the game remains
installed.

Work and recovery remnants are hidden under
`/Assets/butterscotch/common/.butterscotch-install-*`; the plugin report is
`/Assets/butterscotch/common/.butterscotch-<game>.json`. There are no separate
top-level input or prepared-output folders. The desktop CLI still writes
`PREPARED-DATA.json` in a separate output folder and refuses existing output
folders. Keep original and converted game data private.

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
