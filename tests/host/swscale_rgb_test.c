/*
 * swscale scaling to RGB32 in NEON (FFmpeg patch 0017, scaled_rgb_neon.S)
 * against the C: contexts made with the CPU flags off (C) and on (NEON),
 * YUV420P to RGBA and BGRA with SWS_FAST_BILINEAR.
 *  1. the functions alone: hyscale_fast, hcscale_fast (every scale the
 *     sizes below give), yuv2packed2 and yuv2packed1 (both chroma cases)
 *     with random lines, weights and colour settings (BT.601/709, both
 *     ranges, brightness, contrast, saturation);
 *  2. whole pictures through sws_scale, up and down, noise and gradients:
 *     the NEON context, then the same context with the C functions put
 *     back (a context made with the flags off can differ anyway: NEON
 *     rounds vertical filter sizes up to 2).
 * Everything byte for byte. Run under the trapping qemu.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libavutil/cpu.h"
#include "libswscale/swscale.h"
#include "libswscale/swscale_internal.h"

static unsigned st = 7;
static unsigned rnd(void) { st = st * 1103515245u + 12345u; return st >> 8; }
static int fails, runs;
#define FAIL(...) do { if (fails++ < 10) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void colours(SwsContext *s, int k)
{
    int *inv, *tab, sr, dr, b, c, sat;
    sws_getColorspaceDetails(s, &inv, &sr, &tab, &dr, &b, &c, &sat);
    sws_setColorspaceDetails(s, sws_getCoefficients(k & 1 ? SWS_CS_ITU709 : SWS_CS_ITU601), (k >> 1) & 1, tab, dr,
                             k >= 4 ? (k * 1234) % 30000 - 15000 : 0,          /* brightness */
                             k >= 4 ? (1 << 16) + (k * 777) % 20000 - 10000 : 1 << 16,
                             k >= 4 ? (1 << 16) + (k * 555) % 30000 - 15000 : 1 << 16);
}

static SwsContext *make(int sw, int sh, int dw, int dh, enum AVPixelFormat f, int neon, int k)
{
    SwsContext *s;
    av_force_cpu_flags(neon ? -1 : 0);
    s = sws_getContext(sw, sh, AV_PIX_FMT_YUV420P, dw, dh, f, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    av_force_cpu_flags(-1);
    if (s) colours(s, k);
    return s;
}

static void functions(int sw, int dw, enum AVPixelFormat f, int k)
{
    SwsContext *c = make(sw, 64, dw, 48, f, 0, k), *n = make(sw, 64, dw, 48, f, 1, k);
    static uint8_t src1[4200], src2[4200], dc[4 * 4100], dn[4 * 4100];
    static int16_t l0[4200], l1[4200], u0[2100], u1[2100], v0[2100], v1[2100], hc[4200], hn[4200], hc2[4200], hn2[4200];
    const int16_t *lb[2] = { l0, l1 }, *ub[2] = { u0, u1 }, *vb[2] = { v0, v1 };
    if (!c || !n) { FAIL("no context %dx.. -> %d", sw, dw); return; }
    if (n->yuv2packed2 == c->yuv2packed2 || !n->yuv2rgb_arith_ok ||
        (c->hyscale_fast && n->hyscale_fast == c->hyscale_fast)) {
        FAIL("NEON not in use (%d -> %d, colours %d, arith %d)", sw, dw, k, n->yuv2rgb_arith_ok);
        return;
    }
    for (int rep = 0; rep < 4; rep++) {
        for (int i = 0; i < 4200; i++) { src1[i] = rnd(); src2[i] = rep == 1 ? (rnd() & 1) * 255 : rnd(); }
        /* horizontal */
        memset(hc, 0x55, sizeof(hc)); memset(hn, 0x55, sizeof(hn));
        if (!c->hyscale_fast) break;       /* (none at this scale) */
        c->hyscale_fast(c, hc, c->dstW, src1, c->srcW, c->lumXInc);
        n->hyscale_fast(n, hn, n->dstW, src1, n->srcW, n->lumXInc);
        runs++;
        if (memcmp(hc, hn, sizeof(hc))) FAIL("hyscale_fast %d -> %d", sw, dw);
        memset(hc, 0x55, sizeof(hc)); memset(hn, 0x55, sizeof(hn)); memset(hc2, 1, sizeof(hc2)); memset(hn2, 1, sizeof(hn2));
        c->hcscale_fast(c, hc, hc2, c->chrDstW, src1, src2, c->chrSrcW, c->chrXInc);
        n->hcscale_fast(n, hn, hn2, n->chrDstW, src1, src2, n->chrSrcW, n->chrXInc);
        runs++;
        if (memcmp(hc, hn, sizeof(hc)) || memcmp(hc2, hn2, sizeof(hc2))) FAIL("hcscale_fast %d -> %d", sw / 2, dw / 2);
        /* vertical + RGB: 15-bit lines as the scalers make them */
        for (int i = 0; i < 4200; i++) { l0[i] = rnd() % 32641; l1[i] = rnd() % 32641; }
        for (int i = 0; i < 2100; i++) {
            u0[i] = rnd() % 32641; u1[i] = rnd() % 32641; v0[i] = rnd() % 32641; v1[i] = rnd() % 32641;
            if (rep == 2) { u0[i] = u0[i] & 1 ? 32640 : 0; v1[i] = v1[i] & 1 ? 32640 : 0; }
        }
        {
            int ya = rep == 3 ? 4096 : rnd() % 4097, uva = rep == 3 ? 0 : rnd() % 4097;
            memset(dc, 0x55, sizeof(dc)); memset(dn, 0x55, sizeof(dn));
            c->yuv2packed2(c, lb, ub, vb, NULL, dc, dw, ya, uva, 5);
            n->yuv2packed2(n, lb, ub, vb, NULL, dn, dw, ya, uva, 5);
            runs++;
            if (memcmp(dc, dn, sizeof(dc))) {
                int i = 0; while (dc[i] == dn[i]) i++;
                FAIL("yuv2packed2 w %d %s colours %d: byte %d C %d NEON %d", dw, f == AV_PIX_FMT_RGBA ? "RGBA" : "BGRA", k, i, dc[i], dn[i]);
            }
            for (int half = 0; half < 2; half++) {
                int a = half ? 2048 + rnd() % 2049 : rnd() % 2048;
                memset(dc, 0x55, sizeof(dc)); memset(dn, 0x55, sizeof(dn));
                c->yuv2packed1(c, l0, ub, vb, NULL, dc, dw, a, 5);
                n->yuv2packed1(n, l0, ub, vb, NULL, dn, dw, a, 5);
                runs++;
                if (memcmp(dc, dn, sizeof(dc))) FAIL("yuv2packed1 w %d uvalpha %d colours %d", dw, a, k);
            }
        }
    }
    sws_freeContext(c); sws_freeContext(n);
}

static void pictures(int sw, int sh, int dw, int dh, enum AVPixelFormat f, int kind, int k)
{
    SwsContext *c = make(sw, sh, dw, dh, f, 0, k), *n = make(sw, sh, dw, dh, f, 1, k);
    void *keep[4];
    int ls[3] = { sw + 32, sw / 2 + 16, sw / 2 + 16 }, ds[1] = { dw * 4 + 8 };
    uint8_t *p[3], *oc = malloc((size_t)ds[0] * dh + 64), *on = malloc((size_t)ds[0] * dh + 64);
    for (int i = 0; i < 3; i++) {
        int h = i ? sh / 2 : sh;
        p[i] = malloc((size_t)ls[i] * h + 64);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < ls[i]; x++)
                p[i][y * ls[i] + x] = kind ? (x * 3 + y * 2 + i * 50) & 255 : rnd();
    }
    memset(oc, 0x55, (size_t)ds[0] * dh + 64); memset(on, 0x55, (size_t)ds[0] * dh + 64);
    sws_scale(n, (const uint8_t *const *)p, ls, 0, sh, &on, ds);
    keep[0] = n->hyscale_fast; keep[1] = n->hcscale_fast; keep[2] = n->yuv2packed1; keep[3] = n->yuv2packed2;
    if (n->hyscale_fast) { n->hyscale_fast = ff_hyscale_fast_c; n->hcscale_fast = ff_hcscale_fast_c; }
    n->yuv2packed1 = c->yuv2packed1;
    n->yuv2packed2 = c->yuv2packed2;
    sws_scale(n, (const uint8_t *const *)p, ls, 0, sh, &oc, ds);
    if (keep[3] == n->yuv2packed2 || !n->yuv2rgb_arith_ok)
        FAIL("picture %dx%d -> %dx%d: NEON not in use", sw, sh, dw, dh);
    runs++;
    if (memcmp(oc, on, (size_t)ds[0] * dh + 64)) {
        size_t i = 0; while (oc[i] == on[i]) i++;
        FAIL("picture %dx%d -> %dx%d %s colours %d: byte %zu (row %zu) C %d NEON %d", sw, sh, dw, dh,
             f == AV_PIX_FMT_RGBA ? "RGBA" : "BGRA", k, i, i / ds[0], oc[i], on[i]);
    }
    for (int i = 0; i < 3; i++) free(p[i]);
    free(oc); free(on);
    sws_freeContext(c); sws_freeContext(n);
}

int main(void)
{
    static const int sizes[][2] = { { 322, 640 }, { 640, 322 }, { 1280, 1920 }, { 1920, 1280 }, { 1280, 1100 },
                                    { 720, 1024 }, { 64, 50 }, { 200, 16 }, { 16, 200 }, { 1920, 640 }, { 100, 102 },
                                    { 34, 66 }, { 1280, 1832 }, { 1920, 1034 }, { 46, 24 } };
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        for (int k = 0; k < 8; k++)
            functions(sizes[s][0], sizes[s][1], k & 1 ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA, k);
    for (int dw = 2; dw <= 40; dw += 2)
        functions(30 + dw * 3 / 2, dw, dw & 2 ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA, dw % 8);
    pictures(322, 184, 640, 366, AV_PIX_FMT_RGBA, 0, 0);
    pictures(322, 184, 1920, 1080, AV_PIX_FMT_BGRA, 1, 1);
    pictures(1280, 544, 1920, 816, AV_PIX_FMT_RGBA, 1, 2);
    pictures(1280, 720, 800, 450, AV_PIX_FMT_BGRA, 0, 3);
    pictures(1920, 1080, 640, 360, AV_PIX_FMT_RGBA, 1, 5);
    pictures(720, 576, 1024, 768, AV_PIX_FMT_RGBA, 0, 6);
    pictures(640, 360, 322, 182, AV_PIX_FMT_BGRA, 1, 7);
    {   /* speed under qemu: 1280x544 -> 1920x816 */
        SwsContext *c = make(1280, 544, 1920, 816, AV_PIX_FMT_RGBA, 0, 0), *n = make(1280, 544, 1920, 816, AV_PIX_FMT_RGBA, 1, 0);
        static uint8_t y[1280 * 544], u[640 * 272], v[640 * 272], out[1920 * 816 * 4];
        const uint8_t *p[3] = { y, u, v };
        int ls[3] = { 1280, 640, 640 }, ds[1] = { 1920 * 4 };
        uint8_t *o[1] = { out };
        clock_t t0 = clock();
        double tc, tn;
        n->hyscale_fast = ff_hyscale_fast_c; n->hcscale_fast = ff_hcscale_fast_c;
        n->yuv2packed1 = c->yuv2packed1; n->yuv2packed2 = c->yuv2packed2;
        for (int i = 0; i < 3; i++) sws_scale(n, p, ls, 0, 544, o, ds);
        tc = (double)(clock() - t0) / CLOCKS_PER_SEC;
        sws_freeContext(n);
        n = make(1280, 544, 1920, 816, AV_PIX_FMT_RGBA, 1, 0);
        t0 = clock();
        for (int i = 0; i < 3; i++) sws_scale(n, p, ls, 0, 544, o, ds);
        tn = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  %d comparisons; 1280x544 -> 1920x816: C %.2f s, NEON %.2f s under qemu, which emulates NEON slowly: not a speed measure (%.1fx)\n", runs, tc, tn, tn > 0 ? tc / tn : 0);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
