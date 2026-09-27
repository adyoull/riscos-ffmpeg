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
 * the window closes the video; Message_Quit quits.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static const char *clip1, *clip2;
static int fails, step;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define WIN 0x100
#define FULL 0x200
#define INFO 0x300                    /* the media info window */
#define SCR_W 1920                    /* pixels; eig 1 -> 3840 x 2160 OS units */
#define SCR_H 1080

static int created, opened_w, opened_h, win_x0, win_y1, full_open, win_open, nicons;
static int state[3][9];               /* window states: [0] WIN, [1] FULL, [2] INFO */
static int info_created, info_open, info_updates, info_texts;
static char info_seen[8192];          /* what Wimp_TextOp drew in the info window */
static int drawing_info;
static int plots, plots_full, clip_ok = 1, updates, acks, reports, keys_passed, last_mask;
static int caret_win;
static int icon_box[8][4];
static char title[64];
static int *title_ptr;

static int *st(int w) { return state[w == FULL ? 1 : w == INFO ? 2 : 0]; }

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
enum { P_PLAY1, P_INFO, P_INFOPLAY, P_INFOCLOSE, P_PAUSE, P_PAUSED, P_RESUME, P_PLAY2, P_SEEKBAR, P_PLAY3, P_FULL, P_PLAYFULL, P_UNFULL,
       P_RESIZE, P_PLAY4, P_DROP2, P_PLAY5, P_DIR, P_OPEN_OTHER, P_OPEN_VIDEO, P_PLAY6, P_CLOSE, P_QUIT };
static int phase = -1, phase_step;
/* the phases that play (deliver nulls) */
static int next_is_null(void)
{
    return phase == P_PLAY1 || phase == P_PLAY2 || phase == P_PLAY3 || phase == P_PLAYFULL || phase == P_PLAY4 ||
           phase == P_PLAY5 || phase == P_PLAY6 || phase == P_INFOPLAY;
}
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
            if (phase_step++ < 40) {
                fake_time += 0.02;
                return 0;                                             /* null */
            }
            {   /* the sprite holds the current frame, letterboxed, as ffegl draws it */
                int w, h, rows, bad = 0;
                const uint8_t *px = reel_test_sprite(&w, &h, &rows);
                uint8_t *want = malloc((size_t)w * h * 4);
                reelcore_draw_pixels(v, want, w * 4, w, h, 0, 0);
                for (int i = 0; px && i < w * h; i++)
                    bad += memcmp(px + i * 4, want + i * 4, 3) != 0;
                CHECK(px && !bad, "phase %d: sprite %dx%d, %d pixels differ", phase, w, h, bad);
                free(want);
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
            CHECK((strstr(info_seen, "25.0 a second") || strstr(info_seen, "24.8 a second") || strstr(info_seen, "24.9 a second")) && (strstr(info_seen, "SharedSoundBuffer: ") || strstr(info_seen, "SDL: ")) &&
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
                CHECK(fake_render_buffer == EGL_SINGLE_BUFFER && fake_surfaces == 1,
                      "full screen: render buffer 0x%x (want single: Direct), %d surfaces", fake_render_buffer, fake_surfaces);
#endif
                CHECK(state[1][3] - state[1][1] == SCR_W * 2 && state[1][4] - state[1][2] == SCR_H * 2,
                      "full screen window %dx%d", state[1][3] - state[1][1], state[1][4] - state[1][2]);
                CHECK(caret_win == FULL, "no caret in the full screen window");
                plots_full = PLOTS;
            }
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
        case P_DROP2:
            if (phase_step++ == 0) { message(b, 3, WIN, -1, 0xFFD, clip2); return 18; }
            {
                const char *leaf = strrchr(clip2, '/');
                CHECK(title_ptr && !strcmp((char *)title_ptr, leaf ? leaf + 1 : clip2), "title '%s'", title_ptr ? (char *)title_ptr : "");
                CHECK(v && reelcore_width(v) > 0, "second file not playing");
            }
            break;
        case P_DIR:
            if (phase_step++ == 0) { message(b, 3, -2, 1, 0x1000, "/tmp"); return 18; }
            CHECK(reports == 1, "directory: %d reports", reports);
            break;
        case P_OPEN_OTHER:
            if (phase_step++ == 0) { message(b, 5, 0, 0, 0xFFF, "/tmp/text"); return 17; }   /* DataOpen, text */
            CHECK(acks == 2, "text file claimed (%d acks)", acks);
            break;
        case P_OPEN_VIDEO:
            if (phase_step++ == 0) { message(b, 5, 0, 0, 0xBF8, clip1); return 17; }    /* DataOpen, MPEG */
            CHECK(acks == 3, "video not claimed (%d acks)", acks);
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
    CHECK(created == 1 && nicons == 7, "window: created %d, %d icons", created, nicons);
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
        if ((b[7] & 0x80000040) == 0x80000040 && !(b[7] & 0x04000000)) { out->r[0] = FULL; return NULL; }
        if (b[7] & 0x10000000) { info_created++; out->r[0] = INFO; return NULL; }      /* v scroll: info */
        created++; nicons = b[21];
        title_ptr = (int *)(intptr_t)b[18];
        out->r[0] = WIN;
        return NULL;
    case 0x400FC:                                                         /* ResizeIcon */
        if (in->r[0] == WIN && in->r[1] >= 0 && in->r[1] < 8)
            for (int i = 0; i < 4; i++) icon_box[in->r[1]][i] = in->r[2 + i];
        return NULL;
    case 0x400C5:                                                         /* OpenWindow */
        memcpy(st(b[0]), b, 32);
        st(b[0])[5] = st(b[0])[6] = 0;
        if (b[0] == WIN) { win_open = 1; opened_w = b[3] - b[1]; opened_h = b[4] - b[2]; }
        else if (b[0] == INFO) info_open = 1;
        else full_open = 1;
        return NULL;
    case 0x400CB: memcpy(b + 1, st(b[0]) + 1, 32); return NULL;           /* GetWindowState */
    case 0x400C6: if (b[0] == WIN) win_open = 0; else if (b[0] == INFO) info_open = 0; else full_open = 0; return NULL;   /* CloseWindow */
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
    case 0x400DF: reports++; return NULL;                                 /* ReportError */
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
        out->r[0] = (w == WIN && win_open) || (w == FULL && full_open) || (w == INFO && info_open);
        return NULL;
    }
    case 0x400CA: out->r[0] = 0; return NULL;                             /* GetRectangle: no more */
    case 0x46: {                                                          /* OS_WriteN: VDU 24 */
        const unsigned char *v = (const unsigned char *)(intptr_t)in->r[0];
        int y0 = (short)(v[3] | v[4] << 8), y1 = (short)(v[7] | v[8] << 8);
        int w = full_open ? FULL : WIN;
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
    reel_main(1, args);
    return 1;
}
