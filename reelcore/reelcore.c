/*
 * reelcore.c - the player core of riscos-ffmpeg: plays a video file with its
 * sound and gives each picture at its time, with FFmpeg. No EGL and no
 * threads. See reelcore.h. Part of riscos-ffmpeg. LGPL 2.1 or later.
 */
#include <SDL.h>
#include <stdarg.h>
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
#include "reelcore.h"

/* On RISC OS the sound goes straight to SharedSoundBuffer/StreamManager
   from this (the caller's) thread: StreamManager plays it from interrupts,
   so nothing depends on a thread being scheduled. SDL's audio needs its own
   thread to take the queued sound, and in a Wimp task that only runs while
   the task is paged in, which on the Pi wasn't enough (Reel 0.1.4: the queue
   never drained). REELCORE_AUDIO=sdl uses SDL anyway. REELCORE_SSB builds the
   same code for the host test, with fake SWIs. */
#if defined(__riscos__) || defined(REELCORE_SSB)
#define USE_SSB 1
#include <kernel.h>
#endif

#define QMAX          8      /* decoded frames kept ahead of the clock */
#define AUDIO_AHEAD   0.25   /* seconds of sound kept queued (SDL) */
#define SSB_AHEAD     0.5    /* and with SharedSoundBuffer: rides out a busy desktop */
#define SSB_BLOCK     2048   /* sample frames per StreamManager block */
#define READ_BUDGET   64     /* packets read per reelcore_update at most */
#define DECODE_BUDGET 8      /* video packets decoded per reelcore_update at most */
#define VPK_MAX_BYTES (48 << 20)  /* video packets read ahead (for the sound) at most */
#define LATE_SKIP     0.3    /* this far behind: skip decoding non-reference frames, */
#define LATE_KEYS     1.5    /* this far: decode only keyframes, */
#define LATE_OK       0.05   /* until this close again */

struct ReelCore {
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
    /* video packets read but not decoded yet: the file is read as far as
       the sound needs, however slowly the pictures decode (Reel 0.1.6 on the
       Pi: 7 pictures waiting blocked reading, the sound ran dry at 0.5 s
       and its clock stopped with it) */
    AVPacket **vpk;
    int vpk_head, vpk_n, vpk_cap;
    size_t vpk_bytes;
    int vflushed;                      /* the video decoder has been sent the end */
    int skipping;                      /* behind: non-reference frames aren't decoded */
    unsigned skip_spells;

    /* for reelcore_stats */
    unsigned n_decoded, n_shown;
    int64_t t_decode, t_audio, t_convert;   /* microseconds */
    int conv_w, conv_h;
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
    int stalled;                       /* the sound device stopped playing: no sound now */
    char audio_note[96];               /* why there's no sound, for reelcore_info */
    double stall_clock;                /* the sound clock when it last moved ... */
    int64_t stall_since;               /* ... and when that was */
    double latency;                    /* the device's own buffer, seconds */
    uint8_t *abuf, *mixbuf;
    int abuf_size;
    int volume;                        /* 0..SDL_MIX_MAXVOLUME */
    double ahead;                      /* seconds of sound to keep queued */
#ifdef USE_SSB
    int ssb;                           /* 1: SharedSoundBuffer, not SDL (dev is then 1) */
    int ssb_handle, sm_stream;
    int ssb_started, ssb_user_paused;
    uint8_t *pend;                     /* sound not yet given to StreamManager */
    unsigned pend_len, pend_size;
    unsigned added_bytes, added_blocks;
    unsigned ssb_refused, ssb_stat_added, ssb_stat_played;   /* for reelcore_debug */
#endif

    /* timer clock (no sound, or after the sound ended) */
    int64_t t0;                        /* av_gettime_relative() at pts 0 */
    int paused;
    double pause_pos;
    unsigned dropped;                  /* late frames skipped */

    /* conversion */
    struct SwsContext *sws;
    int cs_key[8];

    /* a layer's own state (reelcore's textures), released on close */
    void *attach;
    void (*attach_release)(void *);
};

static char last_error[256];

static void set_error(const char *fmt, const char *arg)
{
    snprintf(last_error, sizeof(last_error), fmt, arg);
    av_log(NULL, AV_LOG_ERROR, "reelcore: %s\n", last_error);
}

const char *reelcore_last_error(void) { return last_error; }


/* ---------------------------------------------------------------- sound out */

#ifdef USE_SSB
#define OS_SWINumberFromString              0x39
#define SharedSoundBuffer_OpenStream        0x55FC0
#define SharedSoundBuffer_CloseStream       0x55FC1
#define SharedSoundBuffer_Volume            0x55FC4
#define SharedSoundBuffer_SampleRate        0x55FC5
#define SharedSoundBuffer_Pause             0x55FC9
#define SharedSoundBuffer_ReturnStreamHandle 0x55FCE
#define StreamManager_AddBlock              0x57282
#define StreamManager_SetBuffer             0x57287
#define StreamManager_BufferStats           0x57288
#define SSB_BLOCK_BYTES (SSB_BLOCK * 4)

static _kernel_oserror *ssb_swi(int n, int r0, int r1, int r2, _kernel_swi_regs *out)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = r0; r.r[1] = r1; r.r[2] = r2;
    return _kernel_swi(n, &r, out ? out : &r);
}

static int ssb_have(const char *swi)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[1] = (int)(intptr_t)swi;
    return _kernel_swi(OS_SWINumberFromString, &r, &r) == NULL;
}

/* Opens the stream, paused; 0 or an error message. */
static const char *ssb_start(ReelCore *v)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    static char msg[96];
    if ((e = ssb_swi(SharedSoundBuffer_OpenStream, 2, (int)(intptr_t)"reelcore", SSB_BLOCK_BYTES, &r))) {
        snprintf(msg, sizeof(msg), "SharedSoundBuffer_OpenStream: %.60s", e->errmess);
        return msg;
    }
    v->ssb_handle = r.r[0];
    if ((e = ssb_swi(SharedSoundBuffer_ReturnStreamHandle, v->ssb_handle, 0, 0, &r))) {
        ssb_swi(SharedSoundBuffer_CloseStream, v->ssb_handle, 0, 0, NULL);
        v->ssb_handle = 0;
        snprintf(msg, sizeof(msg), "SharedSoundBuffer_ReturnStreamHandle: %.50s", e->errmess);
        return msg;
    }
    v->sm_stream = r.r[0];
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: SharedSoundBuffer stream %#x (StreamManager %#x), %d Hz, blocks of %d bytes\n",
           v->ssb_handle, v->sm_stream, v->rate, SSB_BLOCK_BYTES);
    /* room for far more than we keep queued, so AddBlock doesn't refuse */
    ssb_swi(StreamManager_SetBuffer, v->sm_stream, v->rate * 4 * 2 + SSB_BLOCK_BYTES * 4, 0, NULL);
    ssb_swi(SharedSoundBuffer_SampleRate, v->ssb_handle, v->rate * 1024, 0, NULL);
    ssb_swi(SharedSoundBuffer_Volume, v->ssb_handle, (int)0xFFFFFFFF, 0, NULL);
    ssb_swi(SharedSoundBuffer_Pause, v->ssb_handle, 0, 0, NULL);     /* R1 bit 0 clear: paused */
    v->ssb_started = 0;
    v->pend_len = 0;
    v->added_bytes = v->added_blocks = 0;
    return NULL;
}

static void ssb_stop(ReelCore *v)
{
    if (v->ssb_handle)
        ssb_swi(SharedSoundBuffer_CloseStream, v->ssb_handle, 0, 0, NULL);
    v->ssb_handle = v->sm_stream = 0;
}

/* Bytes StreamManager holds that haven't been played. BufferStats gives
   added and played; if they turn out to count blocks, convert. */
static unsigned ssb_unplayed(ReelCore *v)
{
    _kernel_swi_regs r;
    unsigned added, played;
    if (!v->sm_stream || ssb_swi(StreamManager_BufferStats, v->sm_stream, 0, 0, &r))
        return 0;
    added = (unsigned)r.r[0];
    played = (unsigned)r.r[1];
    v->ssb_stat_added = added;
    v->ssb_stat_played = played;
    if (added == v->added_blocks && added != v->added_bytes) {
        added *= SSB_BLOCK_BYTES;
        played *= SSB_BLOCK_BYTES;
    }
    return added > played ? added - played : 0;
}

/* Gives StreamManager whole blocks (all of it, padded, when FINAL), and
   starts playing once two blocks are there. */
static void ssb_push(ReelCore *v, int final)
{
    unsigned off = 0;
    if (!v->ssb_handle)
        return;
    while (v->pend_len - off >= SSB_BLOCK_BYTES || (final && v->pend_len > off)) {
        unsigned n = v->pend_len - off;
        if (n < SSB_BLOCK_BYTES) {                 /* the last bit: pad with silence */
            memset(v->pend + off + n, 0, SSB_BLOCK_BYTES - n);
            v->pend_len = off + SSB_BLOCK_BYTES;
        }
        {
            _kernel_oserror *e = ssb_swi(StreamManager_AddBlock, v->sm_stream,
                                         (int)(intptr_t)(v->pend + off), SSB_BLOCK_BYTES, NULL);
            if (e) {                               /* full: try again next time */
                if (!v->ssb_refused++)
                    av_log(NULL, AV_LOG_VERBOSE, "reelcore: StreamManager_AddBlock refused a block: %s\n", e->errmess);
                break;
            }
        }
        off += SSB_BLOCK_BYTES;
        v->added_bytes += SSB_BLOCK_BYTES;
        v->added_blocks++;
    }
    if (off) {
        memmove(v->pend, v->pend + off, v->pend_len - off);
        v->pend_len -= off;
    }
    if (!v->ssb_started && !v->ssb_user_paused &&
        (v->added_blocks >= 2 || (final && v->added_blocks > 0))) {
        _kernel_oserror *e = ssb_swi(SharedSoundBuffer_Pause, v->ssb_handle, 1, 0, NULL);   /* play */
        v->ssb_started = 1;
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: sound starts (%u blocks given)%s%s\n", v->added_blocks,
               e ? "; SharedSoundBuffer_Pause: " : "", e ? e->errmess : "");
    }
}
#endif

/* Opens the sound output at about FREQ; 0, or -1 with audio_note set. */
static int aud_open(ReelCore *v, int freq)
{
    SDL_AudioSpec want, have;
#ifdef USE_SSB
    const char *which = getenv("REELCORE_AUDIO");
    if (!which || strcmp(which, "sdl")) {
        const char *err;
        if (!ssb_have("SharedSoundBuffer_OpenStream") || !ssb_have("StreamManager_AddBlock")) {
            snprintf(v->audio_note, sizeof(v->audio_note), "SharedSoundBuffer/StreamManager not loaded");
            return -1;
        }
        v->rate = freq;
        if ((err = ssb_start(v)) != NULL) {
            snprintf(v->audio_note, sizeof(v->audio_note), "%s", err);
            return -1;
        }
        v->ssb = 1;
        v->dev = 1;
        v->bytes_per_sec = freq * 4;
        v->latency = 0.02;                         /* SharedSound's own buffer, about */
        v->ahead = SSB_AHEAD;
        v->ssb_user_paused = 1;                    /* until reelcore_open unpauses */
        return 0;
    }
#endif
#ifdef __riscos__
    /* Only SDL's RISC OS driver (SharedSoundBuffer + StreamManager) plays
       reliably. Without it SDL would fall back to UnixLib's /dev/dsp, which
       takes the sound and may never play it; better to say what's missing.
       SDL_AUDIODRIVER set by the user still wins. */
    if (!SDL_getenv("SDL_AUDIODRIVER"))
        SDL_setenv("SDL_AUDIODRIVER", "riscos", 1);
#endif
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        snprintf(v->audio_note, sizeof(v->audio_note), "%s", SDL_GetError());
        return -1;
    }
    memset(&want, 0, sizeof(want));
    want.freq = freq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    v->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!v->dev) {
        snprintf(v->audio_note, sizeof(v->audio_note), "%s", SDL_GetError());
        return -1;
    }
    v->rate = have.freq;
    v->bytes_per_sec = have.freq * 4;
    v->latency = have.samples / (double)have.freq;
    v->ahead = AUDIO_AHEAD;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: SDL audio driver %s, %d Hz, %d samples a buffer\n",
           SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq, have.samples);
    return 0;
}

static unsigned aud_queued(ReelCore *v)
{
#ifdef USE_SSB
    if (v->ssb)
        return ssb_unplayed(v) + v->pend_len;
#endif
    return SDL_GetQueuedAudioSize(v->dev);
}

static void aud_queue(ReelCore *v, const void *data, unsigned bytes)
{
#ifdef USE_SSB
    if (v->ssb) {
        /* room for the new sound plus padding to a whole block */
        if (v->pend_len + bytes + SSB_BLOCK_BYTES > v->pend_size) {
            unsigned size = v->pend_len + bytes + SSB_BLOCK_BYTES * 2;
            uint8_t *p = av_realloc(v->pend, size);
            if (!p)
                return;
            v->pend = p;
            v->pend_size = size;
        }
        memcpy(v->pend + v->pend_len, data, bytes);
        v->pend_len += bytes;
        ssb_push(v, 0);
        return;
    }
#endif
    SDL_QueueAudio(v->dev, data, bytes);
}

/* The sound has all been decoded: play what's left. */
static void aud_flush(ReelCore *v)
{
#ifdef USE_SSB
    if (v->ssb)
        ssb_push(v, 1);
#else
    (void)v;
#endif
}

static void aud_pause(ReelCore *v, int paused)
{
#ifdef USE_SSB
    if (v->ssb) {
        v->ssb_user_paused = paused;
        if (v->ssb_started)
            ssb_swi(SharedSoundBuffer_Pause, v->ssb_handle, paused ? 0 : 1, 0, NULL);
        else if (!paused)
            ssb_push(v, 0);
        return;
    }
#endif
    SDL_PauseAudioDevice(v->dev, paused);
}

/* Throws away everything queued (seek). */
static void aud_clear(ReelCore *v)
{
#ifdef USE_SSB
    if (v->ssb) {
        /* StreamManager has no "empty the buffer": start a new stream */
        const char *err;
        ssb_stop(v);
        if ((err = ssb_start(v)) != NULL) {
            av_log(NULL, AV_LOG_WARNING, "reelcore: can't reopen the sound after a seek: %s\n", err);
            v->stalled = 1;                        /* carry on without sound */
        }
        return;
    }
#endif
    SDL_ClearQueuedAudio(v->dev);
}

static void aud_close(ReelCore *v)
{
#ifdef USE_SSB
    if (v->ssb) {
        ssb_stop(v);
        av_freep(&v->pend);
        v->dev = 0;
        return;
    }
#endif
    SDL_CloseAudioDevice(v->dev);
}

/* ---------------------------------------------------------------- clock */

static double queued_audio(const ReelCore *v)
{
    return v->dev && !v->stalled ? aud_queued((ReelCore *)v) / (double)v->bytes_per_sec : 0;
}

/* If the sound device takes none of the queued sound for this long while
   playing, it isn't playing (e.g. no SharedSoundBuffer on RISC OS): stop
   sending it sound and let the timer drive the pictures, which would
   otherwise stay on the first frame. */
#define STALL_SECONDS 1.0

static void audio_stalled(ReelCore *v, double c)
{
    {
        char d[200];
        reelcore_debug(v, d, sizeof(d));
        av_log(NULL, AV_LOG_WARNING, "reelcore: the sound device isn't playing; carrying on without sound (%s)\n", d);
    }
    v->stalled = 1;
    aud_clear(v);
    aud_pause(v, 1);
    v->audio_clock = 0;
    v->t0 = av_gettime_relative() - (int64_t)(c * 1e6);
}

static double clock_now(ReelCore *v)
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
        {
            double c = v->audio_end - q - v->latency;
            int64_t now = av_gettime_relative();
            if (c > v->stall_clock + 0.001 || q <= 0 || !v->stall_since) {
                v->stall_clock = c;
                v->stall_since = now;
            } else if (now - v->stall_since > (int64_t)(STALL_SECONDS * 1e6)) {
                audio_stalled(v, c);
                return c;
            }
            return c;
        }
    }
    return (av_gettime_relative() - v->t0) / 1e6;
}

/* Make the timer read pts now. */
static void timer_set(ReelCore *v, double pts)
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

static int open_audio(ReelCore *v)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int freq = v->adec->sample_rate > 0 ? v->adec->sample_rate : 44100;

    if (freq != 44100 && freq != 48000 && freq != 22050)
        freq = 48000;
    if (aud_open(v, freq) < 0) {
        av_log(NULL, AV_LOG_WARNING, "reelcore: no sound (%s)\n", v->audio_note);
        return -1;
    }
    if (swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_S16, v->rate,
                            &v->adec->ch_layout, v->adec->sample_fmt, v->adec->sample_rate,
                            0, NULL) < 0 || swr_init(v->swr) < 0) {
        aud_close(v);
        v->dev = 0;
        return -1;
    }
    return 0;
}

ReelCore *reelcore_open(const char *url, int flags)
{
    ReelCore *v = av_mallocz(sizeof(*v));
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

    if (!(flags & REELCORE_NO_AUDIO)) {
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
    if (flags & REELCORE_PAUSED) {
        v->paused = 1;
        v->pause_pos = 0;
    } else if (v->dev)
        aud_pause(v, 0);
    timer_set(v, 0);
    return v;

fail:
    reelcore_close(v);
    return NULL;
}

static void clear_queue(ReelCore *v)
{
    for (int i = 0; i < v->qn; i++)
        av_frame_free(&v->q[i]);
    v->qn = 0;
}

static void vpk_clear(ReelCore *v);

void reelcore_close(ReelCore *v)
{
    if (!v)
        return;
    if (v->dev)
        aud_close(v);
    clear_queue(v);
    vpk_clear(v);
    av_freep(&v->vpk);
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
    if (v->attach_release)
        v->attach_release(v->attach);
    av_free(v);
}

int reelcore_width(const ReelCore *v)        { return v->w; }
int reelcore_height(const ReelCore *v)       { return v->h; }
double reelcore_frame_rate(const ReelCore *v) { return v->fps; }
double reelcore_duration(const ReelCore *v)  { return v->duration; }
int reelcore_has_audio(const ReelCore *v)    { return v->dev != 0 && !v->stalled; }
unsigned reelcore_dropped_frames(const ReelCore *v) { return v->dropped; }

int reelcore_debug(const ReelCore *v, char *buf, int size)
{
    ReelCore *w = (ReelCore *)v;
    double q = v->dev ? aud_queued(w) / (double)(v->bytes_per_sec ? v->bytes_per_sec : 1) : 0;
    /* the clock as clock_now() would give it, without its stall check */
    double c = v->paused ? v->pause_pos
             : v->audio_clock && v->audio_end >= 0 ? v->audio_end - q - v->latency
             : (av_gettime_relative() - v->t0) / 1e6;
    int n = snprintf(buf, size, "pos %.2f clock %.2f%s, %d pictures and %d packets (%u KB) waiting, "
                     "%u late%s, %u skip spells",
                     reelcore_position(v), c,
                     v->paused ? " (paused)" : v->audio_clock ? " (sound)" : " (timer)", v->qn,
                     v->vpk_n, (unsigned)(v->vpk_bytes >> 10), v->dropped,
                     v->skipping == 2 ? ", keyframes only" : v->skipping ? ", skipping non-reference frames" : "",
                     v->skip_spells);
    if (n >= size)
        return n;
    if (!v->dev)
        return n + snprintf(buf + n, size - n, "; no sound%s%s", v->audio_note[0] ? ": " : "", v->audio_note);
#ifdef USE_SSB
    if (v->ssb)
        return n + snprintf(buf + n, size - n,
                            "; SSB %s%s, queued %.2f s, added %u played %u (StreamManager), waiting %u bytes, %u refused",
                            v->stalled ? "stalled" : v->ssb_started ? "playing" : "not started",
                            v->ssb_user_paused ? ", paused" : "", q,
                            v->ssb_stat_added, v->ssb_stat_played, v->pend_len, v->ssb_refused);
#endif
    return n + snprintf(buf + n, size - n, "; SDL %s, queued %.2f s", v->stalled ? "stalled" : "playing", q);
}

void reelcore_stats(const ReelCore *v, ReelCoreStats *st)
{
    ReelCore *w = (ReelCore *)v;
    memset(st, 0, sizeof(*st));
    st->position = reelcore_position(v);
    st->sound_queued = v->dev ? aud_queued(w) / (double)(v->bytes_per_sec ? v->bytes_per_sec : 1) : 0;
    st->clock_source = v->paused ? 2 : v->audio_clock ? 1 : 0;
    st->clock = v->paused ? v->pause_pos
              : v->audio_clock && v->audio_end >= 0 ? v->audio_end - st->sound_queued - v->latency
              : (av_gettime_relative() - v->t0) / 1e6;
    st->fps = v->fps;
    st->decoded = v->n_decoded;
    st->shown = v->n_shown;
    st->late = v->dropped;
    st->decode_time = v->t_decode / 1e6;
    st->audio_time = v->t_audio / 1e6;
    st->convert_time = v->t_convert / 1e6;
    st->convert_w = v->conv_w;
    st->convert_h = v->conv_h;
    st->pictures_waiting = v->qn;
    st->packets_waiting = v->vpk_n;
    st->packet_bytes = (unsigned)v->vpk_bytes;
    st->skip_level = v->skipping;
    st->skip_spells = v->skip_spells;
    st->sound = !v->dev ? 0 : 2;
    st->sound_stalled = v->stalled;
#ifdef USE_SSB
    if (v->ssb) {
        st->sound = 1;
        st->sound_added = v->ssb_stat_added;
        st->sound_played = v->ssb_stat_played;
    }
#endif
    st->bytes_read = v->fmt && v->fmt->pb ? v->fmt->pb->bytes_read : 0;
}

#define ADD(...) do { if (n < size) n += snprintf(buf + n, size - n, __VA_ARGS__); } while (0)

static void info_bitrate(char *b, size_t size, int64_t bps)
{
    if (bps <= 0)
        snprintf(b, size, "?");
    else if (bps >= 1000000)
        snprintf(b, size, "%.2f Mbit/s", bps / 1e6);
    else
        snprintf(b, size, "%lld kbit/s", (long long)(bps / 1000));
}

static const char *or_q(const char *s) { return s ? s : "?"; }

int reelcore_media_info(const ReelCore *v, char *buf, int size)
{
    const AVFormatContext *fc = v->fmt;
    const AVDictionaryEntry *t;
    char b[96];
    int n = 0;

    if (size > 0)
        buf[0] = 0;
    ADD("#File\n");
    if (fc->url) {
        const char *leaf = strrchr(fc->url, fc->url[0] == '/' ? '/' : '.');
        ADD("Name\t%s\n", leaf ? leaf + 1 : fc->url);
    }
    if ((t = av_dict_get(fc->metadata, "title", NULL, 0)))
        ADD("Title\t%s\n", t->value);
    ADD("Container\t%s (%s)\n", or_q(fc->iformat->long_name), fc->iformat->name);
    if (fc->duration > 0) {
        int64_t d = fc->duration / AV_TIME_BASE;
        ADD("Length\t%d:%02d:%02d (%.2f s)\n", (int)(d / 3600), (int)(d / 60 % 60), (int)(d % 60),
            fc->duration / (double)AV_TIME_BASE);
    }
    if (fc->pb && avio_size(fc->pb) > 0)
        ADD("Size\t%.1f MB (%lld bytes)\n", avio_size(fc->pb) / 1048576.0, (long long)avio_size(fc->pb));
    info_bitrate(b, sizeof(b), fc->bit_rate);
    ADD("Bit rate\t%s\n", b);
    ADD("Streams\t%u\n", fc->nb_streams);

    if (v->vs >= 0) {
        const AVStream *st = fc->streams[v->vs];
        const AVCodecParameters *p = st->codecpar;
        const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
        const char *prof = avcodec_profile_name(p->codec_id, p->profile);
        AVRational dar;
        ADD("#Video\n");
        ADD("Codec\t%s (%s)\n", d ? d->long_name : "?", avcodec_get_name(p->codec_id));
        if (prof)
            ADD("Profile\t%s%s\n", prof, "");
        if (p->level > 0)
            ADD("Level\t%d.%d\n", p->level / 10, p->level % 10);
        ADD("Size\t%dx%d pixels\n", p->width, p->height);
        av_reduce(&dar.num, &dar.den, (int64_t)p->width * (p->sample_aspect_ratio.num ? p->sample_aspect_ratio.num : 1),
                  (int64_t)p->height * (p->sample_aspect_ratio.den ? p->sample_aspect_ratio.den : 1), 1 << 20);
        ADD("Aspect\t%d:%d (shown %dx%d)\n", dar.num, dar.den, v->w, v->h);
        if (v->fps > 0)
            ADD("Frame rate\t%.3f fps\n", v->fps);
        ADD("Pixels\t%s\n", or_q(av_get_pix_fmt_name(p->format)));
        if (p->color_space == AVCOL_SPC_UNSPECIFIED && p->color_range == AVCOL_RANGE_UNSPECIFIED)
            ADD("Colours\tnot given (shown as BT.601, limited range)\n");
        else
            ADD("Colours\t%s, %s range, %s primaries, %s transfer\n",
                or_q(av_color_space_name(p->color_space)), or_q(av_color_range_name(p->color_range)),
                or_q(av_color_primaries_name(p->color_primaries)), or_q(av_color_transfer_name(p->color_trc)));
        info_bitrate(b, sizeof(b), p->bit_rate);
        ADD("Bit rate\t%s\n", b);
        if (st->nb_frames > 0)
            ADD("Frames\t%lld\n", (long long)st->nb_frames);
        ADD("Decoder\t%s, 1 thread\n", v->vdec && v->vdec->codec ? v->vdec->codec->name : "?");
        if (v->vdec && v->vdec->has_b_frames)
            ADD("Reordering\t%d frame%s (B-frames)\n", v->vdec->has_b_frames, v->vdec->has_b_frames == 1 ? "" : "s");
    }
    if (v->as >= 0) {
        const AVStream *st = fc->streams[v->as];
        const AVCodecParameters *p = st->codecpar;
        const AVCodecDescriptor *d = avcodec_descriptor_get(p->codec_id);
        const char *prof = avcodec_profile_name(p->codec_id, p->profile);
        char layout[64];
        ADD("#Audio\n");
        ADD("Codec\t%s (%s)\n", d ? d->long_name : "?", avcodec_get_name(p->codec_id));
        if (v->adec && v->adec->profile != FF_PROFILE_UNKNOWN &&
            (prof = avcodec_profile_name(p->codec_id, v->adec->profile)) != NULL)
            ADD("Profile\t%s\n", prof);
        else if (prof)
            ADD("Profile\t%s\n", prof);
        if (av_channel_layout_describe(&p->ch_layout, layout, sizeof(layout)) < 0)
            snprintf(layout, sizeof(layout), "%d channels", p->ch_layout.nb_channels);
        ADD("Channels\t%d (%s)\n", p->ch_layout.nb_channels, layout);
        ADD("Sample rate\t%d Hz\n", p->sample_rate);
        if (v->adec)
            ADD("Samples\t%s\n", or_q(av_get_sample_fmt_name(v->adec->sample_fmt)));
        info_bitrate(b, sizeof(b), p->bit_rate);
        ADD("Bit rate\t%s\n", b);
        if ((t = av_dict_get(st->metadata, "language", NULL, 0)))
            ADD("Language\t%s\n", t->value);
        ADD("Decoder\t%s\n", v->adec && v->adec->codec ? v->adec->codec->name : "?");
    }
    ADD("#Sound output\n");
    if (!v->dev)
        ADD("Output\tnone%s%s\n", v->audio_note[0] ? ": " : "", v->audio_note);
#ifdef USE_SSB
    else if (v->ssb)
        ADD("Output\tSharedSoundBuffer (StreamManager), %d Hz 16-bit stereo\n", v->rate);
#endif
    else
        ADD("Output\tSDL (%s), %d Hz 16-bit stereo\n",
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", v->rate);
    if (v->dev) {
        ADD("Kept queued\t%.2f s\n", v->ahead);
        if (v->stalled)
            ADD("State\tthe device isn't playing: no sound\n");
    }
    return n;
}
#undef ADD

static void (*log_fn)(int, const char *);

static void log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    static char line[1024];
    static int len;
    int n;
    if (level > av_log_get_level())
        return;
    n = vsnprintf(line + len, sizeof(line) - len, fmt, vl);
    if (n < 0)
        return;
    len += n;
    if (len >= (int)sizeof(line) - 1)
        len = sizeof(line) - 1;
    if (len && line[len - 1] == '\n') {
        line[len - 1] = 0;
        if (log_fn)
            log_fn(level, line);
        len = 0;
    }
    (void)avcl;
}

void reelcore_set_log(void (*fn)(int level, const char *line), int verbose)
{
    log_fn = fn;
    if (fn) {
        av_log_set_level(verbose ? AV_LOG_VERBOSE : AV_LOG_INFO);
        av_log_set_callback(log_callback);
    } else {
        av_log_set_level(AV_LOG_INFO);
        av_log_set_callback(av_log_default_callback);
    }
}

int reelcore_info(const ReelCore *v, char *buf, int size)
{
    const AVCodecParameters *vp = v->fmt->streams[v->vs]->codecpar;
    int n = snprintf(buf, size, "%s %dx%d", avcodec_get_name(vp->codec_id), vp->width, vp->height);
    if (v->fps > 0 && n < size)
        n += snprintf(buf + n, size - n, ", %.3g fps", v->fps);
    if (v->as >= 0 && n < size) {
        const AVCodecParameters *ap = v->fmt->streams[v->as]->codecpar;
        n += snprintf(buf + n, size - n, "; %s %d Hz, %d channel%s%s", avcodec_get_name(ap->codec_id),
                      ap->sample_rate, ap->ch_layout.nb_channels, ap->ch_layout.nb_channels == 1 ? "" : "s",
                      !v->dev ? " (no sound device" : v->stalled ? " (the sound device isn't playing: no sound)" : "");
        if (!v->dev && n < size)
            n += snprintf(buf + n, size - n, "%s%s)", v->audio_note[0] ? ": " : "", v->audio_note);
    } else if (n < size && v->audio_note[0])      /* sound in the file, but no device */
        n += snprintf(buf + n, size - n, "; no sound device: %s", v->audio_note);
    else if (n < size)
        n += snprintf(buf + n, size - n, "; no sound");
    if (v->fmt->iformat && n < size)
        n += snprintf(buf + n, size - n, "; %s", v->fmt->iformat->name);
    return n;
}

double reelcore_position(const ReelCore *v)
{
    double start = v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time / (double)AV_TIME_BASE : 0;
    return v->cur ? v->cur_pts - start : 0;
}
int reelcore_paused(const ReelCore *v)       { return v->paused; }

void reelcore_set_volume(ReelCore *v, double volume)
{
    v->volume = (int)(av_clipd(volume, 0, 1) * SDL_MIX_MAXVOLUME + 0.5);
}

/* ---------------------------------------------------------------- decode */

static double frame_pts(AVFrame *f, AVStream *st, double fallback)
{
    int64_t t = f->best_effort_timestamp;
    return t == AV_NOPTS_VALUE ? fallback : t * av_q2d(st->time_base);
}

static void got_video(ReelCore *v, AVFrame *f)
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
    v->n_decoded++;
    if (v->q[v->qn])
        v->qn++;
}

static void got_audio(ReelCore *v, AVFrame *f)
{
    int out_max = swr_get_out_samples(v->swr, f->nb_samples);
    int n, bytes;
    double pts = frame_pts(f, v->fmt->streams[v->as], v->audio_end >= 0 ? v->audio_end : 0);

    if (v->aseek_target >= 0) {
        if (pts + f->nb_samples / (double)f->sample_rate < v->aseek_target)
            return;                       /* before the seek point */
        v->aseek_target = -1;
    }
    if (out_max <= 0 || v->stalled)
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
            aud_queue(v, m, bytes);
            av_free(m);
        }
    } else
        aud_queue(v, v->abuf, bytes);
    v->audio_end += n / (double)v->rate;
}

/* ---- video packets waiting to be decoded ---- */

static int vpk_push(ReelCore *v, AVPacket *pkt)
{
    AVPacket *p;
    if (v->vpk_n == v->vpk_cap) {
        int cap = v->vpk_cap ? v->vpk_cap * 2 : 256;
        AVPacket **q = av_malloc_array(cap, sizeof(*q));
        if (!q)
            return -1;
        for (int i = 0; i < v->vpk_n; i++)
            q[i] = v->vpk[(v->vpk_head + i) % v->vpk_cap];
        av_free(v->vpk);
        v->vpk = q;
        v->vpk_cap = cap;
        v->vpk_head = 0;
    }
    if (!(p = av_packet_alloc()))
        return -1;
    av_packet_move_ref(p, pkt);
    v->vpk[(v->vpk_head + v->vpk_n) % v->vpk_cap] = p;
    v->vpk_n++;
    v->vpk_bytes += p->size;
    return 0;
}

static AVPacket *vpk_pop(ReelCore *v)
{
    AVPacket *p;
    if (!v->vpk_n)
        return NULL;
    p = v->vpk[v->vpk_head];
    v->vpk_head = (v->vpk_head + 1) % v->vpk_cap;
    v->vpk_n--;
    v->vpk_bytes -= p->size;
    return p;
}

static void vpk_clear(ReelCore *v)
{
    AVPacket *p;
    while ((p = vpk_pop(v)) != NULL)
        av_packet_free(&p);
    v->vpk_head = 0;
}

/* Sends pkt (NULL = flush) to a decoder and takes all it gives back. */
static void decode_frames(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video);

static void decode(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video)
{
    int64_t t0 = av_gettime_relative();
    decode_frames(v, dec, pkt, video);
    if (video)
        v->t_decode += av_gettime_relative() - t0;
    else
        v->t_audio += av_gettime_relative() - t0;
}

static void decode_frames(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video)
{
    int ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN))
        return;                            /* a damaged packet: skip it */
    for (;;) {
        ret = avcodec_receive_frame(dec, v->frame);
        if (ret == AVERROR_EOF) {
            if (video) v->eof_video = 1; else { v->eof_audio = 1; if (v->dev && !v->stalled) aud_flush(v); }
            return;
        }
        if (ret < 0)
            return;
        if (video) got_video(v, v->frame); else got_audio(v, v->frame);
        av_frame_unref(v->frame);
    }
}

/* Behind the clock: stop decoding the frames nothing else refers to (most
   B-frames) until caught up; far behind, decode only keyframes. Those
   frames would only be dropped as late anyway, and the sound (the clock)
   doesn't wait. */
static void check_late(ReelCore *v)
{
    static const char *what[3] = { "decoding every frame", "skipping non-reference frames",
                                   "decoding only keyframes" };
    double lag, last;
    int want;
    if (v->paused || !v->cur || v->need_first)
        return;
    last = v->qn ? v->qpts[v->qn - 1] : v->cur_pts;
    lag = clock_now(v) - last;
    want = lag > LATE_KEYS ? 2 : lag > LATE_SKIP ? (v->skipping > 1 ? 2 : 1) : lag < LATE_OK ? 0 : v->skipping;
    if (want != v->skipping) {
        if (want > v->skipping)
            v->skip_spells++;
        v->skipping = want;
        v->vdec->skip_frame = want == 2 ? AVDISCARD_NONKEY : want ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: %.2f s behind: %s\n", lag, what[want]);
    }
}

/* Reads until there's a little sound queued and a video packet to decode,
   then decodes pictures until a few are ready. */
static void fill(ReelCore *v)
{
    for (int budget = READ_BUDGET; budget > 0 && !v->eof_demux; budget--) {
        int need_a = v->dev && !v->stalled && queued_audio(v) < v->ahead;
        int need_v = v->qn < 3 && v->vpk_n == 0;
        int ret;
        if (!need_a && !need_v)
            break;
        if (!need_v && v->vpk_bytes > VPK_MAX_BYTES)
            break;                         /* a strange file: don't eat all the memory */
        ret = av_read_frame(v->fmt, v->pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0) {                     /* the end: flush the sound decoder now, */
            v->eof_demux = 1;              /* the video's once its packets are done */
            if (v->adec) decode(v, v->adec, NULL, 0); else v->eof_audio = 1;
            break;
        }
        if (v->pkt->stream_index == v->vs)
            vpk_push(v, v->pkt);
        else if (v->pkt->stream_index == v->as)
            decode(v, v->adec, v->pkt, 0);
        av_packet_unref(v->pkt);
    }
    /* after a seek, decode on to the seek point in one go (as before) */
    int budget = v->need_first ? READ_BUDGET : DECODE_BUDGET;
    for (int n = 0; n < budget && v->qn < 3; n++) {
        if (!v->vpk_n && !v->eof_demux && v->need_first) {
            /* more packets on the way to the seek point */
            int ret = av_read_frame(v->fmt, v->pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                v->eof_demux = 1;
                if (v->adec) decode(v, v->adec, NULL, 0); else v->eof_audio = 1;
            } else if (ret >= 0) {
                if (v->pkt->stream_index == v->vs)
                    vpk_push(v, v->pkt);
                else if (v->pkt->stream_index == v->as)
                    decode(v, v->adec, v->pkt, 0);
                av_packet_unref(v->pkt);
                continue;
            }
        }
        AVPacket *p = vpk_pop(v);
        if (p) {
            check_late(v);
            decode(v, v->vdec, p, 1);
            av_packet_free(&p);
        } else if (v->eof_demux && !v->vflushed) {
            v->vflushed = 1;
            decode(v, v->vdec, NULL, 1);
        } else
            break;
    }
    if (!v->adec)
        v->eof_audio = v->eof_demux;
}

static void take_frame(ReelCore *v)
{
    av_frame_free(&v->cur);
    v->cur = v->q[0];
    v->cur_pts = v->qpts[0];
    memmove(v->q, v->q + 1, (v->qn - 1) * sizeof(v->q[0]));
    memmove(v->qpts, v->qpts + 1, (v->qn - 1) * sizeof(v->qpts[0]));
    v->qn--;
}

int reelcore_update(ReelCore *v)
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
        v->n_shown++;
        return REELCORE_NEW_FRAME;
    }
    if (v->paused)
        return REELCORE_SAME_FRAME;

    now = clock_now(v);
    if (v->qn && v->qpts[0] <= now + 0.005) {
        /* skip the frames that are already late */
        while (v->qn > 1 && v->qpts[1] <= now) {
            take_frame(v);
            v->dropped++;
        }
        take_frame(v);
        v->n_shown++;
        return REELCORE_NEW_FRAME;
    }

end_check:
    if (v->eof_demux && v->eof_video && !v->qn && queued_audio(v) <= 0) {
        if ((v->flags & REELCORE_LOOP) && v->cur) {
            if (reelcore_seek(v, 0) < 0)
                return REELCORE_END;
            return reelcore_update(v);
        }
        return REELCORE_END;
    }
    return REELCORE_SAME_FRAME;
}

#define IDLE_MAX 0.1

double reelcore_idle_time(ReelCore *v)
{
    double due;
    if (v->paused)
        return IDLE_MAX;
    if (v->need_first)
        return 0;
    /* pictures still to decode */
    if (v->qn < 3 && (v->vpk_n || !v->eof_demux || !v->vflushed))
        return 0;
    /* the sound to top up (fill() keeps it at v->ahead) */
    if (v->dev && !v->stalled && !v->eof_demux && queued_audio(v) < v->ahead - IDLE_MAX - 0.05)
        return 0;
    if (!v->qn)
        return v->eof_demux ? IDLE_MAX / 4 : 0;   /* the end: sound draining */
    due = v->qpts[0] - clock_now(v);
    if (due <= 0)
        return 0;
    return due > IDLE_MAX ? IDLE_MAX : due;
}

void reelcore_pause(ReelCore *v, int paused)
{
    paused = !!paused;
    if (paused == v->paused)
        return;
    if (paused) {
        v->pause_pos = v->cur ? clock_now(v) : 0;
        v->paused = 1;
        if (v->dev)
            aud_pause(v, 1);
    } else {
        v->paused = 0;
        v->stall_since = 0;                 /* the device needs a moment to start again */
        if (!v->audio_clock || v->audio_end < 0)
            timer_set(v, v->pause_pos);
        if (v->dev)
            aud_pause(v, 0);
    }
}

int reelcore_seek(ReelCore *v, double seconds)
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
    if (v->dev && !v->stalled)
        aud_clear(v);
    if (v->swr)
        swr_init(v->swr);                 /* drop what it buffered */
    clear_queue(v);
    vpk_clear(v);
    v->vflushed = 0;
    v->eof_demux = v->eof_video = v->eof_audio = 0;
    v->audio_end = -1;
    v->audio_clock = v->dev != 0 && !v->stalled;
    v->stall_since = 0;
    v->seek_target = v->aseek_target = seconds > 0 ? ts / (double)AV_TIME_BASE : -1;
    v->need_first = 1;
    return 0;
}

/* ---------------------------------------------------------------- draw */

/* Converts the current frame into w x h pixels at dst (fmt), with the
   frame's colour space and range. */
static int convert(ReelCore *v, uint8_t *dst, int pitch, int w, int h, enum AVPixelFormat fmt)
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
    {
        int64_t t0 = av_gettime_relative();
        int ret = sws_scale(v->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height, d, ds);
        v->t_convert += av_gettime_relative() - t0;
        v->conv_w = w;
        v->conv_h = h;
        return ret < 0 ? AVERROR_EXTERNAL : 0;
    }
}

static void fill_black(uint8_t *p, int pitch, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        memset(p + (y + j) * pitch + x * 4, 0, (size_t)w * 4);
}

int reelcore_draw_pixels(ReelCore *v, void *pixels, int pitch, int w, int h, int bgr, int flags)
{
    uint8_t *p = pixels;
    int x = 0, y = 0, rw = w, rh = h;

    if (!v->cur)
        return AVERROR(EAGAIN);
    if (w < 1 || h < 1)
        return AVERROR(EINVAL);
    if (!(flags & REELCORE_STRETCH)) {
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
        if (!(flags & REELCORE_NO_BORDERS)) {
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

int reelcore_frame_size(const ReelCore *v, int *w, int *h)
{
    if (!v->cur)
        return AVERROR(EAGAIN);
    *w = v->cur->width;
    *h = v->cur->height;
    return 0;
}

void reelcore_attach(ReelCore *v, void *data, void (*release)(void *data))
{
    v->attach = data;
    v->attach_release = release;
}

void *reelcore_attachment(const ReelCore *v) { return v->attach; }
