#!/bin/bash
#
# mkcompare.sh — add benchmark instances and v0.9 comparison cores to the
# assembled Pocket tree, so one SD card can run the same benchmark on:
#
#   Undertale          this SDK's runtime (v0.7), os25 bitstream
#   Undertale09os25    v0.9 runtime, os25 bitstream
#   Undertale09os20    v0.9 runtime, os20 bitstream (dual-issue CPU)
#
# All three share Assets/undertale/common (data.win, textures.bin,
# music.bin); the v0.9 cores use their own os09.bin and undertale09.elf.
#
# Usage: mkcompare.sh <build tree> <v0.9 runtime/pocket dir> <v0.9 app.elf>
#
set -e

OUT="$1"; RT09="$2"; ELF09="$3"
[ -d "$OUT/Cores/chanderson.Undertale" ] || { echo "mkcompare: $OUT is not an assembled tree"; exit 1; }
[ -f "$RT09/os.bin" ] || { echo "mkcompare: no os.bin in $RT09"; exit 1; }
[ -f "$ELF09" ] || { echo "mkcompare: $ELF09 not found"; exit 1; }

COMMON="$OUT/Assets/undertale/common"
BASE_CORE="$OUT/Cores/chanderson.Undertale"
BASE_INSTANCE="$OUT/Assets/undertale/chanderson.Undertale/undertale.json"

write_ini() { # <file> <elf> <args> <variant>
    printf '[os]\nELF=%s\nARGS=%s\nVARIANT=%s\n' "$2" "$3" "$4" > "$COMMON/$1"
}

# Instance JSON derived from the base one with different OS, config and ELF files.
write_instance() { # <dest> <os.bin name> <ini name> <elf name>
    sed -e "s/\"os\.bin\"/\"$2\"/" \
        -e "s/\"undertale_os\.ini\"/\"$3\"/" \
        -e "s/\"undertale\.elf\"/\"$4\"/" \
        "$BASE_INSTANCE" > "$1"
}

# Benchmark instance for the base (v0.7) core.
write_ini undertale_bench.ini undertale.elf --bench os25
write_instance "$OUT/Assets/undertale/chanderson.Undertale/Benchmark.json" os.bin undertale_bench.ini undertale.elf
write_ini undertale_b320.ini undertale.elf --bench-lowres os25
write_instance "$OUT/Assets/undertale/chanderson.Undertale/Benchmark 320.json" os.bin undertale_b320.ini undertale.elf

cp "$RT09/os.bin" "$COMMON/os09.bin"
cp "$ELF09" "$COMMON/undertale09.elf"

for variant in os25 os20; do
    name="Undertale09$variant"
    core="$OUT/Cores/chanderson.$name"
    rm -rf "$core"
    mkdir -p "$core" "$OUT/Assets/undertale/chanderson.$name"
    cp "$BASE_CORE"/*.json "$core/"
    cp "$RT09/$variant.rbf_r" "$RT09/loader.bin" "$core/"
    sed -i -e "s/\"shortname\": \"Undertale\"/\"shortname\": \"$name\"/" \
           -e "s/\"description\": \"[^\"]*\"/\"description\": \"Undertale, openfpgaOS v0.9 $variant\"/" \
           -e "s/\"filename\": \"[a-z0-9]*\.rbf_r\"/\"filename\": \"$variant.rbf_r\"/" \
           "$core/core.json"

    write_ini "ut09_$variant.ini" undertale09.elf "" "$variant"
    write_ini "ut09_${variant}_bench.ini" undertale09.elf --bench "$variant"
    write_instance "$OUT/Assets/undertale/chanderson.$name/Undertale.json" os09.bin "ut09_$variant.ini" undertale09.elf
    write_instance "$OUT/Assets/undertale/chanderson.$name/Benchmark.json" os09.bin "ut09_${variant}_bench.ini" undertale09.elf
    # The same benchmark held at 320x240 throughout, in case a bitstream's
    # 640x480 mode is the thing that fails.
    write_ini "ut09_${variant}_b320.ini" undertale09.elf --bench-lowres "$variant"
    write_instance "$OUT/Assets/undertale/chanderson.$name/Benchmark 320.json" os09.bin "ut09_${variant}_b320.ini" undertale09.elf
done

echo "Comparison cores added: Undertale (v0.7), Undertale09os25, Undertale09os20"
