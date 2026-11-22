# Install Butterscotch Pocket

Butterscotch Pocket plays **Undertale v1.08** and, as an experiment,
**Deltarune Chapter 1** on the Analogue Pocket. No game files are included.
You build them from your own copy of the game.

**The core on its own won't start a game.** If you open Undertale and see
*"Couldn't load Undertale. The game data is missing."*, the core is installed
correctly and only step 2 below is left.

## Quick start

1. **Install the core.** Download `butterscotch-pocket.zip` from the
   [latest release](https://github.com/zenibako/butterscotch-pocket/releases/latest)
   and put it on your SD card (Pocket Sync or by hand, both below).
2. **Build your game files.** Your game's `data.win` must be converted into
   three files. Use the Pocket Sync plugin, or the `prepare-data` tool on
   your computer if you don't use Pocket Sync.
3. **Play.** Put the card in the Pocket, open **openFPGA → Butterscotch**
   and pick your game. Starting takes about 20 seconds.

You need one of these:

| Game | What you need |
|---|---|
| Undertale | Undertale **v1.08** for Windows or macOS, such as the Steam release. Console versions (Switch, PlayStation, Xbox) can't be used. |
| Deltarune | A Windows or macOS install of Deltarune that contains `chapter1_windows` or `chapter1_mac` and the shared `mus` folder. Only Chapter 1 is supported. |

When you're done, your card holds these three files for each game you set up,
next to the files the core ZIP installed:

```text
SD card
├── Assets/
│   └── butterscotch/
│       └── common/
│           ├── data.win          ← Undertale (built from your game)
│           ├── textures.bin      ← Undertale (built from your game)
│           ├── music.bin         ← Undertale (built from your game)
│           ├── deltarune.win     ← Deltarune (built from your game)
│           ├── dr_textures.bin   ← Deltarune (built from your game)
│           ├── dr_music.bin      ← Deltarune (built from your game)
│           └── os.bin, undertale.elf, …  (from the core ZIP)
├── Cores/
│   └── zenibako.Butterscotch/
└── Platforms/
    └── butterscotch.json
```

`textures.bin` and `music.bin` don't exist in your game folder. Only the
tool can make them, so copying the game's `data.win` by itself won't work.

Pick the path that fits you:

- [**A. With Pocket Sync**](#a-with-pocket-sync): no terminal needed.
- [**B. Without Pocket Sync**](#b-without-pocket-sync): unzip the core, run
  `prepare-data` once in a terminal, copy one folder.

---

## A. With Pocket Sync

[Pocket Sync](https://github.com/neil-morrison44/pocket-sync) is a free
desktop app for managing your Pocket's SD card.

### A1. Install the core

1. Download `butterscotch-pocket.zip` from the
   [latest release](https://github.com/zenibako/butterscotch-pocket/releases/latest).
2. Open Pocket Sync, click **Connect to Pocket** and select your SD card
   (the folder that holds `Assets`, `Cores` and `Saves`).
3. Drag `butterscotch-pocket.zip` onto the Pocket Sync window and confirm.

### A2. Install the plugin

1. In Pocket Sync, open **Plugins → Add plugin via URL**.
2. Enter `https://github.com/zenibako/butterscotch-pocket` and choose
   **Verify & install plugin**.

The plugin is separate from the core. Installing one doesn't install the other.

### A3. Copy your game onto the card

The plugin can only read the SD card, so first copy your game's files there,
into a new folder:

| Game | Copy into |
|---|---|
| Undertale | `Assets/butterscotch/common/undertale/` |
| Deltarune | `Assets/butterscotch/common/deltarune/` |

Where your game's files are (default Steam folders):

- **Windows, Undertale:** `C:\Program Files (x86)\Steam\steamapps\common\Undertale\`.
  Copy everything in it, including `data.win` and the `.ogg` music files.
- **Windows, Deltarune:** `C:\Program Files (x86)\Steam\steamapps\common\DELTARUNE\`.
  Copy the `chapter1_windows` and `mus` folders.
- **macOS:** in Steam, right-click the game → **Manage → Browse local files**,
  then right-click the app → **Show Package Contents** → `Contents/Resources`.
  For Undertale copy everything in `Resources`. For Deltarune copy
  `chapter1_mac` and `mus`.

On macOS you can also do it in Terminal. Replace `/Volumes/POCKET` with your
card's name:

```sh
# Undertale
mkdir -p "/Volumes/POCKET/Assets/butterscotch/common/undertale"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/Undertale/UNDERTALE.app/Contents/Resources" "/Volumes/POCKET/Assets/butterscotch/common/undertale"

# Deltarune
mkdir -p "/Volumes/POCKET/Assets/butterscotch/common/deltarune"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app/Contents/Resources/chapter1_mac" "/Volumes/POCKET/Assets/butterscotch/common/deltarune/chapter1_mac"
ditto "$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app/Contents/Resources/mus" "/Volumes/POCKET/Assets/butterscotch/common/deltarune/mus"
```

Keep the original file names. Don't copy an installer or a ZIP.

### A4. Build the game files

1. In Pocket Sync's plugins, run **Butterscotch Game Data**.
2. Pick your game, then **Install and keep originals** (recommended) or
   **Install and discard originals** to free the space afterwards.
3. Keep Pocket Sync open until it says the install finished.

The three game files go straight into `Assets/butterscotch/common/`. There's
nothing else to copy. Eject the card and play.

---

## B. Without Pocket Sync

You need a computer with an SD card reader and a terminal (Terminal on macOS,
PowerShell on Windows). You run one command, once per game.

### B1. Install the core

1. Download `butterscotch-pocket.zip` from the
   [latest release](https://github.com/zenibako/butterscotch-pocket/releases/latest).
2. Unzip it. You get three folders: `Assets`, `Cores` and `Platforms`.
3. Copy those three folders to the **root** of your SD card. When asked,
   **merge** them with the folders already there. Don't replace them, which
   would delete your other cores.

On macOS, Finder replaces folders instead of merging them. Use Terminal
instead, replacing `/Volumes/POCKET` with your card's name:

```sh
ditto ~/Downloads/butterscotch-pocket "/Volumes/POCKET"
```

### B2. Get the preparation tool

From the same release, download the `prepare-data` file for your computer:

| Computer | Download |
|---|---|
| Mac with Apple silicon (M1 or newer) | `prepare-data-darwin-arm64.tar.gz` |
| Intel Mac | `prepare-data-darwin-amd64.tar.gz` |
| Windows PC | `prepare-data-windows-amd64.tar.gz` |
| Linux PC | `prepare-data-linux-amd64.tar.gz` |

Extract it somewhere on your computer, not on the SD card. On Windows,
open PowerShell in your Downloads folder and run
`tar -xf prepare-data-windows-amd64.tar.gz`.

### B3. Build the game files

Open a terminal in the extracted folder. Run only the line for your game.
Keep the quotes, and use your own game's location if it isn't the default
Steam folder.

**macOS** (Terminal):

```sh
./prepare-data --game undertale --source "$HOME/Library/Application Support/Steam/steamapps/common/Undertale/UNDERTALE.app" --output prepared-undertale
./prepare-data --game deltarune --source "$HOME/Library/Application Support/Steam/steamapps/common/DELTARUNE/DELTARUNE.app" --output prepared-deltarune
```

**Windows** (PowerShell):

```powershell
.\prepare-data.exe --game undertale --source "C:\Program Files (x86)\Steam\steamapps\common\Undertale" --output prepared-undertale
.\prepare-data.exe --game deltarune --source "C:\Program Files (x86)\Steam\steamapps\common\DELTARUNE" --output prepared-deltarune
```

Wait until it prints `Ready`. If it reports a missing or invalid file, fix
that and run it again with a new `--output` name, because it never
overwrites an existing folder.

### B4. Copy the result to the card

The output folder (for example `prepared-undertale`) contains an `Assets`
folder. Copy that `Assets` folder to the root of your SD card and merge it.

On macOS:

```sh
ditto prepared-undertale/Assets "/Volumes/POCKET/Assets"
```

Check that the three files from the [card layout](#quick-start) are in
`Assets/butterscotch/common/`, eject the card and play. You can keep
`PREPARED-DATA.json` on your computer; the card doesn't need it.

---

## Troubleshooting

**"Couldn't load Undertale. The game data is missing. Cannot open data.win"**
The core is fine, but the game files aren't on the card yet. Do
[A3–A4](#a3-copy-your-game-onto-the-card) or [B2–B4](#b2-get-the-preparation-tool).
If you already did, check the files are in `Assets/butterscotch/common/`
itself, not in a subfolder such as `prepared-undertale/Assets/…` or
`undertale/`.

**I copied my game's `data.win` but it still won't start.**
The game's own `data.win` isn't enough. The tool also builds `textures.bin`
and `music.bin`, and all three must come from the same run. Copy all three.

**"Load error in Core" from the Pocket menu.**
Part of the core is missing or in the wrong place, usually because the ZIP
was copied as a folder (`butterscotch-pocket/Cores/…`) instead of its three
folders going to the card root. Redo [A1](#a1-install-the-core) or
[B1](#b1-install-the-core).

**My other cores disappeared after installing.**
The folders were replaced instead of merged. Reinstall those cores, then
install Butterscotch again by merging, or use Pocket Sync, which merges for you.

**macOS says `prepare-data` "cannot be opened" or "Apple could not verify" it.**
The tool isn't signed by Apple yet. Open **System Settings → Privacy &
Security**, scroll down and choose **Open Anyway**, then run the command
again. Or, in Terminal in the tool's folder, run
`xattr -d com.apple.quarantine prepare-data` once.

**Windows blocks `prepare-data.exe`.**
If SmartScreen appears, choose **More info → Run anyway**. In PowerShell
you can also run `Unblock-File .\prepare-data.exe` once.

**The tool says it "requires bytecode version".**
That copy of the game isn't a supported version. Only Undertale v1.08 and Deltarune Chapter 1 work. Update the game in
Steam and try again. Console versions can't be used.

**I can't find the plugin in Pocket Sync.**
It isn't in the plugin list yet. Add it by URL, as in
[A2](#a2-install-the-plugin).

**The game starts but slows down in places.**
Some heavy scenes are still slow. **Select → Performance Mode → Speed** (the
default) is the fastest setting.

Still stuck? Open the game, choose **Select → Debug → Save log to SD card**,
quit from the Analogue menu, and attach the `_1.sav` file from
`Saves/butterscotch/common/` to a
[new issue](https://github.com/zenibako/butterscotch-pocket/issues/new).

---

## Updating

Install a newer core the same way as in step A1 or B1. Your game files and
saves stay on the card, so you don't need to build them again unless the
release notes say so.

Your progress is saved in `Saves/butterscotch/common/`. Back up your SD card
before updating.

---

## Details

### Accepted game sources

For Undertale, `--source` (or the plugin's `undertale/` folder) can be the
installed game folder containing `data.win`, `UNDERTALE.app` on macOS, or the
exact `data.win`, `game.ios` or Linux `game.unx` file. The game's original
`.ogg` music must be beside it or in its `music` folder.

For Deltarune, it can be the installed game folder containing
`chapter1_windows`, `chapter1_mac` or `chapter1_linux` and the shared `mus`
folder, or `DELTARUNE.app` on macOS. A Chapter 1 folder or its exact data file
also works when the shared `mus` folder is beside it. The tool needs:

- Chapter 1's `data.win` or `game.ios`.
- `audiogroup1.dat` in the chapter folder.
- `lang/lang_en.json` in the chapter folder.
- Chapter music and the shared `mus` folder's `.ogg` files.

It packs the chapter's music, language and extra sound data into the
generated files. You don't need to rename or copy those extra files yourself.

### What the plugin does with your files

**Keep originals** leaves the `undertale/` or `deltarune/` folder on the card
for another install. **Discard originals** removes that folder only after a
successful install. Neither touches the other game, your installed files,
your computer's game install or your saves. Leave enough free space for the
originals, the conversion and the previous game files during a replacement.

If conversion or installation fails, the originals are kept and the previous
game files are restored. If restoring fails, keep the recovery backups at the
reported location. Work and recovery files are hidden under
`Assets/butterscotch/common/.butterscotch-install-*`, and the plugin's report
is `Assets/butterscotch/common/.butterscotch-<game>.json`.

The plugin installs from the latest stable release's `plugin.wasm` and
`plugin.json`. The direct-install workflow needs v0.7.1 or newer. Update the
plugin if you installed v0.7.0.

### Installing game files with Pocket Sync but without the plugin

You can run `prepare-data` (steps B2–B3), then compress the output's
**Assets** folder into a ZIP and drag that ZIP onto Pocket Sync. Opening the
ZIP must show `Assets` straight away, with `butterscotch/common` inside it;
don't compress the `prepared-undertale` folder itself. This ZIP holds your
own game data, so keep it private.

### Keep these separate

- The **core ZIP** is the only thing to import into Pocket Sync as a core.
- The `prepare-data-*.tar.gz` tools and source archives run on your computer.
- Neither the core nor the tools include, download or change your games or
  your saves. Keep your original and converted game files private.

## Building from source

From `src/openfpga/tools/pocket-data`, with Go 1.25 or newer:

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
decoder's own regressions. Run `REQUIRE_REFERENCE_TESTS=1 go test ./integration` for independent C converter comparisons against this checkout. Python and a host C compiler are required. `BUTTERSCOTCH_SOURCE` can explicitly select another runner checkout.
The old `prepare_game.py`, `mktexpack.c` and `mkmusic.c` remain developer references in [this checkout](tools/);
they are not dependencies of the Go executable or plugin.

The Vorbis decoder is pure Go. The generated synthetic Vorbis fixture matches
the old decoder in current checks; this does not prove byte-identical decoding
for every commercial track. Full-game compatibility and resource use still need
on-device tests with the user's own supported game files.

See [Building and releasing](docs/BUILD.md) for the shared source commit and pinned SDK/runtime contract.
