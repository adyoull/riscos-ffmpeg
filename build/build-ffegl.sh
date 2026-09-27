#!/bin/bash
# ffegl: the library for EGL/OpenGL programs (ffegl/), and its examples
# videowin and videocube. Needs build-deps.sh (with egl) and build-ffmpeg.sh.
#   -> $STAGE/lib/libffegl.a, $STAGE/include/ffegl.h, $STAGE/bin/{videowin,videocube},
#      $STAGE/bin/fffront (!FFmpeg's icon bar front end, !RunImage)
#      $STAGE/bin/reel (!Reel, the video player) and $STAGE/bin/reelegl (!ReelEGL)
set -euo pipefail
source "$(dirname "$0")/env.sh"
O=$SRC/ffegl
mkdir -p "$O" "$STAGE/bin"
CC="${CROSS}gcc $CFLAGS_RO -Wall -I$STAGE/include -I$STAGE/include/SDL2 -I$TOP/ffegl"

$CC -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl.o"
rm -f "$STAGE/lib/libffegl.a"
${CROSS}ar rcs "$STAGE/lib/libffegl.a" "$O/ffegl.o"
cp "$TOP/ffegl/ffegl.h" "$STAGE/include/"

LIBS="-L$STAGE/lib -lffegl -lavformat -lavcodec -lswresample -lswscale -lavutil \
  -ldav1d -lx264 -lmp3lame -lopus -lvorbisenc -lvorbis -logg -lz -lSDL2"
GL="-lglut -lGLU -lEGL -lOSMesa -lstdc++"
$CC -o "$STAGE/bin/videowin"  "$TOP/ffegl/examples/videowin.c"  -static $LIBS -lEGL -lOSMesa -lstdc++ -lm
$CC -o "$STAGE/bin/videocube" "$TOP/ffegl/examples/videocube.c" -static $LIBS $GL -lm
# !Reel, the video player (player/reel.c): ffegl without EGL/GL, no Mesa
${CROSS}gcc $CFLAGS_RO -Wall -DFFEGL_NO_GL -I$TOP/ffegl -I$STAGE/include -I$STAGE/include/SDL2 \
  -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl_nogl.o"
${CROSS}gcc $CFLAGS_RO -Wall -I$TOP/ffegl -I$STAGE/include -I$STAGE/include/SDL2 \
  -o "$STAGE/bin/reel" "$TOP/player/reel.c" "$O/ffegl_nogl.o" -static \
  -L$STAGE/lib -lavformat -lavcodec -lswresample -lswscale -lavutil \
  -ldav1d -lx264 -lmp3lame -lopus -lvorbisenc -lvorbis -logg -lz -lSDL2 -lm

# !ReelEGL: the same player drawing through riscos-mesa's EGL (an example
# of ffegl_draw_surface in a real program); ffegl without GL textures
${CROSS}gcc $CFLAGS_RO -Wall -DFFEGL_NO_TEXTURE -I$TOP/ffegl -I$STAGE/include -I$STAGE/include/SDL2 \
  -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl_notex.o"
${CROSS}gcc $CFLAGS_RO -Wall -DREEL_EGL -I$TOP/ffegl -I$STAGE/include -I$STAGE/include/SDL2 \
  -o "$STAGE/bin/reelegl" "$TOP/player/reel.c" "$O/ffegl_notex.o" -static \
  -L$STAGE/lib -lavformat -lavcodec -lswresample -lswscale -lavutil \
  -ldav1d -lx264 -lmp3lame -lopus -lvorbisenc -lvorbis -logg -lz -lSDL2 -lEGL -lOSMesa -lstdc++ -lm

# !FFmpeg's icon bar front end (frontend/fffront.c)
$CC -o "$STAGE/bin/fffront" "$TOP/frontend/fffront.c" -static
ls -la "$STAGE/lib/libffegl.a" "$STAGE/bin/"
