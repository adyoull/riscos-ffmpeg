/*
 * skip_loop_filter and skip_frame changed while decoding, as a player that
 * falls behind changes them (FFmpeg patch 0026 for VP9, libdav1d with
 * dav1d_riscos_set_skip for AV1). The clip decoded twice, one thread: as it
 * is, and with skip_loop_filter all for packets from 0.4 s to 0.8 s and
 * skip_frame nokey from 1.2 s to 1.6 s. Then: some pictures from the first
 * stretch differ (the filter was skipped), fewer pictures came out (frames
 * were skipped), none between 1.2 s and the keyframe at 2 s (once frames
 * others refer to are skipped, the decoder skips to the next keyframe
 * whatever skip_frame becomes: decoded from references that missed them,
 * they'd come out spoilt), no decoding errors, and from the keyframe on
 * every picture is as it was (nothing left switched on).
 * Usage: skip_switch_test clip [decoder]
 */
#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/md5.h"

#define MAXP 400
typedef struct { int n; int64_t pts[MAXP]; uint8_t md5[MAXP][16]; int errors; } Run;

static void add(Run *r, AVFrame *f)
{
    int size = av_image_get_buffer_size(f->format, f->width, f->height, 1);
    uint8_t *buf = av_malloc(size);
    if (r->n >= MAXP || !buf) { av_free(buf); return; }
    av_image_copy_to_buffer(buf, size, (const uint8_t * const *)f->data, f->linesize, f->format, f->width, f->height, 1);
    av_md5_sum(r->md5[r->n], buf, size);
    r->pts[r->n++] = f->best_effort_timestamp;
    av_free(buf);
}

static int run(const char *path, const char *name, int switched, Run *r)
{
    AVFormatContext *fc = NULL;
    const AVCodec *dec = NULL;
    AVCodecContext *c;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    AVRational tb;
    int vi;
    if (avformat_open_input(&fc, path, NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
        return -1;
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (name) dec = avcodec_find_decoder_by_name(name);
    tb = fc->streams[vi]->time_base;
    c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fc->streams[vi]->codecpar);
    c->thread_count = 1;
    c->pkt_timebase = tb;
    if (avcodec_open2(c, dec, NULL) < 0) return -1;
    for (;;) {
        int eof = av_read_frame(fc, pkt) < 0;
        if (!eof && pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
        if (!eof && switched) {
            double t = pkt->pts * av_q2d(tb);
            c->skip_loop_filter = t >= 0.4 && t < 0.8 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
            c->skip_frame = t >= 1.2 && t < 1.6 ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
        }
        if (avcodec_send_packet(c, eof ? NULL : pkt) < 0) r->errors++;
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, f) >= 0) { add(r, f); av_frame_unref(f); }
        if (eof) break;
    }
    avcodec_free_context(&c);
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    av_frame_free(&f);
    return (int)(2.0 / av_q2d(tb) + 0.5);  /* 2 s in the stream's time base */
}

static Run a, b;

int main(int argc, char **argv)
{
    int i, j, differ_early = 0, differ_late = 0, late = 0, gap = 0, t2;
    if (argc < 2) return 1;
    av_log_set_level(AV_LOG_FATAL);
    if ((t2 = run(argv[1], argc > 2 ? argv[2] : NULL, 0, &a)) < 0 || run(argv[1], argc > 2 ? argv[2] : NULL, 1, &b) < 0)
        return 1;
    for (i = 0; i < b.n; i++) {
        for (j = 0; j < a.n && a.pts[j] != b.pts[i]; j++)
            ;
        if (j == a.n) { printf("FAIL: a picture at %"PRId64" not decoded without skipping\n", b.pts[i]); return 1; }
        if (memcmp(a.md5[j], b.md5[i], 16)) {
            if (b.pts[i] >= t2) differ_late++;
            else differ_early++;
        }
        late += b.pts[i] >= t2;
        gap += b.pts[i] >= t2 * 0.6 && b.pts[i] < t2;           /* 1.2 s to 2 s */
    }
    printf("  %d pictures, %d with skipping (%d errors on the way); %d changed before 2 s, %d of %d after; "
           "%d from 1.2 s to 2 s\n", a.n, b.n, b.errors, differ_early, differ_late, late, gap);
    if (a.errors || b.errors || gap || !differ_early || b.n >= a.n || differ_late || late < 10) {
        printf("FAIL: skipping switched while decoding\n");
        return 1;
    }
    return 0;
}
