/*
 * reelcore's halving (big reductions, e.g. Reel's mini player), under the
 * align-trap qemu with reelcore built for NEON as on RISC OS:
 *   1. reelcore_halve_plane gives exactly the bytes of the plain C
 *      definition, (a + b + c + d + 2) >> 2, for every width 1..80 (the
 *      NEON loop and its C tail), at every source/destination alignment,
 *      with padded and negative pitches;
 *   2. through reelcore_draw_pixels: a quarter-size picture is halved
 *      twice (then only colour-converted), a third-size one once, a
 *      same-size one not at all; and the quarter-size picture's brightness
 *      is the 4x4 average of the full-size one's (to within rounding and
 *      4:2:0 colour edges), where swscale's fast bilinear alone would skip
 *      three pixels in four.
 *
 *   halve_test CLIP   (a 640x360 or bigger clip)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void ref_halve(uint8_t *d, int dp, const uint8_t *s, int sp, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *a = s + (ptrdiff_t)2 * y * sp + 2 * x, *b = a + sp;
            d[(ptrdiff_t)y * dp + x] = (uint8_t)((a[0] + a[1] + b[0] + b[1] + 2) >> 2);
        }
}

static void plane_checks(void)
{
    enum { H = 5, SP = 200, DP = 100 };
    static uint8_t src[SP * (2 * H + 2) + 64], got[DP * H + 64], want[DP * H + 64];
    unsigned seed = 12345, cases = 0, bad = 0;
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
    for (int i = 0; i < 16; i++) src[i] = 255;              /* the rounding at the top: 255*4+2 */
    for (int w = 1; w <= 80; w++)
        for (int so = 0; so < 8; so++)
            for (int doff = 0; doff < 4; doff++) {
                memset(got, 0xA5, sizeof(got));
                memset(want, 0xA5, sizeof(want));
                reelcore_halve_plane(got + doff, DP, src + so, SP, w, H);
                ref_halve(want + doff, DP, src + so, SP, w, H);
                cases++;
                if (memcmp(got, want, sizeof(got)))            /* the bytes around must be untouched too */
                    bad++;
            }
    /* upside down: the last row first, a negative pitch */
    for (int w = 1; w <= 40; w++) {
        memset(got, 0, sizeof(got));
        memset(want, 0, sizeof(want));
        reelcore_halve_plane(got, DP, src + SP * (2 * H - 1) + 3, -SP, w, H);
        ref_halve(want, DP, src + SP * (2 * H - 1) + 3, -SP, w, H);
        cases++;
        bad += memcmp(got, want, sizeof(got)) != 0;
    }
    printf("  halving a plane: %u cases (widths 1-80, every alignment, negative pitch), %u differ\n", cases, bad);
    CHECK(bad == 0, "reelcore_halve_plane differs from (a+b+c+d+2)>>2 in %u cases", bad);
}

/* PSNR of the brightness (luma) of two RGB pictures: chroma is only at
   half resolution in 4:2:0 video, so at sharp colour edges averaging the
   RGB (the reference here) and averaging the YUV (what halving does, like
   any scaler) differ; the brightness shouldn't */
static double luma_psnr(const uint32_t *a, const uint32_t *b, int n)
{
    double se = 0;
    for (int i = 0; i < n; i++) {
        double ya = 0.299 * (a[i] & 255) + 0.587 * ((a[i] >> 8) & 255) + 0.114 * ((a[i] >> 16) & 255);
        double yb = 0.299 * (b[i] & 255) + 0.587 * ((b[i] >> 8) & 255) + 0.114 * ((b[i] >> 16) & 255);
        se += (ya - yb) * (ya - yb);
    }
    se /= n;
    return se == 0 ? 99 : 10 * log10(255.0 * 255.0 / se);
}

int main(int argc, char **argv)
{
    ReelCore *v;
    ReelCoreStats st;
    int w, h, qw, qh;
    uint32_t *full, *quarter, *box, *third;

    plane_checks();

    v = reelcore_open(argv[1], 0);
    CHECK(v != NULL, "open %s", argv[1]);
    if (!v)
        return 1;
    for (int i = 0; i < 400; i++, fake_time += 0.01)
        if (reelcore_update(v) == REELCORE_NEW_FRAME && fake_time > 1.0)
            break;
    w = reelcore_width(v) & ~3;
    h = reelcore_height(v) & ~3;
    qw = w / 4; qh = h / 4;
    full = malloc((size_t)w * h * 4);
    quarter = malloc((size_t)qw * qh * 4);
    box = malloc((size_t)qw * qh * 4);
    third = malloc((size_t)(w / 3) * (h / 3) * 4);

    reelcore_draw_pixels(v, full, w * 4, w, h, 0, REELCORE_STRETCH);
    reelcore_stats(v, &st);
    CHECK(st.halvings == 0, "same size: halved %d times", st.halvings);

    reelcore_draw_pixels(v, quarter, qw * 4, qw, qh, 0, REELCORE_STRETCH);
    reelcore_stats(v, &st);
    CHECK(st.halvings == 2, "a quarter of the size: halved %d times (want 2)", st.halvings);

    reelcore_draw_pixels(v, third, (w / 3) * 4, w / 3, h / 3, 0, REELCORE_STRETCH);
    reelcore_stats(v, &st);
    CHECK(st.halvings == 1, "a third of the size: halved %d times (want 1)", st.halvings);

    /* the quarter picture is the 4x4 average of the full one */
    for (int y = 0; y < qh; y++)
        for (int x = 0; x < qw; x++) {
            unsigned s[3] = { 0, 0, 0 };
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    uint32_t p = full[(size_t)(4 * y + j) * w + 4 * x + i];
                    for (int c = 0; c < 3; c++) s[c] += (p >> (8 * c)) & 255;
                }
            box[(size_t)y * qw + x] = ((s[0] + 8) / 16) | (((s[1] + 8) / 16) << 8) | (((s[2] + 8) / 16) << 16);
        }
    {
        double p = luma_psnr(quarter, box, qw * qh);
        if (getenv("HALVE_DUMP")) {
            FILE *o = fopen(getenv("HALVE_DUMP"), "wb");
            fprintf(o, "P6 %d %d 255\n", qw, qh * 2);
            for (int i = 0; i < qw * qh * 2; i++) {
                uint32_t px = i < qw * qh ? quarter[i] : box[i - qw * qh];
                fputc(px & 255, o); fputc((px >> 8) & 255, o); fputc((px >> 16) & 255, o);
            }
            fclose(o);
        }
        printf("  %dx%d -> %dx%d: halved twice, brightness %.1f dB from the 4x4 average of the full-size picture\n",
               w, h, qw, qh, p);
        CHECK(p > 38, "the quarter-size picture's brightness is only %.1f dB from the 4x4 average", p);
    }
    reelcore_close(v);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
