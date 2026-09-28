#!/bin/bash
# check-fresh-tree.sh [TREE]
# Proves the patch series is the source of truth: unpacks FFmpeg's tarball
# afresh, applies patches/ffmpeg/*.patch, runs the same NEON alignment pass
# as build/build-ffmpeg.sh, and compares the ARM code and the files the
# patches touch with TREE (default: src/ffmpeg-5.1.10, the tree that was
# built and tested). Also checks that the NEON written for this port
# (patches 0015-0017) passes tools/neon-align.py untouched, so the pass
# never changes it: it must be alignment-safe as written.
# Exit status 0 when all is the same.
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
TREE=$(cd "${1:-$TOP/src/ffmpeg-5.1.10}" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
tar xf "$TOP/dl/ffmpeg-5.1.10.tar.xz" -C "$TMP"
cd "$TMP/ffmpeg-5.1.10"
for p in "$TOP"/patches/ffmpeg/*.patch; do patch -s -p1 < "$p"; done
bad=0
for f in libavfilter/arm/vf_yadif_neon.S libavcodec/arm/hevcdsp_epel_neon.S libswscale/arm/scaled_rgb_neon.S; do
  python3 "$TOP/tools/neon-align.py" --check "$f" 2>/dev/null || { echo "not alignment-safe as written: $f"; bad=1; }
done
find libavcodec/arm libavutil/arm libswscale/arm libswresample/arm libavfilter/arm -name '*.S' -print0 |
  xargs -0 "$TOP/tools/neon-align-apply.sh" "$TOP/tools/neon-align-ffmpeg.allow" 2>&1 | tail -1
for d in libavcodec/arm libavutil/arm libswscale/arm libswresample/arm libavfilter/arm; do
  diff -rq -x '*.o' -x '*.d' -x '*.orig' "$d" "$TREE/$d" || bad=1
done
for f in $(cat "$TOP"/patches/ffmpeg/*.patch | sed -n 's#^+++ b/##p' | sort -u); do
  [ -f "$TREE/$f" ] && { cmp -s "$f" "$TREE/$f" || case "$f" in *.S) ;; *) echo "differs: $f"; bad=1;; esac; }
done
[ $bad = 0 ] && echo "fresh tree: the same as $TREE" || echo "fresh tree: DIFFERENT from $TREE"
exit $bad
