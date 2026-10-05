#!/bin/bash
# ffegl_texture() with riscos-mesa's real libEGL and libOSMesa (the devkit's
# RISC OS builds), linked into an arm-linux program with riscos_shim.c and
# run under the trapping qemu. Called by tests/host/run.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
S=$TOP/stage-linuxarm
DEVKIT=${DEVKIT:-$TOP/devkit/riscos-mesa-devkit-20.3.5-12}
O=$TOP/src-linuxarm/host-tests/mesa
mkdir -p "$O"

# The RISC OS libraries with their UnixLib-only symbols renamed to ro_*
# (riscos_shim.c): stdio streams, ctype table, errno, pthreads.
SYMS="__stdin __stdout __stderr __ctype errno"
for l in EGL OSMesa; do
  syms=$SYMS
  syms+=" $(arm-linux-gnueabihf-nm -u "$DEVKIT/lib/lib$l.a" 2>/dev/null | awk '$2 ~ /^pthread_/ {print $2}' | sort -u | tr '\n' ' ')"
  args=()
  for s in $syms; do args+=(--redefine-sym "$s=ro_$s"); done
  arm-linux-gnueabihf-objcopy "${args[@]}" "$DEVKIT/lib/lib$l.a" "$O/lib$l.a"
done

CC="arm-linux-gnueabihf-gcc -O1 -g -marm -mno-unaligned-access -DEGL_NO_X11"
$CC -I$HERE/../fake -c "$HERE/riscos_shim.c" -o "$O/riscos_shim.o"
$CC -DFAKE_SDL_ONLY -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$HERE/.. -c "$HERE/../fake_sdl_gl.c" -o "$O/fake_sdl.o"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore.o"
$CC -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl.o"
$CC -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE/.. -c "$HERE/mesa_eglimage_test.c" -o "$O/mesa_eglimage_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/mesa_eglimage_test" \
  "$O/mesa_eglimage_test.o" "$O/ffegl.o" "$O/reelcore.o" "$O/fake_sdl.o" "$O/riscos_shim.o" \
  "$O/libEGL.a" "$O/libOSMesa.a" "$DEVKIT/lib/libz.a" \
  -L$S/lib -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lx264 -lmp3lame \
  -lopus -lvorbisenc -lvorbis -logg /usr/arm-linux-gnueabihf/lib/libstdc++.so.6 -lm -lpthread \
  2>"$O/link.log" || { cat "$O/link.log"; exit 1; }

bad=0
for clip in h264_aac_322_182.mp4 h264_aac_640_360.mp4; do
  echo "== mesa_eglimage_test $clip ($(basename "$DEVKIT"))"
  "$TOP/tests/qemu/aligntrap.sh" "$O/mesa_eglimage_test" "$TOP/tests/qemu/samples/$clip" 2>&1 |
    grep -v "swscaler\|reelcore: \|ffegl: " || bad=1
done
exit $bad
