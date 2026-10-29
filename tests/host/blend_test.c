/*
 * reelcore's subtitle and stats panel blending, made quicker with NEON
 * (reelcore_blend_rgb_run: into 32bpp, Reel's sprite and ReelEGL;
 * reelcore_blend_cols: a scaled layer into a plane, a DVD/Blu-ray/DVB
 * subtitle's picture over the overlay), the layer's columns worked out once
 * a placing, and its colours worked out only for new pixels.
 *
 *   blend_test
 *     the two NEON runs against the plain C, every length to 40 and 1920,
 *     scaled and not, R,G,B and B,G,R, clear, solid and in-between alpha;
 *   blend_test --frames VIDEO SUBS [SUBS...]
 *     plays VIDEO with each subtitle file and the stats panel, drawing every
 *     picture into 32bpp (the frame's size, smaller, bigger) and into 4:2:0
 *     (the frame's size, as for an overlay the display halves, and the
 *     frame halved: picture subtitles scaled into each plane), and
 *     prints one MD5 of all of it. run.sh builds it twice, with reelcore as
 *     it is and with -DREELCORE_PLAIN_LAYERS (the plain C throughout), and
 *     the two must print the same.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

/* (weak: the frames can be drawn by a reelcore from before them, to compare) */
void reelcore_blend_rgb_run(uint8_t *d, const uint8_t *row, const int *lx, int n, int bgr) __attribute__((weak));
void reelcore_blend_cols(uint8_t *d, const uint16_t *pm, const uint8_t *ia, const int *lx, int n) __attribute__((weak));

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 10) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

#define DIV255(m) (((unsigned)(m) + 1 + ((unsigned)(m) >> 8)) >> 8)

static uint32_t seed = 12345;
static unsigned rnd(void) { seed = seed * 1103515245 + 12345; return seed >> 8; }

/* an alpha as subtitles have them: mostly clear or solid */
static int rnd_alpha(void)
{
    unsigned k = rnd() % 8;
    return k < 3 ? 0 : k < 6 ? 255 : (int)(rnd() % 256);
}

static void ref_rgb(uint8_t *d, const uint8_t *row, const int *lx, int n, int bgr)
{
    for (int i = 0; i < n; i++, d += 4) {
        const uint8_t *s = row + (lx ? lx[i] : i) * 4;
        int a;
        if (!s[3])
            continue;
        a = 255 - s[3];
        d[0] = (uint8_t)(s[bgr ? 2 : 0] + DIV255(d[0] * a));
        d[1] = (uint8_t)(s[1] + DIV255(d[1] * a));
        d[2] = (uint8_t)(s[bgr ? 0 : 2] + DIV255(d[2] * a));
    }
}

static void units(void)
{
    enum { LW = 2000, N = 1920 };
    static uint8_t row[LW * 4], d1[N * 4 + 16], d2[N * 4 + 16], ia[LW], p1[N + 16], p2[N + 16];
    static uint16_t pm[LW];
    static int lx[N];
    int cases = 0;
    for (int len = 0; len <= N; len = len < 40 ? len + 1 : len == 40 ? N - 7 : len + 1) {
        for (int mode = 0; mode < 4; mode++) {
            int bgr = mode & 1, scaled = mode & 2;
            for (int i = 0; i < LW; i++) {      /* premultiplied (each colour at most its alpha), mostly */
                int a = rnd_alpha();
                row[i * 4 + 3] = (uint8_t)a;
                for (int c = 0; c < 3; c++)     /* (clear pixels with colour: still left alone) */
                    row[i * 4 + c] = (uint8_t)(a ? rnd() % (a + 1) : rnd() % 4 ? 0 : rnd() % 256);
                ia[i] = (uint8_t)(255 - a);
                pm[i] = (uint16_t)((rnd() % 256) * a);   /* (Y, Cb or Cr times alpha) */
            }
            for (int i = 0; i < len; i++)       /* up or down, as layer_columns gives them */
                lx[i] = scaled ? (int)((int64_t)i * (mode & 1 ? 46000 : 98000) >> 16) % LW : i;
            for (int i = 0; i < (int)sizeof(d1); i++)
                d1[i] = d2[i] = (uint8_t)rnd();
            for (int i = 0; i < (int)sizeof(p1); i++)
                p1[i] = p2[i] = (uint8_t)rnd();
            reelcore_blend_rgb_run(d1, row, scaled ? lx : NULL, len, bgr);
            ref_rgb(d2, row, scaled ? lx : NULL, len, bgr);
            CHECK(!memcmp(d1, d2, sizeof(d1)), "into 32bpp, %d pixels, %s, %s: not the C's", len,
                  scaled ? "scaled" : "1:1", bgr ? "B,G,R" : "R,G,B");
            reelcore_blend_cols(p1, pm, ia, lx, len);
            for (int i = 0; i < len; i++)
                p2[i] = (uint8_t)DIV255(pm[lx[i]] + p2[i] * ia[lx[i]]);
            CHECK(!memcmp(p1, p2, sizeof(p1)), "into a plane, %d pixels, %s: not the C's", len,
                  scaled ? "scaled" : "in turn");
            cases++;
        }
    }
    printf("  NEON blending against the C: %d cases\n", cases);
}

static int frames(int argc, char **argv)
{
    struct AVMD5 *md5 = av_md5_alloc();
    uint8_t sum[16];
    int pictures = 0;
    av_md5_init(md5);
    for (int f = 2; f < argc; f++) {
        ReelCore *v = reelcore_open(argv[1], REELCORE_NO_AUDIO);
        ReelCorePanel pan;
        const char *label[3] = { "Position", "Pictures shown", "Network" };
        const char *value[3] = { "0:01 of 0:20 (1.04 s)", "25.0 a second (the video: 25)", "3.1 s ahead (238 KB)" };
        static const float g[20] = { 0.1f, 0.5f, 0.9f, 0.3f, 1, 0, 0.7f, 0.2f, 0.4f, 0.6f,
                                     0.8f, 0.1f, 0.5f, 0.9f, 0.3f, 1, 0, 0.7f, 0.2f, 0.4f };
        int r = 0, fw = 0, fh = 0;
        if (!v || reelcore_add_subtitle_file(v, argv[f]) < 0) {
            printf("FAIL: can't open %s with %s\n", argv[1], argv[f]);
            return 1;
        }
        reelcore_set_subtitle_track(v, reelcore_subtitle_tracks(v) - 1);
        memset(&pan, 0, sizeof(pan));
        pan.rows = 3;
        for (int i = 0; i < 3; i++) {
            pan.label[i] = label[i];
            pan.value[i] = value[i];
        }
        pan.graph[2] = g;
        pan.graph_rgb[2] = 0xFFB300;
        pan.graph_n = 20;
        for (int i = 0; i < 4000 && r != REELCORE_END; i++) {
            r = reelcore_update(v);
            fake_time += 0.02;
            if (r != REELCORE_NEW_FRAME)
                continue;
            pan.yuv_scale = pictures & 1 ? 2 : 1;
            reelcore_set_panel(v, (pictures % 50) < 25 ? &pan : NULL);   /* (on and off) */
            reelcore_frame_size(v, &fw, &fh);
            {   /* 32bpp: the frame's size, smaller and bigger (bitmap subtitles scaled each way) */
                static const int sz[3][2] = { { 0, 0 }, { 200, 112 }, { 640, 360 } };
                for (int k = 0; k < 3; k++) {
                    int w = sz[k][0] ? sz[k][0] : fw, h = sz[k][1] ? sz[k][1] : fh;
                    uint8_t *b = calloc((size_t)w * h, 4);
                    reelcore_draw_pixels(v, b, w * 4, w, h, k == 1, k ? 0 : REELCORE_STRETCH);
                    av_md5_update(md5, b, w * h * 4);
                    free(b);
                }
            }
            for (int k = 1; k <= 3; k++) {      /* 4:2:0, as an overlay the display halves, and halved */
                int w = (k == 3 ? fw / 2 : fw) & ~1, h = (k == 3 ? fh / 2 : fh) & ~1, pitch[3] = { w, w / 2, w / 2 };
                uint8_t *b = calloc((size_t)w * h * 3 / 2, 1), *planes[3] = { b, b + w * h, b + w * h + w * h / 4 };
                reelcore_set_yuv_scale(v, k == 2 ? 2 : 1);
                reelcore_draw_yuv420(v, planes, pitch, w, h, NULL);
                av_md5_update(md5, b, w * h * 3 / 2);
                free(b);
            }
            pictures++;
        }
        reelcore_close(v);
    }
    av_md5_final(md5, sum);
    av_free(md5);
    printf("  %d pictures drawn with subtitles and the panel, MD5 ", pictures);
    for (int i = 0; i < 16; i++)
        printf("%02x", sum[i]);
    printf("\n");
    return pictures > 50 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--frames"))
        return frames(argc - 1, argv + 1);
    if (!reelcore_blend_rgb_run || !reelcore_blend_cols) {
        printf("FAIL: no reelcore_blend_rgb_run / reelcore_blend_cols\n");
        return 1;
    }
    units();
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
