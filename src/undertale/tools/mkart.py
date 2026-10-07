#!/usr/bin/env python3
"""
mkart.py — draws the core's menu images for the Analogue Pocket.

    mkart.py <dist/undertale directory> [preview directory]

Writes Platforms/_images/undertale.bin (521x165, the banner shown in the
openFPGA menu) and Cores/<core>/icon.bin (36x36). With a preview directory
it also writes PNGs of both as the Pocket shows them.

The artwork is drawn here from scratch (a block-letter wordmark and a
heart), so nothing from any game or from Butterscotch is redistributed.

File format, taken from the SDK's own images: two bytes per pixel, the first
holding 255 for the dark background down to 0 for full white, the second
zero; the picture is stored rotated a quarter turn anticlockwise.
"""

import sys
from pathlib import Path

from PIL import Image

LETTERS = {
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "N": ["#...#", "##..#", "##..#", "#.#.#", "#..##", "#..##", "#...#"],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "C": [".####", "#....", "#....", "#....", "#....", "#....", ".####"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
}

HEART = [
    "..##...##..",
    ".####.####.",
    "###########",
    "###########",
    "###########",
    ".#########.",
    "..#######..",
    "...#####...",
    "....###....",
    ".....#.....",
]


def stamp(image, rows, left, top, scale):
    """Draws a bitmap of '#' cells in white, each cell scale x scale pixels."""
    for y, row in enumerate(rows):
        for x, cell in enumerate(row):
            if cell != "#":
                continue
            for dy in range(scale):
                for dx in range(scale):
                    image.putpixel((left + x * scale + dx, top + y * scale + dy), 255)


def word_width(word, scale):
    return len(word) * 5 * scale + (len(word) - 1) * scale


def draw_word(image, word, top, scale):
    left = (image.width - word_width(word, scale)) // 2
    for i, letter in enumerate(word):
        stamp(image, LETTERS[letter], left + i * 6 * scale, top, scale)


def banner():
    image = Image.new("L", (521, 165), 0)
    big, small, gap = 6, 4, 18
    top = (165 - (7 * big + gap + 7 * small)) // 2
    draw_word(image, "BUTTERSCOTCH", top, big)
    draw_word(image, "POCKET", top + 7 * big + gap, small)
    return image


def icon():
    image = Image.new("L", (36, 36), 0)
    scale = 3
    stamp(image, HEART, (36 - len(HEART[0]) * scale) // 2, (36 - len(HEART) * scale) // 2, scale)
    return image


def write_bin(image, path):
    """image is upright, white (255) on black (0), as the Pocket displays it."""
    stored = image.rotate(90, expand=True)
    data = bytearray()
    for value in stored.tobytes():
        data += bytes((255 - value, 0))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    dist = Path(sys.argv[1])
    cores = [p for p in (dist / "Cores").iterdir() if p.is_dir()]
    if len(cores) != 1:
        sys.exit(f"mkart: expected one core under {dist / 'Cores'}")

    images = {"banner": (banner(), dist / "Platforms/_images/undertale.bin"), "icon": (icon(), cores[0] / "icon.bin")}
    for name, (image, path) in images.items():
        write_bin(image, path)
        print(f"{path} ({image.width}x{image.height})")
        if len(sys.argv) == 3:
            preview = Path(sys.argv[2])
            preview.mkdir(parents=True, exist_ok=True)
            image.save(preview / f"{name}.png")


if __name__ == "__main__":
    main()
