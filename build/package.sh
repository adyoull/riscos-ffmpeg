#!/bin/bash
# Release files, from a finished build (build-deps.sh + build-ffmpeg.sh):
#   dist/FFmpeg-VERSION.zip             !FFmpeg: ffmpeg, ffprobe, ffplay (AIF),
#                                       Bench, docs (licences, source)
#   dist/FFmpeg-EGL-examples-VERSION.zip  videowin, videocube (ffegl examples)
#   dist/riscos-ffmpeg-devkit-VERSION.tgz  static libraries (and libffegl) + headers + .pc
# Filetypes go in the zip's Acorn extra fields (tools/mkrozip.py), so SparkFS
# and RISC OS unzip give the files their real types.
# Usage: build/package.sh [VERSION]      (default 5.1.10-riscos4)
#        ELF2AIF=path/to/elf2aif        (host elf2aif; see tools/elf2aif)
set -euo pipefail
source "$(dirname "$0")/env.sh"
V=${1:-5.1.10-riscos4}
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
cp "$TOP/docs/EGL.md" "$D/EGL,fff"
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
( cd "$TOP" && tar cf - build patches tools app ffegl tests/qemu/*.sh tests/qemu/*.md tests/qemu/*.patch \
    README.md CHANGELOG.md docs Makefile 2>/dev/null ) | tar xf - -C "$D/source"
rm -f "$DIST/FFmpeg-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$DIST/FFmpeg-$V.zip" '!FFmpeg' )

# --- the ffegl examples ---------------------------------------------------
E=$TMP/EGLExamples
mkdir -p "$E/source"
for p in videowin videocube; do
  ${CROSS}strip -o "$TMP/$p.elf" "$STAGE/bin/$p"
  "$ELF2AIF" -e "$TMP/$p.elf" "$E/$p,ff8" >/dev/null
  cp "$TOP/ffegl/examples/$p.c" "$E/source/$p,fff"
done
cp "$TOP/docs/EGL.md" "$E/ReadMe,fff"
cp "$FF/COPYING.GPLv2" "$E/Licence,fff"
# (double-clicked: a plain directory's !Boot isn't run by the Filer). The
# commands start the programs as desktop tasks of their own (*WimpTask), so
# they work from a TaskWindow too; their messages go to a log in the scrap
# directory.
cat > "$E/SetUp,feb" <<'EOF'
| ffegl examples (riscos-ffmpeg): double-click to set up the videowin and
| videocube commands. Each needs about 44MB of application space.
Set FFmpegEGL$Dir <Obey$Dir>
Set Alias$videowin  WimpTask Obey <FFmpegEGL$Dir>.Task videowin %%*0 > <Wimp$ScrapDir>.videowin/log 2>&1
Set Alias$videocube WimpTask Obey <FFmpegEGL$Dir>.Task videocube %%*0 > <Wimp$ScrapDir>.videocube/log 2>&1
EOF
cat > "$E/Task,feb" <<'EOF'
| Runs one of the programs here (the first argument) with the space it needs.
WimpSlot -min 45056K -max 45056K
Run <Obey$Dir>.%*0
EOF
rm -f "$DIST/FFmpeg-EGL-examples-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$DIST/FFmpeg-EGL-examples-$V.zip" EGLExamples )

# --- the devkit ----------------------------------------------------------
K=$TMP/riscos-ffmpeg-devkit-$V
mkdir -p "$K/lib/pkgconfig"
cp -r "$STAGE/include" "$K/"
for l in avcodec avdevice avfilter avformat avutil postproc swresample swscale \
         x264 dav1d mp3lame opus ogg vorbis vorbisenc vorbisfile ffegl; do
  ${CROSS}strip --strip-debug -o "$K/lib/lib$l.a" "$STAGE/lib/lib$l.a"
done
for pc in "$STAGE"/lib/pkgconfig/*.pc; do
  case $(basename "$pc") in sdl2.pc|zlib.pc) continue ;; esac   # from riscos-mesa
  sed "s#$STAGE#\${pcfiledir}/../..#g" "$pc" > "$K/lib/pkgconfig/$(basename "$pc")"
done
# riscos-mesa's own headers (SDL2, zlib, EGL, GL) stay in its devkit
rm -rf "$K/include/SDL2" "$K/include/zlib.h" "$K/include/zconf.h" \
       "$K/include/EGL" "$K/include/KHR" "$K/include/GL" "$K/include/GLES" "$K/include/GLES2"
cp -r "$D/Licences" "$K/Licences"
cat > "$K/README.txt" <<EOF
riscos-ffmpeg devkit $V: FFmpeg 5.1.10 static libraries for GCCSDK GCC 10
(arm-riscos-gnueabihf), ARMv7 + NEON, alignment-safe (see the riscos-ffmpeg
README). Also x264, dav1d, LAME, Opus, Ogg, Vorbis, and ffegl (include/ffegl.h,
lib/libffegl.a: video into EGL surfaces and GL textures; see the EGL
document in !FFmpeg.docs).
SDL2, zlib, EGL and OpenGL are not here: take them from the riscos-mesa
devkit, 20.3.5-7pre12 or later (EGLImage textures for ffegl; libavdevice's
egl output device needs libEGL and libOSMesa).
Point PKG_CONFIG_LIBDIR at lib/pkgconfig and use pkg-config --static.
Licence: GPL version 2 or later (x264); see Licences.
EOF
rm -f "$DIST/riscos-ffmpeg-devkit-$V.tgz"
tar czf "$DIST/riscos-ffmpeg-devkit-$V.tgz" -C "$TMP" "riscos-ffmpeg-devkit-$V"
ls -la "$DIST"
