# NEON on RISC OS

How this port uses ARM's NEON (the SIMD unit of ARMv7: Raspberry Pi 2 and
later, Titanium, IGEPv5, ...), why NEON code written for Linux breaks on
RISC OS, what was done about it, the NEON added here, and how to keep all
of it working. It's written for anyone porting or maintaining codec code
on RISC OS, not only this project.

- `docs/ALIGNMENT.md`: the details of making FFmpeg's, dav1d's and x264's
  own assembly safe (the rewrite tool, the patches, the allow lists).
- `tests/qemu/README.md`: the test rig that finds unaligned accesses.

## Contents

1. [Why NEON code breaks on RISC OS](#1-why-neon-code-breaks-on-risc-os)
2. [Making existing NEON safe](#2-making-existing-neon-safe)
3. [The NEON added by this port](#3-the-neon-added-by-this-port)
4. [Rules for writing NEON for RISC OS](#4-rules-for-writing-neon-for-risc-os)
5. [Testing](#5-testing)
6. [Maintaining it](#6-maintaining-it)
7. [Where everything is](#7-where-everything-is)

## 1. Why NEON code breaks on RISC OS

### Alignment checking is on

On ARMv7 an access can be *unaligned*: a 4-byte load from an address that
isn't a multiple of 4. Linux turns the CPU's alignment checking off
(SCTLR.A = 0), so the hardware simply handles it. RISC OS 5 keeps it on
(SCTLR.A = 1): older ARMs gave "rotated" results for unaligned word loads,
and old RISC OS software relied on that, so an unaligned access is made to
fault rather than silently give a different answer. The program stops
with "Abort on data transfer" (or UnixLib's "EMT trap").

With checking on, what faults is:

| instruction | faults unless the address is a multiple of |
|---|---|
| `vld1.8` / `vst1.8` (any form) | never faults |
| `vld1.16` / `.32` / `.64`, `vst1…`, `vld2/3/4…` | the element size: 2, 4 or 8 |
| any of those with a qualifier, `[r0, :128]` | the qualifier (16 bytes here); on Linux too |
| `ldrh` / `strh` | 2 |
| `ldr` / `str` / `ldm` / `stm` | 4 |
| `ldrd` / `strd` | 4 (use two `ldr`s if in doubt) |

FFmpeg's, dav1d's and x264's ARM assembly is written and tested on Linux,
so it uses `vld1.32`, `vst1.64` and `ldrh` freely on byte pointers
(pixels, bitstreams). Those only fault for some addresses, which depend
on the picture's width, the block sizes and the motion vectors, so code
can pass simple tests and crash on a real film. That's what happened
here (H.264's chroma `ldrh`, found by a Pi playing Big Buck Bunny), and it
is almost certainly why other RISC OS builds of FFmpeg leave NEON out.

C is safe: GCCSDK's GCC compiles with `-mno-unaligned-access`, so the
compiler never emits an unaligned access on its own. Its NEON
intrinsics on typed data (`int16_t *`, `float *`) are aligned to their
element. The problem is hand-written assembly.

### VFP and NEON registers are shared

VFP and NEON use the same registers. On RISC OS every program that uses
them has a context with the VFPSupport module, switched lazily. UnixLib
makes one for each program at start-up, on its stack. When one of our
programs starts another (`Wimp_StartTask`), its context must be switched
off around the call, or VFPSupport can later save it into the other
program's memory (riscos3's crash; `fffront.c` and patch 0013 show the
fix: `XVFPSupport_ChangeContext 0`, `Wimp_StartTask`, then back, in one
asm block so the compiler can't put a VFP instruction between them).

### Build flags

GCCSDK GCC 10 (hard float): `-march=armv7-a -mfpu=neon-vfpv3
-mtune=cortex-a72`. Plain `-mfpu=neon` makes GCCSDK add `-mcpu=cortex-a8`.
FFmpeg's configure: `--enable-neon --enable-vfp --disable-fast-unaligned
--disable-runtime-cpudetect` (there's no CPU detection on RISC OS: the
features are the compile-time ones).

## 2. Making existing NEON safe

In short (`docs/ALIGNMENT.md` has the details):

- **`tools/neon-align.py`** runs over every `.S` file after the patches
  are applied (`build/build-ffmpeg.sh` does it). It rewrites each NEON
  load/store with an element wider than 8 bits and no qualifier into a
  form that can't fault: whole registers become `.8` (the same bytes on
  little-endian, at no cost); a lane becomes byte lanes; a "dup" becomes
  byte lanes and `vdup`. Counts: FFmpeg 508 instructions, dav1d 477,
  x264 282. What it can't decide (de-interleaving loads, register lists
  from macro arguments) it reports, and a person checks it and records the
  decision, with the reason, in `tools/neon-align-*.allow`.
- **Patches** for what the tool doesn't cover: core `ldr`/`ldrh` on byte
  pointers (0005, 0006, 0014), a qualifier that didn't hold for RISC OS
  callers (0009), macros whose callers pass lanes (0008), and more in
  `patches/dav1d` and `patches/x264`.
- **`tools/scan-neon.py`** disassembles a finished binary and lists what
  could still fault, as a last check.

## 3. The NEON added by this port

Three pieces of FFmpeg that were plain C on 32-bit ARM. Each is **bit-exact
with FFmpeg's C**: the same output bytes for every input, not merely
close. That makes testing decisive (any difference is a bug) and means
the NEON can't change what anyone sees.

| patch | what | file | C it replaces |
|---|---|---|---|
| 0015 | yadif deinterlacing, the line filter | `libavfilter/arm/vf_yadif_neon.S`, `vf_yadif_init.c` | `filter_line_c` (vf_yadif.c) |
| 0016 | HEVC chroma motion compensation: epel h, v, hv; plain, uni, bi; widths 2-64 | `libavcodec/arm/hevcdsp_epel_neon.S`, wrappers in `hevcdsp_init_neon.c` | `put_hevc_epel*` (hevcdsp_template.c) |
| 0017 | swscale's fast bilinear scaling to RGB32: horizontal scalers and the yuv2rgbx32 output | `libswscale/arm/scaled_rgb_neon.S`, wrappers in `arm/swscale.c`, constants from `yuv2rgb.c` | `ff_hyscale_fast_c`, `ff_hcscale_fast_c`, `yuv2rgbx32_1/2_c` |

Each `.S` file starts with a full description: the C it mirrors, how the
NEON does it, **why the arithmetic is exact**, its contract with the
caller (what it may read or write past the end, which widths it takes),
and a map of the registers. A summary:

- **yadif (0015):** 8 pixels an iteration. The two lines around each
  missing one are loaded once, and `vext` makes the seven shifted views
  the edge search needs. Averages of bytes are `vhadd.u8` (exact), scores
  16-bit. The C's nested `if (score < best)` becomes masks and `vbit`
  selects, the inner check ANDed with the outer one. Like FFmpeg's x86
  versions it may write up to 7 pixels past the width it's given, which
  vf_yadif.c leaves room for.
- **HEVC epel (0016):** every epel filter has signs (-, +, +, -), so the
  first pass is unsigned 8 x 8 multiply-accumulates with the outer taps
  subtracted: the true sum always fits 16 bits. hv's second pass is
  16 x 16 -> 32 bits. Rounding and clipping are one `vqrshrun`. For
  bi-prediction the add is saturating (`vqadd.s16`); that is exact
  because wherever it saturates, the C's result is clipped to 0 or 255
  anyway. Columns go in strips of 8; the last strip of a 2, 4 or 6 wide
  block stores only those columns.
- **swscale to RGB32 (0017):** NEON has no gather load, so the horizontal
  scaler loads 16 bytes and picks each output's two with `vtbl`, which
  covers scales down to about 1.85x (the C does the rest). The RGB output
  in C looks colours up in tables, which NEON can't do at that size, but
  the tables are built by arithmetic, so the arithmetic is done directly
  instead:
  `R = clip((Y*cy + kR + ((clip(V)*crv) >> 16)*cy) >> 16)`, and the same
  for G and B. `ff_yuv2rgb_c_init_tables` now also stores those constants
  (`SwsContext.yuv2rgb_arith`) and says where they fit 32 bits (`_ok`);
  saturating narrows (`vqshrun`, `vqmovn`) are the clip.

### Outside FFmpeg: reelcore's halving

reelcore (the player core under Reel) has one small piece of NEON of its
own, written with GCC's `arm_neon.h` intrinsics rather than assembly:
`reelcore_halve_plane`, which halves a picture plane by averaging each
2x2 block, `(a + b + c + d + 2) >> 2`. It is used for big reductions (a
1280-pixel video in Reel's 320-pixel mini player): while the picture is
at least twice the size wanted both ways it is halved first, and swscale
does what's left. The fast bilinear scaler alone would skip three pixels
in four at a quarter size, which looks jagged; halving twice is a 4x4 box
filter (as good as swscale's area filter) and, by instruction count, costs
about what the scaling it replaces did. Eight output pixels are two `vld1.8` loads, `vpaddl.u8`,
`vpadal.u8`, `vrshrn.i16 #2` and a `vst1.8`; the C loop does the rest of
a row, and without NEON all of it. Byte loads and stores never fault on
RISC OS whatever the address, and the compiler adds no alignment
qualifiers here (checked with objdump). `tests/host/halve_test.c`
checks it is byte for byte the C, and what it does to whole pictures.

### Performance

Measured under qemu nothing is meaningful: qemu emulates each NEON
instruction slowly, so NEON can even look slower than C there. The
numbers that count come from the hardware:

- **Reel**: Media info's stats rows, and the log's once-a-second line
  "ms a picture: decode, convert (to WxH), draw, deinterlace", in full
  screen too. Compare with an older Reel, or the same file 1:1 against
  resized.
- **ffmpeg**: `-benchmark`, with and without `-cpuflags 0` (all NEON
  off), for example
  `ffmpeg -benchmark -i clip/mkv -vf yadif -f null -`.

Expected from the instruction counts (to be confirmed on a Pi): yadif and
HEVC chroma several times faster than their C, and the resized
conversion to RGB 2-3x.

## 4. Rules for writing NEON for RISC OS

The code in section 3 follows these; they're what makes it both safe and
easy to check.

1. **Use `.8` elements for loads and stores of pixel data**: `vld1.8
   {d0-d1}, [r0]` loads the same bytes into the register as `vld1.16` or
   `.32` would (on little-endian ARM), and never faults. Byte lanes
   (`vst1.8 {d0[3]}, [r0]`) likewise.
2. **A wider element only where the alignment is certain, and then say so
   with a qualifier**: `vst1.16 {d16[0]}, [r5, :16]` for an `int16_t`
   destination. The qualifier costs nothing, documents the promise in the
   instruction, and `tools/neon-align.py` leaves it alone (it's a
   promise: it would fault on Linux too if broken). Never add a qualifier
   for data whose alignment depends on the caller: FFmpeg's own buffers
   are aligned, but a RISC OS sprite, window surface or EGL surface at an
   odd x isn't (patch 0009).
3. **Core loads too**: `ldr`/`ldrh`/`ldrd` only on addresses known to be
   aligned (arguments on the stack, C structures). Bytes otherwise.
4. **Check with the tool**: `python3 tools/neon-align.py --check file.S`
   must report "0 instructions to rewrite" for new code: then the build's
   own pass leaves it exactly as written and tested.
   `tools/check-fresh-tree.sh` checks this for the port's files.
5. **Bit-exact with the C**, and a test that compares them on many
   inputs, including extremes (all 0, all 255, random) and every width
   and alignment, under the trapping qemu (section 5). If exactness needs
   an argument (saturation, overflow), write it in the file's header.
6. **The caller's contract** in the header: how far past the end the
   function may read or write, and what the C caller guarantees that
   makes that safe.
7. **ARM calling convention**: d8-d15 are callee-saved (`vpush`/`vpop` if
   used); by-scalar operands must be d0-d7 for 16-bit elements and d0-d15
   for 32-bit ones; count the pushed registers when reading stack
   arguments, and say so in a comment.
8. **Keep the C wrappers thin** and make offsets checkable: a structure
   the assembly reads at fixed offsets gets a `_Static_assert` on those
   offsets next to its declaration (as `YUV2RGBX32Args` in
   `libswscale/arm/swscale.c` has).

## 5. Testing

Everything below runs on Linux, built for `arm-linux-gnueabihf` with the
same sources and patches, under QEMU patched to trap unaligned accesses
the way RISC OS does (`tests/qemu`). `tests/host/run.sh` runs it all;
`tests/qemu/run-all.sh` adds FFmpeg's, dav1d's and x264's own assembly
tests (checkasm: FFmpeg 962/962; dav1d 2039/2039 and x264's, unchanged
since riscos2).

| test | what it proves |
|---|---|
| `tests/host/yadif_test.c` | ff_yadif_filter_line_neon = filter_line_c on 56,064 lines: every mode and parity, first/middle/last line, widths 1-70 and wide, every alignment, random and extreme pictures; nothing written outside the line |
| run.sh "yadif, whole pictures" | `ffmpeg -vf yadif` gives the same bytes with NEON, with `-cpuflags 0` and on x86 FFmpeg |
| `tests/host/hevc_epel_test.c` | every epel function (h/v/hv x plain/uni/bi x 10 widths x 7 fractions x random heights and alignments, typical and extreme src2) = the C; the whole destination buffer compared |
| run.sh "HEVC decodes" | HEVC files decode to the same pictures with NEON, with `-cpuflags 0` and on x86 |
| `tests/host/swscale_rgb_test.c` | the scalers and yuv2rgbx32 = the C over 15 scales x 8 colour settings (BT.601/709, both ranges, brightness, contrast, saturation), and whole pictures in the same context with and without the NEON; it also fails if the NEON isn't actually in use |
| `tests/host/halve_test.c` | reelcore_halve_plane = `(a + b + c + d + 2) >> 2` for widths 1-80 at every alignment and with a negative pitch (2,600 cases, nothing written outside); reductions of 4x and 3x halve twice and once; a quarter-size picture's brightness is within 42 dB of the 4x4 average of the full-size one (fast bilinear alone: 32 dB) |
| `tools/check-fresh-tree.sh` | the patches, applied to a fresh FFmpeg, reproduce exactly the tested code, and the port's NEON is alignment-safe as written |

**Each test was shown to catch mistakes** (mutation testing): a deliberate
bug planted in the NEON (a wrong shift, `vorr` for `vand`, a wrong lane,
a missing `- 1`) makes it fail with hundreds or thousands of
differences. When you change a test, do the same: plant a bug, see it
fail, take it out.

The qemu timings the tests print are **not** speed measurements (see
Performance above).

## 6. Maintaining it

### Changing one of the port's NEON files

1. Edit it in a git tree of FFmpeg with the patches as commits, not in
   `src/`: the patches are the source of truth. To make one:
   `git clone -b n5.1.10 https://git.ffmpeg.org/ffmpeg.git` (or unpack the
   tarball and `git init`), then `git am patches/ffmpeg/*.patch`.
2. `python3 tools/neon-align.py --check` on it: 0 to rewrite.
3. Rebuild the test copy (`LINUX_ARM_TEST=1 build/build-ffmpeg.sh`) and
   run its test (`QEMU=... tests/host/run.sh`, or the single test). Plant
   a bug once to see the test still catches it.
4. Commit into the right patch (amend that commit), then
   `git format-patch` the series into `patches/ffmpeg/` and run
   `tools/check-fresh-tree.sh`.
5. If only comments changed, compare the machine code: assemble the old
   and new file and `objdump -d` both; they should be identical.

### Updating FFmpeg (or dav1d, x264)

1. Rebase the patch series onto the new release. Where FFmpeg changed a
   function the port replaced, compare the C again (the tests keep a copy
   of the C only in `yadif_test.c`: update it with vf_yadif.c's `FILTER`
   and `CHECK`; the others compare with FFmpeg's own C at run time).
2. Upstream may have added NEON for the same things: prefer theirs if it
   is exact and passes the tests, and drop the patch.
3. Run `tools/neon-align-apply.sh tools/neon-align-ffmpeg.allow --check`
   over the new `.S` files: new "need a look" lines need a decision,
   recorded with a reason in the allow list.
4. Run the whole test rig, then `tools/scan-neon.py` on the RISC OS
   binary.

### Offering it upstream

Patches 0015-0017 are ordinary 32-bit ARM NEON with nothing
RISC OS-specific, so they could go to FFmpeg. For that they'd want
checkasm tests (FFmpeg's test framework) in place of `tests/host`, and to
be rebased onto current FFmpeg, where 32-bit ARM matters less but the
same C functions still exist. The alignment work (sections 1-2) is useful
to anyone running with alignment checking on.

## 7. Where everything is

| what | where |
|---|---|
| the port's NEON | `patches/ffmpeg/0015-*` (yadif), `0016-*` (HEVC epel), `0017-*` (swscale); applied: `src/ffmpeg-5.1.10/lib*/arm/` |
| the alignment rewrite | `tools/neon-align.py`, `tools/neon-align-apply.sh`, `tools/neon-align-*.allow` |
| checking a binary | `tools/scan-neon.py` |
| checking the patches reproduce the tested code | `tools/check-fresh-tree.sh` |
| the test rig | `tests/qemu/` (QEMU patch, samples, run-all.sh), `tests/host/` (the tests above) |
| where it's used | Reel (`reelcore/`: deinterlacing and conversion), ffplay's drawing (patch 0003), `ffmpeg -vf yadif`, HEVC decoding everywhere |
