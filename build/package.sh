#!/bin/bash
# Release files, from a finished build (build-deps.sh + build-ffmpeg.sh):
#   dist/FFmpeg-VERSION.zip             !FFmpeg: ffmpeg, ffprobe, ffplay (AIF),
#                                       Bench, docs (licences, source)
#   dist/FFmpeg-EGL-examples-VERSION.zip  videowin, videocube (reelcore + ffegl examples)
#   dist/Reel-REEL_VERSION.zip          !Reel and !ReelEGL, the video player
#   dist/riscos-ffmpeg-devkit-VERSION.tgz  static libraries (and libreelcore, libffegl) + headers + .pc
# Filetypes go in the zip's Acorn extra fields (tools/mkrozip.py), so SparkFS
# and RISC OS unzip give the files their real types.
# Usage: build/package.sh [VERSION]      (default 5.1.10-riscos18)
#        ELF2AIF=path/to/elf2aif        (host elf2aif; see tools/elf2aif)
set -euo pipefail
source "$(dirname "$0")/env.sh"
V=${1:-5.1.10-riscos18}
ELF2AIF=${ELF2AIF:-$TOP/tools/elf2aif/elf2aif}
[ -x "$ELF2AIF" ] || { echo "no elf2aif at $ELF2AIF (make -C tools/elf2aif GCCSDK_SRC=...)" >&2; exit 1; }
FF=$SRC/ffmpeg-5.1.10
DIST=$TOP/dist
mkdir -p "$DIST"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# every program must be linked with UnixLib 5.0.3.1 (the thread timer !Run loads PThreadTicker
# for, and files over 2GB)
echo "UnixLib in the programs:"
CROSS=$CROSS "$TOP/tools/check-unixlib.sh" "$FF/ffmpeg_g" "$FF/ffprobe_g" "$FF/ffplay_g" "$STAGE/bin/fffront" \
  "$STAGE/bin/reel" "$STAGE/bin/reelegl" "$STAGE/bin/videowin" "$STAGE/bin/videocube" || exit 1

# --- the application ---------------------------------------------------
A=$TMP/'!FFmpeg'
cp -r "$TOP/app/!FFmpeg" "$A"
python3 "$TOP/tools/mksprites.py" "$A/!Sprites,ff9"
for p in ffmpeg ffprobe ffplay; do
  ${CROSS}strip -o "$TMP/$p.elf" "$FF/${p}_g"
  "$ELF2AIF" -e "$TMP/$p.elf" "$A/$p,ff8" >/dev/null
done
${CROSS}strip -o "$TMP/fffront.elf" "$STAGE/bin/fffront"
"$ELF2AIF" -e "$TMP/fffront.elf" "$A/!RunImage,ff8" >/dev/null
# UnixLib's thread timer module (0.01, from 5.0.1), loaded by !Run (third_party/pthreadticker)
cp "$TOP/third_party/pthreadticker/PThrTicker" "$A/PThrTicker,ffa"
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
cp "$TOP/third_party/pthreadticker/Licence" "$D/Licences/PThreadTicker,fff"
cp "$TOP/third_party/dejavu/Licence" "$D/Licences/DejaVu,fff"
cp "$TOP/third_party/reelhwaccel/Licence" "$D/Licences/ReelHWAccel,fff"
# Corresponding source for the GPL: this port's changes and how it is built
# (the upstream tarballs are named, with checksums, in SOURCES).
( cd "$TOP" && tar cf - build patches tools app common third_party reelcore ffegl frontend player tests/qemu/*.sh tests/qemu/*.md tests/qemu/*.patch \
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
cp "$TOP/third_party/reelhwaccel/Licence" "$E/ReelHWAccel,fff"   # (vcdec, linked into both)
# (double-clicked: a plain directory's !Boot isn't run by the Filer). The
# commands start the programs as desktop tasks of their own (*WimpTask), so
# they work from a TaskWindow too; their messages go to a log in the scrap
# directory.
cat > "$E/SetUp,feb" <<'EOF'
| ffegl examples (riscos-ffmpeg): double-click to set up the videowin and
| videocube commands. Each needs about 44MB of application space.
Set FFmpegEGL$Dir <Obey$Dir>
RMEnsure PThreadTicker 0.01 IfThere System:Modules.PThrTicker Then RMLoad System:Modules.PThrTicker
RMEnsure PThreadTicker 0.01 IfThere <FFmpegEGL$Dir>.PThrTicker Then RMLoad <FFmpegEGL$Dir>.PThrTicker
RMEnsure SharedSound 1.07 IfThere System:Modules.SSound Then RMLoad System:Modules.SSound
RMEnsure StreamManager 0.03 IfThere System:Modules.StreamMan Then RMLoad System:Modules.StreamMan
RMEnsure SharedSoundBuffer 0.07 IfThere System:Modules.SSBuffer Then RMLoad System:Modules.SSBuffer
Set Alias$videowin  WimpTask Obey <FFmpegEGL$Dir>.Task videowin %%*0 > <Wimp$ScrapDir>.videowin/log 2>&1
Set Alias$videocube WimpTask Obey <FFmpegEGL$Dir>.Task videocube %%*0 > <Wimp$ScrapDir>.videocube/log 2>&1
EOF
cat > "$E/Task,feb" <<'EOF'
| Runs one of the programs here (the first argument) with the space it needs.
WimpSlot -min 45056K -max 45056K
Run <Obey$Dir>.%*0
EOF
cp "$TOP/third_party/pthreadticker/PThrTicker" "$E/PThrTicker,ffa"
rm -f "$DIST/FFmpeg-EGL-examples-$V.zip"
( cd "$TMP" && python3 "$TOP/tools/mkrozip.py" "$DIST/FFmpeg-EGL-examples-$V.zip" EGLExamples )

# --- !Reel and !ReelEGL, the video player (sprite / EGL drawing) ---------
RV=${REEL_VERSION:-0.1.25}
RT=$TMP/Reel
mkdir -p "$RT"
for app in Reel ReelEGL; do
  R=$RT/"!$app"
  lc=$(echo "$app" | tr A-Z a-z)
  cp -r "$TOP/app/!$app" "$R"
  # a test build (a version with a suffix, as 0.1.22-vc3): the log on (the
  # !Run line that turns it off commented out); a release: as in the repository
  case "$RV" in *-*)
    python3 - "$R/!Run,feb" "$app" <<'PY'
import sys
p, app = sys.argv[1], sys.argv[2]
s = open(p).read()
off = 'If "<%s$Log>"="" Then Set %s$Log off\n' % (app, app)
assert s.count(off) == 1, "!Run's log line"
open(p, "w").write(s.replace(off, "| " + off))
PY
  ;; esac
  python3 "$TOP/tools/mksprites.py" --$lc "$R/!Sprites,ff9"
  ${CROSS}strip -o "$TMP/$lc.elf" "$STAGE/bin/$lc"
  "$ELF2AIF" -e "$TMP/$lc.elf" "$R/!RunImage,ff8" >/dev/null
  cp "$TOP/third_party/pthreadticker/PThrTicker" "$R/PThrTicker,ffa"
  mkdir -p "$R/docs/source/c" "$R/docs/source/h"
  cp -r "$D/Licences" "$R/docs/Licences"
  cp "$TOP/player/reel.c"  "$R/docs/source/c/reel,fff"
  cp "$TOP/player/sources.c"  "$R/docs/source/c/sources,fff"
  cp "$TOP/player/sources.h"  "$R/docs/source/h/sources,fff"
  cp "$TOP/reelcore/reelcore.c"  "$R/docs/source/c/reelcore,fff"
  cp "$TOP/reelcore/reelcore.h"  "$R/docs/source/h/reelcore,fff"
  cp "$TOP/reelcore/panel_font.h"  "$R/docs/source/h/panel_font,fff"
  cp "$TOP/reelcore/sub_font.h"  "$R/docs/source/h/sub_font,fff"
  cp "$TOP/common/version.h"  "$R/docs/source/h/version,fff"
  cp "$TOP/common/proginfo.h"  "$R/docs/source/h/proginfo,fff"
  if [ "$app" = ReelEGL ]; then
    cp "$TOP/ffegl/ffegl.c"  "$R/docs/source/c/ffegl,fff"
    cp "$TOP/ffegl/ffegl.h"  "$R/docs/source/h/ffegl,fff"
  fi
  cp "$TOP/tools/mksprites.py" "$R/docs/source/mksprites_py,fff"
  mkdir -p "$R/docs/source/patches"               # FFmpeg changes newer than the FFmpeg package
  cp "$TOP"/patches/ffmpeg/0015-*.patch "$R/docs/source/patches/yadif-neon,fff"
  cp "$TOP"/patches/ffmpeg/0016-*.patch "$R/docs/source/patches/hevc-epel-neon,fff"
  cp "$TOP"/patches/ffmpeg/0017-*.patch "$R/docs/source/patches/sws-rgb-neon,fff"
  cp "$TOP"/patches/ffmpeg/0018-*.patch "$R/docs/source/patches/tls-acornssl,fff"
  cp "$TOP/build/build-apps.sh" "$R/docs/source/build-apps_sh,fff"
  cat > "$R/docs/source/ReadMe,fff" <<EOF
$app $RV's own source is here (player/reel.c, and sources.c, which finds
web addresses and yt-dlp's output in text; ReelEGL is the same built with
-DREEL_EGL), and the player core it runs on (reelcore; ReelEGL
also has ffegl, which puts reelcore's pictures into EGL). It is part of riscos-ffmpeg ($V), whose full
source (FFmpeg 5.1.10 plus the RISC OS patches and build scripts) is in
the FFmpeg package (!FFmpeg.docs.source) and the riscos-ffmpeg git
repository. Both are built by build/build-apps.sh.
EOF
done
cp "$DEVKIT/LICENCES.txt" "$RT/!ReelEGL/docs/Licences/riscos-mesa,fff"
rm -f "$DIST/Reel-$RV.zip"
( cd "$RT" && python3 "$TOP/tools/mkrozip.py" "$DIST/Reel-$RV.zip" '!Reel' '!ReelEGL' )

# --- the devkit ----------------------------------------------------------
K=$TMP/riscos-ffmpeg-devkit-$V
mkdir -p "$K/lib/pkgconfig"
cp -r "$STAGE/include" "$K/"
for l in avcodec avdevice avfilter avformat avutil postproc swresample swscale \
         x264 dav1d mp3lame opus ogg vorbis vorbisenc vorbisfile reelcore ffegl; do
  ${CROSS}strip --strip-debug -o "$K/lib/lib$l.a" "$STAGE/lib/lib$l.a"
done
[ -f "$STAGE/lib/libvcdec.a" ] && cp "$STAGE/lib/libvcdec.a" "$K/lib/"   # riscos-reelhwaccel's, for h264_vchiq
[ -f "$STAGE/lib/libhevcdec.a" ] && cp "$STAGE/lib/libhevcdec.a" "$K/lib/"   # and for hevc_hwdec
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
README). Also x264, dav1d, LAME, Opus, Ogg, Vorbis; reelcore
(include/reelcore.h, lib/libreelcore.a: the player core, no EGL); and
ffegl (include/ffegl.h, lib/libffegl.a: reelcore's pictures into EGL
surfaces and GL textures; see the EGL document in !FFmpeg.docs).
SDL2, zlib, EGL and OpenGL are not here: take them from the riscos-mesa
devkit, 20.3.5-7pre12 or later (EGLImage textures for ffegl; libavdevice's
egl output device needs libEGL and libOSMesa).
Point PKG_CONFIG_LIBDIR at lib/pkgconfig and use pkg-config --static.
libavcodec includes h264_vchiq (H.264 on the Raspberry Pi's VideoCore)
when lib/libvcdec.a is here, and hevc_hwdec (HEVC on the Pi 4's HEVC
block) when lib/libhevcdec.a is here (riscos-reelhwaccel's vcdec and
hevcdec, GPL version 2): link with -lvcdec -lhevcdec after -lavcodec
(pkg-config adds them). libreelcore.a is then built to convert the HEVC
block's frames itself when they're shown: link it with -lhevcdec too.
Licence: GPL version 2 or later (x264); see Licences.
EOF
rm -f "$DIST/riscos-ffmpeg-devkit-$V.tgz"
tar czf "$DIST/riscos-ffmpeg-devkit-$V.tgz" -C "$TMP" "riscos-ffmpeg-devkit-$V"
ls -la "$DIST"
