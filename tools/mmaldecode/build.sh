#!/bin/bash
# tools/mmaldecode/build.sh [VERSION] - builds MMALDecode-VERSION.zip in dist/:
# mmaldecode for RISC OS, three H.264 test clips made here with the host's
# ffmpeg and x264 (640x360 Main with B frames, 426x240 Baseline, 1920x1080
# High), each with FFmpeg's own per-picture checksums (-f framecrc), the
# Test Obey file and the ReadMe.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
FFMPEG=${FFMPEG:-ffmpeg}
TMP=$(mktemp -d)
mkdir -p "$TMP/MMALDecode/clips"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/mmaldecode.elf" "$HERE/mmaldecode.c"
"$ELF2AIF" -e "$TMP/mmaldecode.elf" "$TMP/MMALDecode/mmaldecode,ff8" >/dev/null
C="$TMP/MMALDecode/clips"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -frames:v 60 -c:v libx264 -profile:v main -bf 2 \
  -pix_fmt yuv420p "$C/small.h264"   # (moved to clips/NAME/stream below: no dots in RISC OS names)
$FFMPEG -v error -y -f lavfi -i testsrc2=size=426x240:rate=25 -frames:v 30 -c:v libx264 -profile:v baseline \
  -pix_fmt yuv420p "$C/odd.h264"
$FFMPEG -v error -y -f lavfi -i testsrc2=size=1920x1080:rate=30 -frames:v 150 -c:v libx264 -profile:v high \
  -level 4.1 -crf 26 -bf 3 -pix_fmt yuv420p "$C/hd.h264"
for c in small odd hd; do
  mkdir "$C/$c"
  $FFMPEG -v error -y -i "$C/$c.h264" -f framecrc -pix_fmt yuv420p "$C/$c/crc"
  dims=$($FFMPEG -v error -i "$C/$c.h264" -f framecrc - 2>/dev/null | sed -n 's/^#dimensions 0: //p')
  $FFMPEG -v error -i "$C/$c.h264" -f rawvideo -pix_fmt yuv420p - | python3 "$HERE/mksig.py" ${dims%x*} ${dims#*x} > "$C/$c/sig"
  mv "$C/$c.h264" "$C/$c/stream"
done
cp "$HERE"/app/* "$TMP/MMALDecode/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/MMALDecode-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/MMALDecode-$V.zip" MMALDecode )
rm -rf "$TMP"
echo "dist/MMALDecode-$V.zip"
