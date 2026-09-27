/* Host fakes of the RISC OS SWIs and riscos-mesa EGL calls that
 * libavdevice/riscos_egl.c uses, so its logic runs on Linux.
 * The screen is FAKE_SW x FAKE_SH pixels (eig 1), the desktop is running
 * when fake_desktop is set, and every EGL window surface is plain memory
 * whose last shown frame is kept for the test to look at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_riscos.h>
#include "fake_riscos.h"

int fake_desktop, fake_taskwindow, fake_visual, fake_close_after = -1, fake_escape_after = -1;
int fake_swaps, fake_locked, fake_tasks_open, fake_windows_open, fake_force_redraws;
int fake_swap_interval = -1, fake_render_buffer;
int fake_win_w, fake_win_h;               /* visible area of the last window, pixels */
unsigned char *fake_shown;                /* copy of the surface at the last swap */
int fake_surf_w, fake_surf_h, fake_surf_pitch;

static unsigned char *mem;
int fake_scr_w = FAKE_SW, fake_scr_h = FAKE_SH;       /* the screen, pixels */
int fake_wa[4] = { -1, -1, -1, -1 };                  /* work area surface: x, y, w, h */
int fake_plots, fake_surfaces;

#ifndef FAKE_EGL_ONLY     /* (reel_test has its own scripted Wimp) */
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake error" };
    switch (swi) {
    case OS_ReadVduVariables: {
        int *vars = (int *)(long)in->r[0], *vals = (int *)(long)in->r[1];
        for (; *vars != -1; vars++, vals++)
            *vals = *vars == 4 || *vars == 5 ? 1 : *vars == 11 ? FAKE_SW - 1 :
                    *vars == 12 ? FAKE_SH - 1 : 0;
        return NULL;
    }
    case Wimp_ReadSysInfo:
        out->r[0] = fake_desktop ? 3 : 0;
        return NULL;
    case Wimp_Initialise:       /* in a TaskWindow: "Window Manager is currently in use" */
        if (!fake_desktop || fake_taskwindow) return &err;
        fake_tasks_open++;
        out->r[1] = 0x1234;
        return NULL;
    case Wimp_CreateWindow: {
        int *wb = (int *)(long)in->r[1];
        fake_win_w = (wb[2] - wb[0]) >> 1;
        fake_win_h = (wb[3] - wb[1]) >> 1;
        fake_windows_open++;
        out->r[0] = 0x5000;
        return NULL;
    }
    case Wimp_Poll: {
        int *block = (int *)(long)in->r[1];
        /* a close request after fake_close_after frames, else null events */
        if (fake_close_after >= 0 && fake_swaps >= fake_close_after) {
            fake_close_after = -2;                /* once */
            block[0] = 0x5000;
            out->r[0] = 3;
        } else
            out->r[0] = 0;
        return NULL;
    }
    case OS_Byte:
        if (in->r[0] == 121)
            out->r[1] = (fake_escape_after >= 0 && fake_swaps >= fake_escape_after) ? 0xFF : 0;
        return NULL;
    case 0x43380:               /* TaskWindow_TaskInfo 0 */
        out->r[0] = fake_taskwindow;
        return NULL;
    case Wimp_DeleteWindow: fake_windows_open--; return NULL;
    case Wimp_CloseDown:    fake_tasks_open--;   return NULL;
    case Wimp_ForceRedraw:  fake_force_redraws++; return NULL;
    default:
        return NULL;
    }
}

#endif

EGLDisplay eglGetDisplay(EGLNativeDisplayType d) { return (EGLDisplay)1; }
EGLBoolean eglInitialize(EGLDisplay d, EGLint *ma, EGLint *mi) { if (ma) *ma = 1; if (mi) *mi = 4; return EGL_TRUE; }
#ifdef FAKE_EGL_ONLY     /* (otherwise fake_sdl_gl.c has it) */
const char *eglQueryString(EGLDisplay d, EGLint name) { return name == EGL_VENDOR ? "fake" : "1.4 (fake)"; }
#endif
EGLBoolean eglTerminate(EGLDisplay d) { return EGL_TRUE; }
EGLint eglGetError(void) { return EGL_SUCCESS; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *num)
{
    for (; *a != EGL_NONE; a += 2)
        if (a[0] == EGL_SURFACE_TYPE && !(a[1] & EGL_LOCK_SURFACE_BIT_KHR)) { *num = 0; return EGL_TRUE; }
    *c = (EGLConfig)1; *num = 1; return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v)
{
    *v = attr == EGL_NATIVE_VISUAL_ID ? fake_visual : 0;
    return EGL_TRUE;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a)
{
    fake_render_buffer = 0;
    fake_wa[0] = fake_wa[1] = fake_wa[2] = fake_wa[3] = -1;
    for (; a && *a != EGL_NONE; a += 2) {
        if (a[0] == EGL_RENDER_BUFFER) fake_render_buffer = a[1];
        if (a[0] >= EGL_WORK_AREA_X_RISCOS && a[0] <= EGL_WORK_AREA_HEIGHT_RISCOS)
            fake_wa[a[0] - EGL_WORK_AREA_X_RISCOS] = a[1];
    }
    if (mem) return EGL_NO_SURFACE;                   /* the fake keeps one surface */
    fake_surfaces++;
    if ((long)w == -1)          { fake_surf_w = fake_scr_w; fake_surf_h = fake_scr_h; }
    else if (fake_wa[2] > 0)    { fake_surf_w = fake_wa[2]; fake_surf_h = fake_wa[3]; }
    else                        { fake_surf_w = fake_win_w; fake_surf_h = fake_win_h; }
    fake_surf_pitch = fake_surf_w * 4;
    mem = calloc(fake_surf_pitch, fake_surf_h);
    memset(mem, 0x55, fake_surf_pitch * fake_surf_h);   /* not black: borders must be drawn */
    return (EGLSurface)2;
}
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { free(mem); mem = NULL; fake_surfaces--; return EGL_TRUE; }
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { fake_swap_interval = i; return EGL_TRUE; }
EGLBoolean eglLockSurfaceKHR(EGLDisplay d, EGLSurface s, const EGLint *a)
{
    if (fake_locked) return EGL_FALSE;
    fake_locked = 1; return EGL_TRUE;
}
EGLBoolean eglUnlockSurfaceKHR(EGLDisplay d, EGLSurface s) { fake_locked = 0; return EGL_TRUE; }
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *v)
{
    switch (attr) {
    case EGL_BITMAP_POINTER_KHR: *v = fake_locked ? (EGLint)(long)mem : 0; break;
    case EGL_BITMAP_PITCH_KHR:   *v = fake_surf_pitch; break;
    case EGL_WIDTH:              *v = fake_surf_w; break;
    case EGL_HEIGHT:             *v = fake_surf_h; break;
    case EGL_BITMAP_PIXEL_BLUE_OFFSET_KHR: *v = fake_visual == 0x4000 ? 0 : 16; break;
    default:                     *v = 0;
    }
    return EGL_TRUE;
}
EGLBoolean eglQuerySurface64KHR(EGLDisplay d, EGLSurface s, EGLint attr, EGLAttribKHR *v)
{
    *v = attr == EGL_BITMAP_POINTER_KHR && fake_locked ? (EGLAttribKHR)mem : 0;
    return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    if (fake_locked) return EGL_FALSE;
    fake_swaps++;
    free(fake_shown);
    fake_shown = malloc(fake_surf_pitch * fake_surf_h);
    memcpy(fake_shown, mem, fake_surf_pitch * fake_surf_h);
    return EGL_TRUE;
}
EGLBoolean eglRedrawWindowRISCOS(EGLDisplay d, int *block) { return EGL_TRUE; }
EGLBoolean eglPlotSurfaceRISCOS(EGLDisplay d, EGLSurface s, const int *block)
{
    if (!mem) return EGL_FALSE;
    fake_plots++;
    return EGL_TRUE;
}
