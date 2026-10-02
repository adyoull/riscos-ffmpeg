#!/bin/bash
# tools/eabiprobe/build.sh [VERSION] - builds EABIProbe-VERSION.zip in dist/
# (eabiprobe for RISC OS, the three Obey files and the ReadMe).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
TMP=$(mktemp -d)
mkdir "$TMP/EABIProbe"
${CROSS}gcc -O2 -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -Wall -static \
  -o "$TMP/eabiprobe.elf" "$HERE/eabiprobe.c"
"$ELF2AIF" -e "$TMP/eabiprobe.elf" "$TMP/EABIProbe/eabiprobe,ff8" >/dev/null
cp "$HERE"/app/* "$TMP/EABIProbe/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/EABIProbe-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/EABIProbe-$V.zip" EABIProbe )
rm -rf "$TMP"
echo "dist/EABIProbe-$V.zip"
