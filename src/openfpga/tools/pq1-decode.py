#!/usr/bin/env python3
"""Decodes a Pocket report QR code (PQ1) back to its text.

The Pocket shows a benchmark report, or a fatal error's log, as a QR code
holding "PQ1:" + base45 (RFC 9285) of a zlib stream of the text (see
platform/of_qr.h). Pass a photo or screenshot of the code, or the
PQ1 text itself (as an argument, a file, or on stdin):

    tools/pq1-decode.py photo.jpg
    tools/pq1-decode.py 'PQ1:NCF6-L...'
    pbpaste | tools/pq1-decode.py

Reading a photo needs zxing-cpp and Pillow (pip install zxing-cpp pillow).
The zlib checksum makes a bad read fail loudly instead of printing junk.
"""

import os
import sys
import zlib

BASE45 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
PREFIX = "PQ1:"


def b45decode(text):
    values = [BASE45.index(c) for c in text]
    out = bytearray()
    for i in range(0, len(values), 3):
        chunk = values[i:i + 3]
        if len(chunk) == 3:
            n = chunk[0] + chunk[1] * 45 + chunk[2] * 2025
            if n > 0xFFFF:
                raise ValueError("not base45")
            out += bytes((n >> 8, n & 0xFF))
        elif len(chunk) == 2:
            n = chunk[0] + chunk[1] * 45
            if n > 0xFF:
                raise ValueError("not base45")
            out.append(n)
        else:
            raise ValueError("base45 text has a stray last character")
    return bytes(out)


def decode_payload(payload):
    payload = payload.strip()
    if not payload.startswith(PREFIX):
        raise ValueError("not a PQ1 code (it starts %r)" % payload[:8])
    return zlib.decompress(b45decode(payload[len(PREFIX):])).decode("utf-8", "replace")


def read_image(path):
    try:
        import zxingcpp
        from PIL import Image, ImageOps
    except ImportError:
        sys.exit("reading a photo needs: pip install zxing-cpp pillow")
    image = ImageOps.exif_transpose(Image.open(path))
    # A photo of a screen sometimes reads only once it is smaller or grey.
    candidates = [image, image.convert("L")]
    for factor in (2, 3, 4):
        candidates.append(image.convert("L").resize((image.width // factor, image.height // factor)))
    for candidate in candidates:
        for result in zxingcpp.read_barcodes(candidate):
            if result.text.startswith(PREFIX):
                return result.text
    sys.exit("%s: no PQ1 QR code found" % path)


def main():
    args = sys.argv[1:]
    if not args:
        payloads = [sys.stdin.read()]
    else:
        payloads = []
        for arg in args:
            if arg.startswith(PREFIX):
                payloads.append(arg)
            elif os.path.isfile(arg):
                with open(arg, "rb") as f:
                    head = f.read(len(PREFIX))
                if head == PREFIX.encode():
                    with open(arg) as f:
                        payloads.append(f.read())
                else:
                    payloads.append(read_image(arg))
            else:
                sys.exit("%s: not a file or PQ1 text" % arg)
    for payload in payloads:
        sys.stdout.write(decode_payload(payload))


if __name__ == "__main__":
    main()
