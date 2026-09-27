#!/bin/bash
# Release files, from a finished build (build-deps.sh + build-ffmpeg.sh):
#   dist/FFmpeg-VERSION.zip             !FFmpeg: ffmpeg, ffprobe, ffplay (AIF),
#                                       Bench, docs (licences, source)
#   dist/riscos-ffmpeg-devkit-VERSION.tgz  static libraries + headers + .pc
# Filetypes go in the zip's Acorn extra fields (tools/mkrozip.py), so SparkFS
# and RISC OS unzip give the files their real types.
# Usage: build/package.sh [VERSION]      (default 5.1.10-riscos1)
#        ELF2AIF=path/to/elf2aif        (host elf2aif; see tools/elf2aif)
set -euo pipefail
source "$(dirname "$0")/env.sh"
V=${1:-5.1.10-riscos1}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
[ -x "$ELF2AIF" ] || { echo "no elf2aif at $ELF2AIF (make -C tools/elf2aif GCCSDK_SRC=...)" >&2; exit 1; }
FF=$SRC/ffmpeg-5.1.10
DIST=$TOP/dist
mkdir -p "$DIST"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- the application ---------------------------------------------------
A=$TMP/'!FFmpeg'
cp -r "$TOP/app/!FFmpeg" "$A"
python3 "$TOP/tools/mksprites.py" "$A/!Sprites,ff9"
for p in ffmpeg ffprobe ffplay; do
  ${CROSS}strip -o "$TMP/$p.elf" "$FF/${p}_g"
  "$ELF2AIF" -e "$TMP/$p.elf" "$A/$p,ff8" >/dev/null
done
D=$A/docs
mkdir -p "$D/Licences" "$D/source"
cp "$TOP/README.md" "$D/ReadMe,fff"
cp "$TOP/CHANGELOG.md" "$D/Changes,fff"
cp "$TOP/docs/SOURCES.md" "$D/SOURCES,fff"
cp "$FF/COPYING.GPLv2"            "$D/Licences/GPLv2,fff"
cp "$FF/COPYING.LGPLv2.1"         "$D/Licences/LGPLv21,fff"
cp "$FF/LICENSE.md"               "$D/Licences/FFmpeg,fff"
cp "$SRC/x264-master/COPYING"     "$D/Licences/x264,fff"
cp "$SRC/dav1d-1.5.4/COPYING"     "$D/Licences/dav1d,fff"
cp "$SRC/lame-3.100/COPYING"      "$D/Licences/LAME,fff"
cp "$SRC/opus-1.5.2/COPYING"      "$D/Licences/Opus,fff"
cp "$SRC/libogg-1.3.5/COPYING"    "$D/Licences/Ogg,fff"
cp "$SRC/libvorbis-1.3.7/COPYING" "$D/Licences/Vorbis,fff"
cp "$SRC/SDL-release-2.26.0/LICENSE.txt" "$D/Licences/SDL2,fff"
# Corresponding source for the GPL: this port's changes and how it is built
# (the upstream tarballs are named, with checksums, in SOURCES).
( cd "$TOP" && tar cf - build patches tools app tests/qemu/*.sh tests/qemu/*.md tests/qemu/*.patch \
    README.md CHANGELOG.md docs Makefile 2>/dev/null ) | tar xf - -C "$D/source"
rm -f "$DIST/FFmpeg-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$DIST/FFmpeg-$V.zip" '!FFmpeg' )

# --- the devkit ----------------------------------------------------------
K=$TMP/riscos-ffmpeg-devkit-$V
mkdir -p "$K/lib/pkgconfig"
cp -r "$STAGE/include" "$K/"
for l in avcodec avdevice avfilter avformat avutil postproc swresample swscale \
         x264 dav1d mp3lame opus ogg vorbis vorbisenc vorbisfile; do
  ${CROSS}strip --strip-debug -o "$K/lib/lib$l.a" "$STAGE/lib/lib$l.a"
done
for pc in "$STAGE"/lib/pkgconfig/*.pc; do
  case $(basename "$pc") in sdl2.pc|zlib.pc) continue ;; esac   # from riscos-mesa
  sed "s#$STAGE#\${pcfiledir}/../..#g" "$pc" > "$K/lib/pkgconfig/$(basename "$pc")"
done
rm -rf "$K/include/SDL2" "$K/include/zlib.h" "$K/include/zconf.h"
cp -r "$D/Licences" "$K/Licences"
cat > "$K/README.txt" <<EOF
riscos-ffmpeg devkit $V: FFmpeg 5.1.10 static libraries for GCCSDK GCC 10
(arm-riscos-gnueabihf), ARMv7 + NEON, alignment-safe (see the riscos-ffmpeg
README). Also x264, dav1d, LAME, Opus, Ogg, Vorbis.
SDL2 and zlib are not here: take them from the riscos-mesa devkit.
Point PKG_CONFIG_LIBDIR at lib/pkgconfig and use pkg-config --static.
Licence: GPL version 2 or later (x264); see Licences.
EOF
rm -f "$DIST/riscos-ffmpeg-devkit-$V.tgz"
tar czf "$DIST/riscos-ffmpeg-devkit-$V.tgz" -C "$TMP" "riscos-ffmpeg-devkit-$V"
ls -la "$DIST"
