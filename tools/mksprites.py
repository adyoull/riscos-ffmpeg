#!/usr/bin/env python3
"""Write !Sprites for !FFmpeg: '!ffmpeg' (34x34) and 'sm!ffmpeg' (17x17),
plus the 180 dpi versions ('!ffmpeg' in a 22 file is not needed: RISC OS 5
picks the 90 dpi sprite and scales it). 32 bpp (TBGR) new-format sprites
with a 1 bpp mask. The picture is our own: a film frame with a play
triangle, drawn from shapes here.

Usage: mksprites.py [--reel] OUT_FILE   (write it as !Sprites,ff9;
       --reel: !Reel's film reel icon instead; --reelegl: !ReelEGL's)
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


def draw_reel_egl(size):
    """!ReelEGL: the reel with a green hub."""
    rows = draw_reel(size)
    return [[(40, 170, 60) if px == (200, 40, 40) else px for px in row] for row in rows]


def draw_reel(size):
    """!Reel: a film reel (a grey disc with five holes and a hub) with a
    strip of film running off it to the right."""
    import math
    s = size / 34.0
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = (x + 0.5) / s, (y + 0.5) / s
            px = None
            # film strip: along the bottom right
            if 17 <= fx <= 33 and 25 <= fy <= 31:
                px = (40, 44, 52)
                if (fy < 26.6 or fy > 29.4) and int(fx) % 3 == 0:
                    px = (230, 230, 230)
            cx, cy, r = 15.5, 15.5, 13.5
            d = math.hypot(fx - cx, fy - cy)
            if d <= r:
                px = (175, 180, 190) if d > r - 1.2 else (205, 210, 218)
                ang = math.atan2(fy - cy, fx - cx)
                for k in range(5):
                    hx = cx + 7.2 * math.cos(ang * 0 + k * 2 * math.pi / 5 - math.pi / 2)
                    hy = cy + 7.2 * math.sin(k * 2 * math.pi / 5 - math.pi / 2)
                    if math.hypot(fx - hx, fy - hy) <= 3.3:
                        px = (40, 44, 52)
                if d <= 2.4:
                    px = (200, 40, 40)
                if d <= 1.0:
                    px = (40, 44, 52)
            row.append(px)
        rows.append(row)
    return rows


def draw_grip(size):
    """Reel's resize grip (bottom right of its windows): three diagonal
    ridges in the corner, dark with a light edge, like the desktop's own
    size icon. Transparent elsewhere."""
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            k = x + y
            px = None
            for ridge in (size + 3, size + 7, size + 11):
                if k == ridge:
                    px = (68, 68, 68)
                elif k == ridge + 1:
                    px = (255, 255, 255)
            row.append(px)
        rows.append(row)
    return rows


def sprite(name, size, drawer=None):
    rows = (drawer or draw)(size)
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
    if len(sys.argv) > 2 and sys.argv[1] == "--reel":
        sys.argv.pop(1)
        sprites = [sprite("!reel", 34, draw_reel), sprite("sm!reel", 17, draw_reel),
                   sprite("reelgrip", 16, draw_grip)]
    elif len(sys.argv) > 2 and sys.argv[1] == "--reelegl":
        sys.argv.pop(1)
        sprites = [sprite("!reelegl", 34, draw_reel_egl), sprite("sm!reelegl", 17, draw_reel_egl),
                   sprite("reelgrip", 16, draw_grip)]
    else:
        sprites = [sprite("!ffmpeg", 34), sprite("sm!ffmpeg", 17)]
    body = b"".join(sprites)
    # file = sprite area without its first word: count, first, free
    area = struct.pack("<iii", len(sprites), 16, 16 + len(body)) + body
    open(sys.argv[1], "wb").write(area)


main()
