#!/usr/bin/env python3
"""Makes reelcore/panel_font.h: DejaVu Sans Mono Bold as 8-bit coverage
bitmaps in fixed cells, for reelcore's stats panel (drawn into the picture).
Characters 32-126 and 160-255 (Latin-1, as RISC OS titles are).

  tools/mkpanelfont.py [SIZE] [FONT.ttf]   (default 15 px)

DejaVu fonts: Bitstream Vera licence (free to use, modify and redistribute,
including embedded in software; see docs/Licences)."""
import sys
from PIL import Image, ImageDraw, ImageFont

size = int(sys.argv[1]) if len(sys.argv) > 1 else 15
path = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
font = ImageFont.truetype(path, size)
chars = list(range(32, 127)) + list(range(160, 256))
asc, desc = font.getmetrics()
cw = int(round(font.getlength("M")))
ch = asc + desc
rows = []
for c in chars:
    im = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(im).text((0, 0), bytes([c]).decode("latin-1"), font=font, fill=255)
    rows.append(im.tobytes())
out = ["/* Made by tools/mkpanelfont.py from DejaVu Sans Mono Bold, %d px" % size,
       "   (Bitstream Vera licence). Characters 32-126 then 160-255, each a",
       "   PANEL_FONT_W x PANEL_FONT_H cell of 8-bit coverage. */",
       "#define PANEL_FONT_W %d" % cw, "#define PANEL_FONT_H %d" % ch,
       "static const unsigned char panel_font[%d][%d] = {" % (len(chars), cw * ch)]
for c, r in zip(chars, rows):
    out.append("    { /* %d */ %s }," % (c, ",".join(str(b) for b in r)))
out.append("};")
open(sys.argv[3] if len(sys.argv) > 3 else "reelcore/panel_font.h", "w").write("\n".join(out) + "\n")
print("cell %dx%d, %d characters" % (cw, ch, len(chars)))
