/*
 * reelcore's deinterlacing (yadif; NEON with patch 0015):
 *   Auto on an interlaced clip (H.264 MBAFF, marked interlaced): pictures
 *     go through yadif, and the one shown matches x86 FFmpeg's
 *     "-vf yadif=0:-1:1" at the same frame (to within swscale rounding);
 *   Off: the picture matches the plain decode, and differs from Auto's;
 *   seeking with yadif holding a picture, then carrying on;
 *   Auto on a progressive clip: yadif isn't used at all; On: it is.
 *
 *   deint_test INTERLACED_CLIP DEINT.yuv PLAIN.yuv PROGRESSIVE_CLIP
 *   (the .yuv files: yuv420p frames of the clip's size, made by run.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <libswscale/swscale.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int play(ReelCore *v, double secs)
{
    int n = 0;
    for (double end = fake_time + secs; fake_time < end; fake_time += 0.01)
        if (reelcore_update(v) == REELCORE_NEW_FRAME)
            n++;
    return n;
}

/* the share of pixels of the picture shown that are within 4 (each of R,
   G, B) of frame k of a yuv420p file */
static double match(ReelCore *v, const char *yuv, int k)
{
    int w, h, same = 0;
    size_t fs;
    uint8_t *f, *rgb, *pic;
    FILE *fp;
    struct SwsContext *sws;
    reelcore_frame_size(v, &w, &h);
    fs = (size_t)w * h * 3 / 2;
    f = malloc(fs);
    rgb = malloc((size_t)w * h * 4);
    pic = malloc((size_t)w * h * 4);
    fp = fopen(yuv, "rb");
    if (!fp || fseek(fp, (long)(fs * k), SEEK_SET) || fread(f, 1, fs, fp) != fs) {
        printf("FAIL: no frame %d in %s\n", k, yuv);
        fails++;
        if (fp) fclose(fp);
        return 0;
    }
    fclose(fp);
    {
        const uint8_t *src[3] = { f, f + w * h, f + w * h + (w / 2) * (h / 2) };
        int ss[3] = { w, w / 2, w / 2 };
        uint8_t *dst[1] = { rgb };
        int ds[1] = { w * 4 };
        sws = sws_getContext(w, h, AV_PIX_FMT_YUV420P, w, h, AV_PIX_FMT_RGB0, SWS_BILINEAR, NULL, NULL, NULL);
        sws_scale(sws, src, ss, 0, h, dst, ds);
        sws_freeContext(sws);
    }
    reelcore_draw_pixels(v, pic, w * 4, w, h, 0, REELCORE_STRETCH);
    for (int i = 0; i < w * h; i++) {
        int ok = 1;
        for (int c = 0; c < 3; c++)
            ok &= abs(rgb[i * 4 + c] - pic[i * 4 + c]) <= 4;
        same += ok;
    }
    free(f); free(rgb); free(pic);
    return same / (double)(w * h);
}

static int frame_index(ReelCore *v)
{
    return (int)lrint(reelcore_position(v) * reelcore_frame_rate(v));
}

int main(int argc, char **argv)
{
    ReelCoreStats st;
    ReelCore *v;
    double m_deint, m_plain, off_plain, off_deint;
    int k;

    /* Auto, interlaced */
    v = reelcore_open(argv[1], 0);
    CHECK(v && reelcore_deinterlace(v) == REELCORE_DEINT_AUTO, "not Auto to start with");
    if (!v) return 1;
    play(v, 1.0);
    reelcore_pause(v, 1);
    reelcore_stats(v, &st);
    k = frame_index(v);
    m_deint = match(v, argv[2], k);
    m_plain = match(v, argv[3], k);
    printf("  auto: %u of %u pictures interlaced, %u deinterlaced; frame %d: %.1f%% as x86 yadif, %.1f%% as plain\n",
           st.interlaced, st.decoded, st.deinterlaced, k, m_deint * 100, m_plain * 100);
    CHECK(st.interlaced > 10 && st.deinterlaced > 10, "auto: %u interlaced, %u deinterlaced", st.interlaced, st.deinterlaced);
    CHECK(m_deint > 0.99, "auto: only %.1f%% like x86 yadif at frame %d", m_deint * 100, k);
    CHECK(m_plain < 0.95, "auto: %.1f%% like the plain decode (not deinterlaced?)", m_plain * 100);

    /* seek, with yadif holding a picture; then on */
    reelcore_pause(v, 0);
    reelcore_seek(v, 0.4);
    play(v, 0.5);
    reelcore_pause(v, 1);
    k = frame_index(v);
    m_deint = match(v, argv[2], k);
    CHECK(reelcore_position(v) > 0.6 && m_deint > 0.99, "after a seek: at %.2f, %.1f%% like x86 yadif", reelcore_position(v), m_deint * 100);

    /* Off */
    reelcore_set_deinterlace(v, REELCORE_DEINT_OFF);
    reelcore_pause(v, 0);
    play(v, 0.3);
    reelcore_pause(v, 1);
    k = frame_index(v);
    off_plain = match(v, argv[3], k);
    off_deint = match(v, argv[2], k);
    printf("  off: frame %d: %.1f%% as plain, %.1f%% as yadif\n", k, off_plain * 100, off_deint * 100);
    CHECK(off_plain > 0.99 && off_deint < 0.95, "off: %.1f%% plain, %.1f%% yadif", off_plain * 100, off_deint * 100);
    reelcore_close(v);

    /* progressive: Auto leaves it alone; On deinterlaces anyway */
    v = reelcore_open(argv[4], 0);
    play(v, 1.0);
    reelcore_stats(v, &st);
    CHECK(st.interlaced == 0 && st.deinterlaced == 0, "progressive, auto: %u interlaced, %u deinterlaced", st.interlaced, st.deinterlaced);
    reelcore_set_deinterlace(v, REELCORE_DEINT_ON);
    play(v, 1.0);
    reelcore_stats(v, &st);
    CHECK(st.deinterlaced > 10 && st.deinterlace == REELCORE_DEINT_ON, "progressive, on: %u deinterlaced", st.deinterlaced);
    printf("  progressive: auto left it alone; on: %u pictures through yadif\n", st.deinterlaced);
    reelcore_close(v);

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
