# Video into EGL and OpenGL

riscos-ffmpeg has two ways to show video through riscos-mesa's EGL:

- **`-f egl`**, an ffmpeg output device, for the command line and for
  programs that already use libavformat;
- **ffegl**, a small C library for programs that use EGL or OpenGL. It
  puts the pictures of **reelcore**, the player core (which plays a file
  with its sound and says when each picture is due), into an EGL surface
  or an OpenGL texture. reelcore itself has no EGL in it: !Reel uses it
  alone, drawing into a sprite.

Neither draws with OpenGL. Each frame is converted and scaled by swscale
(its NEON code where it has some) straight into the EGL surface's memory
(`EGL_KHR_lock_surface`), and `eglSwapBuffers` shows it. ffegl's texture
call converts the frame straight into the texture's own pixels through an
EGLImage (riscos-mesa 20.3.5-7pre12 or later), or else copies it in with
`glTexSubImage2D`.

## `ffmpeg -f egl`

```
ffmpeg -re -i clip/mp4 -map 0:v -f egl "Holiday"                    a desktop window
ffmpeg -re -i clip/mp4 -map 0:v -f egl -fullscreen 1 -              the whole screen
ffmpeg -re -i clip/mp4 -map 0:v -f egl -fullscreen 1 -direct 1 -    straight into screen memory
ffmpeg -i clip/mp4 -map 0:v -f egl -                                as fast as it decodes (a benchmark)
```

- `-re` plays at the video's own speed; without it, frames are shown as
  fast as they are decoded.
- The output name is the window title.
- There is no sound: the device is video only, so use `-map 0:v`, or
  ffplay for films with sound.
- The window's close icon, Escape or Q stops ffmpeg.
- Outside the desktop the device always uses the whole screen.
- **From a TaskWindow** there can be no window: a TaskWindow is already
  the program's Wimp task, so `Wimp_Initialise` fails ("Window Manager is
  currently in use"). The device stops with an error that says so. Use
  `-fullscreen 1`, or start ffmpeg as its own task:
  `WimpTask ffmpeg -nostats -re -i clip/mp4 -map 0:v -f egl "Holiday"`.
  (ffplay does this by itself.)

| Option | Meaning |
|---|---|
| `-window_title T` | window title (default: the output name) |
| `-window_size WxH` | window size (default: the video's, shrunk to fit the screen) |
| `-window_x`, `-window_y` | window position in pixels from the top left (default: centred) |
| `-window_handle H` | draw in an existing Wimp window instead of opening one |
| `-fullscreen 1` | the whole screen (a sprite plotted after the vsync wait) |
| `-direct 1` | full screen: write straight into screen memory (no plot; can tear) |
| `-vsync N` | full screen: vertical syncs to wait for each frame (default 1; 0 = none) |
| `-scale S` | scaler when the size differs: `fast_bilinear` (default), `bilinear`, `bicubic`, `neighbor`, `area` |

- The picture keeps its shape, with black bars.
- Any pixel format ffmpeg decodes to can go in: `yuv420p` goes straight
  to the screen format in one swscale pass.

## reelcore and ffegl (libraries)

Both are in the devkit, and each header documents its calls:

- **reelcore** (`include/reelcore.h`, `lib/libreelcore.a`): the player
  core. It opens a file or stream, decodes inside `reelcore_update()`,
  plays the sound (the clock the pictures follow) and says when a new
  picture is due; `reelcore_draw_pixels()` puts it into any 32bpp memory.
  No EGL, no threads.
- **ffegl** (`include/ffegl.h`, `lib/libffegl.a`): the EGL side, on top
  of reelcore. `ffegl_draw_surface()` puts the current picture into an EGL
  surface and `ffegl_texture()` into an OpenGL texture.

```c
#include "ffegl.h"                              /* includes reelcore.h */

ReelCore *v = reelcore_open("SDFS::Pi.$.Films.clip/mp4", 0);   /* or REELCORE_LOOP, REELCORE_NO_AUDIO */
for (;;) {                                      /* your event loop */
    int r = reelcore_update(v);                 /* decodes what's needed; sound keeps time */
    if (r == REELCORE_NEW_FRAME) {
        ffegl_draw_surface(v, dpy, surf, 0, 0, 0, 0, 0);   /* whole surface, letterboxed */
        eglSwapBuffers(dpy, surf);
    } else if (r == REELCORE_END)
        break;
    /* ... Wimp_PollIdle until reelcore_idle_time() says a picture is due ... */
}
reelcore_close(v);
```

- **Into an OpenGL texture:** `ffegl_texture(v, tex, &tex)` with a
  context current, after each `REELCORE_NEW_FRAME`. Pass 0 the first time
  to have it make the texture. The texture is the frame's size, with
  linear filtering and clamping. Row 0 is the top, so draw with t = 1 at
  the bottom. It works with GL 1.x, ES 1.1 and ES 2.0 contexts.
  - **With riscos-mesa 7pre12 or later** (`GL_OES_EGL_image` in the
    extension string), ffegl makes a 32bpp sprite of the frame's size, in
    the context's colour order, and binds it to the texture once with
    `eglCreateImageKHR` + `glEGLImageTargetTexture2DOES`. Each new frame
    is converted straight into the sprite: no upload, no second copy.
    Such a texture is opaque (`GL_RGB`) and has level 0 only, so keep a
    `GL_LINEAR` or `GL_NEAREST` min filter.
  - **Otherwise** (an older riscos-mesa, or `FFEGL_NO_EGLIMAGE=1` set) each
    frame is converted into a buffer and copied in with
    `glTexSubImage2D` (`GL_RGBA`).
  - Don't `glTexImage2D` the texture yourself while ffegl uses it. Call
    `reelcore_close` with the context still current (it gives the texture
    one black texel, then frees the sprite) or after destroying the
    context.
- **Into your own memory** (a sprite, a work area): `reelcore_draw_pixels(v,
  pixels, pitch, w, h, bgr, flags)`: no EGL needed.
- **Controls:** `reelcore_pause`, `reelcore_seek`, `reelcore_set_volume`,
  `reelcore_position`, `reelcore_duration`, `reelcore_width`/`height`
  (display size, aspect applied); `reelcore_idle_time` for sleeping
  between pictures; `reelcore_media_info` and `reelcore_stats` for an
  information window.
- **Playback options** (from Reel 0.1.11): `reelcore_set_speed` (0.5x to
  2x; the sound keeps its pitch through FFmpeg's atempo filter, so link
  `-lavfilter`), `reelcore_set_fast` (skips the deblocking filter),
  `reelcore_audio_tracks`/`_track_name`/`set_audio_track` (choose the
  sound track). `reelcore_draw_pixels` (and `ffegl_draw_surface`) take
  `REELCORE_FILL` (cover the rectangle, cropping) or `REELCORE_ORIGINAL`
  (1:1, centred) as well as `REELCORE_STRETCH`; 0 fits, letterboxed.
- **Sound:** on RISC OS straight to SharedSoundBuffer and StreamManager
  (`REELCORE_AUDIO=sdl` for SDL2's audio), and the pictures follow it.
  Without sound, or if no sound device opens, they follow a timer.
- **Programs using them in a window** are desktop tasks, so they can't
  run inside a TaskWindow either: start them with `*WimpTask`, from an
  Obey file, or from the Filer.
- **No threads:** decoding happens inside `reelcore_update`, so call it
  often (every Wimp null event, or every GL frame). A 1080p H.264 frame
  takes roughly 25–40 ms to decode on a Pi 4.

Link (static), EGL programs:

```
-lffegl -lreelcore -lavfilter -lpostproc -lavformat -lavcodec -lswresample -lswscale -lavutil -ldav1d -lx264
-lmp3lame -lopus -lvorbisenc -lvorbis -logg -lz -lSDL2 -lEGL -lOSMesa -lstdc++ -lm
```

A program without EGL (like !Reel) links `-lreelcore` and the FFmpeg
libraries only, with no Mesa.

SDL2, EGL, OSMesa and zlib come from the riscos-mesa devkit. Add
`-lglut -lGLU` for freeglut programs.

## Examples (FFmpeg-EGL-examples zip)

Double-click `SetUp` in the EGLExamples directory once; then, from a
TaskWindow or the command line (the commands start each program as its
own desktop task, with its messages in `<Wimp$ScrapDir>.videowin/log` or
`.videocube/log`):

- **`videowin FILE`** (`-f` full screen, `-loop`): a desktop window drawn
  with `ffegl_draw_surface`.
  - Keys: Space pauses, Left/Right seek 10 s, Up/Down seek 1 min,
    Escape/Q quit.
  - The window can be resized; the picture follows.
- **`videocube FILE`** (`-flat` for a plain quad): the video on a spinning
  cube through `ffegl_texture`, with freeglut and OpenGL 1.x. The window
  is small (480×360) on purpose: software GL pays for every textured
  pixel. The window title says whether the texture shares the frame
  (EGLImage) or gets copies.

## Speed

- **Window at the video's size:** one swscale pass (NEON for YUV→RGB),
  then riscos-mesa's sprite plot of the window.
- **Full screen with `-direct 1` / `EGL_SINGLE_BUFFER`:** no plot at all.
- **Scaling:** one C pass (`fast_bilinear` by default).
- **Textures:** one swscale pass into the sprite behind the texture
  (EGLImage), then the GL drawing, whose cost grows with the pixels the
  texture covers (software GL). Without EGLImage, add a whole-frame copy
  (`glTexSubImage2D`, about 3–4 ms for 1080p on a Pi 4) and a second copy
  of the picture in memory.

## Tests

`tests/host/run.sh` runs both, built for arm-linux with fake RISC OS,
EGL, SDL audio and GL, under the qemu that traps unaligned accesses. The
checks:

- pixels equal a separate swscale conversion, and the bars are black;
- one swap per frame, and the Wimp task and window tidied up;
- the close icon and Escape stop playback;
- A/V timing against a fake sound device: each frame within one 10 ms
  step of its time;
- pause, seek, loop, and the timer when there is no sound device;
- textures: the fallback copies; the EGLImage path in both colour orders
  (a valid sprite, the pixels converted in place with no GL upload, the
  texture unlinked before the image is destroyed); falling back when the
  image can't be made, and `FFEGL_NO_EGLIMAGE=1`.

`tests/host/mesa/run.sh` then runs `ffegl_texture()` against riscos-mesa's
real libEGL and libOSMesa from the devkit (the RISC OS libraries, linked
into an arm-linux program with `riscos_shim.c`): a pbuffer and a GL 1.x
context in each colour order, the video drawn on a quad and read back
with `glReadPixels`, for the EGLImage path and the copies. The pictures
must match ffegl's own conversion, and an EGLImage texture must be
`GL_RGB` (proof that the sprite is really used).
