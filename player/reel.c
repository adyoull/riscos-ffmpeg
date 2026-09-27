/*
 * Reel - a video player for the RISC OS desktop, on FFmpeg (via ffegl).
 *
 * A normal Wimp application: an icon on the icon bar, and one window with
 * the picture above a row of controls (play/pause, back and forward 10 s,
 * a position bar you can click, the time, full screen). Drop a video file
 * on the icon or the window, or double-click one in the Filer while Reel is
 * loaded. Full screen is a window with no furniture that covers the screen.
 *
 * Drawing: each new frame is converted and scaled by swscale (NEON) into a
 * 32bpp sprite of the picture area's size, letterboxed, in the screen's own
 * colour order, and plotted 1:1 with OS_SpriteOp (clipped to the picture
 * area). Sound: SDL2's audio (SharedSoundBuffer), which ffegl uses as the
 * clock. No threads of our own: decoding happens on null events.
 *
 * Keys (window or full screen): Space pause, Left/Right 10 s, Up/Down 1 min,
 * F full screen on/off, Escape leaves full screen, Q closes the video.
 *
 * Part of riscos-ffmpeg. GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "ffegl.h"

/* Big heap in a dynamic area (the default would share the WimpSlot) */
const char *const __dynamic_da_name = "Reel Heap";
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
#define Wimp_RedrawWindow      0x400C8
#define Wimp_UpdateWindow      0x400C9
#define Wimp_GetRectangle      0x400CA
#define Wimp_GetWindowState    0x400CB
#define Wimp_SetIconState      0x400CD
#define Wimp_GetPointerInfo    0x400CF
#define Wimp_ForceRedraw       0x400D1
#define Wimp_SetCaretPosition  0x400D2
#define Wimp_CreateMenu        0x400D4
#define Wimp_ProcessKey        0x400DC
#define Wimp_CloseDown         0x400DD
#define Wimp_ReportError       0x400DF
#define Wimp_SendMessage       0x400E7
#define Wimp_ResizeIcon        0x400FC
#define MimeMap_Translate      0x50B00
#define TaskManager_EnumerateTasks 0x42681

#define MSG_QUIT        0
#define MSG_DATALOAD    3
#define MSG_DATALOADACK 4
#define MSG_DATAOPEN    5
#define MSG_PREQUIT     8
#define MSG_MODECHANGE  0x400C1

#define APP     "Reel"
#define CH      64          /* height of the controls row, OS units */
#define GAP     4
#define MIN_W   880         /* narrowest window: the position bar still has room */
#define W_PLAY  104         /* control widths, OS units */
#define W_SKIP  96
#define W_TIME  176
#define W_FULL  88

/* The position bar's track, in work area x, for the window's width */
static void track_x(int vw, int *x0, int *x1)
{
    *x0 = GAP + W_PLAY + GAP + W_SKIP + GAP + W_SKIP + GAP * 2;
    *x1 = vw - GAP - W_FULL - GAP - W_TIME - GAP;
    if (*x1 < *x0 + 16)
        *x1 = *x0 + 16;
}

enum { I_PLAY, I_BACK, I_FWD, I_TRACK, I_FILL, I_TIME, I_FULL, N_ICONS };

typedef struct { int x0, y0, x1, y1; } box_t;

static struct {
    int task, bar_icon;
    int win, full;                      /* the window; the full screen window (0 = none) */
    int fullscreen;                     /* showing full screen */
    int loop;
    FFEGLVideo *v;
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
    int fill_x1;                        /* current right edge of the position fill */
} S;

/* ---- small helpers ---------------------------------------------------- */

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static void report(const char *text)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 1 | 16;
    r.r[2] = (intptr_t)APP;
    swi(Wimp_ReportError, &r);
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
    S.xeig = mode_var(4);
    S.yeig = mode_var(5);
    S.log2bpp = mode_var(9);
    S.trgb = S.log2bpp == 5 && (mode_var(0) & 0x4000);
    S.scr_w = (mode_var(11) + 1) << S.xeig;
    S.scr_h = (mode_var(12) + 1) << S.yeig;
}

static void vdu_clip(int x0, int y0, int x1, int y1)    /* inclusive, OS units */
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
    if (ffegl_draw_pixels(S.v, sprite_pixels(), S.spr_w * 4, S.spr_w, S.spr_h, S.trgb, 0) == 0)
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
#define IF_COL(fg, bg) (((fg) << 24) | ((bg) << 28))

static char back_text[] = "\x8b 10s", fwd_text[] = "10s \x8a", full_text[] = "Full";
static char empty_text[] = "";

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
    icon_def(&w.icon[I_PLAY], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             S.play_text, "R5,3", sizeof(S.play_text));
    icon_def(&w.icon[I_BACK], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             back_text, "R5,3", sizeof(back_text));
    icon_def(&w.icon[I_FWD], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             fwd_text, "R5,3", sizeof(fwd_text));
    icon_def(&w.icon[I_TRACK], IF_BORDER | IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 0),
             empty_text, "R2", 1);
    icon_def(&w.icon[I_FILL], IF_FILLED | IF_INDIR | IF_TEXT | IF_CLICK | IF_COL(7, 8),
             empty_text, (const char *)-1, 1);
    icon_def(&w.icon[I_TIME], IF_TEXT | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 1),
             S.time_text, (const char *)-1, sizeof(S.time_text));
    icon_def(&w.icon[I_FULL], IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_CLICK | IF_COL(7, 1),
             full_text, "R5,3", sizeof(full_text));
    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    S.win = r.r[0];
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
    resize_icon(I_PLAY, x, y0, x + W_PLAY, y1);  x += W_PLAY + GAP;
    resize_icon(I_BACK, x, y0, x + W_SKIP, y1);  x += W_SKIP + GAP;
    resize_icon(I_FWD,  x, y0, x + W_SKIP, y1);
    track_x(vw, &track_x0, &track_x1);
    resize_icon(I_TRACK, track_x0, y0 + 12, track_x1, y1 - 12);
    S.fill_x1 = track_x0 + 4;
    resize_icon(I_FILL, track_x0 + 4, y0 + 16, S.fill_x1, y1 - 16);
    resize_icon(I_TIME, vw - GAP - W_FULL - GAP - W_TIME, y0, vw - GAP - W_FULL - GAP, y1);
    resize_icon(I_FULL, vw - GAP - W_FULL, y0, vw - GAP, y1);
    if (!S.fullscreen)
        sprite_make(vw >> S.xeig, (vh - CH) >> S.yeig);
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
    snprintf(S.play_text, sizeof(S.play_text), "%s", (ffegl_paused(S.v) || S.ended) ? "Play" : "Pause");
    p = ffegl_position(S.v);
    d = ffegl_duration(S.v);
    format_time(pos, sizeof(pos), p);
    format_time(dur, sizeof(dur), d);
    if (d > 0)
        snprintf(text, sizeof(text), "%s / %s", pos, dur);
    else
        snprintf(text, sizeof(text), "%s", pos);
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
        if (c.x0 < c.x1 && c.y0 < c.y1)
            sprite_plot(ox + pic.x0, oy + pic.y1, &c);
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
}

static void show_frame(void)
{
    int b[11];
    _kernel_swi_regs r;
    int w = S.fullscreen ? S.full : S.win;
    box_t pic = S.fullscreen ? (box_t){ 0, -S.vis_h, S.vis_w, 0 } : S.pic;
    sprite_draw_frame();
    b[0] = w;
    b[1] = pic.x0; b[2] = pic.y0; b[3] = pic.x1; b[4] = pic.y1;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_UpdateWindow, &r))
        return;
    draw_rects(w, b, r.r[0], 1);
}

/* ---- full screen ------------------------------------------------------------ */

static void set_fullscreen(int on)
{
    int b[9];
    _kernel_swi_regs r;
    if (on == S.fullscreen || !S.v)
        return;
    if (on) {
        read_screen();
        if (!S.full && create_full_window() < 0)
            return;
        S.fullscreen = 1;
        S.vis_w = S.scr_w;
        S.vis_h = S.scr_h;
        sprite_make(S.scr_w >> S.xeig, S.scr_h >> S.yeig);
        sprite_draw_frame();
        b[0] = S.full; b[1] = 0; b[2] = 0; b[3] = S.scr_w; b[4] = S.scr_h;
        b[5] = 0; b[6] = 0; b[7] = -1;
        r.r[1] = (intptr_t)b;
        swi(Wimp_OpenWindow, &r);
        set_caret(S.full);
    } else {
        int st[9];
        r.r[1] = (intptr_t)&S.full;
        swi(Wimp_CloseWindow, &r);
        S.fullscreen = 0;
        window_state(S.win, st);
        layout(st[3] - st[1], st[4] - st[2]);
        sprite_draw_frame();
        force_redraw(S.win, 0, -8192, 8192, 0);
        set_caret(S.win);
    }
}

/* ---- opening and closing a file ------------------------------------------------ */

static void close_video(void)
{
    _kernel_swi_regs r;
    if (S.fullscreen)
        set_fullscreen(0);
    if (S.win) {
        r.r[1] = (intptr_t)&S.win;
        swi(Wimp_CloseWindow, &r);
    }
    ffegl_close(S.v);
    S.v = NULL;
    sprite_free();
}

static const char *leaf(const char *path)
{
    /* RISC OS names: after the last '.'; Unix names (UnixLib): after the last '/' */
    const char *p = strrchr(path, path[0] == '/' ? '/' : '.');
    return p ? p + 1 : path;
}

static void play_file(const char *file)
{
    FFEGLVideo *v;
    int vw, vh, w, h, maxw, maxh, st[9];
    int was_open = S.v != NULL;

    v = ffegl_open(file, S.loop ? FFEGL_LOOP : 0);
    if (!v) {
        char msg[300];
        snprintf(msg, sizeof(msg), "%s: %s", leaf(file), ffegl_last_error());
        report(msg);
        return;
    }
    if (S.v) {                          /* replace what was playing, keep the window */
        ffegl_close(S.v);
        S.v = NULL;
    }
    S.v = v;
    S.ended = 0;
    snprintf(S.file, sizeof(S.file), "%s", file);
    snprintf(S.title, sizeof(S.title), "%s", leaf(file));
    read_screen();
    if (!S.win && create_window() < 0) {
        report("Can't create the window.");
        return;
    }
    /* size: the video's own (pixels -> OS units), at most 3/4 of the screen */
    w = ffegl_width(v) << S.xeig;
    h = ffegl_height(v) << S.yeig;
    maxw = S.scr_w * 3 / 4;
    maxh = S.scr_h * 3 / 4 - CH;
    if (w > maxw) { h = (int)((long long)h * maxw / w); w = maxw; }
    if (h > maxh) { w = (int)((long long)w * maxh / h); h = maxh; }
    if (w < MIN_W) w = MIN_W;          /* room for the controls; the picture is letterboxed */
    vw = w;
    vh = h + CH;
    if (S.fullscreen) {                 /* stay full screen; the window follows later */
        sprite_make(S.scr_w >> S.xeig, S.scr_h >> S.yeig);
        force_redraw(S.full, 0, -8192, 8192, 0);
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

/* ---- menus -------------------------------------------------------------------- */

typedef struct { int flags, sub, iflags; char text[12]; } item_t;
typedef struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    item_t item[5];
} menu_t;
static menu_t menu;
static int menu_is_bar;                 /* the open menu: 1 icon bar, 0 window */
static int menu_x, menu_y;

static void menu_item(int i, const char *text, int tick, int last)
{
    menu.item[i].flags = (tick ? 1 : 0) | (last ? 0x80 : 0);
    menu.item[i].sub = -1;
    menu.item[i].iflags = 0x07000021;
    strncpy(menu.item[i].text, text, 12);
}

static void menu_open(int bar, int x, int y)
{
    _kernel_swi_regs r;
    int n;
    memset(&menu, 0, sizeof(menu));
    strcpy(menu.title, APP);
    menu.tfg = 7; menu.tbg = 2; menu.wfg = 7; menu.wbg = 0;
    menu.width = 12 * 16 + 32;
    menu.height = 44;
    if (bar) {
        menu_item(0, "Info", 0, 0);
        menu_item(1, "Loop", S.loop, 0);
        menu_item(2, "Quit", 0, 1);
        n = 3;
        y = 96 + n * 44;
        x -= 64;
    } else {
        menu_item(0, "File info", 0, 0);
        menu_item(1, "Full screen", S.fullscreen, 0);
        menu_item(2, "Loop", S.loop, 0);
        menu_item(3, "Close", 0, 1);
        n = 4;
        x -= 64;
    }
    menu_is_bar = bar;
    menu_x = x; menu_y = y;
    r.r[1] = (intptr_t)&menu;
    r.r[2] = x;
    r.r[3] = y;
    swi(Wimp_CreateMenu, &r);
}

static void quit(void)
{
    _kernel_swi_regs r;
    close_video();
    r.r[0] = S.task;
    swi(Wimp_CloseDown, &r);
    exit(0);
}

static void show_info(void)
{
    char info[256], msg[400], dur[16];
    if (!S.v) {
        report("Reel plays videos: drop one on its icon bar icon, or double-click it in the Filer "
               "while Reel is loaded. riscos-ffmpeg (FFmpeg 5.1), GPL v2 or later.");
        return;
    }
    ffegl_info(S.v, info, sizeof(info));
    format_time(dur, sizeof(dur), ffegl_duration(S.v));
    snprintf(msg, sizeof(msg), "%s: %s; %s long; %u late frames skipped so far.",
             leaf(S.file), info, dur, ffegl_dropped_frames(S.v));
    report(msg);
}

static void menu_select(const int *sel)
{
    int b[5];
    _kernel_swi_regs r;
    if (menu_is_bar) {
        switch (sel[0]) {
        case 0: show_info(); break;
        case 1: S.loop = !S.loop; break;
        case 2: quit();
        }
    } else {
        switch (sel[0]) {
        case 0: show_info(); break;
        case 1: set_fullscreen(!S.fullscreen); break;
        case 2: S.loop = !S.loop; break;        /* applies from the next file */
        case 3: close_video(); return;
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
        ffegl_seek(S.v, 0);
        ffegl_pause(S.v, 0);
    } else
        ffegl_pause(S.v, !ffegl_paused(S.v));
    update_controls(1);
}

static void seek_by(double d)
{
    double p, dur;
    if (!S.v)
        return;
    p = ffegl_position(S.v) + d;
    dur = ffegl_duration(S.v);
    if (p < 0) p = 0;
    if (dur > 0 && p > dur - 1) p = dur - 1;
    S.ended = 0;
    ffegl_seek(S.v, p);
    update_controls(1);
}

static void click_track(int mouse_x)
{
    int b[11], x0, x1, wx;
    double d;
    if (!S.v || (d = ffegl_duration(S.v)) <= 0)
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
    ffegl_seek(S.v, d * (wx - x0) / (x1 - x0));
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
    r2 = ffegl_update(S.v);
    if (r2 == FFEGL_NEW_FRAME)
        show_frame();
    else if (r2 == FFEGL_END) {
        S.ended = 1;
        update_controls(1);
        return;
    }
    swi(OS_ReadMonotonicTime, &r);
    t = r.r[0];
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
            play_file(file);
        }
        break;
    case MSG_DATAOPEN:
        if (is_video_type(b[10])) {
            snprintf(file, sizeof(file), "%s", (const char *)&b[11]);
            ack(b);                     /* claims it */
            play_file(file);
        }
        break;
    case MSG_MODECHANGE:
        read_screen();
        if (S.fullscreen) {
            set_fullscreen(0);
        } else if (S.v && S.win) {
            int st[9];
            window_state(S.win, st);
            layout(st[3] - st[1], st[4] - st[2]);
            sprite_draw_frame();
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
                if (!strncmp(name, APP, 4) && (unsigned char)name[4] < 32)
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
    strcpy(b.name, "!reel");
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
    read_screen();
    iconbar_icon();
    if (argc > 1)
        play_file(argv[1]);

    for (;;) {
        int playing = S.v && !S.ended && !ffegl_paused(S.v);
        r.r[0] = (playing ? 0 : 1) | (1 << 4) | (1 << 5);
        r.r[1] = (intptr_t)block;
        if (swi(Wimp_Poll, &r))
            continue;
        switch (r.r[0]) {
        case 0:  tick(); break;                            /* null */
        case 1:  redraw(block); break;
        case 2:                                            /* Open_Window_Request */
            if (block[0] == S.win) {
                layout(block[3] - block[1], block[4] - block[2]);
                sprite_draw_frame();
            }
            r.r[1] = (intptr_t)block;
            swi(Wimp_OpenWindow, &r);
            if (block[0] == S.win)
                force_redraw(S.win, 0, -8192, 8192, 0);
            break;
        case 3:                                            /* Close_Window_Request */
            if (block[0] == S.win)
                close_video();
            else if (block[0] == S.full)
                set_fullscreen(0);
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
            switch (block[4]) {
            case I_PLAY: toggle_pause(); break;
            case I_BACK: seek_by(-10); break;
            case I_FWD:  seek_by(10); break;
            case I_TRACK: case I_FILL: click_track(block[0]); break;
            case I_FULL: set_fullscreen(1); break;
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
FFEGLVideo *reel_test_video(void) { return S.v; }
const uint8_t *reel_test_sprite(int *w, int *h, int *rows)
{
    *w = S.spr_w; *h = S.spr_h; *rows = S.spr_rows;
    return S.area ? sprite_pixels() : NULL;
}
int reel_test_fullscreen(void) { return S.fullscreen; }
#endif

#ifndef REEL_NO_MAIN
int main(int argc, char **argv) { return reel_main(argc, argv); }
#endif
