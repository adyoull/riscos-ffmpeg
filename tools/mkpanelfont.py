#!/usr/bin/env python3
"""Makes reelcore/panel_font.h: DejaVu Sans Mono Bold as 8-bit coverage
bitmaps in fixed cells, at a few sizes, for reelcore's stats panel (drawn
into the picture at the size it will be seen: an overlay's picture can be
smaller or bigger than the screen shows it). Characters 32-126 and 160-255
(Latin-1, as RISC OS titles are).

  tools/mkpanelfont.py [FONT.ttf] [OUT]

DejaVu fonts: Bitstream Vera licence (free to use, modify and redistribute,
including embedded in software; see third_party/dejavu/Licence)."""
import sys
from PIL import Image, ImageDraw, ImageFont

SIZES = (9, 10, 12, 15, 18, 22, 27, 33)
path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
outp = sys.argv[2] if len(sys.argv) > 2 else "reelcore/panel_font.h"
chars = list(range(32, 127)) + list(range(160, 256))
out = ["/* Made by tools/mkpanelfont.py from DejaVu Sans Mono Bold (Bitstream Vera",
       "   licence): sizes %s px. Characters 32-126 then 160-255, each a" % ", ".join(map(str, SIZES)),
       "   w x h cell of 8-bit coverage, one after another. */",
       "typedef struct { int size, w, h; const unsigned char *data; } PanelFont;"]
fonts = []
for size in SIZES:
    font = ImageFont.truetype(path, size)
    asc, desc = font.getmetrics()
    cw = int(round(font.getlength("M")))
    ch = asc + desc
    data = bytearray()
    for c in chars:
        im = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(im).text((0, 0), bytes([c]).decode("latin-1"), font=font, fill=255)
        data += im.tobytes()
    out.append("static const unsigned char panel_font_%d[%d] = {" % (size, len(data)))
    for i in range(0, len(data), 40):
        out.append("    " + ",".join(str(b) for b in data[i:i + 40]) + ",")
    out.append("};")
    fonts.append((size, cw, ch))
    print("%d px: cell %dx%d" % (size, cw, ch))
out.append("static const PanelFont panel_fonts[] = {")
for size, cw, ch in fonts:
    out.append("    { %d, %d, %d, panel_font_%d }," % (size, cw, ch, size))
out.append("};")
open(outp, "w").write("\n".join(out) + "\n")
