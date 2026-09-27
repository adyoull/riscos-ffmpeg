#!/bin/bash
# Host tests of the RISC OS-only code, with fakes of the RISC OS SWIs, EGL,
# SDL audio and GL (fake_*.c): built for arm-linux against the
# LINUX_ARM_TEST=1 FFmpeg (stage-linuxarm) and run under the trapping qemu
# (tests/qemu/aligntrap.sh), so they also catch unaligned accesses.
#   egl_outdev_test: libavdevice/riscos_egl.c (the -f egl output device)
#   ffegl_test:      ffegl/ffegl.c (the library)
#   fffront_test:    frontend/fffront.c (!FFmpeg's icon bar front end), host gcc
#   mesa/run.sh:     ffegl_texture() with riscos-mesa's real libEGL/libOSMesa
# Usage: QEMU=path/to/patched/qemu-arm tests/host/run.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
S=$TOP/stage-linuxarm
F=$TOP/src-linuxarm/ffmpeg-5.1.10              # patched source (riscos_egl.c, internal headers)
DEVKIT=${DEVKIT:-$TOP/devkit/riscos-mesa-devkit-20.3.5-7pre12}
O=$TOP/src-linuxarm/host-tests
SAMPLES=$TOP/tests/qemu/samples
mkdir -p "$O" "$HERE/fake/EGL" "$HERE/fake/KHR"
# riscos-mesa's EGL headers (RISC OS additions and lock_surface)
cp "$DEVKIT"/include/EGL/*.h "$HERE/fake/EGL/"
cp "$DEVKIT"/include/KHR/*.h "$HERE/fake/KHR/"
CC="arm-linux-gnueabihf-gcc -O1 -g -marm -mno-unaligned-access -DEGL_NO_X11 -I$HERE/fake -I$HERE"
LIBS="-L$S/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lx264 -lmp3lame \
  -lopus -lvorbisenc -lvorbis -logg -lm -lpthread"

$CC -I$F -c "$F/libavdevice/riscos_egl.c" -o "$O/riscos_egl.o"
$CC -c "$HERE/fake_riscos.c" -o "$O/fake_riscos.o"
$CC -I$S/include -c "$HERE/egl_outdev_test.c" -o "$O/egl_outdev_test.o"
arm-linux-gnueabihf-gcc -no-pie -o "$O/egl_outdev_test" "$O/riscos_egl.o" "$O/fake_riscos.o" \
  "$O/egl_outdev_test.o" $LIBS 2>/dev/null

$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/ffegl -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl.o"
$CC -I$DEVKIT/include -I$DEVKIT/include/SDL2 -c "$HERE/fake_sdl_gl.c" -o "$O/fake_sdl_gl.o"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/ffegl -c "$HERE/ffegl_test.c" -o "$O/ffegl_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/ffegl_test" "$O/ffegl.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/ffegl_test.o" $LIBS 2>/dev/null

bad=0
for clip in h264_aac_640_360.mp4 h264_aac_322_182.mp4 vp9_640_360.webm hevc_322_182.mkv; do
  echo "== egl_outdev_test $clip"
  "$TOP/tests/qemu/aligntrap.sh" "$O/egl_outdev_test" "$SAMPLES/$clip" 2>&1 | grep -v "swscaler\|egl @" || bad=1
done
for clip in h264_aac_640_360.mp4 h264_aac_322_182.mp4; do
  echo "== ffegl_test $clip"
  "$TOP/tests/qemu/aligntrap.sh" "$O/ffegl_test" "$SAMPLES/$clip" 2>&1 | grep -v "swscaler\|ffegl: " || bad=1
done
# Reel (player/reel.c) with ffegl built without GL, fake Wimp, fake SDL audio
$CC -DFFEGL_NO_GL -I$S/include -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$TOP/ffegl -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl_nogl.o"
$CC -DREEL_TEST -DREEL_NO_MAIN -I$S/include -I$DEVKIT/include -I$TOP/ffegl -c "$TOP/player/reel.c" -o "$O/reel.o"
$CC -DFAKE_SDL_ONLY -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$HERE -c "$HERE/fake_sdl_gl.c" -o "$O/fake_sdl_only.o"
$CC -I$S/include -I$DEVKIT/include -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reel_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reel_test" "$O/reel_test.o" "$O/reel.o" \
  "$O/ffegl_nogl.o" "$O/fake_sdl_only.o" $LIBS 2>/dev/null
echo "== reel_test (the player, scripted desktop)"
"$TOP/tests/qemu/aligntrap.sh" "$O/reel_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|ffegl: " || bad=1

echo "== fffront_test (the icon bar front end, on this host)"
gcc -O1 -DFFFRONT_NO_MAIN -I"$HERE/fake" "$TOP/frontend/fffront.c" "$HERE/fffront_test.c" \
  -o "$O/fffront_test" 2>/dev/null && "$O/fffront_test" || bad=1
"$HERE/mesa/run.sh" || bad=1
exit $bad
