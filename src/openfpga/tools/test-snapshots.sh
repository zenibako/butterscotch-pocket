#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$port/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/ut-snapshot-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
flags=(-std=c11 -DUSE_FLOAT_REALS -DNO_RVALUE_INT64 -DENABLE_WAD16
    '-DOF_SAVE_SLOT_FILE="undertale_0.sav"'
    -ffunction-sections -fsanitize=address,undefined -fno-sanitize=alignment,function
    -I"$repo/src" -I"$repo/src/video" -I"$port/platform" -I"$repo/vendor/stb/ds")
if [ "$(uname -s)" = Darwin ]; then
    flags+=(-Wl,-dead_strip)
    export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/lib}"
else flags+=(-D_GNU_SOURCE -Wl,--gc-sections); fi
export ASAN_OPTIONS=detect_leaks=0
"$cc" "${flags[@]}" -fno-sanitize-recover=undefined \
    "$repo/tests/opword_test.c" -o "$out/opword"
"$out/opword"
"$cc" "${flags[@]}" -I"$repo/vendor/miniz" -I"$repo/vendor/zstd" \
    "$repo/tests/runner_snapshot_test.c" "$repo/src/runner_snapshot.c" "$repo/src/runner.c" \
    "$repo/src/instance.c" "$repo/src/gml_array.c" "$repo/src/gml_method.c" "$repo/src/int_rvalue_hashmap.c" \
    "$repo/src/spatial_grid.c" "$repo/src/random.c" "$repo/src/ini.c" \
    "$repo/src/stb_ds.c" "$repo/src/gettime.c" "$repo/src/data_win.c" \
    "$repo/src/binary_reader.c" "$repo/src/log.c" \
    "$repo/src/physics/disabled/physics_disabled.c" \
    "$repo/vendor/miniz/miniz.c" "$repo/vendor/miniz/miniz_tdef.c" \
    "$repo/vendor/miniz/miniz_tinfl.c" "$repo/vendor/zstd/zstd.c" -lm -o "$out/runner"
"$out/runner"
echo "runner snapshot: passed"
"$cc" "${flags[@]}" -DENABLE_WAD17 -I"$repo/vendor/miniz" -I"$repo/vendor/zstd" \
    "$repo/tests/runner_snapshot_test.c" "$repo/src/runner_snapshot.c" "$repo/src/runner.c" \
    "$repo/src/instance.c" "$repo/src/gml_array.c" "$repo/src/gml_method.c" "$repo/src/int_rvalue_hashmap.c" \
    "$repo/src/spatial_grid.c" "$repo/src/random.c" "$repo/src/ini.c" \
    "$repo/src/stb_ds.c" "$repo/src/gettime.c" "$repo/src/data_win.c" \
    "$repo/src/binary_reader.c" "$repo/src/log.c" \
    "$repo/src/physics/disabled/physics_disabled.c" \
    "$repo/vendor/miniz/miniz.c" "$repo/vendor/miniz/miniz_tdef.c" \
    "$repo/vendor/miniz/miniz_tinfl.c" "$repo/vendor/zstd/zstd.c" -lm -o "$out/runner17"
"$out/runner17"
echo "runner snapshot (WAD 17): passed"
"$cc" "${flags[@]}" -DPIXEL_SIZE=16 -I"$repo/src/sw" \
    "$repo/tests/sw_button_cache_test.c" "$repo/src/log.c" -lm -o "$out/button-cache"
"$out/button-cache"
"$cc" "${flags[@]}" "$repo/tests/data_win_room_cache_test.c" \
    "$repo/src/data_win.c" "$repo/src/binary_reader.c" \
    "$repo/src/log.c" "$repo/src/stb_ds.c" -lm -o "$out/room-cache"
"$out/room-cache" "$repo/tests/try-catch-finally-test/data.win"
"$cc" "${flags[@]}" "$repo/tests/of_snapshot_envelope_test.c" \
    "$port/platform/of_snapshot_envelope.c" -o "$out/envelope"
"$out/envelope"
"$cc" "${flags[@]}" "$repo/tests/of_save_snapshot_test.c" -o "$out/files"
"$out/files"
"$cc" "${flags[@]}" -I"${SDK_ROOT:?Set SDK_ROOT}/src/sdk/include" \
    "$repo/tests/of_audio_snapshot_test.c" "$repo/src/stb_ds.c" -o "$out/audio"
"$out/audio"
"$cc" "${flags[@]}" -I"$SDK_ROOT/src/sdk/include" \
    "$repo/tests/of_audio_silence_test.c" "$repo/src/stb_ds.c" -o "$out/audio-silence"
"$out/audio-silence"
"$cc" "${flags[@]}" -I"$SDK_ROOT/src/sdk/include" -isystem "$SDK_ROOT/src/sdk/pc" \
    -DOF_PC_SDL_SOURCE='"of_sdl2.c"' $(pkg-config --cflags sdl2) \
    "$repo/tests/of_pc_audio_snapshot_test.c" $(pkg-config --libs sdl2) -lm -o "$out/pc-audio"
"$out/pc-audio"
export SDK_ROOT
for test in test_runtime_packaging test_game_fingerprint test_state_identity test_focus_build_cache test_snapshot_notice test_save_fs_native_close test_save_fs_sync \
    test_snapshot_completion test_restore_commit test_state_coverage test_state_probe; do
    python3 "$port/tests/$test.py"
done
