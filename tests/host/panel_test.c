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

static void set(ReelCore *v, double k)
{
    ReelCorePanel p;
    memset(&p, 0, sizeof(p));
    p.rows = 3;
    p.label[0] = "Video / Source"; p.value[0] = "clip.mp4 / mov,mp4";
    p.label[1] = "Connection Speed"; p.value[1] = "1234 Kbps"; p.graph[1] = graph; p.graph_rgb[1] = 0x1E88E5;
    p.label[2] = "Date"; p.value[2] = "Tue Sep 29 2026 12:00:00 \xa3";   /* a Latin-1 character too */
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

    {   /* YV12 for the overlay, 1:1 and doubled */
        int fw, fh;
        reelcore_frame_size(v, &fw, &fh);
        fw &= ~1; fh &= ~1;
        for (int k = 1; k <= 2; k++) {
            uint8_t *ya = malloc(fw * fh * 3 / 2), *yb = malloc(fw * fh * 3 / 2);
            uint8_t *pa[3] = { ya, ya + fw * fh, ya + fw * fh + fw * fh / 4 }, *pb[3] = { yb, yb + fw * fh, yb + fw * fh + fw * fh / 4 };
            int pitch[3] = { fw, fw / 2, fw / 2 }, inside = 0, outside = 0, bright = 0, n = 0, cdiff = 0;
            reelcore_set_panel(v, NULL);
            reelcore_draw_yuv420(v, pa, pitch, fw, fh, NULL);
            set(v, k);
            reelcore_draw_yuv420(v, pb, pitch, fw, fh, NULL);
            for (int y = 0; y < fh; y++)
                for (int x = 0; x < fw; x++) {
                    int in = x >= 10 && x < 10 + pw * k && y >= 10 && y < 10 + ph * k, d = ya[y * fw + x] != yb[y * fw + x];
                    if (in) { inside += d; bright += yb[y * fw + x] > 200; n++; }
                    else outside += d;
                }
            for (int i = fw * fh; i < fw * fh * 3 / 2; i++)
                cdiff += ya[i] != yb[i];
            printf("  YV12 x%d: %d of %d changed, %d bright; %d changed outside; %d chroma changed\n",
                   k, inside, n, bright, outside, cdiff);
            CHECK(outside == 0, "YV12 x%d: %d changed outside the panel", k, outside);
            CHECK(inside > n * 8 / 10 && bright > 300 * k * k && cdiff > 1000, "YV12 x%d: the panel isn't there", k);
            if (fw >= 10 + pw * 2 + 10) {       /* x2 reaches twice as far */
                int far = 0;
                for (int y = 10; y < 10 + ph; y++)
                    far += ya[y * fw + 10 + pw + pw / 2] != yb[y * fw + 10 + pw + pw / 2];
                CHECK(k == 1 ? far == 0 : far > ph / 2, "YV12 x%d: %d changed at 1.5x its width", k, far);
            }
            free(ya); free(yb);
        }
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
