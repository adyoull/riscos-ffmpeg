#!/bin/bash
# -movflags +faststart (the Convert window uses it for MP4 and M4A) moves
# the index to the front of the file. FFmpeg does that by opening the
# output a second time for reading, which RISC OS refuses ("This file is
# already open"). Patch 0020 shifts the data in place instead through one
# read-write opening; on RISC OS always, here when FFMPEG_SHIFT_IN_PLACE is
# set. The output must be byte for byte what the usual way makes: an MP4
# (video and sound, several 256 KB chunks), a MOV and an M4A.
# Usage: tests/qemu/faststart.sh   (after a LINUX_ARM_TEST=1 build)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
FF=${FF:-$TOP/src-linuxarm/ffmpeg-5.1.10/ffmpeg_g}
W=${WORK:-$HERE/out}/faststart
mkdir -p "$W"
bad=0
ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25:duration=12 -f lavfi -i sine=d=12 \
  -c:v mpeg2video -q:v 4 -c:a mp2 "$W/src.mpg" || exit 1
one() {   # name, extra args
  local n=$1; shift
  local a=(-hide_banner -loglevel error -nostdin -threads 1 -i "$W/src.mpg" "$@" -movflags +faststart -bitexact -y)
  rm -f "$W/usual.$n" "$W/inplace.$n"
  "$HERE/aligntrap.sh" "$FF" "${a[@]}" "$W/usual.$n" || { echo "FAIL: $n: the usual way failed"; bad=1; return; }
  FFMPEG_SHIFT_IN_PLACE=1 "$HERE/aligntrap.sh" "$FF" "${a[@]}" "$W/inplace.$n" 2>"$W/inplace.$n.err" ||
    { echo "FAIL: $n: in place failed: $(cat "$W/inplace.$n.err")"; bad=1; return; }
  if [ -s "$W/inplace.$n.err" ]; then echo "FAIL: $n: $(cat "$W/inplace.$n.err")"; bad=1; fi
  if cmp -s "$W/usual.$n" "$W/inplace.$n"; then
    echo "  $n: in place the same as usual ($(stat -c %s "$W/inplace.$n") bytes)"
  else
    echo "FAIL: $n: in place differs from usual"; bad=1
  fi
}
one mp4 -c:v libx264 -preset veryfast -c:a aac -b:a 128k
one mov -c:v libx264 -preset ultrafast -an
one m4a -vn -c:a aac
[ $bad -eq 0 ] && echo "all passed" || echo "FAILED"
exit $bad
