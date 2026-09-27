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
