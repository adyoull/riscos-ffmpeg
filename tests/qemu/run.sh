#!/bin/bash
# Run the arm-linux build of the same FFmpeg (build with LINUX_ARM_TEST=1)
# under a qemu-arm that traps unaligned accesses the way RISC OS does
# (SCTLR.A set; see aligntrap.sh and README.md). For every job:
#   - the NEON/asm run must not take an alignment fault (SIGBUS here =
#     "abort on data transfer" on RISC OS); on a fault gdb reports where;
#   - its output must equal the plain C run (-cpuflags 0), packet by packet
#     (framemd5, -bitexact). "loose" jobs (float maths, IDCT choice) are
#     only checked for faults.
# Jobs: every file in samples/ is decoded; then encoders, swscale and
# swresample run from generated input (lavfi).
# Usage: QEMU=path/to/qemu-arm tests/qemu/run.sh [job-name-pattern]
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
export QEMU=${QEMU:-qemu-arm}
FF=${FF:-$TOP/src-linuxarm/ffmpeg-5.1.10/ffmpeg_g}
WORK=${WORK:-$HERE/out}
PATTERN=${1:-}
mkdir -p "$WORK"
TRAP=$HERE/aligntrap.sh
pass=0; fail=0; failed=()

where_fault() {   # args: ffmpeg arguments
  local port=$((20000 + RANDOM % 10000))
  QEMU_ARGS="-g $port" "$TRAP" "$FF" "$@" >/dev/null 2>&1 &
  sleep 1
  timeout 120 gdb-multiarch -q -batch -ex "file $FF" -ex "target remote :$port" -ex continue \
    -ex 'x/i $pc' -ex 'bt 5' -ex kill 2>&1 | grep -E "^=>|^#[0-4] " | cut -c1-200
  wait
}

# job NAME LOOSE(0/1) ffmpeg-args...   (-bitexact -f framemd5 OUT appended)
job() {
  local name=$1 loose=$2; shift 2
  [ -n "$PATTERN" ] && [[ "$name" != *$PATTERN* ]] && return
  local args=(-hide_banner -loglevel error -nostdin -threads 1 "$@" -bitexact -f framemd5)
  "$TRAP" "$FF" "${args[@]}" -y "$WORK/$name.neon" 2>"$WORK/$name.neon.err"
  local rc=$?
  if grep -q "not found for input stream\|Unknown encoder\|Unknown input format" "$WORK/$name.neon.err"; then
    echo "skip $name: not in this build"; return
  fi
  if [ $rc -ne 0 ]; then
    echo "FAIL $name: exit $rc (NEON) $(grep -v '^qemu:\|Frame size limit' "$WORK/$name.neon.err" | head -c 200)"
    [ $rc -eq 135 ] && where_fault "${args[@]}" -y /dev/null
    fail=$((fail+1)); failed+=("$name"); return
  fi
  "$TRAP" "$FF" -cpuflags 0 "${args[@]}" -y "$WORK/$name.c" 2>"$WORK/$name.c.err"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "FAIL $name: exit $rc (C)"; fail=$((fail+1)); failed+=("$name"); return
  fi
  local n
  n=$(diff <(grep -v '^#' "$WORK/$name.neon") <(grep -v '^#' "$WORK/$name.c") | grep -c '^<')
  if [ "$n" -eq 0 ]; then
    echo "ok   $name ($(grep -vc '^#' "$WORK/$name.neon") packets, NEON == C)"; pass=$((pass+1))
  elif [ "$loose" = 1 ]; then
    echo "ok   $name (no fault; $n packets differ from C: float/IDCT, expected)"; pass=$((pass+1))
  else
    echo "DIFF $name: $n packets differ between NEON and C"; fail=$((fail+1)); failed+=("$name")
  fi
}

# --- decoders: every sample
for f in "$HERE"/samples/*; do
  b=$(basename "$f")
  case "$b" in
    aac*|ac3*|eac3*|vorbis*|opus*|wmav2*|mp3*|mp2*|h264_aac*) loose=1 ;;
    mpeg4*|mpeg2*|mjpeg*|wmv2*|h263*|dv_*|theora*|mv_mpeg4*|mv_mpeg2*|mv_wmv2*|mv_h263*|mv_theora*) loose=1 ;;  # IDCT choice, see README
    *) loose=0 ;;
  esac
  job "dec-$b" $loose -i "$f"
done

# --- the same decoders with the IDCT pinned, so NEON == C must hold
for f in "$HERE"/samples/{mpeg4,mpeg2,mjpeg,wmv2,h263,mv_mpeg4,mv_mpeg2,mv_wmv2,mv_h263}*; do
  [ -e "$f" ] || continue
  job "dec-idctsimple-$(basename "$f")" 0 -idct simple -i "$f"
done

# --- encoders (motion estimation, DCT, quantisers, pixel access)
V="testsrc2=size=322x182:rate=25:duration=0.4"
V2="testsrc2=size=640x360:rate=25:duration=0.4"
A="sine=frequency=440:sample_rate=48000:duration=0.5"
for s in "$V" "$V2"; do
  t=${s#*size=}; t=${t%%:*}
  job "enc-mpeg4-$t" 0 -f lavfi -i "$s" -c:v mpeg4 -q:v 5 -idct simple -dct int
  job "enc-mpeg2-$t" 0 -f lavfi -i "$s" -c:v mpeg2video -q:v 5 -idct simple -dct int
  job "enc-mjpeg-$t" 0 -f lavfi -i "$s" -pix_fmt yuvj420p -c:v mjpeg -q:v 5 -idct simple -dct int
  job "enc-x264-$t" 0 -f lavfi -i "$s" -c:v libx264 -preset medium
  job "enc-x264-10bit-$t" 0 -f lavfi -i "$s" -pix_fmt yuv420p10le -c:v libx264 -preset fast
  job "enc-png-$t" 0 -f lavfi -i "$s" -c:v png
  job "enc-ffv1-$t" 0 -f lavfi -i "$s" -c:v ffv1
  job "enc-prores-$t" 0 -f lavfi -i "$s" -c:v prores_ks
  job "enc-huffyuv-$t" 0 -f lavfi -i "$s" -c:v huffyuv
done
job "enc-h263" 0 -f lavfi -i "testsrc2=size=352x288:rate=25:duration=0.4" -c:v h263 -idct simple -dct int
job "enc-dv" 0 -f lavfi -i "testsrc2=size=720x576:rate=25:duration=0.2" -pix_fmt yuv420p -c:v dvvideo -idct simple -dct int
job "enc-mp3lame" 0 -f lavfi -i "$A" -c:a libmp3lame
job "enc-mp2" 0 -f lavfi -i "$A" -c:a mp2
job "enc-ac3" 1 -f lavfi -i "$A" -c:a ac3
job "enc-ac3fixed" 0 -f lavfi -i "$A" -c:a ac3_fixed
job "enc-aac" 1 -f lavfi -i "$A" -c:a aac
job "enc-opus" 0 -f lavfi -i "$A" -c:a libopus
job "enc-vorbis" 1 -f lavfi -i "$A" -c:a libvorbis
job "enc-flac" 0 -f lavfi -i "$A" -c:a flac
job "enc-alac" 0 -f lavfi -i "$A" -c:a alac
job "enc-sbc" 0 -f lavfi -i "$A" -c:a sbc
job "enc-truehd" 0 -f lavfi -i "$A" -strict -2 -c:a truehd

# --- swscale: YUV -> RGB (ffplay's path; NEON needs width % 16 == 0), scaling
# (the NEON YUV->RGB converters round differently from C: up to 3 levels)
for fmt in rgba bgra argb abgr rgb24 rgb565 rgb0 bgr0; do
  job "sws-yuv420p-$fmt-640" 1 -f lavfi -i "$V2" -pix_fmt $fmt -c:v rawvideo
  job "sws-yuv420p-$fmt-322" 1 -f lavfi -i "$V" -pix_fmt $fmt -c:v rawvideo
done
job "sws-nv12-rgba" 1 -f lavfi -i "$V2" -vf format=nv12,format=rgba -c:v rawvideo
job "sws-yuv422p-rgba" 1 -f lavfi -i "$V2" -vf format=yuv422p,format=rgba -c:v rawvideo
job "sws-rgba-nv12" 1 -f lavfi -i "$V2" -vf format=rgba,format=nv12 -c:v rawvideo
for flags in bilinear bicubic lanczos fast_bilinear; do
  job "sws-scale-$flags" 1 -f lavfi -i "$V" -vf scale=500:280:flags=$flags -c:v rawvideo
done
job "sws-scale-rgba" 1 -f lavfi -i "$V" -vf format=rgba,scale=1000:562 -c:v rawvideo
job "sws-10bit" 1 -f lavfi -i "$V" -vf format=yuv420p10le,scale=640:360,format=yuv420p -c:v rawvideo

# --- swresample: rate, format and layout conversion
job "swr-rate" 1 -f lavfi -i "$A" -ar 44100 -c:a pcm_s16le
job "swr-s16-flt" 1 -f lavfi -i "$A" -sample_fmt flt -c:a pcm_f32le
job "swr-mono-stereo" 1 -f lavfi -i "$A" -ac 2 -ar 22050 -c:a pcm_s16le

echo "passed $pass, failed $fail ${failed[*]:-}"
[ $fail -eq 0 ]
