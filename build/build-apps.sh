#!/bin/bash
# The player core, its EGL layer, and the programs on them. Needs
# build-deps.sh (with egl) and build-ffmpeg.sh.
#   reelcore (reelcore/): the player core, no EGL
#     -> $STAGE/lib/libreelcore.a, $STAGE/include/reelcore.h
#   ffegl (ffegl/): reelcore's pictures into EGL surfaces and GL textures
#     -> $STAGE/lib/libffegl.a, $STAGE/include/ffegl.h
#   $STAGE/bin/reel      !Reel: reelcore only, no Mesa
#   $STAGE/bin/reelegl   !ReelEGL: reelcore + ffegl (surfaces only)
#   $STAGE/bin/videowin, videocube   the EGL examples: reelcore + ffegl
#   $STAGE/bin/fffront   !FFmpeg's icon bar front end (!RunImage)
set -euo pipefail
source "$(dirname "$0")/env.sh"
O=$SRC/apps
mkdir -p "$O" "$STAGE/bin"
CC="${CROSS}gcc $CFLAGS_RO -D_FILE_OFFSET_BITS=64 -Wall -I$TOP/reelcore -I$TOP/ffegl -I$STAGE/include -I$STAGE/include/SDL2"
VC=$([ -f "$STAGE/lib/libvcdec.a" ] && echo -lvcdec || true)   # h264_vchiq (build-ffmpeg.sh)
VC="$VC $([ -f "$STAGE/lib/libhevcdec.a" ] && echo -lhevcdec || true)"   # hevc_hwdec
FF="-lavfilter -lpostproc -lavformat -lavcodec $VC -lswresample -lswscale -lavutil \
  -ldav1d -lx264 -lmp3lame -lopus -lvorbisenc -lvorbis -logg -lz -lSDL2"

# the libraries
$CC -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore.o"
$CC -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl.o"
$CC -DFFEGL_NO_TEXTURE -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl_notex.o"
rm -f "$STAGE/lib/libreelcore.a" "$STAGE/lib/libffegl.a"
${CROSS}ar rcs "$STAGE/lib/libreelcore.a" "$O/reelcore.o"
${CROSS}ar rcs "$STAGE/lib/libffegl.a" "$O/ffegl.o"
cp "$TOP/reelcore/reelcore.h" "$TOP/ffegl/ffegl.h" "$STAGE/include/"

# the EGL examples
$CC -o "$STAGE/bin/videowin"  "$TOP/ffegl/examples/videowin.c"  -static \
  -L$STAGE/lib -lffegl -lreelcore $FF -lEGL -lOSMesa -lstdc++ -lm
$CC -o "$STAGE/bin/videocube" "$TOP/ffegl/examples/videocube.c" -static \
  -L$STAGE/lib -lffegl -lreelcore $FF -lglut -lGLU -lEGL -lOSMesa -lstdc++ -lm

# !Reel, the video player (player/reel.c): reelcore alone, no EGL, no Mesa
$CC -o "$STAGE/bin/reel" "$TOP/player/reel.c" "$TOP/player/sources.c" "$O/reelcore.o" -static \
  -L$STAGE/lib $FF -lm

# !ReelEGL: the same player drawing through riscos-mesa's EGL (ffegl's
# surfaces, no GL textures)
$CC -DREEL_EGL -o "$STAGE/bin/reelegl" "$TOP/player/reel.c" "$TOP/player/sources.c" "$O/ffegl_notex.o" "$O/reelcore.o" -static \
  -L$STAGE/lib $FF -lEGL -lOSMesa -lstdc++ -lm

# !FFmpeg's icon bar front end (frontend/fffront.c)
$CC -o "$STAGE/bin/fffront" "$TOP/frontend/fffront.c" "$TOP/frontend/convert.c" -static
ls -la "$STAGE/lib/libreelcore.a" "$STAGE/lib/libffegl.a" "$STAGE/bin/"
