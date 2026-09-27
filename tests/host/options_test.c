/*
 * reelcore's playback options against the fake clock and fake SDL sound:
 *   speed (0.5x, 2x): the pictures follow a clock that runs at that speed,
 *     while the sound device is still fed in real time (atempo keeps the
 *     pitch, so a second of sound holds `speed` seconds of the file);
 *   fast decoding; choosing between two sound tracks;
 *   the picture modes: fill (cover, cropped), original size (1:1, cropped
 *     or with bars), and that a crop takes the right pixels (to within swscale rounding).
 *
 *   options_test CLIP TWO_TRACK_CLIP   (6 s and 4 s clips with sound)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;

/* the same colour, give or take swscale's rounding (a different width can
   take a different NEON path) */
static int close_rgb(uint32_t a, uint32_t b)
{
    for (int i = 0; i < 24; i += 8)
        if (abs((int)((a >> i) & 255) - (int)((b >> i) & 255)) > 4)
            return 0;
    return 1;
}
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* plays for `secs` of fake time in 10 ms steps; returns pictures shown */
static int play(ReelCore *v, double secs)
{
    int n = 0;
    for (double end = fake_time + secs; fake_time < end; fake_time += 0.01)
        if (reelcore_update(v) == REELCORE_NEW_FRAME)
            n++;
    return n;
}

static void speed_check(const char *clip, double speed)
{
    ReelCore *v = reelcore_open(clip, 0);
    double p0, q0, p1, q1;
    int n;
    CHECK(v != NULL, "open %s", clip);
    if (!v) return;
    play(v, 0.5);
    reelcore_set_speed(v, speed);
    CHECK(reelcore_speed(v) == speed, "speed %.2f not set", speed);
    play(v, 0.5);                                   /* settle after the restart */
    p0 = reelcore_position(v);
    q0 = fake_queued_total;
    n = play(v, 2.0);
    p1 = reelcore_position(v);
    q1 = fake_queued_total;
    printf("  speed %.1fx: 2 s played %.2f s of the file, %d pictures, %.2f s of sound given to the device\n",
           speed, p1 - p0, n, (q1 - q0) / (48000 * 4.0));
    CHECK(fabs((p1 - p0) - 2 * speed) < 0.25, "speed %.1f: %.2f s of the file in 2 s", speed, p1 - p0);
    CHECK(fabs((q1 - q0) / (48000 * 4.0) - 2.0) < 0.35, "speed %.1f: %.2f s of sound in 2 s (want real time)",
          speed, (q1 - q0) / (48000 * 4.0));
    CHECK(n >= (int)(2 * 25 * speed * 0.8) || speed > 1, "speed %.1f: only %d pictures", speed, n);
    reelcore_close(v);
}

int main(int argc, char **argv)
{
    ReelCore *v;
    char name[80];
    int w, h;

    /* speed */
    speed_check(argv[1], 2.0);
    speed_check(argv[1], 0.5);

    /* fast decoding */
    v = reelcore_open(argv[1], 0);
    reelcore_set_fast(v, 1);
    {
        int n = play(v, 1.0);
        ReelCoreStats st;
        reelcore_stats(v, &st);
        CHECK(st.fast == 1 && n > 15, "fast decoding: %d pictures, fast %d", n, st.fast);
    }
    reelcore_set_fast(v, 0);
    CHECK(!reelcore_fast(v), "fast decoding still on");

    /* picture modes: fill covers, original is 1:1, a crop takes the right pixels */
    {
        int dw = reelcore_width(v), dh = reelcore_height(v), bars = 0, left = 0, bad = 0;
        uint32_t *full = malloc((size_t)dw * dh * 4), *half, *big;
        uint32_t *wide = malloc(800 * 200 * 4);
        for (int i = 0; i < 800 * 200; i++) wide[i] = 0x5A5A5A5A;
        reelcore_draw_pixels(v, wide, 800 * 4, 800, 200, 0, REELCORE_FILL);
        for (int i = 0; i < 800 * 200; i++) left += wide[i] == 0x5A5A5A5A;
        for (int x = 0; x < 800; x++) bars += (wide[x] & 0xFFFFFF) == 0;   /* a black top row = bars */
        CHECK(left == 0 && bars < 800, "fill: %d pixels not drawn, top row %d black", left, bars);

        big = malloc((size_t)(dw + 100) * (dh + 60) * 4);
        for (int i = 0; i < (dw + 100) * (dh + 60); i++) big[i] = 0x5A5A5A5A;
        reelcore_draw_pixels(v, big, (dw + 100) * 4, dw + 100, dh + 60, 0, REELCORE_ORIGINAL | REELCORE_NO_BORDERS);
        left = 0;
        for (int i = 0; i < (dw + 100) * (dh + 60); i++) left += big[i] != 0x5A5A5A5A;
        CHECK(left == dw * dh, "original size in a bigger rectangle: %d pixels drawn, want %dx%d", left, dw, dh);

        reelcore_frame_size(v, &w, &h);
        reelcore_draw_pixels(v, full, dw * 4, dw, dh, 0, REELCORE_ORIGINAL);
        half = malloc(160 * dh * 4);
        reelcore_draw_pixels(v, half, 160 * 4, 160, dh, 0, REELCORE_ORIGINAL);
        {
            int cx = ((w - 160) / 2) & ~1;          /* the crop starts on a chroma sample */
            for (int y = 0; y < dh; y++)
                for (int x = 0; x < 160; x++)
                    bad += !close_rgb(half[y * 160 + x], full[y * dw + cx + x]);
            CHECK(bad == 0 && w == dw, "original size cropped: %d pixels differ from the full picture at x %d", bad, cx);
        }
        printf("  picture modes: fill covers 800x200, original size 1:1 (%dx%d), a 160-wide crop matches\n", dw, dh);
        free(full); free(half); free(big); free(wide);
    }
    reelcore_close(v);

    /* sound tracks */
    v = reelcore_open(argv[2], 0);
    CHECK(v && reelcore_audio_tracks(v) == 2, "two sound tracks: %d", v ? reelcore_audio_tracks(v) : -1);
    if (v) {
        double p;
        reelcore_audio_track_name(v, 1, name, sizeof(name));
        CHECK(reelcore_audio_track(v) == 0 && strstr(name, "fra") != NULL, "tracks: now %d, second '%s'",
              reelcore_audio_track(v), name);
        play(v, 1.0);
        p = reelcore_position(v);
        CHECK(reelcore_set_audio_track(v, 1) == 0 && reelcore_audio_track(v) == 1, "track 2 not chosen");
        play(v, 1.0);
        CHECK(reelcore_has_audio(v) && fabs(reelcore_position(v) - p - 1.0) < 0.3,
              "after changing track: sound %d, %.2f s on from %.2f", reelcore_has_audio(v), reelcore_position(v), p);
        printf("  sound tracks: 2, changed to '%s' at %.2f s\n", name, p);
        reelcore_close(v);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
