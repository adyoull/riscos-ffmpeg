#!/bin/bash
# tools/hevcprobe/build.sh [VERSION] - builds HEVCProbe-VERSION.zip in dist/
# (hevcprobe for RISC OS, and the Probe / ProbeNoClock Obey files).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
TMP=$(mktemp -d)
mkdir "$TMP/HEVCProbe"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/hevcprobe.elf" "$HERE/hevcprobe.c"
"$ELF2AIF" -e "$TMP/hevcprobe.elf" "$TMP/HEVCProbe/hevcprobe,ff8" >/dev/null
cp "$HERE"/app/* "$TMP/HEVCProbe/"
mkdir -p "$TOP/dist"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/HEVCProbe-$V.zip" HEVCProbe )
rm -rf "$TMP"
echo "dist/HEVCProbe-$V.zip"
