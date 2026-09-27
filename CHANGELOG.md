# Changes

## 5.1.10-riscos1 (2026-09-27)

First release: FFmpeg 5.1.10 LTS for RISC OS on ARMv7 + NEON.

- Built with GCCSDK GCC 10.2 (`-O3`, `-mfpu=neon-vfpv3`,
  `-mtune=cortex-a72`, `-fstack-clash-protection`) as static AIF programs:
  `ffmpeg`, `ffprobe`, `ffplay`.
- With x264, dav1d 1.5.4, LAME 3.100, Opus 1.5.2 and Vorbis 1.3.7.
  `ffplay` uses riscos-mesa's SDL2 overlay without GL.
- **Alignment:** the NEON assembly of FFmpeg, dav1d and x264 is rewritten
  at build time so it can't fault with RISC OS's alignment checking on.
  The ARMv6 media routines are not used, and a few core-ARM loads and
  stores are patched (docs/ALIGNMENT.md). It was checked under a QEMU that
  traps like RISC OS: all three checkasm suites and 110 decode/encode jobs
  pass.
- **RISC OS file names** work for input and output (`SDFS::…`,
  `<Var$Dir>.…`), and a `/ext` suffix picks the output format.
- **Memory:** the heap is in the "FFmpeg Heap" dynamic area (512 MB
  maximum).
- **ffplay** converts frames to the screen's own 32bpp format with
  swscale's NEON code.
- **Bench:** an Obey file that compares NEON with plain C on your machine.
- Not tested on RISC OS hardware yet.
