/*
 * reelcore's stats panel (reelcore_set_panel), drawn into the picture:
 *   - reelcore_draw_pixels: the panel's rows at the top left (text white,
 *     the background darker), everything else exactly as without it, in
 *     both byte orders;
 *   - reelcore_draw_yuv420 (for a hardware overlay): the same in Y/Cb/Cr,
 *     at yuv_scale 1 and 2 (twice the size in frame pixels);
 *   - switched off again: exactly the picture without it.
 *
 *   panel_test CLIP
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const float graph[60] = { 0.1f, 0.5f, 1.0f, 0.3f, 0.7f };

static const char *speed = "1234 Kbps", *date = "Tue Sep 29 2026 12:00:00 \xa3";
static const float *g1 = graph;

void reelcore_blend_run(uint8_t *d, const uint16_t *pm, const uint8_t *ia, int n, int k);   /* (reelcore.c) */

static void set(ReelCore *v, double k)
{
    ReelCorePanel p;
    memset(&p, 0, sizeof(p));
    p.rows = 3;
    p.label[0] = "Video / Source"; p.value[0] = "clip.mp4 / mov,mp4";
    p.label[1] = "Connection Speed"; p.value[1] = speed; p.graph[1] = g1; p.graph_rgb[1] = 0x1E88E5;
    p.label[2] = "Date"; p.value[2] = date;   /* a Latin-1 character too */
    p.graph_n = 60;
    p.yuv_scale = k;
    CHECK(reelcore_set_panel(v, &p) == 0, "set_panel failed");
}

int main(int argc, char **argv)
{
    ReelCore *v = reelcore_open(argv[1], 0);
    int W = 640, H = 360, pw, ph;
    uint8_t *a = malloc(W * H * 4), *b = malloc(W * H * 4);
    if (!v)
        return printf("FAIL: can't open\n"), 1;
    for (int i = 0; i < 200 && reelcore_update(v) != REELCORE_NEW_FRAME; i++)
        fake_time += 0.01;

    for (int bgr = 0; bgr < 2; bgr++) {
        int inside = 0, outside = 0, white = 0, darker = 0, n = 0;
        reelcore_set_panel(v, NULL);
        reelcore_draw_pixels(v, a, W * 4, W, H, bgr, REELCORE_STRETCH);
        set(v, 1);
        reelcore_panel_size(v, &pw, &ph);
        CHECK(pw > 300 && ph > 50 && pw < W && ph < H, "panel %dx%d", pw, ph);
        reelcore_draw_pixels(v, b, W * 4, W, H, bgr, REELCORE_STRETCH);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *p = a + (y * W + x) * 4, *q = b + (y * W + x) * 4;
                int in = x >= 10 && x < 10 + pw && y >= 10 && y < 10 + ph, diff = memcmp(p, q, 3) != 0;
                if (in) {
                    inside += diff;
                    white += q[0] > 220 && q[1] > 220 && q[2] > 220;
                    darker += q[0] + q[1] + q[2] <= p[0] + p[1] + p[2];
                    n++;
                } else
                    outside += diff;
            }
        printf("  pixels %s: panel %dx%d, %d of %d changed, %d white, %d darker; %d changed outside\n",
               bgr ? "BGR" : "RGB", pw, ph, inside, n, white, darker, outside);
        CHECK(outside == 0, "%d pixels changed outside the panel", outside);
        CHECK(inside > n * 9 / 10 && white > 300 && darker > n * 8 / 10, "the panel isn't there (%d changed, %d white, %d darker)",
              inside, white, darker);
        {   /* the graph's blue bars */
            int blue = 0;
            for (int y = 10; y < 10 + ph; y++)
                for (int x = 10; x < 10 + pw; x++) {
                    const uint8_t *q = b + (y * W + x) * 4;
                    int r = q[bgr ? 2 : 0], bl = q[bgr ? 0 : 2];
                    blue += bl > 200 && r < 60;
                }
            CHECK(blue > 20, "no graph bars (%d blue pixels)", blue);
        }
    }

    {   /* YV12 for the overlay: 1:1, twice the size (a big frame shown
           small) and half (a 720p frame full screen on 1920x1200: the
           panel was drawn at 15 px and scaled down a pixel at a time,
           unreadable). It must be drawn in a font of the size seen, not
           scaled: about k times as wide, only where its layer is. */
        int fw, fh;
        static const double ks[3] = { 1, 2, 0.5 };
        reelcore_frame_size(v, &fw, &fh);
        fw &= ~1; fh &= ~1;
        for (int ki = 0; ki < 3; ki++) {
            double k = ks[ki];
            uint8_t *ya = malloc(fw * fh * 3 / 2), *yb = malloc(fw * fh * 3 / 2);
            uint8_t *pa[3] = { ya, ya + fw * fh, ya + fw * fh + fw * fh / 4 }, *pb[3] = { yb, yb + fw * fh, yb + fw * fh + fw * fh / 4 };
            int pitch[3] = { fw, fw / 2, fw / 2 }, inside = 0, outside = 0, bright = 0, n = 0, cdiff = 0;
            int kw, kh, m = fw, my = fh;
            reelcore_set_panel(v, NULL);
            reelcore_draw_yuv420(v, pa, pitch, fw, fh, NULL);
            set(v, k);
            reelcore_draw_yuv420(v, pb, pitch, fw, fh, NULL);
            reelcore_panel_size(v, &kw, &kh);
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++)
                    if (ya[y * fw + x] != yb[y * fw + x]) {
                        m = x < m ? x : m;
                        my = y < my ? y : my;
                    }
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++) {
                    int in = x >= m && x < m + kw && y >= my && y < my + kh, d = ya[y * fw + x] != yb[y * fw + x];
                    if (in) { inside += d; bright += yb[y * fw + x] > 200; n++; }
                    else outside += d;
                }
            for (int i = fw * fh; i < fw * fh * 3 / 2; i++)
                cdiff += ya[i] != yb[i];
            printf("  YV12 x%.1f: panel %dx%d at %d,%d; %d of %d changed, %d bright; %d changed outside; %d chroma changed\n",
                   k, kw, kh, m, my, inside, n, bright, outside, cdiff);
            CHECK(outside == 0, "YV12 x%.1f: %d changed outside the panel", k, outside);
            CHECK(m == my && m >= 10 * k * 0.6 && m <= 10 * k * 1.4 + 1, "YV12 x%.1f: margin %d,%d", k, m, my);
            CHECK(inside > n * 8 / 10 && bright > 300 * k * k && cdiff > 1000 * k * k, "YV12 x%.1f: the panel isn't there", k);
            CHECK(k == 1 ? kw == pw && kh == ph : kw > pw * k * 0.75 && kw < pw * k * 1.25 && kh > ph * k * 0.7 && kh < ph * k * 1.4,
                  "YV12 x%.1f: panel %dx%d, not about %.0fx%.0f", k, kw, kh, pw * k, ph * k);
            free(ya); free(yb);
        }
    }

    {   /* YV12 at half the frame's size (a 4K video into an HD-sized overlay):
           each output pixel the rounded 2x2 average, as reelcore_halve_plane */
        int fw, fh, hw, hh, bad = 0;
        reelcore_set_panel(v, NULL);
        reelcore_frame_size(v, &fw, &fh);
        hw = fw / 2 & ~1; hh = fh / 2 & ~1;
        {
            uint8_t *full = malloc(fw * fh * 3 / 2), *got = malloc(hw * hh * 3 / 2), *want = malloc(hw * hh * 3 / 2);
            uint8_t *pf[3] = { full, full + fw * fh, full + fw * fh * 5 / 4 };
            uint8_t *pg[3] = { got, got + hw * hh, got + hw * hh * 5 / 4 };
            uint8_t *pw_[3] = { want, want + hw * hh, want + hw * hh * 5 / 4 };
            int ff[3] = { fw, fw / 2, fw / 2 }, hp[3] = { hw, hw / 2, hw / 2 };
            reelcore_draw_yuv420(v, pf, ff, fw, fh, NULL);
            CHECK(reelcore_draw_yuv420(v, pg, hp, hw, hh, NULL) == 0, "YV12 at half size refused");
            for (int p = 0; p < 3; p++)
                reelcore_halve_plane(pw_[p], hp[p], pf[p], ff[p], p ? hw / 2 : hw, p ? hh / 2 : hh);
            bad = memcmp(got, want, hw * hh * 3 / 2) != 0;
            printf("  YV12 halved: %dx%d from %dx%d%s\n", hw, hh, fw, fh, bad ? " (differs)" : ", the 2x2 averages");
            CHECK(!bad, "YV12 halved isn't the 2x2 averages");
            CHECK(reelcore_draw_yuv420(v, pg, hp, hw + 2, hh, NULL) == 0, "YV12 at just over half: refused");
            free(full); free(got); free(want);
        }
    }

    if (argc > 2) {   /* a 4:4:4 video at half size: luma the 2x2 averages, colour near */
        ReelCore *q = reelcore_open(argv[2], 0);
        CHECK(q != NULL, "can't open %s", argv[2]);
        for (int i = 0; q && i < 200 && reelcore_update(q) != REELCORE_NEW_FRAME; i++)
            fake_time += 0.01;
        if (q) {
            int fw, fh, hw, hh, bady, cdiff = 0;
            reelcore_frame_size(q, &fw, &fh);
            hw = fw / 2 & ~1; hh = fh / 2 & ~1;
            uint8_t *full = malloc(fw * fh * 3 / 2), *got = malloc(hw * hh * 3 / 2), *want = malloc(hw * hh * 3 / 2);
            uint8_t *pf[3] = { full, full + fw * fh, full + fw * fh * 5 / 4 };
            uint8_t *pg[3] = { got, got + hw * hh, got + hw * hh * 5 / 4 };
            uint8_t *pw_[3] = { want, want + hw * hh, want + hw * hh * 5 / 4 };
            int ff[3] = { fw, fw / 2, fw / 2 }, hp[3] = { hw, hw / 2, hw / 2 };
            reelcore_draw_yuv420(q, pf, ff, fw, fh, NULL);           /* (luma as it is; colour sampled) */
            CHECK(reelcore_draw_yuv420(q, pg, hp, hw, hh, NULL) == 0, "4:4:4 at half size refused");
            for (int p = 0; p < 3; p++)
                reelcore_halve_plane(pw_[p], hp[p], pf[p], ff[p], p ? hw / 2 : hw, p ? hh / 2 : hh);
            bady = memcmp(got, want, hw * hh) != 0;
            for (int i = hw * hh; i < hw * hh * 3 / 2; i++)
                cdiff += abs(got[i] - want[i]);
            printf("  4:4:4 halved: %dx%d from %dx%d, luma %s, colour off by %.2f on average\n", hw, hh, fw, fh,
                   bady ? "differs" : "the 2x2 averages", cdiff / (hw * hh / 2.0));
            CHECK(!bady && cdiff / (hw * hh / 2.0) < 3, "4:4:4 halved wrong");
            free(full); free(got); free(want);
            reelcore_close(q);
        }
    }

    {   /* the blend (NEON, 8 at a time, then one at a time): exactly
           (Y*a + d*(255-a)) / 255, every other layer pixel for a chroma row,
           and nothing past the run read (the run at the end of the buffers) */
        int bad = 0, cases = 0;
        srand(7);
        for (int k = 1; k <= 2; k++)
            for (int n = 1; n <= 40; n++)
                for (int rep = 0; rep < 20; rep++) {
                    int len = (n - 1) * k + 1;
                    uint16_t *pm = malloc(len * 2);
                    uint8_t *ia = malloc(len), d[40], want[40];
                    for (int i = 0; i < len; i++) {
                        int a = rep == 0 ? 255 * (i & 1) : rand() % 256, y = rand() % 256;
                        pm[i] = (uint16_t)(y * a);
                        ia[i] = (uint8_t)(255 - a);
                    }
                    for (int i = 0; i < n; i++) {
                        d[i] = (uint8_t)(rand() % 256);
                        want[i] = (uint8_t)((pm[k * i] + d[i] * ia[k * i]) / 255);
                    }
                    reelcore_blend_run(d, pm, ia, n, k);
                    bad += memcmp(d, want, n) != 0;
                    cases++;
                    free(pm); free(ia);
                }
        printf("  blend runs: %d of %d exact\n", cases - bad, cases);
        CHECK(!bad, "blend runs: %d of %d wrong", bad, cases);
    }

    {   /* updated (once a second in Reel): only the rows that changed are
           made again, and the picture is exactly the panel made afresh;
           a shorter value doesn't make it narrower (or all of it again) */
        static const float graph2[60] = { 0.9f, 0.2f, 0.4f, 0.8f, 0.6f, 0.1f };
        int fw, fh, w0, h0, w1, h1, w2, h2;
        reelcore_frame_size(v, &fw, &fh);
        fw &= ~1; fh &= ~1;
        for (int ki = 1; ki <= 2; ki++) {
            uint8_t *ya = malloc(fw * fh * 3 / 2), *yb = malloc(fw * fh * 3 / 2);
            uint8_t *pa[3] = { ya, ya + fw * fh, ya + fw * fh + fw * fh / 4 }, *pb[3] = { yb, yb + fw * fh, yb + fw * fh + fw * fh / 4 };
            int pitch[3] = { fw, fw / 2, fw / 2 }, same_yuv, same_rgb;
            for (int rgb = 0; rgb < 2; rgb++) {                     /* drawn into an overlay, then a sprite */
                uint8_t *x = rgb ? a : ya, *y = rgb ? b : yb;
                size_t n = rgb ? (size_t)W * H * 4 : (size_t)fw * fh * 3 / 2;
                reelcore_set_panel(v, NULL);
                speed = "1234 Kbps"; date = "Tue Sep 29 2026 12:00:00 \xa3"; g1 = graph;
                set(v, ki);
                if (rgb) reelcore_draw_pixels(v, a, W * 4, W, H, 0, REELCORE_STRETCH);
                else reelcore_draw_yuv420(v, pa, pitch, fw, fh, NULL);   /* (made, and into Y,Cb,Cr) */
                speed = "5678 Kbps"; date = "Tue Sep 29 2026 12:00:01 \xa3"; g1 = graph2;
                set(v, ki);                                         /* rows 1 and 2 changed */
                if (rgb) reelcore_draw_pixels(v, a, W * 4, W, H, 0, REELCORE_STRETCH);
                else reelcore_draw_yuv420(v, pa, pitch, fw, fh, NULL);
                reelcore_panel_size(v, &w0, &h0);
                reelcore_set_panel(v, NULL);                        /* the same, afresh */
                set(v, ki);
                if (rgb) reelcore_draw_pixels(v, b, W * 4, W, H, 0, REELCORE_STRETCH);
                else reelcore_draw_yuv420(v, pb, pitch, fw, fh, NULL);
                *(rgb ? &same_rgb : &same_yuv) = !memcmp(x, y, n);
            }
            speed = "9 Kbps";                                       /* shorter */
            set(v, ki);
            reelcore_panel_size(v, &w1, &h1);
            speed = "123456789012345678901234567890 Kbps";          /* longer than any */
            set(v, ki);
            reelcore_panel_size(v, &w2, &h2);
            printf("  updated x%d: as made afresh: YV12 %s, 32bpp %s; %dx%d, a shorter value %dx%d, a longer %dx%d\n", ki,
                   same_yuv ? "yes" : "no", same_rgb ? "yes" : "no", w0, h0, w1, h1, w2, h2);
            CHECK(same_yuv && same_rgb, "updated x%d: not as made afresh (YV12 %d, 32bpp %d)", ki, same_yuv, same_rgb);
            CHECK(w1 == w0 && h1 == h0 && w2 > w0 && h2 == h0, "updated x%d: sizes %dx%d, %dx%d, %dx%d", ki, w0, h0, w1, h1, w2, h2);
            free(ya); free(yb);
        }
        speed = "1234 Kbps"; date = "Tue Sep 29 2026 12:00:00 \xa3"; g1 = graph;
    }

    {   /* off again: exactly as before */
        reelcore_set_panel(v, NULL);
        reelcore_draw_pixels(v, b, W * 4, W, H, 0, REELCORE_STRETCH);
        reelcore_draw_pixels(v, a, W * 4, W, H, 0, REELCORE_STRETCH);
        reelcore_panel_size(v, &pw, &ph);
        CHECK(!memcmp(a, b, W * H * 4) && pw == 0 && ph == 0, "switched off, the picture still changes");
    }
    reelcore_close(v);
    free(a); free(b);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
