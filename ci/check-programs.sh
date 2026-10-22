#!/bin/sh
# riscos-crossdev's checks (checks/check-all.sh: static, stack probes, FPU
# level, UnixLib) on every program the zips carry, as linked (the
# unstripped ELFs), after `make ffmpeg apps`. Run by `make check`; needs
# RCD_DIR, set in riscos-crossdev's container image (or a checkout of it).
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
: "${RCD_DIR:?set RCD_DIR to riscos-crossdev (its container image sets it)}"
FF=$TOP/src/ffmpeg-5.1.10
rc=0
for p in "$FF/ffmpeg_g" "$FF/ffprobe_g" "$FF/ffplay_g" \
         "$TOP/stage/bin/fffront" "$TOP/stage/bin/reel" "$TOP/stage/bin/reelegl" \
         "$TOP/stage/bin/videowin" "$TOP/stage/bin/videocube"; do
  echo "== $(basename "$p")"
  "$RCD_DIR/checks/check-all.sh" "$p" || rc=1
done
exit $rc
