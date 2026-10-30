#!/usr/bin/env python3
"""Copies a Matroska file with its Cues (the index) turned into a Void
element: as a muxer that wrote none, but with the Duration kept.
   mkv_nocues.py IN.mkv OUT.mkv"""
import sys

def vint(b, i):
    """An EBML size at b[i]: (its value, its length)"""
    n = 1
    while not b[i] & (0x80 >> (n - 1)):
        n += 1
    x = b[i] & ((0x80 >> (n - 1)) - 1)
    for k in range(1, n):
        x = x << 8 | b[i + k]
    return x, n

b = bytearray(open(sys.argv[1], 'rb').read())
at = b.rfind(b'\x1c\x53\xbb\x6b')
if at < 0:
    sys.exit('no Cues in ' + sys.argv[1])
size, n = vint(b, at + 4)
total = 4 + n + size
# Void (0xEC) with an 8-byte size: the same number of bytes in all
b[at:at + 9] = b'\xec\x01' + (total - 9).to_bytes(7, 'big')
open(sys.argv[2], 'wb').write(b)
