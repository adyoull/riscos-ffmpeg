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

## Licence

GPL version 2 or later (see `COPYING`), like the rest of riscos-ffmpeg:
Reel and FFmpeg, which will use it, are GPL already (FFmpeg's build
includes x264).

What it's built from, and how:

| Source | Licence | Used for |
|---|---|---|
| Raspberry Pi userland, MMAL client (`interface/mmal/vc/mmal_vc_msgs.h`, `mmal_vc_client.c`) | BSD-3-Clause (Broadcom) | MMAL message layouts and how the client handles them: facts only, no code copied |
| Linux `vchiq-mmal` (`mmal-msg*.h`, `mmal-vchiq.c`) | GPL-2.0 only | the same layouts, checked against these: facts only, no code copied |
| Linux `rpivid` (`rpivid_hw.h`) and device tree (`bcm2711*.dtsi`) | GPL-2.0 | the HEVC block's register offsets, addresses and interrupt: facts only |
| RISC OS's VCHIQ, BCMSound and BCMVideo modules (in the ROM) | - | the VCHIQ SWI conventions, learnt by running and disassembling them |

The HEVC decoder itself (still to come) may follow rpivid's two-phase
pipeline (`rpivid_h265.c`, GPL-2.0-or-later); any part translated from
it stays GPL-2.0-or-later. Code from `mmal-vchiq.c` (GPL-2.0 only) is
not to be copied.
