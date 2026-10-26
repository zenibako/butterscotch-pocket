# Pocket data tools

This nested Go module supplies the native `prepare-data` command, a WASI command,
and the Pocket Sync Extism plugin. It prepares privately owned Undertale 1.08 or
Deltarune Chapter 1 files for the Butterscotch Pocket core. Installation and input
layout instructions are in [`../../GAME-DATA.md`](../../GAME-DATA.md); source and
release build instructions are in [`../../docs/BUILD.md`](../../docs/BUILD.md).

Run these commands from this directory (`src/openfpga/tools/pocket-data`):

```sh
go build -trimpath -o prepare-data ./cmd/prepare-data
./prepare-data --game undertale --source /path/to/installed/game --output /path/to/new/output
GOOS=wasip1 GOARCH=wasm CGO_ENABLED=0 go build -trimpath -o prepare-data.wasm ./cmd/prepare-data
tinygo build -p 2 -trimpath -target wasip1 -buildmode=c-shared -o plugin.wasm ./cmd/pocket-plugin
```

Use Go 1.25 or newer. Plugin builds use TinyGo 0.43.0 and Binaryen 133
(`wasm-opt` on `PATH`). The tools operate on user-provided local files. The plugin
installs the three generated game files directly into `Assets/butterscotch/common`,
keeps originals by default, and only discards originals after a successful install
when explicitly selected. It preserves saves and rolls back failed installations.
Its manifest grants no network hosts.

Verification from this directory:

```sh
go test ./...
go vet ./...
go test -C third_party/vorbis ./...
go vet -C third_party/oggvorbis ./...
REQUIRE_REFERENCE_TESTS=1 REQUIRE_PLUGIN_TESTS=1 go test ./... -count=1
```

The integration suite builds and runs the WASI command and unit tests in a WASI
host. With TinyGo available, it builds and runs the actual Extism plugin in both
mount orders and checks byte parity with native output, installation, original
retention/discard, and save preservation. `TINYGO` can select a compiler and
`POCKET_PLUGIN_WASM` can select an already-built plugin.

The independent C oracle defaults to `../../prepare_game.py` from the integration
test directory, so a full checkout needs no second source checkout. It requires
Python 3 and a host C compiler. `BUTTERSCOTCH_SOURCE` can explicitly select another
full Butterscotch checkout. Missing optional tools skip their integration checks;
`REQUIRE_REFERENCE_TESTS=1` and `REQUIRE_PLUGIN_TESTS=1` make missing dependencies
fail. An invalid explicit source override always fails.

The original AGPL-3.0 license remains in [`LICENSE-GO-TOOLS`](LICENSE-GO-TOOLS).
Patched decoder licenses and patch descriptions, dependency provenance, and
release toolchain notices remain in [`third_party/`](third_party/SOURCES.md).
