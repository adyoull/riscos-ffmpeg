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
CC="arm-linux-gnueabihf-gcc -O1 -g -marm -mno-unaligned-access -mfpu=neon-vfpv3 -DEGL_NO_X11 -I$HERE/fake -I$HERE"
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
echo "== versions agree (common/version.h, Makefile, package.sh, build-ffmpeg.sh, !Help)"
"$TOP/tools/check-versions.sh" || bad=1
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
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=12 -f lavfi -i sine=d=12:sample_rate=44100 \
  -c:v libx264 -preset ultrafast -g 25 -c:a aac "$O/gop1s.mp4"
"$TOP/tests/qemu/aligntrap.sh" "$O/slow_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$O/gop1s.mp4" 2>&1 | grep -v "swscaler" || bad=1

# playback options: speed, fast decoding, sound tracks, picture modes
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/options_test.c" -o "$O/options_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/options_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/options_test.o" $LIBS -lm 2>/dev/null
echo "== options_test (speed, fast decoding, sound tracks, picture modes)"
"$TOP/tests/qemu/aligntrap.sh" "$O/options_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/twoaudio_h264_aac_322_184.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: " || bad=1
# halving for big reductions (the mini player): NEON = C byte for byte, and the picture it makes
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/halve_test.c" -o "$O/halve_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/halve_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/halve_test.o" $LIBS -lm 2>/dev/null
echo "== halve_test (big reductions halved first: NEON vs C, and the picture)"
"$TOP/tests/qemu/aligntrap.sh" "$O/halve_test" "$SAMPLES/h264_aac_640_360.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: " || bad=1

# what Reel finds in text: addresses, playlists, yt-dlp -g and -j output (host gcc, sanitizers)
echo "== sources_test (web addresses and yt-dlp's output in text)"
gcc -O1 -g -fsanitize=address,undefined -I"$TOP/player" "$TOP/player/sources.c" "$HERE/sources_test.c" -o "$O/sources_test" &&
  "$O/sources_test" || bad=1

# the sound clock in StreamManager's 2048-frame steps, a 30 fps video with 44.1 kHz sound
ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=30:duration=7 \
  -f lavfi -i sine=frequency=440:duration=7:sample_rate=44100 -c:v libx264 -preset ultrafast -c:a aac -ar 44100 "$O/c30_44k.mp4"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=60:duration=7 \
  -f lavfi -i sine=frequency=440:duration=7:sample_rate=44100 -c:v libx264 -preset ultrafast -c:a aac -ar 44100 "$O/c60_44k.mp4"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/clock_test.c" -o "$O/clock_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -Wl,--wrap=avcodec_send_packet -o "$O/clock_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/clock_test.o" $LIBS -lm 2>/dev/null
echo "== clock_test (30 and 60 fps against a sound clock moving in 46 ms steps)"
"$TOP/tests/qemu/aligntrap.sh" "$O/clock_test" "$O/c30_44k.mp4" "$O/c60_44k.mp4" 2>&1 | grep -v "reelcore: " || bad=1

# the stats panel drawn into the picture (RGB and YV12)
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/panel_test.c" -o "$O/panel_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/panel_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/panel_test.o" $LIBS -lm 2>/dev/null
ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25:duration=1 -c:v libx264 -preset ultrafast -pix_fmt yuv444p "$O/c444.mp4"
echo "== panel_test (stats drawn into the picture)"
"$TOP/tests/qemu/aligntrap.sh" "$O/panel_test" "$O/c30_44k.mp4" "$O/c444.mp4" 2>&1 | grep -v "reelcore: " || bad=1

# subtitles, chapters, turned pictures, frame steps
printf '1\n00:00:01,000 --> 00:00:02,500\nHello <i>there</i>\n\n2\n00:00:03,000 --> 00:00:04,000\n\xe2\x80\x9cQuoted\xe2\x80\x9d \xe2\x80\x94 it\xe2\x80\x99s caf\xc3\xa9\nsecond line\n\n' > "$O/subs.srt"
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=Opening\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=4000\ntitle=Middle\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=4000\nEND=5000\n' > "$O/chapters.txt"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=5 -f lavfi -i sine=d=5:sample_rate=44100 \
  -i "$O/subs.srt" -i "$O/chapters.txt" -map 0 -map 1 -map 2 -map_metadata 3 -map_chapters 3 -c:v libx264 \
  -preset ultrafast -c:a aac -c:s srt -disposition:s:0 default -metadata:s:s:0 language=eng "$O/subs.mkv"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=5 -f lavfi -i sine=d=5:sample_rate=44100 \
  -c:v libx264 -preset ultrafast -c:a aac "$O/plain.mp4"
ffmpeg -v error -y -display_rotation 90 -i "$O/plain.mp4" -c copy "$O/turned.mp4"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=10 -c:v libx264 -preset veryfast -bf 3 \
  -g 250 -keyint_min 250 -sc_threshold 0 -an "$O/gop.mp4"
printf '[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 180\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\b1}Bold{\\b0} words,\\Nwith a comma\\hand space\n' > "$O/subs.ass"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/sub_test.c" -o "$O/sub_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/sub_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/sub_test.o" $LIBS -lm 2>/dev/null
echo "== sub_test (subtitles, chapters, turned pictures, frame steps)"
"$TOP/tests/qemu/aligntrap.sh" "$O/sub_test" "$O/subs.mkv" "$O/plain.mp4" "$O/subs.srt" "$O/turned.mp4" "$O/subs.ass" "$O/gop.mp4" 2>&1 | grep -v "reelcore: " || bad=1

# RISC OS names typed relative to the current directory (FFmpeg patch 0019): holiday/mp4
# tools/hevcprobe: the Pi 4 HEVC block probe, against a fake firmware and fake registers
arm-linux-gnueabihf-gcc -O1 -DPROBE_TEST -I$HERE/fake -Wall -no-pie -o "$O/hevcprobe_test" "$TOP/tools/hevcprobe/hevcprobe.c" "$HERE/hevcprobe_test.c"
echo "== hevcprobe_test (the Pi 4 HEVC block probe: the clock on only around reads, put back)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevcprobe_test" | grep -v "^  &\|^hevcprobe:\|^$" || bad=1

# hwhevc/module: the HEVCHW module (its C, and header.s's veneers and IRQ handler) on a fake RISC OS
arm-linux-gnueabihf-gcc -c -o "$O/hevchw_header.o" "$TOP/hwhevc/module/header.s" &&
  arm-linux-gnueabihf-objcopy --weaken-symbol=hw_swi "$O/hevchw_header.o" &&
  arm-linux-gnueabihf-gcc -O1 -marm -DHW_TEST -Wall -no-pie -o "$O/hevchw_test" "$TOP/hwhevc/module/hevchw.c" \
    "$HERE/hevchw_test.c" "$O/hevchw_header.o"
echo "== hevchw_test (the HEVCHW module: maps, register test, the interrupt found then claimed, memory)"
"$TOP/tests/qemu/aligntrap.sh" "$O/hevchw_test" | grep -v "^  &\|^$" || bad=1
"$TOP/hwhevc/module/build.sh" "$O/hevchw" | tail -1 || bad=1

echo "== riscos_name_test (holiday/mp4 as a RISC OS name, Unix names untouched)"
gcc -O1 -g -fsanitize=address,undefined -I"$F/libavformat" "$HERE/riscos_name_test.c" -o "$O/riscos_name_test" &&
  "$O/riscos_name_test" || bad=1

# reelcore from the network (its reader thread) and from two inputs (video and sound apart)
NS=$O/netsamples; mkdir -p "$NS"
cp "$SAMPLES/long_h264_aac_322_184.mp4" "$NS/"
ffmpeg -v error -y -i "$NS/long_h264_aac_322_184.mp4" -map 0:v -c copy "$NS/net_video_only.mp4"
ffmpeg -v error -y -i "$NS/long_h264_aac_322_184.mp4" -map 0:a -c copy "$NS/net_sound_only.m4a"
NP=$((20000 + RANDOM % 20000)); : > "$O/net_headers.log"
python3 "$HERE/httpserve.py" "$NP" "$NS" "$O/net_headers.log" >/dev/null 2>&1 & NS1=$!
python3 "$HERE/httpserve.py" $((NP + 1)) --silent >/dev/null 2>&1 & NS2=$!
for i in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$NP/" && break; sleep 0.1; done   # (up)
: > "$O/net_headers.log"
$CC -I$S/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -I$HERE -c "$HERE/net_test.c" -o "$O/net_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/net_test" "$O/reelcore.o" \
  "$O/fake_sdl_gl.o" "$O/fake_riscos.o" "$O/net_test.o" $LIBS -lm 2>/dev/null
echo "== net_test (reelcore: addresses read by a thread; video and sound apart)"
"$TOP/tests/qemu/aligntrap.sh" "$O/net_test" "http://127.0.0.1:$NP" "http://127.0.0.1:$((NP + 1))/x.mp4" "$NS" \
  "$O/net_headers.log" 2>&1 | grep -v "swscaler\|reelcore: " || bad=1
echo '<!DOCTYPE html><html><head><title>A video</title></head><body><p>A page about a video.</p></body></html>' > "$NS/page.html"
export REEL_TEST_URL="http://127.0.0.1:$NP"    # Reel's Open address and yt-dlp output (reel_test)

# https through AcornSSL (patch 0018): a stand-in module in the arm-linux build
echo "== https (FFmpeg's AcornSSL backend, with a pass-through stand-in for the module)"
bash "$HERE/https.sh" || bad=1

# the patches reproduce the tested code, and the port's NEON is alignment-safe as written
echo "== check-fresh-tree (patches/ffmpeg on a fresh FFmpeg = the tested tree)"
"$TOP/tools/check-fresh-tree.sh" "$F" || bad=1

# yadif's NEON line filter (FFmpeg patch 0015) against its C, bit for bit
$CC -mfpu=neon -c "$F/libavfilter/arm/vf_yadif_neon.S" -I$F -o "$O/vf_yadif_neon.o"
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
$CC -c "$TOP/player/sources.c" -o "$O/sources.o"
$CC -DFAKE_SDL_ONLY -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$HERE -c "$HERE/fake_sdl_gl.c" -o "$O/fake_sdl_only.o"
$CC -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reel_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reel_test" "$O/reel_test.o" "$O/reel.o" "$O/sources.o" \
  "$O/reelcore.o" "$O/fake_sdl_only.o" $LIBS 2>/dev/null
echo "== reel_test (the player, scripted desktop)"
"$TOP/tests/qemu/aligntrap.sh" "$O/reel_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" "$SAMPLES/twoaudio_h264_aac_322_184.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1

# Reel again with the sound going straight to (fake) SharedSoundBuffer, as on RISC OS
$CC -DREELCORE_SSB -I$HERE/fake -I$S/include -I$DEVKIT/include -I$DEVKIT/include/SDL2 -I$TOP/reelcore -c "$TOP/reelcore/reelcore.c" -o "$O/reelcore_ssb.o"
$CC -DFAKE_SSB -I$S/include -I$DEVKIT/include -I$TOP/reelcore -I$TOP/ffegl -I$HERE -c "$HERE/reel_test.c" -o "$O/reel_ssb_test.o"
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reel_ssb_test" "$O/reel_ssb_test.o" "$O/reel.o" "$O/sources.o" \
  "$O/reelcore_ssb.o" "$O/fake_sdl_only.o" $LIBS 2>/dev/null
echo "== reel_ssb_test (the player, sound through SharedSoundBuffer)"
rm -f "$O/reel_ssb.log"
env 'Reel$Log'="$O/reel_ssb.log" "$TOP/tests/qemu/aligntrap.sh" "$O/reel_ssb_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" "$SAMPLES/twoaudio_h264_aac_322_184.mp4" 2>&1 |
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
arm-linux-gnueabihf-gcc -no-pie -Wl,--wrap=av_gettime_relative -o "$O/reelegl_test" "$O/reelegl_test.o" "$O/reelegl.o" "$O/sources.o" \
  "$O/reelcore.o" "$O/ffegl_notex.o" "$O/fake_sdl_only.o" "$O/fake_egl_only.o" $LIBS 2>/dev/null
echo "== reelegl_test (the EGL build of the player, scripted desktop, fake EGL)"
rm -f "$O/reelegl.log"
env 'ReelEGL$Log'="$O/reelegl.log" "$TOP/tests/qemu/aligntrap.sh" "$O/reelegl_test" "$SAMPLES/long_h264_aac_322_184.mp4" "$SAMPLES/h264_aac_640_360.mp4" "$SAMPLES/twoaudio_h264_aac_322_184.mp4" 2>&1 |
  grep -v "swscaler\|reelcore: \|ffegl: " || bad=1
for want in "ReelEGL log" "EGL surface .*work area" "EGL surface .*screen" "SDL audio driver"; do
  grep -q "$want" "$O/reelegl.log" || { echo "  the log has no \"$want\""; bad=1; }
done
kill $NS1 $NS2 2>/dev/null
unset REEL_TEST_URL

# The Convert window's ffmpeg command lines, built from real ffprobe output (its own
# query) and run by the ARM ffmpeg 5.1; the results checked with ffprobe
echo "== convert commands (the Convert window's presets through ffmpeg 5.1)"
gcc -O1 -I"$HERE/fake" -I"$TOP/frontend" "$HERE/convert_cmds.c" "$TOP/frontend/convert.c" -o "$O/convert_cmds" 2>/dev/null
CV=$O/convert; mkdir -p "$CV"
ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=25:duration=4 -f lavfi -i sine=frequency=440:duration=4 \
  -c:v libx264 -crf 30 -preset ultrafast -c:a aac "$CV/hd.mp4"
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25:duration=2 -vf scale=321:181 -pix_fmt yuv444p -c:v ffv1 "$CV/odd.mkv"
convert_case() {    # name src out want-video want-height want-audio want-duration settings...
  local name=$1 src=$2 out=$3 wv=$4 wh=$5 wa=$6 wd=$7; shift 7
  ffprobe -v error -of compact=p=0 -show_entries \
    format=duration:stream=codec_type,codec_name,width,height,r_frame_rate,field_order,channels,sample_rate "$src" > "$CV/probe.txt"
  local args; args=$("$O/convert_cmds" "$CV/probe.txt" "$src" "$CV/$out" "$@" 2>"$CV/desc.txt") ||
    { echo "FAIL: $name: $(cat "$CV/desc.txt")"; bad=1; return; }
  rm -f "$CV/$out"
  if ! "$TOP/tests/qemu/aligntrap.sh" "$F/ffmpeg_g" $args > "$CV/progress.txt" 2> "$CV/err.txt"; then
    echo "FAIL: $name: ffmpeg: $(head -3 "$CV/err.txt")"; bad=1; return
  fi
  grep -q "^out_time_us=" "$CV/progress.txt" && grep -q "^progress=end" "$CV/progress.txt" ||
    { echo "FAIL: $name: no -progress lines"; bad=1; }
  local v h a d
  v=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$CV/$out")
  h=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$CV/$out")
  a=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$CV/$out")
  d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$CV/$out")
  if [ "$v" = "$wv" ] && [ "$h" = "$wh" ] && [ "$a" = "$wa" ] && awk -v d="$d" -v w="$wd" 'BEGIN { exit !(d > w - 0.3 && d < w + 0.3) }'; then
    echo "  $name: $out: ${v:-no video}${h:+ ${h} lines}, ${a:-no sound}, ${d%????} s  ($(sed 's/^  //' "$CV/desc.txt"))"
  else
    echo "FAIL: $name: $out: video '$v' $h lines, sound '$a', $d s (want '$wv' $wh, '$wa', $wd s)"; bad=1
  fi
}
convert_case "for playing here"   "$CV/hd.mp4" a.mp4 h264 720 aac 4 preset=0
convert_case "480 lines"          "$CV/hd.mp4" b.mp4 h264 480 aac 4 preset=0 size=4
convert_case "sound only: MP3"    "$CV/hd.mp4" c.mp3 "" "" mp3 4 preset=3
convert_case "sound only: AAC"    "$CV/hd.mp4" d.m4a "" "" aac 4 preset=4
convert_case "MKV, 0:01 to 0:03"  "$CV/hd.mp4" e.mkv h264 720 aac 2 preset=1 format=1 from=0:01 to=3
convert_case "silent, balanced"   "$CV/hd.mp4" f.mp4 h264 360 "" 4 preset=1 size=5 sound=0 speed=2 quality=0
convert_case "interlaced MPEG-2"  "$SAMPLES/mv_mpeg2_il.mpg" g.mp4 h264 184 "" 2 preset=2
convert_case "HEVC"               "$SAMPLES/hevc_322_182.mkv" h.mp4 h264 182 "" 1 preset=0
convert_case "odd size"           "$CV/odd.mkv" i.mp4 h264 180 "" 2 preset=1

echo "== fffront_test (the icon bar front end, on this host)"
gcc -O1 -DFFFRONT_NO_MAIN -I"$HERE/fake" -I"$TOP/frontend" "$TOP/frontend/fffront.c" "$TOP/frontend/convert.c" \
  "$HERE/fffront_test.c" -o "$O/fffront_test" 2>/dev/null && "$O/fffront_test" || bad=1
# the Convert window: 32-bit ARM like RISC OS (the Wimp's blocks hold pointers), under the trapping qemu
echo "== convert_test (!FFmpeg's Convert window, scripted desktop)"
arm-linux-gnueabihf-gcc -O1 -g -marm -mno-unaligned-access -DFFFRONT_NO_MAIN -DCONV_HOST_PATHS -DCONV_TEST \
  -I"$HERE/fake" -I"$TOP/frontend" "$TOP/frontend/fffront.c" "$TOP/frontend/convert.c" "$HERE/convert_test.c" \
  -o "$O/convert_test" 2>/dev/null && "$TOP/tests/qemu/aligntrap.sh" "$O/convert_test" || bad=1
"$HERE/mesa/run.sh" || bad=1
exit $bad
