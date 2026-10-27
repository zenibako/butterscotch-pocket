#!/bin/bash
#
# bs-frames.sh — dump frames from the desktop build for repeatable comparison.
#
#   bs-frames.sh [-g game] [-s script-file|-S "inline script"] [-e VAR=VALUE]... [-p prefix] [-k] frame [frame...]
#
# For each frame number, runs <game>_pc from a clean save state with a
# fixed seed and no frame pacing, and writes <prefix><frame>.ppm into the run
# directory. The game log of the last run is left in <prefix>last.log.
#
#   -g GAME   undertale (default) or deltarune; its data is in src/openfpga/games/GAME/
#   -s FILE   input script file (see assets/*.script; these are Undertale's)
#   -S TEXT   input script given inline, e.g. "30:Z,100:Z,700:R*400"
#   -e V=X    extra environment for the run, e.g. -e BS_SMOOTH=1
#   -p NAME   output prefix (default "f")
#   -k        keep the save file between frames (default: start fresh each time)
#
# Run directory: $BS_RUN_DIR, default /tmp/butterscotch-pocket-run-<game>. It is
# created on first use with links to data.win, textures.bin and music.bin.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/../../../../src/openfpga" && pwd)"
game="undertale"; script=""; prefix="f"; keep=0; extra=()
while getopts "g:s:S:e:p:k" opt; do
    case "$opt" in
        g) game="$OPTARG" ;;
        s) script="$(cat "$OPTARG")" ;;
        S) script="$OPTARG" ;;
        e) extra+=("$OPTARG") ;;
        p) prefix="$OPTARG" ;;
        k) keep=1 ;;
        *) sed -n '3,20p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
BIN="$PORT/${game}_pc"
DATA="$PORT/games/$game"
# UT_RUN_DIR is the name it had until 2026-10-13. rename-ut: keep
RUN="${BS_RUN_DIR:-${UT_RUN_DIR:-/tmp/butterscotch-pocket-run-$game}}"
[ $# -ge 1 ] || { sed -n '3,20p' "$0"; exit 2; }
[ -x "$BIN" ] || { echo "bs-frames: $BIN not built; run 'make GAME=$game ${game}_pc' in src/openfpga"; exit 1; }
[ -f "$DATA/data.win" ] || { echo "bs-frames: $DATA/data.win missing (the user's own game data)"; exit 1; }

mkdir -p "$RUN"
for f in data.win textures.bin music.bin; do
    [ -e "$DATA/$f" ] && ln -sf "$DATA/$f" "$RUN/$f"
done

cd "$RUN"
for frame in "$@"; do
    # The game writes saves as it plays; a leftover save changes later runs
    # (Flowey greets a returning player differently), so start clean.
    [ $keep -eq 1 ] || rm -f "${game}_0.sav" file0 file9 undertale.ini
    env BS_SEED=7 BS_UNCAPPED=1 BS_SCRIPT="$script" BS_DUMP_FRAME="$frame" BS_DUMP_PATH="$RUN/$prefix$frame.ppm" \
        ${extra[@]+"${extra[@]}"} "$BIN" > "$RUN/${prefix}last.log" 2>&1 || {
            echo "bs-frames: run for frame $frame failed; see $RUN/${prefix}last.log"; exit 1; }
    echo "$RUN/$prefix$frame.ppm $(sed -n 2p "$RUN/$prefix$frame.ppm")"
done
