#!/usr/bin/env python3
"""Append a desktop-captured Flowey backdrop to a private Undertale pack."""
import argparse
import itertools
from pathlib import Path
import struct

WIDTH, HEIGHT = 320, 184
HEADER = struct.Struct('<4sIII')
PAGE = struct.Struct('<HHI')
TILE = struct.Struct('<II')
TAIL = struct.Struct('<III4s')
STALK = struct.Struct('<HHII')
STALK_POSES = 11


def encode_tile(pixels: list[int]) -> bytes:
    if not any(pixels):
        return b''
    words = []
    literals = []

    def flush_literals():
        if literals:
            words.append(len(literals) - 1)
            words.extend(literals)
            literals.clear()

    for color, group in itertools.groupby(pixels):
        count = sum(1 for _ in group)
        if count >= 3:
            flush_literals()
            words.extend((0x8000 | (count - 1), color))
        else:
            literals.extend([color] * count)
    flush_literals()
    return struct.pack(f'<{len(words)}H', *words)


def append_backdrop(stock: bytes, raw: bytes) -> bytes:
    magic, pages, side, tiles = HEADER.unpack_from(stock)
    if magic != b'UTX2' or not 0 < pages < 4096 or side not in (16, 32, 64, 128) or not tiles:
        raise ValueError('expected a stock UTX2 pack')
    tile_start = HEADER.size + PAGE.size * pages
    payload_start = tile_start + TILE.size * tiles
    if payload_start > len(stock) or PAGE.unpack_from(stock, tile_start - PAGE.size)[:2] == (WIDTH, HEIGHT):
        raise ValueError('pack index is truncated or already contains the backdrop')
    if len(raw) != WIDTH * HEIGHT * 2 or not any(raw):
        raise ValueError('expected a nonempty 320x184 RGB555 backdrop')
    pixels = struct.unpack(f'<{WIDTH * HEIGHT}H', raw)
    art_tiles = []
    for top in range(0, HEIGHT, side):
        for left in range(0, WIDTH, side):
            tile = []
            for y in range(top, min(top + side, HEIGHT)):
                tile.extend(pixels[y * WIDTH + left:y * WIDTH + min(left + side, WIDTH)])
            art_tiles.append(encode_tile(tile))

    extra_index = PAGE.size + TILE.size * len(art_tiles)
    index = bytearray()
    for offset, size in TILE.iter_unpack(stock[tile_start:payload_start]):
        if size and (offset < payload_start or offset + size > len(stock)):
            raise ValueError('tile outside stock payload')
        index.extend(TILE.pack(offset + extra_index if size else offset, size))
    offset = len(stock) + extra_index
    for tile in art_tiles:
        index.extend(TILE.pack(offset if tile else 0, len(tile)))
        offset += len(tile)
    return b''.join((
        HEADER.pack(magic, pages + 1, side, tiles + len(art_tiles)),
        stock[HEADER.size:tile_start], PAGE.pack(WIDTH, HEIGHT, tiles), index,
        stock[payload_start:], *art_tiles,
    ))


def append_stalks(stock: bytes, captures: Path) -> bytes:
    if stock.endswith(b'UST1'):
        raise ValueError('pack already contains stalk poses')
    payload = bytearray()
    index = bytearray()
    for pose in range(STALK_POSES):
        capture = (captures / f'{pose}.rgb555').read_bytes()
        if len(capture) < 4:
            raise ValueError(f'truncated stalk pose {pose}')
        w, h = struct.unpack_from('<HH', capture)
        if w != 180 or not 75 <= h <= 95 or len(capture) != 4 + w * h * 2:
            raise ValueError(f'invalid dimensions or payload for stalk pose {pose}')
        pixels = struct.unpack_from(f'<{w * h}H', capture, 4)
        encoded = encode_tile(pixels)
        if not encoded or len(encoded) > 2 * (128 * 128 + 16):
            raise ValueError(f'invalid encoded size for stalk pose {pose}')
        index.extend(STALK.pack(w, h, len(stock) + len(payload), len(encoded)))
        payload.extend(encoded)
    return b''.join((stock, payload, index, b'UST1'))


def embed(music: bytes, stock: bytes, extended: bytes) -> bytes:
    if not music.startswith(b'UTM1'):
        raise ValueError('expected a UTM1 audio pack')
    if music.endswith(b'UTXT'):
        offset, size, reserved, _ = TAIL.unpack_from(music, len(music) - TAIL.size)
        if reserved or offset + size != len(music) - TAIL.size or music[offset:offset + size] != stock:
            raise ValueError('music.bin does not contain the supplied stock pack')
        music = music[:offset]
    else:
        music += bytes(-len(music) % 4096)
    return music + extended + TAIL.pack(len(music), len(extended), 0, b'UTXT')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path, help='original textures.bin')
    parser.add_argument('backdrop', type=Path, help='RGB555 capture from BS_FLOWEY_BACKDROP_OUT')
    parser.add_argument('output', type=Path)
    parser.add_argument('--music', type=Path, help='embed the extended pack in a copy of music.bin')
    parser.add_argument('--stalks', type=Path, help='directory of BS_FLOWEY_STALKS_OUT captures (requires --music)')
    args = parser.parse_args()
    if args.stalks and not args.music:
        parser.error('--stalks requires --music')
    stock = args.stock.read_bytes()
    extended = append_backdrop(stock, args.backdrop.read_bytes())
    if args.stalks:
        extended = append_stalks(extended, args.stalks)
    payload = embed(args.music.read_bytes(), stock, extended) if args.music else extended
    with args.output.open('xb') as out:
        out.write(payload)


if __name__ == '__main__':
    main()
