# Changes

## 5.1.10-riscos3 (2026-09-27)

- **ffplay from a TaskWindow crashed on the Pi** ("Internal error: abort on
  data transfer" in VFPSupport; the new ffplay's log showed an EMT trap in
  its video decoder thread). The ffplay in the TaskWindow started the new
  task with its own VFP (floating point) context still active. VFPSupport
  later saved that context to its old stack address, which by then
  belonged to the new ffplay, and corrupted it. ffplay now switches its
  context off around `Wimp_StartTask`, as UnixLib does before running
  another program (patch 0013).
- **Built with the fixed UnixLib from the Warzone 2100 port's toolchain**
  (caea90c3): the pthread ticker's code is in the RMA, so a thread switch
  can't jump into ffplay's memory while another task is paged in. ffplay
  runs several threads (reading, decoding, SDL's timer). Warzone 2100 hit
  exactly that abort.

## 5.1.10-riscos2 (2026-09-27)

- **`ffmpeg -f egl`:** a new output device that shows video through
  riscos-mesa's EGL, in a desktop window or the whole screen (optionally
  straight into screen memory). swscale converts and scales each frame
  into the surface in one pass.
- **ffegl:** a library for EGL and OpenGL programs. It plays a file with
  sound (SDL2 audio is the clock) into an EGL surface, an OpenGL texture
  or any 32bpp memory, and has pause, seek and loop. It is in the devkit
  (`ffegl.h`, `libffegl.a`), with two examples in
  FFmpeg-EGL-examples: `videowin` and `videocube`.
- **Textures without a copy:** with riscos-mesa 20.3.5-7pre12 or later,
  `ffegl_texture()` binds a sprite to the texture through an EGLImage
  (`EGL_KHR_image_pixmap`, `GL_OES_EGL_image`), and swscale writes each
  frame straight into it. Older riscos-mesa, or `FFEGL_NO_EGLIMAGE=1`,
  copies with `glTexSubImage2D`.
- **ffplay** converts and scales frames straight into its window surface,
  in the screen's format, instead of through an SDL texture and a second
  scale. `FFPLAY_SCALE` picks the scaler; `FFPLAY_RENDERER=sdl` gives the
  old path.
- **Fixes found by the new tests** (both also affect ffplay):
  - swscale's NEON YUV→RGBA store demanded 16-byte aligned
    destinations and would fault on RISC OS otherwise (patch 0009);
  - swscale's NEON converters reported 0 lines done, which callers took as
    failure (patch 0010).
- **ffplay in a TaskWindow:** riscos1 stopped with "Failed to create
  window or renderer: Wimp_Initialise failed: Window Manager is currently
  in use", because a TaskWindow can't open windows. ffplay now starts
  itself as a desktop task of its own (through the new `Task` Obey file,
  which sets the WimpSlot), with its messages in
  `<Wimp$ScrapDir>.ffplay/log`; `FFPLAY_NEWTASK=0` turns this off
  (patch 0013). `-f egl` gives a clear error there instead of taking over
  the screen (patch 0012).
- `ffmpeg -version` now says `5.1.10-riscos2` rather than a git hash.
- The programs are larger (about 31MB) because they include riscos-mesa's
  EGL and OpenGL; the WimpSlot is now 44MB.
- Built with the riscos-mesa 20.3.5-7pre12 devkit.

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
