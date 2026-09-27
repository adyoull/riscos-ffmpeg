/*
 * Host test of the egl output device (libavdevice/riscos_egl.c) with the
 * fake RISC OS and EGL in fake_riscos.c: decodes a clip, sends its frames
 * to the device as raw video and checks what reached the surface.
 *
 *   egl_outdev_test CLIP
 *
 * Checks, for a desktop window, full screen, direct full screen and both
 * colour orders:
 *   - one swap per frame, the surface unlocked after each;
 *   - the picture: the last frame shown equals the same frame converted and
 *     scaled by swscale separately (bit for bit), in the letterboxed
 *     rectangle, and the borders are black;
 *   - the Wimp task and window are closed at the end, a close request or
 *     Escape stops the output (AVERROR_EXIT).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
#include <EGL/egl.h>
#include "fake_riscos.h"

extern const AVOutputFormat ff_egl_muxer;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* All the frames of the clip's video, as yuv420p. */
static AVFrame **load(const char *clip, int *n)
{
    AVFormatContext *ic = NULL;
    AVCodecContext *dc;
    const AVCodec *codec;
    AVPacket *pkt = av_packet_alloc();
    AVFrame **frames = NULL;
    int vs;
    *n = 0;
    if (avformat_open_input(&ic, clip, NULL, NULL) < 0 || avformat_find_stream_info(ic, NULL) < 0)
        exit(2);
    vs = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    dc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dc, ic->streams[vs]->codecpar);
    avcodec_open2(dc, codec, NULL);
    for (;;) {
        int eof = av_read_frame(ic, pkt) < 0;
        if (!eof && pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(dc, eof ? NULL : pkt);
        av_packet_unref(pkt);
        for (;;) {
            AVFrame *f = av_frame_alloc();
            if (avcodec_receive_frame(dc, f) < 0) { av_frame_free(&f); break; }
            frames = realloc(frames, (*n + 1) * sizeof(*frames));
            frames[(*n)++] = f;
        }
        if (eof) break;
    }
    avcodec_free_context(&dc);
    avformat_close_input(&ic);
    av_packet_free(&pkt);
    return frames;
}

/* Sends frames to the device; returns the first error (0 = all sent). */
static int play(AVFrame **frames, int n, const char *opts, int *sent)
{
    AVFormatContext *oc = NULL;
    AVStream *st;
    AVDictionary *d = NULL;
    AVPacket *pkt = av_packet_alloc();
    int ret, i, size;

    avformat_alloc_output_context2(&oc, &ff_egl_muxer, NULL, "test video");
    st = avformat_new_stream(oc, NULL);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->width = frames[0]->width;
    st->codecpar->height = frames[0]->height;
    st->codecpar->format = frames[0]->format;
    st->time_base = (AVRational){ 1, 25 };
    av_dict_parse_string(&d, opts, "=", ":", 0);
    ret = avformat_write_header(oc, &d);
    av_dict_free(&d);
    *sent = 0;
    size = av_image_get_buffer_size(frames[0]->format, frames[0]->width, frames[0]->height, 1);
    for (i = 0; ret >= 0 && i < n; i++) {
        av_new_packet(pkt, size);
        av_image_copy_to_buffer(pkt->data, size, (const uint8_t * const *)frames[i]->data,
                                frames[i]->linesize, frames[i]->format,
                                frames[i]->width, frames[i]->height, 1);
        pkt->pts = i;
        ret = av_write_frame(oc, pkt);
        av_packet_unref(pkt);
        if (ret >= 0)
            (*sent)++;
    }
    av_write_trailer(oc);
    avformat_free_context(oc);
    av_packet_free(&pkt);
    return ret;
}

/* Compare the shown surface with frame converted separately. */
static void check_picture(AVFrame *f, enum AVPixelFormat fmt, const char *what)
{
    int sw = fake_surf_w, sh = fake_surf_h, w, h, x, y, bad = 0, border = 0;
    uint8_t *want;
    struct SwsContext *c;
    uint8_t *dst[4] = { NULL };
    int ds[4] = { 0 };

    /* the rectangle riscos_egl.c should use (square pixels) */
    h = sh; w = (int)av_rescale(sh, f->width, f->height) & ~1;
    if (w > sw) { w = sw; h = (int)av_rescale(sw, f->height, f->width) & ~1; }
    x = (sw - w) / 2; y = (sh - h) / 2;
    want = malloc(w * h * 4);
    c = sws_getContext(f->width, f->height, f->format, w, h, fmt,
                       (w == f->width && h == f->height) ? SWS_POINT : SWS_FAST_BILINEAR,
                       NULL, NULL, NULL);
    dst[0] = want; ds[0] = w * 4;
    sws_scale(c, (const uint8_t * const *)f->data, f->linesize, 0, f->height, dst, ds);
    sws_freeContext(c);
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++) {
            const uint8_t *p = fake_shown + j * fake_surf_pitch + i * 4;
            if (i >= x && i < x + w && j >= y && j < y + h) {
                if (memcmp(p, want + ((j - y) * w + (i - x)) * 4, 3)) bad++;
            } else if (p[0] | p[1] | p[2])
                border++;
        }
    CHECK(!bad, "%s: %d picture pixels differ", what, bad);
    CHECK(!border, "%s: %d border pixels not black", what, border);
    printf("  %s: %dx%d surface, picture %dx%d at %d,%d\n", what, sw, sh, w, h, x, y);
    free(want);
}

static void reset(void)
{
    fake_swaps = fake_locked = 0;
    fake_close_after = fake_escape_after = -1;
    fake_force_redraws = 0;
}

int main(int argc, char **argv)
{
    int n, sent, ret;
    AVFrame **frames = load(argv[1], &n);
    printf("%d frames of %dx%d\n", n, frames[0]->width, frames[0]->height);

    /* 1. desktop window, 0x00BBGGRR screen */
    reset(); fake_desktop = 1; fake_visual = 0;
    ret = play(frames, n, "", &sent);
    CHECK(ret >= 0 && sent == n && fake_swaps == n, "window: ret %d sent %d swaps %d", ret, sent, fake_swaps);
    CHECK(fake_win_w == frames[0]->width && fake_win_h == frames[0]->height,
          "window: %dx%d, not the video's size", fake_win_w, fake_win_h);
    CHECK(!fake_locked && !fake_tasks_open && !fake_windows_open, "window: not tidied up");
    check_picture(frames[n - 1], AV_PIX_FMT_RGBA, "window, TBGR");

    /* 2. a bigger window (scaling, letterbox), 0x00RRGGBB screen */
    reset(); fake_visual = 0x4000;
    ret = play(frames, n, "window_size=900x700:scale=fast_bilinear", &sent);
    CHECK(ret >= 0 && fake_swaps == n, "big window: ret %d swaps %d", ret, fake_swaps);
    check_picture(frames[n - 1], AV_PIX_FMT_BGRA, "900x700 window, TRGB");

    /* 3. no desktop: full screen, vsync 1 */
    reset(); fake_desktop = 0; fake_visual = 0;
    ret = play(frames, n, "", &sent);
    CHECK(ret >= 0 && fake_swaps == n && fake_swap_interval == 1 && fake_render_buffer == 0,
          "full screen: ret %d swaps %d interval %d", ret, fake_swaps, fake_swap_interval);
    check_picture(frames[n - 1], AV_PIX_FMT_RGBA, "full screen (no desktop)");

    /* 4. full screen from the desktop, direct, no vsync; repaints the desktop after */
    reset(); fake_desktop = 1;
    ret = play(frames, n, "fullscreen=1:direct=1:vsync=0", &sent);
    CHECK(ret >= 0 && fake_render_buffer == EGL_SINGLE_BUFFER && fake_swap_interval == 0,
          "direct: ret %d buffer 0x%x interval %d", ret, fake_render_buffer, fake_swap_interval);
    CHECK(fake_force_redraws == 1 && !fake_tasks_open, "direct: desktop not repainted (%d)", fake_force_redraws);
    check_picture(frames[n - 1], AV_PIX_FMT_RGBA, "full screen direct");

    /* 5. the close icon stops it */
    reset(); fake_desktop = 1; fake_close_after = 3;
    ret = play(frames, n, "", &sent);
    CHECK(ret == AVERROR_EXIT && sent == 3 && !fake_windows_open, "close: ret %d sent %d", ret, sent);

    /* 6. Escape stops full screen */
    reset(); fake_desktop = 0; fake_escape_after = 5;
    ret = play(frames, n, "", &sent);
    CHECK(ret == AVERROR_EXIT && sent == 5, "escape: ret %d sent %d", ret, sent);

    /* 7. in a TaskWindow: no window possible; an error, not a surprise full
       screen. With -fullscreen 1 it plays. */
    reset(); fake_desktop = 1; fake_taskwindow = 1;
    ret = play(frames, n, "", &sent);
    CHECK(ret == AVERROR(EINVAL) && fake_swaps == 0 && !fake_windows_open && !fake_tasks_open,
          "taskwindow: ret %d swaps %d", ret, fake_swaps);
    reset();
    ret = play(frames, n, "fullscreen=1", &sent);
    CHECK(ret >= 0 && fake_swaps == n, "taskwindow full screen: ret %d swaps %d", ret, fake_swaps);
    fake_taskwindow = 0;
    printf("  TaskWindow: a clear error without -fullscreen 1, plays with it\n");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
