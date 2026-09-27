#!/usr/bin/env python3
"""Write !Sprites for !FFmpeg: '!ffmpeg' (34x34) and 'sm!ffmpeg' (17x17),
plus the 180 dpi versions ('!ffmpeg' in a 22 file is not needed: RISC OS 5
picks the 90 dpi sprite and scales it). 32 bpp (TBGR) new-format sprites
with a 1 bpp mask. The picture is our own: a film frame with a play
triangle, drawn from shapes here.

Usage: mksprites.py OUT_FILE   (write it as !Sprites,ff9)
"""
import struct
import sys


def draw(size):
    """Returns rows of (r, g, b, opaque) for a size x size icon."""
    s = size / 34.0
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = (x + 0.5) / s, (y + 0.5) / s     # in 34-unit space
            px = None
            # rounded dark frame 2..32
            if 2 <= fx <= 32 and 4 <= fy <= 30:
                corner = min(fx - 2, 32 - fx, fy - 4, 30 - fy)
                px = (40, 44, 52)
                # sprocket holes along top and bottom
                if (fy < 7 or fy > 27) and int(fx - 2) % 5 in (1, 2):
                    px = (230, 230, 230)
                # picture area
                if 5 <= fx <= 29 and 8 <= fy <= 26:
                    px = (30, 110, 200)
                    # play triangle: apex at right
                    tx, ty = fx - 12, fy - 17
                    if 0 <= tx <= 11 and abs(ty) <= (11 - tx) * 0.55:
                        px = (250, 250, 250)
                if corner < 0.6:
                    px = None if corner < 0.2 else (40, 44, 52)
            row.append(px)
        rows.append(row)
    return rows


def sprite(name, size):
    rows = draw(size)
    w, h = size, size
    img = bytearray()
    mask = bytearray()
    mask_row_words = (w + 31) // 32
    for y in range(h):                 # sprites are stored top row first
        mbits = [0] * (mask_row_words * 32)
        for x in range(w):
            px = rows[y][x]
            if px:
                r, g, b = px
                img += bytes((r, g, b, 0))      # TBGR: 0x00BBGGRR
                mbits[x] = 1
            else:
                img += b"\0\0\0\0"
        for wd in range(mask_row_words):
            v = 0
            for bit in range(32):
                if mbits[wd * 32 + bit]:
                    v |= 1 << bit
            mask += struct.pack("<I", v)
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1
    hdr_len = 44
    img_off = hdr_len
    mask_off = img_off + len(img)
    total = mask_off + len(mask)
    hdr = struct.pack("<I12siiiiiii", total, name.encode().ljust(12, b"\0"),
                      w - 1, h - 1, 0, 31, img_off, mask_off, mode)
    return hdr + img + mask


def main():
    sprites = [sprite("!ffmpeg", 34), sprite("sm!ffmpeg", 17)]
    body = b"".join(sprites)
    # file = sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(sys.argv[1], "wb").write(area)


main()
