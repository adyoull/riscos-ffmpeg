/*
 * ffegl.h - puts reelcore's pictures into riscos-mesa's EGL.
 *
 * reelcore (reelcore.h) plays the video and sound and says when a picture
 * is due; ffegl is the EGL side, for programs that draw with EGL or OpenGL:
 *
 *   - ffegl_draw_surface(): into an EGL window surface (a Wimp window, a
 *     work area or the whole screen) through EGL_KHR_lock_surface, scaled
 *     to fit. No GL drawing, no context needed; eglSwapBuffers shows it.
 *   - ffegl_texture(): into an OpenGL texture, to draw the video on
 *     anything: a quad, a cube, a 3D scene. With riscos-mesa 7pre12 or
 *     later the texture shares the pixels through an EGLImage (no copy).
 *
 *   ReelCore *v = reelcore_open("SDFS::Pi.$.clip/mp4", 0);
 *   ...
 *   int r = reelcore_update(v);          // in the event loop
 *   if (r == REELCORE_NEW_FRAME) {
 *       ffegl_draw_surface(v, dpy, surf, 0, 0, 0, 0, 0);
 *       eglSwapBuffers(dpy, surf);
 *   }
 *
 * Link libffegl, libreelcore, FFmpeg's libraries, then -lEGL -lOSMesa (and
 * -lGL... for textures). Built with FFEGL_NO_TEXTURE, only
 * ffegl_draw_surface is there, and no OpenGL is needed.
 *
 * Part of riscos-ffmpeg. LGPL 2.1 or later (as FFmpeg's libraries).
 */
#ifndef FFEGL_H
#define FFEGL_H

#include <EGL/egl.h>
#include "reelcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draws the current frame into an EGL surface through EGL_KHR_lock_surface:
   the surface must not be current to a context. x, y, w, h: the rectangle
   in pixels from the top left; w = 0 or h = 0 means the whole surface.
   flags: as reelcore_draw_pixels (REELCORE_STRETCH, REELCORE_NO_BORDERS).
   Then call eglSwapBuffers (or eglSwapBuffersWithDamageKHR).
   0, or an AVERROR code (AVERROR(EAGAIN) before the first frame). */
int ffegl_draw_surface(ReelCore *v, EGLDisplay dpy, EGLSurface surf,
                       int x, int y, int w, int h, int flags);

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
   Passing a different texture, or reelcore_close() with the context
   current, gives the old one a single black texel; delete it or call
   reelcore_close() before destroying the context, or after (both are
   fine). (Not in a build with FFEGL_NO_TEXTURE.) */
int ffegl_texture(ReelCore *v, unsigned int tex, unsigned int *tex_out);

#ifdef __cplusplus
}
#endif

#endif
