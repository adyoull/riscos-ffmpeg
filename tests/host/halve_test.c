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
 *   3. reelcore_narrow10_plane (10-bit pictures to 8, NEON) gives exactly
 *      swscale's own 10 to 8-bit copy (its dither), widths 2-80, and a
 *      10-bit clip's first picture drawn into an overlay-sized YUV420 is
 *      swscale's conversion of it, byte for byte.
 *
 *   halve_test CLIP [CLIP10]   (a 640x360 or bigger clip; a 10-bit 4:2:0 one)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"

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

/* 3. 10-bit to 8 against swscale (yuv420p10le to yuv420p, the same size),
   odd sizes too; then the NEON loop and its tail against the plain C at
   every alignment and upside down (a negative pitch) */
static void ref_narrow(uint8_t *d, int dp, const uint16_t *s, int sp, int w, int h)
{
    static const int dith[2][2] = { { 1, 2 }, { 3, 0 } };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned n = (s[(ptrdiff_t)y * sp + x] + dith[y & 1][x & 1]) >> 2;
            d[(ptrdiff_t)y * dp + x] = n > 255 ? 255 : n;
        }
}

static void narrow_checks(void)
{
    enum { H = 17, SP = 100, DP = 90, RH = 6 };
    static uint16_t src[SP * RH + 16];
    static uint8_t got[DP * RH + 16], want[DP * RH + 16];
    unsigned seed = 777, cases = 0, bad = 0, cases2 = 0, bad2 = 0;
    for (int w = 1; w <= 80; w++) {
        AVFrame *a = av_frame_alloc(), *b = av_frame_alloc();
        uint8_t *mine[3];
        struct SwsContext *s;
        a->format = AV_PIX_FMT_YUV420P10LE; a->width = w; a->height = H;
        b->format = AV_PIX_FMT_YUV420P; b->width = w; b->height = H;
        av_frame_get_buffer(a, 0);
        av_frame_get_buffer(b, 0);
        for (int p = 0; p < 3; p++) {
            int pw = p ? (w + 1) / 2 : w, ph = p ? (H + 1) / 2 : H;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) {
                    uint16_t *q = (uint16_t *)(a->data[p] + y * a->linesize[p]) + x;
                    seed = seed * 1103515245u + 12345u;
                    *q = (seed >> 16) % 7 == 0 ? 1023 : (seed >> 16) % 7 == 1 ? 0 : (seed >> 8) & 1023;
                }
            mine[p] = calloc(pw, ph);
            reelcore_narrow10_plane(mine[p], pw, (const uint16_t *)a->data[p], a->linesize[p] / 2, pw, ph);
        }
        s = sws_getContext(w, H, AV_PIX_FMT_YUV420P10LE, w, H, AV_PIX_FMT_YUV420P, SWS_POINT, NULL, NULL, NULL);
        sws_scale(s, (const uint8_t * const *)a->data, a->linesize, 0, H, b->data, b->linesize);
        for (int p = 0; p < 3; p++) {
            int pw = p ? (w + 1) / 2 : w, ph = p ? (H + 1) / 2 : H;
            cases++;
            for (int y = 0; y < ph; y++)
                if (memcmp(mine[p] + y * pw, b->data[p] + y * b->linesize[p], pw)) { bad++; break; }
            free(mine[p]);
        }
        sws_freeContext(s);
        av_frame_free(&a);
        av_frame_free(&b);
    }
    for (size_t i = 0; i < sizeof(src) / 2; i++)
        src[i] = (seed = seed * 1103515245u + 12345u) >> 8 & 1023;
    src[0] = src[1] = 1023;
    for (int w = 1; w <= 80; w++)
        for (int so = 0; so < 8; so++)
            for (int doff = 0; doff < 4; doff++) {
                memset(got, 0xA5, sizeof(got));
                memset(want, 0xA5, sizeof(want));
                reelcore_narrow10_plane(got + doff, DP, src + so, SP, w, RH);
                ref_narrow(want + doff, DP, src + so, SP, w, RH);
                cases2++;
                bad2 += memcmp(got, want, sizeof(got)) != 0;     /* the bytes around untouched too */
            }
    for (int w = 1; w <= 80; w++) {                              /* upside down */
        memset(got, 0, sizeof(got));
        memset(want, 0, sizeof(want));
        reelcore_narrow10_plane(got, DP, src + SP * (RH - 1) + 3, -SP, w, RH);
        ref_narrow(want, DP, src + SP * (RH - 1) + 3, -SP, w, RH);
        cases2++;
        bad2 += memcmp(got, want, sizeof(got)) != 0;
    }
    printf("  10-bit to 8: %u planes (widths 1-80, height %d) against swscale, %u differ; %u against C "
           "(every alignment, negative pitch), %u differ\n", cases, H, bad, cases2, bad2);
    CHECK(bad == 0, "reelcore_narrow10_plane differs from swscale in %u planes", bad);
    CHECK(bad2 == 0, "reelcore_narrow10_plane differs from the C in %u cases", bad2);
}

/* the first picture of a 10-bit clip as reelcore draws it into YUV420 the
   same size, against swscale's conversion of the decoder's picture */
static void narrow_clip(const char *clip)
{
    AVFormatContext *fc = NULL;
    const AVCodec *dec = NULL;
    AVCodecContext *c;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc(), *o = av_frame_alloc();
    ReelCore *v;
    uint8_t *pl[3];
    int pitch[3], vi, got = 0, bad = 0;
    if (avformat_open_input(&fc, clip, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0) {
        CHECK(0, "open %s", clip);
        return;
    }
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fc->streams[vi]->codecpar);
    c->thread_count = 1;
    avcodec_open2(c, dec, NULL);
    while (!got && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vi && avcodec_send_packet(c, pkt) >= 0)
            got = avcodec_receive_frame(c, f) >= 0;
        av_packet_unref(pkt);
    }
    if (!got) {                                  /* (a decoder that holds pictures back) */
        avcodec_send_packet(c, NULL);
        got = avcodec_receive_frame(c, f) >= 0;
    }
    CHECK(got && f->format == AV_PIX_FMT_YUV420P10LE, "%s: a 10-bit 4:2:0 picture", clip);
    if (!got || f->format != AV_PIX_FMT_YUV420P10LE)
        return;
    o->format = AV_PIX_FMT_YUV420P; o->width = f->width & ~1; o->height = f->height & ~1;
    av_frame_get_buffer(o, 0);
    {
        struct SwsContext *s = sws_getContext(f->width, f->height, f->format, o->width, o->height, o->format,
                                              SWS_POINT, NULL, NULL, NULL);
        sws_scale(s, (const uint8_t * const *)f->data, f->linesize, 0, f->height, o->data, o->linesize);
        sws_freeContext(s);
    }
    v = reelcore_open(clip, 0);
    CHECK(v != NULL, "open %s", clip);
    if (!v)
        return;
    for (int i = 0; i < 400 && reelcore_update(v) != REELCORE_NEW_FRAME; i++, fake_time += 0.01)
        ;
    for (int p = 0; p < 3; p++) {
        pitch[p] = (p ? o->width / 2 : o->width) + 16;
        pl[p] = calloc(pitch[p], o->height);
    }
    CHECK(reelcore_draw_yuv420(v, pl, pitch, o->width, o->height, NULL) == 0, "draw_yuv420");
    {
        ReelCoreStats st;
        reelcore_stats(v, &st);
        CHECK(st.narrowed > 0, "the 10-bit picture wasn't narrowed (drawn by swscale as before)");
    }
    for (int p = 0; p < 3; p++) {
        for (int y = 0; y < (p ? o->height / 2 : o->height); y++)
            bad += memcmp(pl[p] + y * pitch[p], o->data[p] + y * o->linesize[p], p ? o->width / 2 : o->width) != 0;
        free(pl[p]);
    }
    printf("  %s: first picture drawn %dx%d, %d rows differ from swscale's\n", clip, o->width, o->height, bad);
    CHECK(bad == 0, "the 10-bit picture drawn differs from swscale's conversion");
    reelcore_close(v);
    avcodec_free_context(&c);
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    av_frame_free(&o);
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
    narrow_checks();
    if (argc > 2)
        narrow_clip(argv[2]);

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
