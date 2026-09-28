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
**UnixLib 5.0.1** from github.com/adyoull/riscos-unixlib (release v5.0.1:
`libunixlib.a` sha256 bf0e9709…a7a6 into
`arm-riscos-gnueabihf/lib/`, and its two changed headers, `sched.h` and
`unistd.h`, into `arm-riscos-gnueabihf/include/`), from riscos10 and Reel
0.1.16. `tools/check-unixlib.sh` (run by package.sh) checks every program
was linked with it. Its module PThreadTicker 0.01 is in
`third_party/pthreadticker` and goes into each app. riscos3 to riscos9
used the Warzone toolchain's own UnixLib (the ticker code copied to the
RMA, a 248-byte block); riscos1 and riscos2 an earlier one without the
ticker fix.
