# riscos-reelhwaccel devkit (vcdec: H.264 on the Raspberry Pi's VideoCore)

`build/build-ffmpeg.sh` builds FFmpeg with `--enable-vchiq` (patch 0021,
`h264_vchiq`) when `include/vcdec.h` and `lib/libvcdec.a` are here, and
without it otherwise. `build/build-apps.sh` then links Reel, ReelEGL and
the EGL examples with `-lvcdec`, and the devkit carries `libvcdec.a`.

- From github.com/adyoull/riscos-reelhwaccel's devkit (GPL version 2 or
  later): `include/vcdec.h`, `lib/libvcdec.a`, `COPYING`, `README.md`.
  Patch 0021 is the devkit's `ffmpeg/0001-avcodec-h264_vchiq.patch`.
- Devkit 0.1, `riscos-reelhwaccel-devkit-0.1.tgz` (sha256
  7cfcea8cdec7e51648698a9443bc47bbbbfc620c3635087c825a1a33cf2484ce),
  riscos-reelhwaccel commit 2c3b8ca. Its `libvcdec.a` holds `vcdec.o` and
  `vcdec_copy.o`.
