/*
 * HEVC chroma motion compensation (FFmpeg patch 0016, hevcdsp_epel_neon.S)
 * against FFmpeg's C: HEVCDSPContext made with the CPU flags off (C) and
 * on (NEON). put_hevc_epel, _uni and _bi, filters h, v and hv (every
 * fraction), every width (2 to 64), heights 1 to 64, sources at every
 * alignment, src2 from typical and extreme values. The whole destination
 * buffer is compared, so writing outside the block is caught too. Run
 * under the trapping qemu.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libavutil/cpu.h"
#include "libavutil/mem_internal.h"
#include "libavcodec/hevcdsp.h"

static const int widths[10] = { 2, 4, 6, 8, 12, 16, 24, 32, 48, 64 };
static unsigned st = 99;
static unsigned rnd(void) { st = st * 1103515245u + 12345u; return st >> 8; }

#define SRCSTRIDE 200
#define DSTSTRIDE 150
static uint8_t srcbuf[80 * SRCSTRIDE + 64];
static int16_t src2[(64 + 1) * MAX_PB_SIZE];
static int16_t d16c[70 * MAX_PB_SIZE], d16n[70 * MAX_PB_SIZE];
static uint8_t d8c[70 * DSTSTRIDE + 64], d8n[70 * DSTSTRIDE + 64];

int main(void)
{
    HEVCDSPContext c, n;
    int fails = 0, runs = 0;
    av_force_cpu_flags(0);
    ff_hevc_dsp_init(&c, 8);
    av_force_cpu_flags(-1);
    ff_hevc_dsp_init(&n, 8);
    if (n.put_hevc_epel[3][1][1] == c.put_hevc_epel[3][1][1]) {
        printf("FAIL: no NEON epel installed\n");
        return 1;
    }
    for (int iter = 0; iter < 3; iter++)
    for (int wi = 0; wi < 10; wi++)
        for (int f = 1; f < 4; f++)                     /* h, v, hv */
            for (int frac = 1; frac < 8; frac++) {
                int w = widths[wi], h = iter == 0 ? w : 1 + rnd() % 64;
                int mx = (f & 1) ? frac : 0, my = (f & 2) ? (iter == 2 ? 8 - frac : frac) : 0;
                int align = rnd() % 16, kind = rnd() % 3;
                uint8_t *src = srcbuf + 2 * SRCSTRIDE + 8 + align;
                for (size_t i = 0; i < sizeof(srcbuf); i++)
                    srcbuf[i] = iter == 1 ? ((rnd() & 1) ? 255 : 0) : rnd() & 255;
                for (size_t i = 0; i < sizeof(src2) / 2; i++)
                    src2[i] = kind == 0 ? (int16_t)rnd() : (int)(rnd() % 16384) * ((rnd() & 1) ? 1 : -1) + (int)(rnd() % 8192);
                for (int k = 0; k < 3; k++) {             /* plain, uni, bi */
                    int bad;
                    memset(d16c, 0x55, sizeof(d16c)); memcpy(d16n, d16c, sizeof(d16c));
                    memset(d8c, 0x55, sizeof(d8c));   memcpy(d8n, d8c, sizeof(d8c));
                    if (k == 0) {
                        c.put_hevc_epel[wi][!!my][!!mx](d16c, src, SRCSTRIDE, h, mx, my, w);
                        n.put_hevc_epel[wi][!!my][!!mx](d16n, src, SRCSTRIDE, h, mx, my, w);
                        bad = memcmp(d16c, d16n, sizeof(d16c));
                    } else if (k == 1) {
                        c.put_hevc_epel_uni[wi][!!my][!!mx](d8c + 3, DSTSTRIDE, src, SRCSTRIDE, h, mx, my, w);
                        n.put_hevc_epel_uni[wi][!!my][!!mx](d8n + 3, DSTSTRIDE, src, SRCSTRIDE, h, mx, my, w);
                        bad = memcmp(d8c, d8n, sizeof(d8c));
                    } else {
                        c.put_hevc_epel_bi[wi][!!my][!!mx](d8c + 1, DSTSTRIDE, src, SRCSTRIDE, src2, h, mx, my, w);
                        n.put_hevc_epel_bi[wi][!!my][!!mx](d8n + 1, DSTSTRIDE, src, SRCSTRIDE, src2, h, mx, my, w);
                        bad = memcmp(d8c, d8n, sizeof(d8c));
                    }
                    runs++;
                    if (bad && fails++ < 8)
                        printf("FAIL: %s w %d h %d mx %d my %d align %d\n",
                               k == 0 ? "epel" : k == 1 ? "epel_uni" : "epel_bi", w, h, mx, my, align);
                }
            }
    {   /* whole-sample copies (shared with luma) */
        for (int wi = 0; wi < 10; wi++) {
            memset(d16c, 0, sizeof(d16c)); memset(d16n, 0, sizeof(d16n));
            c.put_hevc_epel[wi][0][0](d16c, srcbuf + 5, SRCSTRIDE, 16, 0, 0, widths[wi]);
            n.put_hevc_epel[wi][0][0](d16n, srcbuf + 5, SRCSTRIDE, 16, 0, 0, widths[wi]);
            runs++;
            if (memcmp(d16c, d16n, sizeof(d16c)) && fails++ < 8)
                printf("FAIL: pel_pixels w %d\n", widths[wi]);
        }
    }
    {   /* speed under qemu: 16x16 uni hv (a 32x32 PU's chroma) */
        clock_t t0 = clock();
        double tc, tn;
        for (int i = 0; i < 20000; i++)
            c.put_hevc_epel_uni[5][1][1](d8c, DSTSTRIDE, srcbuf + 2 * SRCSTRIDE + 8, SRCSTRIDE, 16, 3, 5, 16);
        tc = (double)(clock() - t0) / CLOCKS_PER_SEC;
        t0 = clock();
        for (int i = 0; i < 20000; i++)
            n.put_hevc_epel_uni[5][1][1](d8n, DSTSTRIDE, srcbuf + 2 * SRCSTRIDE + 8, SRCSTRIDE, 16, 3, 5, 16);
        tn = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  %d comparisons; 20000 16x16 uni hv blocks: C %.2f s, NEON %.2f s under qemu, which emulates NEON slowly: not a speed measure (%.1fx)\n",
               runs, tc, tn, tn > 0 ? tc / tn : 0);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
