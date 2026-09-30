#!/bin/bash
# tools/mmalprobe/build.sh [VERSION] - builds MMALProbe-VERSION.zip in dist/
# (mmalprobe for RISC OS, the Probe Obey file and the ReadMe).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
TMP=$(mktemp -d)
mkdir "$TMP/MMALProbe"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/mmalprobe.elf" "$HERE/mmalprobe.c"
"$ELF2AIF" -e "$TMP/mmalprobe.elf" "$TMP/MMALProbe/mmalprobe,ff8" >/dev/null
cp "$HERE"/app/* "$TMP/MMALProbe/"
mkdir -p "$TOP/dist"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/MMALProbe-$V.zip" MMALProbe )
rm -rf "$TMP"
echo "dist/MMALProbe-$V.zip"
