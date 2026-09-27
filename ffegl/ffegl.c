/*
 * ffegl.c - play videos into riscos-mesa's EGL and OpenGL, with FFmpeg.
 * See ffegl.h. Part of riscos-ffmpeg. LGPL 2.1 or later.
 */
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "ffegl.h"

#define QMAX          8      /* decoded frames kept ahead of the clock */
#define AUDIO_AHEAD   0.25   /* seconds of sound kept queued */
#define READ_BUDGET   64     /* packets read per ffegl_update at most */

struct FFEGLVideo {
    int flags;
    AVFormatContext *fmt;
    int vs, as;                        /* stream indexes, -1 = none */
    AVCodecContext *vdec, *adec;
    AVPacket *pkt;
    AVFrame *frame;                    /* decoding scratch */
    int w, h;                          /* display size (aspect applied) */
    double fps, duration;

    /* decoded frames waiting for their time */
    AVFrame *q[QMAX];
    double qpts[QMAX];
    int qn;
    AVFrame *cur;                      /* the current frame (NULL before the first) */
    double cur_pts;
    int need_first;                    /* show the next frame at once, and sync to it */
    double seek_target;                /* drop pictures before this; <0 = none */
    double aseek_target;               /* and sound */

    int eof_demux, eof_video, eof_audio;

    /* sound */
    SDL_AudioDeviceID dev;
    SwrContext *swr;
    int rate, bytes_per_sec;
    double audio_end;                  /* pts at the end of the queued sound; <0 = unknown */
    int audio_clock;                   /* the sound is the clock */
    double latency;                    /* the device's own buffer, seconds */
    uint8_t *abuf, *mixbuf;
    int abuf_size;
    int volume;                        /* 0..SDL_MIX_MAXVOLUME */

    /* timer clock (no sound, or after the sound ended) */
    int64_t t0;                        /* av_gettime_relative() at pts 0 */
    int paused;
    double pause_pos;

    /* conversion */
    struct SwsContext *sws;
    int cs_key[8];
    uint8_t *rgba;                     /* for textures: glTexSubImage2D copies from here */
    int tex_w, tex_h;
    unsigned int tex;

    /* textures through EGLImage (riscos-mesa 7pre12+): the texture uses a
       sprite's pixels and swscale writes each frame straight into them */
    int img_state;                     /* 0 = not checked, 1 = usable, -1 = not */
    uint8_t *spr_mem;                  /* allocation: sprite header at +4, pixels at +48 */
    EGLDisplay img_dpy;
    EGLImageKHR img;
    int img_bgr;                       /* sprite colour order: 0 TBGR (R,G,B,x), 1 TRGB */
};

static char last_error[256];

static void set_error(const char *fmt, const char *arg)
{
    snprintf(last_error, sizeof(last_error), fmt, arg);
    av_log(NULL, AV_LOG_ERROR, "ffegl: %s\n", last_error);
}

const char *ffegl_last_error(void) { return last_error; }


/* ---------------------------------------------------------------- clock */

static double queued_audio(const FFEGLVideo *v)
{
    return v->dev ? SDL_GetQueuedAudioSize(v->dev) / (double)v->bytes_per_sec : 0;
}

static double clock_now(FFEGLVideo *v)
{
    if (v->paused)
        return v->pause_pos;
    if (v->audio_clock && v->audio_end >= 0) {
        double q = queued_audio(v);
        if (v->eof_audio && q <= 0) {
            /* the sound has finished: carry on with the timer */
            double c = v->audio_end - v->latency;
            v->audio_clock = 0;
            v->t0 = av_gettime_relative() - (int64_t)(c * 1e6);
            return c;
        }
        return v->audio_end - q - v->latency;
    }
    return (av_gettime_relative() - v->t0) / 1e6;
}

/* Make the timer read pts now. */
static void timer_set(FFEGLVideo *v, double pts)
{
    v->t0 = av_gettime_relative() - (int64_t)(pts * 1e6);
}

/* ---------------------------------------------------------------- open */

static AVCodecContext *open_decoder(AVStream *st)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *c;
    if (!codec || !(c = avcodec_alloc_context3(codec)))
        return NULL;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    c->pkt_timebase = st->time_base;
    c->thread_count = 1;              /* one core; no point in threads */
    if (avcodec_open2(c, codec, NULL) < 0)
        avcodec_free_context(&c);
    return c;
}

static int open_audio(FFEGLVideo *v)
{
    SDL_AudioSpec want, have;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: no sound (%s)\n", SDL_GetError());
        return -1;
    }
    memset(&want, 0, sizeof(want));
    want.freq = v->adec->sample_rate > 0 ? v->adec->sample_rate : 44100;
    if (want.freq != 44100 && want.freq != 48000 && want.freq != 22050)
        want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    v->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!v->dev) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: no sound (%s)\n", SDL_GetError());
        return -1;
    }
    v->rate = have.freq;
    v->bytes_per_sec = have.freq * 4;
    v->latency = have.samples / (double)have.freq;
    if (swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_S16, v->rate,
                            &v->adec->ch_layout, v->adec->sample_fmt, v->adec->sample_rate,
                            0, NULL) < 0 || swr_init(v->swr) < 0) {
        SDL_CloseAudioDevice(v->dev);
        v->dev = 0;
        return -1;
    }
    return 0;
}

FFEGLVideo *ffegl_open(const char *url, int flags)
{
    FFEGLVideo *v = av_mallocz(sizeof(*v));
    AVStream *st;
    int ret;

    if (!v)
        return NULL;
    v->flags = flags;
    v->vs = v->as = -1;
    v->audio_end = -1;
    v->seek_target = v->aseek_target = -1;
    v->need_first = 1;
    v->volume = SDL_MIX_MAXVOLUME;
    v->cs_key[0] = -1;
    if ((ret = avformat_open_input(&v->fmt, url, NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(v->fmt, NULL)) < 0) {
        char e[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, e, sizeof(e));
        set_error("can't open the file: %s", e);
        goto fail;
    }
    v->vs = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v->vs < 0) {
        set_error("%s", "no video in the file");
        goto fail;
    }
    st = v->fmt->streams[v->vs];
    if (!(v->vdec = open_decoder(st))) {
        set_error("no decoder for the video (%s)", avcodec_get_name(st->codecpar->codec_id));
        goto fail;
    }
    v->h = v->vdec->height;
    v->w = v->vdec->width;
    {
        AVRational sar = av_guess_sample_aspect_ratio(v->fmt, st, NULL);
        if (sar.num > 0 && sar.den > 0)
            v->w = (int)av_rescale(v->w, sar.num, sar.den);
    }
    {
        AVRational fr = av_guess_frame_rate(v->fmt, st, NULL);
        v->fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
    }
    v->duration = v->fmt->duration > 0 ? v->fmt->duration / (double)AV_TIME_BASE : 0;

    if (!(flags & FFEGL_NO_AUDIO)) {
        v->as = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, v->vs, NULL, 0);
        if (v->as >= 0 && (!(v->adec = open_decoder(v->fmt->streams[v->as])) || open_audio(v) < 0)) {
            avcodec_free_context(&v->adec);
            v->as = -1;
        }
    }
    v->audio_clock = v->dev != 0;
    for (unsigned i = 0; i < v->fmt->nb_streams; i++)
        if ((int)i != v->vs && (int)i != v->as)
            v->fmt->streams[i]->discard = AVDISCARD_ALL;
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    if (!v->pkt || !v->frame)
        goto fail;
    if (flags & FFEGL_PAUSED) {
        v->paused = 1;
        v->pause_pos = 0;
    } else if (v->dev)
        SDL_PauseAudioDevice(v->dev, 0);
    timer_set(v, 0);
    return v;

fail:
    ffegl_close(v);
    return NULL;
}

static void clear_queue(FFEGLVideo *v)
{
    for (int i = 0; i < v->qn; i++)
        av_frame_free(&v->q[i]);
    v->qn = 0;
}

static void texture_image_free(FFEGLVideo *v, int unlink);

void ffegl_close(FFEGLVideo *v)
{
    if (!v)
        return;
    if (v->dev)
        SDL_CloseAudioDevice(v->dev);
    clear_queue(v);
    av_frame_free(&v->cur);
    av_frame_free(&v->frame);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->vdec);
    avcodec_free_context(&v->adec);
    avformat_close_input(&v->fmt);
    swr_free(&v->swr);
    sws_freeContext(v->sws);
    av_free(v->abuf);
    av_free(v->mixbuf);
    av_free(v->rgba);
    texture_image_free(v, 1);
    av_free(v);
}

int ffegl_width(const FFEGLVideo *v)        { return v->w; }
int ffegl_height(const FFEGLVideo *v)       { return v->h; }
double ffegl_frame_rate(const FFEGLVideo *v) { return v->fps; }
double ffegl_duration(const FFEGLVideo *v)  { return v->duration; }
int ffegl_has_audio(const FFEGLVideo *v)    { return v->dev != 0; }
double ffegl_position(const FFEGLVideo *v)
{
    double start = v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time / (double)AV_TIME_BASE : 0;
    return v->cur ? v->cur_pts - start : 0;
}
int ffegl_paused(const FFEGLVideo *v)       { return v->paused; }

void ffegl_set_volume(FFEGLVideo *v, double volume)
{
    v->volume = (int)(av_clipd(volume, 0, 1) * SDL_MIX_MAXVOLUME + 0.5);
}

/* ---------------------------------------------------------------- decode */

static double frame_pts(AVFrame *f, AVStream *st, double fallback)
{
    int64_t t = f->best_effort_timestamp;
    return t == AV_NOPTS_VALUE ? fallback : t * av_q2d(st->time_base);
}

static void got_video(FFEGLVideo *v, AVFrame *f)
{
    double pts = frame_pts(f, v->fmt->streams[v->vs],
                           v->qn ? v->qpts[v->qn - 1] + (v->fps > 0 ? 1 / v->fps : 0.04)
                                 : v->cur ? v->cur_pts : 0);
    if (v->seek_target >= 0) {
        if (pts < v->seek_target - 0.001)
            return;                       /* before the seek point */
        v->seek_target = -1;
    }
    if (v->qn == QMAX) {                  /* full: the oldest goes */
        av_frame_free(&v->q[0]);
        memmove(v->q, v->q + 1, (QMAX - 1) * sizeof(v->q[0]));
        memmove(v->qpts, v->qpts + 1, (QMAX - 1) * sizeof(v->qpts[0]));
        v->qn--;
    }
    v->q[v->qn] = av_frame_clone(f);
    v->qpts[v->qn] = pts;
    if (v->q[v->qn])
        v->qn++;
}

static void got_audio(FFEGLVideo *v, AVFrame *f)
{
    int out_max = swr_get_out_samples(v->swr, f->nb_samples);
    int n, bytes;
    double pts = frame_pts(f, v->fmt->streams[v->as], v->audio_end >= 0 ? v->audio_end : 0);

    if (v->aseek_target >= 0) {
        if (pts + f->nb_samples / (double)f->sample_rate < v->aseek_target)
            return;                       /* before the seek point */
        v->aseek_target = -1;
    }
    if (out_max <= 0)
        return;
    av_fast_malloc(&v->abuf, (unsigned *)&v->abuf_size, out_max * 4);
    if (!v->abuf)
        return;
    n = swr_convert(v->swr, &v->abuf, out_max, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n <= 0)
        return;
    bytes = n * 4;
    if (v->audio_end < 0)
        v->audio_end = pts;
    if (v->volume < SDL_MIX_MAXVOLUME) {
        uint8_t *m = av_mallocz(bytes);
        if (m) {
            SDL_MixAudioFormat(m, v->abuf, AUDIO_S16SYS, bytes, v->volume);
            SDL_QueueAudio(v->dev, m, bytes);
            av_free(m);
        }
    } else
        SDL_QueueAudio(v->dev, v->abuf, bytes);
    v->audio_end += n / (double)v->rate;
}

/* Sends pkt (NULL = flush) to a decoder and takes all it gives back. */
static void decode(FFEGLVideo *v, AVCodecContext *dec, AVPacket *pkt, int video)
{
    int ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
        return;                            /* a damaged packet: skip it */
    for (;;) {
        ret = avcodec_receive_frame(dec, v->frame);
        if (ret == AVERROR_EOF) {
            if (video) v->eof_video = 1; else v->eof_audio = 1;
            return;
        }
        if (ret < 0)
            return;
        if (video) got_video(v, v->frame); else got_audio(v, v->frame);
        av_frame_unref(v->frame);
    }
}

/* Reads and decodes until a few frames and a little sound are ready. */
static void fill(FFEGLVideo *v)
{
    for (int budget = READ_BUDGET; budget > 0 && !v->eof_demux; budget--) {
        int need_v = v->qn < 3;
        int need_a = v->dev && queued_audio(v) < AUDIO_AHEAD;
        int ret;
        if (!need_v && !need_a)
            break;
        if (need_a && !need_v && v->qn >= QMAX - 1)
            break;                         /* don't throw pictures away for sound */
        ret = av_read_frame(v->fmt, v->pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0) {                     /* the end: flush the decoders */
            v->eof_demux = 1;
            decode(v, v->vdec, NULL, 1);
            if (v->adec) decode(v, v->adec, NULL, 0); else v->eof_audio = 1;
            break;
        }
        if (v->pkt->stream_index == v->vs)
            decode(v, v->vdec, v->pkt, 1);
        else if (v->pkt->stream_index == v->as)
            decode(v, v->adec, v->pkt, 0);
        av_packet_unref(v->pkt);
    }
    if (!v->adec)
        v->eof_audio = v->eof_demux;
}

static void take_frame(FFEGLVideo *v)
{
    av_frame_free(&v->cur);
    v->cur = v->q[0];
    v->cur_pts = v->qpts[0];
    memmove(v->q, v->q + 1, (v->qn - 1) * sizeof(v->q[0]));
    memmove(v->qpts, v->qpts + 1, (v->qn - 1) * sizeof(v->qpts[0]));
    v->qn--;
}

int ffegl_update(FFEGLVideo *v)
{
    double now;

    fill(v);
    if (v->need_first) {
        if (!v->qn)
            goto end_check;
        take_frame(v);
        v->need_first = 0;
        /* the timer starts at this frame; the sound clock starts itself */
        if (v->paused)
            v->pause_pos = v->cur_pts;
        else if (!v->audio_clock || v->audio_end < 0)
            timer_set(v, v->cur_pts);
        return FFEGL_NEW_FRAME;
    }
    if (v->paused)
        return FFEGL_SAME_FRAME;

    now = clock_now(v);
    if (v->qn && v->qpts[0] <= now + 0.005) {
        /* skip the frames that are already late */
        while (v->qn > 1 && v->qpts[1] <= now)
            take_frame(v);
        take_frame(v);
        return FFEGL_NEW_FRAME;
    }

end_check:
    if (v->eof_demux && v->eof_video && !v->qn && queued_audio(v) <= 0) {
        if ((v->flags & FFEGL_LOOP) && v->cur) {
            if (ffegl_seek(v, 0) < 0)
                return FFEGL_END;
            return ffegl_update(v);
        }
        return FFEGL_END;
    }
    return FFEGL_SAME_FRAME;
}

void ffegl_pause(FFEGLVideo *v, int paused)
{
    paused = !!paused;
    if (paused == v->paused)
        return;
    if (paused) {
        v->pause_pos = v->cur ? clock_now(v) : 0;
        v->paused = 1;
        if (v->dev)
            SDL_PauseAudioDevice(v->dev, 1);
    } else {
        v->paused = 0;
        if (!v->audio_clock || v->audio_end < 0)
            timer_set(v, v->pause_pos);
        if (v->dev)
            SDL_PauseAudioDevice(v->dev, 0);
    }
}

int ffegl_seek(FFEGLVideo *v, double seconds)
{
    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    int ret;
    if (v->fmt->start_time != AV_NOPTS_VALUE)
        ts += v->fmt->start_time;
    ret = avformat_seek_file(v->fmt, -1, INT64_MIN, ts, ts, 0);
    if (ret < 0)
        return ret;
    avcodec_flush_buffers(v->vdec);
    if (v->adec)
        avcodec_flush_buffers(v->adec);
    if (v->dev)
        SDL_ClearQueuedAudio(v->dev);
    if (v->swr)
        swr_init(v->swr);                 /* drop what it buffered */
    clear_queue(v);
    v->eof_demux = v->eof_video = v->eof_audio = 0;
    v->audio_end = -1;
    v->audio_clock = v->dev != 0;
    v->seek_target = v->aseek_target = seconds > 0 ? ts / (double)AV_TIME_BASE : -1;
    v->need_first = 1;
    return 0;
}

/* ---------------------------------------------------------------- draw */

/* Converts the current frame into w x h pixels at dst (fmt), with the
   frame's colour space and range. */
static int convert(FFEGLVideo *v, uint8_t *dst, int pitch, int w, int h, enum AVPixelFormat fmt)
{
    AVFrame *f = v->cur;
    uint8_t *d[4] = { dst };
    int ds[4] = { pitch };
    int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    int key[8] = { cs, full, f->width, f->height, f->format, w, h, fmt };

    v->sws = sws_getCachedContext(v->sws, f->width, f->height, f->format, w, h, fmt,
                                  (w == f->width && h == f->height) ? SWS_POINT : SWS_FAST_BILINEAR,
                                  NULL, NULL, NULL);
    if (!v->sws)
        return AVERROR(ENOMEM);
    if (memcmp(key, v->cs_key, sizeof(key))) {
        int *inv, *tab, sr, dr, b, c, s;
        if (sws_getColorspaceDetails(v->sws, &inv, &sr, &tab, &dr, &b, &c, &s) >= 0)
            sws_setColorspaceDetails(v->sws, sws_getCoefficients(cs), full, tab, dr, b, c, s);
        memcpy(v->cs_key, key, sizeof(key));
    }
    /* (swscale's arm NEON converters used to return 0 lines; only < 0 is an error) */
    return sws_scale(v->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height, d, ds) < 0
           ? AVERROR_EXTERNAL : 0;
}

static void fill_black(uint8_t *p, int pitch, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        memset(p + (y + j) * pitch + x * 4, 0, (size_t)w * 4);
}

int ffegl_draw_pixels(FFEGLVideo *v, void *pixels, int pitch, int w, int h, int bgr, int flags)
{
    uint8_t *p = pixels;
    int x = 0, y = 0, rw = w, rh = h;

    if (!v->cur)
        return AVERROR(EAGAIN);
    if (w < 1 || h < 1)
        return AVERROR(EINVAL);
    if (!(flags & FFEGL_STRETCH)) {
        rh = h;
        rw = (int)av_rescale(h, v->w, v->h);
        if (rw > w) {
            rw = w;
            rh = (int)av_rescale(w, v->h, v->w);
        }
        rw = FFMAX(rw, 1);
        rh = FFMAX(rh, 1);
        x = (w - rw) / 2;
        y = (h - rh) / 2;
        if (!(flags & FFEGL_NO_BORDERS)) {
            fill_black(p, pitch, 0, 0, w, y);
            fill_black(p, pitch, 0, y + rh, w, h - y - rh);
            fill_black(p, pitch, 0, y, x, rh);
            fill_black(p, pitch, x + rw, y, w - x - rw, rh);
        }
    }
    /* RGBA/BGRA: the fourth byte isn't shown, and these get swscale's NEON */
    return convert(v, p + y * pitch + x * 4, pitch, rw, rh,
                   bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA);
}

int ffegl_draw_surface(FFEGLVideo *v, EGLDisplay dpy, EGLSurface surf,
                       int x, int y, int w, int h, int flags)
{
    EGLint lock_attr[] = { EGL_LOCK_USAGE_HINT_KHR, EGL_WRITE_SURFACE_BIT_KHR, EGL_NONE };
    EGLAttribKHR ptr = 0;
    EGLint pitch = 0, sw = 0, sh = 0, blue = 16;
    int ret;

    if (!v->cur)
        return AVERROR(EAGAIN);
    if (!eglLockSurfaceKHR(dpy, surf, lock_attr))
        return AVERROR_EXTERNAL;
    eglQuerySurface64KHR(dpy, surf, EGL_BITMAP_POINTER_KHR, &ptr);
    eglQuerySurface(dpy, surf, EGL_BITMAP_PITCH_KHR, &pitch);
    eglQuerySurface(dpy, surf, EGL_BITMAP_PIXEL_BLUE_OFFSET_KHR, &blue);
    eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = sw;
        h = sh;
    }
    if (!ptr || x < 0 || y < 0 || x + w > sw || y + h > sh)
        ret = AVERROR(EINVAL);
    else
        ret = ffegl_draw_pixels(v, (uint8_t *)ptr + y * pitch + x * 4, pitch, w, h,
                                blue == 0, flags);   /* blue in the low byte: 0x00RRGGBB */
    eglUnlockSurfaceKHR(dpy, surf);
    return ret;
}

/* ---- textures ---------------------------------------------------------- */

typedef EGLImageKHR (*create_image_fn)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*destroy_image_fn)(EGLDisplay, EGLImageKHR);
typedef void (*target_texture_fn)(GLenum, void *);
static create_image_fn create_image;
static destroy_image_fn destroy_image;
static target_texture_fn target_texture;

#define SPRITE_HDR   44
#define SPRITE_TBGR  (1 | (90 << 1) | (90 << 14) | (6 << 27))   /* type 6, 0x00BBGGRR */
/* 0x00RRGGBB: a mode selector asking for 32bpp with ModeFlags TRGB, as
   riscos-mesa uses for its own TRGB sprites. The sprite is never plotted. */
static int trgb_selector[] = {
    1, 640, 480, 5, -1,         /* flags, x, y, log2bpp, frame rate */
    0, 0x4000,                  /* ModeFlags: TRGB */
    3, -1,                      /* NColour: 16M */
    4, 1, 5, 1,                 /* X/YEigFactor */
    -1
};

static int has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
    return 0;
}

/* Whether the current context can texture from EGLImages (GL_OES_EGL_image
   and EGL_KHR_image_pixmap); checked once per video. FFEGL_NO_EGLIMAGE=1 in
   the environment turns it off (then every frame is copied). */
static int image_usable(FFEGLVideo *v)
{
    if (!v->img_state) {
        const char *no = getenv("FFEGL_NO_EGLIMAGE");
        EGLDisplay dpy = eglGetCurrentDisplay();
        v->img_state = -1;
        if (no && *no && *no != '0')
            return 0;
        if (dpy == EGL_NO_DISPLAY || !has_word((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image") ||
            !has_word(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_image_pixmap"))
            return 0;
        create_image   = (create_image_fn)eglGetProcAddress("eglCreateImageKHR");
        destroy_image  = (destroy_image_fn)eglGetProcAddress("eglDestroyImageKHR");
        target_texture = (target_texture_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!create_image || !destroy_image || !target_texture)
            return 0;
        v->img_dpy = dpy;
        v->img_state = 1;
    }
    return v->img_state > 0;
}

/* The colour order of the current context's config (EGL_NATIVE_VISUAL_ID),
   so the texture has the same byte order as what it is drawn into. */
static int context_bgr(EGLDisplay dpy)
{
    EGLint id = 0, n = 0, visual = 0;
    EGLConfig cfg;
    EGLint attr[] = { EGL_CONFIG_ID, 0, EGL_NONE };
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || !eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id))
        return 0;
    attr[1] = id;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1 ||
        !eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &visual))
        return 0;
    return (visual & 0x4000) != 0;
}

/* Ends the texture's use of the sprite, destroys the image and frees the
   sprite, in that order. unlink: re-specify the texture first (it is given
   one black texel) if it still exists in the current context. */
static void texture_image_free(FFEGLVideo *v, int unlink)
{
    if (!v->spr_mem)
        return;
    if (unlink && v->tex && eglGetCurrentContext() != EGL_NO_CONTEXT && glIsTexture(v->tex)) {
        static const uint8_t black[4];
        GLint old = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
        glBindTexture(GL_TEXTURE_2D, v->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture(GL_TEXTURE_2D, old);
    }
    if (v->img != EGL_NO_IMAGE_KHR && destroy_image)
        destroy_image(v->img_dpy, v->img);
    v->img = EGL_NO_IMAGE_KHR;
    av_freep(&v->spr_mem);
    v->tex = 0;
}

static void set_texture_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* level 0 only */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Makes a sprite of the frame's size and binds it to tex through an
   EGLImage. 0, or < 0 if the image can't be made (then copies are used). */
static int texture_image_bind(FFEGLVideo *v, AVFrame *f, unsigned int tex)
{
    int *spr;
    size_t bytes = (size_t)f->width * f->height * 4;

    texture_image_free(v, 1);
    /* header at +4 so the pixels (at +48) are 16-byte aligned */
    if (!(v->spr_mem = av_mallocz(4 + SPRITE_HDR + bytes)))
        return AVERROR(ENOMEM);
    v->img_bgr = context_bgr(v->img_dpy);
    spr = (int *)(v->spr_mem + 4);
    spr[0] = SPRITE_HDR + (int)bytes;           /* offset to the next sprite */
    memcpy(&spr[1], "ffegl\0\0\0\0\0\0\0", 12);
    spr[4] = f->width - 1;                      /* width in words - 1 */
    spr[5] = f->height - 1;
    spr[6] = 0;                                 /* first bit used */
    spr[7] = 31;                                /* last bit used */
    spr[8] = SPRITE_HDR;                        /* image */
    spr[9] = SPRITE_HDR;                        /* mask = image: none */
    spr[10] = v->img_bgr ? (int)(intptr_t)trgb_selector : SPRITE_TBGR;
    v->img = create_image(v->img_dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)spr, NULL);
    if (v->img == EGL_NO_IMAGE_KHR) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: eglCreateImageKHR failed (0x%x), copying frames\n", eglGetError());
        av_freep(&v->spr_mem);
        return -1;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    target_texture(GL_TEXTURE_2D, v->img);
    if (glGetError() != GL_NO_ERROR) {
        av_log(NULL, AV_LOG_WARNING, "ffegl: glEGLImageTargetTexture2DOES failed, copying frames\n");
        texture_image_free(v, 0);
        return -1;
    }
    set_texture_params();
    v->tex = tex;
    v->tex_w = f->width;
    v->tex_h = f->height;
    return 0;
}

int ffegl_texture(FFEGLVideo *v, unsigned int tex, unsigned int *tex_out)
{
    AVFrame *f = v->cur;
    int ret, fresh = 0;

    if (!f)
        return AVERROR(EAGAIN);
    if (!tex) {
        glGenTextures(1, &tex);
        if (!tex)
            return AVERROR_EXTERNAL;
        fresh = 1;
    }
    if (tex_out)
        *tex_out = tex;

    /* EGLImage: swscale writes straight into the texture's pixels */
    if (image_usable(v)) {
        if (!v->spr_mem || fresh || tex != v->tex || v->tex_w != f->width || v->tex_h != f->height) {
            glGetError();                       /* clear any old error */
            if (texture_image_bind(v, f, tex) < 0)
                v->img_state = -1;              /* copy from now on */
        }
        if (v->spr_mem)
            return convert(v, v->spr_mem + 4 + SPRITE_HDR, f->width * 4, f->width, f->height,
                           v->img_bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA);
        fresh = 1;                              /* the texture needs specifying again */
    }

    /* otherwise: convert into a buffer and copy it into the texture */
    if (!v->rgba || v->tex_w != f->width || v->tex_h != f->height) {
        av_freep(&v->rgba);
        v->rgba = av_malloc((size_t)f->width * f->height * 4);
        if (!v->rgba)
            return AVERROR(ENOMEM);
        v->tex_w = f->width;
        v->tex_h = f->height;
        fresh = 1;
    }
    if ((ret = convert(v, v->rgba, f->width * 4, f->width, f->height, AV_PIX_FMT_RGBA)) < 0)
        return ret;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fresh || tex != v->tex) {
        set_texture_params();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f->width, f->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, v->rgba);
        v->tex = tex;
    } else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height,
                        GL_RGBA, GL_UNSIGNED_BYTE, v->rgba);
    return 0;
}
