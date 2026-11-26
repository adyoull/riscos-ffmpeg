#!/bin/sh
# unixlib-overlay.sh CROSSDEV UNIXLIB OUT - a copy of the riscos-crossdev
# 1.13 toolchain at CROSSDEV with riscos-unixlib 5.0.3.11 in place of its
# UnixLib 5.0.3.10, made at OUT; then build with RISCOS_TOOLCHAIN=OUT.
#
# UNIXLIB is riscos-unixlib's dist folder: libunixlib.a and include/ (the
# three headers changed since 5.0.3.10: pthread.h, stdlib.h, unistd.h).
# Until riscos-crossdev releases a toolchain with 5.0.3.11, this is how the
# builds get it. The library's assembler .data sections are aligned to 4,
# as crossdev's own build of UnixLib does (tools/elf-align-data.py), and
# the result is checked against the sha256 tools/check-unixlib.sh expects.
set -e
CROSSDEV=$1 UNIXLIB=$2 OUT=$3
[ -n "$OUT" ] || { echo "usage: $0 CROSSDEV UNIXLIB OUT" >&2; exit 2; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
TARGET=arm-riscos-gnueabihf
CROSSDEV_SHA=d343734348ec02ed260d2de506407f614bba5fa810043b0e3fa539ca01f72ba3   # crossdev 1.13: 5.0.3.10
RELEASE_SHA=4d1b3cc68cf906b66aa1a5cfe1f7321d39e0145336b454d7fbaa6df70e5a62a0    # riscos-unixlib 5.0.3.11
ALIGNED_SHA=$(sed -n 's/^UNIXLIB_5_0_3_11=//p' "$TOP/tools/check-unixlib.sh")

sha() { sha256sum "$1" | cut -d' ' -f1; }
[ "$(sha "$CROSSDEV/$TARGET/lib/libunixlib.a")" = $CROSSDEV_SHA ] ||
  { echo "$CROSSDEV: not riscos-crossdev 1.13's UnixLib" >&2; exit 1; }
[ "$(sha "$UNIXLIB/libunixlib.a")" = $RELEASE_SHA ] ||
  { echo "$UNIXLIB/libunixlib.a: not riscos-unixlib 5.0.3.11's" >&2; exit 1; }

rm -rf "$OUT"
cp -al "$CROSSDEV" "$OUT"           # hard links: each file replaced is unlinked first
for h in pthread.h stdlib.h unistd.h; do
  rm "$OUT/$TARGET/include/$h"; cp "$UNIXLIB/include/$h" "$OUT/$TARGET/include/$h"
done
rm "$OUT/$TARGET/lib/libunixlib.a"
cp "$UNIXLIB/libunixlib.a" "$OUT/$TARGET/lib/libunixlib.a"
python3 "$TOP/tools/elf-align-data.py" "$OUT/$TARGET/lib/libunixlib.a"
got=$(sha "$OUT/$TARGET/lib/libunixlib.a")
if [ -n "$ALIGNED_SHA" ] && [ "$got" != "$ALIGNED_SHA" ]; then
  echo "$OUT: libunixlib.a came out as $got, not $ALIGNED_SHA" >&2; exit 1
fi
echo "$OUT: riscos-crossdev 1.13 with UnixLib 5.0.3.11 (libunixlib.a $got)"
