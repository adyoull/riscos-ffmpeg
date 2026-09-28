/*
 * reelcore.c - the player core of riscos-ffmpeg: plays a video file with its
 * sound and gives each picture at its time, with FFmpeg. No EGL. One
 * thread, except for network addresses, which are opened and read by a
 * thread of their own (see "Network sources"). See reelcore.h. Part of
 * riscos-ffmpeg. LGPL 2.1 or later.
 */
#include <SDL.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "reelcore.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>                 /* reelcore_halve: 2x2 averages, 8 at a time */
#define REELCORE_NEON 1
#endif

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
#define NET_AHEAD     10.0   /* network: seconds read ahead of the picture shown */
#define NET_MAX_BYTES (32 << 20)  /* ... and at most this much */
#define NET_LOW       3.0    /* below this, reelcore_update gives the reader time */

struct Net;

struct ReelCore {
    int flags;
    AVFormatContext *fmt;
    AVFormatContext *afmt;             /* the sound from another address (yt-dlp's
                                          bestvideo+bestaudio), or NULL: it's in fmt */
    struct Net *net;                   /* network: the reading thread, or NULL */
    char *title;                       /* ReelCoreSource.title, or NULL */
    int ready;                         /* opened and set up (async opening: not yet) */
    double lpts[2];                    /* local file + separate sound: the last packet */
    int leof[2];                       /* read from each, and which have ended */
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
    int volume;                        /* 0..SDL_MIX_MAXVOLUME (mixed in: SDL) */
    double vol;                        /* 0..1, as set */
    double speed;                      /* playback speed, 0.5..2 (1 = normal) */
    AVFilterGraph *tempo;              /* atempo: the sound at speed, same pitch */
    AVFilterContext *tempo_in, *tempo_out;
    AVFrame *tempo_frame;
    int fast;                          /* fast decoding: no deblocking filter */
    int deint;                         /* REELCORE_DEINT_* */
    AVFilterGraph *dgraph;             /* buffer -> yadif -> buffersink, made when needed */
    AVFilterContext *din, *dout;
    AVFrame *dframe;
    int dg_w, dg_h, dg_fmt;            /* what it was made for */
    int deint_failed;                  /* couldn't be made: pictures go straight through */
    unsigned n_interlaced, n_deint;
    int64_t t_deint;                   /* microseconds */
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
    struct SwsContext *sws_yuv;         /* reelcore_draw_yuv420: other formats to 4:2:0 */
    uint8_t *half[REELCORE_HALVINGS];  /* big reductions: the picture halved, once per level */
    size_t half_size[REELCORE_HALVINGS];
    int halvings;                      /* how many the last conversion did (reelcore_stats) */

    /* a layer's own state (reelcore's textures), released on close */
    void *attach;
    void (*attach_release)(void *);
};

static char last_error[256];

/* The context the sound comes from */
static AVFormatContext *actx(const ReelCore *v) { return v->afmt ? v->afmt : v->fmt; }

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

/* SharedSoundBuffer_Volume: left and right, 16 bits each (0xFFFF full) */
static int ssb_volume_word(const ReelCore *v)
{
    unsigned l = (unsigned)(av_clipd(v->vol, 0, 1) * 0xFFFF + 0.5);
    return (int)(l | l << 16);
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
    ssb_swi(SharedSoundBuffer_Volume, v->ssb_handle, ssb_volume_word(v), 0, NULL);
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

static void timer_set(ReelCore *v, double pts);

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
    timer_set(v, c);
}

static double clock_now(ReelCore *v)
{
    if (v->paused)
        return v->pause_pos;
    if (v->audio_clock && v->audio_end >= 0) {
        double q = queued_audio(v);
        if (v->eof_audio && q <= 0) {
            /* the sound has finished: carry on with the timer */
            double c = v->audio_end - v->latency * v->speed;
            v->audio_clock = 0;
            timer_set(v, c);
            return c;
        }
        {
            /* q and the latency are real time; at speed s they hold s times
               as much of the file */
            double c = v->audio_end - (q + v->latency) * v->speed;
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
    return (av_gettime_relative() - v->t0) / 1e6 * v->speed;
}

/* Make the timer read pts now. */
static void timer_set(ReelCore *v, double pts)
{
    v->t0 = av_gettime_relative() - (int64_t)(pts / v->speed * 1e6);
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

/* ---------------------------------------------------------------- network sources

   An address (http:, https:, and so on) is opened and read by a thread of
   its own, so that a slow or stalled connection never holds up the caller
   (on RISC OS: the desktop). The thread opens the input(s), chooses the
   streams, then reads packets ahead into a queue: up to NET_AHEAD seconds
   ahead of the picture being shown, or NET_MAX_BYTES. reelcore_update takes
   packets from the queue instead of calling av_read_frame; when it's empty
   the decoders simply wait (and, with sound, so does the clock). Seeking
   asks the thread to seek; a serial number tells old packets from new.

   RISC OS threads (UnixLib) only run while the task is paged in, and share
   one core: reelcore_update yields to the reader for a few milliseconds when
   the queue is low, and reelcore_idle_time asks to be woken sooner, so the
   reader gets time even when the caller would otherwise sleep. UnixLib's
   select() (which FFmpeg's sockets wait in) yields to the other threads
   while it waits, so a reader waiting for the network doesn't stop the
   caller. */

typedef struct NetPkt { AVPacket *pkt; int kind; } NetPkt;   /* kind 0 video, 1 sound */

struct Net {
    pthread_t th;
    int started;
    pthread_mutex_t lock;
    pthread_cond_t cond;               /* the reader waits here for room, or a seek */
    int quit;
    char *url, *audio_url, *headers, *user_agent;
    int state;                         /* 0 opening, 1 open, -1 failed */
    char error[200];
    NetPkt *q;
    int head, n, cap;
    size_t bytes;
    double last[2];                    /* seconds: the newest packet of each kind queued */
    int eof[2], eof_all;
    int serial;                        /* bumped by each seek */
    int seek_req;
    double seek_to;
    double play_pos;                   /* the picture shown now (from the caller) */
    int64_t bytes_read;
    char read_error[160];              /* the connection failed while reading */
};

int reelcore_is_network(const char *url)
{
    const char *c = url ? strstr(url, "://") : NULL;
    if (!c || c == url)
        return 0;
    for (const char *p = url; p < c; p++)                 /* a scheme: letters, digits, + - . */
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '+' || *p == '-' || *p == '.'))
            return 0;
    return av_strncasecmp(url, "file:", 5) != 0;
}

static int net_interrupt(void *opaque)
{
    struct Net *n = opaque;
    return n && n->quit;
}

/* Waits for up to ms milliseconds, or until signalled; lock held */
static void net_wait(struct Net *n, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&n->cond, &n->lock, &ts);
}

/* Opens one input: a file or an address, with the options network input
   needs (reconnecting, a timeout, a bigger socket buffer, the caller's HTTP
   headers), and local playlists (HLS .m3u8) allowed to refer to addresses. */
static int open_input(ReelCore *v, AVFormatContext **fc, const char *url, const ReelCoreSource *src)
{
    AVDictionary *o = NULL;
    int ret;
    if (!(*fc = avformat_alloc_context()))
        return AVERROR(ENOMEM);
    (*fc)->interrupt_callback.callback = net_interrupt;
    (*fc)->interrupt_callback.opaque = v->net;
    av_dict_set(&o, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data,httpproxy", 0);
    if (reelcore_is_network(url)) {
        av_dict_set(&o, "reconnect", "1", 0);
        av_dict_set(&o, "reconnect_on_network_error", "1", 0);
        av_dict_set(&o, "reconnect_delay_max", "4", 0);
        av_dict_set(&o, "rw_timeout", "20000000", 0);       /* 20 s without data: give up */
        av_dict_set(&o, "recv_buffer_size", "262144", 0);
        if (src && src->headers && *src->headers)
            av_dict_set(&o, "headers", src->headers, 0);
        if (src && src->user_agent && *src->user_agent)
            av_dict_set(&o, "user_agent", src->user_agent, 0);
    }
    ret = avformat_open_input(fc, url, NULL, &o);
    av_dict_free(&o);
    if (ret >= 0)
        ret = avformat_find_stream_info(*fc, NULL);
    return ret;
}

/* The streams to play: the best video, and the best sound (from the second
   input when there is one); the rest aren't read at all. */
static int select_streams(ReelCore *v)
{
    AVFormatContext *a;
    v->vs = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (v->vs < 0) {
        set_error("%s", "no video in the file");
        return -1;
    }
    a = actx(v);
    v->as = v->flags & REELCORE_NO_AUDIO ? -1
          : av_find_best_stream(a, AVMEDIA_TYPE_AUDIO, -1, a == v->fmt ? v->vs : -1, NULL, 0);
    for (unsigned i = 0; i < v->fmt->nb_streams; i++)
        if ((int)i != v->vs && (v->afmt || (int)i != v->as))
            v->fmt->streams[i]->discard = AVDISCARD_ALL;
    if (v->afmt)
        for (unsigned i = 0; i < v->afmt->nb_streams; i++)
            if ((int)i != v->as)
                v->afmt->streams[i]->discard = AVDISCARD_ALL;
    return 0;
}

/* After the inputs are open and the streams chosen: the decoders, the
   picture's size and rate, the sound device. In the caller's thread. */
static int setup_decoders(ReelCore *v)
{
    AVStream *st = v->fmt->streams[v->vs];
    if (!(v->vdec = open_decoder(st))) {
        set_error("no decoder for the video (%s)", avcodec_get_name(st->codecpar->codec_id));
        return -1;
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
    v->duration = v->fmt->duration > 0 ? v->fmt->duration / (double)AV_TIME_BASE :
                  v->afmt && v->afmt->duration > 0 ? v->afmt->duration / (double)AV_TIME_BASE : 0;
    if (v->as >= 0 && (!(v->adec = open_decoder(actx(v)->streams[v->as])) || open_audio(v) < 0)) {
        avcodec_free_context(&v->adec);
        v->as = -1;                        /* (its packets are dropped as they come) */
    }
    v->audio_clock = v->dev != 0;
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    if (!v->pkt || !v->frame)
        return -1;
    if (v->flags & REELCORE_PAUSED) {
        v->paused = 1;
        v->pause_pos = 0;
    } else if (v->dev)
        aud_pause(v, 0);
    timer_set(v, 0);
    v->ready = 1;
    return 0;
}

static void open_error(const char *what, int ret)
{
    char e[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, e, sizeof(e));
    snprintf(last_error, sizeof(last_error), "can't open %s: %s", what, e);
    av_log(NULL, AV_LOG_ERROR, "reelcore: %s\n", last_error);
}

static void net_push(struct Net *n, AVPacket *pkt, int kind, double t)
{
    if (n->n == n->cap) {
        int cap = n->cap ? n->cap * 2 : 256;
        NetPkt *q = av_malloc_array(cap, sizeof(*q));
        if (!q)
            return;
        for (int i = 0; i < n->n; i++)
            q[i] = n->q[(n->head + i) % n->cap];
        av_free(n->q);
        n->q = q;
        n->cap = cap;
        n->head = 0;
    }
    NetPkt *e = &n->q[(n->head + n->n) % n->cap];
    if (!(e->pkt = av_packet_alloc()))
        return;
    av_packet_move_ref(e->pkt, pkt);
    e->kind = kind;
    n->n++;
    n->bytes += e->pkt->size;
    if (t != AV_NOPTS_VALUE && t > n->last[kind])
        n->last[kind] = t;
}

static void net_flush(struct Net *n)
{
    while (n->n) {
        av_packet_free(&n->q[n->head].pkt);
        n->head = (n->head + 1) % n->cap;
        n->n--;
    }
    n->head = 0;
    n->bytes = 0;
    n->last[0] = n->last[1] = -1e9;
}

/* Seconds read ahead of the picture shown (the kind behind); lock held */
static double net_ahead(const ReelCore *v)
{
    const struct Net *n = v->net;
    double a = n->eof[0] ? 1e9 : n->last[0] - n->play_pos;
    if (v->as >= 0 && !n->eof[v->afmt ? 1 : 0]) {
        double b = n->last[1] - n->play_pos;
        if (b < a)
            a = b;
    }
    return a;
}

/* Seconds of a packet from the start of its input */
static double pkt_time(AVFormatContext *fc, const AVPacket *p)
{
    AVStream *st = fc->streams[p->stream_index];
    int64_t t = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
    double s;
    if (t == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    s = t * av_q2d(st->time_base);
    if (fc->start_time != AV_NOPTS_VALUE)
        s -= fc->start_time / (double)AV_TIME_BASE;
    return s;
}

/* The kind of a packet read from fc: 0 video, 1 sound, -1 not played */
static int pkt_kind(const ReelCore *v, AVFormatContext *fc, const AVPacket *p)
{
    if (fc == v->fmt && p->stream_index == v->vs)
        return 0;
    if (fc == actx(v) && p->stream_index == v->as)
        return 1;
    return -1;
}

static void seek_input(AVFormatContext *fc, double seconds)
{
    int64_t ts = (int64_t)(seconds * AV_TIME_BASE);
    if (fc->start_time != AV_NOPTS_VALUE)
        ts += fc->start_time;
    avformat_seek_file(fc, -1, INT64_MIN, ts, ts, 0);
}

static void *net_thread(void *arg)
{
    ReelCore *v = arg;
    struct Net *n = v->net;
    ReelCoreSource src = { n->url, n->audio_url, n->headers, n->user_agent, NULL };
    AVPacket *pkt = av_packet_alloc();
    int ret;

    ret = open_input(v, &v->fmt, n->url, &src);
    if (ret < 0)
        open_error(n->audio_url ? "the video" : "the address", ret);
    else if (n->audio_url && (ret = open_input(v, &v->afmt, n->audio_url, &src)) < 0)
        open_error("the sound", ret);
    else if (select_streams(v) < 0)
        ret = -1;
    pthread_mutex_lock(&n->lock);
    n->state = ret < 0 || !pkt ? -1 : 1;
    if (n->state < 0)
        snprintf(n->error, sizeof(n->error), "%s", n->quit ? "stopped" : last_error);
    pthread_mutex_unlock(&n->lock);
    if (n->state < 0) {
        av_packet_free(&pkt);
        return NULL;
    }

    pthread_mutex_lock(&n->lock);
    while (!n->quit) {
        int serial, full, k;
        AVFormatContext *fc;
        if (n->seek_req) {
            double to = n->seek_to;
            serial = n->serial;
            n->seek_req = 0;
            pthread_mutex_unlock(&n->lock);
            seek_input(v->fmt, to);
            if (v->afmt)
                seek_input(v->afmt, to);
            pthread_mutex_lock(&n->lock);
            if (serial == n->serial) {
                net_flush(n);
                n->eof[0] = n->eof[1] = n->eof_all = 0;
            }
            continue;
        }
        full = n->bytes > NET_MAX_BYTES || (n->n && net_ahead(v) >= NET_AHEAD);
        if (full || n->eof_all) {
            net_wait(n, 50);                /* room, a seek or the end */
            continue;
        }
        /* from the input that's behind */
        k = v->afmt && !n->eof[1] && (n->eof[0] || n->last[1] < n->last[0]) ? 1 : 0;
        fc = k ? v->afmt : v->fmt;
        serial = n->serial;
        pthread_mutex_unlock(&n->lock);
        ret = av_read_frame(fc, pkt);
        pthread_mutex_lock(&n->lock);
        if (serial != n->serial) {          /* a seek meanwhile: from before it */
            av_packet_unref(pkt);
            continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            net_wait(n, 5);
            continue;
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF && ret != AVERROR_EXIT && !n->quit) {
                char e[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, e, sizeof(e));
                snprintf(n->read_error, sizeof(n->read_error), "reading stopped: %s", e);
                av_log(NULL, AV_LOG_WARNING, "reelcore: %s\n", n->read_error);
            }
            n->eof[k] = 1;
            n->eof_all = n->eof[0] && (!v->afmt || n->eof[1]);
            continue;
        }
        n->bytes_read += pkt->size;
        {
            int kind = pkt_kind(v, fc, pkt);
            if (kind >= 0)
                net_push(n, pkt, kind, pkt_time(fc, pkt));
            av_packet_unref(pkt);
        }
    }
    pthread_mutex_unlock(&n->lock);
    av_packet_free(&pkt);
    return NULL;
}

/* Gives the reader some time (RISC OS: a single core, threads switched by
   UnixLib only while the task runs) while its queue is low */
static void net_give_time(ReelCore *v, int ms)
{
#ifdef __riscos__
    int64_t end = av_gettime_relative() + ms * 1000;
    for (;;) {
        int enough;
        pthread_mutex_lock(&v->net->lock);
        enough = v->net->state && (v->net->eof_all || net_ahead(v) >= NET_LOW);
        pthread_mutex_unlock(&v->net->lock);
        if (enough || av_gettime_relative() >= end)
            break;
        pthread_yield();
    }
#else
    (void)v; (void)ms;                    /* real threads: it runs anyway */
#endif
}

static ReelCore *core_alloc(int flags)
{
    ReelCore *v = av_mallocz(sizeof(*v));
    if (!v)
        return NULL;
    v->flags = flags;
    v->vs = v->as = -1;
    v->audio_end = -1;
    v->seek_target = v->aseek_target = -1;
    v->need_first = 1;
    v->volume = SDL_MIX_MAXVOLUME;
    v->vol = 1;
    v->speed = 1;
    v->deint = REELCORE_DEINT_AUTO;
    v->cs_key[0] = -1;
    return v;
}

ReelCore *reelcore_open_source(const ReelCoreSource *src, int flags)
{
    ReelCore *v;
    int ret;

    if (!src || !src->url) {
        set_error("%s", "nothing to open");
        return NULL;
    }
    if (!(v = core_alloc(flags)))
        return NULL;
    if (src->title && !(v->title = av_strdup(src->title)))
        goto fail;

    if (reelcore_is_network(src->url) || (src->audio_url && reelcore_is_network(src->audio_url))) {
        struct Net *n = av_mallocz(sizeof(*n));
        if (!(v->net = n))
            goto fail;
        pthread_mutex_init(&n->lock, NULL);
        pthread_cond_init(&n->cond, NULL);
        n->last[0] = n->last[1] = -1e9;
        n->url = av_strdup(src->url);
        n->audio_url = src->audio_url ? av_strdup(src->audio_url) : NULL;
        n->headers = src->headers ? av_strdup(src->headers) : NULL;
        n->user_agent = src->user_agent ? av_strdup(src->user_agent) : NULL;
        if (!n->url || pthread_create(&n->th, NULL, net_thread, v) != 0) {
            set_error("%s", "can't start reading");
            goto fail;
        }
        n->started = 1;
        if (flags & REELCORE_ASYNC)
            return v;                      /* reelcore_update says when it's open */
        for (;;) {                         /* wait for it here */
            int state;
            pthread_mutex_lock(&n->lock);
            state = n->state;
            pthread_mutex_unlock(&n->lock);
            if (state < 0) {
                snprintf(last_error, sizeof(last_error), "%s", n->error);
                goto fail;
            }
            if (state > 0)
                break;
#ifdef __riscos__
            pthread_yield();
#else
            av_usleep(2000);
#endif
        }
        if (setup_decoders(v) < 0)
            goto fail;
        return v;
    }

    /* a file: opened here, read here */
    if ((ret = open_input(v, &v->fmt, src->url, src)) < 0) {
        open_error("the file", ret);
        goto fail;
    }
    if (src->audio_url && (ret = open_input(v, &v->afmt, src->audio_url, src)) < 0) {
        open_error("the sound", ret);
        goto fail;
    }
    if (select_streams(v) < 0 || setup_decoders(v) < 0)
        goto fail;
    return v;

fail:
    reelcore_close(v);
    return NULL;
}

ReelCore *reelcore_open(const char *url, int flags)
{
    ReelCoreSource src = { url, NULL, NULL, NULL, NULL };
    return reelcore_open_source(&src, flags & ~REELCORE_ASYNC);
}

/* The reader's state: 0 opening, 1 open, -1 failed */
static int net_state(const ReelCore *v)
{
    int state;
    pthread_mutex_lock(&v->net->lock);
    state = v->net->state;
    pthread_mutex_unlock(&v->net->lock);
    return state;
}

int reelcore_ready(const ReelCore *v)
{
    if (v->ready)
        return 1;
    return v->net && net_state(v) < 0 ? -1 : 0;
}

int reelcore_net(const ReelCore *v, ReelCoreNet *st)
{
    struct Net *n = v->net;
    memset(st, 0, sizeof(*st));
    if (!n)
        return 0;
    pthread_mutex_lock(&n->lock);
    st->opening = n->state == 0;
    st->ahead = n->state > 0 && v->ready ? net_ahead(v) : 0;
    if (st->ahead > 1e8)
        st->ahead = n->eof_all ? 0 : st->ahead;
    st->bytes_ahead = (unsigned)n->bytes;
    st->bytes_read = n->bytes_read;
    st->ended = n->eof_all;
    st->buffering = v->ready && !n->eof_all && n->n == 0 && !v->paused;
    snprintf(st->error, sizeof(st->error), "%s", n->state < 0 ? n->error : n->read_error);
    pthread_mutex_unlock(&n->lock);
    return 1;
}

/* The next packet to decode: 0 with *kind (0 video, 1 sound, -1 not
   played), AVERROR(EAGAIN) when none has arrived yet (network), or the
   end/an error. */
static int next_packet(ReelCore *v, AVPacket *pkt, int *kind)
{
    if (v->net) {
        struct Net *n = v->net;
        int ret;
        pthread_mutex_lock(&n->lock);
        n->play_pos = reelcore_position(v);
        if (n->n) {
            NetPkt *e = &n->q[n->head];
            av_packet_move_ref(pkt, e->pkt);
            av_packet_free(&e->pkt);
            *kind = e->kind;
            n->bytes -= pkt->size;
            n->head = (n->head + 1) % n->cap;
            n->n--;
            pthread_cond_signal(&n->cond);  /* room */
            ret = 0;
        } else
            ret = n->eof_all ? AVERROR_EOF : AVERROR(EAGAIN);
        pthread_mutex_unlock(&n->lock);
        return ret;
    }
    if (v->afmt) {                         /* two files: from the one behind */
        for (;;) {
            int k = !v->leof[1] && (v->leof[0] || v->lpts[1] < v->lpts[0]) ? 1 : 0;
            AVFormatContext *fc = k ? v->afmt : v->fmt;
            int ret;
            if (v->leof[0] && v->leof[1])
                return AVERROR_EOF;
            ret = av_read_frame(fc, pkt);
            if (ret == AVERROR(EAGAIN))
                return ret;
            if (ret < 0) {
                v->leof[k] = 1;
                continue;
            }
            {
                double t = pkt_time(fc, pkt);
                if (t != AV_NOPTS_VALUE && t > v->lpts[k])
                    v->lpts[k] = t;
            }
            *kind = pkt_kind(v, fc, pkt);
            return 0;
        }
    }
    {
        int ret = av_read_frame(v->fmt, pkt);
        if (ret >= 0)
            *kind = pkt_kind(v, v->fmt, pkt);
        return ret;
    }
}

static void clear_queue(ReelCore *v)
{
    for (int i = 0; i < v->qn; i++)
        av_frame_free(&v->q[i]);
    v->qn = 0;
}

static void vpk_clear(ReelCore *v);
static void tempo_close(ReelCore *v);
static void deint_close(ReelCore *v);
static int tempo_open(ReelCore *v);

void reelcore_close(ReelCore *v)
{
    if (!v)
        return;
    if (v->net) {
        struct Net *n = v->net;
        if (n->started) {
            pthread_mutex_lock(&n->lock);
            n->quit = 1;                   /* (also interrupts what FFmpeg is waiting for) */
            pthread_cond_signal(&n->cond);
            pthread_mutex_unlock(&n->lock);
            pthread_join(n->th, NULL);
        }
        net_flush(n);
        av_free(n->q);
        av_free(n->url);
        av_free(n->audio_url);
        av_free(n->headers);
        av_free(n->user_agent);
        pthread_mutex_destroy(&n->lock);
        pthread_cond_destroy(&n->cond);
    }
    if (v->dev)
        aud_close(v);
    clear_queue(v);
    vpk_clear(v);
    av_freep(&v->vpk);
    av_frame_free(&v->cur);
    av_frame_free(&v->frame);
    deint_close(v);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->vdec);
    avcodec_free_context(&v->adec);
    avformat_close_input(&v->fmt);
    avformat_close_input(&v->afmt);
    av_free(v->net);
    av_free(v->title);
    swr_free(&v->swr);
    tempo_close(v);
    sws_freeContext(v->sws);
    sws_freeContext(v->sws_yuv);
    for (int i = 0; i < REELCORE_HALVINGS; i++)
        av_free(v->half[i]);
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
             : v->audio_clock && v->audio_end >= 0 ? v->audio_end - (q + v->latency) * v->speed
             : (av_gettime_relative() - v->t0) / 1e6 * v->speed;
    int n = snprintf(buf, size, "pos %.2f clock %.2f%s, %d pictures and %d packets (%u KB) waiting, "
                     "%u late%s, %u skip spells",
                     reelcore_position(v), c,
                     v->paused ? " (paused)" : v->audio_clock ? " (sound)" : " (timer)", v->qn,
                     v->vpk_n, (unsigned)(v->vpk_bytes >> 10), v->dropped,
                     v->skipping == 2 ? ", keyframes only" : v->skipping ? ", skipping non-reference frames" : "",
                     v->skip_spells);
    if (n >= size)
        return n;
    if (v->net) {
        ReelCoreNet ns;
        reelcore_net(v, &ns);
        n += snprintf(buf + n, size - n, "; net %s%.1f s ahead (%u KB), %lld KB read%s%s",
                      ns.opening ? "opening, " : ns.buffering ? "BUFFERING, " : "", ns.ahead,
                      ns.bytes_ahead >> 10, ns.bytes_read >> 10, ns.ended ? ", all read" : "",
                      ns.error[0] ? ", " : "");
        if (ns.error[0] && n < size)
            n += snprintf(buf + n, size - n, "%s", ns.error);
        if (n >= size)
            return n;
    }
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
              : v->audio_clock && v->audio_end >= 0 ? v->audio_end - (st->sound_queued + v->latency) * v->speed
              : (av_gettime_relative() - v->t0) / 1e6 * v->speed;
    st->fps = v->fps;
    st->decoded = v->n_decoded;
    st->shown = v->n_shown;
    st->late = v->dropped;
    st->decode_time = v->t_decode / 1e6;
    st->audio_time = v->t_audio / 1e6;
    st->convert_time = v->t_convert / 1e6;
    st->convert_w = v->conv_w;
    st->convert_h = v->conv_h;
    st->halvings = v->halvings;
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
    if (v->net) {
        pthread_mutex_lock(&v->net->lock);
        st->bytes_read = v->net->bytes_read;
        pthread_mutex_unlock(&v->net->lock);
    } else
        st->bytes_read = (v->fmt && v->fmt->pb ? v->fmt->pb->bytes_read : 0) +
                         (v->afmt && v->afmt->pb ? v->afmt->pb->bytes_read : 0);
    st->speed = v->speed;
    st->fast = v->fast;
    st->deinterlace = v->deint;
    st->interlaced = v->n_interlaced;
    st->deinterlaced = v->n_deint;
    st->deinterlace_time = v->t_deint / 1e6;
    st->audio_track = reelcore_audio_track(v);
    st->audio_tracks = reelcore_audio_tracks(v);
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
    if (!v->ready)
        return 0;
    ADD("#File\n");
    if (v->title)
        ADD("Title\t%s\n", v->title);
    if (fc->url && v->net) {
        ADD("Address\t%.150s%s\n", fc->url, strlen(fc->url) > 150 ? "..." : "");
        if (v->afmt && v->afmt->url)
            ADD("Sound from\t%.150s%s\n", v->afmt->url, strlen(v->afmt->url) > 150 ? "..." : "");
    } else if (fc->url) {
        const char *leaf = strrchr(fc->url, fc->url[0] == '/' ? '/' : '.');
        ADD("Name\t%s\n", leaf ? leaf + 1 : fc->url);
        if (v->afmt && v->afmt->url)
            ADD("Sound from\t%s\n", v->afmt->url);
    }
    if (!v->title && (t = av_dict_get(fc->metadata, "title", NULL, 0)))
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
        ADD("Scan\t%s\n", p->field_order == AV_FIELD_PROGRESSIVE ? "progressive" :
                          p->field_order == AV_FIELD_TT || p->field_order == AV_FIELD_TB ? "interlaced, top field first" :
                          p->field_order == AV_FIELD_BB || p->field_order == AV_FIELD_BT ? "interlaced, bottom field first" :
                          v->n_interlaced ? "interlaced (the pictures say)" : "not given");
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
        const AVStream *st = actx(v)->streams[v->as];
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
    if (!v->ready)
        return snprintf(buf, size, "opening");
    const AVCodecParameters *vp = v->fmt->streams[v->vs]->codecpar;
    int n = snprintf(buf, size, "%s %dx%d", avcodec_get_name(vp->codec_id), vp->width, vp->height);
    if (v->fps > 0 && n < size)
        n += snprintf(buf + n, size - n, ", %.3g fps", v->fps);
    if (v->as >= 0 && n < size) {
        const AVCodecParameters *ap = actx(v)->streams[v->as]->codecpar;
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
    double start;
    if (!v->ready)
        return 0;
    start = v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time / (double)AV_TIME_BASE : 0;
    return v->cur ? v->cur_pts - start : 0;
}
int reelcore_paused(const ReelCore *v)       { return v->paused; }

void reelcore_set_volume(ReelCore *v, double volume)
{
    v->vol = av_clipd(volume, 0, 1);
#ifdef USE_SSB
    if (v->ssb && v->ssb_handle) {
        /* SharedSoundBuffer's own volume: heard at once, not after the
           0.5 s already queued */
        ssb_swi(SharedSoundBuffer_Volume, v->ssb_handle, ssb_volume_word(v), 0, NULL);
        return;
    }
#endif
    v->volume = (int)(v->vol * SDL_MIX_MAXVOLUME + 0.5);
}

double reelcore_volume(const ReelCore *v) { return v->vol; }

/* ---------------------------------------------------------------- decode */

static double frame_pts(AVFrame *f, AVStream *st, double fallback)
{
    int64_t t = f->best_effort_timestamp;
    return t == AV_NOPTS_VALUE ? fallback : t * av_q2d(st->time_base);
}

/* Queues a picture (a new reference to f) to be shown at pts. */
static void queue_picture(ReelCore *v, AVFrame *f, double pts)
{
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

/* ---- deinterlacing: yadif ---- */

static void deint_close(ReelCore *v)
{
    avfilter_graph_free(&v->dgraph);
    v->din = v->dout = NULL;
    av_frame_free(&v->dframe);
}

/* buffer -> yadif (a picture a frame) -> buffersink, for f's size and
   format; times in microseconds. */
static int deint_open(ReelCore *v, const AVFrame *f)
{
    char args[200];
    AVFilterContext *y = NULL;
    AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : (AVRational){ 1, 1 };
    deint_close(v);
    if (!(v->dgraph = avfilter_graph_alloc()) || !(v->dframe = av_frame_alloc()))
        goto fail;
    v->dgraph->nb_threads = 1;            /* no threads of its own (a Wimp task) */
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=%d/%d",
             f->width, f->height, f->format, sar.num, sar.den);
    if (avfilter_graph_create_filter(&v->din, avfilter_get_by_name("buffer"), "in", args, NULL, v->dgraph) < 0)
        goto fail;
    snprintf(args, sizeof(args), "mode=send_frame:parity=auto:deint=%s", v->deint == REELCORE_DEINT_ON ? "all" : "interlaced");
    if (avfilter_graph_create_filter(&y, avfilter_get_by_name("yadif"), "yadif", args, NULL, v->dgraph) < 0 ||
        avfilter_graph_create_filter(&v->dout, avfilter_get_by_name("buffersink"), "out", NULL, NULL, v->dgraph) < 0 ||
        avfilter_link(v->din, 0, y, 0) < 0 || avfilter_link(y, 0, v->dout, 0) < 0 ||
        avfilter_graph_config(v->dgraph, NULL) < 0)
        goto fail;
    v->dg_w = f->width;
    v->dg_h = f->height;
    v->dg_fmt = f->format;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: deinterlacing (yadif, %s) %dx%d %s\n",
           v->deint == REELCORE_DEINT_ON ? "every picture" : "interlaced pictures", f->width, f->height,
           av_get_pix_fmt_name(f->format));
    return 0;
fail:
    av_log(NULL, AV_LOG_WARNING, "reelcore: can't deinterlace (%s); pictures are shown as they are\n",
           av_get_pix_fmt_name(f->format));
    deint_close(v);
    v->deint_failed = 1;
    return -1;
}

/* Queues what yadif has ready */
static void deint_drain(ReelCore *v)
{
    AVFrame *o = v->dframe;
    double tb = av_q2d(av_buffersink_get_time_base(v->dout));
    while (av_buffersink_get_frame(v->dout, o) >= 0) {
        queue_picture(v, o, o->pts * tb);
        v->n_deint++;
        av_frame_unref(o);
    }
}

/* Gives yadif a picture (it gives back the one before: it needs the next
   to deinterlace). Returns -1 if the picture must be queued as it is. */
static int deint_feed(ReelCore *v, AVFrame *f, double pts)
{
    int64_t t0 = av_gettime_relative(), keep_pts = f->pts;
    int ret;
    if ((!v->dgraph || f->width != v->dg_w || f->height != v->dg_h || f->format != v->dg_fmt) &&
        deint_open(v, f) < 0)
        return -1;
    f->pts = llrint(pts * 1e6);
    ret = av_buffersrc_add_frame_flags(v->din, f, AV_BUFFERSRC_FLAG_KEEP_REF);
    f->pts = keep_pts;
    if (ret < 0)
        return -1;
    deint_drain(v);
    v->t_deint += av_gettime_relative() - t0;
    return 0;
}

static void got_video(ReelCore *v, AVFrame *f)
{
    double pts = frame_pts(f, v->fmt->streams[v->vs],
                           v->qn ? v->qpts[v->qn - 1] + (v->fps > 0 ? 1 / v->fps : 0.04)
                                 : v->cur ? v->cur_pts : 0);
    v->n_decoded++;
    if (f->interlaced_frame)
        v->n_interlaced++;
    /* AUTO: the graph is made at the first interlaced picture, then kept */
    if (!v->deint_failed && (v->deint == REELCORE_DEINT_ON ||
                             (v->deint == REELCORE_DEINT_AUTO && (f->interlaced_frame || v->dgraph))) &&
        deint_feed(v, f, pts) == 0)
        return;
    queue_picture(v, f, pts);
}

/* Gives the device sound (16-bit stereo at v->rate), at the volume set
   when SDL is the output (SharedSoundBuffer has its own volume). */
static void queue_sound(ReelCore *v, const uint8_t *data, int bytes)
{
    if (v->volume < SDL_MIX_MAXVOLUME) {
        uint8_t *m = av_mallocz(bytes);
        if (m) {
            SDL_MixAudioFormat(m, data, AUDIO_S16SYS, bytes, v->volume);
            aud_queue(v, m, bytes);
            av_free(m);
        }
    } else
        aud_queue(v, data, bytes);
}

/* ---- speed: the sound through atempo (same pitch) ---- */

static void tempo_close(ReelCore *v)
{
    avfilter_graph_free(&v->tempo);
    v->tempo_in = v->tempo_out = NULL;
    av_frame_free(&v->tempo_frame);
}

/* A graph abuffer -> atempo=speed -> abuffersink for 16-bit stereo at
   v->rate; none at speed 1. */
static int tempo_open(ReelCore *v)
{
    char args[160];
    AVFilterContext *t = NULL;
    tempo_close(v);
    if (v->speed == 1 || !v->rate)
        return 0;
    if (!(v->tempo = avfilter_graph_alloc()) || !(v->tempo_frame = av_frame_alloc()))
        goto fail;
    v->tempo->nb_threads = 1;             /* no threads of its own (a Wimp task) */
    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=s16:channel_layout=stereo:time_base=1/%d",
             v->rate, v->rate);
    if (avfilter_graph_create_filter(&v->tempo_in, avfilter_get_by_name("abuffer"), "in", args, NULL, v->tempo) < 0)
        goto fail;
    snprintf(args, sizeof(args), "tempo=%.4f", v->speed);
    if (avfilter_graph_create_filter(&t, avfilter_get_by_name("atempo"), "tempo", args, NULL, v->tempo) < 0 ||
        avfilter_graph_create_filter(&v->tempo_out, avfilter_get_by_name("abuffersink"), "out", NULL, NULL, v->tempo) < 0 ||
        avfilter_link(v->tempo_in, 0, t, 0) < 0 || avfilter_link(t, 0, v->tempo_out, 0) < 0 ||
        avfilter_graph_config(v->tempo, NULL) < 0)
        goto fail;
    return 0;
fail:
    av_log(NULL, AV_LOG_WARNING, "reelcore: can't change the sound's speed; it plays at normal speed\n");
    tempo_close(v);
    return -1;
}

/* Takes what atempo has ready: each output sample is v->speed of the file. */
static void tempo_drain(ReelCore *v)
{
    AVFrame *o = v->tempo_frame;
    while (av_buffersink_get_frame(v->tempo_out, o) >= 0) {
        queue_sound(v, o->data[0], o->nb_samples * 4);
        v->audio_end += o->nb_samples / (double)v->rate * v->speed;
        av_frame_unref(o);
    }
}

static void tempo_feed(ReelCore *v, const uint8_t *data, int n)
{
    AVFrame *f = av_frame_alloc();
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (!f)
        return;
    f->nb_samples = n;
    f->format = AV_SAMPLE_FMT_S16;
    f->sample_rate = v->rate;
    av_channel_layout_copy(&f->ch_layout, &stereo);
    if (av_frame_get_buffer(f, 0) >= 0) {
        memcpy(f->data[0], data, (size_t)n * 4);
        if (av_buffersrc_add_frame(v->tempo_in, f) >= 0)
            tempo_drain(v);
    }
    av_frame_free(&f);
}

static void got_audio(ReelCore *v, AVFrame *f)
{
    int out_max = swr_get_out_samples(v->swr, f->nb_samples);
    int n, bytes;
    double pts = frame_pts(f, actx(v)->streams[v->as], v->audio_end >= 0 ? v->audio_end : 0);

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
    if (v->tempo)
        tempo_feed(v, v->abuf, n);
    else {
        queue_sound(v, v->abuf, bytes);
        v->audio_end += n / (double)v->rate;
    }
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
            if (video) {
                if (v->dgraph && av_buffersrc_add_frame(v->din, NULL) >= 0)
                    deint_drain(v);       /* the last picture yadif held */
                v->eof_video = 1;
            } else {
                v->eof_audio = 1;
                if (v->tempo && av_buffersrc_add_frame(v->tempo_in, NULL) >= 0)
                    tempo_drain(v);       /* what atempo still holds */
                if (v->dev && !v->stalled)
                    aud_flush(v);
            }
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
        int ret, kind;
        if (!need_a && !need_v)
            break;
        if (!need_v && v->vpk_bytes > VPK_MAX_BYTES)
            break;                         /* a strange file: don't eat all the memory */
        ret = next_packet(v, v->pkt, &kind);
        if (ret == AVERROR(EAGAIN))
            break;                         /* (the network: not here yet) */
        if (ret < 0) {                     /* the end: flush the sound decoder now, */
            v->eof_demux = 1;              /* the video's once its packets are done */
            if (v->adec) decode(v, v->adec, NULL, 0); else v->eof_audio = 1;
            break;
        }
        if (kind == 0)
            vpk_push(v, v->pkt);
        else if (kind == 1 && v->adec)
            decode(v, v->adec, v->pkt, 0);
        av_packet_unref(v->pkt);
    }
    /* after a seek, decode on to the seek point in one go (as before) */
    int budget = v->need_first ? READ_BUDGET : DECODE_BUDGET;
    for (int n = 0; n < budget && v->qn < 3; n++) {
        if (!v->vpk_n && !v->eof_demux && v->need_first) {
            /* more packets on the way to the seek point */
            int kind, ret = next_packet(v, v->pkt, &kind);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                v->eof_demux = 1;
                if (v->adec) decode(v, v->adec, NULL, 0); else v->eof_audio = 1;
            } else if (ret >= 0) {
                if (kind == 0)
                    vpk_push(v, v->pkt);
                else if (kind == 1 && v->adec)
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

    if (!v->ready) {                       /* opening (REELCORE_ASYNC) */
        int state = v->net ? net_state(v) : -1;
        if (state < 0) {
            pthread_mutex_lock(&v->net->lock);
            snprintf(last_error, sizeof(last_error), "%s", v->net->error);
            pthread_mutex_unlock(&v->net->lock);
            return REELCORE_FAILED;
        }
        if (state == 0) {
            net_give_time(v, 10);
            return REELCORE_OPENING;
        }
        if (setup_decoders(v) < 0) {
            if (!last_error[0])
                set_error("%s", "can't play it");
            return REELCORE_FAILED;
        }
        return REELCORE_READY;
    }
    if (v->net) {
        int low;
        pthread_mutex_lock(&v->net->lock);
        v->net->play_pos = reelcore_position(v);
        low = !v->net->eof_all && net_ahead(v) < NET_LOW;
        pthread_mutex_unlock(&v->net->lock);
        if (low)
            net_give_time(v, 8);
    }
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

static double reelcore_idle_time_play(ReelCore *v);

double reelcore_idle_time(ReelCore *v)
{
    if (!v->ready)
        return 0.02;                       /* opening: the reader needs time */
    if (v->net) {                          /* reading ahead: come back soon */
        int low;
        pthread_mutex_lock(&v->net->lock);
        low = !v->net->eof_all && (v->net->n == 0 || net_ahead(v) < NET_AHEAD - 1);
        pthread_mutex_unlock(&v->net->lock);
        if (low) {
            double d = reelcore_idle_time_play(v);
            return d > 0.02 ? 0.02 : d;
        }
    }
    return reelcore_idle_time_play(v);
}

static double reelcore_idle_time_play(ReelCore *v)
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
    due = (v->qpts[0] - clock_now(v)) / v->speed;   /* real seconds */
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
    if (!v->ready)
        return AVERROR(EAGAIN);
    if (v->fmt->start_time != AV_NOPTS_VALUE)
        ts += v->fmt->start_time;
    if (v->net) {                          /* the reader seeks; its queue is from before */
        pthread_mutex_lock(&v->net->lock);
        v->net->serial++;
        v->net->seek_req = 1;
        v->net->seek_to = seconds;
        net_flush(v->net);
        v->net->eof[0] = v->net->eof[1] = v->net->eof_all = 0;
        v->net->play_pos = seconds;
        pthread_cond_signal(&v->net->cond);
        pthread_mutex_unlock(&v->net->lock);
    } else {
        ret = avformat_seek_file(v->fmt, -1, INT64_MIN, ts, ts, 0);
        if (ret < 0)
            return ret;
        if (v->afmt) {
            seek_input(v->afmt, seconds);
            v->lpts[0] = v->lpts[1] = -1e9;
            v->leof[0] = v->leof[1] = 0;
        }
    }
    avcodec_flush_buffers(v->vdec);
    if (v->adec)
        avcodec_flush_buffers(v->adec);
    if (v->dev && !v->stalled)
        aud_clear(v);
    if (v->swr)
        swr_init(v->swr);                 /* drop what it buffered */
    if (v->tempo)
        tempo_open(v);                    /* and atempo */
    clear_queue(v);
    vpk_clear(v);
    deint_close(v);                       /* the pictures it held are from before */
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

/* Halving, for big reductions (the mini player: a 1280-pixel video in a
   320-pixel window is a quarter of its size).

   swscale's fast bilinear scaler, which Reel uses because it is quick,
   takes two source pixels for each output pixel whatever the reduction,
   so at a quarter size it skips three pixels in four: fine detail breaks
   up into jagged, blocky edges. Its better filters (area, bilinear) look
   at every pixel but cost three to six times as much.

   So while the picture is still at least twice the size wanted both ways,
   it is first halved: each output pixel the rounded average of a 2x2
   block, (a + b + c + d + 2) >> 2, for each plane. Halving twice is a 4x4
   box filter, as good as swscale's area filter, and cheap: in NEON, eight
   output pixels from two 16-byte loads, a pairwise add, an accumulate and
   a rounding narrow. swscale then does what's left (less than 2x), or,
   when the halving lands exactly on the size wanted (1280 -> 320), only
   the colour conversion.

   Only for 8-bit planar YUV (what nearly every video decodes to); other
   formats go straight to swscale as before. The NEON and C loops give
   the same bytes (tests/host/halve_test.c). vld1.8/vst1.8 never fault on
   RISC OS's alignment checking, whatever the addresses (docs/NEON.md). */

/* One plane: w x h output pixels at dst from 2w x 2h at src */
void reelcore_halve_plane(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint8_t *a = src + (ptrdiff_t)2 * y * spitch, *b = a + spitch;
        uint8_t *d = dst + (ptrdiff_t)y * dpitch;
        int x = 0;
#ifdef REELCORE_NEON
        for (; x + 8 <= w; x += 8) {
            uint16x8_t sum = vpaddlq_u8(vld1q_u8(a + 2 * x));    /* a0+a1, a2+a3, ... */
            sum = vpadalq_u8(sum, vld1q_u8(b + 2 * x));          /* + b0+b1, ... */
            vst1_u8(d + x, vrshrn_n_u16(sum, 2));               /* (sum + 2) >> 2 */
        }
#endif
        for (; x < w; x++)
            d[x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
}

/* 8-bit planar YUV without alpha: what halving handles */
static int can_halve(const AVPixFmtDescriptor *d)
{
    if (!d || d->nb_components != 3 ||
        (d->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL |
                     AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_BE)))
        return 0;
    for (int i = 0; i < 3; i++)
        if (d->comp[i].plane != i || d->comp[i].step != 1 || d->comp[i].depth != 8 || d->comp[i].shift)
            return 0;
    return 1;
}

/* Halves src (cw x ch) into v->half[] while it is still at least twice
   w x h both ways. Updates src, pitch, cw and ch; returns the levels done. */
static int halve(ReelCore *v, const AVPixFmtDescriptor *d, const uint8_t *src[4], int pitch[4],
                 int *cw, int *ch, int w, int h)
{
    int n = 0, lw = d->log2_chroma_w, lh = d->log2_chroma_h;
    while (n < REELCORE_HALVINGS && *cw >= 2 * w && *ch >= 2 * h &&
           *cw >= (4 << lw) && *ch >= (4 << lh)) {
        /* whole chroma samples on both sides: trim to a multiple of 2 chroma samples */
        int iw = *cw & ~((2 << lw) - 1), ih = *ch & ~((2 << lh) - 1);
        int ow = iw / 2, oh = ih / 2;
        int op[3], ow_p[3], oh_p[3];
        size_t size = 0;
        uint8_t *out[3];
        for (int p = 0; p < 3; p++) {
            ow_p[p] = p ? ow >> lw : ow;
            oh_p[p] = p ? oh >> lh : oh;
            op[p] = FFALIGN(ow_p[p], 16);
            size += (size_t)op[p] * oh_p[p];
        }
        if (size > v->half_size[n]) {
            av_free(v->half[n]);
            v->half[n] = av_malloc(size);
            v->half_size[n] = v->half[n] ? size : 0;
            if (!v->half[n])
                break;                            /* no memory: swscale does it all */
        }
        out[0] = v->half[n];
        out[1] = out[0] + (size_t)op[0] * oh_p[0];
        out[2] = out[1] + (size_t)op[1] * oh_p[1];
        for (int p = 0; p < 3; p++) {
            reelcore_halve_plane(out[p], op[p], src[p], pitch[p], ow_p[p], oh_p[p]);
            src[p] = out[p];
            pitch[p] = op[p];
        }
        *cw = ow;
        *ch = oh;
        n++;
    }
    return n;
}

/* Converts the part cw x ch at cx, cy of the current frame into w x h
   pixels at dst (fmt), with the frame's colour space and range. */
static int convert(ReelCore *v, uint8_t *dst, int pitch, int w, int h, enum AVPixelFormat fmt,
                   int cx, int cy, int cw, int ch)
{
    AVFrame *f = v->cur;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(f->format);
    const uint8_t *src[4] = { f->data[0], f->data[1], f->data[2], f->data[3] };
    uint8_t *d[4] = { dst };
    int ds[4] = { pitch };
    int sp[4] = { f->linesize[0], f->linesize[1], f->linesize[2], f->linesize[3] };
    int cs = f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    int full = f->color_range == AVCOL_RANGE_JPEG;
    int key[8];

    if (cx || cy || cw != f->width || ch != f->height) {
        if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL))) {
            cx = cy = 0;                          /* can't crop this format: all of it */
            cw = f->width;
            ch = f->height;
        } else {
            /* the crop starts on a whole chroma sample */
            int mw = (1 << desc->log2_chroma_w) - 1, mh = (1 << desc->log2_chroma_h) - 1;
            int done[4] = { 0 };
            cx &= ~mw;
            cy &= ~mh;
            cw = FFMAX(cw & ~mw, mw + 1);
            ch = FFMAX(ch & ~mh, mh + 1);
            for (int i = 0; i < desc->nb_components; i++) {
                const AVComponentDescriptor *c = &desc->comp[i];
                int chroma = (i == 1 || i == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
                if (done[c->plane])
                    continue;
                done[c->plane] = 1;
                src[c->plane] = f->data[c->plane] +
                                (cy >> (chroma ? desc->log2_chroma_h : 0)) * f->linesize[c->plane] +
                                (cx >> (chroma ? desc->log2_chroma_w : 0)) * c->step;
            }
        }
    }
    int64_t t0 = av_gettime_relative();         /* the conversion's time, halving included */
    v->halvings = can_halve(desc) ? halve(v, desc, src, sp, &cw, &ch, w, h) : 0;
    key[0] = cs; key[1] = full; key[2] = cw; key[3] = ch; key[4] = f->format;
    key[5] = w; key[6] = h; key[7] = fmt;
    v->sws = sws_getCachedContext(v->sws, cw, ch, f->format, w, h, fmt,
                                  (w == cw && h == ch) ? SWS_POINT : SWS_FAST_BILINEAR,
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
        int ret = sws_scale(v->sws, src, sp, 0, ch, d, ds);
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
    AVFrame *f = v->cur;
    int x = 0, y = 0, rw = w, rh = h, cx = 0, cy = 0, cw, ch;

    if (!f)
        return AVERROR(EAGAIN);
    if (w < 1 || h < 1)
        return AVERROR(EINVAL);
    cw = f->width;
    ch = f->height;
    if (flags & (REELCORE_FILL | REELCORE_ORIGINAL)) {
        /* the whole picture at scale s (display pixels, aspect applied):
           fill = cover the rectangle, original = 1:1; what's outside the
           rectangle is cropped, what's left over gets bars */
        double s = flags & REELCORE_FILL ? FFMAX((double)w / v->w, (double)h / v->h) : 1.0;
        double dw = v->w * s, dh = v->h * s;
        rw = FFMAX(dw < w ? (int)(dw + 0.5) : w, 1);
        rh = FFMAX(dh < h ? (int)(dh + 0.5) : h, 1);
        cw = FFMIN(FFMAX((int)(f->width * rw / dw + 0.5), 1), f->width);
        ch = FFMIN(FFMAX((int)(f->height * rh / dh + 0.5), 1), f->height);
        cx = (f->width - cw) / 2;
        cy = (f->height - ch) / 2;
        x = (w - rw) / 2;
        y = (h - rh) / 2;
    } else if (!(flags & REELCORE_STRETCH)) {
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
    }
    if ((x || y || rw < w || rh < h) && !(flags & REELCORE_NO_BORDERS)) {
        fill_black(p, pitch, 0, 0, w, y);
        fill_black(p, pitch, 0, y + rh, w, h - y - rh);
        fill_black(p, pitch, 0, y, x, rh);
        fill_black(p, pitch, x + rw, y, w - x - rw, rh);
    }
    /* RGBA/BGRA: the fourth byte isn't shown, and these get swscale's NEON */
    return convert(v, p + y * pitch + x * 4, pitch, rw, rh,
                   bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, cx, cy, cw, ch);
}

/* ---------------------------------------------------------------- options */

int reelcore_set_speed(ReelCore *v, double speed)
{
    double pos;
    if (!v->ready)
        return AVERROR(EAGAIN);
    speed = av_clipd(speed, 0.5, 2.0);
    if (speed == v->speed)
        return 0;
    pos = v->paused ? v->pause_pos : v->cur ? clock_now(v) : 0;
    v->speed = speed;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: speed %.2fx\n", speed);
    if (v->dev && !v->stalled && v->swr) {
        /* the sound already queued was made at the old speed: start again
           from the picture on screen, through a new atempo */
        tempo_open(v);
        return reelcore_seek(v, reelcore_position(v));
    }
    if (!v->paused)
        timer_set(v, pos);
    return 0;
}

double reelcore_speed(const ReelCore *v) { return v->speed; }

void reelcore_set_fast(ReelCore *v, int mode)
{
    if (!v->ready)
        return;
    if (mode != REELCORE_FAST_ON && mode != REELCORE_FAST_LIGHT)
        mode = REELCORE_FAST_OFF;
    if (mode == v->fast)
        return;
    v->fast = mode;
    v->vdec->skip_loop_filter = mode == REELCORE_FAST_ON ? AVDISCARD_ALL :
                                mode == REELCORE_FAST_LIGHT ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    if (mode == REELCORE_FAST_ON)       /* (the "fast" shortcuts change pictures others are predicted from) */
        v->vdec->flags2 |= AV_CODEC_FLAG2_FAST;
    else
        v->vdec->flags2 &= ~AV_CODEC_FLAG2_FAST;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: fast decoding %s\n",
           mode == REELCORE_FAST_ON ? "on (no deblocking)" :
           mode == REELCORE_FAST_LIGHT ? "light (no deblocking of pictures nothing is predicted from)" : "off");
}

int reelcore_fast(const ReelCore *v) { return v->fast; }

void reelcore_set_deinterlace(ReelCore *v, int mode)
{
    if (mode < REELCORE_DEINT_OFF || mode > REELCORE_DEINT_ON || mode == v->deint)
        return;
    v->deint = mode;
    v->deint_failed = 0;
    deint_close(v);                       /* made again as needed (the picture held is lost) */
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: deinterlace %s\n",
           mode == REELCORE_DEINT_ON ? "on" : mode == REELCORE_DEINT_AUTO ? "auto" : "off");
}

int reelcore_deinterlace(const ReelCore *v) { return v->deint; }

/* The file's i-th sound stream (0 = the first), its index, or -1 */
static int audio_stream(const ReelCore *v, int i)
{
    AVFormatContext *a = v->ready ? actx(v) : NULL;
    for (unsigned s = 0; a && s < a->nb_streams; s++)
        if (a->streams[s]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && i-- == 0)
            return (int)s;
    return -1;
}

int reelcore_audio_tracks(const ReelCore *v)
{
    int n = 0;
    while (audio_stream(v, n) >= 0)
        n++;
    return n;
}

int reelcore_audio_track(const ReelCore *v)
{
    for (int i = 0, s; (s = audio_stream(v, i)) >= 0; i++)
        if (s == v->as)
            return i;
    return -1;
}

int reelcore_audio_track_name(const ReelCore *v, int i, char *buf, int size)
{
    int s = audio_stream(v, i), n;
    const AVStream *st;
    const AVDictionaryEntry *lang, *title;
    if (s < 0)
        return snprintf(buf, size, "?");
    st = actx(v)->streams[s];
    lang = av_dict_get(st->metadata, "language", NULL, 0);
    title = av_dict_get(st->metadata, "title", NULL, 0);
    n = snprintf(buf, size, "%s, %d ch", avcodec_get_name(st->codecpar->codec_id), st->codecpar->ch_layout.nb_channels);
    if (lang && strcmp(lang->value, "und") && n < size)
        n += snprintf(buf + n, size - n, ", %s", lang->value);
    if (title && n < size)
        n += snprintf(buf + n, size - n, ", %s", title->value);
    return n;
}

int reelcore_set_audio_track(ReelCore *v, int i)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVCodecContext *dec;
    SwrContext *swr = NULL;
    int s = audio_stream(v, i);
    if (s < 0)
        return AVERROR(EINVAL);
    if (s == v->as)
        return 0;
    if (!v->dev)
        return AVERROR(ENODEV);                /* no sound output to play it on */
    if (v->net)
        return AVERROR(ENOSYS);                /* (the reader chose the streams) */
    if (!(dec = open_decoder(actx(v)->streams[s])))
        return AVERROR_DECODER_NOT_FOUND;
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, v->rate,
                            &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, NULL) < 0 ||
        swr_init(swr) < 0) {
        swr_free(&swr);
        avcodec_free_context(&dec);
        return AVERROR(EINVAL);
    }
    if (v->as >= 0)
        actx(v)->streams[v->as]->discard = AVDISCARD_ALL;
    actx(v)->streams[s]->discard = AVDISCARD_DEFAULT;
    avcodec_free_context(&v->adec);
    swr_free(&v->swr);
    v->adec = dec;
    v->swr = swr;
    v->as = s;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: sound track %d (stream %d)\n", i + 1, s);
    return reelcore_seek(v, reelcore_position(v));   /* the new track from here */
}

int reelcore_draw_yuv420(ReelCore *v, uint8_t *const planes[3], const int pitch[3], int w, int h, int *colour)
{
    AVFrame *f = v->cur;
    int64_t t0;
    int c = 0;
    if (!f)
        return AVERROR(EAGAIN);
    if (f->colorspace == AVCOL_SPC_BT709)            /* as convert() decides */
        c |= REELCORE_YUV_709;
    if (f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P)
        c |= REELCORE_YUV_FULL;
    if (colour)
        *colour = c;
    if (!planes)
        return 0;                                   /* just the colours */
    if (w < 2 || h < 2 || w > f->width || h > f->height)
        return AVERROR(EINVAL);
    t0 = av_gettime_relative();
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        /* the decoder's own planes: row copies (the rows may be wider) */
        for (int p = 0; p < 3; p++) {
            int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
            for (int y = 0; y < ph; y++)
                memcpy(planes[p] + (size_t)y * pitch[p], f->data[p] + (size_t)y * f->linesize[p], pw);
        }
    } else {
        /* anything else (10-bit, 4:2:2, 4:4:4, ...): to 4:2:0 at the same size */
        const uint8_t *src[4] = { f->data[0], f->data[1], f->data[2], f->data[3] };
        uint8_t *d[4] = { planes[0], planes[1], planes[2], NULL };
        int dp[4] = { pitch[0], pitch[1], pitch[2], 0 };
        v->sws_yuv = sws_getCachedContext(v->sws_yuv, w, h, f->format, w, h, AV_PIX_FMT_YUV420P,
                                          SWS_POINT, NULL, NULL, NULL);
        if (!v->sws_yuv || sws_scale(v->sws_yuv, src, f->linesize, 0, h, d, dp) < 0)
            return AVERROR_EXTERNAL;
    }
    v->t_convert += av_gettime_relative() - t0;
    v->conv_w = w;
    v->conv_h = h;
    v->halvings = 0;
    return 0;
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
