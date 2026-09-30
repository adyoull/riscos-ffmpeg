# hwhevc - hardware HEVC decoding on the Raspberry Pi 4

Work towards Reel decoding HEVC (H.265) with the Pi 4's own decoder block
(the one Linux drives as "rpivid"), one step at a time.

- Step 1, `tools/hevcprobe`: can RISC OS reach the block? Yes: it answers
  VERSION &202 with its clock on (250 MHz on the Pi tested).
- Step 2, `hwhevc/module` (this): a module, HEVCHW, that checks writing
  the registers, memory the block can use, and the block's interrupt at
  RISC OS: `*HEVCInfo`, `*HEVCRegTest`, `*HEVCMemTest`, `*HEVCIRQTest`.

## Building

    hwhevc/build.sh [VERSION]      # dist/HEVCHW-VERSION.zip
    hwhevc/module/build.sh [DIR]   # just HEVCHW,ffa

The module is freestanding C (no C library; every SWI goes through
`hw_swi` in `header.s`) compiled position independent with the Linux ARM
cross compiler, linked at 0 with the header first and written out as a
flat binary. The build fails if anything would need relocating.

## Tests

`tests/host/hevchw_test.c` (in `tests/host/run.sh`) runs the module's C
with the real `header.s` veneers and IRQ handler under qemu, against a
fake RISC OS, firmware, HAL and memory standing in for the block, its
interrupt control and the GIC.

## What it relies on

The facts, and where each comes from, are in `hw.h`: the block at
&FEB00000 and its interrupt control at &FEB10000, GIC_SPI 98 (ID 130),
firmware clock 11. The RISC OS device number (34) is inferred from the
V3D module's use of device 10 for GIC_SPI 74, which is why
`*HEVCIRQTest` checks the line at the GIC before claiming anything.
