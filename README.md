# riscos-ffmpeg

FFmpeg 5.1.10 (LTS) for RISC OS: `ffmpeg`, `ffprobe` and `ffplay`, plus the
static libraries. It is built with GCCSDK GCC 10.2 (`arm-riscos-gnueabihf`)
for ARMv7 with NEON: Pi 2/3/4/400, Titanium, IGEPv5, iMX6, BeagleBoard-xM
and Pandaboard.

The point of this port is speed. It uses FFmpeg's, dav1d's and x264's
hand-written NEON code, made safe for RISC OS's alignment checking.

## Why this version

| | 4.4 (PackMan) | **5.1 LTS** | 6.0 | 6.1 – 9.0 |
|---|---|---|---|---|
| CPU target | ARMv4, no VFP or NEON | ARMv7 + NEON | same | same |
| NEON MDCT for AAC, AC-3, Vorbis, WMA (32-bit ARM) | – | yes | no: generic C (lavu/tx) | no |
| `ffmpeg` command needs threads | no | no | no | yes (a thread per stage) |
| Maintained upstream | yes (4.4.8; PackMan has 4.4) | yes (5.1.10) | ended 2024 | yes |

- 6.0 moved the audio codecs to `libavutil/tx`, which has NEON only for
  64-bit ARM, so audio decoding got slower on 32-bit ARM.
- 6.1 and later run every stage of the `ffmpeg` command in its own
  thread. On RISC OS that means one core and UnixLib's pthreads, so the
  threads only add switching.
- The new codecs since then (VVC, for example) are not worth that cost on
  these machines.

## What's in it

- **Programs:** `ffmpeg`, `ffprobe`, and `ffplay` (SDL2 from riscos-mesa:
  Wimp window, SharedSoundBuffer sound).
- **Icon bar:** double-click `!FFmpeg` and drop video files on its icon to
  play them (`frontend/fffront.c`).
- **!Reel:** a video player for the desktop (`player/reel.c`, its own zip),
  built on reelcore, the player core (no EGL), with **!ReelEGL**, the same
  player drawing through riscos-mesa's EGL with ffegl.
  - A Wimp window with Play/Pause, skip, a position bar, time, volume and
    full screen; drop a file on the icon or double-click it in the Filer.
  - A mini player above the icon bar (optionally kept on top), playlists,
    speed 0.5x–2x, picture sizes, sound tracks, A-B repeat, carry on
    where you stopped, vsync full screen, fast decoding, deinterlacing.
  - **Subtitles** (the file's own tracks, and .srt/.ass/.vtt files dropped
    on the window or found beside the video), **chapters** (a menu, Page
    Up/Down), **frame steps** (`.` and `,`), and videos turned as the file
    says; stats "for nerds" drawn on the picture (S).
  - **Web addresses** (http, https through AcornSSL, HLS): Open address…
    on the icon bar menu (type or paste), or a dropped URI/URL/text/M3U
    file. **yt-dlp's output** plays too: `-g`'s one or two addresses (the
    best video and best sound apart, played together) or `-j`'s JSON
    (title, addresses, the site's HTTP headers; a line per video for a
    playlist). A thread in reelcore reads up to 10 s ahead.
  - **Hardware acceleration** through the VideoOverlay module: pictures are
    copied as YV12 into a hardware overlay, which the display scales and
    converts (4K is halved into it first); drawn as before whenever that
    isn't possible.
- **NEON added by this port** (each bit-exact with FFmpeg's C, tested
  under the alignment-trapping qemu): yadif deinterlacing (patch 0015),
  HEVC chroma motion compensation (0016), and swscale's fast bilinear
  scaling to RGB32 (0017), which Reel uses whenever the picture is
  resized.
  - Sound straight to SharedSoundBuffer, which is also the clock the
    pictures follow; when decoding can't keep up, deblocking is turned off
    by itself first, then late frames are skipped (and, when far behind,
    non-reference frames aren't decoded).
  - **Media info** window: the file's codecs and formats, and "stats for
    nerds" every second (pictures shown and decoded, decode time and
    speed, drawing time, queues, sound, reading rate).
  - Sleeps between pictures (Wimp_PollIdle), and writes a log
    (`<Wimp$ScrapDir>.ReelLog`).
- **Codec libraries:** x264 (H.264 encoding), dav1d (AV1 decoding, NEON),
  LAME, Opus, Vorbis, and zlib.
- **Network:** http, tcp, udp, rtp, rtmp and hls, and **https** (and HLS
  over https) through RISC OS's AcornSSL module (patch 0018,
  `libavformat/tls_acornssl.c`; RISC OS 5.28 or later): the TCP socket is
  handed to AcornSSL, which checks certificates itself and asks in the
  desktop about one it can't verify.
- **Input device:** `lavfi` (test patterns and sources).
- **RISC OS file names:** `SDFS::Pi.$.clip/mp4` and `<Obey$Dir>.x/mkv` work
  both for input and output. The `/ext` part picks the output format.
- **Memory:** the heap goes in a dynamic area, "FFmpeg Heap" (up to
  512 MB).
- **ffplay:** converts and scales each frame straight into its window's
  surface, in the screen's pixel format, with swscale (NEON YUV→RGB). SDL
  only plots the window. `FFPLAY_SCALE` picks the scaler, and
  `FFPLAY_RENDERER=sdl` goes back to SDL's texture drawing. Started in a
  TaskWindow (which can't open windows), it starts itself as a desktop
  task of its own.
- **Libraries for your own programs:** **reelcore** plays a video with
  its sound and says when each picture is due (no EGL; `reelcore.h`), and
  **ffegl** puts its pictures into riscos-mesa's EGL: EGL surfaces and
  OpenGL textures (`ffegl.h`). With riscos-mesa 20.3.5-7pre12 or later a
  texture shares the pixels through an EGLImage, with no copy.
- **EGL output device:** `ffmpeg -f egl` shows video through
  riscos-mesa's EGL (a window or the whole screen). See [docs/EGL.md](docs/EGL.md); the
  FFmpeg-EGL-examples zip has `videowin` and `videocube`.

## Downloads

From [Releases](../../releases):

- `FFmpeg-5.1.10-riscosN.zip`: `!FFmpeg` with the programs.
- `FFmpeg-EGL-examples-5.1.10-riscosN.zip`: `videowin` and `videocube`.
- `riscos-ffmpeg-devkit-5.1.10-riscosN.tgz`: headers and static libraries
  (FFmpeg, reelcore, ffegl, codecs) for GCCSDK.
- `Reel-X.Y.Z.zip`: `!Reel` and `!ReelEGL`.

They need SharedUnixLibrary 1.16 or later and ARMEABISupport (PackMan).
They are linked with UnixLib 5.0.3.1 (github.com/adyoull/riscos-unixlib, an unofficial fork of GCCSDK's UnixLib: files over 2GB, up to 4GB-1, and threads that run in programs that poll often) and carry its PThreadTicker module (0.03)
(the thread timer's code; `third_party/pthreadticker`), which each `!Run`
loads.
For sound: SharedSoundBuffer and StreamManager, John Duffell's modules.
Download `ssb.zip` from Andrew Sellors' RDPClient page,
https://orac.co.uk/software/rdpclient/rdpclient.html, and merge its `!System` into yours;
the apps' `!Run` files load them. (SharedSound, which they use, is part of
RISC OS.) John Duffell's own site (now on the Internet Archive) has more
details: https://web.archive.org/web/20110920080106/http://www.duffell.riscos.me.uk/ . The EGL examples and !ReelEGL need riscos-mesa.

## Alignment: the main porting work

RISC OS runs ARMv7 with alignment checking on, so an unaligned `ldr` or
`vld1.32` aborts ("abort on data transfer"). Linux allows these accesses,
and the ARM assembly in FFmpeg, dav1d and x264 relies on that.
[docs/ALIGNMENT.md](docs/ALIGNMENT.md) has the details. In short:

- **`tools/neon-align.py`** rewrites every NEON load/store whose element is
  wider than a byte and that has no alignment qualifier, into forms that
  can't fault and do the same thing. It changes about 510 instructions in
  FFmpeg, 480 in dav1d and 280 in x264, at build time.
  - Whole-register transfers become `.8`. This is identical on
    little-endian and costs nothing.
  - Single-lane and "dup" transfers become byte-lane sequences.
- **Other assembly:**
  - FFmpeg's ARMv6 media routines are never selected (NEON or C instead).
  - VP8/VP9's `ldrh` bitstream refill uses C.
  - dav1d's entropy decoder reads bytes.
  - dav1d's cdef padding uses `strh`, and big frames probe the stack.
  - x264's ARMv6 4×4 SAD uses C.
- **The test rig:** QEMU patched to trap like RISC OS
  (`tests/qemu`).
  - FFmpeg, dav1d and x264's own asm test suites pass under it (881 +
    2039 checks + x264's).
  - So do 128 decode/encode/scale jobs (including clips with lots of motion), whose NEON output equals the C
    output bit for bit wherever the code is meant to be bit-exact.

## Building

On an x86-64 Linux with the GCCSDK GCC 10.2 environment in `~/gccsdk/env`
(the Warzone 2100 port's `gccsdk-gcc10.2-x86_64-linux-env.tgz`) and the
riscos-mesa devkit (20.3.5-7pre12 or later) unpacked in `devkit/`:

```
make sources      # checks dl/ against build/SHA256SUMS (fetch them first)
make deps         # SDL2 (riscos-mesa overlay, no GL), x264, dav1d, LAME, ...
make ffmpeg       # FFmpeg 5.1.10 + patches/ffmpeg + the NEON rewrite
make apps         # libreelcore, libffegl, !Reel, !ReelEGL, videowin, videocube, the icon bar front end
make package      # dist/: the FFmpeg, EGL examples and Reel zips, the devkit
make test         # the same code for arm-linux, run under the trapping qemu
```

`build/env.sh` holds the settings:

- `-O3 -march=armv7-a -mfpu=neon-vfpv3 -mtune=cortex-a72
  -fstack-clash-protection`
- vfpv3 rather than vfpv4, so Cortex-A8/A9 boards work;
- `-fstack-clash-protection` because GCC 10 programs get their stack a
  page at a time (see riscos-mesa).

`docs/SOURCES.md` lists the upstream sources and their checksums.

## Layout

```
build/      env.sh, build-deps.sh, build-ffmpeg.sh, build-apps.sh, package.sh, SHA256SUMS
patches/    ffmpeg/ (git format-patch series), dav1d/, x264/, lame/, sdl2/ (riscos-mesa overlay copy)
reelcore/   the player core (reelcore.h, reelcore.c): no EGL
ffegl/      the EGL layer on reelcore (ffegl.h, ffegl.c) and the videowin/videocube examples
player/     reel.c: !Reel and !ReelEGL; sources.c: addresses and yt-dlp output in text
frontend/   fffront.c: !FFmpeg's icon bar front end
tools/      neon-align.py (+ *.allow lists), mkrozip.py, mksprites.py, elf2aif/, check-stack-probes.py
app/        !FFmpeg, !Reel, !ReelEGL (the parts that aren't built)
tests/qemu/ the alignment rig: qemu patch, sample maker, runners
tests/host/ reelcore, ffegl, Reel and front end tests with fake Wimp/SDL/EGL, under the trapping qemu
docs/       ALIGNMENT.md, EGL.md, SOURCES.md
```

## Hardware decoding

Decoding with the Raspberry Pi's own hardware (H.264 on the VideoCore,
HEVC on the Pi 4's HEVC block) is a separate project, ReelHWAccel:
github.com/adyoull/riscos-reelhwaccel. It started here; its test tools
and the HEVCHW module moved there with their history.

## Licence

The build scripts, tools, patches and programs are GPL version 2 or later
(see `COPYING`), like the FFmpeg build they make; reelcore and ffegl are LGPL 2.1 or
later, and the ffegl examples' sources (`videowin.c`, `videocube.c`) are MIT, so
they can be copied freely. That build includes x264, so it is GPL (version 2
or later). The other libraries keep their own licences: see
`dist/…/docs/Licences`.

FFmpeg is a trademark of Fabrice Bellard. This is an unofficial port.
