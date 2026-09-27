#!/bin/bash
# ffplay's RISC OS drawing path (patch 0003: frames converted and scaled
# straight into SDL's window surface), run on the Linux host: ffplay.c is
# compiled with -D__riscos__ against the host's SDL2, whose "dummy" video
# driver writes every presented window surface to a BMP. Checks:
#   1. at the video's size: the picture equals ffmpeg's own conversion
#      (to within swscale's rounding, PSNR > 45 dB);
#   2. in a bigger window (-x 800 -y 600): letterboxed, black bars;
#   3. the audio waveform mode and FFPLAY_RENDERER=sdl still draw;
#   4. in a (fake) TaskWindow it starts itself as a new Wimp task through
#      !FFmpeg.Task, with its messages in a log, and exits.
# Needs: libsdl2-dev, python3-pil, python3-numpy, a host ffmpeg (for the
# reference frames). Builds a host FFmpeg in src-host/ the first time
# (about 10 minutes).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
H=$TOP/src-host/ffmpeg-5.1.10
CLIP=$TOP/tests/qemu/samples/h264_aac_640_360.mp4
SMALL=$TOP/tests/qemu/samples/h264_aac_322_182.mp4

if [ ! -x "$H/ffplay_g" ]; then
  mkdir -p "$TOP/src-host"
  tar xf "$TOP/dl/ffmpeg-5.1.10.tar.xz" -C "$TOP/src-host"
  (cd "$H" && for p in "$TOP"/patches/ffmpeg/*.patch; do patch -s -p1 < "$p"; done &&
   ./configure --disable-doc --enable-sdl2 --enable-ffplay --disable-debug --disable-asm >/dev/null &&
   make -j"$(nproc)" ffplay_g >/dev/null)
fi
# the RISC OS path in ffplay.c, on this host
(cd "$H" && gcc -D__riscos__ -I. -I"$HERE/fake" -include "$HERE/ffplay-kernel-stub.h" $(sdl2-config --cflags) \
   -O2 -c fftools/ffplay.c -o fftools/ffplay.o &&
 make ffplay_g >/dev/null)

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
run() {   # run DIR env... -- ffplay args
  local d=$1; shift
  mkdir -p "$W/$d"
  (cd "$W/$d" && env SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy \
     SDL_VIDEO_DUMMY_SAVE_FRAMES=1 "$@" "$H/ffplay_g" -v error -autoexit "${ARGS[@]}")
}
ARGS=("$CLIP");                         run same
ARGS=(-x 800 -y 600 "$SMALL");          run big
ARGS=(-showmode waves "$SMALL");        run waves
ARGS=("$SMALL");                        run sdl FFPLAY_RENDERER=sdl

# 4. a TaskWindow
out=$(env FAKE_TASKWINDOW=1 FAKE_STARTTASK="$W/starttask" 'Wimp$ScrapDir=ADFS::HD4.$.Scrap' \
      SDL_VIDEODRIVER=dummy "$H/ffplay_g" -v error "$SMALL" 2>&1) || { echo "taskwindow: exit status $?"; exit 1; }
want="Obey SDFS::Pi.\$.Apps.!FFmpeg.Task ffplay -nostats -hide_banner -v error $SMALL > ADFS::HD4.\$.Scrap.ffplay/log 2>&1"
got=$(cat "$W/starttask" 2>/dev/null || true)
if [ "$got" = "$want" ] && [[ $out == *"runs as its own"* ]]; then
  echo "taskwindow: started as a new task: $got"
else
  echo "taskwindow FAILED: got [$got]"; echo "$out"; exit 1
fi

python3 - "$W" "$CLIP" <<'PY'
import glob, math, subprocess, sys
import numpy as np
from PIL import Image
w, clip = sys.argv[1], sys.argv[2]
fails = 0
def frames(d): return sorted(glob.glob(f"{w}/{d}/*.bmp"))
def img(f): return np.asarray(Image.open(f).convert("RGB")).astype(int)

raw = subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                     capture_output=True).stdout
ref = np.frombuffer(raw, np.uint8).reshape(-1, 360, 640, 3).astype(int)
fs = frames("same")
a = img(fs[len(fs) // 2])
best = min(range(len(ref)), key=lambda i: np.abs(ref[i] - a).mean())
mse = ((ref[best] - a) ** 2).mean()
psnr = 10 * math.log10(255 ** 2 / mse) if mse else 99
print(f"same size: {len(fs)} frames shown, PSNR {psnr:.1f} dB against the reference")
fails += psnr < 45 or len(fs) < 20

fs = frames("big")
a = img(fs[len(fs) // 2])
rows = np.where(a.max(axis=(1, 2)) > 20)[0]
print(f"800x600 window: picture rows {rows.min()}-{rows.max()} (expect about 74-525)")
fails += a.shape[:2] != (600, 800) or abs(rows.min() - 74) > 3 or abs(rows.max() - 525) > 3 \
         or a[:60].max() > 0 or a[540:].max() > 0

for d in ("waves", "sdl"):
    fs = frames(d)
    lit = (img(fs[len(fs) // 2]).max(axis=2) > 20).sum() if fs else 0
    print(f"{d}: {len(fs)} frames, {lit} lit pixels")
    fails += len(fs) < 20 or lit < 100
print("all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
PY
