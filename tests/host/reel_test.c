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
#include "../../common/version.h"
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
#define FRAMES_DRAWN fake_swaps
#else
const uint8_t *reel_test_sprite(int *w, int *h, int *rows);
#define PLOTS plots
#define FRAMES_DRAWN updates
#endif
int reel_test_fullscreen(void);
int reel_test_pic_flags(void);
int reel_test_ab(double *a, double *b);
int reel_test_list(int *n);
int reel_test_mini(int *ontop);
int reel_test_deint(void);

static const char *clip1, *clip2;
static int fails, step;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define WIN 0x100
#define FULL 0x200
#define INFO 0x300                    /* the media info window */
#define MINI 0x400                    /* the mini player */
#define PROGINFO 0x500                /* About this program (Info on the icon bar menu) */
#define URLW 0x600                    /* the Open address window */
#define COVERW 0x700                  /* another task's window, put over the picture */
#define SCR_W 1920                    /* pixels; eig 1 -> 3840 x 2160 OS units */
#define SCR_H 1080

static int created, opened_w, opened_h, win_x0, win_y1, full_open, win_open, nicons;
static int state[5][9];               /* window states: [0] WIN, [1] FULL, [2] INFO, [3] MINI, [4] URLW */
static int info_created, info_open, info_updates, info_texts;
static char info_seen[8192];          /* what Wimp_TextOp drew in the info window */
static int drawing_info;
static int plots, plots_full, clip_ok = 1, updates, acks, reports, keys_passed, last_mask;
static int caret_win;
static int icon_box[12][4], mini_box[12][4];
static int drag_win, drag_type;         /* the last Wimp_DragBox */
static int mini_created, mini_open, mini_nicons, mini_tops, main_w, main_h;
static int vsyncs, asks;
static double ab_lo, ab_hi;
static char choices_dir[64];

/* the window menu (reel.c's WM_*) */
enum { M_INFO, M_STATS, M_FULL, M_MINI, M_ONTOP, M_SIZE, M_PIC, M_DEINT, M_SPEED, M_TRACK, M_LIST, M_AB, M_LOOP, M_FAST, M_VSYNC, M_HWACCEL, M_CLOSE };
static char title[64];
static int *title_ptr;
static int proginfo_made, proginfo_icons, bar_info_sub = -99;
static char proginfo_seen[512];     /* "Name:=Reel|Purpose:=..|" from the window's icons */

static int *st(int w) { return state[w == FULL ? 1 : w == INFO ? 2 : w == MINI ? 3 : w == URLW ? 4 : 0]; }

/* web addresses (REEL_TEST_URL: tests/host/httpserve.py serving the clip and
   its video-only and sound-only copies) */
static const char *base_url;
static int url_created, url_open, url_nicons, msg_refs = 1000;
static char *url_field;                 /* the address field's text (the icon's buffer) */
static int sent_action, sent_code, sent_to, sent_my_ref, sent_your_ref, sent_flags, sent_win;
static char scrap[64];
static char last_report[256];

/* a fake VideoOverlay module (off until the overlay phases) */
static int ovl_present, ovl_create_fail, ovl_map_fail_from = 99, ovl_creates, ovl_destroys, ovl_id, ovl_banks;
static int ovl_sel[16], ovl_displays, ovl_shown = -1, ovl_redraws, ovl_window, ovl_scale[2], ovl_pos[6];
static int ovl_fw, ovl_fh, ovl_unmapped = 1;
static uint8_t *ovl_buf[3];
static int ovl_arr[3][6];
static int cover_on;                  /* COVERW is open over the picture */
static int fake_vsync;
static int last_poll_idle;               /* the poll now was Wimp_PollIdle */
static int vsync_held = -1;             /* >= 0: the vsync counter stands still at this */
static int ovl_last_vsync = -1, ovl_same_vsync;   /* two switches before one vsync: it would tear */
static int vsync_now(void) { return vsync_held >= 0 ? vsync_held : (fake_vsync + (int)(fake_time * 60)) & 0xFF; }

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
    case 0:  click(b, 1000, 40, 2, -2, 1); return 6;                   /* Menu on the icon bar icon */
    case 1:  b[0] = 0; b[1] = -1; return 9;                           /* Info (its window is the submenu) */
    case 2:  message(b, 3, -2, 1, 0xBF8, clip1); return 18;           /* drop on the icon bar */
    case 3:  nulls = 60; /* fallthrough */
    default: break;
    }
    return -1;
}

/* the phases after the first drop */
enum { P_PLAY1, P_VOLUME, P_SPEED, P_SPEEDPLAY, P_SPEEDBACK, P_PICFILL, P_PICFIT, P_DEINT, P_FAST, P_AB, P_ABPLAY, P_ABOFF,
       P_INFO, P_INFOPLAY, P_INFOCLOSE, P_PANEL, P_PANELOFF, P_PAUSE, P_PAUSED, P_RESUME, P_PLAY2, P_SEEKBAR, P_PLAY3, P_FULL, P_PLAYFULL,
       P_VSYNCOFF, P_UNFULL, P_RESIZE, P_GRIP, P_SIZEHALF, P_SIZEFIT, P_SIZEACTUAL, P_PLAY4, P_DROP2, P_PLAY5, P_LIST,
       P_MINI, P_MINIPLAY, P_ONTOP, P_MINIMOVE, P_MINIGRIP, P_MINIBACK, P_DIR, P_OPEN_OTHER, P_OPEN_VIDEO, P_PLAY6, P_OVLREFUSE, P_OVLMODE, P_OVLPLAY, P_OVLWAIT, P_OVLREDRAW, P_OVLCOVER, P_OVLUNCOVER,
       P_OVLPAUSE, P_OVLRESUME, P_OVLFEWER, P_OVLOFF, P_URL, P_URLOPENING, P_URLPLAY, P_CLOSE, P_URLFILE,
       P_URLFILEOPENING, P_URLFILEPLAY, P_URLBAD, P_CLOSE2, P_QUIT };
static int phase = -1, phase_step;
/* the phases that play (deliver nulls) */
static int next_is_null(void)
{
    return phase == P_PLAY1 || phase == P_PLAY2 || phase == P_PLAY3 || phase == P_PLAYFULL || phase == P_PLAY4 ||
           phase == P_PLAY5 || phase == P_PLAY6 || phase == P_INFOPLAY || phase == P_PANEL || phase == P_PANELOFF || phase == P_SPEEDPLAY || phase == P_AB ||
           phase == P_ABPLAY || phase == P_MINIPLAY || phase == P_ONTOP || phase == P_URLPLAY || phase == P_URLFILEPLAY ||
           phase == P_URLOPENING || phase == P_URLFILEOPENING || phase == P_OVLREFUSE || phase == P_OVLPLAY || phase == P_OVLWAIT ||
           phase == P_OVLCOVER || phase == P_OVLUNCOVER || phase == P_OVLRESUME || phase == P_OVLFEWER ||
           phase == P_OVLOFF || phase == P_OVLMODE;
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
        case P_MINIPLAY: case P_URLPLAY: case P_URLFILEPLAY:
            if (!base_url && (phase == P_URLPLAY || phase == P_URLFILEPLAY))
                break;
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
        case P_DEINT:                                                 /* Deinterlace: Off from the menu, then D: Auto */
            MENU_PICK(WIN, M_DEINT, 2);
            if (phase_step == 2) {
                CHECK(reel_test_deint() == REELCORE_DEINT_OFF && reelcore_deinterlace(v) == REELCORE_DEINT_OFF &&
                      choices_has("deinterlace Off"), "deinterlace off: %d, core %d", reel_test_deint(), reelcore_deinterlace(v));
                phase_step++;
                key_event(b, WIN, 'd');
                return 8;
            }
            CHECK(reel_test_deint() == REELCORE_DEINT_AUTO && reelcore_deinterlace(v) == REELCORE_DEINT_AUTO &&
                  choices_has("deinterlace Auto"), "D: deinterlace %d (want Auto)", reel_test_deint());
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
            {                                                         /* beside the player, not over it */
                int *w = st(WIN), *i = st(INFO);
                CHECK(i[1] >= w[3] + 44 || i[3] <= w[1] - 44 || i[4] <= w[2] - 44 || i[2] >= w[4] + 44,
                      "I: media info (%d,%d)-(%d,%d) over the player (%d,%d)-(%d,%d)",
                      i[1], i[2], i[3], i[4], w[1], w[2], w[3], w[4]);
                CHECK(i[1] >= 0 && i[3] <= SCR_W * 2 && i[2] >= 0 && i[4] <= SCR_H * 2 && i[3] - i[1] >= 1000,
                      "I: media info (%d,%d)-(%d,%d) off the screen or too narrow", i[1], i[2], i[3], i[4]);
            }
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
            CHECK(strstr(info_seen, "Deinterlacing|Auto: not needed") != NULL, "no deinterlacing row: %.300s", info_seen);
            CHECK((strstr(info_seen, "SharedSoundBuffer: ") || strstr(info_seen, "SDL: ")) &&
                  strstr(info_seen, "pictures,") && strstr(info_seen, "Mbit/s"),
                  "stats text: %.400s", info_seen);
            printf("  info: %s\n", strstr(info_seen, "Stats") ? strstr(info_seen, "Stats") : info_seen);
            break;
        case P_INFOCLOSE:
            if (phase_step++ == 0) { memset(b, 0, 4); b[0] = INFO; return 3; }
            CHECK(!info_open, "info window didn't close");
            break;
        case P_PANEL: {                                               /* S: stats drawn into the picture */
            int pw, ph, w, h, rows, white = 0;
            const uint8_t *px;
            if (phase_step == 0) { phase_step++; key_event(b, WIN, 's'); return 8; }
            if (phase_step++ < 80) { fake_time += 0.02; return 0; }
            reelcore_panel_size(v, &pw, &ph);
            px = reel_test_sprite(&w, &h, &rows);
            CHECK(pw > 300 && ph > 150, "S: stats panel %dx%d", pw, ph);
            for (int y = 10; px && y < 10 + ph && y < h; y++)
                for (int x = 10; x < 10 + pw && x < w; x++)
                    white += px[(y * w + x) * 4] > 220 && px[(y * w + x) * 4 + 1] > 220 && px[(y * w + x) * 4 + 2] > 220;
            CHECK(white > 500, "S: the panel's text isn't in the picture (%d white pixels)", white);
            check_picture("stats panel");
            printf("  stats panel: %dx%d, %d white pixels of text in the picture\n", pw, ph, white);
            break;
        }
        case P_PANELOFF: {
            int pw, ph;
            if (phase_step == 0) { phase_step++; key_event(b, WIN, 's'); return 8; }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            reelcore_panel_size(v, &pw, &ph);
            CHECK(pw == 0 && ph == 0, "S again: the panel is still %dx%d", pw, ph);
            check_picture("stats panel off");
            break;
        }
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
        case P_GRIP:                                                  /* the grip: the Wimp's size drag */
            if (phase_step == 0) {
                int vh = st(WIN)[4] - st(WIN)[2], vw = st(WIN)[3] - st(WIN)[1];
                phase_step++;
                CHECK(icon_box[9][0] == vw - 32 && icon_box[9][1] == -vh && icon_box[9][2] == vw && icon_box[9][3] == -vh + 32,
                      "grip at %d,%d-%d,%d (want the bottom right corner of %dx%d)",
                      icon_box[9][0], icon_box[9][1], icon_box[9][2], icon_box[9][3], vw, vh);
                drag_win = drag_type = 0;
                click(b, sx(WIN, vw - 16), st(WIN)[2] + 16, 64, WIN, 9);   /* a Select drag on it */
                return 6;
            }
            if (phase_step++ == 1) {
                CHECK(drag_win == WIN && drag_type == 2, "grip: Wimp_DragBox window %x type %d (want the window, 2)",
                      drag_win, drag_type);
                memcpy(b, st(WIN), 36);                               /* what the Wimp sends as it's dragged */
                b[3] = b[1] + 1400;
                b[2] = b[4] - 800;
                return 2;
            }
            {
                int w, h, rows;
                reel_test_sprite(&w, &h, &rows);
                CHECK(w == 700 && h == (800 - 64) / 2 && icon_box[9][0] == 1400 - 32,
                      "after the grip: picture %dx%d, grip at %d", w, h, icon_box[9][0]);
            }
            break;
        case P_SIZEHALF:                                              /* Window size > Half: never narrower than the controls */
            MENU_PICK(WIN, M_SIZE, 0);
            CHECK(opened_w == 1132 && opened_h == 184 + 64, "half size: %dx%d (want 1132x248)", opened_w, opened_h);
            break;
        case P_SIZEFIT:                                               /* Fit the screen: as big as fits, the video's shape */
            MENU_PICK(WIN, M_SIZE, 3);
            {
                int *w = st(WIN), ph = opened_h - 64;
                CHECK(opened_w > 1400 && w[1] >= 0 && w[3] <= SCR_W * 2 && w[2] >= 134 && w[4] <= SCR_H * 2 - 44 &&
                      abs(ph * 322 - opened_w * 184) < opened_w * 184 / 50,
                      "fit the screen: %dx%d at %d,%d-%d,%d", opened_w, opened_h, w[1], w[2], w[3], w[4]);
            }
            break;
        case P_SIZEACTUAL:                                            /* Actual size: 322x184 pixels (the controls' width) */
            MENU_PICK(WIN, M_SIZE, 1);
            CHECK(opened_w == 1132 && opened_h == 368 + 64, "actual size: %dx%d (want 1132x432)", opened_w, opened_h);
            check_picture("actual size");
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
                CHECK(reel_test_mini(&on) && mini_created == 1 && mini_open && !win_open && mini_nicons == 11,
                      "mini player: made %d, open %d, window open %d, %d icons", mini_created, mini_open, win_open, mini_nicons);
                CHECK(vw == 640 && vh == 364 + 64, "mini player %dx%d (want 640x428)", vw, vh);
                CHECK(m[3] == SCR_W * 2 - 32 && m[2] == 134 + 16, "mini player at %d,%d-%d,%d (want the bottom right, above the icon bar)",
                      m[1], m[2], m[3], m[4]);
                CHECK(w == 320 && h == 182, "mini player picture %dx%d", w, h);
#ifdef REEL_EGL
                CHECK(fake_surfaces == 1 && fake_wa[2] == 320 && fake_wa[3] == 182, "mini player surface %dx%d, %d surfaces",
                      fake_wa[2], fake_wa[3], fake_surfaces);
#endif
                CHECK(mini_box[10][2] == 640 - 4 - 32 - 4 && mini_box[5][2] < 0 && mini_box[3][2] < mini_box[10][0],
                      "mini player controls: Normal to %d, time to %d, bar to %d", mini_box[10][2], mini_box[5][2], mini_box[3][2]);
                CHECK(mini_box[9][0] == 640 - 32 && mini_box[9][1] == -vh && mini_box[9][2] == 640 && mini_box[9][3] == -vh + 32,
                      "mini player grip at %d,%d-%d,%d (want the bottom right corner)",
                      mini_box[9][0], mini_box[9][1], mini_box[9][2], mini_box[9][3]);
                CHECK(reelcore_fast(v) == REELCORE_FAST_LIGHT, "mini player: fast decoding %d (want light)", reelcore_fast(v));
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
        case P_MINIGRIP:                                              /* the mini player's grip: wider, the video's shape */
            if (phase_step++ == 0) {
                memcpy(b, st(MINI), 36);
                b[3] = b[1] + 960;
                b[2] = b[4] - 300;                                    /* any height: it follows the width */
                return 2;
            }
            {
                int *m = st(MINI), w, h, rows;
                reel_test_sprite(&w, &h, &rows);
                CHECK(m[3] - m[1] == 960 && m[4] - m[2] == 548 + 64 && m[4] == 600 + 428,
                      "mini player resized to %dx%d, top %d (want 960x612, top 1028)", m[3] - m[1], m[4] - m[2], m[4]);
                CHECK(w == 480 && h == 274, "mini player picture %dx%d (want 480x274)", w, h);
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
                CHECK(reelcore_fast(v) == REELCORE_FAST_OFF, "normal window: fast decoding %d (want off)", reelcore_fast(v));
                CHECK(choices_has("mini_width 960"), "the mini player's width wasn't saved");
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
        /* ---- the hardware overlay (a fake VideoOverlay) ---- */
        case P_OVLREFUSE: {                                           /* the module there, but Create refuses */
            static int draws0;
            if (phase_step == 0) {
                ovl_present = 1;
                ovl_create_fail = 1;
                draws0 = FRAMES_DRAWN;
            }
            if (phase_step++ < 30) { fake_time += 0.02; return 0; }
            CHECK(ovl_creates == 1 && !ovl_id, "Create refused: %d tries (want 1, not again), overlay %d",
                  ovl_creates, ovl_id);
            CHECK(FRAMES_DRAWN > draws0 + 10, "Create refused: pictures not drawn as before");
            check_picture("overlay refused: drawn as before");
            break;
        }
        case P_OVLMODE:                                               /* a mode change: tries again, and gets one */
            if (phase_step == 0) {
                phase_step++;
                ovl_create_fail = 0;
                memset(b, 0, 256); b[0] = 20; b[4] = 0x400C1;
                return 17;                                            /* Message_ModeChange */
            }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            {
                int fw, fh;
                reelcore_frame_size(v, &fw, &fh);
                CHECK(ovl_creates == 2 && ovl_id && ovl_banks == 3, "after the mode change: %d creates, id %d, %d buffers",
                      ovl_creates, ovl_id, ovl_banks);
                CHECK(ovl_sel[1] == (fw & ~1) && ovl_sel[2] == (fh & ~1) && ovl_sel[3] == 7 && ovl_sel[5] == 0 &&
                      (ovl_sel[6] & 0x3000) == 0x2000 && ovl_sel[7] == 3 && ovl_sel[8] == 0x32315659 &&
                      ovl_sel[9] == 13 && ovl_sel[10] == 3 && ovl_sel[11] == -1,
                      "the selector: %dx%d log2bpp %d, var %d=%x, var %d=%x, var %d=%d", ovl_sel[1], ovl_sel[2],
                      ovl_sel[3], ovl_sel[5], ovl_sel[6], ovl_sel[7], ovl_sel[8], ovl_sel[9], ovl_sel[10]);
                CHECK(ovl_sel[6] == 0x6000, "ModeFlags %x (want &6000: YCbCr, BT.601, video range)", ovl_sel[6]);
            }
            break;
        case P_OVLPLAY: {                                             /* pictures go through it, not the sprite */
            static int draws0, displays0;
            if (phase_step == 0) {
                draws0 = FRAMES_DRAWN;
                displays0 = ovl_displays;
            }
            if (phase_step++ < 40) { fake_time += 0.02; return 0; }
            CHECK(ovl_displays >= displays0 + 15 && ovl_shown >= 0 && ovl_unmapped, "overlay: %d pictures shown, buffer %d%s",
                  ovl_displays - displays0, ovl_shown, ovl_unmapped ? "" : ", left mapped");
            CHECK(FRAMES_DRAWN == draws0, "overlay: %d pictures drawn as well", FRAMES_DRAWN - draws0);
            CHECK(ovl_window == WIN, "overlay attached to %x", ovl_window);
            {   /* its Y plane is the frame's; Cb, Cr likewise */
                int fw = ovl_fw, fh = ovl_fh, bad = 0;
                uint8_t *p[3], *want = malloc((size_t)fw * fh * 3 / 2);
                int pitch[3] = { fw, fw / 2, fw / 2 };
                p[0] = want; p[1] = want + fw * fh; p[2] = p[1] + fw / 2 * (fh / 2);
                reelcore_draw_yuv420(v, p, pitch, fw, fh, NULL);
                bad = memcmp(ovl_buf[ovl_shown], want, (size_t)fw * fh * 3 / 2) != 0;
                CHECK(!bad, "the overlay's buffer isn't the frame's YV12");
                free(want);
            }
            {   /* Fit: the video's shape, as big as fits in the picture area, centred */
                int bw = (icon_box[9][2]) / 2, bh = (st(WIN)[4] - st(WIN)[2] - 64) / 2;
                int dw = reelcore_width(v), dh = reelcore_height(v);
                double sc = (double)bw / dw < (double)bh / dh ? (double)bw / dw : (double)bh / dh;
                int rw = (int)(dw * sc + 0.5), rh = (int)(dh * sc + 0.5);
                CHECK(abs(ovl_scale[0] - rw) <= 1 && abs(ovl_scale[1] - rh) <= 1 && ovl_pos[4] == bw * 2 && ovl_pos[5] == 0 &&
                      ovl_pos[2] == 0 && ovl_pos[3] == -(st(WIN)[4] - st(WIN)[2] - 64),
                      "placed %dx%d (want %dx%d in %dx%d), clip %d,%d-%d,%d", ovl_scale[0], ovl_scale[1], rw, rh, bw, bh,
                      ovl_pos[2], ovl_pos[3], ovl_pos[4], ovl_pos[5]);
            }
            printf("  overlay: %d pictures shown through it, %dx%d on screen\n", ovl_displays - displays0,
                   ovl_scale[0], ovl_scale[1]);
            break;
        }
        case P_OVLWAIT: {                                             /* no vsync yet: pictures wait, nothing blocks */
            static int displays0, vs0, draws0;
            if (phase_step == 0) {
                vsync_held = vsync_now();
                displays0 = ovl_displays; vs0 = vsyncs; draws0 = FRAMES_DRAWN;
            }
            if (phase_step++ < 15) { fake_time += 0.02; return 0; }
            if (phase_step == 16) {
                CHECK(ovl_displays <= displays0 + 1 && vsyncs == vs0 && FRAMES_DRAWN == draws0,
                      "no vsync: %d switches, %d vsync waits, %d drawn (want <= 1, none, none)",
                      ovl_displays - displays0, vsyncs - vs0, FRAMES_DRAWN - draws0);
                CHECK(!last_poll_idle, "a picture waiting for the overlay, yet Reel slept");
                vsync_held = -1;                                      /* the refresh comes: shown on the next pass */
                displays0 = ovl_displays;
                fake_time += 0.02;
                return 0;
            }
            CHECK(ovl_displays == displays0 + 1, "after the vsync: %d switches (want 1)", ovl_displays - displays0);
            CHECK(!ovl_same_vsync, "%d overlay switches before a vsync (tearing)", ovl_same_vsync);
            break;
        }
        case P_OVLREDRAW:                                             /* a redraw asks VideoOverlay to do its part */
            if (phase_step++ == 0) {
                ovl_redraws = 0;
                memset(b, 0, 64); b[0] = WIN;
                return 1;
            }
            CHECK(ovl_redraws >= 1, "redraw: VideoOverlay_RedrawWindow not called");
            break;
        case P_OVLCOVER: {                                            /* a window over the picture: hidden, drawn as before */
            static int draws0;
            if (phase_step == 0) {
                cover_on = 1;
                st(WIN)[7] = COVERW;
                draws0 = FRAMES_DRAWN;
            }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            CHECK(ovl_shown == -1 && FRAMES_DRAWN > draws0 + 3, "covered: overlay buffer %d, %d pictures drawn",
                  ovl_shown, FRAMES_DRAWN - draws0);
            check_picture("covered: drawn as before");
            break;
        }
        case P_OVLUNCOVER:
            if (phase_step == 0) {
                cover_on = 0;
                st(WIN)[7] = -1;
            }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            CHECK(ovl_shown >= 0, "uncovered: the overlay isn't back");
            break;
        case P_OVLPAUSE:                                              /* paused: hidden, the paused picture drawn */
            if (phase_step++ == 0) { key_event(b, WIN, ' '); return 8; }
            CHECK(ovl_shown == -1 && reelcore_paused(v), "paused: overlay buffer %d", ovl_shown);
            check_picture("paused: drawn as before");
            break;
        case P_OVLRESUME:
            if (phase_step == 0) { phase_step++; key_event(b, WIN, ' '); return 8; }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            CHECK(ovl_shown >= 0 && !reelcore_paused(v), "resumed: overlay buffer %d", ovl_shown);
            break;
        case P_OVLFEWER:                                              /* the GPU has room for 2 buffers only */
            if (phase_step == 0) {
                phase_step++;
                ovl_map_fail_from = 2;
                memset(b, 0, 256); b[0] = 20; b[4] = 0x400C1;
                return 17;
            }
            if (phase_step++ < 10) { fake_time += 0.02; return 0; }
            CHECK(ovl_id && ovl_banks == 2 && ovl_shown >= 0 && ovl_destroys >= 2,
                  "short of GPU memory: id %d, %d buffers, shown %d, %d destroyed", ovl_id, ovl_banks, ovl_shown, ovl_destroys);
            break;
        case P_OVLOFF: {                                              /* Hardware acceleration off: drawn as before */
            static int draws0;
            MENU_PICK(WIN, M_HWACCEL, -1);
            if (phase_step == 2) {
                phase_step++;
                CHECK(!ovl_id && ovl_shown == -1 && choices_has("hardware_acceleration 0"),
                      "switched off: overlay %d, shown %d, saved %d", ovl_id, ovl_shown, choices_has("hardware_acceleration 0"));
                draws0 = FRAMES_DRAWN;
            }
            if (phase_step++ < 20) { fake_time += 0.02; return 0; }
            CHECK(!ovl_id && FRAMES_DRAWN > draws0 + 5, "switched off: overlay %d, %d drawn", ovl_id, FRAMES_DRAWN - draws0);
            check_picture("hardware acceleration off");
            break;
        }
        case P_URL:                                                   /* Open address..., Ctrl-V, Return */
            if (!base_url)
                break;
            if (phase_step == 0) { phase_step++; click(b, 1000, 40, 2, -2, 1); return 6; }     /* the icon bar menu */
            if (phase_step == 1) { phase_step++; b[0] = 1; b[1] = -1; return 9; }             /* Open address... */
            if (phase_step == 2) {
                phase_step++;
                CHECK(url_created == 1 && url_open && url_nicons == 5 && caret_win == URLW && url_field,
                      "Open address: made %d, open %d, %d icons, caret in %x", url_created, url_open, url_nicons, caret_win);
                sent_action = 0;
                key_event(b, URLW, 22);                               /* Ctrl-V */
                return 8;
            }
            if (phase_step == 3) {                                    /* the clipboard's holder answers */
                phase_step++;
                CHECK(sent_action == 0x10 && sent_code == 18 && sent_to == 0 && sent_win == URLW && (sent_flags & 4),
                      "Ctrl-V: sent %x (code %d, to %x, window %x, flags %x): want a DataRequest for the clipboard",
                      sent_action, sent_code, sent_to, sent_win, sent_flags);
                message(b, 1, URLW, 1, 0xFFF, "Clipboard");           /* DataSave */
                b[3] = sent_my_ref;
                sent_action = 0;
                return 17;
            }
            if (phase_step == 4) {                                    /* it saves to the scrap file we named */
                FILE *f;
                phase_step++;
                CHECK(sent_action == 2 && sent_your_ref == 99 && !strcmp(scrap, "<Wimp$Scrap>"),
                      "DataSave: answered %x to %d, file '%s' (want DataSaveAck, <Wimp$Scrap>)", sent_action, sent_your_ref, scrap);
                strcpy(scrap, "/tmp/reel_scrapXXXXXX");
                close(mkstemp(scrap));
                f = fopen(scrap, "w");
                fprintf(f, "%s/long_h264_aac_322_184.mp4\n", base_url);
                fclose(f);
                message(b, 3, URLW, 1, 0xFFF, scrap);                 /* DataLoad */
                b[3] = sent_my_ref;
                return 17;
            }
            if (phase_step == 5) {
                char want[256];
                phase_step++;
                snprintf(want, sizeof(want), "%s/long_h264_aac_322_184.mp4", base_url);
                CHECK(access(scrap, F_OK) != 0, "the scrap file wasn't deleted");
                CHECK(url_field && !strcmp(url_field, want), "pasted: '%s'", url_field ? url_field : "");
                key_event(b, URLW, 13);                               /* Return: play it */
                return 8;
            }
            CHECK(!url_open, "the Open address window stayed open");
            CHECK(title_ptr && strstr((char *)title_ptr, "Opening"), "while opening, the title is '%s'",
                  title_ptr ? (char *)title_ptr : "");
            break;
        case P_URLOPENING: case P_URLFILEOPENING: {                   /* nulls until it's open */
            static ReelCore *was;
            if (!base_url)
                break;
            if (phase_step == 0)
                was = v;
            if (phase_step++ < 5000 && (reel_test_video() == was || !reel_test_video())) {
                fake_time += 0.01;
                usleep(2000);                                         /* the reader thread's time */
                return 0;
            }
            v = reel_test_video();
            CHECK(v && v != was && reelcore_width(v) == 322, "the address didn't open (%d nulls)", phase_step);
            CHECK(title_ptr && strstr((char *)title_ptr, phase == P_URLOPENING ? "127.0.0.1:" : "net_video_only.mp4") &&
                  strstr((char *)title_ptr, phase == P_URLOPENING ? "long_h264_aac_322_184" : "127.0.0.1:") &&
                  !strstr((char *)title_ptr, "Opening"),
                  "opened: the title is '%s'", title_ptr ? (char *)title_ptr : "");
            if (phase == P_URLFILEOPENING)
                CHECK(v && reelcore_has_audio(v), "video and sound apart: no sound");
            printf("  %s opened after %d nulls\n", phase == P_URLOPENING ? "the address" : "yt-dlp -g's two addresses", phase_step);
            break;
        }
        case P_URLFILE:                                               /* yt-dlp -g's output dropped, nothing playing */
            if (!base_url)
                break;
            if (phase_step++ == 0) {
                FILE *f;
                strcpy(scrap, "/tmp/reel_ytdlpXXXXXX");
                close(mkstemp(scrap));
                f = fopen(scrap, "w");
                fprintf(f, "%s/net_video_only.mp4?mime=video%%2Fmp4\n%s/net_sound_only.m4a?mime=audio%%2Fmp4\n",
                        base_url, base_url);
                fclose(f);
                message(b, 3, -2, 1, 0xFFF, scrap);
                return 18;
            }
            unlink(scrap);
            CHECK(win_open && title_ptr && strstr((char *)title_ptr, "Opening") && !reel_test_video(),
                  "nothing playing: the window should say it's opening (open %d, title '%s')", win_open,
                  title_ptr ? (char *)title_ptr : "");
            break;
        case P_URLBAD: {                                              /* a web page: the reason, and the one playing stays */
            static int reports_before;
            static ReelCore *was;
            if (!base_url)
                break;
            if (phase_step == 0) {
                phase_step++;
                reports_before = reports;
                was = v;
                snprintf(url_field, 1024, "%s/page.html", base_url);  /* a web page: HTML */
                key_event(b, URLW, 13);
                return 8;
            }
            if (phase_step++ < 5000 && reports == reports_before) {
                fake_time += 0.01;
                usleep(2000);
                return 0;
            }
            CHECK(reports == reports_before + 1 && reel_test_video() == was && strstr(last_report, "web page, not a video"),
                  "a web page: %d reports ('%s'), still playing %d", reports - reports_before, last_report,
                  reel_test_video() == was);
            printf("  a web page: %s\n", last_report);
            break;
        }
        case P_CLOSE: case P_CLOSE2:
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
    CHECK(created == 1 && nicons == 10, "window: created %d, %d icons", created, nicons);
    {   /* Info on the icon bar menu: the standard About this program window, as its submenu */
#ifdef REEL_EGL
        const char *want = "Name:=ReelEGL|Purpose:=Video player, EGL (FFmpeg 5.1.10)|Author:=Andrew Youll|"
                           "Version:=" REEL_VERSION " (" REEL_DATE ")|";
#else
        const char *want = "Name:=Reel|Purpose:=Video player (FFmpeg 5.1.10)|Author:=Andrew Youll|"
                           "Version:=" REEL_VERSION " (" REEL_DATE ")|";
#endif
        CHECK(proginfo_made == 1 && proginfo_icons == 8, "Info window: made %d, %d icons", proginfo_made, proginfo_icons);
        CHECK(!strcmp(proginfo_seen, want), "Info window says %s", proginfo_seen);
        CHECK(bar_info_sub == PROGINFO, "icon bar menu: Info's submenu is %x (want the Info window)", bar_info_sub);
        printf("  Info: %s\n", proginfo_seen);
    }
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
        if (!strncmp((char *)(intptr_t)in->r[1], "VideoOverlay_", 13))
            return 0;                                             /* (the fake VideoOverlay's) */
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

/* libavformat's AcornSSL backend (patch 0018) refers to UnixLib's
   __get_ro_socket; Reel never opens https */
__attribute__((weak)) int __get_ro_socket(int fd) { return fd; }

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
    case 0x39: {                                                          /* OS_SWINumberFromString */
        static const char *const names[] = { "Create", "Destroy", "DisplayBuffer", "MapBuffer", "UnmapBuffer",
                                             "DiscardBuffer", "Vet", "SetScale", "SetWindow", "SetPosition",
                                             "RedrawWindow" };
        const char *n = (const char *)(intptr_t)in->r[1];
        if (ovl_present && !strncmp(n, "VideoOverlay_", 13))
            for (int i = 0; i < 11; i++)
                if (!strcmp(n + 13, names[i])) { out->r[0] = 0x59CC0 + i; return NULL; }
        return &err;
    }
    case 0x59CC0:                                                         /* VideoOverlay_Create */
        ovl_creates++;
        if (ovl_create_fail || ovl_id) return &err;
        memcpy(ovl_sel, (const int *)(intptr_t)in->r[0], sizeof(ovl_sel));
        ovl_fw = ovl_sel[1]; ovl_fh = ovl_sel[2]; ovl_banks = ovl_sel[10];
        for (int i = 0; i < 3; i++) {
            free(ovl_buf[i]);
            ovl_buf[i] = calloc(1, (size_t)ovl_fw * ovl_fh * 3 / 2);
            ovl_arr[i][0] = (int)(intptr_t)ovl_buf[i]; ovl_arr[i][1] = ovl_fw;
            ovl_arr[i][2] = (int)(intptr_t)(ovl_buf[i] + ovl_fw * ovl_fh); ovl_arr[i][3] = ovl_fw / 2;
            ovl_arr[i][4] = (int)(intptr_t)(ovl_buf[i] + ovl_fw * ovl_fh + ovl_fw / 2 * (ovl_fh / 2)); ovl_arr[i][5] = ovl_fw / 2;
        }
        ovl_id = 0x42; ovl_shown = -1; ovl_window = 0;
        out->r[0] = ovl_id; out->r[1] = 1; out->r[2] = 16; out->r[3] = 16; out->r[4] = 4096; out->r[5] = 4096;
        return NULL;
    case 0x59CC1:                                                         /* Destroy */
        if (in->r[0] != ovl_id || !ovl_id) return &err;
        ovl_id = 0; ovl_shown = -1; ovl_destroys++;
        return NULL;
    case 0x59CC2:                                                         /* DisplayBuffer */
        if (in->r[0] != ovl_id || !ovl_id || in->r[1] >= ovl_banks) return &err;
        ovl_shown = in->r[1] < 0 ? -1 : in->r[1];
        if (in->r[1] >= 0) {
            ovl_displays++;
            if (vsync_now() == ovl_last_vsync) ovl_same_vsync++;
            ovl_last_vsync = vsync_now();
        }
        return NULL;
    case 0x59CC3:                                                         /* MapBuffer */
        if (in->r[0] != ovl_id || !ovl_id || in->r[1] < 0 || in->r[1] >= ovl_banks || in->r[1] >= ovl_map_fail_from)
            return &err;
        ovl_unmapped = 0;
        out->r[0] = (int)(intptr_t)ovl_arr[in->r[1]];
        return NULL;
    case 0x59CC4: ovl_unmapped = 1; return NULL;                          /* UnmapBuffer */
    case 0x59CC7: ovl_scale[0] = in->r[1]; ovl_scale[1] = in->r[2]; return NULL;       /* SetScale */
    case 0x59CC8: ovl_window = in->r[1]; return NULL;                     /* SetWindow */
    case 0x59CC9: for (int i = 0; i < 6; i++) ovl_pos[i] = in->r[1 + i]; return NULL;  /* SetPosition */
    case 0x59CCA: ovl_redraws++; return NULL;                             /* RedrawWindow */
    case 0x400C0: out->r[1] = 0x1234; return NULL;                        /* Wimp_Initialise */
    case 0x42681: out->r[0] = -1; return NULL;                            /* EnumerateTasks */
    case 0x35:                                                            /* OS_ReadModeVariable */
        out->r[2] = in->r[1] == 4 || in->r[1] == 5 ? 1 : in->r[1] == 9 ? 5 : in->r[1] == 11 ? SCR_W - 1 :
                    in->r[1] == 12 ? SCR_H - 1 : 0;
        return NULL;
    case 0x400C2: out->r[0] = 1; return NULL;                             /* CreateIcon */
    case 0x400C1:                                                         /* CreateWindow */
        if (b[7] == (int)0x84000012) {                                    /* About this program */
            proginfo_made++; proginfo_icons = b[21];
            proginfo_seen[0] = 0;
            for (int i = 0; i + 1 < b[21]; i += 2) {
                const int *label = b + 22 + 8 * i, *value = label + 8;
                char row[128];
                snprintf(row, sizeof(row), "%.12s=%s|", (const char *)(label + 5),
                         (value[4] & 0x100) ? (const char *)(intptr_t)value[5] : "?");
                strncat(proginfo_seen, row, sizeof(proginfo_seen) - strlen(proginfo_seen) - 1);
            }
            out->r[0] = PROGINFO; return NULL;
        }
        if (b[7] == (int)0x80000002) { mini_created++; mini_nicons = b[21]; out->r[0] = MINI; return NULL; }
        if (b[7] == (int)0x87000002) {                                    /* Open address */
            url_created++; url_nicons = b[21];
            url_field = (char *)(intptr_t)b[22 + 8 * 1 + 5];              /* icon 1's text */
            out->r[0] = URLW; return NULL;
        }
        if ((b[7] & 0x80000040) == 0x80000040 && !(b[7] & 0x04000000)) { out->r[0] = FULL; return NULL; }
        if (b[7] & 0x10000000) { info_created++; out->r[0] = INFO; return NULL; }      /* v scroll: info */
        created++; nicons = b[21];
        title_ptr = (int *)(intptr_t)b[18];
        out->r[0] = WIN;
        return NULL;
    case 0x400FC:                                                         /* ResizeIcon */
        if (in->r[0] == WIN && in->r[1] >= 0 && in->r[1] < 12)
            for (int i = 0; i < 4; i++) icon_box[in->r[1]][i] = in->r[2 + i];
        if (in->r[0] == MINI && in->r[1] >= 0 && in->r[1] < 12)
            for (int i = 0; i < 4; i++) mini_box[in->r[1]][i] = in->r[2 + i];
        return NULL;
    case 0x400C5:                                                         /* OpenWindow */
        memcpy(st(b[0]), b, 32);
        st(b[0])[5] = st(b[0])[6] = 0;
        if (b[0] == WIN) { win_open = 1; opened_w = b[3] - b[1]; opened_h = b[4] - b[2]; }
        else if (b[0] == INFO) info_open = 1;
        else if (b[0] == MINI) { mini_open = 1; if (phase == P_ONTOP && b[7] == -1) mini_tops++; }
        else if (b[0] == URLW) url_open = 1;
        else full_open = 1;
        return NULL;
    case 0x400CB:                                                         /* GetWindowState */
        if (b[0] == -2) {                                                 /* the icon bar */
            b[1] = 0; b[2] = 0; b[3] = SCR_W * 2; b[4] = 134; b[5] = b[6] = 0; b[7] = -1;
            return NULL;
        }
        if (b[0] == COVERW) {                                             /* over the picture */
            int *w = st(WIN);
            b[1] = w[1] + 50; b[2] = w[4] - 300; b[3] = w[1] + 400; b[4] = w[4] + 20;
            b[5] = b[6] = 0; b[7] = -1; b[8] = cover_on ? 1 << 16 : 0;
            return NULL;
        }
        memcpy(b + 1, st(b[0]) + 1, 28);
        b[8] = (b[0] == WIN && win_open) || (b[0] == MINI && mini_open) || (b[0] == FULL && full_open) ||
               (b[0] == INFO && info_open) || (b[0] == URLW && url_open) ? 1 << 16 : 0;
        return NULL;
    case 0x400C6:                                                         /* CloseWindow */
        if (b[0] == WIN) win_open = 0; else if (b[0] == INFO) info_open = 0; else if (b[0] == MINI) mini_open = 0;
        else if (b[0] == URLW) url_open = 0; else full_open = 0;
        return NULL;
    case 0x400F9:                                                         /* Wimp_TextOp */
        if (in->r[0] == 2 && drawing_info && strlen(info_seen) + strlen((char *)(intptr_t)in->r[1]) + 2 < sizeof(info_seen)) {
            strcat(info_seen, (char *)(intptr_t)in->r[1]);
            strcat(info_seen, "|");
            info_texts++;
        }
        return NULL;
    case 0x400D4:                                                         /* CreateMenu */
        if (b && b[7 + 3] && !strcmp((const char *)(intptr_t)b[7 + 3], "Info"))   /* the icon bar menu */
            bar_info_sub = b[7 + 1];
        return NULL;
    case 0x400D1: case 0x400CD: case 0x400DC: return NULL;
    case 0x400D2: caret_win = in->r[0]; return NULL;
    case 0x400D0: drag_win = b[0]; drag_type = b[1]; return NULL;       /* Wimp_DragBox */                      /* SetCaretPosition */
    case 0x400CF: b[2] = 4; return NULL;                                  /* GetPointerInfo: Select */
    case 0x400DF:                                                         /* ReportError */
        if (((in->r[1] >> 9) & 7) == 4) { asks++; out->r[1] = 3; }        /* our own buttons: "Carry on" */
        else {
            reports++;
            snprintf(last_report, sizeof(last_report), "%s", (const char *)(intptr_t)in->r[0] + 4);
        }
        return NULL;
    case 0x06:                                                            /* OS_Byte */
        if (in->r[0] == 19) { vsyncs++; fake_vsync++; }                   /* wait for the vsync */
        else if (in->r[0] == 176)                                         /* vsync counter */
            out->r[1] = vsync_now();
        else if (in->r[0] == 129) out->r[1] = 0;                          /* INKEY: Shift isn't held */
        return NULL;
    case 0x400E7:                                                         /* SendMessage */
        b[2] = ++msg_refs;                                                /* the Wimp fills in my_ref */
        if (in->r[0] == 17 && b[4] == 4 && b[3] == 99) acks++;
        sent_action = b[4]; sent_code = in->r[0]; sent_to = in->r[2]; sent_my_ref = b[2]; sent_your_ref = b[3];
        if (b[4] == 0x10) { sent_win = b[5]; sent_flags = b[9]; }
        if (b[4] == 2) snprintf(scrap, sizeof(scrap), "%s", (const char *)&b[11]);
        return NULL;
    case 0x400D3:                                                         /* GetCaretPosition */
        b[0] = caret_win; b[1] = caret_win == URLW ? 1 : -1; b[5] = -1;
        return NULL;
    case 0x400DD: final_checks(); return NULL;                            /* CloseDown */
    case 0x42: out->r[0] = (int)(fake_time * 100); return NULL;           /* OS_ReadMonotonicTime */
    case 0x65: return &err;                                               /* OS_ScreenMode: TBGR anyway */
    case 0x50B00:                                                         /* MimeMap_Translate */
        if (in->r[0] == 2) {                                              /* MIME type to filetype */
            out->r[3] = !strcmp((const char *)(intptr_t)in->r[1], "application/json") ? 0xF79 : 0xFFF;
            return NULL;
        }
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
        last_poll_idle = swi == 0x400E1;
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
    base_url = getenv("REEL_TEST_URL");
    (void)title;
    strcpy(choices_dir, "/tmp/reelchoicesXXXXXX");                        /* a fresh Choices directory */
    if (!mkdtemp(choices_dir))
        return 1;
    setenv("Reel$ChoicesDir", choices_dir, 1);
    setenv("ReelEGL$ChoicesDir", choices_dir, 1);
    reel_main(1, args);
    return 1;
}
