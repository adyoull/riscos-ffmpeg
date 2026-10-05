#!/usr/bin/env python3
"""skip_loop_filter and skip_frame for VP9 and AV1 (FFmpeg patch 0026, dav1d's
dav1d_riscos_set_skip): checks a decode made with one against the decode made
without.  Pictures are matched by time (framemd5's pts in its #tb, ffprobe's
best_effort_timestamp_time), in ms.

  skipcheck.py lf MODE default.framemd5 skipped.framemd5 info.csv refs.txt
      the loop filter skipped for MODE (noref, nointra, nokey, all): the
      pictures it still filters are unchanged, and some it doesn't are changed
  skipcheck.py kf default.framemd5 skipped.framemd5 info.csv
      skip_frame nokey: just the keyframes, unchanged
  skipcheck.py same a.framemd5 b.framemd5
      the same pictures at the same times
  skipcheck.py times x.framemd5 > refs.txt
  skipcheck.py vp9refs trace.txt info.csv > refs.txt

info.csv: ffprobe's frame=key_frame,pict_type,best_effort_timestamp_time
(csv: key_frame, time, pict_type).  refs.txt: the times of the pictures
others are predicted from."""
import sys
from fractions import Fraction


def md5s(path):
    out, tb = {}, Fraction(1, 1000)
    for line in open(path):
        if line.startswith('#tb 0:'):
            tb = Fraction(line.split(':')[1].strip())
        if line.startswith('#'):
            continue
        f = [x.strip() for x in line.split(',')]
        out[round(int(f[2]) * tb * 1000)] = f[-1]
    return out


def info(path):
    out = {}
    for line in open(path):
        f = line.strip().split(',')
        if len(f) >= 4 and f[0] == 'frame' and f[2] not in ('', 'N/A'):
            out[round(float(f[2]) * 1000)] = (f[1] == '1', f[3])
    return out


def vp9refs(trace, inf):
    """The VP9 pictures that refresh a reference (trace_headers' log, in
    decode order; a clip without hidden frames, so the pictures' order)."""
    frames = []
    for line in open(trace):
        if ' frame_type ' in line:
            frames.append(line.rstrip().endswith('= 0'))   # keyframes refresh all
        elif ' refresh_frame_flags ' in line and frames:
            frames[-1] = frames[-1] or not line.rstrip().endswith('= 0')
    pts = sorted(inf)
    if len(pts) != len(frames) or not frames:
        raise SystemExit("vp9refs: %d frames, %d pictures" % (len(frames), len(pts)))
    for p, r in zip(pts, frames):
        if r:
            print(p)
    return 0


def main():
    what = sys.argv[1]
    if what == 'vp9refs':
        return vp9refs(sys.argv[2], info(sys.argv[3]))
    if what == 'times':
        for p in sorted(md5s(sys.argv[2])):
            print(p)
        return 0
    if what == 'same':
        a, b = md5s(sys.argv[2]), md5s(sys.argv[3])
        return 0 if a == b and a else 1
    if what == 'lf':
        mode, d, s, inf, refs = sys.argv[2], md5s(sys.argv[3]), md5s(sys.argv[4]), info(sys.argv[5]), sys.argv[6]
        refset = {int(x) for x in open(refs).read().split()}
        if set(d) != set(s) or len(d) < 10 or not set(d) <= set(inf):
            print("FAIL: %s: %d pictures, %d without skipping, %d in the info" % (mode, len(s), len(d), len(inf)))
            return 1

        def kept(p):                       # still filtered under this mode
            key, t = inf[p]
            return {'noref': p in refset, 'nointra': t == 'I', 'nokey': key, 'all': False}[mode]
        keep = [p for p in d if kept(p)]
        rest = [p for p in d if not kept(p)]
        bad = [p for p in keep if d[p] != s[p]]
        changed = [p for p in rest if d[p] != s[p]]
        print("  skip_loop_filter %s: %d pictures still filtered (%d changed), %d not (%d changed)"
              % (mode, len(keep), len(bad), len(rest), len(changed)))
        if bad or not changed:
            print("FAIL: skip_loop_filter %s" % mode)
            return 1
        return 0
    d, s, inf = md5s(sys.argv[2]), md5s(sys.argv[3]), info(sys.argv[4])
    keys = {p for p in d if inf[p][0]}
    ok = set(s) == keys and all(s[p] == d[p] for p in s)
    print("  skip_frame nokey: %d pictures (%d keyframes)%s" % (len(s), len(keys), "" if ok else " FAIL"))
    return 0 if ok and keys else 1


sys.exit(main())
