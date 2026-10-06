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
#ifdef REELCORE_HEVCDEC
#include <hwhevcdec.h>
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int out_buffers = -1;                 /* asked of h264_vchiq (zero-copy: devkit 0.2.1) */
static int asked, pending, refuse_open, refuse_send, fail_after = -1, refused, vc_frames, failed;
static int asked_hb, output_8bit = -1, fail_err = AVERROR_EXTERNAL;   /* hevc_hwdec's stand-in */
/* the failure given back by send_packet rather than receive_frame: FFmpeg 5.1
   runs a receive_frame decoder (h264_vchiq, hevc_hwdec) inside send_packet,
   so that's where a real one's error comes back first */
static int fail_in_send;
static int output_hw = -1, vc_is_hevc, hw_live, to_i420_calls, to_half_calls, half_refused;
static long conv_px, drawn_px;          /* output pixels the block converted; pixels drawn */
static long cached_px;                  /* ... of them converted elsewhere than the caller's planes (cached memory) */
static const uint8_t *draw_lo, *draw_hi;   /* the caller's planes */
static int conv_x_off;                  /* conversions not starting on one of the block's 128-pixel columns */

#ifdef REELCORE_HEVCDEC
/* hevc_hwdec's output_hw (devkit 0.2.8): its frames are AV_PIX_FMT_HEVCDEC
   with data[3] a hevcdec_frame, converted by the caller when shown. Here
   that frame holds FFmpeg's own decoded picture, and the conversions copy
   from it (1:1) or take its 2x2 rounded means (halved). */
struct hevcdec_frame { AVFrame *soft; double ready_at; };
/* the block's latency (devkit 0.2.10): a picture handed out is decoded this
   long after (fake time); converting it sooner waits until then */
static double hb_latency;
static int conv_waited;
static void conv_wait(const hevcdec_frame *f)
{
    if (fake_time < f->ready_at) {
        conv_waited++;
        fake_time = f->ready_at;
    }
}
int hevcdec_frame_done(hevcdec *d, const hevcdec_frame *f)
{
    (void)d;
    return fake_time >= f->ready_at;
}
void hevcdec_get_stats(const hevcdec *d, hevcdec_stats *s)
{
    (void)d;
    memset(s, 0, sizeof(*s));
    s->convert_waits = conv_waited;
}

static void hf_free(void *opaque, uint8_t *data)
{
    struct hevcdec_frame *h = opaque;
    (void)data;
    av_frame_free(&h->soft);
    av_free(h);
    hw_live--;
}

static void to_hw(AVFrame *f)
{
    struct hevcdec_frame *h = av_mallocz(sizeof *h);
    h->soft = av_frame_alloc();
    av_frame_move_ref(h->soft, f);
    av_frame_copy_props(f, h->soft);
    f->format = AV_PIX_FMT_HEVCDEC;
    f->width = h->soft->width;
    f->height = h->soft->height;
    f->buf[0] = av_buffer_create((uint8_t *)h, sizeof *h, hf_free, h, 0);
    f->data[3] = (uint8_t *)h;
    h->ready_at = fake_time + hb_latency;
    hw_live++;
}

hevcdec *hevcdec_frame_decoder(const hevcdec_frame *f)
{
    return f ? (hevcdec *)1 : NULL;
}

int hevcdec_frame_to_i420(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x,
                          int y, int w, int h)
{
    const AVFrame *s = f->soft;
    (void)d;
    conv_wait(f);
    fake_time += 1e-6;                       /* (a conversion takes a moment: the log's timings) */
    to_i420_calls++;
    conv_px += (long)w * h;
    if (planes[0] < draw_lo || planes[0] >= draw_hi) cached_px += (long)w * h;
    conv_x_off += x % 128 != 0;              /* (not on the block's 128-pixel columns) */
    for (int p = 0; p < 3; p++) {
        int pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h, px = p ? x / 2 : x, py = p ? y / 2 : y;
        for (int r = 0; r < ph; r++)
            memcpy(planes[p] + (size_t)r * strides[p], s->data[p] + (size_t)(py + r) * s->linesize[p] + px, pw);
    }
    return HEVCDEC_OK;
}

int hevcdec_frame_to_i420_half(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x,
                               int y, int w, int h)
{
    const AVFrame *s = f->soft;
    (void)d;
    conv_wait(f);
    fake_time += 1e-6;
    to_half_calls++;
    conv_px += (long)w * h;
    if (planes[0] < draw_lo || planes[0] >= draw_hi) cached_px += (long)w * h;
    conv_x_off += x % 128 != 0;
    if ((x & 3) || (y & 1) || x + 2 * w > s->width || y + 2 * h > s->height)
        return half_refused++, HEVCDEC_UNSUPPORTED;
    for (int p = 0; p < 3; p++) {
        int pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h, px = p ? x / 2 : x, py = p ? y / 2 : y;
        for (int r = 0; r < ph; r++) {
            const uint8_t *a = s->data[p] + (size_t)(py + 2 * r) * s->linesize[p] + px, *b = a + s->linesize[p];
            for (int c = 0; c < pw; c++)
                planes[p][(size_t)r * strides[p] + c] = (a[2 * c] + a[2 * c + 1] + b[2 * c] + b[2 * c + 1] + 2) >> 2;
        }
    }
    return HEVCDEC_OK;
}
#endif
static AVCodecContext *vc_ctx;
#define BURST 16
static int burst, releasing, stash_n, stash_max;
static AVFrame *stash[64];
static int64_t drop_before = INT64_MIN;
static int vc_dropped, drop_sets;
/* the block only just keeping up: a picture out every hb_period (fake time)
   at most, dropped ones too (it decodes every picture), the caller waiting
   for it meanwhile; 0: as fast as asked */
static double hb_period, hb_next;

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
    if (!strcmp(name, "hevc_hwdec")) {       /* the Pi 4's HEVC block: FFmpeg's hevc plays its part too */
        asked_hb++;
        pending = 2;
        return avcodec_find_decoder(AV_CODEC_ID_HEVC);
    }
    return __real_avcodec_find_decoder_by_name(name);
}

int __real_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o);
int __wrap_avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **o)
{
    if (pending) {
        AVDictionaryEntry *e = o && *o ? av_dict_get(*o, "out_buffers", NULL, 0) : NULL;
        AVDictionaryEntry *e8 = o && *o ? av_dict_get(*o, "output_8bit", NULL, 0) : NULL;
        AVDictionaryEntry *eh = o && *o ? av_dict_get(*o, "output_hw", NULL, 0) : NULL;
        out_buffers = e ? atoi(e->value) : -1;
        output_8bit = e8 ? atoi(e8->value) : -1;
        output_hw = eh ? atoi(eh->value) : -1;
        vc_is_hevc = pending == 2;
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
    if (c == vc_ctx && failed && fail_in_send)
        return fail_err;
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
        return fail_in_send ? AVERROR(EAGAIN) : fail_err;
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
    for (;;) {
        r = __real_avcodec_receive_frame(c, f);
        if (c == vc_ctx && hb_period > 0 && r >= 0) {
            if (fake_time < hb_next)
                fake_time = hb_next;         /* waited for it (as hevc_hwdec waits for the block's room) */
            hb_next = fake_time + hb_period;
        }
        if (!(c == vc_ctx && r >= 0 && dropping(f)))
            break;
        av_frame_unref(f);
    }
    if (c == vc_ctx && r >= 0 && ++vc_frames == fail_after) {
        av_frame_unref(f);
        failed = 1;
        return fail_in_send ? AVERROR(EAGAIN) : fail_err;   /* the VideoCore stopped answering (or hevc_hwdec refused) */
    }
#ifdef REELCORE_HEVCDEC
    if (c == vc_ctx && r >= 0 && vc_is_hevc && output_hw == 1)
        to_hw(f);
#endif
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
        if (strstr(line, "on the ARM from") && !strstr(line, "from here")) fell_back++;   /* "<codec> on the ARM from N s" */
        snprintf(last_log, sizeof last_log, "%s", line);
    }
}

typedef struct { int end, decoder, decoder_early, jumps, back; unsigned decoded, shown, late, skip_spells, crc, drawn, slips, conv_busy, conv_idle; int max_skip; double pos, last, lag_late, rect_time; char info[4096]; } run_t;

/* each picture shown, drawn (1: 1:1 YUV, as into an overlay; 2: halved, as
   4K into an HD overlay; 3: 32bpp, as into a sprite), its bytes summed */
static int draw_mode;
static void draw(ReelCore *v, run_t *out)
{
    int fw, fh;
    if (!draw_mode || reelcore_frame_size(v, &fw, &fh) < 0)
        return;
    if (draw_mode == 3) {
        uint32_t *px = calloc((size_t)fw * fh, 4);
        if (reelcore_draw_pixels(v, px, fw * 4, fw, fh, 0, REELCORE_STRETCH) >= 0) {
            for (int i = 0; i < fw * fh; i++) out->crc = out->crc * 31 + (px[i] & 0xFFFFFF);
            out->drawn++;
        }
        free(px);
    } else {
        int w = draw_mode == 2 ? fw / 2 : fw, h = draw_mode == 2 ? fh / 2 : fh;
        int pitch[3] = { w, (w + 1) / 2, (w + 1) / 2 };
        uint8_t *buf = calloc((size_t)w * h * 2, 1), *planes[3] = { buf, buf + (size_t)w * h, buf + (size_t)w * h * 3 / 2 };
        draw_lo = buf; draw_hi = buf + (size_t)w * h * 2;
        if (reelcore_draw_yuv420(v, planes, pitch, w, h, NULL) >= 0) {
            for (size_t i = 0; i < (size_t)w * h * 3 / 2 + (size_t)((w + 1) / 2) * ((h + 1) / 2); i++)
                out->crc = out->crc * 31 + buf[i];
            out->drawn++;
            drawn_px += (long)w * h;
        }
        free(buf);
    }
}

/* the stats panel on (yuv_scale panel_on), as Reel's: drawn into the picture */
static double panel_on;
static void set_panel(ReelCore *v)
{
    static float g[60];
    ReelCorePanel p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 60; i++) g[i] = (float)(i % 7) / 7;
    p.rows = 3;
    p.label[0] = "Decoder"; p.value[0] = "HEVC block (hardware)";
    p.label[1] = "Convert"; p.value[1] = "7.7 ms"; p.graph[1] = g; p.graph_rgb[1] = 0x40C040;
    p.label[2] = "Dropped"; p.value[2] = "0";
    p.graph_n = 60;
    p.yuv_scale = panel_on;
    CHECK(reelcore_set_panel(v, &p) == 0, "set_panel");
}

static double seek_at = -1, seek_to, slow;   /* slow: the time each picture shown takes */

static double hiccup_at = -1;               /* the desktop busy 0.3 s this far in: the queue empties */
static double hiccup_len = 0.3;
static int idle_long;                        /* a picture left for the block, and reelcore_idle_time 10 ms or more */
static void play(const char *clip, int flags, run_t *out)
{
    int hiccuped = 0;
    ReelCore *v;
    ReelCoreStats st;
    int r = 0;
    double t0 = fake_time;
    memset(out, 0, sizeof *out);
    vc_ctx = NULL; vc_frames = 0; failed = 0; refused = 0; fell_back = 0; pushed_out = 0; stash_max = 0;
    drop_before = INT64_MIN; vc_dropped = 0; drop_sets = 0; hb_next = 0;
    v = reelcore_open(clip, flags | REELCORE_NO_AUDIO);
    CHECK(v != NULL, "open %s", clip);
    if (!v) return;
    reelcore_media_info(v, out->info, sizeof out->info);
    if (panel_on > 0)
        set_panel(v);
    for (int i = 0; i < 40000 && r != REELCORE_END && fake_time - t0 < 30; i++) {
        if (hiccup_at >= 0 && !hiccuped && fake_time - t0 > hiccup_at) {
            fake_time += hiccup_len;
            hiccuped = 1;
        }
        r = reelcore_update(v);
        if (hb_latency > 0) {                /* (Reel sleeps whole centiseconds: nothing given to the block meanwhile) */
            static unsigned nd;
            reelcore_stats(v, &st);
            if (st.hb_not_done != nd && reelcore_idle_time(v) >= 0.01)
                idle_long++;
            nd = st.hb_not_done;
        }
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
            reelcore_stats(v, &st);
            if (st.skip_level > out->max_skip) out->max_skip = st.skip_level;
            if (fake_time - t0 > 3) {         /* how late pictures are shown, from 3 s on */
                reelcore_stats(v, &st);
                if (st.clock - at > out->lag_late) out->lag_late = st.clock - at;
            }
            fake_time += slow;
            draw(v, out);
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
    out->skip_spells = st.skip_spells;
    out->slips = st.clock_slips;
    out->conv_busy = st.hb_conv_busy;
    out->conv_idle = st.hb_conv_idle;
    out->rect_time = st.rect_time;
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
    /* 25 fps: 4 decoded ahead, + the one shown + one coming in, + the decoder's 3 */
    printf("  out_buffers asked for: %d\n", out_buffers);
    CHECK(out_buffers == 9, "VideoCore: out_buffers %d, want 9 (3 + 4 ahead + 2)", out_buffers);
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
    /* (no sound: the clock moved back now and then, a machine too slow
       for the video slowed by no more than that, not every half second) */
    printf("  slow: %u shown, %u late, %d dropped uncopied, %u clock slips, end %d\n", a.shown, a.late, vc_dropped, a.slips,
           a.end);
    CHECK(a.end && !a.back && vc_dropped > 0 && a.shown + a.late + vc_dropped == ref.decoded && a.late < (unsigned)vc_dropped &&
          a.slips <= 2,
          "slow: end %d, back %d, %u shown + %u late + %d dropped of %u, %u clock slips (at most 2: one every 5 s)", a.end,
          a.back, a.shown, a.late, vc_dropped, ref.decoded, a.slips);
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

    /* HEVC: the Pi 4's HEVC block (hevc_hwdec), the same way */
    if (argc > 2) {
        run_t href;
        int asked0 = asked;
        play(argv[2], REELCORE_NO_HEVC_BLOCK, &href);
        printf("  HEVC on the ARM: %u decoded, %u shown, decoder %s\n", href.decoded, href.shown, decoder_line(href.info));
        CHECK(asked_hb == 0 && href.end && href.decoded > 20 && href.decoder == REELCORE_DECODER_ARM &&
              strstr(decoder_line(href.info), "hevc, 1 thread"), "HEVC, NO_HEVC_BLOCK: asked %d, end %d, %u decoded, '%s'",
              asked_hb, href.end, href.decoded, decoder_line(href.info));
        play(argv[2], 0, &a);
        printf("  HEVC block: %u decoded, %u shown, output_8bit %d, decoder %s\n", a.decoded, a.shown, output_8bit,
               decoder_line(a.info));
        CHECK(asked_hb == 1 && asked == asked0 && a.end && a.decoded == href.decoded && output_8bit == 1 &&
              !strcmp(decoder_line(a.info), "HEVC block (hevc_hwdec)") && a.decoder_early == REELCORE_DECODER_HEVC_BLOCK &&
              a.decoder == REELCORE_DECODER_HEVC_BLOCK,
              "HEVC block: asked %d (h264_vchiq %d), end %d, %u of %u decoded, output_8bit %d, '%s', stats %d %d", asked_hb,
              asked - asked0, a.end, a.decoded, href.decoded, output_8bit, decoder_line(a.info), a.decoder_early, a.decoder);
        CHECK(!vc_dropped && !a.slips, "HEVC block, keeping up: %d dropped, %u clock slips", vc_dropped, a.slips);
        /* a slow machine (60 ms a picture shown, at 25 fps): while behind
           the non-reference pictures not decoded at all (skip_frame nonref,
           devkit 0.2.11), never keyframes only (skip_frame nonkey left 4K
           on the Pi keyframes only); late ones passed over in reelcore's
           queue (unconverted: output_hw), given back by the decoder
           (drop_before) only when half a second late */
        slow = 0.06;
        play(argv[2], 0, &a);
        slow = 0;
        printf("  HEVC block, slow: %u shown, %u late, %d dropped unconverted, %u not decoded, %u skip spells "
               "(at most level %d), end %d\n", a.shown, a.late, vc_dropped, href.decoded - a.decoded - vc_dropped,
               a.skip_spells, a.max_skip, a.end);
        CHECK(a.end && !a.back && a.max_skip == 1 && !a.slips && a.shown + a.late == a.decoded &&
              a.decoded + vc_dropped < href.decoded,
              "HEVC block, slow: end %d, back %d, %u shown + %u late of %u decoded + %d dropped of %u, skip level %d, "
              "%u slips", a.end, a.back, a.shown, a.late, a.decoded, vc_dropped, href.decoded, a.max_skip, a.slips);
#ifdef REELCORE_HEVCDEC
        /* output_hw: each picture shown converted straight into the caller's
           planes, 1:1 or halved, or copied for 32bpp; the same pictures as
           FFmpeg's hevc on the ARM gives; every frame given back */
        {
            static const char *how[4] = { "", "1:1 (overlay)", "halved (4K into an HD overlay)", "32bpp (sprite)" };
            for (draw_mode = 1; draw_mode <= 3; draw_mode++) {
                run_t r1, r2;
                int c0 = to_i420_calls, h0 = to_half_calls;
                play(argv[2], REELCORE_NO_HEVC_BLOCK, &r1);
                play(argv[2], 0, &r2);
                printf("  HEVC block, output_hw %d, drawn %s: %u and %u drawn, sums %08x %08x, %d 1:1 and %d halved "
                       "conversions, %d frames held at the end\n", output_hw, how[draw_mode], r1.drawn, r2.drawn, r1.crc,
                       r2.crc, to_i420_calls - c0, to_half_calls - h0, hw_live);
                CHECK(output_hw == 1 && r2.drawn == r1.drawn && r2.drawn > 20 && r2.crc == r1.crc && !hw_live &&
                      (draw_mode == 2 ? to_half_calls - h0 == (int)r2.drawn : to_i420_calls - c0 >= (int)r2.drawn),
                      "output_hw, %s: %u/%u drawn, sums %08x/%08x, %d+%d conversions, %d held", how[draw_mode], r2.drawn,
                      r1.drawn, r2.crc, r1.crc, to_i420_calls - c0, to_half_calls - h0, hw_live);
            }
            /* the stats panel on: the same pictures again, each pixel
               converted once, every part from one of the block's columns:
               the panel's rectangle into cached memory, blended there and
               written over (the overlay never read back: 4K stuttered when
               it was blended in place), the rest straight in (all of it
               through cached memory took 4K 10-bit from 9.5 to 18 ms) */
            for (int k = 1; k <= 2; k++)
                for (draw_mode = 1; draw_mode <= 2; draw_mode++) {
                    run_t r0, r1, r2;
                    int c0, h0;
                    panel_on = 0;
                    play(argv[2], REELCORE_NO_HEVC_BLOCK, &r0);
                    panel_on = k;
                    play(argv[2], REELCORE_NO_HEVC_BLOCK, &r1);
                    c0 = to_i420_calls; h0 = to_half_calls; half_refused = 0;
                    conv_px = drawn_px = cached_px = 0; conv_x_off = 0;
                    play(argv[2], 0, &r2);
                    panel_on = 0;
                    printf("  HEVC block, panel on (scale %d), drawn %s: %u and %u drawn, sums %08x %08x (no panel %08x), "
                           "%d 1:1 and %d halved conversions, %.2f of each picture converted, %.2f of it through cached memory\n",
                           k, how[draw_mode], r1.drawn, r2.drawn, r1.crc, r2.crc, r0.crc, to_i420_calls - c0,
                           to_half_calls - h0, drawn_px ? (double)conv_px / drawn_px : 0.0,
                           drawn_px ? (double)cached_px / drawn_px : 0.0);
                    CHECK(r2.rect_time > 0 && r1.rect_time == 0, "the panel's rectangle timed: %.6f s (on the ARM: %.6f)",
                          r2.rect_time, r1.rect_time);
                    CHECK(r2.drawn == r1.drawn && r2.drawn > 20 && r2.crc == r1.crc && r1.crc != r0.crc && !hw_live && !half_refused &&
                          conv_px == drawn_px && !conv_x_off && cached_px > 0 && cached_px * 10 < drawn_px * 9 &&
                          (draw_mode == 2 ? to_i420_calls == c0 && to_half_calls - h0 <= 5 * (int)r2.drawn
                                          : to_half_calls == h0 && to_i420_calls - c0 <= 5 * (int)r2.drawn),
                          "panel on, scale %d, %s: %u/%u drawn, sums %08x/%08x (no panel %08x), %d+%d conversions, %d refused, "
                          "%ld of %ld pixels converted, %d not from a column", k, how[draw_mode], r2.drawn, r1.drawn, r2.crc, r1.crc,
                          r0.crc, to_i420_calls - c0, to_half_calls - h0, half_refused, conv_px, drawn_px, conv_x_off);
                }
            /* the block's latency 0.06 s (fine while pictures are queued
               0.16 s ahead), and the desktop busy for 0.3 s 0.5 s in: the
               queue empties, and the picture due is the one just handed
               out. Converting it then would wait for it (its latency, with
               nothing given to the block meanwhile: on the Pi, 4K 10-bit
               with the panel went to 14 a second and stayed there); a due
               picture not yet done is left a moment instead, and the block
               fed. So no conversion waits (but the first picture's) */
            {
                run_t r3;
                int w0;
                ReelCoreStats st0;
                (void)st0;
                draw_mode = 2;
                hb_latency = 0.06;
                hiccup_at = 0.5;
                conv_waited = 0;
                idle_long = 0;
                play(argv[2], 0, &r3);
                w0 = conv_waited;
                hb_latency = 0;
                hiccup_at = -1;
                printf("  HEVC block, 0.06 s latency, a 0.3 s stall: %u drawn, %d conversions waited, end %d\n",
                       r3.drawn, w0, r3.end);
                /* (and the log's figures: each conversion counted once, busy
                   or idle, some with the block on the next one) */
                printf("  ... converted %u with the block busy, %u idle\n", r3.conv_busy, r3.conv_idle);
                CHECK(r3.conv_busy + r3.conv_idle == r3.drawn && r3.conv_busy > 0 && r3.conv_idle > 0,
                      "conversions counted: %u busy + %u idle of %u drawn", r3.conv_busy, r3.conv_idle, r3.drawn);
                CHECK(r3.drawn > 10 && w0 <= 1 && r3.end && !hw_live && !idle_long,
                      "HEVC block not done when due: %u drawn, %d conversions waited (want at most the first's), end %d, "
                      "%d held, %d sleeps of 10 ms or more while a picture was left", r3.drawn, w0, r3.end, hw_live, idle_long);
            }
            /* the block only just keeping up (a picture out each picture's
               time, as 4K 10-bit at 60 fps on the Pi) with its 0.06 s
               latency, no sound, and the desktop busy 0.3 s: behind for
               good, pictures out late, most given back unconverted (on the
               Pi, 9 a second from then on; here 55 of 150 shown).
               - argv[3], half its pictures non-reference (B): those aren't
                 given to the block while behind (skip_frame nonref, devkit
                 0.2.11), so it gets ahead again (a clock slip at most);
               - argv[4], P pictures only (nothing to skip): the clock is
                 moved back instead, once, to a little before the next
                 picture (moved to it, it was soon behind again: 7 times);
               - argv[5], the same at 60 fps with a keyframe every 12
                 pictures, as the owner's 4K test clip (VideoToolbox): on
                 the Pi it stayed 0.08-0.14 s behind, every picture given
                 back, 14 a second shown, never the 0.1 s behind for long
                 enough to slip; now two pictures behind is enough;
               - and with a shorter stall and a longer latency (0.1 s):
                 behind is judged by the picture shown, not the newest out
                 (on the Pi the newest was hardly late, those shown 0.07-0.1
                 s late: 15-20 a second for good). (This fake doesn't
                 reproduce that: judged by the newest, it slips here too.)
               Either way, from 3 s on every picture is shown on time, and
               none given back unconverted by the decoder (a little late,
               passed over in reelcore's queue instead, at no cost: given
               back two pictures late, the Pi showed 16-18 a second where
               it could convert 30).
               (6 s clips.) */
            for (int k = 3; k <= 6; k++) {
                static const char *kind[7] = { "", "", "", "B pictures", "P only", "P only, 60 fps, a keyframe every 12",
                                               "the same, newest picture hardly late" };
                static const double stall[7] = { 0, 0, 0, 0.3, 0.3, 0.2, 0.15 };
                int c = k == 6 ? 5 : k;
                run_t r4, r5;
                double fps = c == 5 ? 60 : 25;
                if (c >= argc) break;
                play(argv[c], REELCORE_NO_HEVC_BLOCK, &r5);
                hb_period = 1 / fps;
                hiccup_at = 1.0;
                hiccup_len = stall[k];               /* (60 fps: left a little behind, under 0.1 s) */
                hb_latency = k == 6 ? 0.1 : 0.06; conv_waited = 0; draw_mode = 2;
                play(argv[c], 0, &r4);
                hb_latency = 0;
                hb_period = 0;
                hiccup_at = -1;
                hiccup_len = 0.3;
                printf("  HEVC block only just keeping up (%s), a %.2f s stall: %u shown, %u late, %d dropped unconverted, "
                       "%u not decoded, of %u; %u skip spells, %u clock slips, from 3 s on shown up to %.3f s late, end %d\n",
                       kind[k], stall[k], r4.shown, r4.late, vc_dropped, r5.decoded - r4.decoded - vc_dropped,
                       r5.decoded, r4.skip_spells, r4.slips, r4.lag_late, r4.end);
                CHECK(r4.end && !r4.back && r5.decoded > 100 && r4.lag_late < 1.5 / fps &&
                      (c == 3 ? r4.slips <= 1 && r4.skip_spells >= 1 && r4.decoded + vc_dropped < r5.decoded &&
                                r4.shown + r4.late + vc_dropped + 30 >= r4.decoded + vc_dropped
                              : r4.slips == 1 && !vc_dropped && r4.decoded == r5.decoded && r4.shown + fps * 0.9 >= r5.decoded),
                      "only just keeping up (%s): %u shown, %u decoded of %u, %u skip spells, %u clock slips, %.3f s late "
                      "(want under 1.5 pictures), end %d, back %d", kind[k], r4.shown, r4.decoded,
                      r5.decoded, r4.skip_spells, r4.slips, r4.lag_late, r4.end, r4.back);
                asked_hb--;
            }
            draw_mode = 0;
            asked_hb -= 8;                       /* (eight more uses of the block above) */
        }
#endif
        refuse_open = 1;
        play(argv[2], 0, &a);
        printf("  HEVC block refused at open: %u decoded, decoder %s\n", a.decoded, decoder_line(a.info));
        CHECK(asked_hb == 3 && a.end && a.decoded == href.decoded && strstr(decoder_line(a.info), "hevc, 1 thread") &&
              strstr(last_log, "HEVC block can't take") && a.decoder == REELCORE_DECODER_ARM,
              "HEVC refused: asked %d, end %d, %u decoded, '%s', '%s'", asked_hb, a.end, a.decoded, decoder_line(a.info), last_log);
        refuse_open = 0;
        /* refused at the first picture (a raw stream, ENOSYS), and failing part way */
        fail_err = AVERROR(ENOSYS);
        fail_after = 1;
        play(argv[2], 0, &a);
        printf("  HEVC block refused at its first picture: %u shown, end %d, decoder %s\n", a.shown, a.end, decoder_line(a.info));
        CHECK(fell_back == 1 && a.end && a.shown + 2 >= href.shown && strstr(decoder_line(a.info), "the HEVC block failed part way") &&
              a.decoder == REELCORE_DECODER_ARM_AFTER, "HEVC refused at the first picture: fell back %d, end %d, %u shown of %u, '%s'",
              fell_back, a.end, a.shown, href.shown, decoder_line(a.info));
        fail_err = AVERROR_EXTERNAL;
        fail_after = href.decoded - 5;       /* (after the stats' early look, at the 10th shown) */
        play(argv[2], 0, &a);
        printf("  HEVC block failed at picture %d: %u shown, end %d, decoder %s\n", fail_after, a.shown, a.end, decoder_line(a.info));
        CHECK(fell_back == 1 && a.end && a.shown > href.shown * 8 / 10 && a.decoder_early == REELCORE_DECODER_HEVC_BLOCK &&
              a.decoder == REELCORE_DECODER_ARM_AFTER, "HEVC failed part way: fell back %d, end %d, %u shown of %u, stats %d %d",
              fell_back, a.end, a.shown, href.shown, a.decoder_early, a.decoder);
        fail_after = -1;
        /* the same two, the error coming back from send_packet (as from the
           real decoders): before, reelcore didn't see it, and the picture
           stopped with no fall back to the ARM */
        fail_in_send = 1;
        fail_err = AVERROR(ENOSYS);
        fail_after = 1;
        play(argv[2], 0, &a);
        printf("  HEVC block refused at its first picture (from send_packet): %u shown, end %d, decoder %s\n", a.shown,
               a.end, decoder_line(a.info));
        CHECK(fell_back == 1 && a.end && a.shown + 2 >= href.shown && a.decoder == REELCORE_DECODER_ARM_AFTER,
              "HEVC refused at the first picture, from send_packet: fell back %d, end %d, %u shown of %u, '%s'",
              fell_back, a.end, a.shown, href.shown, decoder_line(a.info));
        fail_err = AVERROR_EXTERNAL;
        fail_after = href.decoded - 5;
        play(argv[2], 0, &a);
        printf("  HEVC block failed at picture %d (from send_packet): %u shown, end %d, decoder %s\n", fail_after, a.shown,
               a.end, decoder_line(a.info));
        CHECK(fell_back == 1 && a.end && a.shown > href.shown * 8 / 10 && a.decoder == REELCORE_DECODER_ARM_AFTER,
              "HEVC failed part way, from send_packet: fell back %d, end %d, %u shown of %u, stats %d", fell_back, a.end,
              a.shown, href.shown, a.decoder);
        fail_after = -1;
        fail_in_send = 0;
        failed = 0;
    }

    /* the VideoCore failing part way, its error from send_packet */
    fail_in_send = 1;
    fail_err = AVERROR_EXTERNAL;
    fail_after = 40;
    play(argv[1], 0, &a);
    printf("  VideoCore failed at picture 40 (from send_packet): %u shown, decoder %s\n", a.shown, decoder_line(a.info));
    CHECK(fell_back == 1 && a.end && a.shown > ref.shown * 9 / 10 && a.decoder == REELCORE_DECODER_ARM_AFTER,
          "VideoCore failed part way, from send_packet: fell back %d, end %d, %u shown of %u, stats %d", fell_back, a.end,
          a.shown, ref.shown, a.decoder);
    fail_after = -1;
    fail_in_send = 0;

    printf(fails ? "vc_test: %d failures\n" : "vc_test: all passed\n", fails);
    return fails != 0;
}
