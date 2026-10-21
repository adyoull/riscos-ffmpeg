#!/usr/bin/env bash
# Instruction profiles of riscos-ffmpeg's code, run for arm-linux under QEMU:
# where the instructions go, per function. Not timings (memory and cache
# costs don't show), but the place to start looking, and a check that a
# change cut what it was meant to.
#
#   tools/profile/run.sh reel FILE MODE [prof_reel options]   reelcore playback
#   tools/profile/run.sh ffmpeg [ffmpeg options]              the ffmpeg command
#
# Needs: make test-build (FFmpeg and its libraries for arm-linux, in
# stage-linuxarm and src-linuxarm), and a qemu-arm built with plugins
# (QEMU_PLUGINS=1 tests/qemu/build-qemu.sh): QEMU (default
# ~/qemu-aligntrap/qemu-arm) and QEMU_INC (its include/qemu, for
# qemu-plugin.h). TOP_N=N lines (default 30); PROF_OUT: the raw profile
# (default src-linuxarm/profile/insprof.txt; give each its own to run several at once).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
S=$TOP/stage-linuxarm
F=$TOP/src-linuxarm/ffmpeg-5.1.10
O=$TOP/src-linuxarm/profile
QEMU=${QEMU:-$HOME/qemu-aligntrap/qemu-arm}
QEMU_INC=${QEMU_INC:-$HOME/qemu-aligntrap/qemu-8.2.2/include/qemu}
DEVKIT=${DEVKIT:-$TOP/devkit/riscos-mesa-devkit-$(awk '$1 == "riscos-mesa" {sub(/^v/, "", $2); print $2}' "$TOP/.riscos-deps")}
mkdir -p "$O"

# the plugin, and the profiled programs built as for RISC OS (-O3, the same
# CPU flags as build/env.sh)
[ "$O/libinsprof.so" -nt "$HERE/insprof.c" ] ||
  gcc -O2 -shared -fPIC $(pkg-config --cflags glib-2.0) -I"$QEMU_INC" -o "$O/libinsprof.so" "$HERE/insprof.c"
CC="arm-linux-gnueabihf-gcc -O3 -g -marm -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -mtune=cortex-a72
  -mno-unaligned-access -DEGL_NO_X11 -I$TOP/tests/host/fake -I$TOP/tests/host"
LIBS="-L$S/lib -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lx264 -lmp3lame
  -lopus -lvorbisenc -lvorbis -logg -lm -lpthread"

what=$1; shift
case $what in
reel)
  if [ ! "$O/prof_reel" -nt "$TOP/reelcore/reelcore.c" ] || [ ! "$O/prof_reel" -nt "$HERE/prof_reel.c" ]; then
    $CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore.o" 2>/dev/null
    $CC -I$DEVKIT/include -I$DEVKIT/include/SDL2 -c "$TOP/tests/host/fake_sdl_gl.c" -o "$O/fake_sdl_gl.o"
    $CC -c "$TOP/tests/host/fake_riscos.c" -o "$O/fake_riscos.o"
    $CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$HERE/prof_reel.c" -o "$O/prof_reel.o"
    arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/prof_reel" "$O/reelcore.o" \
      "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/prof_reel.o" $LIBS 2>/dev/null
  fi
  prog=$O/prof_reel ;;
ffmpeg)
  prog=$F/ffmpeg_g ;;
*)
  echo "usage: run.sh reel FILE MODE [options] | run.sh ffmpeg [options]" >&2; exit 2 ;;
esac
OUT=${PROF_OUT:-$O/insprof.txt}
QEMU_ARGS="-plugin $O/libinsprof.so,out=$OUT" QEMU=$QEMU "$TOP/tests/qemu/aligntrap.sh" "$prog" "$@" \
  2>&1 | { grep -v "^\[" || true; } | tail -3
python3 "$HERE/symsum.py" "$prog" "$OUT" "${TOP_N:-30}"
