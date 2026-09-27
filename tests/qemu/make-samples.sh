#!/bin/bash
# Short test clips in many codecs, made with the HOST's ffmpeg (any recent
# one with libx264/x265/vpx/aom/theora). Two picture sizes: 640x360 and an
# awkward 322x182, so odd widths and unaligned rows get exercised.
# Usage: tests/qemu/make-samples.sh [DIR]   (default tests/qemu/samples)
set -euo pipefail
OUT=${1:-$(dirname "$0")/samples}
mkdir -p "$OUT"
FF="ffmpeg -hide_banner -loglevel error -y"

for size in 640x360 322x182; do
  V="-f lavfi -i testsrc2=size=$size:rate=25:duration=1"
  A="-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1"
  s=${size/x/_}
  $FF $V $A -c:v libx264 -preset fast -pix_fmt yuv420p -c:a aac -shortest "$OUT/h264_aac_$s.mp4"
  $FF $V -c:v libx264 -preset fast -pix_fmt yuv420p10le "$OUT/h264_10bit_$s.mkv"
  $FF $V -c:v libx264 -preset fast -pix_fmt yuv422p "$OUT/h264_422_$s.mkv"
  $FF $V -c:v libx265 -preset fast -x265-params log-level=0 "$OUT/hevc_$s.mkv"
  $FF $V -c:v libx265 -preset fast -pix_fmt yuv420p10le -x265-params log-level=0 "$OUT/hevc_10bit_$s.mkv"
  $FF $V -c:v libvpx -b:v 500k "$OUT/vp8_$s.webm"
  $FF $V -c:v libvpx-vp9 -b:v 500k "$OUT/vp9_$s.webm"
  $FF $V -c:v libaom-av1 -cpu-used 8 -b:v 300k "$OUT/av1_$s.mkv"
  $FF $V -c:v mpeg2video -q:v 4 "$OUT/mpeg2_$s.mpg"
  $FF $V -c:v mpeg4 -q:v 4 "$OUT/mpeg4_$s.avi"
  $FF $V -c:v mjpeg -q:v 4 "$OUT/mjpeg_$s.avi"
  $FF $V -c:v libtheora -q:v 6 "$OUT/theora_$s.ogv"
  $FF $V -c:v wmv2 "$OUT/wmv2_$s.wmv"
  $FF $V -c:v prores "$OUT/prores_$s.mov"
  $FF $V -c:v dvvideo -s 720x576 -pix_fmt yuv420p -r 25 "$OUT/dv_$s.dv" 2>/dev/null || true
  $FF $V -c:v png "$OUT/png_$s.mkv"
  $FF $V -c:v ffv1 "$OUT/ffv1_$s.mkv"
done
# Motion stress clips ("mv_" prefix): the testsrc2 clips above hardly move,
# so most motion-compensation paths (sub-pixel, small partitions, B, weighted,
# interlaced) never run on them. Here the picture scrolls diagonally by
# non-integer amounts and zooms, and the encoders are told to use every
# partition / tool. This is how FFmpeg's H.264 chroma mc2 ldrh (4x4
# partitions at integer chroma positions, odd source address) was missed
# at first and then found on a Pi with Big Buck Bunny.
M="-f lavfi -i testsrc2=size=322x184:rate=25:duration=2,scroll=h=0.0071:v=0.0113,zoompan=z='1+0.002*on':d=1:s=322x184:fps=25"
$FF $M -c:v libx264 -preset slow -x264-params partitions=all:subme=9:me=umh:bframes=3:b-pyramid=normal:weightb=1:weightp=2:ref=4:8x8dct=1 -pix_fmt yuv420p "$OUT/mv_h264_all.mkv"
$FF $M -c:v libx264 -preset slow -x264-params partitions=all:subme=9:cabac=0:bframes=2 -profile:v main -pix_fmt yuv420p "$OUT/mv_h264_cavlc.mkv"
$FF $M -c:v libx264 -preset slow -x264-params partitions=all:subme=9:interlaced=1:bframes=2 -pix_fmt yuv420p "$OUT/mv_h264_mbaff.mkv"
$FF $M -c:v libx264 -preset slow -x264-params partitions=all:subme=9:bframes=2 -pix_fmt yuv420p10le "$OUT/mv_h264_10bit.mkv"
$FF $M -c:v libx264 -preset slow -x264-params partitions=all:subme=9:bframes=2 -pix_fmt yuv422p "$OUT/mv_h264_422.mkv"
$FF $M -c:v libx265 -preset slow -x265-params log-level=0:bframes=3:weightb=1 "$OUT/mv_hevc.mkv"
$FF $M -c:v libvpx -b:v 400k -cpu-used 0 "$OUT/mv_vp8.webm"
$FF $M -c:v libvpx-vp9 -b:v 300k -cpu-used 1 "$OUT/mv_vp9.webm"
$FF $M -c:v libaom-av1 -cpu-used 6 -b:v 200k "$OUT/mv_av1.mkv"
$FF $M -c:v mpeg4 -q:v 5 -mbd 2 -flags +qpel+mv4 -bf 2 "$OUT/mv_mpeg4_qpel.avi"
$FF $M -c:v mpeg2video -q:v 5 -bf 2 -flags +ildct+ilme "$OUT/mv_mpeg2_il.mpg"
$FF $M -c:v libtheora -q:v 5 "$OUT/mv_theora.ogv"
$FF $M -c:v wmv2 -q:v 5 "$OUT/mv_wmv2.wmv"
$FF -f lavfi -i testsrc2=size=352x288:rate=25:duration=2,scroll=h=0.0071:v=0.0113 -c:v h263 -q:v 5 "$OUT/mv_h263.3gp"

# A 6 s clip with sound for tests/host/reel_test (the player's controls)
$FF -f lavfi -i testsrc2=size=322x184:rate=25:duration=6 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=6 \
  -c:v libx264 -preset fast -pix_fmt yuv420p -c:a aac -shortest "$OUT/long_h264_aac_322_184.mp4"

# H.263 needs a standard size
$FF -f lavfi -i testsrc2=size=352x288:rate=25:duration=1 -c:v h263 "$OUT/h263_352_288.3gp"

A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 -f lavfi -i sine=frequency=660:sample_rate=44100:duration=2 -filter_complex amerge=inputs=2"
$FF $A -c:a aac -b:a 128k "$OUT/aac.m4a"
$FF $A -c:a libmp3lame -b:a 128k "$OUT/mp3.mp3"
$FF $A -c:a ac3 "$OUT/ac3.ac3"
$FF $A -c:a eac3 "$OUT/eac3.eac3"
$FF $A -c:a libvorbis "$OUT/vorbis.ogg"
$FF $A -c:a libopus -ar 48000 "$OUT/opus.opus"
$FF $A -c:a flac "$OUT/flac.flac"
$FF $A -c:a alac "$OUT/alac.m4a"
$FF $A -c:a wmav2 "$OUT/wmav2.wma"
$FF $A -c:a mp2 "$OUT/mp2.mp2"
$FF $A -c:a pcm_s16le "$OUT/pcm.wav"
ls "$OUT" | wc -l
