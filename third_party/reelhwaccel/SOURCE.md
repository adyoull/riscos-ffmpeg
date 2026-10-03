# riscos-reelhwaccel devkit (vcdec: H.264 on the Raspberry Pi's VideoCore)

`build/build-ffmpeg.sh` builds FFmpeg with `--enable-vchiq` (patch 0021,
`h264_vchiq`) when `include/vcdec.h` and `lib/libvcdec.a` are here, and
without it otherwise. `build/build-apps.sh` then links Reel, ReelEGL and
the EGL examples with `-lvcdec`, and the devkit carries `libvcdec.a`.

- From https://github.com/adyoull/riscos-reelhwaccel's devkit (GPL version
  2, the same licence as Linux's vchiq-mmal driver: none of the driver's
  code, but treated as derived from it, having used it as its reference;
  a new implementation over RISC OS's VCHIQ module
  with its own MMAL client, based on the MMAL message formats as that
  driver's headers define them): `include/vcdec.h`,
  `lib/libvcdec.a`, `COPYING`, `README.md`. `Licence` is ours, packaged
  as docs.Licences.ReelHWAccel.
  Patch 0021 is the devkit's `ffmpeg/0001-avcodec-h264_vchiq.patch`.
- Devkit 0.2.6, `riscos-reelhwaccel-devkit-0.2.6.tgz` (sha256
  80dbabab8d69c744d48b4c803bf7ab744f2882b3980cd6c1668cec52d6d4670c):
  vcdec 0.4.2 (H.264; its contiguous memory never includes the program's
  page at &8000, which ARMEABISupport 1.08 would lose track of) and
  hevcdec 0.1.8 (HEVC on the Pi 4's HEVC block, 8-bit and 10-bit, up to
  4K; it converts one picture while the block decodes the next).
  `lib/libvcdec.a`, `lib/libhevcdec.a`, `include/vcdec.h`,
  `include/hwhevcdec.h`, `include/hevc_ctrls.h`. Patch 0021 is its
  `ffmpeg/0001-avcodec-h264_vchiq.patch` and patch 0023 its
  `ffmpeg/0002-avcodec-hevc_hwdec.patch`, both unchanged; patch 0022
  (`drop_before`) is ours, after 0021. Before: devkit 0.2.4 (8876f366…),
  0.2.1 (749197ad…), 0.1 (7cfcea8c…).
