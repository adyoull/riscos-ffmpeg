#!/bin/bash
# reelhwaccel/hevchw/build.sh [VERSION] - builds the HEVCHW module and HEVCHW-VERSION.zip
# in dist/ (the module, the Test and IRQTest Obey files and the ReadMe).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
V=${1:-0.1}
TMP=$(mktemp -d)
"$HERE/module/build.sh" "$TMP/out" >/dev/null
mkdir "$TMP/HEVCHW"
cp "$TMP/out/HEVCHW,ffa" "$HERE"/app/* "$TMP/HEVCHW/"
mkdir -p "$TOP/dist"
rm -f "$TOP/dist/HEVCHW-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$TOP/dist/HEVCHW-$V.zip" HEVCHW )
rm -rf "$TMP"
echo "dist/HEVCHW-$V.zip"
