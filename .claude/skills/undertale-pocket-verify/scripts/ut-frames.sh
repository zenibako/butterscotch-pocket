#!/bin/bash
#
# ut-frames.sh — dump frames from the desktop build for repeatable comparison.
#
#   ut-frames.sh [-s script-file|-S "inline script"] [-e VAR=VALUE]... [-p prefix] [-k] frame [frame...]
#
# For each frame number, runs undertale_pc from a clean save state with a
# fixed seed and no frame pacing, and writes <prefix><frame>.ppm into the run
# directory. The game log of the last run is left in <prefix>last.log.
#
#   -s FILE   input script file (see assets/*.script)
#   -S TEXT   input script given inline, e.g. "30:Z,100:Z,700:R*400"
#   -e V=X    extra environment for the run, e.g. -e UT_SMOOTH=1
#   -p NAME   output prefix (default "f")
#   -k        keep the save file between frames (default: start fresh each time)
#
# Run directory: $UT_RUN_DIR, default /tmp/undertale-pocket-run. It is created
# on first use with links to data.win, textures.bin and music.bin.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/../../../../src/undertale" && pwd)"
BIN="$PORT/undertale_pc"
RUN="${UT_RUN_DIR:-/tmp/undertale-pocket-run}"

script=""; prefix="f"; keep=0; extra=()
while getopts "s:S:e:p:k" opt; do
    case "$opt" in
        s) script="$(cat "$OPTARG")" ;;
        S) script="$OPTARG" ;;
        e) extra+=("$OPTARG") ;;
        p) prefix="$OPTARG" ;;
        k) keep=1 ;;
        *) sed -n '3,20p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || { sed -n '3,20p' "$0"; exit 2; }
[ -x "$BIN" ] || { echo "ut-frames: $BIN not built; run 'make test' in src/undertale"; exit 1; }
[ -f "$PORT/data.win" ] || { echo "ut-frames: $PORT/data.win missing (the user's own game data)"; exit 1; }

mkdir -p "$RUN"
for f in data.win textures.bin music.bin; do
    [ -e "$PORT/$f" ] && ln -sf "$PORT/$f" "$RUN/$f"
done

cd "$RUN"
for frame in "$@"; do
    # The game writes saves as it plays; a leftover save changes later runs
    # (Flowey greets a returning player differently), so start clean.
    [ $keep -eq 1 ] || rm -f undertale_0.sav file0 file9 undertale.ini
    env UT_SEED=7 UT_UNCAPPED=1 UT_SCRIPT="$script" UT_DUMP_FRAME="$frame" UT_DUMP_PATH="$RUN/$prefix$frame.ppm" \
        ${extra[@]+"${extra[@]}"} "$BIN" > "$RUN/${prefix}last.log" 2>&1 || {
            echo "ut-frames: run for frame $frame failed; see $RUN/${prefix}last.log"; exit 1; }
    echo "$RUN/$prefix$frame.ppm $(sed -n 2p "$RUN/$prefix$frame.ppm")"
done
