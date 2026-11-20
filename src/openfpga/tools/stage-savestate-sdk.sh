#!/usr/bin/env bash
# Copy a built, matching os20 save-state runtime into an isolated SDK.
# The runtime comes from our openfpgaOS fork, zenibako/openfpgaCore.
set -euo pipefail
tools=$(cd "$(dirname "$0")" && pwd)
# --allow-timing-miss packages a build whose setup or hold slack is negative,
# such as the fork's os20 at 100 MHz. Without it, such a build is refused.
allow_miss=0
if [ "${1:-}" = --allow-timing-miss ]; then
    allow_miss=1
    shift
fi
if [ "$#" != 3 ]; then
    echo "Usage: $0 [--allow-timing-miss] <built openfpgaOS checkout> <base SDK> <new staged SDK>" >&2
    exit 1
fi
os=$(cd "$1" && pwd)
base=$(cd "$2" && pwd)
dest="$3"
python3 "$tools/runtime-provenance.py" source "$os"
[ ! -e "$dest" ] || { echo "Use a new destination; the base SDK stays read-only" >&2; exit 1; }
build="$os/src/fpga/targets/pocket/bld/os20"
python3 - "$build" "$allow_miss" <<'PY'
import pathlib, re, sys
build = pathlib.Path(sys.argv[1])
allow_miss = sys.argv[2] == '1'
qsf = (build / 'ap_core.qsf').read_text()
if 'VERILOG_MACRO INCLUDE_SAVESTATE' not in qsf:
    raise SystemExit('os20 was built without INCLUDE_SAVESTATE')
summary = (build / 'output_files/ap_core.sta.summary').read_text()
slacks = [float(x) for x in re.findall(r'^Slack\s*:\s*([-+\d.]+)', summary, re.M)]
if not slacks:
    raise SystemExit('Runtime has no timing report; do not package it')
worst = min(slacks)
print(f'os20 worst slack: {worst:+.3f} ns')
if worst < 0:
    if not allow_miss:
        raise SystemExit('Runtime misses timing; pass --allow-timing-miss to package it anyway')
    print('Packaging a build that misses timing (--allow-timing-miss)')
PY
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
rsync -a --exclude=.git "$base/" "$dest/"
# A skipped/failed export must not recertify any inherited base SDK bytes.
rm -f "$dest/runtime/pocket/SAVESTATES_RUNTIME.json" \
    "$dest/runtime/pocket/os.bin" "$dest/runtime/pocket/os20.rbf_r" \
    "$dest/runtime/pocket/loader.bin"
make -C "$os" sdk TARGET=pocket VARIANT=os20 DEST="$dest"
python3 "$tools/runtime-provenance.py" write "$os" "$dest/runtime/pocket"
{
    echo "Butterscotch native save-state runtime"
    git -C "$os" rev-parse HEAD
    shasum -a 256 "$dest/runtime/pocket/os.bin" "$dest/runtime/pocket/os20.rbf_r"
} > "$dest/runtime/pocket/SAVESTATES_RUNTIME"
