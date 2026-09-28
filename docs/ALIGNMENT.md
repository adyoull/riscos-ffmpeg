# Unaligned access and the ARM assembly

(The overview, the NEON added by this port, rules for new NEON and how to
maintain it all: `docs/NEON.md`. This file is the detail of making the
codecs' own assembly safe.)

## The problem

RISC OS 5 runs ARMv7 CPUs with alignment checking on (SCTLR.A = 1). An
access whose address is not a multiple of its size aborts: "Internal error:
abort on data transfer". RDPClient hit this with 16-bit globals (see its
notes), and Warzone 2100 with `ldrh`/`ldr`. GCCSDK's GCC passes
`-mno-unaligned-access`, so compiled C is safe. Hand-written assembly is
not.

Linux runs ARMv7 with checking off. There, `ldr`, `ldrh` and NEON
`vld1.16`/`.32`/`.64` work at any address, and FFmpeg's, dav1d's and
x264's arm32 assembly uses them on byte (pixel and bitstream) pointers.
With checking on, a NEON load or store faults unless its address is a
multiple of the element size. There are two exceptions:

- `.8` never faults;
- an explicit qualifier (`[r0, :64]`) promises the address is aligned, so
  it faults on Linux too if it isn't. Code that uses a qualifier has
  already made sure the address is aligned.

The first H.264 file tried under the test rig faulted in
`ff_h264_h_loop_filter_luma_neon` (`vst1.32 {d8[0]}, [r0], r1`, r0 = …022).
The PackMan FFmpeg avoids all of this by being built for ARMv4 with no
NEON at all.

## The fix

### 1. `tools/neon-align.py` (build time)

It runs over every `.S` file after the patches are applied. For each
`vldN`/`vstN` whose element is wider than 8 bits and that has no alignment
qualifier:

| form | example | rewritten as | cost |
|---|---|---|---|
| whole registers | `vld1.64 {d0-d1}, [r1], r2` | `vld1.8 {d0-d1}, [r1], r2` | none |
| one lane (vld1–4) | `vst1.32 {d8[1]}, [r0], r1` | 4 × `vst1.8` byte lanes + `sub r0, r0, #3` | a few cycles |
| all lanes ("dup") | `vld1.16 {d16[], d17[]}, [r3]!` | byte-lane loads, `vdup.16`, `vmov` | a few cycles |

Notes on the rewrite:

- **Whole registers.** A little-endian `vld1` puts element *i* in lane *i*
  whatever the element size, so `.8` gives the same register contents.
- **Lanes.** The byte lanes 4k…4k+3 are exactly lane k's bytes. The base
  register ends up where the original addressing mode left it.
- **Macros.**
  - An element size that is a macro argument (`vld1.\wd`) gets one
    branch per size (`.if \wd == 16 …`).
  - An alignment qualifier that is a macro argument (`[r2\align]`) keeps
    the original under `.ifb \align` / `.else`.
- **What the tool won't guess**, and lists (exit status 2) unless it is in
  `tools/neon-align-*.allow`:
  - whole-register vld2/3/4 with wide elements (they de-interleave);
  - register lists that are macro arguments (`{\regs}`: whole registers
    or a lane?).

  Each entry in the allow lists was checked by hand. `whole FILE:LINE`
  means every caller passes whole registers. A plain `FILE:LINE` means the
  data is aligned to its element: 16-bit pixels, int16 coefficients,
  uint16 CDFs, int32 tables.
- **Callers that pass lanes through such a macro** were written out by
  hand so the tool can see them:
  - FFmpeg `ff_hevc_put_qpel_uw_pixels_w4/w12` (patch 0008);
  - (not a NEON instruction, but also asm) FFmpeg's H.264 chroma mc2
    `ldrh` from an odd source (patch 0014, byte loads);
  - x264 `pixel_avg_weight_w4_*` (`patches/x264`).

  FFmpeg's checkasm caught the first of these.
- **Alignment qualifiers** (`[r0,:128]`) are left alone by the tool: they
  fault on Linux too, so the code already guarantees them. One didn't
  hold for RISC OS callers: swscale's YUV→RGBA `vst4.8 {…}, [dst,:128]!`
  assumed a 16-byte aligned destination, which FFmpeg's own buffers are
  but an EGL surface, a window surface or a sprite at an odd x need not
  be. Patch 0009 drops the qualifier (the ffegl and egl output device
  host tests found it).

### 2. Core ARM loads and stores (patches)

**FFmpeg:**

- `libavutil/arm/cpu.c` (patch 0006) never reports ARMv6/ARMv6T2, so the
  ARMv6 media routines (`ldr` at byte addresses) are never picked. Every
  one of them has a NEON or C replacement. Inline asm is compile-time and
  unaffected: the CABAC reader and mathops don't touch unaligned memory.
  CABAC even keeps its 16-bit reads aligned on purpose.
- `vp56_arith.h` (0005) and `arm/vp8.h` (0006): the VP5/6/8/9 range coder's
  `ldrh` bitstream refill is replaced by C.
- swscale's `rgbx_to_nv12_neon` (0007) is left out. It crashes even
  unmodified (it reads past the source) and has nothing to do with
  alignment.

**dav1d** (`patches/dav1d`):

- `msac.S`: the bitstream refill reads 4 bytes one at a time instead of an
  unaligned `ldr`;
- `cdef.S`: the left/right padding store is `strh`, not `str` at an odd
  halfword address;
- `util.S`: big stack frames probe the guard page (the Windows code path).

**x264** (`patches/x264`): the ARMv6 4×4/4×8 SADs (unaligned `ldr`) aren't
used; the C versions run instead. NEON has no 4-wide SAD.

## How it was checked

`tests/qemu`:

- **The rig.** QEMU 8.2.2 linux-user with a small patch. It sets SCTLR.A
  (`QEMU_ARM_ALIGN_TRAP=1`), so it faults exactly as the hardware does:
  QEMU models the NEON element-alignment rule.
  `QEMU_ARM_ALIGN_IGNORE` exempts glibc, which isn't part of the RISC OS
  program.
- **The build under test.** The same sources, patches and rewrite are
  built for `arm-linux-gnueabihf` with `-mno-unaligned-access`, as GCCSDK
  does.

Results:

- **FFmpeg checkasm:** 962/962 tests pass (every arm32 DSP function it
  knows; 881 at riscos2).
- **dav1d checkasm:** 2039/2039 pass (built with `-Dtrim_dsp=false`).
- **x264 checkasm** (8 and 10 bit): "All tests passed".
- **`run.sh`:** 128 ffmpeg jobs.
  - Decoding (60 files, 14 of them `mv_*` motion clips): H.264 8/10-bit/4:2:2, HEVC 8/10-bit, VP8, VP9,
    AV1, MPEG-2/4, H.263, MJPEG, Theora, WMV2, ProRes, DV, FFV1, AAC,
    MP3, MP2, AC-3, E-AC-3, Vorbis, Opus, FLAC, ALAC, WMA.
  - Encoding: x264 8/10-bit, MPEG-4, MPEG-2, MJPEG, H.263, DV, ProRes,
    FFV1, HuffYUV, LAME, MP2, AC-3, AAC, Opus, Vorbis, FLAC, ALAC, SBC,
    TrueHD.
  - swscale (YUV→RGB in 8 formats, scalers) and swresample.
- **What the first test clips missed.** They were static `testsrc2`
  pictures, so H.264 never used 4×4 partitions. FFmpeg's
  `ff_put_h264_chroma_mc2_neon` then kept an `ldrh` from an odd source
  address in its whole-pixel path, and a Pi playing Big Buck Bunny found
  it (riscos2/3; patch 0014 loads bytes).
  - The `mv_*` clips scroll and zoom, and use every partition, B-frames,
    weighted prediction, MBAFF, CAVLC, 10-bit, 4:2:2, qpel/4MV and
    interlacing.
  - A new clip type that moves differently is the first thing to add if
    another decoder path is suspected.

  Each job runs twice (NEON, then `-cpuflags 0`). The outputs are equal
  packet for packet except where FFmpeg's NEON is not meant to be
  bit-exact:
  - float audio;
  - YUV→RGB, within 3 levels;
  - the IDCT that `-idct auto` picks. With `-idct simple` they are equal.

Before the fix, the same rig faulted on the first H.264, AV1 and VP8
files.

### A last look at the binary

`tools/scan-neon.py src/ffmpeg-5.1.10/ffplay_g` lists every function that
still has a NEON access with a wide element and no qualifier. In
5.1.10-riscos1 all of them are one of two kinds:

- compiled C: Opus's intrinsics, dav1d's C and FFmpeg's C, on int16,
  int32 or float arrays;
- assembly on the allow lists: dav1d's cdef, film grain, msac and 16-bit
  MC, FFmpeg's SBC input, and x264's SSIM sums.

## When updating FFmpeg, dav1d or x264

1. Apply the patches, then run
   `tools/neon-align-apply.sh tools/neon-align-X.allow --check <files>`.
   New "need a look" lines need a decision: fix the caller, add
   `whole`, or allow the line with a comment saying why.
2. Build with `LINUX_ARM_TEST=1` and run `tests/qemu/run-all.sh`.
3. Run `tools/scan-neon.py` on the RISC OS binary. Also look for new
   `ldr`/`ldrh`/`str` on byte pointers in any assembly the tests don't
   reach.
4. Run `tests/host/run.sh` (the port's own NEON: patches 0015-0017) and
   `tools/check-fresh-tree.sh` (the patches reproduce the tested code).
   `docs/NEON.md` section 6 has the rest.
