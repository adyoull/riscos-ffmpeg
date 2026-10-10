#!/usr/bin/env python3
"""Makes reelcore/sub_charsets.h: the Windows code pages subtitle files
are often saved in, bytes 0x80-0xFF as Unicode (0xFFFD: not used).

  tools/mkcharsets.py [OUT]"""
import sys
outp = sys.argv[1] if len(sys.argv) > 1 else "reelcore/sub_charsets.h"
SETS = ("cp1252", "cp1250", "cp1251", "cp1253", "cp1254")
out = ["/* Made by tools/mkcharsets.py (Python's codecs): Windows code pages",
       "   %s, bytes 0x80-0xFF as Unicode (0xFFFD: not used). */" % ", ".join(s[2:] for s in SETS),
       "static const unsigned short sub_cp_high[%d][128] = {" % len(SETS)]
for s in SETS:
    out.append("    {   /* %s */" % s)
    row = []
    for b in range(0x80, 0x100):
        try:
            row.append(ord(bytes([b]).decode(s)))
        except UnicodeDecodeError:
            row.append(0xFFFD)
    for i in range(0, 128, 16):
        out.append("        " + ",".join("0x%04X" % c for c in row[i:i + 16]) + ",")
    out.append("    },")
out.append("};")
open(outp, "w").write("\n".join(out) + "\n")
