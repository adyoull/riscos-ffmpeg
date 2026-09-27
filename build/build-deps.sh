#!/bin/bash
# Static libraries FFmpeg links against, cross-built for RISC OS into $STAGE:
#   zlib (from the riscos-mesa devkit), SDL2 (riscos-mesa overlay, no GL),
#   libogg, libvorbis, LAME, Opus, x264, dav1d.
# Usage: build/build-deps.sh [name...]   (default: all, in order)
set -euo pipefail
source "$(dirname "$0")/env.sh"

# Autotools cross configure with our flags.
ac_configure() {
  CC=${CROSS}gcc CXX=${CROSS}g++ AR=${CROSS}ar RANLIB=${CROSS}ranlib STRIP=${CROSS}strip \
  CFLAGS="$CFLAGS_RO -I$STAGE/include" LDFLAGS="-L$STAGE/lib" \
    ./configure --host=$HOST --build=$BUILD --prefix="$STAGE" \
      --disable-shared --enable-static "$@"
}

dep_zlib() {
  cp "$DEVKIT"/include/zlib.h "$DEVKIT"/include/zconf.h "$STAGE/include/"
  cp "$DEVKIT"/lib/libz.a "$STAGE/lib/"
  cat > "$STAGE/lib/pkgconfig/zlib.pc" <<EOF
prefix=$STAGE
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib compression library (riscos-mesa devkit)
Version: 1.3.1
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
}

# SDL2 for ffplay: the riscos-mesa overlay (patches/sdl2, see SOURCE there)
# configured WITHOUT --enable-video-riscos-osmesa, i.e. the OpenTTD
# configuration: Wimp video + SharedSoundBuffer audio, no OSMesa to link.
dep_sdl2() {
  local d=SDL-release-2.26.0
  unpack $d SDL-2.26.0.tgz
  if [ ! -f "$SRC/$d/.riscos-overlay" ]; then
    (cd "$SRC/$d" && for p in "$TOP"/patches/sdl2/*.p; do patch -s -p0 < "$p"; done &&
       ./autogen.sh >/dev/null && touch .riscos-overlay)
  fi
  mkdir -p "$SRC/$d/build-ro" && cd "$SRC/$d/build-ro"
  CFLAGS="$CFLAGS_RO -I$STAGE/include" LDFLAGS="-L$STAGE/lib" \
    ../configure --host=$HOST --build=$BUILD --prefix="$STAGE" --disable-shared --enable-static \
      --disable-video-opengl --disable-video-opengles --disable-video-rpi >/dev/null
  grep -q "define SDL_VIDEO_DRIVER_RISCOS 1" include/SDL_config.h || { echo "no RISC OS video driver" >&2; exit 1; }
  grep -q "define SDL_AUDIO_DRIVER_RISCOS 1" include/SDL_config.h || { echo "no RISC OS audio driver" >&2; exit 1; }
  if grep -q "define SDL_VIDEO_OPENGL_OSMESA 1" include/SDL_config.h; then echo "OSMesa GL must be off" >&2; exit 1; fi
  make -j"$JOBS" >/dev/null && make install >/dev/null
}

# riscos-mesa's EGL, OpenGL (OSMesa), GLU and freeglut, for the egl output
# device, ffegl and !ReelEGL: headers and static libraries from the devkit.
dep_egl() {
  for d in EGL KHR GL GLES GLES2; do
    [ -d "$DEVKIT/include/$d" ] && cp -r "$DEVKIT/include/$d" "$STAGE/include/"
  done
  for l in EGL OSMesa GLU glut; do
    cp "$DEVKIT/lib/lib$l.a" "$STAGE/lib/"
  done
}

dep_ogg() {
  unpack libogg-1.3.5 libogg_1.3.5.orig.tar.gz
  cd "$SRC/libogg-1.3.5" && ac_configure >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null
}

dep_vorbis() {
  unpack libvorbis-1.3.7 libvorbis_1.3.7.orig.tar.gz
  cd "$SRC/libvorbis-1.3.7" && ac_configure --disable-docs --disable-examples --disable-oggtest >/dev/null &&
    make -j"$JOBS" >/dev/null && make install >/dev/null
}

dep_lame() {
  unpack lame-3.100 lame_3.100.orig.tar.gz
  cd "$SRC/lame-3.100"
  # The __riscos__ code is for the old FPA floating point (asmstuff.h is
  # missing anyway); VFP needs none of it.
  patch -s -p1 -N -r - < "$TOP/patches/lame/lame-3.100-riscos-vfp.patch" || true
  # lame 3.100 exports a symbol that doesn't exist; harmless for static builds
  sed -i '/lame_init_old/d' include/libmp3lame.sym
  ac_configure --disable-frontend --disable-decoder --disable-gtktest --disable-analyzer-hooks >/dev/null &&
    make -j"$JOBS" >/dev/null && make install >/dev/null
}

dep_opus() {
  unpack opus-1.5.2 opus_1.5.2.orig.tar.gz
  # Opus has ARM NEON intrinsics and ARMv7 assembly; with the NEON flags in
  # CFLAGS it uses them without run-time detection (none on RISC OS).
  cd "$SRC/opus-1.5.2" && ac_configure --disable-doc --disable-extra-programs \
      --enable-fixed-point=no --disable-rtcd >/dev/null &&
    make -j"$JOBS" >/dev/null && make install >/dev/null
}

dep_x264() {
  local d=x264-master
  unpack $d "x264_0.165.3222+gitb35605ac.orig.tar.gz"
  cd "$SRC/$d"
  if [ ! -f .riscos-align ]; then     # RISC OS traps unaligned accesses
    patch -s -p1 < "$TOP/patches/x264/x264-riscos-align.patch"
    "$TOP/tools/neon-align-apply.sh" "$TOP/tools/neon-align-x264.allow" common/arm/*.S
    touch .riscos-align
  fi
  # x264's configure knows "linux", not riscos; host OS only picks the
  # thread library and -lm, so tell it linux and keep its NEON asm (armv7).
  # No threads: RISC OS has one core, and x264 threads only add switching.
  # NEON is used because it is compiled in (-mfpu=neon-vfpv3), no detection.
  CC=${CROSS}gcc AR=${CROSS}ar RANLIB=${CROSS}ranlib STRIP=${CROSS}strip \
    ./configure --host=arm-linux-gnueabihf --cross-prefix=${CROSS} --prefix="$STAGE" \
      --enable-static --disable-cli --disable-thread --disable-opencl --disable-lavf --disable-swscale \
      --disable-ffms --disable-gpac --disable-lsmash --enable-pic=no \
      --extra-cflags="$CPUFLAGS -fstack-clash-protection $EXTRA_TEST_CFLAGS" \
      ${LINUX_ARM_TEST:+--extra-ldflags=-no-pie} >/dev/null
  make -j"$JOBS" >/dev/null 2>"$SRC/x264-make.log" && make install-lib-static >/dev/null
  if [ -n "${LINUX_ARM_TEST:-}" ]; then make checkasm >/dev/null 2>>"$SRC/x264-make.log"; fi
}

dep_dav1d() {
  unpack dav1d-1.5.4 dav1d_1.5.4.orig.tar.xz
  cd "$SRC/dav1d-1.5.4"
  # RISC OS traps unaligned accesses: byte loads for the entropy decoder's
  # bitstream refill, and the NEON rewrite (tools/neon-align.py).
  if [ ! -f .riscos-align ]; then
    patch -s -p1 < "$TOP/patches/dav1d/dav1d-1.5.4-riscos.patch"
    "$TOP/tools/neon-align-apply.sh" "$TOP/tools/neon-align-dav1d.allow" src/arm/32/*.S
    touch .riscos-align
  fi
  local cf=$SRC/meson-riscos.txt
  # meson's threads dependency adds -pthread, which GCCSDK GCC rejects
  # (pthreads are part of UnixLib), so compile through a wrapper that drops it.
  printf '#!/bin/sh\nfor a; do shift; [ "$a" = -pthread ] || set -- "$@" "$a"; done\nexec %sgcc "$@"\n' \
    "$CROSS" > "$SRC/ro-cc"
  chmod +x "$SRC/ro-cc"
  cat > "$cf" <<EOF
[binaries]
c = '$SRC/ro-cc'
ar = '${CROSS}ar'
strip = '${CROSS}strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = [$(printf "'%s', " $CFLAGS_RO)'-D_GNU_SOURCE']
default_library = 'static'
b_staticpic = false

[host_machine]
system = 'riscos'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF
  local tests=false testopts=
  if [ -n "${LINUX_ARM_TEST:-}" ]; then    # dav1d's checkasm, for tests/qemu
    tests=true
    testopts="-Dtrim_dsp=false -Dc_link_args=-no-pie"
    # the wrap points at code.videolan.org; its author's GitHub copy, pinned
    if [ ! -d subprojects/checkasm ]; then
      git clone -q --depth 1 -b v1.2.0 https://github.com/haasn/checkasm subprojects/checkasm
      [ "$(git -C subprojects/checkasm rev-parse HEAD)" = 0df02535c7435cf3969ca141c9e3ff7b1c1e6c28 ] ||
        { echo "checkasm: unexpected commit" >&2; exit 1; }
    fi
  fi
  rm -rf build-ro
  meson setup build-ro --cross-file "$cf" --prefix="$STAGE" --libdir=lib --buildtype=release \
    -Denable_tools=false -Denable_tests=$tests -Denable_asm=true $testopts >/dev/null
  ninja -C build-ro >/dev/null && ninja -C build-ro install >/dev/null
  sed -i 's/ -pthread//' "$STAGE/lib/pkgconfig/dav1d.pc"
}

ALL="zlib sdl2 egl ogg vorbis lame opus x264 dav1d"
[ -n "${LINUX_ARM_TEST:-}" ] && ALL="ogg vorbis lame opus x264 dav1d"
for d in ${*:-$ALL}; do
  echo "== $d"
  ( "dep_$d" )
done
echo "deps done: $(ls "$STAGE"/lib/*.a | xargs -n1 basename | tr '\n' ' ')"
