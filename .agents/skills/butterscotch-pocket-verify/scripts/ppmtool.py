#!/usr/bin/env python3
"""ppmtool.py — compare and convert the PPM frames written by the desktop build.

  ppmtool.py cmp A.ppm B.ppm          exit 0 if identical; else print how many pixels
                                      differ, the bounding box and the largest channel
                                      difference, and exit 1
  ppmtool.py png IN.ppm OUT.png [N]   convert to PNG, optionally enlarged N times
  ppmtool.py crop IN.ppm OUT.png X Y W H [N]
                                      convert a region to PNG (for reading small text)

No dependencies beyond the Python standard library.
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    magic, size, maxval, pixels = data.split(b"\n", 3)
    if magic != b"P6" or maxval != b"255":
        sys.exit(f"{path}: not a binary 8-bit PPM")
    width, height = map(int, size.split())
    return width, height, pixels


def write_png(path, width, height, pixels):
    rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def region(width, pixels, x, y, w, h, scale):
    out = bytearray()
    for row in range(y, y + h):
        line = b"".join(pixels[(row * width + col) * 3:(row * width + col) * 3 + 3] * scale for col in range(x, x + w))
        out += line * scale
    return w * scale, h * scale, bytes(out)


def main(argv):
    if len(argv) >= 4 and argv[1] == "cmp":
        wa, ha, a = read_ppm(argv[2])
        wb, hb, b = read_ppm(argv[3])
        if (wa, ha) != (wb, hb):
            print(f"sizes differ: {wa}x{ha} vs {wb}x{hb}")
            return 1
        if a == b:
            print("identical")
            return 0
        xs, ys, worst, count = [], [], 0, 0
        for i in range(0, len(a), 3):
            if a[i:i + 3] != b[i:i + 3]:
                count += 1
                xs.append((i // 3) % wa)
                ys.append((i // 3) // wa)
                worst = max(worst, abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        print(f"{count} pixels differ, bounding box x {min(xs)}-{max(xs)} y {min(ys)}-{max(ys)}, "
              f"largest channel difference {worst} of 255")
        return 1
    if len(argv) >= 4 and argv[1] == "png":
        width, height, pixels = read_ppm(argv[2])
        scale = int(argv[4]) if len(argv) > 4 else 1
        write_png(argv[3], *region(width, pixels, 0, 0, width, height, scale))
        return 0
    if len(argv) >= 8 and argv[1] == "crop":
        width, height, pixels = read_ppm(argv[2])
        x, y, w, h = map(int, argv[4:8])
        scale = int(argv[8]) if len(argv) > 8 else 1
        write_png(argv[3], *region(width, pixels, x, y, min(w, width - x), min(h, height - y), scale))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
