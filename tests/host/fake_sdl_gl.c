/* Host fakes of the SDL audio and OpenGL calls ffegl uses, with a fake
 * clock: fake_time (seconds) is what av_gettime_relative() returns (ffegl
 * is linked with -Wl,--wrap=av_gettime_relative), and the fake sound device
 * plays queued bytes at the real rate while unpaused. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include "fake_sdl_gl.h"

double fake_time;
int fake_audio_fail, fake_audio_open, fake_audio_paused = 1;
int fake_audio_stall;                   /* the device opens but never plays */
double fake_queued_total;               /* bytes ever queued */
static double played, last_t;           /* bytes played, at fake time last_t */
static int bps = 48000 * 4;

int64_t __wrap_av_gettime_relative(void) { return (int64_t)(fake_time * 1e6); }

static void advance(void)
{
    if (!fake_audio_paused && !fake_audio_stall && fake_time > last_t) {
        played += (fake_time - last_t) * bps;
        if (played > fake_queued_total) played = fake_queued_total;
    }
    last_t = fake_time;
}

SDL_bool SDL_WasInit(Uint32 f) { return 0; }
int SDL_InitSubSystem(Uint32 f) { return fake_audio_fail ? -1 : 0; }
const char *SDL_GetError(void) { return "RISC OS audio: SharedSoundBuffer/StreamManager not loaded (fake)"; }
const char *SDL_GetCurrentAudioDriver(void) { return "fake"; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *d, int c, const SDL_AudioSpec *w, SDL_AudioSpec *h, int ch)
{
    if (fake_audio_fail) return 0;
    *h = *w;
    bps = h->freq * 4;
    fake_audio_open = 1; fake_audio_paused = 1;
    fake_queued_total = played = 0; last_t = fake_time;
    return 7;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { fake_audio_open = 0; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) { advance(); fake_audio_paused = p; }
int SDL_QueueAudio(SDL_AudioDeviceID d, const void *data, Uint32 len) { advance(); fake_queued_total += len; return 0; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d) { advance(); return (Uint32)(fake_queued_total - played); }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID d) { advance(); played = fake_queued_total; }
void SDL_MixAudioFormat(Uint8 *dst, const Uint8 *src, SDL_AudioFormat f, Uint32 len, int vol)
{
    const int16_t *s = (const int16_t *)src; int16_t *o = (int16_t *)dst;
    for (Uint32 i = 0; i < len / 2; i++) o[i] = (int16_t)(s[i] * vol / 128);
}

#ifndef FAKE_SDL_ONLY     /* (mesa/: the real riscos-mesa GL and EGL) */
/* GL: one texture (42), whose last upload is kept */
unsigned char *fake_tex; int fake_tex_w, fake_tex_h, fake_tex_images, fake_tex_subimages;
static GLuint bound;
void glGenTextures(GLsizei n, GLuint *t) { *t = 42; }
void glBindTexture(GLenum t, GLuint tex) { bound = tex; }
void glPixelStorei(GLenum p, GLint v) {}
void glTexParameteri(GLenum t, GLenum p, GLint v) {}
GLboolean glIsTexture(GLuint t) { return t == 42; }
void glGetIntegerv(GLenum p, GLint *v) { *v = p == GL_TEXTURE_BINDING_2D ? (GLint)bound : 0; }
GLenum glGetError(void) { return GL_NO_ERROR; }
void glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px)
{
    free(fake_tex); fake_tex = malloc((size_t)w * h * 4); memcpy(fake_tex, px, (size_t)w * h * 4);
    fake_tex_w = w; fake_tex_h = h; fake_tex_images++;
    if (bound == fake_tex_linked) fake_tex_linked = 0;      /* re-specifying ends an EGLImage link */
}
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *px)
{
    memcpy(fake_tex, px, (size_t)w * h * 4); fake_tex_subimages++;
}

/* EGLImage, as riscos-mesa 7pre12 has it: EGL_KHR_image_pixmap on 32bpp
   sprites and GL_OES_EGL_image. fake_gl_eglimage = 0 advertises only a
   look-alike extension name. One image at a time. */
int fake_gl_eglimage, fake_gl_context = 1, fake_img_fail;
int fake_images, fake_img_binds, fake_img_w, fake_img_h, fake_img_bgr, fake_destroyed_linked;
GLuint fake_tex_linked;
unsigned char *fake_img_pixels;
static int *img_sprite;

const GLubyte *glGetString(GLenum n)
{
    if (n != GL_EXTENSIONS) return (const GLubyte *)"fake";
    return (const GLubyte *)(fake_gl_eglimage ? "GL_ARB_multitexture GL_OES_EGL_image GL_EXT_bgra"
                                              : "GL_ARB_multitexture GL_OES_EGL_image_external GL_EXT_bgra");
}
EGLDisplay eglGetCurrentDisplay(void) { return fake_gl_context ? (EGLDisplay)1 : EGL_NO_DISPLAY; }
EGLContext eglGetCurrentContext(void) { return fake_gl_context ? (EGLContext)5 : EGL_NO_CONTEXT; }
EGLBoolean eglQueryContext(EGLDisplay d, EGLContext c, EGLint a, EGLint *v) { *v = 3; return EGL_TRUE; }
const char *eglQueryString(EGLDisplay d, EGLint n)
{
    return "EGL_KHR_image EGL_KHR_image_base EGL_KHR_image_pixmap EGL_KHR_lock_surface";
}
static int sprite_bgr(const int *spr)      /* 0 TBGR, 1 TRGB, -1 not a 32bpp sprite mode */
{
    unsigned m = (unsigned)spr[10];
    if (m == (1 | (90 << 1) | (90 << 14) | (6u << 27))) return 0;
    if (m > 255 && !(m & 1)) {                 /* a mode selector */
        const int *sel = (const int *)(uintptr_t)m;
        int flags = 0;
        if (sel[0] != 1 || sel[3] != 5) return -1;
        for (const int *p = sel + 5; *p != -1; p += 2) if (p[0] == 0) flags = p[1];
        return (flags & 0x4000) ? 1 : -1;
    }
    return -1;
}
EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer buf, const EGLint *a)
{
    int *spr = (int *)buf;
    if (fake_img_fail || c != EGL_NO_CONTEXT || target != EGL_NATIVE_PIXMAP_KHR || !spr) return EGL_NO_IMAGE_KHR;
    if (spr == img_sprite) return EGL_NO_IMAGE_KHR;                 /* EGL_BAD_ACCESS */
    if (spr[6] != 0 || spr[7] != 31 || spr[4] < 0 || spr[5] < 0 || spr[8] < 44 ||
        spr[0] < spr[8] + (spr[4] + 1) * 4 * (spr[5] + 1) || sprite_bgr(spr) < 0)
        return EGL_NO_IMAGE_KHR;                                    /* EGL_BAD_NATIVE_PIXMAP */
    if (fake_images) return EGL_NO_IMAGE_KHR;                       /* the fake keeps one */
    img_sprite = spr;
    fake_images++;
    fake_img_w = spr[4] + 1; fake_img_h = spr[5] + 1; fake_img_bgr = sprite_bgr(spr);
    fake_img_pixels = (unsigned char *)spr + spr[8];
    return (EGLImageKHR)spr;
}
EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR img)
{
    if (!fake_images || (int *)img != img_sprite) return EGL_FALSE;
    if (fake_tex_linked) fake_destroyed_linked++;
    fake_images--; img_sprite = NULL;
    return EGL_TRUE;
}
static void target_texture(GLenum t, void *img)
{
    if ((int *)img != img_sprite || !img) return;
    fake_tex_linked = bound; fake_img_binds++;
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *n)
{
    if (!strcmp(n, "eglCreateImageKHR"))  return (__eglMustCastToProperFunctionPointerType)eglCreateImageKHR;
    if (!strcmp(n, "eglDestroyImageKHR")) return (__eglMustCastToProperFunctionPointerType)eglDestroyImageKHR;
    if (!strcmp(n, "glEGLImageTargetTexture2DOES")) return (__eglMustCastToProperFunctionPointerType)target_texture;
    return NULL;
}
#endif
