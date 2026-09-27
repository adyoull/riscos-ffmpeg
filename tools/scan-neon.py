#!/usr/bin/env python3
"""Second check on a finished RISC OS binary: disassemble it and list, per
function, the NEON loads/stores that could still fault with alignment
checking on (element wider than 8 bits, no alignment qualifier).

What should be left:
 - compiled C (GCC only emits these for typed data: int16/int32/float
   arrays and SDL's Uint32 pixels, which are aligned to their elements);
 - assembly listed in tools/neon-align-*.allow (16-bit pixels, int16
   coefficients, uint16 CDFs, int32 tables).
Anything else in hand-written code (names ending _neon, ff_*, dav1d_*,
x264_*) wants a look.

Usage: scan-neon.py BINARY [OBJDUMP]   (default arm-riscos-gnueabihf-objdump)
"""
import collections
import re
import subprocess
import sys

objdump = sys.argv[2] if len(sys.argv) > 2 else "arm-riscos-gnueabihf-objdump"
out = subprocess.run([objdump, "-d", "--no-show-raw-insn", sys.argv[1]],
                     capture_output=True, text=True, check=True).stdout
fn = None
hits = collections.defaultdict(collections.Counter)
insn = re.compile(r"\tv(ld|st)[1-4]\.(16|32|64)\t(.*)$")
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
    if m:
        fn = m.group(1)
        continue
    m = insn.search(line)
    if m and not re.search(r":(16|32|64|128|256)\]", m.group(3)):
        hits[fn][re.sub(r"\s+", " ", line.split("\t", 2)[-1].strip())] += 1
asm = {f: c for f, c in hits.items() if re.search(r"_neon|^ff_|^dav1d_|^x264_|8tap|cdef|grain", f)}
print("%d functions with element-wide NEON accesses; %d look hand-written:" % (len(hits), len(asm)))
for f in sorted(asm):
    print("  %-50s %s" % (f, ", ".join(sorted(set(k.split(" ")[0] for k in asm[f])))))
