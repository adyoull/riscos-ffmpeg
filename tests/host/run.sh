#!/bin/bash
# Host tests of the RISC OS-only code, with fakes of the RISC OS SWIs, EGL,
# SDL audio and GL (fake_*.c): built for arm-linux against the
# LINUX_ARM_TEST=1 FFmpeg (stage-linuxarm) and run under the trapping qemu
# (tests/qemu/aligntrap.sh), so they also catch unaligned accesses.
#   egl_outdev_test: libavdevice/riscos_egl.c (the -f egl output device)
#   ffegl_test:      reelcore/reelcore.c (the player core) + ffegl/ffegl.c (its EGL layer)
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
LIBS="-L$S/lib -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lx264 -lmp3lame \
  -lopus -lvorbisenc -lvorbis -logg -lm -lpthread"

$CC -I$F -c "$F/libavdevice/riscos_egl.c" -o "$O/riscos_egl.o"
$CC -c "$HERE/fake_riscos.c" -o "$O/fake_riscos.o"
$CC -I$S/include -c "$HERE/egl_outdev_test.c" -o "$O/egl_outdev_test.o"
arm-linux-gnueabihf-gcc -no-pie -o "$O/egl_outdev_test" "$O/riscos_egl.o" "$O/fake_riscos.o" \
  "$O/egl_outdev_test.o" $LIBS 2>/dev/null

$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore.o"
$CC -I$S/include -I$TOP/reelcore -I$TOP/ffegl -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl.o"
$CC -I$DEVKIT/include -I$DEVKIT/include/SDL2 -c "$HERE/fake_sdl_gl.c" -o "$O/fake_sdl_gl.o"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$TOP/ffegl -c "$HERE/ffegl_test.c" -o "$O/ffegl_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/ffegl_test" "$O/reelcore.o" "$O/ffegl.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/ffegl_test.o" $LIBS 2>/dev/null

bad=0
for clip in h264_aac_640_360.mp4 h264_aac_322_182.mp4 vp9_640_360.webm hevc_322_182.mkv; do
  echo "== egl_outdev_test $clip"
  "$TOP/tests/qemu/aligntrap.sh" "$O/egl_outdev_test" "$SAMPLES/$clip" 2>&1 | grep -v "swscaler\|egl @" || bad=1
done
for clip in h264_aac_640_360.mp4 h264_aac_322_182.mp4; do
  echo "== ffegl_test $clip"
  "$TOP/tests/qemu/aligntrap.sh" "$O/ffegl_test" "$SAMPLES/$clip" 2>&1 | grep -v "swscaler\|reelcore: \|ffegl: " || bad=1
done
# ffegl on a CPU too slow for the video: skipping frames keeps it with the sound
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/slow_test.c" -o "$O/slow_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -Wl,--wrap=avcodec_send_packet -o "$O/slow_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/slow_test.o" $LIBS 2>/dev/null
echo "== slow_test (decoding slower than real time)"
"$TOP/tests/qemu/aligntrap.sh" "$O/slow_test" "$SAMPLES/long_h264_aac_322_184.mp4" 2>&1 | grep -v "swscaler" || bad=1

# playback options: speed, fast decoding, sound tracks, picture modes
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/options_test.c" -o "$O/options_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/options_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/options_test.o" $LIBS -lm 2>/dev/null
echo "== options_test (speed, fast decoding, sound tracks, picture modes)"
"$TOP/tests/qemu/aligntrap.sh" "$O/options_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/twoaudio_h264_aac_322_184.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: " || bad=1

# yadif's NEON line filter (FFmpeg patch 0015) against its C, bit for bit
$CC -c "$F/libavfilter/arm/vf_yadif_neon.S" -I$F -o "$O/vf_yadif_neon.o"
$CC -O2 -c "$HERE/yadif_test.c" -o "$O/yadif_test.o"
arm-linux-gnueabihf-gcc -no-pie -o "$O/yadif_test" "$O/yadif_test.o" "$O/vf_yadif_neon.o"
echo "== yadif_test (NEON line filter against C)"
"$TOP/tests/qemu/aligntrap.sh" "$O/yadif_test" || bad=1
# ... and whole pictures: ffmpeg -vf yadif with NEON, without (-cpuflags 0), and x86 FFmpeg
echo "== yadif, whole pictures: NEON, C and x86 FFmpeg"
ffmpeg -v error -y -i "$SAMPLES/mv_mpeg2_il.mpg" -f rawvideo -pix_fmt yuv420p "$O/il.yuv"
for vf in yadif=0:0:0 yadif=1:1:0 yadif=2:0:0 yadif=3:1:0; do
  sums=""
  for cpu in "" "-cpuflags 0"; do
    sums="$sums $("$TOP/tests/qemu/aligntrap.sh" "$F/ffmpeg_g" -v error $cpu -f rawvideo -pix_fmt yuv420p -s 322x184 \
      -i "$O/il.yuv" -vf $vf -f rawvideo - | md5sum | cut -c1-32)"
  done
  sums="$sums $(ffmpeg -v error -f rawvideo -pix_fmt yuv420p -s 322x184 -i "$O/il.yuv" -vf $vf -f rawvideo - | md5sum | cut -c1-32)"
  set -- $sums
  if [ "$1" = "$2" ] && [ "$1" = "$3" ]; then echo "  $vf: the same ($1)"; else echo "FAIL: $vf: $sums"; bad=1; fi
done

# HEVC chroma motion compensation in NEON (patch 0016) against the C, and
# whole HEVC decodes: NEON, without (-cpuflags 0) and x86 FFmpeg, picture by picture
$CC -O2 -I$F -c "$HERE/hevc_epel_test.c" -o "$O/hevc_epel_test.o"
arm-linux-gnueabihf-gcc -no-pie -o "$O/hevc_epel_test" "$O/hevc_epel_test.o" -L$S/lib -lavcodec -lavutil -lm -lpthread
echo "== hevc_epel_test (HEVC chroma MC: NEON against C)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevc_epel_test" || bad=1
echo "== HEVC decodes: NEON, C and x86 FFmpeg"
for clip in mv_hevc.mkv hevc_322_182.mkv hevc_640_360.mkv; do
  pics() { grep -v "^#" | awk -F, '{print $NF}' | md5sum | cut -c1-32; }
  a=$("$TOP/tests/qemu/aligntrap.sh" "$F/ffmpeg_g" -v error -i "$SAMPLES/$clip" -f framemd5 - | pics)
  b=$("$TOP/tests/qemu/aligntrap.sh" "$F/ffmpeg_g" -v error -cpuflags 0 -i "$SAMPLES/$clip" -f framemd5 - | pics)
  c=$(ffmpeg -v error -i "$SAMPLES/$clip" -f framemd5 - | pics)
  if [ "$a" = "$b" ] && [ "$a" = "$c" ]; then echo "  $clip: the same ($a)"; else echo "FAIL: $clip: $a $b $c"; bad=1; fi
done

# scaling to RGB32 in NEON (patch 0017) against the C
$CC -O2 -I$F -c "$HERE/swscale_rgb_test.c" -o "$O/swscale_rgb_test.o"
arm-linux-gnueabihf-gcc -no-pie -o "$O/swscale_rgb_test" "$O/swscale_rgb_test.o" -L$S/lib -lswscale -lavutil -lm -lpthread
echo "== swscale_rgb_test (fast bilinear to RGBA/BGRA: NEON against C)"
"$TOP/tests/qemu/aligntrap.sh" "$O/swscale_rgb_test" || bad=1

# reelcore's deinterlacing (Auto/On/Off) against x86 FFmpeg's yadif
ffmpeg -v error -y -i "$SAMPLES/mv_h264_mbaff.mkv" -vf yadif=0:-1:1 -f rawvideo -pix_fmt yuv420p "$O/mbaff_yadif.yuv"
ffmpeg -v error -y -i "$SAMPLES/mv_h264_mbaff.mkv" -f rawvideo -pix_fmt yuv420p "$O/mbaff_plain.yuv"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/deint_test.c" -o "$O/deint_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/deint_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/deint_test.o" $LIBS -lm 2>/dev/null
echo "== deint_test (reelcore: deinterlacing Auto, On, Off)"
"$TOP/tests/qemu/aligntrap.sh" "$O/deint_test" "$SAMPLES/mv_h264_mbaff.mkv" "$O/mbaff_yadif.yuv" "$O/mbaff_plain.yuv" \
  "$SAMPLES/long_h264_aac_322_184.mp4" 2>&1 | grep -v "swscaler\|reelcore: " || bad=1

# sleeping between pictures (ffegl_idle_time) against polling flat out
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/idle_test.c" -o "$O/idle_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -Wl,--wrap=avcodec_send_packet -o "$O/idle_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/idle_test.o" $LIBS -lm 2>/dev/null
echo "== idle_test (sleeping until the next picture is due)"
"$TOP/tests/qemu/aligntrap.sh" "$O/idle_test" "$SAMPLES/long_h264_aac_322_184.mp4" 2>&1 | grep -v "swscaler\|reelcore: \|ffegl: " || bad=1

# Reel (player/reel.c) on reelcore alone (no EGL), fake Wimp, fake SDL audio
$CC -DREEL_TEST -DREEL_NO_MAIN -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -c "$TOP/player/reel.c" -o "$O/reel.o"
$CC -DFAKE_SDL_ONLY -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$HERE -c "$HERE/fake_sdl_gl.c" -o "$O/fake_sdl_only.o"
$CC -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reel_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reel_test" "$O/reel_test.o" "$O/reel.o" \
  "$O/reelcore.o" "$O/fake_sdl_only.o" $LIBS 2>/dev/null
echo "== reel_test (the player, scripted desktop)"
"$TOP/tests/qemu/aligntrap.sh" "$O/reel_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1

# Reel again with the sound going straight to (fake) SharedSoundBuffer, as on RISC OS
$CC -DREELCORE_SSB -I$HERE/fake -I$S/include -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore_ssb.o"
$CC -DFAKE_SSB -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reel_ssb_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reel_ssb_test" "$O/reel_ssb_test.o" "$O/reel.o" \
  "$O/reelcore_ssb.o" "$O/fake_sdl_only.o" $LIBS 2>/dev/null
echo "== reel_ssb_test (the player, sound through SharedSoundBuffer)"
rm -f "$O/reel_ssb.log"
env 'Reel$Log'="$O/reel_ssb.log" "$TOP/tests/qemu/aligntrap.sh" "$O/reel_ssb_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1
# the log (Reel$Log): what was opened, the sound stream, the once-a-second lines, quit
for want in "Reel log" "module SharedSoundBuffer" "open .*long_h264_aac_322_184" "playing: h264" \
            "SharedSoundBuffer stream" "sound starts" "SSB playing" "nulls, .* pictures in" "ms a picture: decode .*convert" "full screen on" \
            "report: That's a directory" "close: pos" "quit"; do
  grep -q "$want" "$O/reel_ssb.log" || { echo "  the log has no \"$want\""; bad=1; }
done
echo "  log: $(wc -l < "$O/reel_ssb.log") lines"
echo "== reel_ssb_test, video read 1 s ahead of the sound (the trailer's freeze)"
"$TOP/tests/qemu/aligntrap.sh" "$O/reel_ssb_test" "$SAMPLES/chunky_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1

# ReelEGL: the same player drawing through (fake) EGL surfaces
$CC -DFFEGL_NO_TEXTURE -I$HERE/fake -I$S/include -I$TOP/reelcore -I$TOP/ffegl -c "$TOP/ffegl/ffegl.c" -o "$O/ffegl_notex.o"
$CC -DREEL_EGL -DREEL_TEST -DREEL_NO_MAIN -I$HERE/fake -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -c "$TOP/player/reel.c" -o "$O/reelegl.o"
$CC -DFAKE_EGL_ONLY -I$HERE/fake -c "$HERE/fake_riscos.c" -o "$O/fake_egl_only.o"
$CC -DREEL_EGL -I$HERE/fake -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reelegl_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reelegl_test" "$O/reelegl_test.o" "$O/reelegl.o" \
  "$O/reelcore.o" "$O/ffegl_notex.o" "$O/fake_sdl_only.o" "$O/fake_egl_only.o" $LIBS 2>/dev/null
echo "== reelegl_test (the EGL build of the player, scripted desktop, fake EGL)"
rm -f "$O/reelegl.log"
env 'ReelEGL$Log'="$O/reelegl.log" "$TOP/tests/qemu/aligntrap.sh" "$O/reelegl_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1
for want in "ReelEGL log" "EGL surface .*work area" "EGL surface .*screen" "SDL audio driver"; do
  grep -q "$want" "$O/reelegl.log" || { echo "  the log has no \"$want\""; bad=1; }
done

echo "== fffront_test (the icon bar front end, on this host)"
gcc -O1 -DFFFRONT_NO_MAIN -I"$HERE/fake" "$TOP/frontend/fffront.c" "$HERE/fffront_test.c" \
  -o "$O/fffront_test" 2>/dev/null && "$O/fffront_test" || bad=1
"$HERE/mesa/run.sh" || bad=1
exit $bad
