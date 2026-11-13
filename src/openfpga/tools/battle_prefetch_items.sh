#!/bin/sh
# Records which texture items each Undertale battle draws, for
# tools/battle_prefetch_table.py.
#
#   tools/battle_prefetch_items.sh ./undertale_pc games/undertale out/items
#
# Runs the battle tour (--bench-battles --bench-battles-list) over every
# global.battlegroup, seven to a run, on the desktop build: uncapped, seed 7.
# Each run leaves runN.list (its groups), runN.items (BS_ITEM_LOG) and
# runN.frames (BS_FRAME_LOG) in the output directory. JOBS (default 4) runs
# go at once. A run that fails or does not finish its benchmark makes the
# script fail, so a table is never made from partial logs.
set -eu
[ $# -eq 3 ] || { echo "usage: $0 <undertale_pc> <game dir with data.win> <output dir>" >&2; exit 2; }
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
data=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
jobs=${JOBS:-4}
groups="1,2,3,4,5,6,7 8,9,10,11,12,13,14 15,16,17,18,19,20,21 22,23,24,25,26,27,28
30,31,32,33,34,35,36 40,41,42,43,44,45,46 47,48,49,50,51,52,53 54,55,56,57,58,59,60
61,62,63,64,65,66,67 68,69,70,71,72,73,74 75,76,77,78,79,80,81 82,83,84,85,86,87,88
89,90,91,92,93,94,95 100,101,120,121,122,123,124 125,126,127,128,129,130,131
132,133,134,135,140,255,256"
status=$(mktemp -d "${TMPDIR:-/tmp}/bsitems-status.XXXXXX")
n=0
for list in $groups; do
    n=$((n + 1))
    (
        run=$(mktemp -d "${TMPDIR:-/tmp}/bsitems.XXXXXX")
        for f in data.win textures.bin music.bin; do ln -s "$data/$f" "$run/$f"; done
        echo "$list" > "$out/run$n.list"
        ok=1
        (cd "$run" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy BS_NOFLIP=1 BS_UNCAPPED=1 BS_SEED=7 \
            BS_ITEM_LOG="$out/run$n.items" BS_FRAME_LOG="$out/run$n.frames" BS_PREFETCH=0 \
            "$bin" --bench-battles --bench-battles-list "$list" > "$out/run$n.out" 2>&1) || ok=0
        grep -q "benchmark finished" "$out/run$n.out" || ok=0
        rm -rf "$run"
        if [ $ok -eq 0 ]; then
            echo "run$n ($list) failed or did not finish; see $out/run$n.out" >&2
            : > "$status/run$n.failed"
        fi
    ) &
    [ $((n % jobs)) -ne 0 ] || wait
done
wait
failed=$(ls "$status" | wc -l | tr -d ' ')
rm -rf "$status"
[ "$failed" -eq 0 ] || { echo "$failed run(s) failed" >&2; exit 1; }
