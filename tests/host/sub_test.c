/*
 * Subtitles, chapters, turned pictures and frame steps (reelcore):
 *   - an MKV with a SubRip track marked default: shown from the start; the
 *     text at 1.5 s ("Hello there": ASS italics dropped) and 3.5 s (UTF-8
 *     quotes, dash and accents in Latin-1, two lines); none at 2.7 s;
 *     drawn into the picture near the bottom (white text, black outline),
 *     nothing else changed; the same in YV12 at 1x and 2x; off again: the
 *     picture as without;
 *   - its chapters: three, titles ("Chapter 3" for the one without), starts,
 *     which one a position is in;
 *   - an MP4 without subtitles plus the .srt file added: the same text;
 *   - an MP4 whose display matrix says turn it 90 degrees: the size and the
 *     frames are turned (320x180 -> 180x320), and Media info says so;
 *   - paused: a step forward is the next picture, a step back the one
 *     before (and playing again carries on from there).
 *
 *   - an ASS file: override blocks ({\b1}) dropped, \N a new line, \h a space,
 *     commas in the text kept.
 *
 *   sub_test SUBS.mkv PLAIN.mp4 SUBS.srt ROT.mp4 SUBS.ass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Plays (with the fake timer) until the picture is at t or after */
static void play_to(ReelCore *v, double t)
{
    for (int i = 0; i < 4000 && reelcore_position(v) < t; i++) {
        reelcore_update(v);
        fake_time += 0.005;
    }
}

/* After a seek: its first picture */
static void seeked(ReelCore *v)
{
    for (int i = 0; i < 400 && reelcore_update(v) != REELCORE_NEW_FRAME; i++)
        fake_time += 0.005;
}

static void text_at(ReelCore *v, double t, char *buf, int size)
{
    play_to(v, t);
    reelcore_subtitle_text(v, buf, size);
}

/* The picture's pixels drawn with the subtitles on, and off */
static void compare(ReelCore *v, const char *what, int want_text)
{
    int W = 640, H = 360, top = 0, bottom = 0, white = 0, black = 0;
    unsigned char *a = malloc(W * H * 4), *b = malloc(W * H * 4);
    reelcore_draw_pixels(v, a, W * 4, W, H, 0, REELCORE_STRETCH);
    reelcore_show_subtitles(v, 0);
    reelcore_draw_pixels(v, b, W * 4, W, H, 0, REELCORE_STRETCH);
    reelcore_show_subtitles(v, 1);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const unsigned char *p = a + (y * W + x) * 4, *q = b + (y * W + x) * 4;
            int d = memcmp(p, q, 3) != 0;
            if (y < H * 2 / 3) top += d; else bottom += d;
            if (d && p[0] > 230 && p[1] > 230 && p[2] > 230) white++;
            if (d && p[0] < 25 && p[1] < 25 && p[2] < 25) black++;
        }
    printf("  %s: %d pixels changed near the bottom (%d white, %d black), %d above\n", what, bottom, white, black, top);
    CHECK(top == 0, "%s: %d pixels changed above the bottom third", what, top);
    if (want_text)
        CHECK(white > 200 && black > 200, "%s: no white text with a black outline (%d, %d)", what, white, black);
    else
        CHECK(bottom == 0, "%s: %d pixels changed with no subtitle", what, bottom);
    free(a);
    free(b);
}

static void yv12(ReelCore *v, int k)
{
    int fw, fh, on = 0, off = 0, bright = 0;
    unsigned char *a, *b;
    reelcore_frame_size(v, &fw, &fh);
    fw &= ~1; fh &= ~1;
    a = malloc(fw * fh * 3 / 2); b = malloc(fw * fh * 3 / 2);
    {
        unsigned char *pa[3] = { a, a + fw * fh, a + fw * fh * 5 / 4 }, *pb[3] = { b, b + fw * fh, b + fw * fh * 5 / 4 };
        int pitch[3] = { fw, fw / 2, fw / 2 };
        reelcore_set_yuv_scale(v, 1.0 / k);          /* shown k times bigger: text 1/k the size in the frame */
        reelcore_draw_yuv420(v, pa, pitch, fw, fh, NULL);
        reelcore_show_subtitles(v, 0);
        reelcore_draw_yuv420(v, pb, pitch, fw, fh, NULL);
        reelcore_show_subtitles(v, 1);
    }
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            int d = a[y * fw + x] != b[y * fw + x];
            if (y < fh / 2) off += d; else on += d;
            bright += d && a[y * fw + x] > 200;
        }
    printf("  YV12, shown %dx: %d changed in the lower half, %d above, %d bright\n", k, on, off, bright);
    CHECK(off == 0 && on > 100 && bright > 20, "YV12 %dx: subtitles not drawn right (%d, %d, %d)", k, on, off, bright);
    free(a); free(b);
}

int main(int argc, char **argv)
{
    char t[256];
    ReelCore *v;
    setenv("FAKE_AUDIO_BLOCK", "2048", 1);

    /* 1. the MKV: its own subtitles, and chapters */
    v = reelcore_open(argv[1], 0);
    CHECK(v != NULL, "can't open %s", argv[1]);
    if (!v) return 1;
    CHECK(reelcore_subtitle_tracks(v) == 1 && reelcore_subtitle_track(v) == 0, "tracks %d, shown %d (want 1, 0: default)",
          reelcore_subtitle_tracks(v), reelcore_subtitle_track(v));
    reelcore_subtitle_track_name(v, 0, t, sizeof(t));
    CHECK(!strcmp(t, "eng (subrip)"), "track name '%s'", t);
    text_at(v, 1.5, t, sizeof(t));
    CHECK(!strcmp(t, "Hello there"), "at 1.5 s: '%s'", t);
    compare(v, "1.5 s", 1);
    text_at(v, 2.7, t, sizeof(t));
    CHECK(!t[0], "at 2.7 s: '%s' (want none)", t);
    compare(v, "2.7 s", 0);
    text_at(v, 3.5, t, sizeof(t));
    CHECK(!strcmp(t, "\"Quoted\" - it's caf\xe9\nsecond line"), "at 3.5 s: '%s'", t);
    compare(v, "3.5 s, two lines", 1);
    yv12(v, 1);
    yv12(v, 2);
    reelcore_set_yuv_scale(v, 1);
    CHECK(reelcore_chapters(v) == 3, "%d chapters (want 3)", reelcore_chapters(v));
    reelcore_chapter_title(v, 1, t, sizeof(t));
    CHECK(!strcmp(t, "Middle") && fabs(reelcore_chapter_start(v, 1) - 2) < 0.05, "chapter 2: '%s' at %.2f", t,
          reelcore_chapter_start(v, 1));
    reelcore_chapter_title(v, 2, t, sizeof(t));
    CHECK(!strcmp(t, "Chapter 3") && reelcore_chapter_at(v, 4.5) == 2 && reelcore_chapter_at(v, 1.0) == 0,
          "chapter 3 '%s', at 4.5 s: %d", t, reelcore_chapter_at(v, 4.5));
    {   /* seeking back: the text isn't doubled, and shows again */
        reelcore_seek(v, 1.0);
        seeked(v);
        text_at(v, 1.5, t, sizeof(t));
        CHECK(!strcmp(t, "Hello there"), "after a seek back, at 1.5 s: '%s'", t);
    }
    {   /* choosing the track again: read again from the picture shown */
        reelcore_set_subtitle_track(v, -1);
        reelcore_subtitle_text(v, t, sizeof(t));
        CHECK(!t[0] && reelcore_subtitle_track(v) == -1, "none: '%s'", t);
        reelcore_seek(v, 3.2);
        seeked(v);
        play_to(v, 3.3);
        reelcore_set_subtitle_track(v, 0);
        seeked(v);
        text_at(v, 3.6, t, sizeof(t));
        CHECK(!strncmp(t, "\"Quoted\"", 8), "track chosen again at 3.3 s, at 3.6 s: '%s'", t);
    }
    reelcore_close(v);

    /* 2. an MP4 and the .srt beside it */
    v = reelcore_open(argv[2], 0);
    CHECK(v && reelcore_subtitle_tracks(v) == 0 && reelcore_subtitle_track(v) == -1, "plain MP4: subtitle tracks");
    CHECK(reelcore_add_subtitle_file(v, argv[3]) == 0 && reelcore_subtitle_track(v) == 0, "adding %s", argv[3]);
    reelcore_subtitle_track_name(v, 0, t, sizeof(t));
    printf("  added: %s\n", t);
    CHECK(!strcmp(t, "File: subs.srt"), "added track's name '%s'", t);
    text_at(v, 1.5, t, sizeof(t));
    CHECK(!strcmp(t, "Hello there"), "file, at 1.5 s: '%s'", t);
    compare(v, "file at 1.5 s", 1);
    CHECK(reelcore_add_subtitle_file(v, "no/such/file.srt") < 0 && reelcore_subtitle_tracks(v) == 1,
          "a missing file isn't added");
    CHECK(reelcore_add_subtitle_file(v, argv[5]) == 1, "adding %s", argv[5]);
    reelcore_seek(v, 1.2);
    seeked(v);
    text_at(v, 1.5, t, sizeof(t));
    CHECK(!strcmp(t, "Bold words,\nwith a comma and space"), "ASS at 1.5 s: '%s'", t);
    reelcore_set_subtitle_track(v, 0);

    /* 3. frame steps */
    reelcore_pause(v, 1);
    {
        double p0 = reelcore_position(v), p1, p2, p3, p4;
        CHECK(reelcore_step(v) == REELCORE_NEW_FRAME, "step: no new picture");
        p1 = reelcore_position(v);
        CHECK(fabs(p1 - p0 - 0.04) < 0.005, "step: %.3f -> %.3f (want +0.04)", p0, p1);
        /* back to the one just shown: kept, at once */
        CHECK(reelcore_step_back(v) == REELCORE_NEW_FRAME && fabs(reelcore_position(v) - p0) < 0.005,
              "step back after a step: %.3f (want %.3f at once)", reelcore_position(v), p0);
        /* back again: nothing kept before it, so a seek and decoding from the key frame */
        CHECK(reelcore_step_back(v) == 0, "step back with nothing kept: not a seek");
        seeked(v);
        p2 = reelcore_position(v);
        CHECK(fabs(p2 - (p0 - 0.04)) < 0.005 && reelcore_paused(v), "step back: %.3f (want %.3f, paused)", p2, p0 - 0.04);
        /* and again: the pictures before were kept on the way, so at once */
        CHECK(reelcore_step_back(v) == REELCORE_NEW_FRAME, "a step back after the seek wasn't at once");
        p3 = reelcore_position(v);
        CHECK(fabs(p3 - (p0 - 0.08)) < 0.005, "kept step back: %.3f (want %.3f)", p3, p0 - 0.08);
        CHECK(reelcore_step(v) == REELCORE_NEW_FRAME && fabs(reelcore_position(v) - p2) < 0.005,
              "forward: %.3f (want %.3f)", reelcore_position(v), p2);
        CHECK(reelcore_step(v) == REELCORE_NEW_FRAME && fabs(reelcore_position(v) - p0) < 0.005,
              "forward: %.3f (want %.3f)", reelcore_position(v), p0);
        p4 = reelcore_position(v);
        printf("  steps: %.3f -> %.3f -> %.3f (kept) -> %.3f (sought) -> %.3f (kept) -> %.3f -> %.3f\n",
               p0, p1, p0, p2, p3, p2, p4);
        reelcore_pause(v, 0);
        play_to(v, p4 + 0.5);
        CHECK(reelcore_position(v) >= p4 + 0.5 && reelcore_position(v) < p4 + 0.7, "playing on after steps: %.3f",
              reelcore_position(v));
    }
    reelcore_close(v);

    /* 4. turned: a 320x180 video the file says to show upright as 180x320 */
    v = reelcore_open(argv[4], 0);
    CHECK(v != NULL, "can't open %s", argv[4]);
    if (v) {
        int fw = 0, fh = 0;
        char info[4096];
        play_to(v, 0.3);
        reelcore_frame_size(v, &fw, &fh);
        reelcore_media_info(v, info, sizeof(info));
        printf("  turned: %dx%d, frames %dx%d\n", reelcore_width(v), reelcore_height(v), fw, fh);
        CHECK(reelcore_width(v) == 180 && reelcore_height(v) == 320 && fw == 180 && fh == 320,
              "turned: %dx%d, frames %dx%d (want 180x320)", reelcore_width(v), reelcore_height(v), fw, fh);
        CHECK(strstr(info, "Turned\t90 degrees anticlockwise") != NULL, "media info: no Turned line");
        reelcore_close(v);
    }
    v = reelcore_open(argv[4], REELCORE_NO_ROTATE);
    if (v) {
        CHECK(reelcore_width(v) == 320, "REELCORE_NO_ROTATE: width %d", reelcore_width(v));
        reelcore_close(v);
    }
    /* 5. a step back from 8 s in a clip with its key frame at 0 and
       B-frames: the B-frames more than half a second before aren't decoded */
    v = reelcore_open(argv[6], 0);
    CHECK(v != NULL, "can't open %s", argv[6]);
    if (v) {
        ReelCoreStats a, b;
        reelcore_seek(v, 8.0);
        seeked(v);
        reelcore_pause(v, 1);
        reelcore_stats(v, &a);
        CHECK(reelcore_step_back(v) == 0, "step back at 8 s");
        seeked(v);
        reelcore_stats(v, &b);
        printf("  step back at 8 s (key frame at 0, B-frames): %u pictures decoded, now at %.3f\n",
               b.decoded - a.decoded, reelcore_position(v));
        CHECK(b.decoded - a.decoded < 120 && fabs(reelcore_position(v) - 7.96) < 0.005,
              "step back at 8 s: %u decoded (want under 120 of 200), at %.3f", b.decoded - a.decoded, reelcore_position(v));
        reelcore_close(v);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
