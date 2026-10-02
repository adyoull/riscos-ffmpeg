# riscos-reelhwaccel devkit (vcdec: H.264 on the Raspberry Pi's VideoCore)

`build/build-ffmpeg.sh` builds FFmpeg with `--enable-vchiq` (patch 0021,
`h264_vchiq`) when `include/vcdec.h` and `lib/libvcdec.a` are here, and
without it otherwise. `build/build-apps.sh` then links Reel, ReelEGL and
the EGL examples with `-lvcdec`, and the devkit carries `libvcdec.a`.

- From https://github.com/adyoull/riscos-reelhwaccel's devkit (GPL version
  2 or later, the same version as Linux's vchiq-mmal driver, which it
  used as a reference: a new implementation over RISC OS's VCHIQ module
  with its own MMAL client, no code copied): `include/vcdec.h`,
  `lib/libvcdec.a`, `COPYING`, `README.md`. `Licence` is ours, packaged
  as docs.Licences.ReelHWAccel.
  Patch 0021 is the devkit's `ffmpeg/0001-avcodec-h264_vchiq.patch`.
- Devkit 0.2.1, `riscos-reelhwaccel-devkit-0.2.1.tgz` (sha256
  749197ad2828a939444678f14d7fbefcc0a51b723ff3b14cbf2d2d5e2cdbfbde),
  vcdec 0.4.1: h264_vchiq's frames are vcdec's own picture buffers
  (zero-copy). Its `libvcdec.a` holds `vcdec.o` and `vcdec_copy.o`. Patch
  0021 is its `ffmpeg/0001-avcodec-h264_vchiq.patch` unchanged; patch 0022
  (`drop_before`) is ours, on top. Before: devkit 0.1 (7cfcea8c…).
