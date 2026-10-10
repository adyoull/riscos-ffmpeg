#!/usr/bin/env python3
"""Makes reelcore/sub_font.h: DejaVu Sans Bold, proportional, as 8-bit
coverage bitmaps cropped to each glyph, at a few sizes, for subtitles
drawn into the picture. Characters 32-126, 160-255 (Latin-1), Latin
Extended-A, Romanian's S and T with commas, Greek, Cyrillic and some
punctuation (quotes, dashes, the ellipsis, the euro, notes).

  tools/mksubfont.py [FONT.ttf] [OUT]

DejaVu fonts: Bitstream Vera licence (third_party/dejavu/Licence)."""
import sys
from PIL import Image, ImageDraw, ImageFont

SIZES = (16, 19, 22, 27, 32, 38, 46, 56)
path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
outp = sys.argv[2] if len(sys.argv) > 2 else "reelcore/sub_font.h"
chars = (list(range(32, 127)) + list(range(160, 256)) + list(range(0x100, 0x180)) +
         [0x218, 0x219, 0x21A, 0x21B] + list(range(0x384, 0x38B)) + [0x38C] + list(range(0x38E, 0x3A2)) +
         list(range(0x3A3, 0x3CF)) + list(range(0x400, 0x460)) + list(range(0x490, 0x492)) +
         [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x20AC, 0x266A, 0x266B])
out = ["/* Made by tools/mksubfont.py from DejaVu Sans Bold (Bitstream Vera",
       "   licence): sizes %s px. The characters (Unicode) are in sub_cps, in" % ", ".join(map(str, SIZES)),
       "   order; their glyphs in the same order. Each glyph:",
       "   advance, the cropped bitmap's offset from the pen (x, and y down from",
       "   the line's top), its size, and where its coverage starts in data. */",
       "typedef struct { unsigned char adv; signed char x; unsigned char y, w, h; unsigned off; } SubGlyph;",
       "typedef struct { int size, line, ascent; const SubGlyph *g; const unsigned char *data; } SubFont;"]
out.append("#define SUB_GLYPHS %d" % len(chars))
out.append("static const unsigned short sub_cps[SUB_GLYPHS] = {")
for i in range(0, len(chars), 16):
    out.append("    " + ",".join("0x%X" % c for c in chars[i:i + 16]) + ",")
out.append("};")
fonts = []
for size in SIZES:
    font = ImageFont.truetype(path, size)
    asc, desc = font.getmetrics()
    line = asc + desc
    data = bytearray()
    glyphs = []
    for c in chars:
        s = chr(c)
        adv = int(round(font.getlength(s)))
        pad = size
        im = Image.new("L", (adv + 2 * pad, line + 2 * pad), 0)
        ImageDraw.Draw(im).text((pad, pad), s, font=font, fill=255)
        bb = im.getbbox()
        if not bb:
            glyphs.append((adv, 0, 0, 0, 0, len(data)))
            continue
        crop = im.crop(bb)
        glyphs.append((adv, bb[0] - pad, bb[1] - pad, crop.width, crop.height, len(data)))
        data += crop.tobytes()
    out.append("static const SubGlyph sub_glyphs_%d[SUB_GLYPHS] = {" % size)
    for g in glyphs:
        out.append("    { %d, %d, %d, %d, %d, %d }," % (g[0], g[1], max(g[2], 0), g[3], g[4], g[5]))
    out.append("};")
    out.append("static const unsigned char sub_data_%d[%d] = {" % (size, len(data)))
    for i in range(0, len(data), 40):
        out.append("    " + ",".join(str(b) for b in data[i:i + 40]) + ",")
    out.append("};")
    fonts.append((size, line, asc))
out.append("static const SubFont sub_fonts[%d] = {" % len(fonts))
for size, line, asc in fonts:
    out.append("    { %d, %d, %d, sub_glyphs_%d, sub_data_%d }," % (size, line, asc, size, size))
out.append("};")
open(outp, "w").write("\n".join(out) + "\n")
print("sizes", SIZES)
