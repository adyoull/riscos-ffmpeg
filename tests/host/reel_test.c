/*
 * Host test of Reel (player/reel.c) with a scripted fake Wimp and the real
 * decoding (ffegl + FFmpeg), fake SDL audio and a fake clock. Built for
 * arm-linux and run under the trapping qemu by tests/host/run.sh.
 *
 *   reel_test CLIP_WITH_SOUND OTHER_CLIP
 *
 * The script: a file dropped on the icon bar icon opens the window at the
 * video's size and plays it (frames reach the sprite and are plotted,
 * clipped to the picture); the controls pause, seek (buttons, keys, the
 * position bar); full screen and back; a resize remakes the sprite; a second
 * file dropped on the window replaces the first; a directory is refused; a
 * double-clicked video (DataOpen) is claimed, other types aren't; closing
 * the window closes the video; Message_Quit quits. The playback options:
 * the volume bar (and the Choices file), speed, picture size, fast
 * decoding, A-B repeat, vsync; a playlist of two files and "carry on from
 * where it was stopped"; the mini player (above the icon bar, keep on top,
 * dragged somewhere else and remembered, double-click for the window).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "kernel.h"
#include "reelcore.h"
#include "fake_sdl_gl.h"

int reel_main(int argc, char **argv);
ReelCore *reel_test_video(void);
#ifdef REEL_EGL
/* ReelEGL: the picture is a (fake) EGL surface; what was last shown is
   fake_shown, from eglSwapBuffers */
#include "fake_riscos.h"
#include <EGL/egl.h>
int reel_test_surface(int *w, int *h, int *full);
static const uint8_t *reel_test_sprite(int *w, int *h, int *rows)
{
    int full;
    if (!reel_test_surface(w, h, &full)) return NULL;
    *rows = *h;
    return fake_shown;
}
#define PLOTS (plots + fake_swaps + fake_plots)
#else
const uint8_t *reel_test_sprite(int *w, int *h, int *rows);
#define PLOTS plots
#endif
int reel_test_fullscreen(void);
int reel_test_pic_flags(void);
int reel_test_ab(double *a, double *b);
int reel_test_list(int *n);
int reel_test_mini(int *ontop);

static const char *clip1, *clip2;
static int fails, step;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define WIN 0x100
#define FULL 0x200
#define INFO 0x300                    /* the media info window */
#define MINI 0x400                    /* the mini player */
#define SCR_W 1920                    /* pixels; eig 1 -> 3840 x 2160 OS units */
#define SCR_H 1080

static int created, opened_w, opened_h, win_x0, win_y1, full_open, win_open, nicons;
static int state[4][9];               /* window states: [0] WIN, [1] FULL, [2] INFO, [3] MINI */
static int info_created, info_open, info_updates, info_texts;
static char info_seen[8192];          /* what Wimp_TextOp drew in the info window */
static int drawing_info;
static int plots, plots_full, clip_ok = 1, updates, acks, reports, keys_passed, last_mask;
static int caret_win;
static int icon_box[10][4], mini_box[10][4];
static int mini_created, mini_open, mini_nicons, mini_tops, main_w, main_h;
static int vsyncs, asks;
static double ab_lo, ab_hi;
static char choices_dir[64];

/* the window menu (reel.c's WM_*) */
enum { M_INFO, M_FULL, M_MINI, M_ONTOP, M_PIC, M_SPEED, M_TRACK, M_LIST, M_AB, M_LOOP, M_FAST, M_VSYNC, M_CLOSE };
static char title[64];
static int *title_ptr;

static int *st(int w) { return state[w == FULL ? 1 : w == INFO ? 2 : w == MINI ? 3 : 0]; }

/* Choices: what Reel saved */
static int choices_has(const char *want)
{
    char path[128], buf[512];
    size_t n;
    FILE *f;
    snprintf(path, sizeof(path), "%s/Choices", choices_dir);
    if (!(f = fopen(path, "r")))
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, want) != NULL;
}

static void message(int *b, int action, int window, int icon, int type, const char *name)
{
    memset(b, 0, 256);
    b[0] = 256; b[1] = 0x777; b[2] = 99; b[4] = action;
    b[5] = window; b[6] = icon; b[10] = type;
    strcpy((char *)&b[11], name);
}

static void click(int *b, int x, int y, int buttons, int window, int icon)
{
    b[0] = x; b[1] = y; b[2] = buttons; b[3] = window; b[4] = icon;
}

/* screen x of work area x in the window */
static int sx(int w, int wx) { return st(w)[1] - st(w)[5] + wx; }

static int nulls;                     /* null events left to deliver in this phase */
static int idle_polls;
static int phase;
static int next_is_null(void);

static int script(int *b)
{
    /* each case returns the event code, having filled b */
    switch (step++) {
    case 0:  message(b, 3, -2, 1, 0xBF8, clip1); return 18;           /* drop on the icon bar */
    case 1:  nulls = 60; /* fallthrough */
    default: break;
    }
    return -1;
}

/* the phases after the first drop */
enum { P_PLAY1, P_VOLUME, P_SPEED, P_SPEEDPLAY, P_SPEEDBACK, P_PICFILL, P_PICFIT, P_FAST, P_AB, P_ABPLAY, P_ABOFF,
       P_INFO, P_INFOPLAY, P_INFOCLOSE, P_PAUSE, P_PAUSED, P_RESUME, P_PLAY2, P_SEEKBAR, P_PLAY3, P_FULL, P_PLAYFULL,
       P_VSYNCOFF, P_UNFULL, P_RESIZE, P_PLAY4, P_DROP2, P_PLAY5, P_LIST, P_MINI, P_MINIPLAY, P_ONTOP, P_MINIMOVE,
       P_MINIBACK, P_DIR, P_OPEN_OTHER, P_OPEN_VIDEO, P_PLAY6, P_CLOSE, P_QUIT };
static int phase = -1, phase_step;
/* the phases that play (deliver nulls) */
static int next_is_null(void)
{
    return phase == P_PLAY1 || phase == P_PLAY2 || phase == P_PLAY3 || phase == P_PLAYFULL || phase == P_PLAY4 ||
           phase == P_PLAY5 || phase == P_PLAY6 || phase == P_INFOPLAY || phase == P_SPEEDPLAY || phase == P_AB ||
           phase == P_ABPLAY || phase == P_MINIPLAY || phase == P_ONTOP;
}

/* The picture (sprite or surface) holds the current frame as reelcore
   draws it with the picture size chosen */
static void check_picture(const char *what)
{
    int w, h, rows, bad = 0;
    const uint8_t *px = reel_test_sprite(&w, &h, &rows);
    uint8_t *want = malloc((size_t)w * h * 4);
    reelcore_draw_pixels(reel_test_video(), want, w * 4, w, h, 0, reel_test_pic_flags());
    for (int i = 0; px && i < w * h; i++)
        bad += memcmp(px + i * 4, want + i * 4, 3) != 0;
    CHECK(px && !bad, "%s: picture %dx%d, %d pixels differ", what, w, h, bad);
    free(want);
}

static void key_event(int *b, int w, int k) { memset(b, 0, 28); b[0] = w; b[6] = k; }

/* Opens the window menu on window w (steps 0, 1 of a phase), then chooses
   item a (submenu item s, or -1) */
#define MENU_PICK(w, a, s) do { \
        if (phase_step == 0) { phase_step++; click(b, sx(w, 100), st(w)[2] + 100, 2, w, -1); return 6; } \
        if (phase_step == 1) { phase_step++; b[0] = (a); b[1] = (s); b[2] = -1; return 9; } \
    } while (0)

static void pump(ReelCore *v, int n) { for (int i = 0; i < n; i++) { reelcore_update(v); fake_time += 0.01; } }
static double pos_before;

static int next_event(int *b)
{
    int ev;
    if (phase < 0) {
        ev = script(b);
        if (ev >= 0)
            return ev;
        phase = P_PLAY1;
        phase_step = 0;
    }
    for (;;) {
        ReelCore *v = reel_test_video();
        switch (phase) {
        case P_PLAY1: case P_PLAY2: case P_PLAY3: case P_PLAYFULL: case P_PLAY4: case P_PLAY5: case P_PLAY6:
        case P_MINIPLAY:
            if (phase_step++ < 40) {
                fake_time += 0.02;
                return 0;                                             /* null */
            }
            {   /* the picture holds the current frame, letterboxed, as reelcore draws it */
                char what[32];
                snprintf(what, sizeof(what), "phase %d", phase);
                check_picture(what);
            }
            break;
        case P_VOLUME:                                                /* a quarter of the way along the bar */
            if (phase_step++ == 0) {
                int x = icon_box[7][0] + 4 + (icon_box[7][2] - icon_box[7][0] - 8) / 4;
                click(b, sx(WIN, x), st(WIN)[2] + 30, 4, WIN, 7);
                return 6;
            }
            CHECK(fabs(reelcore_volume(v) - 0.0625) < 0.01, "volume %.3f (want a quarter, squared)", reelcore_volume(v));
            CHECK(icon_box[8][2] - icon_box[7][0] == 4 + (icon_box[7][2] - icon_box[7][0] - 8) / 4,
                  "volume fill to %d (bar %d-%d)", icon_box[8][2], icon_box[7][0], icon_box[7][2]);
            CHECK(choices_has("volume 0.250"), "the volume wasn't saved in Choices");
            break;
        case P_SPEED:
            MENU_PICK(WIN, M_SPEED, 5);                               /* 2x */
            CHECK(reelcore_speed(v) == 2.0, "speed %.2f (want 2)", reelcore_speed(v));
            break;
        case P_SPEEDPLAY:                                             /* 0.8 s at 2x: 1.6 s of the file */
            if (phase_step == 0) pos_before = reelcore_position(v);
            if (phase_step++ < 40) { fake_time += 0.02; return 0; }
            {
                double moved = reelcore_position(v) - pos_before;
                CHECK(moved > 1.2 && moved < 2.0, "2x: %.2f s of the file in 0.8 s", moved);
            }
            break;
        case P_SPEEDBACK:
            MENU_PICK(WIN, M_SPEED, 2);                               /* Normal */
            CHECK(reelcore_speed(v) == 1.0, "speed %.2f (want 1)", reelcore_speed(v));
            break;
        case P_PICFILL:
            MENU_PICK(WIN, M_PIC, 1);                                 /* Fill (crop) */
            CHECK(reel_test_pic_flags() == REELCORE_FILL, "picture flags %d (want fill)", reel_test_pic_flags());
            check_picture("fill");
            break;
        case P_PICFIT:
            MENU_PICK(WIN, M_PIC, 0);                                 /* Fit */
            CHECK(reel_test_pic_flags() == 0, "picture flags %d (want fit)", reel_test_pic_flags());
            check_picture("fit");
            break;
        case P_FAST:                                                  /* on, then off again */
            MENU_PICK(WIN, M_FAST, -1);
            if (phase_step == 2) {
                CHECK(reelcore_fast(v) == 1, "fast decode not on");
                phase_step++;
                click(b, sx(WIN, 100), st(WIN)[2] + 100, 2, WIN, -1);
                return 6;
            }
            if (phase_step == 3) { phase_step++; b[0] = M_FAST; b[1] = -1; return 9; }
            CHECK(reelcore_fast(v) == 0, "fast decode not off");
            break;
        case P_AB:                                                    /* A, 0.4 s, B */
            if (phase_step == 0) { phase_step++; key_event(b, WIN, 'a'); return 8; }
            if (phase_step <= 20) { phase_step++; fake_time += 0.02; return 0; }
            if (phase_step == 21) { phase_step++; key_event(b, WIN, 'a'); return 8; }
            {
                double a, bb;
                int ab = reel_test_ab(&a, &bb);
                CHECK(ab == 2 && bb - a > 0.3 && bb - a < 0.5, "A-B: state %d, %.2f to %.2f", ab, a, bb);
            }
            break;
        case P_ABPLAY: {                                              /* 1.2 s: round the 0.4 s more than once */
            double a, bb, p = reelcore_position(v);
            reel_test_ab(&a, &bb);
            if (phase_step == 0) { ab_lo = 1e9; ab_hi = -1; }
            if (phase_step > 0) {
                if (p < ab_lo) ab_lo = p;
                if (p > ab_hi) ab_hi = p;
            }
            if (phase_step++ < 60) { fake_time += 0.02; return 0; }
            CHECK(ab_lo > a - 0.15 && ab_hi < bb + 0.15 && ab_hi > a + 0.2,
                  "A-B repeat: played %.2f to %.2f (A %.2f, B %.2f)", ab_lo, ab_hi, a, bb);
            break;
        }
        case P_ABOFF:
            if (phase_step++ == 0) { key_event(b, WIN, 'a'); return 8; }
            {
                double a, bb;
                CHECK(reel_test_ab(&a, &bb) == 0, "A-B repeat not off");
            }
            break;
        case P_INFO:                                                  /* I: the media info window */
            if (phase_step == 0) { phase_step++; memset(b, 0, 28); b[0] = WIN; b[6] = 'i'; return 8; }
            if (phase_step++ == 1) { info_seen[0] = 0; memset(b, 0, 44); b[0] = INFO; return 1; }   /* its redraw */
            CHECK(info_created == 1 && info_open, "I: info window %d made, open %d", info_created, info_open);
            CHECK(strstr(info_seen, "h264") && strstr(info_seen, "Sample rate") && strstr(info_seen, "Stats for nerds"),
                  "info window text: %.300s", info_seen);
            break;
        case P_INFOPLAY:                                              /* its stats, each second */
            if (phase_step == 0) info_updates = 0;
            if (phase_step++ < 70) { fake_time += 0.02; if (phase_step == 1) info_seen[0] = 0; return 0; }
            CHECK(info_updates >= 1, "stats not updated (%d)", info_updates);
            /* (decoding takes no fake time, so there's no "ms each" here) */
            {
                const char *ps = strstr(info_seen, "Pictures shown|");
                double fps = ps ? atof(ps + 15) : 0;
                CHECK(fps >= 23.5 && fps <= 25.5, "pictures shown %.1f a second (want about 25)", fps);
            }
            CHECK((strstr(info_seen, "SharedSoundBuffer: ") || strstr(info_seen, "SDL: ")) &&
                  strstr(info_seen, "pictures,") && strstr(info_seen, "Mbit/s"),
                  "stats text: %.400s", info_seen);
            printf("  info: %s\n", strstr(info_seen, "Stats") ? strstr(info_seen, "Stats") : info_seen);
            break;
        case P_INFOCLOSE:
            if (phase_step++ == 0) { memset(b, 0, 4); b[0] = INFO; return 3; }
            CHECK(!info_open, "info window didn't close");
            break;
        case P_PAUSE:
            if (phase_step++ == 0) {
                /* the Play/Pause button: first icon */
                click(b, sx(WIN, (icon_box[0][0] + icon_box[0][2]) / 2), st(WIN)[2] + 20, 4, WIN, 0);
                return 6;
            }
            break;
        case P_PAUSED:
            if (phase_step == 0) {
                CHECK(v && reelcore_paused(v), "pause button didn't pause");
                CHECK(last_mask & 1, "null events still enabled while paused");
                pos_before = v ? reelcore_position(v) : 0;
            }
            if (phase_step++ < 3) { fake_time += 0.5; memset(b, 0, 44); b[0] = WIN; return 1; }   /* redraws */
            CHECK(v && reelcore_position(v) == pos_before, "moved while paused");
            break;
        case P_RESUME:
            if (phase_step++ == 0) { memset(b, 0, 28); b[0] = WIN; b[6] = ' '; return 8; }   /* Space */
            CHECK(v && !reelcore_paused(v), "Space didn't resume");
            break;
        case P_SEEKBAR:
            if (phase_step++ == 0) {
                /* the middle of the position bar (icon 3) */
                int x0 = icon_box[3][0], x1 = icon_box[3][2];
                CHECK(x1 - x0 > 250, "position bar only %d wide", x1 - x0);
                click(b, sx(WIN, (x0 + x1) / 2), st(WIN)[2] + 30, 4, WIN, 3);
                return 6;
            }
            {
                double d = reelcore_duration(v), p;
                for (int i = 0; i < 30; i++) { reelcore_update(v); fake_time += 0.01; }
                p = reelcore_position(v);
                CHECK(p > d * 0.35 && p < d * 0.7, "position bar: %.2f of %.2f", p, d);
            }
            break;
        case P_FULL:
            if (phase_step++ == 0) { memset(b, 0, 28); b[0] = WIN; b[6] = 'f'; return 8; }
            {
                int w, h, rows;
                reel_test_sprite(&w, &h, &rows);
                CHECK(reel_test_fullscreen() && full_open, "F didn't go full screen");
                CHECK(w == SCR_W && h == SCR_H, "full screen sprite %dx%d", w, h);
#ifdef REEL_EGL
                CHECK(fake_render_buffer != EGL_SINGLE_BUFFER && fake_surfaces == 1,
                      "full screen: render buffer 0x%x (want back: vsync), %d surfaces", fake_render_buffer, fake_surfaces);
#endif
                CHECK(state[1][3] - state[1][1] == SCR_W * 2 && state[1][4] - state[1][2] == SCR_H * 2,
                      "full screen window %dx%d", state[1][3] - state[1][1], state[1][4] - state[1][2]);
                CHECK(caret_win == FULL, "no caret in the full screen window");
                plots_full = PLOTS;
            }
            break;
        case P_VSYNCOFF:                                              /* Vsync off: Direct */
#ifndef REEL_EGL
            if (phase_step == 0)
                CHECK(vsyncs > 0, "full screen didn't wait for the vsync");
#endif
            MENU_PICK(FULL, M_VSYNC, -1);
#ifdef REEL_EGL
            CHECK(fake_render_buffer == EGL_SINGLE_BUFFER && fake_surfaces == 1,
                  "vsync off: render buffer 0x%x (want single: Direct), %d surfaces", fake_render_buffer, fake_surfaces);
#endif
            break;
        case P_UNFULL:
            if (phase_step++ == 0) {
                CHECK(PLOTS > plots_full, "nothing plotted full screen");
                memset(b, 0, 28); b[0] = FULL; b[6] = 27; return 8;       /* Escape */
            }
            CHECK(!reel_test_fullscreen() && !full_open, "Escape didn't leave full screen");
#ifdef REEL_EGL
            CHECK(fake_surfaces == 1 && fake_wa[0] == 0 && fake_wa[1] == 0 && fake_wa[2] > 0,
                  "back in the window: %d surfaces, work area %d,%d %dx%d", fake_surfaces, fake_wa[0], fake_wa[1], fake_wa[2], fake_wa[3]);
#endif
            break;
        case P_RESIZE:
            if (phase_step++ == 0) {
                memcpy(b, st(WIN), 36);
                b[3] = b[1] + 1000;                                   /* 1000 x 700 OS units */
                b[2] = b[4] - 700;
                return 2;                                             /* Open_Window_Request */
            }
            {
                int w, h, rows;
                reel_test_sprite(&w, &h, &rows);
                CHECK(w == 500 && h == (700 - 64) / 2, "sprite after resize %dx%d (want 500x%d)", w, h, (700 - 64) / 2);
#ifndef REEL_EGL
                CHECK(rows * w * 4 >= 1024 * 1024, "sprite not padded to 1 MB (%d rows)", rows);
#else
                CHECK(fake_wa[2] == 500 && fake_wa[3] == 318 && fake_surfaces == 1, "work area surface %dx%d", fake_wa[2], fake_wa[3]);
#endif
            }
            break;
        case P_DROP2:                                                 /* from the middle: remembered */
            if (phase_step == 0) {
                phase_step++;
                click(b, sx(WIN, (icon_box[3][0] + icon_box[3][2]) / 2), st(WIN)[2] + 30, 4, WIN, 3);
                return 6;
            }
            if (phase_step++ == 1) { pump(v, 30); message(b, 3, WIN, -1, 0xFFD, clip2); return 18; }
            {
                const char *leaf = strrchr(clip2, '/');
                CHECK(title_ptr && !strcmp((char *)title_ptr, leaf ? leaf + 1 : clip2), "title '%s'", title_ptr ? (char *)title_ptr : "");
                CHECK(v && reelcore_width(v) > 0, "second file not playing");
            }
            break;
        case P_LIST: {                                                /* two files in one drag, then N */
            static int asks_before;
            if (phase_step == 0) { phase_step++; message(b, 3, WIN, -1, 0xFFD, clip2); return 18; }
            if (phase_step == 1) { phase_step++; message(b, 3, WIN, -1, 0xFFD, clip1); return 18; }
            if (phase_step == 2) { phase_step++; asks_before = asks; key_event(b, WIN, 'n'); return 8; }
            {
                int n, i = reel_test_list(&n);
                double p;
                pump(v, 30);
                p = reelcore_position(v);
                CHECK(n == 2 && i == 1, "playlist: %d of %d (want 2 of 2)", i + 1, n);
                CHECK(title_ptr && strstr((char *)title_ptr, "(2/2)"), "title '%s'", title_ptr ? (char *)title_ptr : "");
                CHECK(asks == asks_before + 1 && p > 2.5, "carry on: %d questions, at %.2f", asks - asks_before, p);
            }
            break;
        }
        case P_MINI:                                                  /* M: the mini player */
            if (phase_step == 0) {
                phase_step++;
                main_w = st(WIN)[3] - st(WIN)[1];
                main_h = st(WIN)[4] - st(WIN)[2];
                key_event(b, WIN, 'm');
                return 8;
            }
            if (phase_step++ == 1) {
                int *m = st(MINI), vw = m[3] - m[1], vh = m[4] - m[2], w, h, rows, on;
                reel_test_sprite(&w, &h, &rows);
                CHECK(reel_test_mini(&on) && mini_created == 1 && mini_open && !win_open && mini_nicons == 10,
                      "mini player: made %d, open %d, window open %d, %d icons", mini_created, mini_open, win_open, mini_nicons);
                CHECK(vw == 640 && vh == 364 + 64, "mini player %dx%d (want 640x428)", vw, vh);
                CHECK(m[3] == SCR_W * 2 - 32 && m[2] == 134 + 16, "mini player at %d,%d-%d,%d (want the bottom right, above the icon bar)",
                      m[1], m[2], m[3], m[4]);
                CHECK(w == 320 && h == 182, "mini player picture %dx%d", w, h);
#ifdef REEL_EGL
                CHECK(fake_surfaces == 1 && fake_wa[2] == 320 && fake_wa[3] == 182, "mini player surface %dx%d, %d surfaces",
                      fake_wa[2], fake_wa[3], fake_surfaces);
#endif
                CHECK(mini_box[9][2] == 640 - 4 && mini_box[5][2] < 0 && mini_box[3][2] < mini_box[9][0],
                      "mini player controls: Normal to %d, time to %d, bar to %d", mini_box[9][2], mini_box[5][2], mini_box[3][2]);
                key_event(b, MINI, 0x18C);                            /* Left: back to the start, time to play */
                return 8;
            }
            break;
        case P_ONTOP:                                                 /* Keep on top, then another window over it */
            MENU_PICK(MINI, M_ONTOP, -1);
            if (phase_step == 2) {
                int on;
                reel_test_mini(&on);
                CHECK(on && choices_has("keep_on_top 1"), "keep on top: %d, saved %d", on, choices_has("keep_on_top 1"));
                st(MINI)[7] = WIN;                                    /* covered */
                mini_tops = 0;
            }
            if (phase_step++ < 62) { fake_time += 0.02; return 0; }
            CHECK(st(MINI)[7] == -1 && mini_tops >= 1, "keep on top: behind %d, brought to the front %d times", st(MINI)[7], mini_tops);
            break;
        case P_MINIMOVE:                                              /* dragged; off and on again: it stays there */
            if (phase_step == 0) {
                int w = st(MINI)[3] - st(MINI)[1], h = st(MINI)[4] - st(MINI)[2];
                phase_step++;
                memcpy(b, st(MINI), 36);
                b[1] = 200; b[3] = 200 + w; b[2] = 600; b[4] = 600 + h;
                return 2;                                             /* Open_Window_Request, as the drag does */
            }
            if (phase_step == 1) { phase_step++; key_event(b, MINI, 'm'); return 8; }
            if (phase_step == 2) {
                CHECK(win_open && !mini_open, "M didn't go back to the window");
                phase_step++;
                key_event(b, WIN, 'm');
                return 8;
            }
            {
                char want[40];
                snprintf(want, sizeof(want), "mini_right %d", SCR_W * 2 - 840);
                CHECK(st(MINI)[1] == 200 && st(MINI)[2] == 600, "mini player back at %d,%d (want 200,600)", st(MINI)[1], st(MINI)[2]);
                CHECK(choices_has(want) && choices_has("mini_bottom 600"), "the mini player's place wasn't saved");
            }
            break;
        case P_MINIBACK:                                              /* double-click: the window as it was */
            if (phase_step++ == 0) { click(b, sx(MINI, 100), st(MINI)[2] + 200, 4, MINI, -1); return 6; }
            {
                int on;
                CHECK(!reel_test_mini(&on) && win_open && !mini_open, "double-click: mini %d, window %d", mini_open, win_open);
                CHECK(opened_w == main_w && opened_h == main_h, "window back at %dx%d (was %dx%d)", opened_w, opened_h, main_w, main_h);
#ifdef REEL_EGL
                CHECK(fake_surfaces == 1 && fake_wa[2] == main_w / 2, "window surface %dx%d", fake_wa[2], fake_wa[3]);
#endif
                check_picture("back from the mini player");
            }
            break;
        case P_DIR:
            if (phase_step++ == 0) { message(b, 3, -2, 1, 0x1000, "/tmp"); return 18; }
            CHECK(reports == 1, "directory: %d reports", reports);
            break;
        case P_OPEN_OTHER:
            if (phase_step++ == 0) { message(b, 5, 0, 0, 0xFFF, "/tmp/text"); return 17; }   /* DataOpen, text */
            CHECK(acks == 4, "text file claimed (%d acks)", acks);
            break;
        case P_OPEN_VIDEO:
            if (phase_step++ == 0) { message(b, 5, 0, 0, 0xBF8, clip1); return 17; }    /* DataOpen, MPEG */
            CHECK(acks == 5, "video not claimed (%d acks)", acks);
            break;
        case P_CLOSE:
            if (phase_step++ == 0) { memset(b, 0, 4); b[0] = WIN; return 3; }
            CHECK(!reel_test_video() && !win_open, "close didn't close");
#ifdef REEL_EGL
            CHECK(fake_surfaces == 0, "close left %d EGL surfaces", fake_surfaces);
#endif
            break;
        case P_QUIT:
            memset(b, 0, 24); b[4] = 0;
            return 17;                                                /* Message_Quit: exits */
        }
        phase++;
        phase_step = 0;
    }
}

#ifdef FAKE_SSB
static int ssb_open, ssb_opens;
static unsigned ssb_played_total;
static int ssb_rate;
#endif
static void final_checks(void)
{
    int w, h, rows;
    (void)w; (void)h; (void)rows;
    CHECK(created == 1 && nicons == 9, "window: created %d, %d icons", created, nicons);
    CHECK(asks >= 1, "never asked to carry on");
    CHECK(idle_polls > 20, "Wimp_PollIdle used only %d times while playing", idle_polls);
    printf("  %d Wimp_PollIdle calls\n", idle_polls);
#ifdef FAKE_SSB
    CHECK(ssb_played_total > (unsigned)ssb_rate * 4, "SharedSoundBuffer played only %u bytes", ssb_played_total);
    CHECK(!ssb_open, "the sound stream was left open");
    printf("  SharedSoundBuffer: %d streams, %.1f s played\n", ssb_opens, ssb_rate ? ssb_played_total / (ssb_rate * 4.0) : 0);
#endif
#ifdef REEL_EGL
    CHECK(fake_swaps > 20 && fake_plots > 0, "frames: %d swaps, %d redraw plots", fake_swaps, fake_plots);
    printf("  %d swaps, %d redraw plots, window %dx%d, %d acks\n", fake_swaps, fake_plots, opened_w, opened_h, acks);
#else
    CHECK(updates > 20 && plots > 20, "frames: %d updates, %d plots", updates, plots);
    CHECK(clip_ok, "a plot wasn't clipped to the picture");
    printf("  %d updates, %d plots, window %dx%d, %d acks\n", updates, plots, opened_w, opened_h, acks);
#endif
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    fflush(stdout);
    _exit(fails ? 1 : 0);
}

static int vis_pic_y0(int w) { return w == FULL ? st(w)[2] : st(w)[2] + 64; }

static int pending_rect;

#ifdef FAKE_SSB
/* SharedSoundBuffer + StreamManager: plays what was added at the stream's
   rate while not paused, going by fake_time. */
static int ssb_playing;
static unsigned ssb_added, ssb_played, ssb_limit;
static double ssb_last;
static void ssb_advance(void)
{
    if (ssb_open && ssb_playing && fake_time > ssb_last) {
        unsigned n = (unsigned)((fake_time - ssb_last) * ssb_rate * 4) & ~3u;
        if (n > ssb_added - ssb_played) n = ssb_added - ssb_played;
        ssb_played += n;
        ssb_played_total += n;
    }
    ssb_last = fake_time;
}
static int fake_ssb(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out, _kernel_oserror **e)
{
    static _kernel_oserror full = { 2, "full" }, unknown = { 3, "No such SWI" };
    ssb_advance();
    switch (swi) {
    case 0x39:                                                    /* OS_SWINumberFromString */
        *e = strncmp((char *)(intptr_t)in->r[1], "SharedSoundBuffer_", 18) &&
             strncmp((char *)(intptr_t)in->r[1], "StreamManager_", 14) ? &unknown : NULL;
        return 1;
    case 0x55FC0: ssb_open = 1; ssb_opens++; ssb_playing = 1; ssb_added = ssb_played = 0;
                  out->r[0] = 7; *e = NULL; return 1;             /* OpenStream (plays until paused) */
    case 0x55FCE: out->r[0] = 9; *e = NULL; return 1;             /* ReturnStreamHandle */
    case 0x57287: ssb_limit = in->r[1]; *e = NULL; return 1;      /* SetBuffer */
    case 0x55FC5: ssb_rate = in->r[1] / 1024; *e = NULL; return 1;/* SampleRate */
    case 0x55FC4: *e = NULL; return 1;                            /* Volume */
    case 0x55FC9: ssb_playing = in->r[1] & 1; *e = NULL; return 1;/* Pause */
    case 0x55FC1: ssb_open = 0; *e = NULL; return 1;              /* CloseStream */
    case 0x57282:                                                 /* AddBlock */
        if (ssb_added - ssb_played + in->r[2] > ssb_limit) { *e = &full; return 1; }
        ssb_added += in->r[2]; *e = NULL; return 1;
    case 0x57288: out->r[0] = ssb_added; out->r[1] = ssb_played; *e = NULL; return 1;   /* BufferStats */
    }
    return 0;
}
#endif

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake" };
    int *b = (int *)(intptr_t)in->r[1];
#ifdef FAKE_SSB
    {
        _kernel_oserror *e;
        if (fake_ssb(swi, in, out, &e))
            return e;
    }
#endif
    switch (swi) {
    case 0x400C0: out->r[1] = 0x1234; return NULL;                        /* Wimp_Initialise */
    case 0x42681: out->r[0] = -1; return NULL;                            /* EnumerateTasks */
    case 0x35:                                                            /* OS_ReadModeVariable */
        out->r[2] = in->r[1] == 4 || in->r[1] == 5 ? 1 : in->r[1] == 9 ? 5 : in->r[1] == 11 ? SCR_W - 1 :
                    in->r[1] == 12 ? SCR_H - 1 : 0;
        return NULL;
    case 0x400C2: out->r[0] = 1; return NULL;                             /* CreateIcon */
    case 0x400C1:                                                         /* CreateWindow */
        if (b[7] == (int)0x80000002) { mini_created++; mini_nicons = b[21]; out->r[0] = MINI; return NULL; }
        if ((b[7] & 0x80000040) == 0x80000040 && !(b[7] & 0x04000000)) { out->r[0] = FULL; return NULL; }
        if (b[7] & 0x10000000) { info_created++; out->r[0] = INFO; return NULL; }      /* v scroll: info */
        created++; nicons = b[21];
        title_ptr = (int *)(intptr_t)b[18];
        out->r[0] = WIN;
        return NULL;
    case 0x400FC:                                                         /* ResizeIcon */
        if (in->r[0] == WIN && in->r[1] >= 0 && in->r[1] < 9)
            for (int i = 0; i < 4; i++) icon_box[in->r[1]][i] = in->r[2 + i];
        if (in->r[0] == MINI && in->r[1] >= 0 && in->r[1] < 10)
            for (int i = 0; i < 4; i++) mini_box[in->r[1]][i] = in->r[2 + i];
        return NULL;
    case 0x400C5:                                                         /* OpenWindow */
        memcpy(st(b[0]), b, 32);
        st(b[0])[5] = st(b[0])[6] = 0;
        if (b[0] == WIN) { win_open = 1; opened_w = b[3] - b[1]; opened_h = b[4] - b[2]; }
        else if (b[0] == INFO) info_open = 1;
        else if (b[0] == MINI) { mini_open = 1; if (phase == P_ONTOP && b[7] == -1) mini_tops++; }
        else full_open = 1;
        return NULL;
    case 0x400CB:                                                         /* GetWindowState */
        if (b[0] == -2) {                                                 /* the icon bar */
            b[1] = 0; b[2] = 0; b[3] = SCR_W * 2; b[4] = 134; b[5] = b[6] = 0; b[7] = -1;
            return NULL;
        }
        memcpy(b + 1, st(b[0]) + 1, 32);
        return NULL;
    case 0x400C6:                                                         /* CloseWindow */
        if (b[0] == WIN) win_open = 0; else if (b[0] == INFO) info_open = 0; else if (b[0] == MINI) mini_open = 0; else full_open = 0;
        return NULL;
    case 0x400F9:                                                         /* Wimp_TextOp */
        if (in->r[0] == 2 && drawing_info && strlen(info_seen) + strlen((char *)(intptr_t)in->r[1]) + 2 < sizeof(info_seen)) {
            strcat(info_seen, (char *)(intptr_t)in->r[1]);
            strcat(info_seen, "|");
            info_texts++;
        }
        return NULL;
    case 0x400D1: case 0x400CD: case 0x400DC: case 0x400D4: return NULL;
    case 0x400D2: caret_win = in->r[0]; return NULL;                      /* SetCaretPosition */
    case 0x400CF: b[2] = 4; return NULL;                                  /* GetPointerInfo: Select */
    case 0x400DF:                                                         /* ReportError */
        if (((in->r[1] >> 9) & 7) == 4) { asks++; out->r[1] = 3; }        /* our own buttons: "Carry on" */
        else reports++;
        return NULL;
    case 0x06:                                                            /* OS_Byte */
        if (in->r[0] == 19) vsyncs++;                                     /* wait for the vsync */
        else if (in->r[0] == 129) out->r[1] = 0;                          /* INKEY: Shift isn't held */
        return NULL;
    case 0x400E7: if (in->r[0] == 17 && b[4] == 4 && b[3] == 99) acks++; return NULL;   /* SendMessage */
    case 0x400DD: final_checks(); return NULL;                            /* CloseDown */
    case 0x42: out->r[0] = (int)(fake_time * 100); return NULL;           /* OS_ReadMonotonicTime */
    case 0x65: return &err;                                               /* OS_ScreenMode: TBGR anyway */
    case 0x50B00:                                                         /* MimeMap_Translate */
        strcpy((char *)(intptr_t)in->r[3], in->r[1] == 0xBF8 ? "video/mpeg" : "text/plain");
        return NULL;
    case 0x400C9: case 0x400C8: {                                         /* Update/RedrawWindow */
        int w = b[0], *s = st(w);
        if (swi == 0x400C9) updates++;
        drawing_info = w == INFO;
        if (w == INFO && swi == 0x400C9) { info_updates++; info_seen[0] = 0; }
        memcpy(b + 1, s + 1, 24);                                         /* visible box, scroll */
        b[7] = s[1]; b[8] = s[2]; b[9] = s[3]; b[10] = s[4];              /* one rectangle: all of it */
        pending_rect = 1;
        out->r[0] = (w == WIN && win_open) || (w == FULL && full_open) || (w == INFO && info_open) || (w == MINI && mini_open);
        return NULL;
    }
    case 0x400CA: out->r[0] = 0; return NULL;                             /* GetRectangle: no more */
    case 0x46: {                                                          /* OS_WriteN: VDU 24 */
        const unsigned char *v = (const unsigned char *)(intptr_t)in->r[0];
        int y0 = (short)(v[3] | v[4] << 8), y1 = (short)(v[7] | v[8] << 8);
        int w = full_open ? FULL : mini_open ? MINI : WIN;
        if (v[0] != 24 || y0 < vis_pic_y0(w) || y1 >= st(w)[4]) clip_ok = 0;
        return NULL;
    }
    case 0x2E: {                                                          /* OS_SpriteOp 52 */
        const int *spr = (const int *)(intptr_t)in->r[2];
        if (in->r[0] != 52 + 512 || spr[7] != 31 || spr[8] != 44) return &err;
        plots++;
        return NULL;
    }
    case 0x400E1:                                                         /* Wimp_PollIdle */
        idle_polls++;
        if (in->r[2] > (int)(fake_time * 100) + 1 && next_is_null())
            fake_time = in->r[2] / 100.0 - 0.02;                          /* slept: time passes */
        /* fallthrough */
    case 0x400C7: {                                                       /* Wimp_Poll */
        last_mask = in->r[0];
        out->r[0] = next_event(b);
        return NULL;
    }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    char *args[2] = { "reel", NULL };
    setvbuf(stdout, NULL, _IONBF, 0);
    clip1 = argv[1];
#ifdef REEL_EGL
    fake_scr_w = SCR_W; fake_scr_h = SCR_H;
#endif
    clip2 = argv[2];
    (void)title;
    strcpy(choices_dir, "/tmp/reelchoicesXXXXXX");                        /* a fresh Choices directory */
    if (!mkdtemp(choices_dir))
        return 1;
    setenv("Reel$ChoicesDir", choices_dir, 1);
    setenv("ReelEGL$ChoicesDir", choices_dir, 1);
    reel_main(1, args);
    return 1;
}
