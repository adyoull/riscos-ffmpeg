# Common settings for the riscos-ffmpeg build scripts. Source it, don't run it.
#
# GCCSDK_ENV  the GCCSDK GCC 10.2 install (default ~/gccsdk/env, the layout of
#             Warzone2100/dist/gccsdk-gcc10.2-x86_64-linux-env.tgz unpacked in /root)
# DEVKIT      riscos-mesa devkit (SDL2 for ffplay), unpacked
# STAGE       where the libraries and headers are installed
# SRC         where source tarballs are unpacked and built
# DL          where the source tarballs are (checked against build/SHA256SUMS)

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${GCCSDK_ENV:=$HOME/gccsdk/env}"
: "${DEVKIT:=$TOP/devkit/riscos-mesa-devkit-20.3.5-7pre8}"
# LINUX_ARM_TEST=1 builds the same code for arm-linux-gnueabihf instead, for
# tests/qemu (run under qemu-arm with RISC OS's alignment traps). No SDL2,
# no zlib, no ffplay there.
if [ -n "${LINUX_ARM_TEST:-}" ]; then
  : "${STAGE:=$TOP/stage-linuxarm}"
  : "${SRC:=$TOP/src-linuxarm}"
else
  : "${STAGE:=$TOP/stage}"
  : "${SRC:=$TOP/src}"
fi
: "${DL:=$TOP/dl}"
: "${JOBS:=$(nproc)}"

for v in GCCSDK_ENV DEVKIT STAGE SRC DL; do
  case "${!v}" in *" "*) echo "env.sh: $v has a space in it; autotools can't cope" >&2; return 1 ;; esac
done

HOST=arm-riscos-gnueabihf
BUILD=$(gcc -dumpmachine)
CROSS=$GCCSDK_ENV/bin/$HOST-
export PATH=$GCCSDK_ENV/bin:$PATH
EXTRA_TEST_CFLAGS=
if [ -n "${LINUX_ARM_TEST:-}" ]; then
  HOST=arm-linux-gnueabihf
  CROSS=$HOST-
  # GCCSDK's driver adds this on RISC OS; match it so C code behaves alike.
  EXTRA_TEST_CFLAGS="-mno-unaligned-access -marm -DFF_STRICT_ALIGN"
fi

# CPU: any ARMv7 with NEON (Pi 2/3/4/400, Titanium, iMX6, BeagleBoard-xM,
# Pandaboard). vfpv3 rather than vfpv4 so the Cortex-A8/A9 boards work too;
# FFmpeg's hand-written NEON code is where the speed comes from, not FMA.
# (neon-vfpv3 = plain "neon"; that spelling makes the GCCSDK driver add
# -mcpu=cortex-a8, which then warns against -march on every file.)
# -fstack-clash-protection: GCC 10 ELF programs get their stack a page at a
# time; a frame bigger than 4 KB can jump the guard page (see riscos-mesa).
CPUFLAGS="-march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -mtune=cortex-a72"
OPT="-O3"
CFLAGS_RO="$OPT $CPUFLAGS -fstack-clash-protection $EXTRA_TEST_CFLAGS"

export PKG_CONFIG_LIBDIR=$STAGE/lib/pkgconfig
export PKG_CONFIG_PATH=
export PKG_CONFIG_SYSROOT_DIR=

mkdir -p "$STAGE/lib/pkgconfig" "$STAGE/include" "$SRC"

# unpack NAME TARBALL: verify the checksum and unpack into $SRC (once)
unpack() {
  local dir=$1 tarball=$2
  (cd "$DL" && grep " $tarball\$" "$TOP/build/SHA256SUMS" | sha256sum -c --quiet -) ||
    { echo "checksum failed: $tarball" >&2; return 1; }
  if [ ! -d "$SRC/$dir" ]; then
    tar xf "$DL/$tarball" -C "$SRC"
  fi
}
