/*
 * ffegl.h - play videos into riscos-mesa's EGL and OpenGL, with FFmpeg.
 *
 * A small layer over libavformat/libavcodec/libswscale/libswresample for
 * programs that already use EGL or OpenGL on RISC OS: open a file (or a
 * network stream), call ffegl_update() from your event loop, and put the
 * current frame where you want it:
 *
 *   - ffegl_draw_surface(): into an EGL window surface (a Wimp window or
 *     the whole screen) through EGL_KHR_lock_surface, scaled to fit. No GL
 *     drawing, no context needed; eglSwapBuffers shows it.
 *   - ffegl_texture(): into an OpenGL texture, to draw the video on
 *     anything: a quad, a cube, a 3D scene. With riscos-mesa 7pre12 or
 *     later the texture shares ffegl's pixels through an EGLImage (no copy).
 *   - ffegl_draw_pixels(): into any 32bpp memory, e.g. a sprite.
 *
 * Sound plays through SDL2's audio (SharedSoundBuffer on RISC OS) and is the
 * clock the pictures follow. There are no threads: decoding happens inside
 * ffegl_update(), so call it often (every Wimp null event, or every frame).
 *
 *   FFEGLVideo *v = ffegl_open("SDFS::Pi.$.clip/mp4", 0);
 *   ...
 *   int r = ffegl_update(v);             // in the event loop
 *   if (r == FFEGL_NEW_FRAME) {
 *       ffegl_draw_surface(v, dpy, surf, 0, 0, 0, 0);
 *       eglSwapBuffers(dpy, surf);
 *   } else if (r == FFEGL_END) ...
 *   ...
 *   ffegl_close(v);
 *
 * Part of riscos-ffmpeg. LGPL 2.1 or later (as FFmpeg's libraries).
 */
#ifndef FFEGL_H
#define FFEGL_H

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FFEGLVideo FFEGLVideo;

/* ffegl_open flags */
#define FFEGL_NO_AUDIO   1   /* don't play the sound (pictures follow a timer) */
#define FFEGL_LOOP       2   /* start again at the end */
#define FFEGL_PAUSED     4   /* open paused (ffegl_pause(v, 0) starts it) */

/* ffegl_update results */
#define FFEGL_SAME_FRAME 0   /* nothing new to show */
#define FFEGL_NEW_FRAME  1   /* a new frame is current: draw it */
#define FFEGL_END        2   /* played to the end (not with FFEGL_LOOP) */

/* ffegl_draw_* and ffegl_texture flags */
#define FFEGL_STRETCH    1   /* fill the rectangle (default: keep the shape, black bars) */
#define FFEGL_NO_BORDERS 2   /* keep the shape but leave the bars alone */

/* Opens a file or URL. NULL on failure (the reason is logged through
   av_log; ffegl_last_error() gives it too). */
FFEGLVideo *ffegl_open(const char *url, int flags);
const char *ffegl_last_error(void);
void ffegl_close(FFEGLVideo *v);

/* The video's size in pixels (display aspect applied to the width), its
   frame rate and length in seconds (0 when unknown, e.g. a live stream),
   and whether it has sound that is being played. */
int ffegl_width(const FFEGLVideo *v);
int ffegl_height(const FFEGLVideo *v);
double ffegl_frame_rate(const FFEGLVideo *v);
double ffegl_duration(const FFEGLVideo *v);
int ffegl_has_audio(const FFEGLVideo *v);

/* Decodes what is needed and chooses the frame for "now".
   Returns FFEGL_NEW_FRAME, FFEGL_SAME_FRAME, FFEGL_END, or a negative
   AVERROR code. The first call after opening (or seeking) always gives
   FFEGL_NEW_FRAME once a picture is decoded. */
int ffegl_update(FFEGLVideo *v);

/* Position of the current frame in seconds. */
double ffegl_position(const FFEGLVideo *v);

void ffegl_pause(FFEGLVideo *v, int paused);
int ffegl_paused(const FFEGLVideo *v);
/* Seeks to a time in seconds (to the key frame at or before it). */
int ffegl_seek(FFEGLVideo *v, double seconds);
/* Sound volume, 0.0 to 1.0. */
void ffegl_set_volume(FFEGLVideo *v, double volume);

/* Draws the current frame into an EGL surface through EGL_KHR_lock_surface:
   the surface must not be current to a context. x, y, w, h: the rectangle
   in pixels from the top left; w = 0 or h = 0 means the whole surface.
   Then call eglSwapBuffers (or eglSwapBuffersWithDamageKHR). */
int ffegl_draw_surface(FFEGLVideo *v, EGLDisplay dpy, EGLSurface surf,
                       int x, int y, int w, int h, int flags);

/* Draws the current frame into 32bpp memory: w x h pixels, pitch bytes a
   row, top row first. bgr = 0: bytes R,G,B,x (sprite type 6 / TBGR, 0x00BBGGRR);
   bgr = 1: bytes B,G,R,x (0x00RRGGBB). */
int ffegl_draw_pixels(FFEGLVideo *v, void *pixels, int pitch, int w, int h,
                      int bgr, int flags);

/* Puts the current frame into an OpenGL texture (GL_TEXTURE_2D, the
   video's size). Needs a current GL context. tex = 0 creates a texture and
   returns its name in *tex_out; after that pass the same name back.
   Sets linear filtering and clamping. Returns 0 or an AVERROR code. The
   picture's top row is the texture's first row (t = 0), so draw with t = 1
   at the bottom.

   Where the context has GL_OES_EGL_image (riscos-mesa 7pre12 or later),
   the texture's pixels are a sprite that ffegl owns, bound through an
   EGLImage, and each frame is converted straight into it: no copy. The
   texture is then opaque (GL_RGB) and has level 0 only, so don't give it
   mipmap filters. Otherwise (or with FFEGL_NO_EGLIMAGE=1 in the
   environment) each frame is copied in with glTexSubImage2D (GL_RGBA).
   Either way, don't glTexImage2D the texture yourself while ffegl uses it.
   Passing a different texture, or ffegl_close() with the context current,
   gives the old one a single black texel; delete it or call ffegl_close()
   before destroying the context, or after (both are fine). */
int ffegl_texture(FFEGLVideo *v, unsigned int tex, unsigned int *tex_out);

#ifdef __cplusplus
}
#endif

#endif
