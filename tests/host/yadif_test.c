/*
 * ff_yadif_filter_line_neon (libavfilter/arm/vf_yadif_neon.S, patch 0015)
 * against the C line filter of vf_yadif.c (copied below: FILTER and CHECK,
 * 8 bits a sample): every mode and parity, first/middle/last lines (the
 * signs of prefs and mrefs), widths 1 to 70 and some wide ones, rows at
 * every alignment, random pictures and ones of extremes. Run under the
 * trapping qemu, so an unaligned access would fault. Also times both (host
 * time under qemu: only a rough ratio).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMAX3(a,b,c) FFMAX(FFMAX(a,b),c)
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMIN3(a,b,c) FFMIN(FFMIN(a,b),c)

/* ---- vf_yadif.c (FFmpeg 5.1.10), unchanged ----
   A copy, so the NEON is compared with the C as FFmpeg has it. When
   updating FFmpeg, copy FILTER, CHECK and filter_line_c again if
   vf_yadif.c changed them (docs/NEON.md, "Updating FFmpeg"). */
#define CHECK(j)\
    {   int score = FFABS(cur[mrefs - 1 + (j)] - cur[prefs - 1 - (j)])\
                  + FFABS(cur[mrefs  +(j)] - cur[prefs  -(j)])\
                  + FFABS(cur[mrefs + 1 + (j)] - cur[prefs + 1 - (j)]);\
        if (score < spatial_score) {\
            spatial_score= score;\
            spatial_pred= (cur[mrefs  +(j)] + cur[prefs  -(j)])>>1;\

#define FILTER(start, end, is_not_edge) \
    for (x = start;  x < end; x++) { \
        int c = cur[mrefs]; \
        int d = (prev2[0] + next2[0])>>1; \
        int e = cur[prefs]; \
        int temporal_diff0 = FFABS(prev2[0] - next2[0]); \
        int temporal_diff1 =(FFABS(prev[mrefs] - c) + FFABS(prev[prefs] - e) )>>1; \
        int temporal_diff2 =(FFABS(next[mrefs] - c) + FFABS(next[prefs] - e) )>>1; \
        int diff = FFMAX3(temporal_diff0 >> 1, temporal_diff1, temporal_diff2); \
        int spatial_pred = (c+e) >> 1; \
 \
        if (is_not_edge) {\
            int spatial_score = FFABS(cur[mrefs - 1] - cur[prefs - 1]) + FFABS(c-e) \
                              + FFABS(cur[mrefs + 1] - cur[prefs + 1]) - 1; \
            CHECK(-1) CHECK(-2) }} }} \
            CHECK( 1) CHECK( 2) }} }} \
        }\
 \
        if (!(mode&2)) { \
            int b = (prev2[2 * mrefs] + next2[2 * mrefs])>>1; \
            int f = (prev2[2 * prefs] + next2[2 * prefs])>>1; \
            int max = FFMAX3(d - e, d - c, FFMIN(b - c, f - e)); \
            int min = FFMIN3(d - e, d - c, FFMAX(b - c, f - e)); \
 \
            diff = FFMAX3(diff, min, -max); \
        } \
 \
        if (spatial_pred > d + diff) \
           spatial_pred = d + diff; \
        else if (spatial_pred < d - diff) \
           spatial_pred = d - diff; \
 \
        dst[0] = spatial_pred; \
 \
        dst++; \
        cur++; \
        prev++; \
        next++; \
        prev2++; \
        next2++; \
    }

static void filter_line_c(void *dst1, void *prev1, void *cur1, void *next1,
                          int w, int prefs, int mrefs, int parity, int mode)
{
    uint8_t *dst  = dst1;
    uint8_t *prev = prev1;
    uint8_t *cur  = cur1;
    uint8_t *next = next1;
    int x;
    uint8_t *prev2 = parity ? prev : cur ;
    uint8_t *next2 = parity ? cur  : next;
    FILTER(0, w, 1)
}
/* ---- end of vf_yadif.c ---- */

void ff_yadif_filter_line_neon(void *dst, void *prev, void *cur, void *next,
                               int w, int prefs, int mrefs, int parity, int mode);

#define STRIDE 1100
#define ROWS 5                     /* the line is row 2: 2 rows above and below */
static uint8_t pic[3][ROWS * STRIDE + 64];

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 16; }

static void fill(int kind)
{
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < (int)sizeof(pic[0]); i++)
            pic[f][i] = kind == 0 ? rnd() & 255 :                  /* noise */
                        kind == 1 ? ((rnd() & 1) ? 255 : 0) :       /* extremes */
                        kind == 2 ? 128 + (int)(rnd() % 9) - 4 :   /* nearly flat */
                        (i / 3 + f * 7 + (i / STRIDE) * 40) & 255; /* gradients, moving */
}

int main(void)
{
    static uint8_t d_c[STRIDE + 64], d_n[STRIDE + 64];
    int fails = 0, runs = 0;
    int widths[80], nw = 0;
    for (int w = 1; w <= 70; w++) widths[nw++] = w;
    widths[nw++] = 350; widths[nw++] = 710; widths[nw++] = 1010;

    for (int kind = 0; kind < 4; kind++) {
        fill(kind);
        for (int wi = 0; wi < nw; wi++)
            for (int off = 3; off < 3 + 8; off++)          /* every alignment of the row */
                for (int mode = 0; mode < 4; mode++)
                    for (int parity = 0; parity < 2; parity++)
                        for (int edge = 0; edge < 3; edge++) {  /* middle, first, last line */
                            int w = widths[wi];
                            int prefs = edge == 2 ? -STRIDE : STRIDE;
                            int mrefs = edge == 1 ? STRIDE : -STRIDE;
                            int m = edge ? 2 : mode;     /* vf_yadif.c: mode 2 at the edges */
                            uint8_t *p = pic[0] + 2 * STRIDE + off, *c = pic[1] + 2 * STRIDE + off,
                                    *n = pic[2] + 2 * STRIDE + off;
                            memset(d_c, 0xAA, sizeof(d_c));
                            memset(d_n, 0xAA, sizeof(d_n));
                            filter_line_c(d_c + off, p, c, n, w, prefs, mrefs, parity, m);
                            ff_yadif_filter_line_neon(d_n + off, p, c, n, w, prefs, mrefs, parity, m);
                            runs++;
                            if (memcmp(d_c + off, d_n + off, w)) {
                                if (fails++ < 5) {
                                    int x = 0;
                                    while (d_c[off + x] == d_n[off + x]) x++;
                                    printf("FAIL: kind %d w %d off %d mode %d parity %d edge %d: x %d: C %d, NEON %d\n",
                                           kind, w, off, m, parity, edge, x, d_c[off + x], d_n[off + x]);
                                }
                            }
                            /* nothing before the line written; at most 7 after */
                            for (int x = 0; x < off; x++)
                                if (d_n[x] != 0xAA) { fails++; printf("FAIL: wrote before the line\n"); break; }
                            for (int x = off + ((w + 7) & ~7); x < (int)sizeof(d_n); x++)
                                if (d_n[x] != 0xAA) { fails++; printf("FAIL: wrote %d past w %d\n", x - off - w, w); break; }
                        }
    }
    {   /* speed, 720 pixels (a PAL line), mode 0 */
        clock_t t0;
        double tc, tn;
        fill(0);
        t0 = clock();
        for (int i = 0; i < 20000; i++)
            filter_line_c(d_c + 3, pic[0] + 2 * STRIDE + 3, pic[1] + 2 * STRIDE + 3, pic[2] + 2 * STRIDE + 3, 710, STRIDE, -STRIDE, i & 1, 0);
        tc = (double)(clock() - t0) / CLOCKS_PER_SEC;
        t0 = clock();
        for (int i = 0; i < 20000; i++)
            ff_yadif_filter_line_neon(d_n + 3, pic[0] + 2 * STRIDE + 3, pic[1] + 2 * STRIDE + 3, pic[2] + 2 * STRIDE + 3, 710, STRIDE, -STRIDE, i & 1, 0);
        tn = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  %d comparisons; 20000 lines of 710: C %.2f s, NEON %.2f s under qemu (%.1fx)\n", runs, tc, tn, tn > 0 ? tc / tn : 0);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
