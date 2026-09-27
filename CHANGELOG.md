# Changes

## Reel 0.2 (2026-09-27)

- **!ReelEGL:** the same player (`player/reel.c` built with `-DREEL_EGL`),
  drawing through riscos-mesa's EGL. It's a working example of
  `ffegl_draw_surface` in a real program.
  - In the window, the picture is a work-area EGL surface (fixed size, top
    of the work area) above the ordinary Wimp icons. Frames arrive by
    `EGL_KHR_lock_surface` and `eglSwapBuffers`; redraws go through
    `eglPlotSurfaceRISCOS` in the task's own redraw loop.
  - Full screen uses the whole-screen surface. It's direct into screen
    memory by default (`EGL_SINGLE_BUFFER`, "Direct" on the window menu,
    `ReelEGL$NoDirect`), otherwise a sprite plotted after vsync.
  - It links riscos-mesa's EGL and OSMesa: 28.8 MB text.
  - It passes the same scripted test as Reel, against a fake EGL: the
    frames shown, work-area surface sizes, direct full screen, no surfaces
    left over.
- Reel: the picture is refreshed only when the window is actually resized
  (not on every move), and the drawing sits behind a small internal layer
  shared by both builds.
- ffegl: `FFEGL_NO_TEXTURE` (EGL surfaces without the GL texture code).
- Reel 0.1's File info said "no sound" for Big Buck Bunny 720p 10s 30MB.
  That's correct: the file has no sound track.

## Reel 0.1 (2026-09-27)

- **!Reel, a video player for the desktop** (`player/reel.c`,
  `dist/Reel-0.1.zip`). It's a native Wimp app built on ffegl: an icon
  bar icon, and a window with the picture above a row of controls.
  - Controls: Play/Pause, back and forward 10 s, a position bar you can
    click, the time, and Full. Keys: Space, the arrow keys, F, Escape
    and Q.
  - Open a video by dropping it on the icon or the window. Double-clicking
    in the Filer also works while Reel is loaded, for filetypes MimeMap
    calls video/*.
  - The picture is scaled into the window by swscale (NEON), letterboxed,
    and plotted as a 32bpp sprite in the screen's colour order. The window
    can be resized.
  - Full screen is a borderless window over the whole screen.
  - Sound goes through SDL2 (SharedSoundBuffer) and is the clock. There
    are no threads of its own: decoding happens on null events.
  - The window menu has File info (codec, size, fps, sound, frames
    skipped), Full screen, Loop and Close. The icon menu has Info, Loop
    and Quit.
  - It uses no EGL or OpenGL (ffegl built with `FFEGL_NO_GL`), so it's a
    20 MB program.
  - **Test:** `tests/host/reel_test.c` drives the whole app through a
    scripted fake desktop, with real decoding under the alignment-trapping
    QEMU. It checks playing (the plotted pixels are the frame),
    pause/resume, the position bar, full screen and back, resize, a second
    file, a directory refused, and double-click claiming only video types.
- ffegl: `ffegl_info()` (a description of the streams),
  `ffegl_dropped_frames()`, and the `FFEGL_NO_GL` build option.

## 5.1.10-riscos5 (2026-09-27)

- **An icon bar icon:** double-clicking `!FFmpeg` now puts the FFmpeg icon
  on the icon bar, as well as setting up the commands.
  - Drop a video or sound file on the icon and it plays in ffplay. Each
    file gets its own window and task, and messages go to
    `<Wimp$ScrapDir>.ffplay/log`.
  - The icon's menu has Info, Full screen (plays with `-fs`) and Quit.
  - It's a small C program (`frontend/fffront.c`, `!RunImage`, 512K); only
    one copy runs. It has a host test with a scripted fake Wimp.
  - Repackaged the same day: the first version said "File name '.Task' not
    recognised" when `!FFmpeg` was started from the boot sequence
    (Configure > Boot > Look at). It used `FFmpeg$Dir` when a file was
    dropped, and that can hold a reference like `<Obey$Dir>` which by then
    points elsewhere. It now works out its full directory when it starts
    (`OS_FSControl` 37, else its own command line).

## 5.1.10-riscos4 (2026-09-27)

- **The real cause of the Pi crash** ("abort on data transfer", which
  `*Where` placed in VFPSupport; an EMT trap in `hl_motion` in the log):
  - FFmpeg's `ff_put_h264_chroma_mc2_neon` copied 2x2 chroma pixels with
    `ldrh` from a source that can be at an odd address. That happens in
    H.264 4x4 sub-partitions at whole-pixel chroma positions.
  - Big Buck Bunny 720p hits it within the first frames; the old test clips
    never used 4x4 partitions.
  - Byte loads now (patch 0014). Single-threaded ffmpeg crashed the same
    way, so it was never a thread or TaskWindow problem.
- **New "motion" test clips** (`tests/qemu/samples/mv_*`). They scroll and
  zoom, and are encoded with every partition size, B-frames, weighted
  prediction, MBAFF, CAVLC, 10-bit and 4:2:2 (H.264), plus HEVC, VP8, VP9,
  AV1, MPEG-4 qpel/4MV, interlaced MPEG-2, Theora, WMV2 and H.263.
  - All decode under the alignment-trapping QEMU with NEON output equal
    to C.
  - The fixed mc2 was compared with an x86 ffmpeg frame by frame.
- riscos3's change (switching the VFP context off around `Wimp_StartTask`)
  stays: UnixLib does the same, and it's correct. But it wasn't this
  crash.

## 5.1.10-riscos3 (2026-09-27)

- **ffplay from a TaskWindow crashed on the Pi** ("Internal error: abort on
  data transfer" in VFPSupport; the new ffplay's log showed an EMT trap in
  its video decoder thread). *Note (riscos4): the explanation below was
  wrong; the crash was the unaligned `ldrh` fixed in riscos4.* The ffplay in the TaskWindow started the new
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
