#!/usr/bin/env python3
"""mksig.py W H < raw yuv420p frames > sig - block-mean signatures of each
picture for mmaldecode (an 8x8 grid of Y, 4x4 of U and of V: 96 numbers a
line, 2 decimals), the same grid as mmaldecode.c's frame_sig()."""
import sys
import numpy as np

def grid(p, n):
    h, w = p.shape
    out = []
    for gy in range(n):
        for gx in range(n):
            y0, y1 = h * gy // n, h * (gy + 1) // n
            x0, x1 = w * gx // n, w * (gx + 1) // n
            out.append(p[y0:y1, x0:x1].mean())
    return out

def sig(frame, w, h):
    cw, ch = (w + 1) // 2, (h + 1) // 2
    y = frame[:w * h].reshape(h, w).astype(np.float64)
    u = frame[w * h:w * h + cw * ch].reshape(ch, cw).astype(np.float64)
    v = frame[w * h + cw * ch:w * h + 2 * cw * ch].reshape(ch, cw).astype(np.float64)
    return grid(y, 8) + grid(u, 4) + grid(v, 4)

if __name__ == '__main__':
    w, h = int(sys.argv[1]), int(sys.argv[2])
    n = w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)
    data = np.frombuffer(sys.stdin.buffer.read(), np.uint8)
    for i in range(len(data) // n):
        print(' '.join('%.2f' % x for x in sig(data[i * n:(i + 1) * n], w, h)))
