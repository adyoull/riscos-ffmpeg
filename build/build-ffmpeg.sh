#!/bin/bash
# FFmpeg 5.1.10 LTS for RISC OS (GCCSDK GCC 10.2, arm-riscos-gnueabihf):
# ffmpeg, ffprobe, ffplay (SDL2) and the static libraries, into $STAGE.
# Needs build/build-deps.sh first.
#
# Usage: build/build-ffmpeg.sh            configure (once) + build + install
#        RECONFIGURE=1 build/build-ffmpeg.sh
set -euo pipefail
source "$(dirname "$0")/env.sh"

D=ffmpeg-5.1.10
unpack $D ffmpeg-5.1.10.tar.xz
cd "$SRC/$D"
if [ ! -f .riscos-patched ]; then
  for p in "$TOP"/patches/ffmpeg/*.patch; do
    echo "patch: $(basename "$p")"
    patch -s -p1 < "$p"
  done
  # RISC OS traps unaligned accesses: make the NEON assembly safe
  # (tools/neon-align.py explains the rewrite).
  find libavcodec/arm libavutil/arm libswscale/arm libswresample/arm -name '*.S' -print0 |
    xargs -0 "$TOP/tools/neon-align-apply.sh" "$TOP/tools/neon-align-ffmpeg.allow"
  touch .riscos-patched
fi

if [ ! -f ffbuild/config.mak ] || [ -n "${RECONFIGURE:-}" ]; then
  # --target-os=linux: FFmpeg has no RISC OS target; "linux" only selects
  #   generic Unix code here (devices are off, and libavutil's ARM CPU
  #   detection is keyed on __linux__, which GCCSDK doesn't define, so the
  #   CPU features are the compile-time ones below).
  # --disable-fast-unaligned: RISC OS traps unaligned loads (alignment
  #   exceptions are on), so C code must not rely on them.
  # --disable-pic: static AIF programs; PIC code costs a register and GOT
  #   loads for every global.
  if [ -n "${LINUX_ARM_TEST:-}" ]; then
    # link maps: tests/qemu/ignore-ranges.py finds the C library in them
    # Linked dynamically against glibc: its string functions assume unaligned
    # access works, so tests/qemu traps alignment only inside the program.
    VARIANT="--disable-sdl2 --disable-ffplay --disable-zlib"
    OUTDEVS=
    LINK=-no-pie
  else
    VARIANT="--enable-zlib --enable-sdl2 --enable-ffplay --enable-riscos-egl"
    OUTDEVS="--enable-outdev=egl"   # after --disable-outdevs
    LINK=-static
  fi
  ./configure \
    --enable-cross-compile --cross-prefix="$CROSS" --arch=arm --cpu=armv7-a \
    --target-os=linux --prefix="$STAGE" \
    --pkg-config=pkg-config --pkg-config-flags=--static \
    --extra-cflags="$CPUFLAGS -fstack-clash-protection $EXTRA_TEST_CFLAGS -I$STAGE/include" \
    --extra-ldflags="-L$STAGE/lib $LINK" \
    --enable-static --disable-shared --disable-pic \
    --enable-neon --enable-vfp --enable-armv5te --enable-armv6 --enable-armv6t2 \
    --disable-fast-unaligned --disable-runtime-cpudetect \
    --enable-gpl \
    --enable-pthreads --disable-w32threads --disable-os2threads \
    --enable-libx264 --enable-libmp3lame --enable-libopus --enable-libvorbis \
    --enable-libdav1d $VARIANT \
    --enable-network --disable-gnutls --disable-openssl \
    --disable-indevs --enable-indev=lavfi --disable-outdevs $OUTDEVS --disable-linux-perf \
    --disable-xlib --disable-libxcb --disable-vaapi --disable-vdpau \
    --disable-v4l2-m2m --disable-libdrm \
    --enable-ffmpeg --enable-ffprobe \
    --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages \
    --extra-version=riscos2 \
    > "$SRC/ffmpeg-configure.log"
  tail -n +1 "$SRC/ffmpeg-configure.log" | sed -n '1,200p' | grep -E "^(ARCH|big-endian|NEON|runtime|pthreads|External libraries:)" || true
fi

for c in NEON VFP ARMV6 FAST_UNALIGNED PTHREADS; do
  printf '%-16s %s\n' "HAVE_$c" "$(grep -E "^#define HAVE_$c " config.h | awk '{print $3}')"
done
# the version is the release, not the git commit of this repository
# (ffbuild/version.sh would otherwise use "git describe" of the tree above)
export revision=$(cat RELEASE)
make -j"$JOBS" >"$SRC/ffmpeg-make.log" 2>&1 || { tail -40 "$SRC/ffmpeg-make.log"; exit 1; }
make install >/dev/null
ls -la ffmpeg_g ffprobe_g ffplay_g 2>/dev/null || true
