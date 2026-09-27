/*
 * videowin - a video in a desktop window, through ffegl and EGL.
 *
 *   videowin [-f] [-loop] FILE
 *
 * The window is a Wimp window whose handle is the EGL native window;
 * ffegl_draw_surface() writes each new frame into the EGL surface
 * (EGL_KHR_lock_surface, no OpenGL) and eglSwapBuffers() shows it.
 * -f uses the whole screen instead.
 *
 * Keys: Space pause, Left/Right 10 s back/on, Up/Down 1 min, Escape or Q
 * quit (or the close icon).
 *
 * Part of riscos-ffmpeg: an example for ffegl. MIT licence.
 */
#define EGL_EGLEXT_PROTOTYPES 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_riscos.h>
#include "ffegl.h"

#define TASK_WORD 0x4B534154

static int task, handle;
static char title[160];

static int vdu_var(int var)
{
    int in[2] = { var, -1 }, out[1] = { 0 };
    _kernel_swi_regs r;
    r.r[0] = (int)in; r.r[1] = (int)out;
    _kernel_swi(OS_ReadVduVariables, &r, &r);
    return out[0];
}

static int open_window(int width, int height)
{
    int wb[23], block[8];
    int xeig = vdu_var(4), yeig = vdu_var(5);
    int sw = (vdu_var(11) + 1) << xeig, sh = (vdu_var(12) + 1) << yeig;
    int ow = width << xeig, oh = height << yeig;
    _kernel_swi_regs r;

    if (ow > sw - 64) { oh = oh * (sw - 64) / ow; ow = sw - 64; }
    if (oh > sh - 96) { ow = ow * (sh - 96) / oh; oh = sh - 96; }
    memset(wb, 0, sizeof(wb));
    wb[0] = (sw - ow) / 2; wb[1] = (sh - oh) / 2; wb[2] = wb[0] + ow; wb[3] = wb[1] + oh;
    wb[6] = -1;
    wb[7] = (int)0xAF000002u;             /* moveable; back, close, title, toggle, adjust size */
    wb[8] = 7 | (2 << 8) | (7 << 16) | (4 << 24);
    wb[9] = 3 | (1 << 8) | (12 << 16);
    wb[11] = -sh; wb[12] = sw;
    wb[14] = 0x07000119;                  /* indirected text title */
    wb[16] = 1;
    wb[18] = (int)title; wb[19] = -1; wb[20] = sizeof(title);
    r.r[1] = (int)wb;
    if (_kernel_swi(Wimp_CreateWindow, &r, &r))
        return 0;
    handle = r.r[0];
    block[0] = handle;
    memcpy(&block[1], wb, 7 * sizeof(int));
    r.r[1] = (int)block;
    _kernel_swi(Wimp_OpenWindow, &r, &r);
    return 1;
}

static void set_title(const char *name, FFEGLVideo *v)
{
    char t[sizeof(title)];
    int p = (int)ffegl_position(v), d = (int)ffegl_duration(v);
    _kernel_swi_regs r;
    snprintf(t, sizeof(t), "%.100s  %d:%02d / %d:%02d%s", name, p / 60, p % 60, d / 60, d % 60,
             ffegl_paused(v) ? "  (paused)" : "");
    if (!strcmp(t, title) || !task)
        return;
    strcpy(title, t);
    r.r[0] = handle; r.r[1] = TASK_WORD; r.r[2] = 3;     /* redraw the title bar */
    _kernel_swi(Wimp_ForceRedraw, &r, &r);
}

/* 1 = go on, 0 = quit */
static int key(FFEGLVideo *v, int k)
{
    switch (k) {
    case 0x1B: case 'q': case 'Q': return 0;
    case ' ':   ffegl_pause(v, !ffegl_paused(v)); break;
    case 0x18C: ffegl_seek(v, ffegl_position(v) - 10); break;   /* Left */
    case 0x18D: ffegl_seek(v, ffegl_position(v) + 10); break;   /* Right */
    case 0x18E: ffegl_seek(v, ffegl_position(v) - 60); break;   /* Down */
    case 0x18F: ffegl_seek(v, ffegl_position(v) + 60); break;   /* Up */
    default:
        if (task) {
            _kernel_swi_regs r;
            r.r[0] = k;
            _kernel_swi(Wimp_ProcessKey, &r, &r);
        }
    }
    return 1;
}

/* Full screen outside the Wimp: a key if one is waiting (OS_Byte 129,
   no wait), else -1. Only the plain keys: Escape, Q and Space. */
static int inkey(void)
{
    _kernel_swi_regs r;
    r.r[0] = 129; r.r[1] = 0; r.r[2] = 0;
    _kernel_swi(OS_Byte, &r, &r);
    if (r.r[2] == 0x1B) {                 /* Escape: acknowledge it */
        r.r[0] = 126;
        _kernel_swi(OS_Byte, &r, &r);
        return 0x1B;
    }
    return r.r[2] == 0 ? r.r[1] : -1;
}

/* The file's leaf name, RISC OS style (a.b.clip/mp4) or Unix (a/b/clip.mp4). */
static const char *leaf(const char *f)
{
    const char *dot = strrchr(f, '.'), *slash = strrchr(f, '/');
    if (strchr(f, ':') || strchr(f, '$') || (dot && slash && slash > dot))
        return dot ? dot + 1 : f;       /* RISC OS: after the last '.' */
    return slash ? slash + 1 : f;
}

int main(int argc, char **argv)
{
    static const int messages[] = { 0 };
    const char *file = NULL, *name;
    int full = 0, flags = 0, running = 1, block[64];
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLSurface surf;
    EGLint n;
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_LOCK_SURFACE_BIT_KHR,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    FFEGLVideo *v;
    _kernel_swi_regs r;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) full = 1;
        else if (!strcmp(argv[i], "-loop")) flags |= FFEGL_LOOP;
        else file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "usage: videowin [-f] [-loop] FILE\n");
        return 1;
    }
    name = leaf(file);

    v = ffegl_open(file, flags);
    if (!v) {
        fprintf(stderr, "videowin: %s\n", ffegl_last_error());
        return 1;
    }
    snprintf(title, sizeof(title), "%.100s", name);
    if (!full) {
        r.r[0] = 380; r.r[1] = TASK_WORD; r.r[2] = (int)"Video"; r.r[3] = (int)messages;
        if (!_kernel_swi(Wimp_Initialise, &r, &r))
            task = r.r[1];
        if (!task || !open_window(ffegl_width(v), ffegl_height(v)))
            full = 1;
    }
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n) {
        fprintf(stderr, "videowin: no EGL config\n");
        return 1;
    }
    surf = eglCreateWindowSurface(dpy, cfg,
                                  full ? EGL_RISCOS_SCREEN_WINDOW : (EGLNativeWindowType)handle, NULL);
    if (surf == EGL_NO_SURFACE) {
        fprintf(stderr, "videowin: no EGL surface (0x%x)\n", eglGetError());
        return 1;
    }

    while (running) {
        int res = ffegl_update(v);
        if (res < 0 || res == FFEGL_END)
            break;
        if (res == FFEGL_NEW_FRAME) {
            ffegl_draw_surface(v, dpy, surf, 0, 0, 0, 0, 0);
            eglSwapBuffers(dpy, surf);
        }
        if (!task) {                       /* full screen, single tasking */
            int k = inkey();
            if (k >= 0)
                running = key(v, k);
            continue;
        }
        set_title(name, v);
        /* Paused: sleep until an event. Playing: back at once (null events). */
        r.r[0] = ffegl_paused(v) ? 1 : 0;
        r.r[1] = (int)block;
        if (_kernel_swi(Wimp_Poll, &r, &r))
            break;
        switch (r.r[0]) {
        case 1:                            /* Redraw_Window_Request */
            if (!eglRedrawWindowRISCOS(dpy, block)) {
                r.r[1] = (int)block;
                _kernel_swi(Wimp_RedrawWindow, &r, &r);
                while (r.r[0]) { r.r[1] = (int)block; _kernel_swi(Wimp_GetRectangle, &r, &r); }
            }
            break;
        case 2:                            /* Open_Window_Request (moved, resized) */
            r.r[1] = (int)block;
            _kernel_swi(Wimp_OpenWindow, &r, &r);
            /* the surface follows the window at the next swap: redraw now */
            if (ffegl_paused(v)) {
                ffegl_draw_surface(v, dpy, surf, 0, 0, 0, 0, 0);
                eglSwapBuffers(dpy, surf);
            }
            break;
        case 3:                            /* Close_Window_Request */
            running = 0;
            break;
        case 6:                            /* Mouse_Click: the input focus */
            r.r[0] = handle; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0; r.r[4] = 1 << 25; r.r[5] = -1;
            _kernel_swi(Wimp_SetCaretPosition, &r, &r);
            break;
        case 8:                            /* Key_Pressed */
            running = key(v, block[6]);
            break;
        case 17: case 18:                  /* Message_Quit */
            if (block[4] == 0)
                running = 0;
            break;
        }
    }

    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    ffegl_close(v);
    if (task) {
        r.r[0] = task; r.r[1] = TASK_WORD;
        _kernel_swi(Wimp_CloseDown, &r, &r);
    } else if (full) {
        r.r[0] = -1; r.r[1] = 0; r.r[2] = 0; r.r[3] = 1 << 30; r.r[4] = 1 << 30;
        _kernel_swi(Wimp_ForceRedraw, &r, &r);
    }
    return 0;
}
