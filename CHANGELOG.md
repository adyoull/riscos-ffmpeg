# Changes

Reel's versions were renumbered 0.1.1–0.1.9 (they were 0.1–0.9; the
tags are `reel-0.1.1` … `reel-0.1.9`).

## 5.1.10-riscos6 (2026-09-27): first GitHub release

The same `ffmpeg`, `ffprobe` and `ffplay` as riscos5; what changed is
around them:

- **!FFmpeg:** `!Run`, `Task` and the EGL examples' `SetUp` load
  SharedSound, StreamManager and SharedSoundBuffer (they were only merged
  into `!System`, never loaded, so ffplay had no working sound device).
  The icon's menu has **Log** (ffplay's messages, now at verbose level,
  in `<Wimp$ScrapDir>.ffplay/log`).
- **ffegl** (in the devkit, and in the EGL examples `videowin` and
  `videocube`), as in Reel 0.1.5–0.1.9: sound straight to SharedSoundBuffer
  on RISC OS (no SDL audio thread); the file read for the sound
  separately from video decoding (the trailer freeze); frame skipping
  when behind; `ffegl_debug`, `ffegl_set_log`, `ffegl_media_info`,
  `ffegl_stats` and `ffegl_idle_time`.
- `COPYING` (GPL version 2) added.

## Reel 0.1.9 (2026-09-27)

- **Sleeps between pictures.** On the Pi, Media info showed ReelEGL
  decoding the 1280x544 trailer in 23% of the time (9.6 ms a picture,
  4.3x real time), converting and drawing in under 5 ms, yet Task
  Manager showed 56%: the rest was 5200 null events a second asking
  whether the next picture was due yet.
  - ffegl: `ffegl_idle_time()` says how long the caller may sleep after
    `ffegl_update()`: 0 while there's work (fewer than 3 pictures
    decoded, sound under 0.35 s queued, a picture due), otherwise the
    time to the next picture, at most 0.1 s.
  - Reel polls with `Wimp_PollIdle` until then (whole centiseconds, so it
    wakes on or just before time; the last few ms are polled as before).
    The next pictures are decoded before sleeping, so showing one on
    time needs only the wake-up.
  - Media info's Desktop line and the log show how much of the time Reel
    was asleep. `Reel$NoSleep` / `ReelEGL$NoSleep` polls flat out.
- Test: `idle_test` plays the same clip polling every 0.5 ms and
  sleeping as Reel does (waking up to 1 cs late at random, 8 ms a
  decode): the same 150 pictures, none late, worst 4.7 ms after its time
  (flat out: up to 5 ms early), with 607 wake-ups instead of 9633.
  reel_test checks Wimp_PollIdle is used.

## Reel 0.1.8 (2026-09-27)

- **Media info window** (window menu "Media info", or I), replacing the
  File info error box, which stopped the picture while it was open. It
  stays open while playing and follows the file being played.
  - At the top, "stats for nerds", every second while playing (and on
    pause): position and clock (sound or timer) and the gap between
    them; pictures shown a second against the video's rate, late ones;
    pictures decoded a second, ms each and times real time; decoding
    load (pictures and sound); frame skipping; swscale time a picture
    and its size; drawing time a picture (OS_SpriteOp, or EGL); pictures
    and packets waiting; the sound output and queue; the reading rate;
    null events a second and the screen mode.
  - Below, from `ffegl_media_info()`: file (name, title, container,
    length, size, bit rate, streams), video (codec, profile, level,
    size, aspect, frame rate, pixel format, colours, bit rate, frames,
    decoder, B-frame reordering), audio (codec, profile e.g. HE-AAC,
    channels and layout, sample rate and format, bit rate, language,
    decoder) and the sound output.
  - Drawn with Wimp_TextOp in the desktop font; the stats are redrawn
    with Wimp_UpdateWindow (no flicker from ForceRedraw).
- ffegl: `ffegl_media_info()` and `ffegl_stats()` (running totals:
  pictures decoded and shown, decoding, sound and swscale time, sizes,
  queues, sound counts, bytes read).
- Test: reel_test opens the window with I, checks its text (codec,
  sample rate, the stats after a second of playing) and closes it.

## Reel 0.1.7 (2026-09-27)

- **The trailer froze after half a second, now with SharedSoundBuffer.**
  Reel 0.1.6's log on the Pi: the sound started (SharedSoundBuffer 0.07,
  StreamManager 0.03 and SharedSound 1.20 all loaded), played 0.51 s,
  then `queued 0.00 s ... 7 pictures waiting` for good: the clock stayed
  at 0.49 s.
  - In this file the video is read about a second ahead of its sound.
    ffegl read packets only while fewer than 7 pictures were waiting, so
    with 7 decoded pictures (all ahead of the clock) it stopped reading;
    the sound ran dry; the clock (the sound) stopped; no picture became
    due. Every build since ffegl 1 had this; the SDL stall fallback hid it
    in 0.1.3.
  - Now ffegl keeps the video packets it reads (compressed) in a queue
    and decodes them separately: the file is read as far as the sound
    needs (0.5 s queued), however far the video is ahead or however slow
    it decodes (up to 48 MB of packets).
- **Falling behind:** 0.3 s behind the clock, ffegl skips decoding
  non-reference frames (`skip_frame = AVDISCARD_NONREF`, mostly
  B-frames); 1.5 s behind, keyframes only; back to every frame once
  within 0.05 s. The log says when ("0.32 s behind: skipping
  non-reference frames"), and `ffegl_debug()` shows the packets waiting
  and the skip level.
- Tests: `chunky_h264_aac_322_184.mp4` (fragments of 1.5 s, video then
  sound) freezes 0.1.6 and plays with 0.1.7 in `reel_ssb_test`;
  `slow_test` makes decoding cost more than real time and checks that
  the pictures keep up with the sound.

## Reel 0.1.6, !FFmpeg front end log (2026-09-27)

- **Reel and ReelEGL write a log**: `<Wimp$ScrapDir>.ReelLog`
  (`ReelEGLLog`), started afresh at each start and flushed line by line.
  - Start: the build, the versions of SharedSound, StreamManager,
    SharedSoundBuffer, SharedUnixLibrary and VFPSupport (from their help
    strings, or "not loaded"), `FFEGL_AUDIO`, `SDL_AUDIODRIVER`, the
    screen mode; ReelEGL adds EGL's version and each surface made.
  - Each file opened (`ffegl_info`), the window size, pause, seek, full
    screen, mode changes, the end, close, quit, and every error box.
  - FFmpeg's messages at verbose level, with repeats folded.
  - Once a second while playing: `ffegl_debug()` (position, clock and
    which one, pictures waiting, late frames; the sound output, queued
    time, StreamManager's added/played counts, bytes waiting, refused
    blocks), plus null events and pictures shown.
  - `Reel$Log` / `ReelEGL$Log` names another file, or `off`. "Log" on
    the icon bar menu opens it (Filer_Run).
- ffegl: new `ffegl_debug()` and `ffegl_set_log()`; SharedSoundBuffer
  events (stream opened, sound starts, first refused block, reopen
  failures) and SDL's driver name are logged at verbose level; the
  stall warning includes the `ffegl_debug()` line.
- !FFmpeg: ffplay runs with `-loglevel verbose` into
  `<Wimp$ScrapDir>.ffplay/log`, and the icon's menu has "Log" to open it.
- Tests: `reel_ssb_test` and `reelegl_test` check the log's contents.

## Reel 0.1.5 (2026-09-27)

- **Still no sound on the Pi with the modules loaded.** With SharedSound,
  StreamManager and SharedSoundBuffer loaded (`*Help SharedSoundBuffer`
  shows 0.07), ReelEGL still said "the sound device isn't playing": SDL's
  RISC OS driver opened, but the sound queued with `SDL_QueueAudio` was
  never taken. SDL takes it on its own audio thread, and in a Wimp task
  UnixLib's threads only run while the task is paged in; Reel spends most
  of its time in `Wimp_Poll` or decoding.
- ffegl now gives the sound to SharedSoundBuffer/StreamManager itself on
  RISC OS, from `ffegl_update()`, in 2048-frame blocks (0.5 s kept
  queued). StreamManager plays it from interrupts, so no thread is
  involved. The clock is `StreamManager_BufferStats` (added - played).
  Seeking closes and reopens the stream. `FFEGL_AUDIO=sdl` goes back to
  SDL. This covers Reel, ReelEGL, videowin and videocube.
- Test: `reel_ssb_test` runs the Reel script with ffegl built
  `-DFFEGL_SSB` against fake SharedSoundBuffer SWIs.

## Reel 0.1.4, and !FFmpeg 5.1.10-riscos5 repackaged (2026-09-27)

- **No sound on the Pi:** SharedSoundBuffer, StreamManager and SharedSound
  were merged into `!System` but never loaded (`*Help SharedSoundBuffer`
  gave "No help found"). Nothing had asked for them.
  - SDL then fell back to UnixLib's `/dev/dsp`, which accepted the sound
    and didn't play it. Reel showed "the sound device isn't playing", and
    ffplay played silently.
  - The `!Run` files of `!FFmpeg`, `!Reel` and `!ReelEGL`, `!FFmpeg.Task`
    and the EGL examples' `SetUp` now RMEnsure/RMLoad `SSound`,
    `StreamMan` and `SSBuffer` from `System:Modules`, the same lines as
    Warzone 2100's `!Run`.
- ffegl (so Reel, ReelEGL, videowin and videocube) now asks SDL for its
  RISC OS sound driver only (`SDL_AUDIODRIVER=riscos` unless it's already
  set). If the modules are missing there's no sound device, and
  `ffegl_info()` gives SDL's reason, e.g. "no sound device: RISC OS audio:
  SharedSoundBuffer/StreamManager not loaded".
- The `!FFmpeg` zip was repackaged with the new `!Run`, `Task` and
  `!Help`. The programs in it are unchanged.

## Reel 0.1.3 (2026-09-27)

- **A sound device that doesn't play no longer freezes the picture.** On
  the Pi, a trailer with sound showed its first frame and stopped. Reel
  follows the sound clock, and that clock doesn't move while the device
  takes none of the queued sound.
  - ffegl now watches for this: if nothing queued is played for 1 s while
    playing, it stops sending sound, pauses the device and carries on
    with its timer.
  - The stall timer restarts after a seek and after a pause.
  - `ffegl_has_audio()` returns 0 then, and `ffegl_info()` says "the
    sound device isn't playing".
  - Tested with a fake device that opens but never plays: all 25 frames
    are shown, finishing after about 2 s for a 1 s clip.
- HE-AAC (the trailer's sound) decodes under the alignment-trapping QEMU,
  and so does its H.264 Main 1280x544 picture. The file itself was never
  the problem.

## Reel 0.1.2 (2026-09-27)

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
- Reel 0.1.1's File info said "no sound" for Big Buck Bunny 720p 10s 30MB.
  That's correct: the file has no sound track.

## Reel 0.1.1 (2026-09-27)

- **!Reel, a video player for the desktop** (`player/reel.c`,
  `dist/Reel-0.1.1.zip`). It's a native Wimp app built on ffegl: an icon
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
