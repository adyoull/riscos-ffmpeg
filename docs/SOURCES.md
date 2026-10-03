# Upstream sources

Every file in `dl/` is checked against `build/SHA256SUMS` before it is
unpacked.

| Package | Version | File in `dl/` | Where from |
|---|---|---|---|
| FFmpeg | 5.1.10 (tag n5.1.10, commit 19feb712f5c1) | ffmpeg-5.1.10.tar.xz | `git archive` of the tag from github.com/FFmpeg/FFmpeg (ffmpeg.org is not reachable from the build machine) |
| x264 | 0.165.3222 (git b35605ac, "stable") | x264_0.165.3222+gitb35605ac.orig.tar.gz | archive.ubuntu.com (Ubuntu's orig tarball) |
| dav1d | 1.5.4 | dav1d_1.5.4.orig.tar.xz (+ .asc) | archive.ubuntu.com (= videolan's release tarball; signature included) |
| LAME | 3.100 | lame_3.100.orig.tar.gz | archive.ubuntu.com; sha256 ddfe36ca… = the SourceForge release |
| Opus | 1.5.2 | opus_1.5.2.orig.tar.gz | archive.ubuntu.com; sha256 65c1d2f7… = the xiph.org release |
| libogg | 1.3.5 | libogg_1.3.5.orig.tar.gz | the Warzone 2100 port's dl-cache |
| libvorbis | 1.3.7 | libvorbis_1.3.7.orig.tar.gz | the Warzone 2100 port's dl-cache |
| SDL | 2.26.0 | SDL-2.26.0.tgz | the Warzone 2100 port's dl-cache (= libsdl-org tag release-2.26.0) |
| SDL RISC OS overlay | riscos-mesa 4f859d5 | patches/sdl2/*.p | riscos-mesa `tools/sdl-overlay-export.sh` (riscos-mesa is the authoritative copy) |
| zlib | 1.3.1 | (library) | the riscos-mesa devkit 20.3.5-7pre12 |
| EGL, OSMesa (GL), GLU, freeglut | riscos-mesa 20.3.5-7pre12 (70ebe8e) | (libraries) | the riscos-mesa devkit 20.3.5-7pre12, md5 95059f254a5ab98affe31fb02d774db5 (EGLImage) |

Test-only (not in any release):

- **checkasm v1.2.0** (commit 0df02535), for dav1d's tests: from
  github.com/haasn/checkasm (its author's copy of the
  code.videolan.org/videolan/checkasm wrap).
- **QEMU 8.2.2**: archive.ubuntu.com `qemu_8.2.2+ds.orig.tar.xz`.

Toolchain: GCCSDK GCC 10.2.0 Release 2 (riscos-warzone2100), built from
GCCSDK 64c6f81 (`Warzone2100/dist/gccsdk-gcc10.2-x86_64-linux-env.tgz`, md5
caea90c3cc14abb4387f2c8d2e009355), with its UnixLib replaced by
**riscos-reelhwaccel devkit** (https://github.com/adyoull/riscos-reelhwaccel,
GPL version 2, the same licence as Linux's vchiq-mmal driver: it contains
none of the driver's code, but having used it as its reference it is
treated as derived from it. A
new implementation over RISC OS's VCHIQ module with its own MMAL client,
based on the MMAL message formats as that driver's headers define them):
`vcdec.h` and `libvcdec.a` in `third_party/reelhwaccel`,
which `build/build-ffmpeg.sh` copies into the stage and builds FFmpeg
with `--enable-vchiq` (patch 0021, `h264_vchiq`, also from that devkit;
patch 0022, its `drop_before` option, is ours);
devkit 0.2.8 (sha256 111f82b5…; also `hwhevcdec.h`, `hevc_ctrls.h` and
`libhevcdec.a`, with `--enable-libhevcdec` and patch 0023, `hevc_hwdec`);
`third_party/reelhwaccel/SOURCE.md` has
the full sha256.
Without it, FFmpeg is built without h264_vchiq.

**UnixLib 5.0.3.1** from github.com/adyoull/riscos-unixlib (an unofficial
fork of GCCSDK's UnixLib; release v5.0.3.1: `libunixlib.a` sha256
fa98152f…0658 into `arm-riscos-gnueabihf/lib/`; its installed headers are
5.0.2's, unchanged (5.0.3.1's sched.h differs by a comment, and is the same
as rc8's). 5.0.3.1 is in riscos15 and Reel 0.1.22. 5.0.3.1-rc8's library, sha256
44cb5481…, was used for the ulrc8 test builds. 5.0.3's library, sha256 761305fa…,
was used for the ul503 test builds. 5.0.2's library, sha256
bcd01280…2254, was used for riscos13, riscos14 and Reel 0.1.20, 0.1.21), and
its changed headers, `sched.h`, `unistd.h`, `sys/stat.h` and
`sys/mman.h`, from the tag, into `arm-riscos-gnueabihf/include/`), from
riscos13 and Reel 0.1.20: files over 2GB (up to 4GB-1) for programs built
with `-D_FILE_OFFSET_BITS=64`, which FFmpeg's configure adds and
build-apps.sh gives the apps; FFmpeg was rebuilt from clean against the
new headers. riscos10 to riscos12 and Reel 0.1.16 to 0.1.19 used UnixLib
5.0.1 (sha256 bf0e9709…a7a6; sched.h and unistd.h). `tools/check-unixlib.sh` (run by package.sh) checks every program
was linked with it. Its module PThreadTicker (0.03 since riscos15 and Reel
0.1.22; 0.01 at first) is in
`third_party/pthreadticker` and goes into each app. riscos3 to riscos9
used the Warzone toolchain's own UnixLib (the ticker code copied to the
RMA, a 248-byte block); riscos1 and riscos2 an earlier one without the
ticker fix.
