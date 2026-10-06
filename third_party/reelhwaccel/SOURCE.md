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
- Devkit 0.2.11, `riscos-reelhwaccel-devkit-0.2.11.tgz` (sha256
  8a570fb81588ab2577f9f8e19bffa602d8af2b7c9f3ac2e14607fc2d9e945ae1):
  vcdec 0.4.2 (H.264; its contiguous memory never includes the program's
  page at &8000) and hevcdec 0.1.11 (HEVC on the Pi 4's HEVC block, 8-bit
  and 10-bit, up to 4K; 0.1.10's audit fixes; a frame's cache cleaned
  once however often it's converted, `hevcdec_frame_done`, and stats of
  conversions that waited). Its hevc_hwdec patch has `drop_before` and
  `output_hw` (frames handed out unconverted, AV_PIX_FMT_HEVCDEC, for
  `hevcdec_frame_to_i420` / `_half` at show time: reelcore converts them
  straight into the overlay). `lib/libvcdec.a`, `lib/libhevcdec.a`,
  `include/vcdec.h`, `include/hwhevcdec.h`, `include/hevc_ctrls.h`. Patch
  0021 is its `ffmpeg/0001-avcodec-h264_vchiq.patch` and patch 0023 its
  `ffmpeg/0002-avcodec-hevc_hwdec.patch`, both unchanged (0.2.11's
  hevc_hwdec passes the caller's `skip_frame` on, so `AVDISCARD_NONREF`
  keeps the pictures nothing refers to from the block, and counts them:
  the read-only option `skipped`); patch 0022
  (`drop_before` for h264_vchiq) is ours, after 0021. Before: devkit
  0.2.10 (babece2b…), 0.2.8
  (111f82b5…), 0.2.7
  (c65b8ac5…), 0.2.6 (80dbabab…), 0.2.4 (8876f366…), 0.2.1 (749197ad…),
  0.1 (7cfcea8c…).
