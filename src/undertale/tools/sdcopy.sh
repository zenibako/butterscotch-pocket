#!/bin/bash
#
# sdcopy.sh — copy an assembled Pocket tree to the SD card on macOS, including
# over SSH with nobody at the screen.
#
# The SDK's copy.sh assumes the card is already mounted. On a headless Mac it
# often is not: the card reader stops noticing insertions while the machine
# counts as idle, and macOS only auto-mounts removable disks into an active
# desktop session. This script declares user activity, waits for the card,
# mounts it if needed, removes renamed-away cores of the same platform,
# copies, removes macOS sidecar files, verifies and ejects, and says what went wrong when it cannot.
#
# Usage: sdcopy.sh <build tree> [seconds to wait for the card, default 90]
#
set -u

TREE="$1"
WAIT="${2:-90}"
[ -d "$TREE/Cores" ] && [ -d "$TREE/Assets" ] || { echo "sdcopy: $TREE is not an assembled Pocket tree"; exit 1; }

# A mounted volume that looks like a Pocket card.
find_mounted() {
    for v in /Volumes/*; do
        [ -d "$v/Cores" ] && [ -d "$v/Assets" ] && { echo "$v"; return 0; }
    done
    return 1
}

# An unmounted FAT/exFAT partition on removable media (never a fixed disk's
# EFI partition, which is also FAT).
find_unmounted() {
    for disk in $(diskutil list external physical 2>/dev/null | awk '/^\/dev\//{print $1}'); do
        for part in $(diskutil list "$disk" | awk '$NF ~ /^disk[0-9]+s[0-9]+$/{print $NF}'); do
            info=$(diskutil info "/dev/$part" 2>/dev/null)
            echo "$info" | grep -q 'Removable Media: *Removable' || continue
            echo "$info" | grep -Eq 'File System Personality: *(MS-DOS|ExFAT)' || continue
            echo "$info" | grep -q 'Mounted: *No' || continue
            echo "/dev/$part"
            return 0
        done
    done
    return 1
}

# Count as an active user for the duration, which wakes the display and gets
# the card reader polling for media again.
caffeinate -u -t $((WAIT + 600)) &
CAFFEINATE=$!
trap 'kill $CAFFEINATE 2>/dev/null' EXIT

CARD=""
announced=0
for ((waited = 0; waited <= WAIT; waited += 2)); do
    if CARD=$(find_mounted); then break; fi
    if PART=$(find_unmounted); then
        echo "sdcopy: mounting $PART"
        diskutil mount "$PART" >/dev/null || { echo "sdcopy: could not mount $PART"; exit 1; }
        continue
    fi
    if [ $announced -eq 0 ]; then
        echo "sdcopy: no card found yet. Insert it now (or pull it and push it back in); waiting up to ${WAIT}s..."
        announced=1
    fi
    sleep 2
done
[ -n "$CARD" ] || { echo "sdcopy: no Pocket SD card appeared. The reader did not report a card at all."; exit 1; }

# Cores of the platforms being copied that the tree no longer contains are
# leftovers from a rename; the Pocket would list them next to the new ones.
for platform in "$TREE"/Assets/*/; do
    platform=$(basename "$platform")
    for old in "$CARD/Assets/$platform"/*/; do
        name=$(basename "$old")
        [ -d "$old" ] && [ "$name" != common ] && [ ! -d "$TREE/Assets/$platform/$name" ] || continue
        echo "sdcopy: removing old core $name"
        rm -rf "$CARD/Assets/$platform/$name" "$CARD/Cores/$name"
    done
done

echo "sdcopy: copying to $CARD"
for dir in Cores Assets Platforms; do
    [ -d "$TREE/$dir" ] || continue
    cp -R "$TREE/$dir/" "$CARD/$dir/" || { echo "sdcopy: copy of $dir failed"; exit 1; }
done

# macOS adds ._ sidecar files on FAT volumes; the Pocket can list them as junk entries.
for dir in "$CARD"/Cores/* "$CARD"/Assets/* "$CARD/Platforms"; do
    [ -d "$dir" ] && dot_clean -m "$dir" 2>/dev/null
done
sync

# Spot-check every app binary and OS image that was copied.
failed=0
while IFS= read -r file; do
    rel="${file#"$TREE"/}"
    cmp -s "$file" "$CARD/$rel" || { echo "sdcopy: MISMATCH $rel"; failed=1; }
done < <(find "$TREE" -type f \( -name '*.elf' -o -name '*.bin' -o -name '*.rbf_r' -o -name '*.json' -o -name '*.ini' \) ! -name 'music.bin' ! -name 'textures.bin')
[ $failed -eq 0 ] || { echo "sdcopy: verification failed; the card was left mounted"; exit 1; }

# With nobody at the screen, loginwindow refuses a normal eject. Everything is
# written and verified by now, so a forced unmount after a sync is safe.
if diskutil eject "$CARD" >/dev/null 2>&1; then
    echo "sdcopy: done, verified and ejected. Safe to remove the card."
else
    sync
    if diskutil unmount force "$CARD" >/dev/null 2>&1; then
        echo "sdcopy: done, verified and unmounted. Safe to remove the card."
    else
        echo "sdcopy: copied and verified, but could not unmount $CARD; eject it before removing."
    fi
fi
