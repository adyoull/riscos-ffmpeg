/*
 * ffegl_texture() against riscos-mesa's own libEGL and libOSMesa (the
 * devkit's RISC OS libraries, run on arm-linux through riscos_shim.c):
 * the EGLImage path and the glTexSubImage2D fallback, drawn with GL 1.x
 * into a pbuffer and read back.
 *
 *   mesa_eglimage_test CLIP
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include "ffegl.h"
#include "fake_sdl_gl.h"     /* fake_time: the clock ffegl sees */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int next_frame(FFEGLVideo *v)
{
    for (int i = 0; i < 1000; i++) {
        int r = ffegl_update(v);
        fake_time += 0.01;
        if (r == FFEGL_NEW_FRAME) return 1;
        if (r != FFEGL_SAME_FRAME) return 0;
    }
    return 0;
}

/* Draws the texture over the whole w x h surface and compares what GL
   drew with ffegl_draw_pixels at the same size. Returns pixels off by > 2. */
static int draw_and_compare(FFEGLVideo *v, unsigned tex, int w, int h, unsigned *sum)
{
    unsigned char *got = malloc((size_t)w * h * 4), *want = malloc((size_t)w * h * 4);
    int bad = 0;
    glViewport(0, 0, w, h);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);                        /* the picture's top row is t = 0 */
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(1, 1); glVertex2f( 1, -1);
    glTexCoord2f(1, 0); glVertex2f( 1,  1);
    glTexCoord2f(0, 0); glVertex2f(-1,  1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, got);   /* bottom row first */
    ffegl_draw_pixels(v, want, w * 4, w, h, 0, FFEGL_STRETCH);
    *sum = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const unsigned char *g = got + ((h - 1 - y) * w + x) * 4, *e = want + (y * w + x) * 4;
            for (int c = 0; c < 3; c++) {
                int d = g[c] - e[c];
                if (d > 2 || d < -2) { bad++; break; }
            }
            *sum += g[0] + g[1] * 3 + g[2] * 7;
        }
    free(got); free(want);
    return bad;
}

static void run(const char *clip, EGLDisplay dpy, EGLConfig cfg, const char *what, int use_image)
{
    FFEGLVideo *v;
    EGLSurface s;
    EGLContext ctx;
    unsigned tex = 0, sum0, sum1;
    int w, h, bad, fails0 = fails;
    GLint ifmt = 0, tw = 0;

    if (!use_image) setenv("FFEGL_NO_EGLIMAGE", "1", 1);
    v = ffegl_open(clip, FFEGL_NO_AUDIO);
    CHECK(v && next_frame(v), "%s: no frame", what);
    if (!v) return;
    w = ffegl_width(v); h = ffegl_height(v);
    {
        EGLint pb[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
        s = eglCreatePbufferSurface(dpy, cfg, pb);
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
        CHECK(s != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, s, s, ctx),
              "%s: pbuffer/context (0x%x)", what, eglGetError());
    }
    CHECK(ffegl_texture(v, 0, &tex) == 0 && tex, "%s: ffegl_texture", what);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    /* an EGLImage texture is GL_RGB (the fourth byte unused); copies are GL_RGBA */
    CHECK(use_image ? ifmt == GL_RGB : ifmt == GL_RGBA, "%s: internal format 0x%x", what, ifmt);
    bad = draw_and_compare(v, tex, w, h, &sum0);
    CHECK(!bad, "%s: first frame, %d pixels wrong", what, bad);
    for (int i = 0; i < 5; i++) {
        CHECK(next_frame(v), "%s: no next frame", what);
        CHECK(ffegl_texture(v, tex, NULL) == 0, "%s: update", what);
    }
    bad = draw_and_compare(v, tex, w, h, &sum1);
    CHECK(!bad && sum1 != sum0, "%s: 6th frame, %d pixels wrong%s", what, bad, sum1 == sum0 ? ", unchanged" : "");
    CHECK(glGetError() == GL_NO_ERROR, "%s: GL error", what);
    printf("  %s: %dx%d, internal format %s, %s\n", what, w, h,
           ifmt == GL_RGB ? "GL_RGB (EGLImage)" : "GL_RGBA (copies)", fails == fails0 ? "pictures match" : "FAILED");
    ffegl_close(v);
    if (use_image) {
        /* closing with the context current unlinks the texture (one texel) */
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        CHECK(glIsTexture(tex) && tw == 1, "%s: after close, texture width %d", what, tw);
    }
    glDeleteTextures(1, &tex);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, s);
    unsetenv("FFEGL_NO_EGLIMAGE");
}

int main(int argc, char **argv)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfgs[64];
    EGLint n = 0, attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                             EGL_RED_SIZE, 8, EGL_NONE };
    int done[2] = { 0, 0 };
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(eglInitialize(dpy, NULL, NULL), "eglInitialize");
    CHECK(strstr(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_image_pixmap") != NULL, "no EGL_KHR_image_pixmap");
    eglBindAPI(EGL_OPENGL_API);
    eglChooseConfig(dpy, attr, cfgs, 64, &n);
    for (int i = 0; i < n; i++) {
        EGLint vis = 0, depth = 0;
        eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vis);
        eglGetConfigAttrib(dpy, cfgs[i], EGL_BUFFER_SIZE, &depth);
        int bgr = (vis & 0x4000) != 0;
        if (depth != 32 || done[bgr]) continue;
        done[bgr] = 1;
        run(argv[1], dpy, cfgs[i], bgr ? "EGLImage, TRGB context" : "EGLImage, TBGR context", 1);
        if (!bgr) run(argv[1], dpy, cfgs[i], "copies (FFEGL_NO_EGLIMAGE=1)", 0);
    }
    CHECK(done[0], "no 32bpp TBGR pbuffer config (%d configs)", n);
    if (!done[1]) printf("  (no TRGB pbuffer config to test)\n");
    eglTerminate(dpy);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
