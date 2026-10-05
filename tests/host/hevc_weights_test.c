/*
 * HEVC slices whose weight table is all defaults (FFmpeg patch 0024): the
 * weighted motion compensation (put_hevc_*_w) must not run for them, and
 * must still run for slices with real weights (a fade).  The decoder's
 * weighted functions are wrapped with counters after the first picture, and
 * each call is put against the slice being decoded (one thread, so
 * priv_data's sh is that slice).  That the pictures are unchanged is checked
 * by run.sh (framemd5 against the x86 FFmpeg).
 * Usage: hevc_weights_test clip.mkv
 */
#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/hevcdec.h"

static HEVCContext *hs;
static long calls_default, calls_real;
static const uint8_t pel_weight[65] = { [2] = 0, [4] = 1, [6] = 2, [8] = 3, [12] = 4, [16] = 5, [24] = 6, [32] = 7, [48] = 8, [64] = 9 };

static __typeof__(hs->hevcdsp.put_hevc_qpel_uni_w) o_qu, o_eu;
static __typeof__(hs->hevcdsp.put_hevc_qpel_bi_w) o_qb;
static __typeof__(hs->hevcdsp.put_hevc_epel_bi_w) o_eb;

static void count(void)
{
    if (hs->sh.default_weights) calls_default++; else calls_real++;
}
static void w_qu(uint8_t *d, ptrdiff_t ds, uint8_t *s, ptrdiff_t ss, int h, int den, int wx, int ox, intptr_t mx, intptr_t my, int w)
{ count(); o_qu[pel_weight[w]][!!my][!!mx](d, ds, s, ss, h, den, wx, ox, mx, my, w); }
static void w_eu(uint8_t *d, ptrdiff_t ds, uint8_t *s, ptrdiff_t ss, int h, int den, int wx, int ox, intptr_t mx, intptr_t my, int w)
{ count(); o_eu[pel_weight[w]][!!my][!!mx](d, ds, s, ss, h, den, wx, ox, mx, my, w); }
static void w_qb(uint8_t *d, ptrdiff_t ds, uint8_t *s, ptrdiff_t ss, int16_t *s2, int h, int den, int a, int b, int c, int e, intptr_t mx, intptr_t my, int w)
{ count(); o_qb[pel_weight[w]][!!my][!!mx](d, ds, s, ss, s2, h, den, a, b, c, e, mx, my, w); }
static void w_eb(uint8_t *d, ptrdiff_t ds, uint8_t *s, ptrdiff_t ss, int16_t *s2, int h, int den, int a, int b, int c, int e, intptr_t mx, intptr_t my, int w)
{ count(); o_eb[pel_weight[w]][!!my][!!mx](d, ds, s, ss, s2, h, den, a, b, c, e, mx, my, w); }

static void wrap(void)
{
    int i, j, k;
    memcpy(o_qu, hs->hevcdsp.put_hevc_qpel_uni_w, sizeof(o_qu));
    memcpy(o_eu, hs->hevcdsp.put_hevc_epel_uni_w, sizeof(o_eu));
    memcpy(o_qb, hs->hevcdsp.put_hevc_qpel_bi_w, sizeof(o_qb));
    memcpy(o_eb, hs->hevcdsp.put_hevc_epel_bi_w, sizeof(o_eb));
    for (i = 0; i < 10; i++) for (j = 0; j < 2; j++) for (k = 0; k < 2; k++) {
        hs->hevcdsp.put_hevc_qpel_uni_w[i][j][k] = w_qu;
        hs->hevcdsp.put_hevc_epel_uni_w[i][j][k] = w_eu;
        hs->hevcdsp.put_hevc_qpel_bi_w[i][j][k] = w_qb;
        hs->hevcdsp.put_hevc_epel_bi_w[i][j][k] = w_eb;
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *c;
    const AVCodec *dec;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int vi, pics = 0, wrapped = 0, dflt_slices = 0, real_slices = 0;

    if (argc < 2 || avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
        return 1;
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fc->streams[vi]->codecpar);
    c->thread_count = 1;
    if (avcodec_open2(c, dec, NULL) < 0) return 1;
    hs = c->priv_data;
    for (;;) {
        int eof = av_read_frame(fc, pkt) < 0;
        if (!eof && pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(c, eof ? NULL : pkt) < 0) { printf("FAIL: decoding\n"); return 1; }
        if (!eof && hs->ps.pps && ((hs->sh.slice_type == HEVC_SLICE_P && hs->ps.pps->weighted_pred_flag) ||
                                   (hs->sh.slice_type == HEVC_SLICE_B && hs->ps.pps->weighted_bipred_flag))) {
            if (hs->sh.default_weights) dflt_slices++; else real_slices++;
        }
        if (!wrapped && hs->ps.sps) { wrap(); wrapped = 1; }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, f) >= 0) { pics++; av_frame_unref(f); }
        if (eof) break;
    }
    printf("  %d pictures; weighted slices: %d with the default weights, %d with others; "
           "weighted MC calls: %ld in default slices, %ld in others\n",
           pics, dflt_slices, real_slices, calls_default, calls_real);
    if (!dflt_slices || !real_slices || !calls_real) { printf("FAIL: the clip should have both kinds of slice\n"); return 1; }
    if (calls_default) { printf("FAIL: weighted prediction used for default weights\n"); return 1; }
    avcodec_free_context(&c);
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    return 0;
}
