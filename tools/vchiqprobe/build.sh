#!/bin/bash
# tools/vchiqprobe/build.sh [VERSION] - builds VCHIQProbe-VERSION.zip in dist/
# (vchiqprobe for RISC OS, the Probe Obey file and the ReadMe).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
TMP=$(mktemp -d)
mkdir "$TMP/VCHIQProbe"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/vchiqprobe.elf" "$HERE/vchiqprobe.c"
"$ELF2AIF" -e "$TMP/vchiqprobe.elf" "$TMP/VCHIQProbe/vchiqprobe,ff8" >/dev/null
cp "$HERE"/app/* "$TMP/VCHIQProbe/"
mkdir -p "$TOP/dist"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/VCHIQProbe-$V.zip" VCHIQProbe )
rm -rf "$TMP"
echo "dist/VCHIQProbe-$V.zip"
