#!/bin/sh
# unixlib-overlay.sh CROSSDEV UNIXLIB OUT - a copy of the riscos-crossdev
# 1.13 toolchain at CROSSDEV with riscos-unixlib 5.0.3.12 in place of its
# UnixLib 5.0.3.10, made at OUT; then build with RISCOS_TOOLCHAIN=OUT.
#
# UNIXLIB is riscos-unixlib's release/v5.0.3.12 folder (libunixlib.a) with
# the three headers changed since 5.0.3.10 (pthread.h, stdlib.h, unistd.h;
# riscos-unixlib's dist/include) in an include/ beside it, or dist/ itself.
# Until riscos-crossdev releases a toolchain with 5.0.3.12, this is how the
# builds get it. 5.0.3.12 is the first release whose assembler .data is
# word-aligned as crossdev's own build makes it, so it's used as released;
# crossdev 1.13's crt0.o (aligned, the same code) stays. The library's
# sha256 is the one tools/check-unixlib.sh expects.
set -e
CROSSDEV=$1 UNIXLIB=$2 OUT=$3
[ -n "$OUT" ] || { echo "usage: $0 CROSSDEV UNIXLIB OUT" >&2; exit 2; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
TARGET=arm-riscos-gnueabihf
CROSSDEV_SHA=$(sed -n 's/^UNIXLIB_5_0_3_10=//p' "$TOP/tools/check-unixlib.sh")   # crossdev 1.13's
RELEASE_SHA=$(sed -n 's/^UNIXLIB_5_0_3_12=//p' "$TOP/tools/check-unixlib.sh")

sha() { sha256sum "$1" | cut -d' ' -f1; }
[ "$(sha "$CROSSDEV/$TARGET/lib/libunixlib.a")" = "$CROSSDEV_SHA" ] ||
  { echo "$CROSSDEV: not riscos-crossdev 1.13's UnixLib" >&2; exit 1; }
[ "$(sha "$UNIXLIB/libunixlib.a")" = "$RELEASE_SHA" ] ||
  { echo "$UNIXLIB/libunixlib.a: not riscos-unixlib 5.0.3.12's" >&2; exit 1; }

rm -rf "$OUT"
cp -al "$CROSSDEV" "$OUT"           # hard links: each file replaced is unlinked first
for h in pthread.h stdlib.h unistd.h; do
  rm "$OUT/$TARGET/include/$h"; cp "$UNIXLIB/include/$h" "$OUT/$TARGET/include/$h"
done
rm "$OUT/$TARGET/lib/libunixlib.a"
cp "$UNIXLIB/libunixlib.a" "$OUT/$TARGET/lib/libunixlib.a"
echo "$OUT: riscos-crossdev 1.13 with UnixLib 5.0.3.12"
