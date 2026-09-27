/*
 * ffegl.c - puts reelcore's pictures into riscos-mesa's EGL: EGL surfaces
 * (EGL_KHR_lock_surface) and OpenGL textures (through an EGLImage where
 * the context has GL_OES_EGL_image). See ffegl.h. Part of riscos-ffmpeg.
 * LGPL 2.1 or later.
 *
 * Built with FFEGL_NO_TEXTURE, only the surface part is there (no OpenGL).
 */
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifndef FFEGL_NO_TEXTURE
#include <GL/gl.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "ffegl.h"

/* ---- EGL surfaces --------------------------------------------------------- */

int ffegl_draw_surface(ReelCore *v, EGLDisplay dpy, EGLSurface surf,
                       int x, int y, int w, int h, int flags)
{
    EGLint lock_attr[] = { EGL_LOCK_USAGE_HINT_KHR, EGL_WRITE_SURFACE_BIT_KHR, EGL_NONE };
    EGLAttribKHR ptr = 0;
    EGLint pitch = 0, sw = 0, sh = 0, blue = 16;
    int ret, fw, fh;

    if (reelcore_frame_size(v, &fw, &fh) < 0)
        return AVERROR(EAGAIN);
    if (!eglLockSurfaceKHR(dpy, surf, lock_attr))
        return AVERROR_EXTERNAL;
    eglQuerySurface64KHR(dpy, surf, EGL_BITMAP_POINTER_KHR, &ptr);
    eglQuerySurface(dpy, surf, EGL_BITMAP_PITCH_KHR, &pitch);
    eglQuerySurface(dpy, surf, EGL_BITMAP_PIXEL_BLUE_OFFSET_KHR, &blue);
    eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = sw;
        h = sh;
    }
    if (!ptr || x < 0 || y < 0 || x + w > sw || y + h > sh)
        ret = AVERROR(EINVAL);
    else
        ret = reelcore_draw_pixels(v, (uint8_t *)ptr + y * pitch + x * 4, pitch, w, h,
                                   blue == 0, flags);   /* blue in the low byte: 0x00RRGGBB */
    eglUnlockSurfaceKHR(dpy, surf);
    return ret;
}

#ifndef FFEGL_NO_TEXTURE
/* ---- OpenGL textures ------------------------------------------------------ */

/* ffegl's state for one video's texture, kept with the video
   (reelcore_attach) and freed by reelcore_close() */
typedef struct {
    unsigned int tex;
    int tex_w, tex_h;
    uint8_t *rgba;                     /* copies: glTexSubImage2D reads from here */
    /* through an EGLImage (riscos-mesa 7pre12+): the texture uses a sprite's
       pixels and swscale writes each frame straight into them */
    int img_state;                     /* 0 = not checked, 1 = usable, -1 = not */
    uint8_t *spr_mem;                  /* allocation: sprite header at +4, pixels at +48 */
    EGLDisplay img_dpy;
    EGLImageKHR img;
    int img_bgr;                       /* sprite colour order: 0 TBGR (R,G,B,x), 1 TRGB */
} FFEGLTex;

/* ---- textures ---------------------------------------------------------- */

typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*target_texture_fn)(GLenum, void *);
static create_image_fn create_image;
static destroy_image_fn destroy_image;
static target_texture_fn target_texture;

#define SPRITE_HDR   44
#define SPRITE_TBGR  (1 | (90 << 1) | (90 << 14) | (6 << 27))   /* type 6, 0x00BBGGRR */
/* 0x00RRGGBB: a mode selector asking for 32bpp with ModeFlags TRGB, as
   riscos-mesa uses for its own TRGB sprites. The sprite is never plotted. */
static int trgb_selector[] = {
    1, 640, 480, 5, -1,         /* flags, x, y, log2bpp, frame rate */
    0, 0x4000,                  /* ModeFlags: TRGB */
    3, -1,                      /* NColour: 16M */
    4, 1, 5, 1,                 /* X/YEigFactor */
    -1
};

static int has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
    return 0;
}

/* Whether the current context can texture from EGLImages (GL_OES_EGL_image
   and EGL_KHR_image_pixmap); checked once per video (texture state). FFEGL_NO_EGLIMAGE=1 in
   the environment turns it off (then every frame is copied). */
static int image_usable(FFEGLTex *t)
{
    if (!t->img_state) {
        const char *no = getenv("FFEGL_NO_EGLIMAGE");
        EGLDisplay dpy = eglGetCurrentDisplay();
        t->img_state = -1;
        if (no && *no && *no != '0')
            return 0;
        if (dpy == EGL_NO_DISPLAY || !has_word((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image") ||
            !has_word(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_image_pixmap"))
            return 0;
        create_image   = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
        destroy_image  = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
        target_texture = (target_texture_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!create_image || !destroy_image || !target_texture)
            return 0;
        t->img_dpy = dpy;
        t->img_state = 1;
    }
    return t->img_state > 0;
}

/* The colour order of the current context's config (EGL_NATIVE_VISUAL_ID),
   so the texture has the same byte order as what it is drawn into. */
static int context_bgr(EGLDisplay dpy)
{
    EGLint id = 0, n = 0, visual = 0;
    EGLConfig cfg;
    EGLint attr[] = { EGL_CONFIG_ID, 0, EGL_NONE };
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || !eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id))
        return 0;
    attr[1] = id;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1 ||
        !eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual))
        return 0;
    return (visual & 0x4000) != 0;
}

/* Ends the texture's use of the sprite, destroys the image and frees the
   sprite, in that order. unlink: re-specify the texture first (it is given
   one black texel) if it still exists in the current context. */
static void texture_image_free(FFEGLTex *t, int unlink)
{
    if (!t->spr_mem)
        return;
    if (unlink && t->tex && eglGetCurrentContext() != EGL_NO_CONTEXT && glIsTexture(t->tex)) {
        static const uint8_t black[4];
        GLint old = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture(GL_TEXTURE_2D, old);
    }
    if (t->img != EGL_NO_IMAGE_KHR && destroy_image)
        destroy_image(t->img_dpy, t->img);
    t->img = EGL_NO_IMAGE_KHR;
    av_freep(&t->spr_mem);
    t->tex = 0;
}

static void set_texture_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* level 0 only */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Makes a sprite of the frame's size and binds it to tex through an
   EGLImage. 0, or < 0 if the image can't be made (then copies are used). */
static int texture_image_bind(FFEGLTex *t, int fw, int fh, unsigned int tex)
{
    int *spr;
    size_t bytes = (size_t)fw * fh * 4;

    texture_image_free(t, 1);
    /* header at +4 so the pixels (at +48) are 16-byte aligned */
    if (!(t->spr_mem = av_mallocz(4 + SPRITE_HDR + bytes)))
        return AVERROR(ENOMEM);
    t->img_bgr = context_bgr(t->img_dpy);
    spr = (int *)(t->spr_mem + 4);
    spr[0] = SPRITE_HDR + (int)bytes;           /* offset to the next sprite */
    memcpy(&spr[1], "ffegl\0\0\0\0\0\0\0", 12);
    spr[4] = fw - 1;                      /* width in words - 1 */
    spr[5] = fh - 1;
    spr[6] = 0;                                 /* first bit used */
    spr[7] = 31;                                /* last bit used */
    spr[8] = SPRITE_HDR;                        /* image */
    spr[9] = SPRITE_HDR;                        /* mask = image: none */
    spr[10] = t->img_bgr ? (int)(intptr_t)trgb_selector : SPRITE_TBGR;
    t->img = create_image(t->img_dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)spr, NULL);
    if (t->img == EGL_NO_IMAGE_KHR) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: eglCreateImageKHR failed (0x%x), copying frames\n", eglGetError());
        av_freep(&t->spr_mem);
        return -1;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    target_texture(GL_TEXTURE_2D, t->img);
    if (glGetError() != GL_NO_ERROR) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: glEGLImageTargetTexture2DOES failed, copying frames\n");
        texture_image_free(t, 0);
        return -1;
    }
    set_texture_params();
    t->tex = tex;
    t->tex_w = fw;
    t->tex_h = fh;
    return 0;
}

static void tex_release(void *p)
{
    FFEGLTex *t = p;
    texture_image_free(t, 1);
    av_free(t->rgba);
    av_free(t);
}

static FFEGLTex *tex_state(ReelCore *v)
{
    FFEGLTex *t = reelcore_attachment(v);
    if (!t && (t = av_mallocz(sizeof(*t))) != NULL) {
        t->img = EGL_NO_IMAGE_KHR;
        reelcore_attach(v, t, tex_release);
    }
    return t;
}

int ffegl_texture(ReelCore *v, unsigned int tex, unsigned int *tex_out)
{
    FFEGLTex *t = tex_state(v);
    int ret, fresh = 0, fw, fh;

    if (reelcore_frame_size(v, &fw, &fh) < 0)
        return AVERROR(EAGAIN);
    if (!t)
        return AVERROR(ENOMEM);
    if (!tex) {
        glGenTextures(1, &tex);
        if (!tex)
            return AVERROR_EXTERNAL;
        fresh = 1;
    }
    if (tex_out)
        *tex_out = tex;

    /* EGLImage: swscale writes straight into the texture's pixels */
    if (image_usable(t)) {
        if (!t->spr_mem || fresh || tex != t->tex || t->tex_w != fw || t->tex_h != fh) {
            glGetError();                       /* clear any old error */
            if (texture_image_bind(t, fw, fh, tex) < 0)
                t->img_state = -1;              /* copy from now on */
        }
        if (t->spr_mem)
            return reelcore_draw_pixels(v, t->spr_mem + 4 + SPRITE_HDR, fw * 4, fw, fh,
                                        t->img_bgr, REELCORE_STRETCH);
        fresh = 1;                              /* the texture needs specifying again */
    }

    /* otherwise: convert into a buffer and copy it into the texture */
    if (!t->rgba || t->tex_w != fw || t->tex_h != fh) {
        av_freep(&t->rgba);
        t->rgba = av_malloc((size_t)fw * fh * 4);
        if (!t->rgba)
            return AVERROR(ENOMEM);
        t->tex_w = fw;
        t->tex_h = fh;
        fresh = 1;
    }
    if ((ret = reelcore_draw_pixels(v, t->rgba, fw * 4, fw, fh, 0, REELCORE_STRETCH)) < 0)
        return ret;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fresh || tex != t->tex) {
        set_texture_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
        t->tex = tex;
    } else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh,
                        GL_RGBA, GL_UNSIGNED_BYTE, t->rgba);
    return 0;
}
#endif /* FFEGL_NO_TEXTURE */
