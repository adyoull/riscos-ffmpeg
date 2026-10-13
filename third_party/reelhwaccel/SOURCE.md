# riscos-reelhwaccel devkit (vcdec and vcenc: H.264 on the Raspberry Pi's VideoCore; hevcdec)

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
- Devkit 0.2.15, `riscos-reelhwaccel-devkit-0.2.15.tgz` (sha256
  fd21f2af8765ea1759a11de6d40e4f4b63dad79577f90f7f0fcca9de1e57ac9e;
  includes 0.2.14): hevcdec 0.1.13 and its hevc_hwdec patch (our 0023,
  the devkit's `ffmpeg/0002-avcodec-hevc_hwdec.patch` unchanged) give the
  HEVC block's frames back to RISC OS once nobody holds them, beyond
  `spare_frames` (default 2) free, where before every frame ever made was
  kept until the decoder closed (14 at 4K 10-bit: about 280MB of memory
  and logical address space). vcenc 0.1.3 (0.2.14): `h264_encvchiq` is
  the devkit's own name now, with profiles by name (`-profile:v main`) as
  well as by number, and no 6 cs at open for `rc_constant`. Patch 0027 is
  the devkit's `ffmpeg/0003-avcodec-h264_encvchiq.patch` made again
  against our series (configure's line moved by 0023, as before; the same
  files result). `include/hwhevcdec.h` (hevcdec_stats is bigger),
  `include/vcenc.h`, both libraries, `README.md` and `COPYING` replaced.
- Devkit 0.2.13, `riscos-reelhwaccel-devkit-0.2.13.tgz` (sha256
  8e37d021ea24ba9c129e7d0575e74299cd491f499e9b43dfa9ea5525258a1457):
  as 0.2.11 below, plus vcenc 0.1.2 (H.264 encoding on the VideoCore, in
  `lib/libvcdec.a`, `include/vcenc.h`) and its FFmpeg patch
  `ffmpeg/0003-avcodec-h264_vchiq-encoder.patch`, our 0027 (made again
  against our series: only configure's line moved). hevcdec 0.1.12 adds a
  row-by-row way to its benchmark. 0001 and 0002 are unchanged.
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
