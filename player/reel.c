/*
 * Reel - a video player for the RISC OS desktop, on FFmpeg (via reelcore).
 *
 * A normal Wimp application: an icon on the icon bar, and one window with
 * the picture above a row of controls (play/pause, back and forward 10 s,
 * a position bar you can click, the time, the volume, full screen). Drop a
 * video file on the icon or the window, or double-click one in the Filer
 * while Reel is loaded; several dropped at once make a playlist. Full
 * screen is a window with no furniture that covers the screen. The mini
 * player is another, small one above the icon bar (Play/Pause, the position
 * bar, Normal), optionally kept on top while playing.
 *
 * Playback options (window menu): window size, picture size, speed
 * (atempo in reelcore), sound track, A-B repeat, loop, fast decoding,
 * vsync full screen. Both windows have a resize grip in the bottom right
 * corner (the Wimp's own size drag, Wimp_DragBox type 2; the windows have
 * no scroll bars, so no size icon); the mini player keeps the video's
 * shape. The mini player decodes with REELCORE_FAST_LIGHT (no deblocking
 * of pictures nothing is predicted from). The volume, keep on top, the
 * mini player's place and size and where each file was stopped are kept
 * in Choices:Reel (ReelEGL).
 *
 * reelcore (reelcore/) is the player core: it reads, decodes, plays the
 * sound (SharedSoundBuffer, the clock) and says when a picture is due.
 * Drawing, Reel: each new frame is converted and scaled by swscale (NEON)
 * with reelcore_draw_pixels() into a 32bpp sprite of the picture area's
 * size, letterboxed, in the screen's own colour order, and plotted 1:1 with
 * OS_SpriteOp (clipped to the picture area). ReelEGL (-DREEL_EGL): into an
 * EGL surface instead, with ffegl (ffegl/, the EGL layer on reelcore).
 * No threads of our own: decoding happens on null events.
 *
 * Keys (window or full screen): Space pause, Left/Right 10 s, Up/Down 1 min,
 * F full screen on/off, Escape leaves full screen, M the mini player, A A-B
 * repeat, N/P next/previous in the playlist, Q closes the video.
 *
 * Media info (window menu, or I): a window with what's in the file (codecs,
 * sizes, rates) and, every second while playing, "stats for nerds": pictures
 * shown and decoded a second, decoding time and speed, conversion and
 * drawing time, late and skipped frames, the sound queue, reading speed.
 *
 * Log: <Wimp$ScrapDir>.ReelLog (ReelEGLLog), written as it goes and
 * started afresh each time Reel starts: the modules and screen found, each
 * file opened, what was done, errors, FFmpeg's messages, and once a second
 * while playing, the state of the picture and the sound (reelcore_debug).
 * Reel$Log (ReelEGL$Log) names another file, or "off". "Log" on the icon
 * bar menu opens it.
 *
 * Info on the icon bar menu: the standard About this program window (name,
 * purpose, author, version from common/version.h), as a submenu.
 *
 * Part of riscos-ffmpeg. GPL v2 or later.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "reelcore.h"
#include "../common/version.h"
#include "../common/proginfo.h"   /* Info: the standard About this program window */

/* Big heap in a dynamic area (the default would share the WimpSlot) */
#ifdef REEL_EGL
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_riscos.h>
#include "ffegl.h"                      /* ffegl_draw_surface: reelcore's pictures into EGL */
const char *const __dynamic_da_name = "ReelEGL Heap";
#else
const char *const __dynamic_da_name = "Reel Heap";
#endif
int __dynamic_da_max_size = 512 << 20;

#define OS_WriteN              0x46
#define OS_ReadModeVariable    0x35
#define OS_SpriteOp            0x2E
#define OS_ReadMonotonicTime   0x42
#define OS_ScreenMode          0x65
#define OS_GetEnv              0x10
#define Wimp_Initialise        0x400C0
#define Wimp_CreateWindow      0x400C1
#define Wimp_CreateIcon        0x400C2
#define Wimp_DeleteWindow      0x400C3
#define Wimp_OpenWindow        0x400C5
#define Wimp_CloseWindow       0x400C6
#define Wimp_Poll              0x400C7
#define Wimp_PollIdle          0x400E1
#define Wimp_RedrawWindow      0x400C8
#define Wimp_UpdateWindow      0x400C9
#define Wimp_GetRectangle      0x400CA
#define Wimp_GetWindowState    0x400CB
#define Wimp_SetIconState      0x400CD
#define Wimp_GetPointerInfo    0x400CF
#define Wimp_DragBox           0x400D0
#define Wimp_ForceRedraw       0x400D1
#define Wimp_SetCaretPosition  0x400D2
#define Wimp_CreateMenu        0x400D4
#define Wimp_ProcessKey        0x400DC
#define Wimp_CloseDown         0x400DD
#define Wimp_ReportError       0x400DF
#define Wimp_SendMessage       0x400E7
#define Wimp_ResizeIcon        0x400FC
#define Wimp_SetColour         0x400E6
#define Wimp_TextOp            0x400F9
#define OS_Plot                0x45
#define MimeMap_Translate      0x50B00
#define TaskManager_EnumerateTasks 0x42681

#define MSG_QUIT        0
#define MSG_DATALOAD    3
#define MSG_DATALOADACK 4
#define MSG_DATAOPEN    5
#define MSG_PREQUIT     8
#define MSG_MODECHANGE  0x400C1

#ifdef REEL_EGL
#define APP     "ReelEGL"
#define ICON    "!reelegl"
#define PURPOSE "Video player, EGL"
#else
#define APP     "Reel"
#define ICON    "!reel"
#define PURPOSE "Video player"
#endif
#define CH      64          /* height of the controls row, OS units */
#define GAP     4
#define MIN_W   1132        /* narrowest window: the position bar still has room */
#define W_PLAY  104         /* control widths, OS units */
#define W_SKIP  96
#define W_TIME  264         /* room for "12:34 / 1:45:00" or "1:23 / 3:45 1.5x AB" */
#define W_FULL  88
#define W_VOL   128         /* the volume bar */
#define W_GRIP  32          /* the resize grip, bottom right (both windows) */
#define RIGHT(vw) ((vw) - GAP - W_GRIP - GAP)   /* where the controls end: the grip is after them */
/* the mini player: a small window with no furniture above the icon bar */
#define MINI_W  640         /* its width at first, OS units (320 pixels on most screens) */
#define MINI_MIN_W 480      /* narrowest when resized (the grip) */
#define W_NORM  120         /* its "Normal" button: back to the full window */
#define MINI_EDGE 32        /* its default gap from the screen's right edge */
#define MINI_LIFT 16        /* ... and above the icon bar */

static int mini;            /* the mini player is showing (the controls lay out for it) */

static void apply_fast(void);

/* The position bar's track, in work area x, for the window's width */
static void track_x(int vw, int *x0, int *x1)
{
    if (mini) {                         /* Play, the bar, Normal */
        *x0 = GAP + W_PLAY + GAP * 2;
        *x1 = RIGHT(vw) - W_NORM - GAP * 2;
    } else {
        *x0 = GAP + W_PLAY + GAP + W_SKIP + GAP + W_SKIP + GAP * 2;
        *x1 = RIGHT(vw) - W_FULL - GAP - W_VOL - GAP - W_TIME - GAP;
    }
    if (*x1 < *x0 + 16)
        *x1 = *x0 + 16;
}

enum { I_PLAY, I_BACK, I_FWD, I_TRACK, I_FILL, I_TIME, I_FULL, I_VOL, I_VOLFILL, I_GRIP, N_ICONS };
#define I_NORMAL N_ICONS            /* the mini player's own extra icon */

/* the volume bar, in work area x, for the window's width */
static void vol_x(int vw, int *x0, int *x1)
{
    *x1 = RIGHT(vw) - W_FULL - GAP;
    *x0 = *x1 - W_VOL;
}

/* Picture sizes (window menu, Picture) */
enum { PIC_FIT, PIC_FILL, PIC_ORIGINAL, PIC_STRETCH, N_PIC };
static const char *const pic_names[N_PIC] = { "Fit", "Fill (crop)", "Original size", "Stretch" };
static const int pic_flags_of[N_PIC] = { 0, REELCORE_FILL, REELCORE_ORIGINAL, REELCORE_STRETCH };

/* Window sizes (window menu, Window size): the video's size times these, or as big as fits */
enum { SIZE_HALF, SIZE_ACTUAL, SIZE_DOUBLE, SIZE_SCREEN, N_SIZE };
static const char *const size_names[N_SIZE] = { "Half (50%)", "Actual size (100%)", "Double (200%)", "Fit the screen" };

/* Deinterlacing (window menu, Deinterlace): in the menu's order */
static const char *const deint_names[3] = { "Auto", "On", "Off" };
static const int deint_modes[3] = { REELCORE_DEINT_AUTO, REELCORE_DEINT_ON, REELCORE_DEINT_OFF };

/* Speeds (window menu, Speed) */
#define N_SPEED 6
static const double speeds[N_SPEED] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
static const char *const speed_names[N_SPEED] = { "0.5x", "0.75x", "Normal", "1.25x", "1.5x", "2x" };

#define LIST_MAX 64                     /* files in the playlist */

typedef struct { int x0, y0, x1, y1; } box_t;

static struct {
    int task, bar_icon;
    int win, full;                      /* the window showing the video (the normal one or the
                                           mini player); the full screen window (0 = none) */
    int main_win, mini_win;             /* the normal window; the mini player (0 = not made yet) */
    int main_st[9];                     /* where the normal window was, while the mini player shows */
    int ontop;                          /* the mini player keeps itself on top while playing */
    int ontop_cs;                       /* when it last looked */
    int mini_w;                         /* the mini player's width, OS units (the grip changes it) */
    int mini_right, mini_bottom;        /* its place: gap from the screen's right edge; bottom
                                           (-1 = just above the icon bar) */
    int fullscreen;                     /* showing full screen */
    int loop;
    ReelCore *v;
    char file[256], title[64], time_text[40], play_text[8];
    int ended;
    /* screen */
    int xeig, yeig, log2bpp, trgb, scr_w, scr_h;   /* scr_* in OS units */
    /* the picture sprite */
    int *area;                          /* sprite area; NULL = none */
    int spr_w, spr_h, spr_rows;         /* pixels; rows allocated (padded) */
    int have_frame;
    box_t pic;                          /* picture area in work area coordinates */
    int vis_w, vis_h;                   /* the window's visible size */
    int last_time_cs;
    int log_cs, log_nulls, log_frames;  /* for the once-a-second log line */
    unsigned log_slept;
    int info, info_open;                /* the media info window */
    int proginfo;                       /* Info on the icon bar menu: About this program */
    char info_title[80];
    unsigned st_nulls, draw_n;          /* for its stats: null events, pictures drawn ... */
    unsigned draw_cs;                   /* ... and the time drawing them took */
    int nosleep;                        /* Reel$NoSleep: poll flat out, as before 0.1.9 */
    int idle_cs;                        /* after a null: centiseconds we may sleep */
    unsigned slept_cs;                  /* for the stats: sleep asked for */
#ifdef REEL_EGL
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLSurface surf;                    /* work area surface, or the whole screen */
    int surf_w, surf_h, surf_full;
#endif
    int fill_x1;                        /* current right edge of the position fill */
    /* playback options */
    double vol;                         /* the volume bar, 0..1 (the sound is vol squared) */
    int vol_fill_x1;
    int vsync;                          /* full screen: wait for the screen's refresh */
    int pic_mode;                       /* PIC_* */
    int speed_i;                        /* speeds[] */
    int fast;                           /* fast decoding */
    int deint_i;                        /* deint_modes[] */
    int ab;                             /* A-B repeat: 0 off, 1 A set, 2 repeating */
    double ab_a, ab_b;
    /* the playlist */
    char *list[LIST_MAX];
    int list_n, list_i;
    int last_drop_cs;                   /* when the last file arrived (a batch = one drag) */
} S;

/* ---- small helpers ---------------------------------------------------- */

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* ---- the log ------------------------------------------------------------ */

static FILE *logf;
static char log_path[256];
static int log_t0;
static char log_last[300];
static int log_repeats;

static int now_cs(void)
{
    _kernel_swi_regs r;
    swi(OS_ReadMonotonicTime, &r);
    return r.r[0];
}

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
    va_list ap;
    int t;
    if (!logf)
        return;
    t = now_cs() - log_t0;
    fprintf(logf, "%4d.%02d ", t / 100, t % 100);
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);                       /* all there even if we crash */
}

/* FFmpeg's and reelcore's messages (AV_LOG_ERROR 16, WARNING 24, INFO 32, VERBOSE 40) */
static void ff_log(int level, const char *line)
{
    if (!strcmp(line, log_last)) {      /* e.g. the same decoder warning every frame */
        log_repeats++;
        return;
    }
    if (log_repeats)
        lg("  (repeated %d more times)", log_repeats);
    log_repeats = 0;
    snprintf(log_last, sizeof(log_last), "%s", line);
    lg("%s%s", level <= 16 ? "ERROR " : level <= 24 ? "warning " : "", line);
}

static void log_module(const char *name)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = 18;                        /* OS_Module 18: look up by name */
    r.r[1] = (intptr_t)name;
    if (_kernel_swi(0x1E, &r, &r) || !r.r[3]) {
        lg("module %s: not loaded", name);
        return;
    }
    {
        const char *base = (const char *)(intptr_t)r.r[3];
        int off = *(const int *)(base + 0x14);
        char help[80];
        int i;
        for (i = 0; off && i < (int)sizeof(help) - 1; i++) {
            char c = base[off + i];
            if (c == 9)
                c = ' ';                /* help strings line up with tabs */
            else if (c < ' ')
                break;
            help[i] = c;
        }
        help[i] = 0;
        lg("module %s: %s", name, off ? help : "(no help string)");
    }
}

static void log_env(const char *name)
{
    const char *v = getenv(name);
    lg("%s = %s", name, v ? v : "(not set)");
}

static void log_open(void)
{
    const char *p = getenv(APP "$Log");
    if (p && !strcmp(p, "off"))
        return;
    if (p && *p)
        snprintf(log_path, sizeof(log_path), "%s", p);
    else {
        const char *scrap = getenv("Wimp$ScrapDir");
        if (!scrap || !*scrap)
            return;
        snprintf(log_path, sizeof(log_path), "%s.%sLog", scrap, APP);
    }
    if (!(logf = fopen(log_path, "w"))) {
        log_path[0] = 0;
        return;
    }
    {
        _kernel_swi_regs r;             /* OS_File 18: make it a Text file */
        r.r[0] = 18;
        r.r[1] = (intptr_t)log_path;
        r.r[2] = 0xFFF;
        _kernel_swi(0x08, &r, &r);
    }
    log_t0 = now_cs();
    lg("%s log (built " __DATE__ " " __TIME__ "), times in seconds", APP);
    log_module("SharedSound");
    log_module("StreamManager");
    log_module("SharedSoundBuffer");
    log_module("SharedUnixLibrary");
    log_module("PThreadTicker");
    log_module("VFPSupport");
    log_env("REELCORE_AUDIO");
    log_env("SDL_AUDIODRIVER");
#ifdef REEL_EGL
    log_env("ReelEGL$NoVsync");
#endif
    reelcore_set_log(ff_log, 1);
}

/* Opens the log in the editor (Filer_Run) */
static void log_show(void);

static void report(const char *text)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    lg("report: %s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 1 | 16;
    r.r[2] = (intptr_t)APP;
    swi(Wimp_ReportError, &r);
}

static void log_show(void)
{
    char cmd[300];
    _kernel_swi_regs r;
    if (!logf) {
        report("There's no log: " APP "$Log is \"off\", or <Wimp$ScrapDir> isn't set.");
        return;
    }
    fflush(logf);
    snprintf(cmd, sizeof(cmd), "Filer_Run %s", log_path);
    r.r[0] = (intptr_t)cmd;
    if (swi(0x400DE /* Wimp_StartTask */, &r))
        report(log_path);
}

static int mode_var(int var)
{
    _kernel_swi_regs r;
    r.r[0] = -1;
    r.r[1] = var;
    if (swi(OS_ReadModeVariable, &r))
        return 0;
    return r.r[2];
}

static void read_screen(void)
{
    static int logged_w, logged_h, logged_bpp;
    S.xeig = mode_var(4);
    S.yeig = mode_var(5);
    S.log2bpp = mode_var(9);
    S.trgb = S.log2bpp == 5 && (mode_var(0) & 0x4000);
    S.scr_w = (mode_var(11) + 1) << S.xeig;
    S.scr_h = (mode_var(12) + 1) << S.yeig;
    if (S.scr_w != logged_w || S.scr_h != logged_h || S.log2bpp != logged_bpp) {
        logged_w = S.scr_w; logged_h = S.scr_h; logged_bpp = S.log2bpp;
        lg("screen %dx%d pixels (%dx%d OS units), %d bpp, %s", S.scr_w >> S.xeig, S.scr_h >> S.yeig,
           S.scr_w, S.scr_h, 1 << S.log2bpp, S.trgb ? "TRGB" : "TBGR");
    }
}

static __attribute__((unused)) void vdu_clip(int x0, int y0, int x1, int y1)    /* inclusive, OS units */
{
    unsigned char b[9] = { 24, x0 & 255, (x0 >> 8) & 255, y0 & 255, (y0 >> 8) & 255,
                           x1 & 255, (x1 >> 8) & 255, y1 & 255, (y1 >> 8) & 255 };
    _kernel_swi_regs r;
    r.r[0] = (intptr_t)b;
    r.r[1] = 9;
    swi(OS_WriteN, &r);
}

static void format_time(char *buf, size_t n, double s)
{
    int t = s > 0 ? (int)s : 0;
    if (t >= 3600)
        snprintf(buf, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

/* ---- the picture sprite ------------------------------------------------ */

#ifndef REEL_EGL   /* the Reel build: a sprite plotted with OS_SpriteOp */
/* On the Pi small sprites weren't always replotted with their new contents
   (riscos-mesa found this); sprites of 1 MB or more always were, so pad
   with extra rows, which the clipping hides. */
#define MIN_SPRITE_BYTES (1024 * 1024)

static void sprite_free(void)
{
    free(S.area);
    S.area = NULL;
    S.spr_w = S.spr_h = 0;
    S.have_frame = 0;
}

static int *sprite_header(void) { return S.area + 4; }
static uint8_t *sprite_pixels(void) { return (uint8_t *)(S.area + 4 + 11); }

static int sprite_make(int w, int h)
{
    size_t rows = h, bytes;
    int *spr, mode;

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (S.area && S.spr_w == w && S.spr_h == h)
        return 0;
    sprite_free();
    if ((size_t)w * 4 * rows < MIN_SPRITE_BYTES)
        rows = (MIN_SPRITE_BYTES + (size_t)w * 4 - 1) / ((size_t)w * 4);
    bytes = 16 + 44 + (size_t)w * 4 * rows;
    if (!(S.area = calloc(1, bytes)))
        return -1;
    S.area[0] = (int)bytes;             /* area: size, sprites, first, free */
    S.area[1] = 1;
    S.area[2] = 16;
    S.area[3] = (int)bytes;
    spr = sprite_header();
    spr[0] = (int)bytes - 16;
    memcpy(&spr[1], "reel\0\0\0\0\0\0\0\0", 12);
    spr[4] = w - 1;
    spr[5] = (int)rows - 1;
    spr[6] = 0;
    spr[7] = 31;
    spr[8] = 44;
    spr[9] = 44;
    mode = 1 | ((180 >> S.xeig) << 1) | ((180 >> S.yeig) << 14) | (6 << 27);   /* 32bpp TBGR */
    if (S.trgb) {
        _kernel_swi_regs r;
        r.r[0] = 1;                                     /* current mode specifier */
        if (!swi(OS_ScreenMode, &r))
            mode = r.r[1];
    }
    spr[10] = mode;
    S.spr_w = w;
    S.spr_h = h;
    S.spr_rows = (int)rows;
    return 0;
}

/* Converts the current frame into the sprite (black bars around it). */
static void sprite_draw_frame(void)
{
    if (!S.v || !S.area)
        return;
    if (reelcore_draw_pixels(S.v, sprite_pixels(), S.spr_w * 4, S.spr_w, S.spr_h, S.trgb, pic_flags_of[S.pic_mode]) == 0)
        S.have_frame = 1;
}

/* Plots the sprite with its top left at screen (x, y1), clipped to clip. */
static void sprite_plot(int x, int y1, const box_t *clip)
{
    _kernel_swi_regs r;
    if (!S.area || !S.have_frame)
        return;
    vdu_clip(clip->x0, clip->y0, clip->x1 - 1, clip->y1 - 1);
    r.r[0] = 52 + 512;                  /* PutSpriteScaled, pointer to sprite */
    r.r[1] = (intptr_t)S.area;
    r.r[2] = (intptr_t)sprite_header();
    r.r[3] = x;
    r.r[4] = y1 - (S.spr_rows << S.yeig);
    r.r[5] = 0;                         /* overwrite */
    r.r[6] = 0;                         /* no scaling (the dpi matches the screen) */
    r.r[7] = 0;                         /* no translation: 16/32bpp screens */
    swi(OS_SpriteOp, &r);
}

#endif

#ifdef REEL_EGL
/* ---- the picture through riscos-mesa's EGL (the ReelEGL build) ----------

   In the window the picture is a work area EGL surface
   (EGL_WORK_AREA_*_RISCOS: fixed size, top left at work area 0,0), so the
   controls stay ordinary icons. ffegl_draw_surface converts each frame into
   it through EGL_KHR_lock_surface and eglSwapBuffers shows it; redraws
   plot it with eglPlotSurfaceRISCOS in our own Wimp_RedrawWindow loop.
   Full screen uses the whole screen surface (EGL_RISCOS_SCREEN_WINDOW):
   with Vsync (the default) a sprite plotted after the vsync wait
   (eglSwapInterval 1); without, EGL_SINGLE_BUFFER: frames go straight
   into screen memory (no plot; can tear). Only one surface exists at a
   time: switching to or from the mini player makes it again. */

static int egl_init(void)
{
    static const EGLint attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_LOCK_SURFACE_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if (S.dpy)
        return 0;
    S.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (S.dpy != EGL_NO_DISPLAY) {
        EGLint major = 0, minor = 0;
        if (eglInitialize(S.dpy, &major, &minor))
            lg("EGL %d.%d: %s, %s", (int)major, (int)minor, eglQueryString(S.dpy, EGL_VENDOR),
               eglQueryString(S.dpy, EGL_VERSION));
    }
    if (S.dpy == EGL_NO_DISPLAY || !eglInitialize(S.dpy, NULL, NULL) ||
        !eglChooseConfig(S.dpy, attr, &S.cfg, 1, &n) || n < 1) {
        lg("EGL: display %p, error 0x%x, %d configs", (void *)S.dpy, eglGetError(), (int)n);
        S.dpy = EGL_NO_DISPLAY;
        report("EGL didn't start (riscos-mesa's EGL with EGL_KHR_lock_surface is needed).");
        return -1;
    }
    return 0;
}

static void surf_free(void)
{
    if (S.surf != EGL_NO_SURFACE)
        eglDestroySurface(S.dpy, S.surf);
    S.surf = EGL_NO_SURFACE;
    S.surf_w = S.surf_h = 0;
    S.have_frame = 0;
}

static void surf_make(int w, int h, int full)
{
    EGLint attr[12], *a = attr;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (S.surf != EGL_NO_SURFACE && S.surf_w == w && S.surf_h == h && S.surf_full == full)
        return;
    surf_free();
    if (egl_init() < 0)
        return;
    if (full) {
        if (!S.vsync) {                 /* Direct: straight into screen memory */
            *a++ = EGL_RENDER_BUFFER; *a++ = EGL_SINGLE_BUFFER;
        }
    } else {
        *a++ = EGL_WORK_AREA_X_RISCOS; *a++ = 0;
        *a++ = EGL_WORK_AREA_Y_RISCOS; *a++ = 0;
        *a++ = EGL_WORK_AREA_WIDTH_RISCOS; *a++ = w;
        *a++ = EGL_WORK_AREA_HEIGHT_RISCOS; *a++ = h;
    }
    *a = EGL_NONE;
    S.surf = eglCreateWindowSurface(S.dpy, S.cfg,
                                    full ? EGL_RISCOS_SCREEN_WINDOW : (EGLNativeWindowType)(intptr_t)S.win, attr);
    if (S.surf == EGL_NO_SURFACE) {
        char msg[80];
        snprintf(msg, sizeof(msg), "eglCreateWindowSurface failed (0x%x).", eglGetError());
        report(msg);
        return;
    }
    if (full)
        eglSwapInterval(S.dpy, S.vsync ? 1 : 0);
    lg("EGL surface %dx%d, %s", w, h, !full ? "work area" : !S.vsync ? "screen, direct (single buffer)" : "screen, vsync");
    S.surf_w = w;
    S.surf_h = h;
    S.surf_full = full;
}
#endif

/* ---- the picture: whichever way it's drawn ---------------------------- */

static void pic_free(void)
{
#ifdef REEL_EGL
    surf_free();
#else
    sprite_free();
#endif
}

/* The picture's size in pixels; full = the whole screen. */
static void pic_make(int w, int h, int full)
{
#ifdef REEL_EGL
    surf_make(w, h, full);
#else
    (void)full;
    sprite_make(w, h);
#endif
}

/* Converts the current frame again (after a resize or a mode change); the
   EGL build also shows it (eglSwapBuffers). */
static void pic_refresh(void)
{
#ifdef REEL_EGL
    if (!S.v || S.surf == EGL_NO_SURFACE)
        return;
    if (ffegl_draw_surface(S.v, S.dpy, S.surf, 0, 0, 0, 0, pic_flags_of[S.pic_mode]) == 0) {
        eglSwapBuffers(S.dpy, S.surf);
        S.have_frame = 1;
    }
#else
    sprite_draw_frame();
#endif
}

/* ---- windows ------------------------------------------------------------ */

/* Indirected text icon data */
typedef struct { char *text; const char *valid; int len; } ind_t;

typedef struct {
    int x0, y0, x1, y1, flags;
    union { char name[12]; ind_t ind; } d;
} icon_t;

static void icon_def(icon_t *ic, int flags, char *text, const char *valid, int len)
{
    memset(ic, 0, sizeof(*ic));
    ic->flags = flags;
    ic->d.ind.text = text;
    ic->d.ind.valid = valid;
    ic->d.ind.len = len;
}

/* icon flags */
#define IF_TEXT   0x1
#define IF_BORDER 0x4
#define IF_HCENT  0x8
#define IF_VCENT  0x10
#define IF_FILLED 0x20
#define IF_INDIR  0x100
#define IF_CLICK  (3 << 12)
#define IF_SPRITE 0x2
#define IF_DRAG   (6 << 12)             /* click/drag: a drag reports buttons x16 */
#define IF_COL(fg, bg) (((fg) << 24) | ((bg) << 28))

static char back_text[] = "\x8b 10s", fwd_text[] = "10s \x8a", full_text[] = "Full", normal_text[] = "Normal";
static char empty_text[] = "";

/* The controls: the same icons (and texts) in the normal window and the
   mini player, so the icon numbers mean the same in both; layout() moves
   the ones the mini player doesn't show out of sight. */
static void icons_def(icon_t *icon)
{
    icon_def(&icon[I_PLAY], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             S.play_text, "R5,3", sizeof(S.play_text));
    icon_def(&icon[I_BACK], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             back_text, "R5,3", sizeof(back_text));
    icon_def(&icon[I_FWD], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             fwd_text, "R5,3", sizeof(fwd_text));
    icon_def(&icon[I_TRACK], IF_BORDER | IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 0),
             empty_text, "R2", 1);
    icon_def(&icon[I_FILL], IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 8),
             empty_text, (const char *)-1, 1);
    icon_def(&icon[I_TIME], IF_TEXT | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 1),
             S.time_text, (const char *)-1, sizeof(S.time_text));
    icon_def(&icon[I_VOL], IF_BORDER | IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 0),
             empty_text, "R2", 1);
    icon_def(&icon[I_VOLFILL], IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 10),
             empty_text, (const char *)-1, 1);
    icon_def(&icon[I_FULL], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             full_text, "R5,3", sizeof(full_text));
    /* the resize grip: the sprite reelgrip (in !Sprites), click or drag to resize */
    memset(&icon[I_GRIP], 0, sizeof(icon[I_GRIP]));
    icon[I_GRIP].flags = IF_SPRITE | IF_HCENT | IF_VCENT | IF_DRAG | IF_COL(7, 1);
    memcpy(icon[I_GRIP].d.name, "reelgrip", 8);
}

static int create_window(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        ind_t title;
        int nicons;
        icon_t icon[N_ICONS];
    } w;
    _kernel_swi_regs r;

    memset(&w, 0, sizeof(w));
    w.vis.x1 = 800; w.vis.y1 = 600;
    w.behind = -1;
    w.flags = (int)0xAF000002u;         /* new format, back, close, title, toggle, adjust size, moveable */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -8192; w.ext.x1 = 8192; w.ext.y1 = 0;
    w.tflags = IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 2);
    w.wbutton = 3 << 12;                /* clicks on the picture: for the caret */
    w.sprites = 1;
    w.minw = MIN_W; w.minh = CH + 96;
    w.title.text = S.title;
    w.title.valid = (const char *)-1;
    w.title.len = sizeof(S.title);
    w.nicons = N_ICONS;
    icons_def(w.icon);
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    S.win = S.main_win = r.r[0];
    return 0;
}

/* The mini player: no title bar or other furniture (drag the picture to
   move it; double-click it for the normal window), the controls cut down
   to Play/Pause, the position bar and Normal. */
static int create_mini_window(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
        icon_t icon[N_ICONS + 1];
    } w;
    _kernel_swi_regs r;

    memset(&w, 0, sizeof(w));
    w.vis.x1 = MINI_W; w.vis.y1 = 400;
    w.behind = -1;
    w.flags = (int)0x80000002u;         /* new format, moveable; no furniture */
    w.tfg = 0xFF;                       /* no title */
    w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -8192; w.ext.x1 = 8192; w.ext.y1 = 0;
    w.wbutton = 10 << 12;               /* click/drag/double: drag moves it, double-click: normal window */
    w.sprites = 1;
    w.minw = MINI_MIN_W; w.minh = CH + 80;
    w.nicons = N_ICONS + 1;
    icons_def(w.icon);
    icon_def(&w.icon[I_NORMAL], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             normal_text, "R5,3", sizeof(normal_text));
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    S.mini_win = r.r[0];
    return 0;
}

static int create_full_window(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
    } w;
    _kernel_swi_regs r;

    memset(&w, 0, sizeof(w));
    w.vis.x1 = S.scr_w; w.vis.y1 = S.scr_h;
    w.behind = -1;
    w.flags = (int)0x80000040u;         /* new format, no furniture, may cover the icon bar */
    w.tfg = 0xFF;                       /* no title */
    w.wfg = 7; w.wbg = 7;               /* black */
    w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -S.scr_h; w.ext.x1 = S.scr_w; w.ext.y1 = 0;
    w.wbutton = 3 << 12;
    w.sprites = 1;
    w.minw = 0; w.minh = 0;
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    S.full = r.r[0];
    return 0;
}

static void resize_icon(int i, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = S.win;
    r.r[1] = i;
    r.r[2] = x0; r.r[3] = y0; r.r[4] = x1; r.r[5] = y1;
    swi(Wimp_ResizeIcon, &r);
}

/* Lays out the controls for a visible size and makes the sprite fit. */
static void layout(int vw, int vh)
{
    int y0 = -vh + GAP, y1 = -vh + CH - GAP;
    int x = GAP, track_x0, track_x1;

    S.vis_w = vw;
    S.vis_h = vh;
    S.pic.x0 = 0;
    S.pic.x1 = vw;
    S.pic.y1 = 0;
    S.pic.y0 = -vh + CH;
    if (mini) {                         /* Play, the position bar, Normal; the rest out of sight */
        static const int hidden[] = { I_BACK, I_FWD, I_TIME, I_VOL, I_VOLFILL, I_FULL };
        for (int i = 0; i < (int)(sizeof(hidden) / sizeof(hidden[0])); i++)
            resize_icon(hidden[i], -256, y0, -128, y1);
        resize_icon(I_PLAY, x, y0, x + W_PLAY, y1);
        track_x(vw, &track_x0, &track_x1);
        resize_icon(I_TRACK, track_x0, y0 + 12, track_x1, y1 - 12);
        S.fill_x1 = track_x0 + 4;
        resize_icon(I_FILL, track_x0 + 4, y0 + 16, S.fill_x1, y1 - 16);
        resize_icon(I_NORMAL, RIGHT(vw) - W_NORM, y0, RIGHT(vw), y1);
        resize_icon(I_GRIP, vw - W_GRIP, -vh, vw, -vh + W_GRIP);   /* the very corner */
        if (!S.fullscreen)
            pic_make(vw >> S.xeig, (vh - CH) >> S.yeig, 0);
        return;
    }
    resize_icon(I_PLAY, x, y0, x + W_PLAY, y1);  x += W_PLAY + GAP;
    resize_icon(I_BACK, x, y0, x + W_SKIP, y1);  x += W_SKIP + GAP;
    resize_icon(I_FWD,  x, y0, x + W_SKIP, y1);
    track_x(vw, &track_x0, &track_x1);
    resize_icon(I_TRACK, track_x0, y0 + 12, track_x1, y1 - 12);
    S.fill_x1 = track_x0 + 4;
    resize_icon(I_FILL, track_x0 + 4, y0 + 16, S.fill_x1, y1 - 16);
    {
        int vx0, vx1;
        vol_x(vw, &vx0, &vx1);
        resize_icon(I_TIME, vx0 - GAP - W_TIME, y0, vx0 - GAP, y1);
        resize_icon(I_VOL, vx0, y0 + 12, vx1, y1 - 12);
        S.vol_fill_x1 = vx0 + 4 + (int)((W_VOL - 8) * S.vol);
        resize_icon(I_VOLFILL, vx0 + 4, y0 + 16, S.vol_fill_x1, y1 - 16);
    }
    resize_icon(I_FULL, RIGHT(vw) - W_FULL, y0, RIGHT(vw), y1);
    resize_icon(I_GRIP, vw - W_GRIP, -vh, vw, -vh + W_GRIP);   /* the very corner */
    if (!S.fullscreen)
        pic_make(vw >> S.xeig, (vh - CH) >> S.yeig, 0);
}

static void window_state(int w, int *block)
{
    _kernel_swi_regs r;
    block[0] = w;
    r.r[1] = (intptr_t)block;
    swi(Wimp_GetWindowState, &r);
}

static void force_redraw(int w, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = x0; r.r[2] = y0; r.r[3] = x1; r.r[4] = y1;
    swi(Wimp_ForceRedraw, &r);
}

static void set_caret(int w)
{
    _kernel_swi_regs r;
    r.r[0] = w; r.r[1] = -1; r.r[2] = 0; r.r[3] = 0;
    r.r[4] = 1 << 25;                   /* invisible */
    r.r[5] = -1;
    swi(Wimp_SetCaretPosition, &r);
}

static void open_window_at(int x0, int y1, int vw, int vh)
{
    int b[9];
    _kernel_swi_regs r;
    b[0] = S.win;
    b[1] = x0; b[2] = y1 - vh; b[3] = x0 + vw; b[4] = y1;
    b[5] = 0; b[6] = 0; b[7] = -1;
    layout(vw, vh);
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
}

/* ---- controls ------------------------------------------------------------ */

static void icon_refresh(int i)
{
    _kernel_swi_regs r;
    int b[4] = { S.win, i, 0, 0 };
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetIconState, &r);         /* no change: just redraws it */
}

static void update_controls(int force)
{
    char pos[16], dur[16], text[40];
    double p, d;
    int x0 = 0, x1, fill;

    if (!S.v || !S.win)
        return;
    snprintf(S.play_text, sizeof(S.play_text), "%s", (reelcore_paused(S.v) || S.ended) ? "Play" : "Pause");
    p = reelcore_position(S.v);
    d = reelcore_duration(S.v);
    format_time(pos, sizeof(pos), p);
    format_time(dur, sizeof(dur), d);
    if (d > 0)
        snprintf(text, sizeof(text), "%s / %s", pos, dur);
    else
        snprintf(text, sizeof(text), "%s", pos);
    if (S.speed_i != 2 || S.ab) {           /* short marks: the icon is narrow */
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, "%s%s%s", S.speed_i != 2 ? " " : "",
                 S.speed_i != 2 ? speed_names[S.speed_i] : "", S.ab == 2 ? " AB" : S.ab == 1 ? " A" : "");
    }
    if (force || strcmp(text, S.time_text)) {
        snprintf(S.time_text, sizeof(S.time_text), "%s", text);
        icon_refresh(I_TIME);
    }
    icon_refresh(I_PLAY);
    /* the position bar */
    {
        int y0 = -S.vis_h + GAP, y1 = -S.vis_h + CH - GAP;
        int track_x0, track_x1;
        track_x(S.vis_w, &track_x0, &track_x1);
        x0 = track_x0 + 4;
        x1 = track_x1 - 4;
        fill = d > 0 ? x0 + (int)((x1 - x0) * (p / d > 1 ? 1 : p / d)) : x0;
        if (force || fill != S.fill_x1) {
            int old = S.fill_x1;
            S.fill_x1 = fill;
            resize_icon(I_FILL, x0, y0 + 16, fill, y1 - 16);
            force_redraw(S.win, (old < fill ? old : fill) - 4, y0, (old > fill ? old : fill) + 4, y1);
        }
    }
}

/* ---- the picture: redraw and update ---------------------------------------- */

static void draw_rects(int w, int *b, int more, int update)
{
    _kernel_swi_regs r;
    while (more) {
        /* b: window, visible box, scroll, clip box */
        int ox = b[1] - b[5], oy = b[4] - b[6];      /* screen position of work area (0,0) */
        box_t pic = S.fullscreen && w == S.full ? (box_t){ 0, -S.vis_h, S.vis_w, 0 } : S.pic;
        box_t c;
        c.x0 = ox + pic.x0; c.x1 = ox + pic.x1; c.y0 = oy + pic.y0; c.y1 = oy + pic.y1;
        if (c.x0 < b[7]) c.x0 = b[7];
        if (c.y0 < b[8]) c.y0 = b[8];
        if (c.x1 > b[9]) c.x1 = b[9];
        if (c.y1 > b[10]) c.y1 = b[10];
        if (c.x0 < c.x1 && c.y0 < c.y1) {
#ifdef REEL_EGL
            /* the window's work area surface plots its own part of this
               rectangle; the full screen surface is redrawn after the loop */
            if (!(S.fullscreen && w == S.full) && S.surf != EGL_NO_SURFACE && S.have_frame)
                eglPlotSurfaceRISCOS(S.dpy, S.surf, b);
#else
            sprite_plot(ox + pic.x0, oy + pic.y1, &c);
#endif
        }
        r.r[1] = (intptr_t)b;
        more = swi(Wimp_GetRectangle, &r) ? 0 : r.r[0];
    }
    (void)update;
}

static void redraw(int *block)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)block;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    draw_rects(block[0], block, r.r[0], 0);
#ifdef REEL_EGL
    if (S.fullscreen && block[0] == S.full)
        pic_refresh();                  /* the Wimp painted it black: draw the frame again */
#endif
}

static void show_frame_now(void);

static void show_frame(void)
{
    int t0 = now_cs();
    show_frame_now();
    S.draw_cs += now_cs() - t0;
    S.draw_n++;
}

static void show_frame_now(void)
{
#ifdef REEL_EGL
    pic_refresh();                      /* ffegl_draw_surface + eglSwapBuffers */
#else
    int b[11];
    _kernel_swi_regs r;
    int w = S.fullscreen ? S.full : S.win;
    box_t pic = S.fullscreen ? (box_t){ 0, -S.vis_h, S.vis_w, 0 } : S.pic;
    sprite_draw_frame();
    if (S.fullscreen && S.vsync) {      /* wait for the screen's refresh: no tearing */
        r.r[0] = 19;                    /* OS_Byte 19 */
        swi(0x06, &r);
    }
    b[0] = w;
    b[1] = pic.x0; b[2] = pic.y0; b[3] = pic.x1; b[4] = pic.y1;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_UpdateWindow, &r))
        return;
    draw_rects(w, b, r.r[0], 1);
#endif
}

/* ---- full screen ------------------------------------------------------------ */

static void set_fullscreen(int on)
{
    int b[9];
    _kernel_swi_regs r;
    if (on == S.fullscreen || !S.v)
        return;
    lg("full screen %s", on ? "on" : "off");
    if (on) {
        read_screen();
        if (!S.full && create_full_window() < 0)
            return;
        S.fullscreen = 1;
        S.vis_w = S.scr_w;
        S.vis_h = S.scr_h;
        b[0] = S.full; b[1] = 0; b[2] = 0; b[3] = S.scr_w; b[4] = S.scr_h;
        b[5] = 0; b[6] = 0; b[7] = -1;
        r.r[1] = (intptr_t)b;
        swi(Wimp_OpenWindow, &r);
        pic_make(S.scr_w >> S.xeig, S.scr_h >> S.yeig, 1);
        pic_refresh();
        set_caret(S.full);
    } else {
        int st[9];
        r.r[1] = (intptr_t)&S.full;
        swi(Wimp_CloseWindow, &r);
        S.fullscreen = 0;
        window_state(S.win, st);
        layout(st[3] - st[1], st[4] - st[2]);
        pic_refresh();
        force_redraw(S.win, 0, -8192, 8192, 0);
        set_caret(S.win);
    }
}

/* ---- opening and closing a file ------------------------------------------------ */

static void info_new_file(void);
static void info_close(void);

static void resume_note(void);
static void list_clear(void);
static void choices_save(void);
static void mini_show(void);

static void close_video(void)
{
    _kernel_swi_regs r;
    if (S.v) {
        char d[300];
        reelcore_debug(S.v, d, sizeof(d));
        lg("close: %s", d);
        resume_note();
    }
    list_clear();
    if (S.fullscreen)
        set_fullscreen(0);
    if (S.win) {
        r.r[1] = (intptr_t)&S.win;
        swi(Wimp_CloseWindow, &r);
    }
    if (mini) {                         /* the next video opens in the normal window */
        mini = 0;
        S.win = S.main_win;
        apply_fast();
        choices_save();
    }
    info_close();
    reelcore_close(S.v);
    S.v = NULL;
    pic_free();
}

static const char *leaf(const char *path)
{
    /* RISC OS names: after the last '.'; Unix names (UnixLib): after the last '/' */
    const char *p = strrchr(path, path[0] == '/' ? '/' : '.');
    return p ? p + 1 : path;
}

/* ---- Choices: the volume, and where each file was stopped ------------------

   Read from Choices:Reel (ReelEGL), written to <Choices$Write>.Reel.
   Reel$ChoicesDir (a directory) overrides both, for the host test. */

#define RESUME_MAX 100

static struct { double pos; char path[256]; } resume[RESUME_MAX];
static int resume_n;

static void choices_path(char *buf, size_t n, const char *leafname, int write)
{
    const char *dir = getenv(APP "$ChoicesDir");
    if (dir && *dir)
        snprintf(buf, n, "%s/%s", dir, leafname);
    else if (write)
        snprintf(buf, n, "<Choices$Write>." APP ".%s", leafname);
    else
        snprintf(buf, n, "Choices:" APP ".%s", leafname);
}

static FILE *choices_open(const char *leafname, int write)
{
    char path[300];
    if (write && !getenv(APP "$ChoicesDir")) {
        _kernel_swi_regs r;             /* OS_File 8: make the directory */
        r.r[0] = 8;
        r.r[1] = (intptr_t)"<Choices$Write>." APP;
        r.r[4] = 0;
        _kernel_swi(0x08, &r, &r);
    }
    choices_path(path, sizeof(path), leafname, write);
    return fopen(path, write ? "w" : "r");
}

static void choices_save(void)
{
    FILE *f = choices_open("Choices", 1);
    if (!f)
        return;
    fprintf(f, "# %s choices\nvolume %.3f\nkeep_on_top %d\nmini_width %d\nmini_right %d\nmini_bottom %d\ndeinterlace %s\n",
            APP, S.vol, S.ontop, S.mini_w, S.mini_right, S.mini_bottom, deint_names[S.deint_i]);
    fclose(f);
}

static void resume_save(void)
{
    FILE *f = choices_open("Resume", 1);
    if (!f)
        return;
    for (int i = 0; i < resume_n; i++)
        fprintf(f, "%.2f\t%s\n", resume[i].pos, resume[i].path);
    fclose(f);
}

static void choices_load(void)
{
    char line[300];
    FILE *f = choices_open("Choices", 0);
    if (f) {
        double vol;
        int n;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "volume %lf", &vol) == 1 && vol >= 0 && vol <= 1)
                S.vol = vol;
            else if (sscanf(line, "keep_on_top %d", &n) == 1)
                S.ontop = n != 0;
            else if (sscanf(line, "mini_width %d", &n) == 1 && n >= MINI_MIN_W && n <= 4096)
                S.mini_w = n;
            else if (sscanf(line, "mini_right %d", &n) == 1 && n >= 0)
                S.mini_right = n;
            else if (sscanf(line, "mini_bottom %d", &n) == 1 && n >= -1)
                S.mini_bottom = n;
            else if (!strncmp(line, "deinterlace ", 12))
                for (int i = 0; i < 3; i++)
                    if (!strncmp(line + 12, deint_names[i], strlen(deint_names[i])))
                        S.deint_i = i;
        }
        fclose(f);
    }
    if ((f = choices_open("Resume", 0)) != NULL) {
        while (resume_n < RESUME_MAX && fgets(line, sizeof(line), f)) {
            char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
            if (!tab)
                continue;
            if (nl)
                *nl = 0;
            resume[resume_n].pos = atof(line);
            snprintf(resume[resume_n].path, sizeof(resume[0].path), "%s", tab + 1);
            resume_n++;
        }
        fclose(f);
    }
}

static int resume_find(const char *path)
{
    for (int i = 0; i < resume_n; i++)
        if (!strcmp(resume[i].path, path))
            return i;
    return -1;
}

/* Remembers where the current file was stopped (the newest first), or
   forgets it if it was played to the end or hardly started. */
static void resume_note(void)
{
    double pos, dur, margin;
    int i;
    if (!S.v)
        return;
    pos = reelcore_position(S.v);
    dur = reelcore_duration(S.v);
    margin = dur > 100 ? 10 : dur / 10;
    if ((i = resume_find(S.file)) >= 0) {
        memmove(&resume[i], &resume[i + 1], (resume_n - i - 1) * sizeof(resume[0]));
        resume_n--;
    }
    if (dur > 0 && !S.ended && pos >= margin && pos <= dur - margin) {
        if (resume_n == RESUME_MAX)
            resume_n--;
        memmove(&resume[1], &resume[0], resume_n * sizeof(resume[0]));
        resume[0].pos = pos;
        snprintf(resume[0].path, sizeof(resume[0].path), "%s", S.file);
        resume_n++;
        lg("remembered %.1f s for %s", pos, S.file);
    }
    resume_save();
}

/* "Carry on from 1:23?": Wimp_ReportError with two buttons of our own */
static int resume_ask(double pos)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    char t[16];
    format_time(t, sizeof(t), pos);
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s was stopped at %s. Carry on from there?", S.title, t);
    r.r[0] = (intptr_t)&e;
    r.r[1] = (1 << 8) | (4 << 9);       /* new style, a question, only our buttons */
    r.r[2] = (intptr_t)APP;
    r.r[3] = (intptr_t)ICON;
    r.r[4] = 1;                         /* the sprite is in the Wimp's pool */
    r.r[5] = (intptr_t)"Carry on,From the start";
    if (swi(Wimp_ReportError, &r))
        return 0;
    return r.r[1] == 3;
}

/* The options set in the menus, for a newly opened file */
static void options_apply(void)
{
    reelcore_set_volume(S.v, S.vol * S.vol);
    apply_fast();
    reelcore_set_deinterlace(S.v, deint_modes[S.deint_i]);
    if (S.speed_i != 2)
        reelcore_set_speed(S.v, speeds[S.speed_i]);
}

static void set_title(void)
{
    if (S.list_n > 1)
        snprintf(S.title, sizeof(S.title), "%s (%d/%d)", leaf(S.file), S.list_i + 1, S.list_n);
    else
        snprintf(S.title, sizeof(S.title), "%s", leaf(S.file));
}

/* ---- the playlist ------------------------------------------------------------ */

static void play_file(const char *file);

static void list_clear(void)
{
    for (int i = 0; i < S.list_n; i++)
        free(S.list[i]);
    S.list_n = S.list_i = 0;
}

static void list_play(int i)
{
    if (i < 0 || i >= S.list_n)
        return;
    S.list_i = i;
    lg("playlist: %d of %d", i + 1, S.list_n);
    play_file(S.list[i]);
}

/* A file dropped or double-clicked. Files that arrive together (one drag
   of several) make a playlist; with Shift held, they're added to it;
   otherwise a new file replaces it. */
static void list_arrived(const char *file)
{
    _kernel_swi_regs r;
    int t = now_cs(), shift;
    r.r[0] = 129;                       /* INKEY(-1): Shift */
    r.r[1] = 0xFF;
    r.r[2] = 0xFF;
    shift = !swi(0x06, &r) && r.r[1] == 0xFF;
    if (S.v && S.list_n > 0 && S.list_n < LIST_MAX && (t - S.last_drop_cs < 50 || shift)) {
        S.list[S.list_n++] = strdup(file);
        S.last_drop_cs = t;
        lg("playlist: added %s (%d files)", file, S.list_n);
        set_title();
        if (S.win) {
            r.r[0] = S.win;             /* the title shows the count */
            r.r[1] = 0x4B534154;
            r.r[2] = 3;
            swi(Wimp_ForceRedraw, &r);
        }
        return;
    }
    list_clear();
    S.list[S.list_n++] = strdup(file);
    S.last_drop_cs = t;
    list_play(0);
}

static void play_file(const char *file)
{
    ReelCore *v;
    int vw, vh, w, h, maxw, maxh, st[9];
    int was_open = S.v != NULL;

    lg("open %s", file);
    resume_note();                      /* where the one playing now was */
    v = reelcore_open(file, S.loop && S.list_n <= 1 ? REELCORE_LOOP : 0);
    if (!v) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s: %s", leaf(file), reelcore_last_error());
        report(msg);
        return;
    }
    if (S.v) {                          /* replace what was playing, keep the window */
        reelcore_close(S.v);
        S.v = NULL;
    }
    S.v = v;
    S.ended = 0;
    S.ab = 0;
    options_apply();
    {
        char info[256];
        reelcore_info(v, info, sizeof(info));
        lg("playing: %s; %.1f s", info, reelcore_duration(v));
        S.log_cs = now_cs();
        S.log_nulls = S.log_frames = 0;
    }
    info_new_file();
    snprintf(S.file, sizeof(S.file), "%s", file);
    set_title();
    {
        int i = resume_find(file);
        if (i >= 0 && resume[i].pos < reelcore_duration(v) && resume_ask(resume[i].pos)) {
            lg("carry on from %.1f s", resume[i].pos);
            reelcore_seek(v, resume[i].pos);
        }
    }
    read_screen();
    if (!S.win && create_window() < 0) {
        report("Can't create the window.");
        return;
    }
    /* size: the video's own (pixels -> OS units), at most 3/4 of the screen */
    w = reelcore_width(v) << S.xeig;
    h = reelcore_height(v) << S.yeig;
    maxw = S.scr_w * 3 / 4;
    maxh = S.scr_h * 3 / 4 - CH;
    if (w > maxw) { h = (int)((long long)h * maxw / w); w = maxw; }
    if (h > maxh) { w = (int)((long long)w * maxh / h); h = maxh; }
    if (w < MIN_W) w = MIN_W;          /* room for the controls; the picture is letterboxed */
    vw = w;
    vh = h + CH;
    lg("window %dx%d OS units%s", vw, vh, S.fullscreen ? " (staying full screen)" : "");
    if (S.fullscreen) {                 /* stay full screen; the window follows later */
        if (mini) {                     /* (that's the mini player: the normal window's size for later) */
            S.main_st[3] = S.main_st[1] + vw;
            S.main_st[2] = S.main_st[4] - vh;
        }
        pic_make(S.scr_w >> S.xeig, S.scr_h >> S.yeig, 1);
        force_redraw(S.full, 0, -8192, 8192, 0);
        return;
    }
    if (mini) {                         /* stay in the mini player, sized for this video */
        S.main_st[3] = S.main_st[1] + vw;   /* the normal window's size, for later */
        S.main_st[2] = S.main_st[4] - vh;
        mini_show();
        return;
    }
    if (was_open) {
        _kernel_swi_regs r;
        window_state(S.win, st);        /* keep its position */
        open_window_at(st[1], st[4], vw, vh);
        r.r[0] = S.win;                 /* the title changed */
        r.r[1] = 0x4B534154;
        r.r[2] = 3;
        swi(Wimp_ForceRedraw, &r);
    } else
        open_window_at((S.scr_w - vw) / 2, (S.scr_h + vh) / 2, vw, vh);
    force_redraw(S.win, 0, -8192, 8192, 0);
    update_controls(1);
    set_caret(S.win);
}

/* ---- the media info window ------------------------------------------------ */

#define INFO_ROW      40               /* OS units a line */
#define INFO_TOP      16
#define INFO_LABEL_X  32
#define INFO_VALUE_X  400
#define INFO_W        1800
#define INFO_MAX      64

typedef struct { char heading; char label[64]; char value[168]; } info_row_t;
static info_row_t info_rows[INFO_MAX];
static int info_n, info_stats_at;       /* rows, and the first of the stats */
static ReelCoreStats info_prev;
static int info_prev_cs;
static unsigned info_prev_nulls, info_prev_draw_n, info_prev_draw_cs, info_prev_slept;

static info_row_t *info_add(int heading, const char *label, const char *value)
{
    info_row_t *r;
    if (info_n >= INFO_MAX)
        return &info_rows[INFO_MAX - 1];
    r = &info_rows[info_n++];
    r->heading = (char)heading;
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
    return r;
}

static const char *const stat_labels[] = {
    "Position", "Clock", "Pictures shown", "Decoded", "Decoding load", "Frame skipping",
    "Converting", "Deinterlacing", "Drawing", "Waiting", "Sound", "Reading", "Desktop", "Playback"
};
#define N_STATS ((int)(sizeof(stat_labels) / sizeof(stat_labels[0])))

/* The rows: the stats (live, at the top), then the file's details from
   reelcore_media_info. */
static void info_build(void)
{
    static char text[4096];
    char *line, *next;
    info_n = 0;
    info_add(1, "Stats for nerds (each second, while playing)", NULL);
    info_stats_at = info_n;
    for (int i = 0; i < N_STATS; i++)
        info_add(0, stat_labels[i], "...");
    reelcore_media_info(S.v, text, sizeof(text));
    for (line = text; *line; line = next) {
        char *tab, *nl = strchr(line, '\n');
        next = nl ? nl + 1 : line + strlen(line);
        if (nl)
            *nl = 0;
        if (line[0] == '#')
            info_add(1, line + 1, NULL);
        else if ((tab = strchr(line, '\t')) != NULL) {
            *tab = 0;
            info_add(0, line, tab + 1);
        }
    }
}

/* The stats rows from what changed since the last time (about a second). */
static void info_stats(void)
{
    ReelCoreStats st;
    info_row_t *r = &info_rows[info_stats_at];
    char pos[16], dur[16];
    int t = now_cs();
    double dt = (t - info_prev_cs) / 100.0;
    unsigned dec, shown, late, draws;
    double dtime;

    if (!S.v || info_stats_at + N_STATS > info_n)
        return;
    reelcore_stats(S.v, &st);
    if (dt <= 0)
        dt = 0.01;
    dec = st.decoded - info_prev.decoded;
    shown = st.shown - info_prev.shown;
    late = st.late - info_prev.late;
    dtime = st.decode_time - info_prev.decode_time;
    draws = S.draw_n - info_prev_draw_n;

    format_time(pos, sizeof(pos), st.position);
    format_time(dur, sizeof(dur), reelcore_duration(S.v));
    snprintf(r[0].value, sizeof(r[0].value), "%s of %s (%.2f s)%s", pos, dur, st.position,
             st.clock_source == 2 ? ", paused" : S.ended ? ", ended" : "");
    snprintf(r[1].value, sizeof(r[1].value), "%.2f s, from the %s; picture %+d ms", st.clock,
             st.clock_source == 1 ? "sound" : st.clock_source == 2 ? "pause position" : "timer",
             (int)((st.position - st.clock) * 1000));
    snprintf(r[2].value, sizeof(r[2].value), "%.1f a second (the video: %.3g); %u late skipped (%u in all)",
             shown / dt, st.fps, late, st.late);
    if (dec && dtime > 0)
        snprintf(r[3].value, sizeof(r[3].value), "%.1f a second, %.1f ms each: %.2fx real time",
                 dec / dt, dtime * 1000 / dec, st.fps > 0 ? (dec / st.fps) / dtime : 0);
    else
        snprintf(r[3].value, sizeof(r[3].value), "%.1f a second", dec / dt);
    snprintf(r[4].value, sizeof(r[4].value), "%.0f%% of the time for pictures, %.0f%% for sound",
             dtime * 100 / dt, (st.audio_time - info_prev.audio_time) * 100 / dt);
    snprintf(r[5].value, sizeof(r[5].value), "%s; %u time%s so far",
             st.skip_level == 2 ? "keyframes only" : st.skip_level ? "non-reference frames skipped" : "off",
             st.skip_spells, st.skip_spells == 1 ? "" : "s");
    if (dec || shown) {
        unsigned conv = shown ? shown : dec;
        snprintf(r[6].value, sizeof(r[6].value), "%.1f ms a picture, to %dx%d (%sswscale)",
                 (st.convert_time - info_prev.convert_time) * 1000 / conv, st.convert_w, st.convert_h,
                 st.halvings == 1 ? "halved, then " : st.halvings == 2 ? "halved twice, then " :
                 st.halvings > 2 ? "halved 3 times, then " : "");
    }
    {
        unsigned dn = st.deinterlaced - info_prev.deinterlaced;
        double dtm = st.deinterlace_time - info_prev.deinterlace_time;
        if (st.deinterlace == REELCORE_DEINT_OFF)
            snprintf(r[7].value, sizeof(r[7].value), "off; %u of %u pictures interlaced", st.interlaced, st.decoded);
        else if (st.deinterlace == REELCORE_DEINT_AUTO && !st.interlaced)
            snprintf(r[7].value, sizeof(r[7].value), "Auto: not needed (no interlaced pictures)");
        else
            snprintf(r[7].value, sizeof(r[7].value), "%s: yadif, %.1f ms a picture; %u of %u pictures interlaced",
                     st.deinterlace == REELCORE_DEINT_ON ? "On" : "Auto", dn ? dtm * 1000 / dn : 0.0,
                     st.interlaced, st.decoded);
    }
    if (draws)
        snprintf(r[8].value, sizeof(r[8].value), "%.1f ms a picture (%s)",
                 (S.draw_cs - info_prev_draw_cs) * 10.0 / draws,
#ifdef REEL_EGL
                 S.fullscreen ? (!S.vsync ? "EGL screen surface, direct" : "EGL screen surface, vsync")
                              : "EGL work area surface, with converting"
#else
                 "OS_SpriteOp, with converting"
#endif
                 );
    snprintf(r[9].value, sizeof(r[9].value), "%d pictures, %d packets (%u KB)",
             st.pictures_waiting, st.packets_waiting, st.packet_bytes >> 10);
    if (st.sound == 0)
        snprintf(r[10].value, sizeof(r[10].value), "none");
    else if (st.sound_stalled)
        snprintf(r[10].value, sizeof(r[10].value), "stalled: the device isn't playing");
    else if (st.sound == 1)
        snprintf(r[10].value, sizeof(r[10].value), "SharedSoundBuffer: %.2f s queued, %.1f MB played",
                 st.sound_queued, st.sound_played / 1048576.0);
    else
        snprintf(r[10].value, sizeof(r[10].value), "SDL: %.2f s queued", st.sound_queued);
    snprintf(r[11].value, sizeof(r[11].value), "%.2f Mbit/s (%.1f MB so far)",
             (st.bytes_read - info_prev.bytes_read) * 8 / dt / 1e6, st.bytes_read / 1048576.0);
    snprintf(r[12].value, sizeof(r[12].value), "%.0f null events a second, %s %.0f%% of the time; screen %dx%d, %d bpp, %s%s",
             (S.st_nulls - info_prev_nulls) / dt, S.nosleep ? "no sleeping (Reel$NoSleep):" : "asleep",
             (S.slept_cs - info_prev_slept) / dt, S.scr_w >> S.xeig, S.scr_h >> S.yeig, 1 << S.log2bpp,
             S.trgb ? "TRGB" : "TBGR", S.fullscreen ? "; full screen" : "");

    {
        char ab[48] = "", pa[16], pb[16];
        if (S.ab) {
            format_time(pa, sizeof(pa), S.ab_a);
            format_time(pb, sizeof(pb), S.ab_b);
            snprintf(ab, sizeof(ab), S.ab == 2 ? "; A-B %s to %s" : "; A at %s", pa, pb);
        }
        snprintf(r[13].value, sizeof(r[13].value), "%s speed, picture %s%s; volume %.0f%%; sound track %d of %d; "
                 "playlist %d of %d%s%s",
                 speed_names[S.speed_i], pic_names[S.pic_mode], st.fast == REELCORE_FAST_ON ? ", fast decode" :
                 st.fast == REELCORE_FAST_LIGHT ? ", light fast decode (mini player)" : "", S.vol * 100,
                 st.audio_track + 1, st.audio_tracks, S.list_n ? S.list_i + 1 : 0, S.list_n, ab,
                 S.vsync ? "; vsync" : "");
    }
    info_prev = st;
    info_prev_cs = t;
    info_prev_nulls = S.st_nulls;
    info_prev_draw_n = S.draw_n;
    info_prev_draw_cs = S.draw_cs;
    info_prev_slept = S.slept_cs;
}

static void text_colour(unsigned fg)
{
    _kernel_swi_regs r;
    r.r[0] = 0;
    r.r[1] = (int)fg;                   /* &BBGGRR00 */
    r.r[2] = (int)0xDDDDDD00u;          /* the window's grey (Wimp colour 1) */
    swi(Wimp_TextOp, &r);
}

static void text_plot(const char *t, int x, int y)
{
    _kernel_swi_regs r;
    r.r[0] = 2;
    r.r[1] = (intptr_t)t;
    r.r[2] = -1;
    r.r[3] = -1;
    r.r[4] = x;
    r.r[5] = y;
    swi(Wimp_TextOp, &r);
}

/* Draws the rows in each rectangle; CLEAR fills the background first (for
   Wimp_UpdateWindow, which doesn't). */
static void info_draw(int *b, int more, int clear)
{
    _kernel_swi_regs r;
    while (more) {
        int ox = b[1] - b[5], oy = b[4] - b[6];      /* screen position of the work area origin */
        if (clear) {
            r.r[0] = 1;
            swi(Wimp_SetColour, &r);
            r.r[0] = 4; r.r[1] = b[7]; r.r[2] = b[8];
            swi(OS_Plot, &r);
            r.r[0] = 96 + 5; r.r[1] = b[9] - 1; r.r[2] = b[10] - 1;
            swi(OS_Plot, &r);
        }
        for (int i = 0; i < info_n; i++) {
            int top = oy - INFO_TOP - i * INFO_ROW, bottom = top - INFO_ROW;
            const info_row_t *row = &info_rows[i];
            if (bottom > b[10] || top < b[8])
                continue;
            if (row->heading) {
                text_colour(0xA0300000u);               /* dark blue */
                text_plot(row->label, ox + 16, bottom + 12);
            } else {
                text_colour(0x55555500u);               /* dark grey */
                text_plot(row->label, ox + INFO_LABEL_X, bottom + 12);
                text_colour(0);
                text_plot(row->value, ox + INFO_VALUE_X, bottom + 12);
            }
        }
        r.r[1] = (intptr_t)b;
        if (swi(Wimp_GetRectangle, &r))
            break;
        more = r.r[0];
    }
}

static int info_height(void) { return INFO_TOP * 2 + info_n * INFO_ROW; }

static void info_redraw(int *block)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)block;
    if (swi(Wimp_RedrawWindow, &r))
        return;
    info_draw(block, r.r[0], 0);
}

/* Redraws just the stats (every second). */
static void info_update(void)
{
    int b[11];
    _kernel_swi_regs r;
    if (!S.info_open)
        return;
    b[0] = S.info;
    b[1] = 0;
    b[2] = -(INFO_TOP + (info_stats_at + N_STATS) * INFO_ROW);
    b[3] = INFO_W * 2;
    b[4] = -(INFO_TOP + info_stats_at * INFO_ROW);
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_UpdateWindow, &r))
        return;
    info_draw(b, r.r[0], 1);
}

static int info_create(void)
{
    struct {
        box_t vis;
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        box_t ext;
        int tflags, wbutton, sprites;
        short minw, minh;
        ind_t title;
        int nicons;
    } w;
    _kernel_swi_regs r;

    memset(&w, 0, sizeof(w));
    w.vis.x1 = INFO_W; w.vis.y1 = 600;
    w.behind = -1;
    w.flags = (int)0xBF000002u;         /* new format, back, close, title, toggle, v scroll, adjust size, moveable */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext.x0 = 0; w.ext.y0 = -info_height(); w.ext.x1 = INFO_W * 2; w.ext.y1 = 0;
    w.tflags = IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 2);
    w.wbutton = 0;                      /* clicks ignored */
    w.sprites = 1;
    w.minw = 400; w.minh = 200;
    w.title.text = S.info_title;
    w.title.valid = (const char *)-1;
    w.title.len = sizeof(S.info_title);
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    S.info = r.r[0];
    return 0;
}

static void info_open(void)
{
    int b[8], h;
    _kernel_swi_regs r;
    if (!S.v)
        return;
    if (!S.info) {
        snprintf(S.info_title, sizeof(S.info_title), "Media info: %s", S.title);
        info_build();
        if (info_create() < 0)
            return;
    }
    info_stats();
    h = info_height();
    if (h > S.scr_h * 3 / 4)
        h = S.scr_h * 3 / 4;
    b[0] = S.info;
    if (S.info_open) {                  /* already open: bring it to the front */
        window_state(S.info, b);
        b[7] = -1;
    } else {
        b[1] = S.scr_w - INFO_W - 96;
        if (b[1] < 0) b[1] = 0;
        b[3] = b[1] + INFO_W;
        b[4] = S.scr_h - 160;
        b[2] = b[4] - h;
        b[5] = 0; b[6] = 0; b[7] = -1;
    }
    r.r[1] = (intptr_t)b;
    swi(Wimp_OpenWindow, &r);
    S.info_open = 1;
    lg("media info window open");
}

static void info_close(void)
{
    _kernel_swi_regs r;
    if (!S.info)
        return;
    r.r[1] = (intptr_t)&S.info;
    swi(Wimp_CloseWindow, &r);
    swi(Wimp_DeleteWindow, &r);         /* made again for the next file */
    S.info = 0;
    S.info_open = 0;
}

/* A new file: fresh rows (and the window again, if it was open). */
static void info_new_file(void)
{
    int was_open = S.info_open;
    reelcore_stats(S.v, &info_prev);
    info_prev_cs = now_cs();
    info_prev_nulls = S.st_nulls;
    info_prev_draw_n = S.draw_n;
    info_prev_draw_cs = S.draw_cs;
    info_prev_slept = S.slept_cs;
    info_close();
    if (was_open)
        info_open();
}

static void info_toggle(void)
{
    if (S.info_open)
        info_close();
    else
        info_open();
}

/* ---- menus -------------------------------------------------------------------- */

/* Menu items have indirected text, so they can be long (file names) */
#define MENU_MAX 24
typedef struct { int flags, sub, iflags; char *text; const char *valid; int len; } item_t;
typedef struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    item_t item[MENU_MAX];
} menu_t;
static menu_t menu, m_pic, m_speed, m_track, m_list, m_deint, m_size;
static char menu_text[7][MENU_MAX][72];
static int menu_is_bar;                 /* the open menu: 1 icon bar, 0 window */
static int menu_x, menu_y;

/* The window menu */
enum { WM_INFO, WM_FULL, WM_MINI, WM_ONTOP, WM_SIZE, WM_PIC, WM_DEINT, WM_SPEED, WM_TRACK, WM_LIST, WM_AB, WM_LOOP, WM_FAST, WM_VSYNC, WM_CLOSE, WM_N };

static void menu_start(menu_t *m, const char *title)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->title, sizeof(m->title), "%s", title);
    m->tfg = 7; m->tbg = 2; m->wfg = 7; m->wbg = 0;
    m->width = 12 * 16 + 32;
    m->height = 44;
}

/* Adds an item; sub: a submenu (NULL = none); shaded: can't be chosen */
static void menu_add(menu_t *m, int which, int *n, const char *text, int tick, const menu_t *sub, int shaded)
{
    item_t *it = &m->item[*n];
    char *t = menu_text[which][*n];
    int w;
    snprintf(t, sizeof(menu_text[0][0]), "%s", text);
    it->flags = tick ? 1 : 0;
    it->sub = sub ? (int)(intptr_t)sub : -1;
    it->iflags = 0x07000021 | IF_INDIR | (shaded ? 1 << 22 : 0);
    it->text = t;
    it->valid = (const char *)-1;
    it->len = (int)strlen(t) + 1;
    w = (int)strlen(t) * 16 + 48;
    if (w > m->width)
        m->width = w;
    (*n)++;
}

static void menu_end(menu_t *m, int n)
{
    if (n > 0)
        m->item[n - 1].flags |= 0x80;
}

static void menu_open(int bar, int x, int y)
{
    _kernel_swi_regs r;
    int n = 0;
    char t[80];
    menu_start(&menu, APP);
    if (bar) {
        menu_add(&menu, 0, &n, "Info", 0, NULL, 0);
        menu.item[0].sub = S.proginfo;  /* the About this program window (-1: none) */
        menu_add(&menu, 0, &n, "Loop", S.loop, NULL, 0);
        menu_add(&menu, 0, &n, "Log", 0, NULL, 0);
        menu_add(&menu, 0, &n, "Quit", 0, NULL, 0);
        y = 96 + n * 44;
        x -= 64;
    } else {
        int k = 0, tracks = S.v ? reelcore_audio_tracks(S.v) : 0;
        menu_start(&m_size, "Window size");
        k = 0;
        for (int i = 0; i < N_SIZE; i++)
            menu_add(&m_size, 6, &k, size_names[i], 0, NULL, 0);
        menu_end(&m_size, k);
        menu_start(&m_pic, "Picture");
        for (int i = 0; i < N_PIC; i++)
            menu_add(&m_pic, 1, &k, pic_names[i], S.pic_mode == i, NULL, 0);
        menu_end(&m_pic, k);
        menu_start(&m_deint, "Deinterlace");
        k = 0;
        for (int i = 0; i < 3; i++)
            menu_add(&m_deint, 5, &k, deint_names[i], S.deint_i == i, NULL, 0);
        menu_end(&m_deint, k);
        menu_start(&m_speed, "Speed");
        k = 0;
        for (int i = 0; i < N_SPEED; i++)
            menu_add(&m_speed, 2, &k, speed_names[i], S.speed_i == i, NULL, 0);
        menu_end(&m_speed, k);
        menu_start(&m_track, "Sound track");
        k = 0;
        for (int i = 0; i < tracks && i < MENU_MAX; i++) {
            char name[64];
            reelcore_audio_track_name(S.v, i, name, sizeof(name));
            snprintf(t, sizeof(t), "%d: %s", i + 1, name);
            menu_add(&m_track, 3, &k, t, reelcore_audio_track(S.v) == i, NULL, 0);
        }
        menu_end(&m_track, k);
        menu_start(&m_list, "Playlist");
        k = 0;
        for (int i = 0; i < S.list_n && i < MENU_MAX - 1; i++)
            menu_add(&m_list, 4, &k, leaf(S.list[i]), S.list_i == i, NULL, 0);
        if (k)
            m_list.item[k - 1].flags |= 2;          /* a dotted line before Clear */
        menu_add(&m_list, 4, &k, "Clear the rest", 0, NULL, S.list_n <= 1);
        menu_end(&m_list, k);

        menu_add(&menu, 0, &n, "Media info", S.info_open, NULL, 0);
        menu_add(&menu, 0, &n, "Full screen", S.fullscreen, NULL, 0);
        menu_add(&menu, 0, &n, "Mini player", mini, NULL, !S.v);
        menu_add(&menu, 0, &n, "Keep on top", S.ontop, NULL, 0);
        menu_add(&menu, 0, &n, "Window size", 0, mini ? NULL : &m_size, mini);   /* the mini player: its grip */
        menu_add(&menu, 0, &n, "Picture", 0, &m_pic, 0);
        snprintf(t, sizeof(t), "Deinterlace (%s)", deint_names[S.deint_i]);
        menu_add(&menu, 0, &n, t, 0, &m_deint, 0);
        snprintf(t, sizeof(t), "Speed (%s)", speed_names[S.speed_i]);
        menu_add(&menu, 0, &n, t, 0, &m_speed, 0);
        menu_add(&menu, 0, &n, "Sound track", 0, tracks > 1 ? &m_track : NULL, tracks < 2);
        snprintf(t, sizeof(t), "Playlist (%d)", S.list_n);
        menu_add(&menu, 0, &n, t, 0, &m_list, 0);
        menu_add(&menu, 0, &n, S.ab == 0 ? "A-B repeat: set A" : S.ab == 1 ? "A-B repeat: set B" : "A-B repeat: off",
                 S.ab == 2, NULL, !S.v);
        menu_add(&menu, 0, &n, "Loop", S.loop, NULL, 0);
        menu_add(&menu, 0, &n, "Fast decode", S.fast, NULL, 0);
        menu_add(&menu, 0, &n, "Vsync (full screen)", S.vsync, NULL, 0);
        menu_add(&menu, 0, &n, "Close", 0, NULL, 0);
        menu.item[WM_ONTOP].flags |= 2;             /* dotted lines between the groups */
        menu.item[WM_LIST].flags |= 2;
        menu.item[WM_VSYNC].flags |= 2;
        x -= 64;
    }
    menu_end(&menu, n);
    menu_is_bar = bar;
    menu_x = x; menu_y = y;
    r.r[1] = (intptr_t)&menu;
    r.r[2] = x;
    r.r[3] = y;
    swi(Wimp_CreateMenu, &r);
}

/* ---- the options ------------------------------------------------------------- */

static void set_volume(double vol)
{
    S.vol = vol < 0 ? 0 : vol > 1 ? 1 : vol;
    if (S.v)
        reelcore_set_volume(S.v, S.vol * S.vol);    /* the bar is roughly how loud it sounds */
    lg("volume %.0f%%", S.vol * 100);
    choices_save();
    if (S.win && !S.fullscreen)
        layout(S.vis_w, S.vis_h), force_redraw(S.win, 0, -S.vis_h, S.vis_w, -S.vis_h + CH);
}

static void set_pic_mode(int mode)
{
    S.pic_mode = mode;
    lg("picture: %s", pic_names[mode]);
    pic_refresh();
    force_redraw(S.fullscreen ? S.full : S.win, 0, -8192, 8192, 0);
}

static void set_speed_i(int i)
{
    S.speed_i = i;
    if (S.v)
        reelcore_set_speed(S.v, speeds[i]);
    lg("speed %s", speed_names[i]);
    update_controls(1);
}

/* Deinterlace: Auto, On, Off (remembered) */
static void set_deint_i(int i)
{
    S.deint_i = i;
    if (S.v)
        reelcore_set_deinterlace(S.v, deint_modes[i]);
    lg("deinterlace %s", deint_names[i]);
    choices_save();
}

/* Fast decode (the window menu) skips the deblocking filter on every
   picture. The mini player, with it off, skips it only on the pictures no
   other is predicted from (REELCORE_FAST_LIGHT): about 15% less decoding
   for typical H.264, invisible at that size, and nothing carries over
   when the window goes back to normal. */
static void apply_fast(void)
{
    int mode = S.fast ? REELCORE_FAST_ON : mini ? REELCORE_FAST_LIGHT : REELCORE_FAST_OFF;
    if (S.v && reelcore_fast(S.v) != mode) {
        reelcore_set_fast(S.v, mode);
        lg("decoding: %s", mode == REELCORE_FAST_ON ? "fast (no deblocking)" :
                           mode == REELCORE_FAST_LIGHT ? "light fast (the mini player)" : "normal");
    }
}

static void set_fast(int on)
{
    S.fast = on;
    apply_fast();
}

/* A-B repeat: the first press marks A, the second B (and repeats), the
   third turns it off */
static void ab_press(void)
{
    if (!S.v)
        return;
    if (S.ab == 0) {
        S.ab_a = reelcore_position(S.v);
        S.ab = 1;
        lg("A-B repeat: A at %.2f", S.ab_a);
    } else if (S.ab == 1) {
        S.ab_b = reelcore_position(S.v);
        if (S.ab_b <= S.ab_a + 0.2) {       /* B before A: start again from here */
            S.ab_a = S.ab_b;
            return;
        }
        S.ab = 2;
        lg("A-B repeat: B at %.2f, repeating", S.ab_b);
    } else {
        S.ab = 0;
        lg("A-B repeat off");
    }
    update_controls(1);
}

static void set_vsync(int on)
{
    S.vsync = on;
    lg("vsync %s", on ? "on" : "off");
#ifdef REEL_EGL
    if (S.fullscreen) {                     /* make the screen surface again */
        surf_free();
        pic_make(S.scr_w >> S.xeig, S.scr_h >> S.yeig, 1);
        pic_refresh();
    }
#endif
}

/* ---- the mini player ------------------------------------------------------------ */

/* The top of the icon bar, OS units (the icon bar is window -2) */
static int iconbar_top(void)
{
    int b[9];
    _kernel_swi_regs r;
    b[0] = -2;
    r.r[1] = (intptr_t)b;
    if (!swi(Wimp_GetWindowState, &r) && b[4] > 0 && b[4] < 512)
        return b[4];
    return 134;                         /* the usual height */
}

/* Window size (the window menu): the picture at half, the same or double
   the video's size in screen pixels, or as big as fits above the icon bar;
   never narrower than the controls need (the picture is then letterboxed).
   The window keeps its top left corner, moved in if it would go off the
   screen. The grip (bottom right) resizes it freely. */
static void set_size(int k)
{
    int st[9], vw, vh, pw, ph, x0, y1, top, ibar;
    double f;
    if (!S.v || mini || S.fullscreen)
        return;
    read_screen();
    ibar = iconbar_top();
    top = S.scr_h - 44;                 /* below the title bar */
    pw = reelcore_width(S.v) << S.xeig;
    ph = reelcore_height(S.v) << S.yeig;
    if (pw <= 0 || ph <= 0)
        return;
    f = k == SIZE_HALF ? 0.5 : k == SIZE_DOUBLE ? 2 : 1;
    if (k == SIZE_SCREEN || pw * f > S.scr_w - 16 || ph * f + CH > top - ibar - 16) {
        double fw = (S.scr_w - 16) / (double)pw, fh = (top - ibar - 16 - CH) / (double)ph;
        double fit = fw < fh ? fw : fh;
        if (k == SIZE_SCREEN || fit < f)
            f = fit;                    /* as big as fits (Double on a big video: this too) */
    }
    vw = (int)(pw * f);
    vh = (int)(ph * f);
    vw -= vw % (1 << S.xeig);
    vh -= vh % (1 << S.yeig);
    if (vw < MIN_W) vw = MIN_W;
    vh += CH;
    window_state(S.win, st);
    x0 = st[1];
    y1 = st[4];
    if (x0 + vw > S.scr_w) x0 = S.scr_w - vw;
    if (x0 < 0) x0 = 0;
    if (y1 > top) y1 = top;
    if (y1 - vh < ibar) y1 = ibar + vh;
    lg("window size: %s, %dx%d OS units", size_names[k], vw, vh);
    open_window_at(x0, y1, vw, vh);
    pic_refresh();
    force_redraw(S.win, 0, -8192, 8192, 0);
    update_controls(1);
}

/* The mini player's picture height for a width: the video's shape */
static int mini_pic_h(int vw)
{
    int ph = S.v && reelcore_width(S.v) > 0 ? (int)((long long)vw * reelcore_height(S.v) / reelcore_width(S.v))
                                            : vw * 9 / 16;
    int most = S.scr_h - CH - 160;
    if (ph < vw / 4) ph = vw / 4;       /* very wide, or tall: the picture is letterboxed */
    if (ph > vw * 3 / 4) ph = vw * 3 / 4;
    if (ph > most) ph = most;
    return ph - ph % (1 << S.yeig);
}

/* Opens (or moves and resizes) the mini player for the current video: the width it
   was last given (at first 320 pixels on most screens), as tall as the video's shape needs, in the
   bottom right just above the icon bar, or where it was dragged to. */
static void mini_show(void)
{
    int vw = S.mini_w, ph, vh, x1, y0;
    read_screen();
    if (vw > S.scr_w) vw = S.scr_w;
    ph = mini_pic_h(vw);
    vh = ph + CH;
    x1 = S.scr_w - S.mini_right;
    y0 = S.mini_bottom >= 0 ? S.mini_bottom : iconbar_top() + MINI_LIFT;
    if (x1 > S.scr_w) x1 = S.scr_w;     /* on the screen, even after a mode change */
    if (x1 < vw) x1 = vw;
    if (y0 + vh > S.scr_h) y0 = S.scr_h - vh;
    if (y0 < 0) y0 = 0;
    lg("mini player %dx%d OS units at %d,%d", vw, vh, x1 - vw, y0);
    open_window_at(x1 - vw, y0 + vh, vw, vh);
    pic_refresh();
    force_redraw(S.win, 0, -8192, 8192, 0);
    update_controls(1);
}

/* Switches between the normal window and the mini player */
static void set_mini(int on)
{
    _kernel_swi_regs r;
    if (on == mini || !S.v || !S.win)
        return;
    if (S.fullscreen)
        set_fullscreen(0);
    lg("mini player %s", on ? "on" : "off");
    pic_free();                         /* the EGL surface belongs to the window it was made for */
    if (on) {
        if (!S.mini_win && create_mini_window() < 0) {
            report("Can't create the mini player.");
            return;
        }
        window_state(S.main_win, S.main_st);    /* to come back to */
        r.r[1] = (intptr_t)&S.main_win;
        swi(Wimp_CloseWindow, &r);
        mini = 1;
        S.win = S.mini_win;
        apply_fast();
        S.ontop_cs = now_cs();
        mini_show();
    } else {
        r.r[1] = (intptr_t)&S.mini_win;
        swi(Wimp_CloseWindow, &r);
        mini = 0;
        S.win = S.main_win;
        apply_fast();
        open_window_at(S.main_st[1], S.main_st[4], S.main_st[3] - S.main_st[1], S.main_st[4] - S.main_st[2]);
        pic_refresh();
        force_redraw(S.win, 0, -8192, 8192, 0);
        update_controls(1);
    }
    choices_save();                     /* (where the mini player was) */
    set_caret(S.win);
}

/* Keep on top: while playing, about once a second, the mini player comes
   back to the front if another window has been opened over it. It never
   takes the caret. */
static void mini_keep_on_top(int t)
{
    int st[9];
    _kernel_swi_regs r;
    if (!mini || !S.ontop || S.fullscreen || t - S.ontop_cs < 100)
        return;
    S.ontop_cs = t;
    window_state(S.win, st);
    if (st[7] == -1)                    /* already at the front */
        return;
    st[7] = -1;
    r.r[1] = (intptr_t)st;
    swi(Wimp_OpenWindow, &r);
}

static void quit(void)
{
    _kernel_swi_regs r;
    close_video();
    lg("quit");
    r.r[0] = S.task;
    swi(Wimp_CloseDown, &r);
    exit(0);
}

static void menu_select(const int *sel)
{
    int b[5];
    _kernel_swi_regs r;
    if (menu_is_bar) {
        switch (sel[0]) {
        case 0: break;                  /* Info: its window is the submenu */
        case 1: S.loop = !S.loop; break;
        case 2: log_show(); break;
        case 3: quit();
        }
    } else {
        switch (sel[0]) {
        case WM_INFO: info_toggle(); break;
        case WM_FULL: set_fullscreen(!S.fullscreen); break;
        case WM_MINI: set_mini(!mini); break;
        case WM_ONTOP:
            S.ontop = !S.ontop;
            lg("keep on top %s", S.ontop ? "on" : "off");
            choices_save();
            break;
        case WM_SIZE: if (sel[1] >= 0 && sel[1] < N_SIZE) set_size(sel[1]); break;
        case WM_PIC: if (sel[1] >= 0 && sel[1] < N_PIC) set_pic_mode(sel[1]); break;
        case WM_DEINT: if (sel[1] >= 0 && sel[1] < 3) set_deint_i(sel[1]); break;
        case WM_SPEED: if (sel[1] >= 0 && sel[1] < N_SPEED) set_speed_i(sel[1]); break;
        case WM_TRACK:
            if (sel[1] >= 0 && S.v && reelcore_set_audio_track(S.v, sel[1]) == 0)
                lg("sound track %d", sel[1] + 1);
            break;
        case WM_LIST:
            if (sel[1] >= 0 && sel[1] < S.list_n && sel[1] < MENU_MAX - 1)
                list_play(sel[1]);
            else if (sel[1] >= 0 && S.list_n > 1) {     /* Clear the rest: keep the one playing */
                char *keep = S.list[S.list_i];
                S.list[S.list_i] = NULL;
                list_clear();
                S.list[0] = keep;
                S.list_n = 1;
                set_title();
            }
            break;
        case WM_AB: ab_press(); break;
        case WM_LOOP: S.loop = !S.loop; break;          /* a single file: from the next one */
        case WM_FAST: set_fast(!S.fast); break;
        case WM_VSYNC: set_vsync(!S.vsync); break;
        case WM_CLOSE: close_video(); return;
        }
    }
    r.r[1] = (intptr_t)b;
    if (!swi(Wimp_GetPointerInfo, &r) && (b[2] & 1))
        menu_open(menu_is_bar, menu_x + 64, menu_y);
}

/* ---- playback control ---------------------------------------------------------- */

static void toggle_pause(void)
{
    if (!S.v)
        return;
    if (S.ended) {
        S.ended = 0;
        reelcore_seek(S.v, 0);
        reelcore_pause(S.v, 0);
    } else
        reelcore_pause(S.v, !reelcore_paused(S.v));
    lg("%s at %.2f", reelcore_paused(S.v) ? "pause" : "play", reelcore_position(S.v));
    if (S.info_open) {                  /* no null events while paused: show where it stopped */
        info_stats();
        info_update();
    }
    update_controls(1);
}

static void seek_by(double d)
{
    double p, dur;
    if (!S.v)
        return;
    p = reelcore_position(S.v) + d;
    dur = reelcore_duration(S.v);
    if (p < 0) p = 0;
    if (dur > 0 && p > dur - 1) p = dur - 1;
    S.ended = 0;
    lg("seek %+.0f s to %.2f", d, p);
    reelcore_seek(S.v, p);
    update_controls(1);
}

static void click_track(int mouse_x)
{
    int b[11], x0, x1, wx;
    double d;
    if (!S.v || (d = reelcore_duration(S.v)) <= 0)
        return;
    window_state(S.win, b);
    wx = mouse_x - (b[1] - b[5]);
    track_x(S.vis_w, &x0, &x1);
    x0 += 4;
    x1 -= 4;
    if (x1 <= x0)
        return;
    if (wx < x0) wx = x0;
    if (wx > x1) wx = x1;
    S.ended = 0;
    lg("seek (position bar) to %.2f", d * (wx - x0) / (x1 - x0));
    reelcore_seek(S.v, d * (wx - x0) / (x1 - x0));
    update_controls(1);
}

static void key(int *b)
{
    _kernel_swi_regs r;
    switch (b[6]) {
    case ' ': toggle_pause(); return;
    case 0x18C: seek_by(-10); return;               /* Left */
    case 0x18D: seek_by(10); return;                /* Right */
    case 0x18E: seek_by(-60); return;               /* Down */
    case 0x18F: seek_by(60); return;                /* Up */
    case 'f': case 'F': set_fullscreen(!S.fullscreen); return;
    case 27:
        if (S.fullscreen) { set_fullscreen(0); return; }
        break;
    case 'q': case 'Q': close_video(); return;
    case 'i': case 'I': info_toggle(); return;
    case 'm': case 'M': set_mini(!mini); return;
    case 'd': case 'D': set_deint_i((S.deint_i + 1) % 3); return;
    case 'a': case 'A': ab_press(); return;
    case 'n': case 'N': if (S.list_i + 1 < S.list_n) list_play(S.list_i + 1); return;
    case 'p': case 'P': if (S.list_i > 0) list_play(S.list_i - 1); return;
    }
    r.r[0] = b[6];
    swi(Wimp_ProcessKey, &r);
}

static void tick(void)
{
    int r2, t;
    _kernel_swi_regs r;
    if (!S.v || S.ended)
        return;
    r2 = reelcore_update(S.v);
    S.log_nulls++;
    S.st_nulls++;
    S.idle_cs = S.nosleep ? 0 : (int)(reelcore_idle_time(S.v) * 100);  /* whole centiseconds: wake no later than due */
    if (r2 == REELCORE_NEW_FRAME) {
        show_frame();
        S.log_frames++;
    } else if (r2 == REELCORE_END) {
        lg("end of the file");
        S.ended = 1;
        if (S.list_i + 1 < S.list_n) {          /* the playlist: the next file */
            list_play(S.list_i + 1);
            return;
        }
        if (S.loop && S.list_n > 1) {           /* ... and round again */
            list_play(0);
            return;
        }
        update_controls(1);
        return;
    }
    if (S.ab == 2 && reelcore_position(S.v) >= S.ab_b) {
        reelcore_seek(S.v, S.ab_a);             /* A-B repeat: back to A */
        update_controls(1);
    }
    swi(OS_ReadMonotonicTime, &r);
    t = r.r[0];
    if (logf && t - S.log_cs >= 100) {    /* once a second */
        char d[300];
        /* what each picture cost (full screen too, where Media info can't be seen) */
        static ReelCoreStats p;
        static unsigned p_draw_n, p_draw_cs;
        ReelCoreStats st;
        unsigned dec, shown, draws;
        reelcore_stats(S.v, &st);
        if (st.decoded < p.decoded)       /* a new file */
            memset(&p, 0, sizeof(p));
        dec = st.decoded - p.decoded;
        shown = st.shown - p.shown;
        draws = S.draw_n - p_draw_n;
        reelcore_debug(S.v, d, sizeof(d));
        lg("%s; %d nulls, %d pictures in %.2f s, asleep %u%%; ms a picture: decode %.1f, convert %.1f (to %dx%d), "
           "draw %.1f, deinterlace %.1f", d, S.log_nulls, S.log_frames, (t - S.log_cs) / 100.0,
           (unsigned)((S.slept_cs - S.log_slept) * 100 / (t - S.log_cs)),
           dec ? (st.decode_time - p.decode_time) * 1000 / dec : 0.0,
           shown ? (st.convert_time - p.convert_time) * 1000 / shown : 0.0, st.convert_w, st.convert_h,
           draws ? (S.draw_cs - p_draw_cs) * 10.0 / draws : 0.0,
           st.deinterlaced > p.deinterlaced ? (st.deinterlace_time - p.deinterlace_time) * 1000 / (st.deinterlaced - p.deinterlaced) : 0.0);
        p = st;
        p_draw_n = S.draw_n;
        p_draw_cs = S.draw_cs;
        S.log_slept = S.slept_cs;
        S.log_cs = t;
        S.log_nulls = S.log_frames = 0;
    }
    mini_keep_on_top(t);
    if (t - info_prev_cs >= 100) {        /* the stats: once a second */
        info_stats();
        info_update();
    }
    if (t - S.last_time_cs >= 20) {
        S.last_time_cs = t;
        if (!S.fullscreen)
            update_controls(0);
    }
}

/* ---- messages ------------------------------------------------------------------ */

static int is_video_type(int type)
{
    char mime[64];
    _kernel_swi_regs r;
    r.r[0] = 0;                         /* from a filetype */
    r.r[1] = type;
    r.r[2] = 1;                         /* to a MIME type */
    r.r[3] = (intptr_t)mime;
    mime[0] = 0;
    if (swi(MimeMap_Translate, &r))
        return 0;
    return !strncmp(mime, "video/", 6);
}

static void ack(int *b)
{
    _kernel_swi_regs r;
    int to = b[1];
    b[3] = b[2];
    b[4] = MSG_DATALOADACK;
    r.r[0] = 17;
    r.r[1] = (intptr_t)b;
    r.r[2] = to;
    swi(Wimp_SendMessage, &r);
}

static void message(int *b)
{
    char file[256];
    switch (b[4]) {
    case MSG_QUIT:
        quit();
        break;
    case MSG_DATALOAD:
        if ((b[5] == -2 && b[6] == S.bar_icon) || (S.win && b[5] == S.win) || (S.full && b[5] == S.full)) {
            if (b[10] == 0x1000 || b[10] == 0x2000) {
                report("That's a directory: drop a video file.");
                break;
            }
            snprintf(file, sizeof(file), "%s", (const char *)&b[11]);
            ack(b);
            list_arrived(file);
        }
        break;
    case MSG_DATAOPEN:
        if (is_video_type(b[10])) {
            snprintf(file, sizeof(file), "%s", (const char *)&b[11]);
            ack(b);                     /* claims it */
            list_arrived(file);
        }
        break;
    case MSG_MODECHANGE:
        lg("mode change");
        read_screen();
        if (S.fullscreen) {
            set_fullscreen(0);
        } else if (S.v && mini) {
            mini_show();                /* back on the screen, above the icon bar */
        } else if (S.v && S.win) {
            int st[9];
            window_state(S.win, st);
            layout(st[3] - st[1], st[4] - st[2]);
            pic_refresh();
            force_redraw(S.win, 0, -8192, 8192, 0);
        }
        break;
    }
}

/* ---- set up and the poll loop ---------------------------------------------------- */

static int already_running(void)
{
    int buf[16 * 4];
    _kernel_swi_regs r;
    r.r[0] = 0;
    do {
        r.r[1] = (intptr_t)buf;
        r.r[2] = sizeof(buf);
        if (swi(TaskManager_EnumerateTasks, &r))
            return 0;
        for (int *p = buf; (char *)p < (char *)(intptr_t)r.r[1]; p += 4)
            if (p[0] != S.task) {
                const char *name = (const char *)(intptr_t)p[1];
                size_t n = strlen(APP);
                if (!strncmp(name, APP, n) && (unsigned char)name[n] < 32)
                    return 1;
            }
    } while (r.r[0] >= 0);
    return 0;
}

static void iconbar_icon(void)
{
    struct { int w, x0, y0, x1, y1, flags; char name[12]; } b;
    _kernel_swi_regs r;
    memset(&b, 0, sizeof(b));
    b.w = -1;
    b.x1 = 68; b.y1 = 68;
    b.flags = 0x301A;
    strcpy(b.name, ICON);
    r.r[0] = 0;
    r.r[1] = (intptr_t)&b;
    swi(Wimp_CreateIcon, &r);
    S.bar_icon = r.r[0];
}

int reel_main(int argc, char **argv)
{
    static const int messages[] = { MSG_DATALOAD, MSG_DATAOPEN, MSG_PREQUIT, MSG_MODECHANGE, 0 };
    int block[64];
    _kernel_swi_regs r;

    memset(&S, 0, sizeof(S));
    snprintf(S.play_text, sizeof(S.play_text), "Pause");
    S.speed_i = 2;                      /* normal */
    S.mini_right = MINI_EDGE;           /* the mini player: bottom right, above the icon bar */
    S.mini_w = MINI_W;
    S.mini_bottom = -1;
    S.nosleep = getenv(APP "$NoSleep") != NULL;
    S.vsync = getenv(APP "$NoVsync") == NULL;
    S.vol = 1;
    r.r[0] = 380;
    r.r[1] = 0x4B534154;
    r.r[2] = (intptr_t)APP;
    r.r[3] = (intptr_t)messages;
    if (swi(Wimp_Initialise, &r))
        return 1;
    S.task = r.r[1];
    if (already_running()) {
        r.r[0] = S.task;
        swi(Wimp_CloseDown, &r);
        return 0;
    }
    log_open();
    choices_load();
    read_screen();
    iconbar_icon();
    S.proginfo = proginfo_create(APP, PURPOSE " (FFmpeg 5.1.10)", APP_AUTHOR,
                                 REEL_VERSION " (" REEL_DATE ")");
    if (argc > 1)
        list_arrived(argv[1]);

    for (;;) {
        int playing = S.v && !S.ended && !reelcore_paused(S.v);
        int sleep_cs = playing ? S.idle_cs : 0;
        r.r[0] = (playing ? 0 : 1) | (1 << 4) | (1 << 5);
        r.r[1] = (intptr_t)block;
        S.idle_cs = 0;                  /* until the next null says otherwise */
        if (sleep_cs > 0) {
            /* nothing to do until the next picture is due: let other tasks
               have the time (a null comes back at that time, or later if
               the desktop is busy; events still come at once) */
            r.r[2] = now_cs() + sleep_cs;
            S.slept_cs += sleep_cs;
            if (swi(Wimp_PollIdle, &r))
                continue;
        } else if (swi(Wimp_Poll, &r))
            continue;
        switch (r.r[0]) {
        case 0:  tick(); break;                            /* null */
        case 1:
            if (S.info && block[0] == S.info)
                info_redraw(block);
            else
                redraw(block);
            break;
        case 2: {                                          /* Open_Window_Request */
            int resized = block[0] == S.win &&
                          (block[3] - block[1] != S.vis_w || block[4] - block[2] != S.vis_h);
            if (resized && mini) {                     /* the grip: keep the video's shape, top left put */
                int vw = block[3] - block[1];
                if (vw < MINI_MIN_W)
                    block[3] = block[1] + (vw = MINI_MIN_W);
                block[2] = block[4] - (mini_pic_h(vw) + CH);
                S.mini_w = vw;
                resized = block[3] - block[1] != S.vis_w || block[4] - block[2] != S.vis_h;
            }
            if (mini && block[0] == S.win) {           /* the mini player dragged: remember where */
                S.mini_right = S.scr_w - block[3];
                S.mini_bottom = block[2];
                if (S.mini_right < 0) S.mini_right = 0;
                if (S.mini_bottom < 0) S.mini_bottom = 0;
            }
            if (resized)
                layout(block[3] - block[1], block[4] - block[2]);
            r.r[1] = (intptr_t)block;
            swi(Wimp_OpenWindow, &r);
            if (resized) {
                pic_refresh();                             /* the picture at its new size */
                force_redraw(S.win, 0, -8192, 8192, 0);
            }
            break;
        }
        case 3:                                            /* Close_Window_Request */
            if (block[0] == S.win)
                close_video();
            else if (block[0] == S.full)
                set_fullscreen(0);
            else if (S.info && block[0] == S.info)
                info_close();
            break;
        case 6:                                            /* Mouse_Click */
            if (block[3] == -2) {
                if (block[2] & 2)
                    menu_open(1, block[0], 0);
                else if (S.v && S.win && !S.fullscreen) {
                    int st[9];
                    window_state(S.win, st);
                    st[7] = -1;
                    r.r[1] = (intptr_t)st;
                    swi(Wimp_OpenWindow, &r);
                } else if (!S.v)
                    report("Drop a video file on the Reel icon to play it.");
                break;
            }
            if (block[2] & 2) {
                menu_open(0, block[0], block[1]);
                break;
            }
            if (block[3] == S.full) {
                set_caret(S.full);
                toggle_pause();
                break;
            }
            if (block[3] != S.win)
                break;
            set_caret(S.win);
            if (mini && block[4] == -1) {              /* the mini player's picture */
                if (block[2] & (64 | 16)) {             /* drag: move the window */
                    int d[10];
                    d[0] = S.win;
                    d[1] = 1;                           /* drag type 1: the window's position */
                    memset(d + 2, 0, 8 * sizeof(int));
                    r.r[1] = (intptr_t)d;
                    swi(Wimp_DragBox, &r);
                } else if (block[2] & (4 | 1))          /* double-click: the normal window */
                    set_mini(0);
                break;
            }
            switch (block[4]) {
            case I_PLAY: toggle_pause(); break;
            case I_BACK: seek_by(-10); break;
            case I_FWD:  seek_by(10); break;
            case I_TRACK: case I_FILL: click_track(block[0]); break;
            case I_VOL: case I_VOLFILL: {
                int st[9], vx0, vx1;
                window_state(S.win, st);
                vol_x(S.vis_w, &vx0, &vx1);
                set_volume((block[0] - (st[1] - st[5]) - vx0 - 4) / (double)(W_VOL - 8));
                break;
            }
            case I_FULL: set_fullscreen(1); break;
            case I_NORMAL: if (mini) set_mini(0); break;
            case I_GRIP: {                              /* the Wimp resizes it: Open_Window_Requests follow */
                int d[10] = { S.win, 2 };               /* drag type 2: the window's size */
                r.r[1] = (intptr_t)d;
                swi(Wimp_DragBox, &r);
                break;
            }
            case -1:
                if (S.v) {                     /* a click on the picture pauses */
                    int st[9];
                    window_state(S.win, st);
                    if (block[1] > st[2] + CH)
                        toggle_pause();
                }
                break;
            }
            break;
        case 8:  key(block); break;
        case 9:  menu_select(block); break;
        case 17: case 18: message(block); break;
        }
    }
}

#ifdef REEL_TEST
ReelCore *reel_test_video(void) { return S.v; }
#ifdef REEL_EGL
int reel_test_surface(int *w, int *h, int *full)
{
    *w = S.surf_w; *h = S.surf_h; *full = S.surf_full;
    return S.surf != EGL_NO_SURFACE;
}
#else
const uint8_t *reel_test_sprite(int *w, int *h, int *rows)
{
    *w = S.spr_w; *h = S.spr_h; *rows = S.spr_rows;
    return S.area ? sprite_pixels() : NULL;
}
#endif
int reel_test_fullscreen(void) { return S.fullscreen; }
int reel_test_pic_flags(void) { return pic_flags_of[S.pic_mode]; }
int reel_test_ab(double *a, double *b) { *a = S.ab_a; *b = S.ab_b; return S.ab; }
int reel_test_list(int *n) { *n = S.list_n; return S.list_i; }
int reel_test_mini(int *ontop) { *ontop = S.ontop; return mini; }
int reel_test_deint(void) { return deint_modes[S.deint_i]; }
#endif

#ifndef REEL_NO_MAIN
int main(int argc, char **argv) { return reel_main(argc, argv); }
#endif
