#!/bin/bash
#
# mkcard.sh — assemble the Pocket SD tree: one Butterscotch core with an
# entry per game. A game is included when its program has been built and
# its data file is there; the entries of the others are left out, so the
# Pocket never lists a game that cannot start.
#
# Each game's program reads "data.win", "textures.bin" and "music.bin" from
# games/<game>/: on the card Deltarune's files carry their own names, and its
# entry (dist/.../Deltarune.json) puts them in the same data slots.
#
# Usage: mkcard.sh <SDK image.sh> <root holding runtime/ and build/> <dist dir> <port dir>
#
set -e
IMAGE_SH="$1"; ROOT="$2"; DIST="$3"; PORT="$4"
NAME=butterscotch
OUT="$ROOT/build/pocket/$NAME"

# <game> <instance file> <folder with its data> <data> <textures> <music>: names on the card
GAMES=(
    "undertale|Undertale.json|games/undertale|data.win|textures.bin|music.bin"
    "deltarune|Deltarune.json|games/deltarune|deltarune.win|dr_textures.bin|dr_music.bin"
)
if [ "${SAVESTATES:-0}" = 1 ]; then
    if [ "${VARIANT:-os20}" != os20 ]; then
        echo "mkcard: save-state runtime requires VARIANT=os20" >&2
        exit 1
    fi
    export VARIANT=os20
    python3 "$(dirname "$0")/runtime-provenance.py" check "$ROOT/runtime/pocket" || exit 1
fi

first=""
for entry in "${GAMES[@]}"; do
    IFS='|' read -r game _ dir _ _ _ <<< "$entry"
    [ -f "$ROOT/.obj/$game/app.elf" ] && [ -f "$PORT/$dir/data.win" ] && { first="$ROOT/.obj/$game/app.elf"; break; }
done
[ -n "$first" ] || { echo "mkcard: no game is built with its data.win in place"; exit 1; }

# The SDK script lays down the core, the bitstream and the OS; it names the
# program after the tree, which is not a name any entry uses.
bash "$IMAGE_SH" "$NAME" "$first" "$ROOT" "$DIST" > /dev/null
COMMON="$OUT/Assets/$NAME/common"
rm -f "$COMMON/$NAME.elf"
if [ "${SAVESTATES:-0}" != 1 ]; then
    rm -f "$OUT/Assets/$NAME"/*/"Deltarune Sleep Test.json" "$COMMON/deltarune_sleep.ini"
fi
if [ "${SAVESTATES:-0}" = 1 ]; then
    python3 - "$OUT" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for path in root.glob('Cores/*/core.json'):
    data = json.loads(path.read_text())
    data['core']['framework']['sleep_supported'] = True
    data['core']['metadata']['description'] = 'Butterscotch with save states and sleep (experimental)'
    path.write_text(json.dumps(data, indent=4) + '\n')
PY
fi

# Four bytes, little-endian, as printf escapes.
le32() {
    printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255))
}

# The texture pack also rides at the end of the music pack, where the
# program reads it (sw_texture_pack.h): switching between two files costs the
# Pocket about 20 ms each way, and music is read between nearly every frame.
# It starts on a 4 KB boundary and is followed by its offset, its size, a
# zero and "UTXT". The pack stays in its own slot for the v0.9 comparison
# cores, which read it there.
append_textures() { # <music pack on the card> <texture pack>
    local size pad base tsize
    size=$(wc -c < "$1"); pad=$(( (4096 - size % 4096) % 4096 ))
    head -c "$pad" /dev/zero >> "$1"
    base=$((size + pad)); tsize=$(wc -c < "$2")
    cat "$2" >> "$1"
    printf "$(le32 "$base")$(le32 "$tsize")$(le32 0)UTXT" >> "$1"
}

included=""
for entry in "${GAMES[@]}"; do
    IFS='|' read -r game instance dir data textures music <<< "$entry"
    elf="$ROOT/.obj/$game/app.elf"
    src="$PORT/$dir"
    # The program cannot draw without its texture pack. The music pack is
    # only built when the .ogg files are there, so a game may go without it.
    if [ -f "$elf" ] && [ -f "$src/data.win" ] && [ ! -f "$src/textures.bin" ]; then
        echo "mkcard: $game left out, $src/textures.bin is missing"
    fi
    if [ -f "$elf" ] && [ -f "$src/data.win" ] && [ -f "$src/textures.bin" ]; then
        cp "$elf" "$COMMON/$game.elf"
        cp "$src/data.win" "$COMMON/$data"
        cp "$src/textures.bin" "$COMMON/$textures"
        if [ -f "$src/music.bin" ]; then
            cp "$src/music.bin" "$COMMON/$music"
            append_textures "$COMMON/$music" "$src/textures.bin"
        else
            echo "mkcard: $game has no $src/music.bin, it will have no sound pack"
        fi
        included="$included $game"
    else
        rm -f "$OUT/Assets/$NAME"/*/"$instance" "$COMMON/${game}_os.ini"
        if [ "$game" = deltarune ]; then
            rm -f "$OUT/Assets/$NAME"/*/"Deltarune Sleep Test.json" "$COMMON/deltarune_sleep.ini"
        fi
    fi
done
echo "Ready: build/pocket/$NAME/ with$included"
