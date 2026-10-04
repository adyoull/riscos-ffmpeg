#!/usr/bin/env python3
"""mkpgs.py OUT.sup N - a Blu-ray (PGS) subtitle file for the tests: N picture
subtitles, the k-th shown from k+0.2 s to k+0.8 s, each a white box 20 lines
high whose width (40 + 4k pixels) says which it is, on a 320x180 picture."""
import struct, sys

def seg(pts, kind, data):
    return b"PG" + struct.pack(">IIBH", int(pts * 90000), 0, kind, len(data)) + data

def rle_box(w, h):
    row = bytes([0, 0xC0 | (w >> 8), w & 0xFF, 1]) + b"\0\0"   # w pixels of colour 1, end of line
    return row * h

def display(pts, n, w=0, h=0, x=0, y=0):
    out = b""
    if n:
        pcs = struct.pack(">HHBHBBBB", 320, 180, 0x10, n, 0x80, 0, 0, 1) + struct.pack(">HBBHH", 0, 0, 0, x, y)
    else:
        pcs = struct.pack(">HHBHBBBB", 320, 180, 0x10, n, 0x00, 0, 0, 0)
    out += seg(pts, 0x16, pcs)
    out += seg(pts, 0x17, struct.pack(">BBHHHH", 1, 0, x, y, max(w, 1), max(h, 1)))
    if n:
        out += seg(pts, 0x14, struct.pack(">BB", 0, 0) + bytes([1, 235, 128, 128, 255]))
        rle = rle_box(w, h)
        ods = struct.pack(">HBB", 0, 0, 0xC0) + struct.pack(">I", len(rle) + 4)[1:] + struct.pack(">HH", w, h) + rle
        out += seg(pts, 0x15, ods)
    out += seg(pts, 0x80, b"")
    return out

out, n = sys.argv[1], int(sys.argv[2])
data = b""
for k in range(n):
    data += display(k + 0.2, 2 * k + 1, 40 + 4 * k, 20, 100, 140)
    data += display(k + 0.8, 2 * k + 2)
open(out, "wb").write(data)
