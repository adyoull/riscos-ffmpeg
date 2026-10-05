/*
 * HEVC with skip_loop_filter (FFmpeg patch 0025): the deblocking boundary
 * strengths are not worked out for pictures whose filtering is skipped, and
 * still are for the rest.  ff_hevc_deblocking_boundary_strengths is wrapped
 * (-Wl,--wrap) and each call put against the picture being decoded (one
 * thread).  That the pictures are unchanged is checked by run.sh (framemd5
 * against the x86 FFmpeg with the same skip_loop_filter).
 * Usage: hevc_skipbs_test clip default|nonref|nonkey|all
 */
#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/hevcdec.h"

static HEVCContext *hs;
static long c_idr, c_ref, c_nonref;

void __real_ff_hevc_deblocking_boundary_strengths(HEVCContext *s, int x0, int y0, int log2_trafo_size);
void __wrap_ff_hevc_deblocking_boundary_strengths(HEVCContext *s, int x0, int y0, int log2_trafo_size)
{
    if (IS_IDR(s)) c_idr++;
    else if (ff_hevc_nal_is_nonref(s->nal_unit_type)) c_nonref++;
    else c_ref++;
    __real_ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_trafo_size);
}

int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *c;
    const AVCodec *dec;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int vi, pics = 0, ok;
    const char *m = argc > 2 ? argv[2] : "";

    if (argc < 3 || avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
        return 1;
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fc->streams[vi]->codecpar);
    c->thread_count = 1;
    c->skip_loop_filter = !strcmp(m, "nonref") ? AVDISCARD_NONREF : !strcmp(m, "nonkey") ? AVDISCARD_NONKEY :
                          !strcmp(m, "all") ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    if (avcodec_open2(c, dec, NULL) < 0) return 1;
    hs = c->priv_data;
    for (;;) {
        int eof = av_read_frame(fc, pkt) < 0;
        if (!eof && pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(c, eof ? NULL : pkt) < 0) { printf("FAIL: decoding\n"); return 1; }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, f) >= 0) { pics++; av_frame_unref(f); }
        if (eof) break;
    }
    printf("  %s: %d pictures; boundary strengths worked out %ld times in IDR pictures, %ld in other "
           "reference pictures, %ld in non-reference\n", m, pics, c_idr, c_ref, c_nonref);
    if (!strcmp(m, "all"))         ok = !c_idr && !c_ref && !c_nonref;
    else if (!strcmp(m, "nonkey")) ok = c_idr && !c_ref && !c_nonref;
    else if (!strcmp(m, "nonref")) ok = c_idr && c_ref && !c_nonref;
    else                           ok = c_idr && c_ref && c_nonref;
    if (!ok) printf("FAIL: boundary strengths for the wrong pictures\n");
    avcodec_free_context(&c);
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    return !ok;
}
