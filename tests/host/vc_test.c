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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int asked, pending, refuse_open, refuse_send, fail_after = -1, refused, vc_frames, failed;
static AVCodecContext *vc_ctx;

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
    r = __real_avcodec_receive_frame(c, f);
    if (c == vc_ctx && r >= 0 && ++vc_frames == fail_after) {
        av_frame_unref(f);
        failed = 1;
        return AVERROR_EXTERNAL;             /* the VideoCore stopped answering */
    }
    return r;
}

void __real_avcodec_free_context(AVCodecContext **c);
void __wrap_avcodec_free_context(AVCodecContext **c)
{
    if (c && *c == vc_ctx)
        vc_ctx = NULL;
    __real_avcodec_free_context(c);
}

static char last_log[256];
static int fell_back;
static void log_line(int level, const char *line)
{
    (void)level;
    if (strstr(line, "on the ARM") || strstr(line, "VideoCore")) {
        printf("    %s%s", line, strchr(line, '\n') ? "" : "\n");
        if (strstr(line, "h264 on the ARM from")) fell_back++;
        snprintf(last_log, sizeof last_log, "%s", line);
    }
}

typedef struct { int end, decoder, decoder_early; unsigned decoded, shown; double pos; char info[4096]; } run_t;

static void play(const char *clip, int flags, run_t *out)
{
    ReelCore *v;
    ReelCoreStats st;
    int r = 0;
    double t0 = fake_time;
    memset(out, 0, sizeof *out);
    vc_ctx = NULL; vc_frames = 0; failed = 0; refused = 0; fell_back = 0;
    v = reelcore_open(clip, flags | REELCORE_NO_AUDIO);
    CHECK(v != NULL, "open %s", clip);
    if (!v) return;
    reelcore_media_info(v, out->info, sizeof out->info);
    for (int i = 0; i < 40000 && r != REELCORE_END && fake_time - t0 < 30; i++) {
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME && ++out->shown == 10) {
            reelcore_stats(v, &st);
            out->decoder_early = st.decoder;  /* (what the stats say while it plays) */
        }
        fake_time += 0.002;
    }
    reelcore_stats(v, &st);
    out->end = r == REELCORE_END;
    out->decoded = st.decoded;
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
    CHECK(a.decoder_early == REELCORE_DECODER_VIDEOCORE && a.decoder == REELCORE_DECODER_VIDEOCORE,
          "VideoCore: stats decoder %d, %d", a.decoder_early, a.decoder);
    refuse_send = 0;

    /* refused at open */
    refuse_open = 1;
    play(argv[1], 0, &a);
    printf("  refused at open: %u decoded, decoder %s\n", a.decoded, decoder_line(a.info));
    CHECK(asked == 2 && a.end && a.decoded == ref.decoded && strstr(decoder_line(a.info), "h264, 1 thread") &&
          strstr(last_log, "can't take this H.264"), "refused: asked %d, end %d, %u decoded, '%s'", asked, a.end, a.decoded,
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
