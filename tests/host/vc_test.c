/*
 * reelcore with the Pi's VideoCore decoder (h264_vchiq, patch 0021). The
 * host's FFmpeg has no VCHIQ, so a stand-in plays its part: asked for by
 * name, "h264_vchiq" is FFmpeg's own h264 decoder, and the calls on the
 * context opened that way are wrapped to behave as h264_vchiq does:
 *   - its input fills: every packet is refused once (EAGAIN) before it's
 *     taken, as avcodec_send_packet does while the VideoCore is busy; not a
 *     picture may be lost;
 *   - it refuses the stream at open (AVERROR(ENOSYS), as for High 10 or
 *     1080p with gpu_mem 64): reelcore uses h264 on the ARM;
 *   - REELCORE_NO_VIDEOCORE: it isn't even asked for;
 *   - it gives its pictures in bursts of 16 (the real one holds them while
 *     its input is full, then hands over many at once), refusing packets
 *     while it does: reelcore keeps the packet and leaves pictures in the
 *     decoder rather than push any out of its queue (a picture lost there
 *     was the stutter seen on the Pi: the picture jumping ahead);
 *   - drop_before (patch 0022): pictures before it are given back without
 *     being copied; reelcore sets it when it's behind (a slow machine: each
 *     picture shown takes longer than its time) and on the way to a seek's
 *     picture, and never otherwise;
 *   - it fails part way (AVERROR_EXTERNAL from receive_frame, as when the
 *     VideoCore stops answering): reelcore goes on with h264 on the ARM,
 *     from where it was, to the end.
 * Media info says which decoder it is, and so do the stats (Reel's
 * picture panel and Media info's Decoded row).
 *
 *   vc_test CLIP   (an H.264 clip)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int asked, pending, refuse_open, refuse_send, fail_after = -1, refused, vc_frames, failed;
static AVCodecContext *vc_ctx;
#define BURST 16
static int burst, releasing, stash_n, stash_max;
static AVFrame *stash[64];
static int64_t drop_before = INT64_MIN;
static int vc_dropped, drop_sets;

int __real_av_opt_set_int(void *obj, const char *name, int64_t val, int flags);
int __wrap_av_opt_set_int(void *obj, const char *name, int64_t val, int flags)
{
    if (obj && obj == vc_ctx && !strcmp(name, "drop_before")) {
        drop_before = val;                   /* (h264_vchiq's option; the stand-in has none) */
        drop_sets++;
        return 0;
    }
    return __real_av_opt_set_int(obj, name, val, flags);
}

/* h264_vchiq's drop_before: an earlier picture given back uncopied */
static int dropping(const AVFrame *f)
{
    if (drop_before != INT64_MIN && f->pts != AV_NOPTS_VALUE && f->pts < drop_before) {
        vc_dropped++;
        return 1;
    }
    return 0;
}

static void stash_clear(void)
{
    while (stash_n)
        av_frame_free(&stash[--stash_n]);
    releasing = 0;
}

const AVCodec *__real_avcodec_find_decoder_by_name(const char *name);
const AVCodec *__wrap_avcodec_find_decoder_by_name(const char *name)
{
    if (!strcmp(name, "h264_vchiq")) {
        asked++;
        pending = 1;                         /* the next avcodec_open2 is "h264_vchiq" */
        return avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    return __real_avcodec_find_decoder_by_name(name);
}

int __real_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o);
int __wrap_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o)
{
    if (pending) {
        pending = 0;
        if (refuse_open)
            return AVERROR(ENOSYS);
        vc_ctx = c;
    }
    return __real_avcodec_open2(c, codec, o);
}

int __real_avcodec_send_packet(AVCodecContext *c, const AVPacket *p);
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    if (c == vc_ctx && burst && releasing)
        return AVERROR(EAGAIN);              /* its input full while it hands pictures over */
    if (c == vc_ctx && p && refuse_send) {
        if (!refused) { refused = 1; return AVERROR(EAGAIN); }   /* full: the same packet again later */
        refused = 0;
    }
    return __real_avcodec_send_packet(c, p);
}

int __real_avcodec_receive_frame(AVCodecContext *c, AVFrame *f);
int __wrap_avcodec_receive_frame(AVCodecContext *c, AVFrame *f)
{
    int r;
    if (c == vc_ctx && failed)
        return AVERROR_EXTERNAL;
    if (c == vc_ctx && burst) {
        for (;;) {
            if (releasing && stash_n) {
                av_frame_move_ref(f, stash[0]);
                av_frame_free(&stash[0]);
                memmove(stash, stash + 1, --stash_n * sizeof stash[0]);
                if (dropping(f)) {
                    av_frame_unref(f);
                    continue;
                }
                return 0;
            }
            releasing = 0;
            AVFrame *t = av_frame_alloc();
            r = __real_avcodec_receive_frame(c, t);
            if (r >= 0) {
                stash[stash_n++] = t;
                if (stash_n > stash_max) stash_max = stash_n;
                if (stash_n >= BURST) releasing = 1;
                continue;
            }
            av_frame_free(&t);
            if (r == AVERROR_EOF && stash_n) { releasing = 1; continue; }
            return r;                        /* EAGAIN: holding them */
        }
    }
    do
        r = __real_avcodec_receive_frame(c, f);
    while (c == vc_ctx && r >= 0 && dropping(f) && (av_frame_unref(f), 1));
    if (c == vc_ctx && r >= 0 && ++vc_frames == fail_after) {
        av_frame_unref(f);
        failed = 1;
        return AVERROR_EXTERNAL;             /* the VideoCore stopped answering */
    }
    return r;
}

void __real_avcodec_flush_buffers(AVCodecContext *c);
void __wrap_avcodec_flush_buffers(AVCodecContext *c)
{
    if (c == vc_ctx)
        stash_clear();
    __real_avcodec_flush_buffers(c);
}

void __real_avcodec_free_context(AVCodecContext **c);
void __wrap_avcodec_free_context(AVCodecContext **c)
{
    if (c && *c == vc_ctx) {
        vc_ctx = NULL;
        stash_clear();
    }
    __real_avcodec_free_context(c);
}

static char last_log[256];
static int fell_back, pushed_out;
static void log_line(int level, const char *line)
{
    (void)level;
    if (strstr(line, "pushed out")) {
        pushed_out++;
        printf("    %s%s", line, strchr(line, '\n') ? "" : "\n");
    }
    if (strstr(line, "on the ARM") || strstr(line, "VideoCore")) {
        printf("    %s%s", line, strchr(line, '\n') ? "" : "\n");
        if (strstr(line, "h264 on the ARM from")) fell_back++;
        snprintf(last_log, sizeof last_log, "%s", line);
    }
}

typedef struct { int end, decoder, decoder_early, jumps, back; unsigned decoded, shown, late; double pos, last; char info[4096]; } run_t;

static double seek_at = -1, seek_to, slow;   /* slow: the time each picture shown takes */

static void play(const char *clip, int flags, run_t *out)
{
    ReelCore *v;
    ReelCoreStats st;
    int r = 0;
    double t0 = fake_time;
    memset(out, 0, sizeof *out);
    vc_ctx = NULL; vc_frames = 0; failed = 0; refused = 0; fell_back = 0; pushed_out = 0; stash_max = 0;
    drop_before = INT64_MIN; vc_dropped = 0; drop_sets = 0;
    v = reelcore_open(clip, flags | REELCORE_NO_AUDIO);
    CHECK(v != NULL, "open %s", clip);
    if (!v) return;
    reelcore_media_info(v, out->info, sizeof out->info);
    for (int i = 0; i < 40000 && r != REELCORE_END && fake_time - t0 < 30; i++) {
        r = reelcore_update(v);
        if (seek_at >= 0 && reelcore_position(v) >= seek_at) {
            reelcore_seek(v, seek_to);
            seek_at = -1;
            out->last = -1;
            continue;
        }
        if (r == REELCORE_NEW_FRAME) {
            double at = reelcore_position(v);   /* a picture skipped: a jump of over 1.5 frames */
            if (out->last > 0 && at - out->last > 1.5 / 25)
                out->jumps++;
            if (out->last > 0 && at < out->last)
                out->back++;
            out->last = at;
            fake_time += slow;
        }
        if (r == REELCORE_NEW_FRAME && ++out->shown == 10) {
            reelcore_stats(v, &st);
            out->decoder_early = st.decoder;  /* (what the stats say while it plays) */
        }
        fake_time += 0.002;
    }
    reelcore_stats(v, &st);
    out->end = r == REELCORE_END;
    out->decoded = st.decoded;
    out->late = st.late;
    out->decoder = st.decoder;
    out->pos = reelcore_position(v);
    if (fell_back)                            /* (media info after the switch) */
        reelcore_media_info(v, out->info, sizeof out->info);
    reelcore_close(v);
}

static const char *decoder_line(const char *info)
{
    static char line[200];
    const char *d = strstr(info, "Decoder\t");
    line[0] = 0;
    if (d) { snprintf(line, sizeof line, "%s", d + 8); *strchrnul(line, '\n') = 0; }
    return line;
}

int main(int argc, char **argv)
{
    run_t ref, a;
    reelcore_set_log(log_line, 1);
    if (argc < 2) return 1;

    /* the ARM only: how many pictures there are */
    play(argv[1], REELCORE_NO_VIDEOCORE, &ref);
    printf("  ARM only: %u decoded, %u shown, decoder %s\n", ref.decoded, ref.shown, decoder_line(ref.info));
    CHECK(asked == 0, "REELCORE_NO_VIDEOCORE: h264_vchiq asked for %d times", asked);
    CHECK(ref.decoder == REELCORE_DECODER_ARM && ref.decoder_early == REELCORE_DECODER_ARM, "ARM only: stats decoder %d, %d",
          ref.decoder_early, ref.decoder);
    CHECK(ref.end && ref.decoded > 50 && strstr(decoder_line(ref.info), "h264, 1 thread"), "ARM only: end %d, %u decoded, '%s'",
          ref.end, ref.decoded, decoder_line(ref.info));

    /* the VideoCore, its input full before every packet */
    refuse_send = 1;
    play(argv[1], 0, &a);
    printf("  VideoCore, every packet refused once: %u decoded, %u shown, decoder %s\n", a.decoded, a.shown, decoder_line(a.info));
    CHECK(asked == 1 && a.end, "VideoCore: asked %d, end %d", asked, a.end);
    CHECK(a.decoded == ref.decoded, "VideoCore: %u pictures decoded, %u on the ARM (packets lost while its input was full?)",
          a.decoded, ref.decoded);
    CHECK(!strcmp(decoder_line(a.info), "VideoCore (h264_vchiq)"), "VideoCore: decoder line '%s'", decoder_line(a.info));
    CHECK(!vc_dropped, "VideoCore, keeping up: %d dropped by the decoder", vc_dropped);
    CHECK(a.decoder_early == REELCORE_DECODER_VIDEOCORE && a.decoder == REELCORE_DECODER_VIDEOCORE,
          "VideoCore: stats decoder %d, %d", a.decoder_early, a.decoder);
    refuse_send = 0;

    /* bursts of 16 pictures, packets refused while it hands them over */
    burst = 1;
    play(argv[1], 0, &a);
    printf("  bursts of %d: %u decoded, %u shown, %d jumps, %d pushed out, up to %d held in the stand-in\n", BURST, a.decoded,
           a.shown, a.jumps, pushed_out, stash_max);
    CHECK(stash_max == BURST, "bursts: the stand-in held %d (not bursting?)", stash_max);
    CHECK(!vc_dropped, "bursts, keeping up: %d pictures dropped by the decoder", vc_dropped);
    CHECK(a.end && a.decoded == ref.decoded && a.shown == ref.shown && !a.jumps && !pushed_out,
          "bursts: end %d, %u decoded, %u shown of %u, %d jumps, %d pictures pushed out of the queue", a.end, a.decoded,
          a.shown, ref.shown, a.jumps, pushed_out);
    /* ... and a seek while it holds some */
    seek_at = 2.0; seek_to = 0.5;
    play(argv[1], 0, &a);
    printf("  bursts, a seek from 2 s to 0.5 s: %u shown, %d jumps, ended %d at %.2f\n", a.shown, a.jumps, a.end, a.pos);
    CHECK(a.end && !a.jumps && !pushed_out && a.shown > ref.shown, "bursts, a seek: end %d, %u shown, %d jumps, %d pushed out",
          a.end, a.shown, a.jumps, pushed_out);
    printf("    %d pictures before the seek's dropped by the decoder\n", vc_dropped);
    CHECK(vc_dropped > 0, "bursts, a seek back: nothing dropped on the way to the seek's picture");

    /* a slow machine: each picture shown takes 60 ms, a 25 fps video has 40 */
    slow = 0.06;
    play(argv[1], 0, &a);
    printf("  slow, bursts: %u shown, %u late (copied, then skipped), %d dropped uncopied by the decoder, %d set, end %d\n",
           a.shown, a.late, vc_dropped, drop_sets, a.end);
    /* (a burst's pictures wait in reelcore's queue, so some go late there) */
    CHECK(a.end && !a.back && vc_dropped > 0 && a.shown + a.late + vc_dropped == ref.decoded,
          "slow, bursts: end %d, back %d, %u shown + %u late + %d dropped of %u", a.end, a.back, a.shown, a.late, vc_dropped,
          ref.decoded);
    burst = 0;
    play(argv[1], 0, &a);
    printf("  slow: %u shown, %u late, %d dropped uncopied, end %d\n", a.shown, a.late, vc_dropped, a.end);
    CHECK(a.end && !a.back && vc_dropped > 0 && a.shown + a.late + vc_dropped == ref.decoded && a.late < (unsigned)vc_dropped,
          "slow: end %d, back %d, %u shown + %u late + %d dropped of %u", a.end, a.back, a.shown, a.late, vc_dropped, ref.decoded);
    slow = 0;

    /* refused at open */
    refuse_open = 1;
    int asked0 = asked;
    play(argv[1], 0, &a);
    printf("  refused at open: %u decoded, decoder %s\n", a.decoded, decoder_line(a.info));
    CHECK(asked == asked0 + 1 && a.end && a.decoded == ref.decoded && strstr(decoder_line(a.info), "h264, 1 thread") &&
          strstr(last_log, "can't take this H.264"), "refused: asked %d, end %d, %u decoded, '%s'", asked - asked0, a.end, a.decoded,
          decoder_line(a.info));
    CHECK(a.decoder == REELCORE_DECODER_ARM, "refused: stats decoder %d", a.decoder);
    refuse_open = 0;

    /* failing part way, at the 40th picture */
    fail_after = 40;
    play(argv[1], 0, &a);
    printf("  failed at picture 40: %u decoded, %u shown, at %.2f s, decoder %s\n", a.decoded, a.shown, a.pos, decoder_line(a.info));
    CHECK(fell_back == 1 && a.end && a.shown > ref.shown * 9 / 10 && strstr(decoder_line(a.info), "the VideoCore failed part way"),
          "failed part way: fell back %d, end %d, %u shown of %u, '%s'", fell_back, a.end, a.shown, ref.shown, decoder_line(a.info));
    CHECK(a.decoder_early == REELCORE_DECODER_VIDEOCORE && a.decoder == REELCORE_DECODER_ARM_AFTER,
          "failed part way: stats decoder %d, then %d", a.decoder_early, a.decoder);
    fail_after = -1;

    printf(fails ? "vc_test: %d failures\n" : "vc_test: all passed\n", fails);
    return fails != 0;
}
