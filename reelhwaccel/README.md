# ReelHWAccel - hardware video decoding for RISC OS on the Raspberry Pi

ReelHWAccel is the name for all of riscos-ffmpeg's work on decoding video
with the Raspberry Pi's own hardware instead of the ARM, for Reel and for
FFmpeg. It has three parts:

| Part | What it drives | Codecs | Boards |
|---|---|---|---|
| `vcdec` | the VideoCore's decoder, through RISC OS's VCHIQ module and the firmware's MMAL service | H.264, Motion JPEG (and MPEG-2 / VC-1 with the licences on a Pi 1-3) | Pi 1, 2, 3, Zero, 4 |
| `hevchw` | the Pi 4's HEVC block ("rpivid" on Linux) directly, with the `HEVCHW` module for its registers and interrupt | HEVC (H.265), 8 and 10-bit | Pi 4, 400, CM4 |
| `hwdec` | one small interface over both: can this codec and size be decoded in hardware, and do it | - | - |

Reel and FFmpeg will only use `hwdec` (`libhwdec`, `hwdec.h`); in FFmpeg
the decoders will appear as `h264_vchiq` and `hevc_hwdec` (not
`h264_mmal`, which is FFmpeg's existing decoder for Linux's MMAL
library).

## Where it's got to

- `vcdec`: `tools/vchiqprobe` (the VCHIQ module's SWIs and how BCMSound
  and BCMVideo use them), `tools/mmalprobe` (the firmware's
  `ril.video_decode` answers: H.264, MVC, MJPG on a Pi 4) and
  `tools/mmaldecode` (a whole decode, every picture checked against
  FFmpeg's). The library comes next, from `mmaldecode`.
- `hevchw`: `tools/hevcprobe` (the block answers) and `hevchw/` (the
  HEVCHW module: registers, memory and the interrupt all work from RISC
  OS). The decoder itself comes after `vcdec`.
- `hwdec`: not started.

Each test tool has a host test in `tests/host` (run by
`tests/host/run.sh`) against fakes of the VCHIQ module, the MMAL firmware
or the hardware.
