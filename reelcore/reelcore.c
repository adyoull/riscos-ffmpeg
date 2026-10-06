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
#include "libavutil/opt.h"
#ifdef REELCORE_HEVCDEC
#include <hwhevcdec.h>                     /* the HEVC block's frames, converted when shown */
#endif
#include "reelcore.h"
static void cur_changed(ReelCore *v);
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
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

#define QMAX          12     /* room for decoded frames kept ahead of the clock */
#define QROOM         (QMAX - 2)  /* a decoder's frames taken while fewer than this wait (yadif
                                    can give 2 a frame): the rest stay in the decoder */
#define HIST_N        10     /* pictures kept for stepping back */
#define PICS_AHEAD    0.13   /* seconds of pictures decoded ahead: rides out a slow one */
#define PICS_MIN      3      /* (as before, for 24-30 fps) */
#define PICS_MAX      8      /* 60 fps; below QMAX, so a packet giving 2 never pushes one out */
#define AUDIO_AHEAD   0.25   /* seconds of sound kept queued (SDL) */
#define SSB_AHEAD     0.5    /* and with SharedSoundBuffer: rides out a busy desktop */
#define SSB_BLOCK     2048   /* sample frames per StreamManager block */
#define READ_BUDGET   64     /* packets read per reelcore_update at most */
#define DECODE_BUDGET 8      /* video packets decoded per reelcore_update at most */
#define VPK_MAX_BYTES (48 << 20)  /* video packets read ahead (for the sound) at most */
#define LATE_SKIP     0.3    /* this far behind: skip decoding non-reference frames, */
#define LATE_KEYS     1.5    /* this far: decode only keyframes, */
#define LATE_OK       0.05   /* until this close again */
#define HB_WAIT_MAX   0.25   /* the HEVC block's picture not done: left this long at most (reelcore_update) */
#define SLIP_LATE     0.1    /* a hardware decoder, no sound: this far behind with nothing in hand, */
#define SLIP_AFTER    0.5    /* for this long: the timer moved back to the next picture (slip_check), */
#define SLIP_AHEAD    0.15   /* and this much more: time to decode some in hand again */
/* Deblocking turned off by itself: when pictures take longer to decode
   than FAST_SLOW of the time between them (or they're LATE_FAST behind
   and take over FAST_BUSY: a hiccup on a video that decodes easily, going
   full screen say, isn't the decoding's fault). On again after FAST_HOLD
   seconds if they take under FAST_EASY (deblocking is about a quarter of
   H.264 decoding: about 0.8 back on, under FAST_SLOW, no see-saw); and
   since how much deblocking costs varies (Reel 0.1.20-autofast1: a 1080p
   trailer full screen kept it off for good), tried on again every
   FAST_PROBE seconds while decoding keeps up, the wait doubling (to
   FAST_PROBE_MAX) each time it had to go straight off again. */
#define FAST_SLOW     0.9
#define FAST_BUSY     0.75
#define FAST_EASY     0.6
#define LATE_FAST     0.1
#define FAST_HOLD     5.0
#define FAST_PROBE    10.0
#define FAST_PROBE_MAX 160.0
#define FAST_REGRET   8.0    /* off again this soon after a try: the try failed */
#define NET_AHEAD     10.0   /* network: seconds read ahead of the picture shown */
#define NET_MAX_BYTES (32 << 20)  /* ... and at most this much */
#define NET_LOW       3.0    /* below this, reelcore_update gives the reader time */

struct Net;

/* something drawn into the picture (the stats panel, a subtitle) */
typedef struct {
    uint8_t *rgba;                     /* premultiplied R,G,B,A, w x h */
    /* the same for blending into Y,Cb,Cr in yuv_c's colours (made when
       wanted): planes of Y*a, Cb*a, Cr*a (w x h each), and 255-a */
    uint16_t *pm;
    uint8_t *ia;
    int w, h, yuv_c;
    int dirty0, dirty1;                /* rows [dirty0, dirty1) to make again (the rest are yuv_c's) */
} Layer;
typedef struct { const Layer *L; int x0, y0; double sx, sy; } Place;

#define SUB_TRACKS  32
#define SUB_MAX     8192               /* subtitle events kept */
#define SUB_BITMAPS 8                  /* ... of them pictures (DVD, Blu-ray) */
typedef struct {
    double start, end;                 /* the picture's time (end: INFINITY = until the next) */
    char *text;                        /* Latin-1, lines apart by \n; or: */
    uint8_t *rgba;                     /* a picture, premultiplied, w x h ... */
    int x, y, w, h, cw, ch;            /* ... at x, y on a cw x ch canvas */
    unsigned id;
} SubEvent;
typedef struct { int stream; char *path; } SubTrack;   /* the file's stream, or a file */

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
    /* sound packets read but not decoded yet: decoded as the sound queue
       needs them. Decoding each as it was read let a video skipping to
       key frames (far behind: a 4K film on a Pi) race through the file
       and push 15 s of sound at StreamManager, which refused it, and the
       sound was given up */
    AVPacket **apk;
    int apk_head, apk_n, apk_cap;
    size_t apk_bytes;
    int aflushed;                      /* the sound decoder has been sent the end */
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
    /* the clock: the system timer, steered by the sound (clock_smooth) */
    int sm_valid;
    double sm_raw, sm_last, sm_step;   /* the last sound reading, what we said, a step's size */
    double sm_base;                    /* the clock at sm_t */
    int64_t sm_t, sm_prev;             /* when sm_base was set; the last look at the sound */
    double sm_err_sum;                 /* |sound - clock| at each step, and how many */
    unsigned sm_err_n;
    /* how evenly the pictures come (reelcore_stats' pace_*) */
    int64_t pace_t;                    /* when the last picture was handed out ... */
    double pace_pts;                   /* ... and its time in the file */
    unsigned pace_seq;                 /* n_shown + dropped then: the next must follow it */
    double pace_sum;
    unsigned pace_n;
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
    int vc;                            /* the video decoded by the Pi's VideoCore (h264_vchiq) */
    int vc_failed;                     /* ... which failed part way: the ARM's decoder from here on */
    /* the chosen streams themselves: the reader thread's av_read_frame can
       add streams (MPEG-TS, HLS), reallocating fmt->streams, while this
       thread looks at them; the AVStreams don't move */
    AVStream *vst, *ast;
    int swr_bad[3];                    /* the sound's rate, format, channels the resampler couldn't take */
    int hb;                            /* the video decoded by the Pi 4's HEVC block (hevc_hwdec) */
    int hb_failed;                     /* ... which failed or refused part way: the ARM's from here on */
    AVFrame *cur_soft;                 /* the current frame as 8-bit YUV, when it's the block's or 10-bit (cur_frame) */
    AVFrame *narrow_spare;             /* the last 10-bit frame's narrowed copy, its memory used again */
    unsigned narrowed;                 /* 10-bit pictures narrowed (cur_frame) */
    int soft_narrowed;                 /* cur_soft is that (not the block's) */
    unsigned hw_draws;                 /* pictures converted straight into the caller's planes */
    unsigned hb_not_done;              /* times a due picture was left: the HEVC block not done with it */
    int hb_wait;                       /* one is being left now (reelcore_idle_time: look again soon) */
    int64_t behind_since;              /* slip_check: behind since (av_gettime_relative), 0 if not */
    int slip;                          /* ... long enough: the timer to be moved back to the next picture */
    unsigned clock_slips;              /* times it was */
    uint8_t *rect_buf;                 /* a layer's rectangle of the block's picture, blended in cached memory */
    unsigned rect_size;
    int vmore;                         /* the video decoder has frames not taken yet (the queue was full) */
    int64_t vc_drop;                   /* h264_vchiq's drop_before last set (INT64_MIN: off) */
    unsigned q_overflow;               /* pictures pushed out of a full queue (should never happen) */
    int auto_fast;                     /* too slow: deblocking off by itself (while fast is off) */
    unsigned auto_fast_spells;
    double auto_fast_since;            /* when turned off (real seconds; for FAST_HOLD) */
    double auto_fast_on_at;            /* when it was last turned on again (-1 never) */
    double auto_fast_probe;            /* seconds off before trying it on again */
    double dec_avg;                    /* moving average: seconds decoding a picture */
    int deint;                         /* REELCORE_DEINT_* */
    AVFilterGraph *dgraph;             /* buffer -> yadif -> buffersink, made when needed */
    AVFilterContext *din, *dout;
    AVFrame *dframe;
    int dg_w, dg_h, dg_fmt;            /* what it was made for */
    int deint_failed;                  /* couldn't be made: pictures go straight through */
    int dg_yadif;                      /* the graph has yadif (it may only turn the picture) */
    int seen_interlaced;               /* AUTO: an interlaced picture has come */
    int rot;                           /* the file says turn the picture: 0, 90, 180, 270 clockwise */
    int stepped;                       /* paused and stepped: the sound needs a seek on playing */
    /* the pictures just before the one shown while stepping, newest last:
       a step back takes one at once instead of decoding from the key frame */
    AVFrame *hist[HIST_N];
    double hist_pts[HIST_N];
    int hist_n;
    int bstep;                         /* a step back's seek: keep the pictures before it */
    int seek_skip;                     /* seeking: non-reference pictures well before it skipped */
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
    unsigned late_skips;               /* times late non-reference pictures went undecoded (check_late) */

    /* conversion */
    struct SwsContext *sws;
    int cs_key[8];
    struct SwsContext *sws_yuv;         /* reelcore_draw_yuv420: other formats to 4:2:0 */
    uint8_t *half[REELCORE_HALVINGS];  /* big reductions: the picture halved, once per level */
    size_t half_size[REELCORE_HALVINGS];
    int halvings;                      /* how many the last conversion did (reelcore_stats) */

    /* a layer's own state (reelcore's textures), released on close */
    void *attach;
    Layer pan;                             /* the stats panel */
    /* what it shows (copied), so it can be made again at another size */
    char *pan_label[REELCORE_PANEL_ROWS], *pan_value[REELCORE_PANEL_ROWS];
    float *pan_graph[REELCORE_PANEL_ROWS];
    unsigned pan_rgb[REELCORE_PANEL_ROWS];
    int pan_rows, pan_graph_n;
    int pan_font;                          /* panel_fonts[] it was made with */
    int pan_lw, pan_made_rows;             /* its label column, and rows, as made */
    uint32_t pan_sum[REELCORE_PANEL_ROWS]; /* each row's text and graph as made (only changed rows made again) */
    double pan_dh, pan_k;                  /* where it was last drawn: display height, pixels a display pixel */
    double yuv_k;                          /* draw_yuv420: frame pixels per display pixel */
    /* subtitles */
    SubTrack sub_tracks[SUB_TRACKS];
    int sub_n, sub_track;                  /* tracks; the one shown (-1 none) */
    int sub_hidden;                        /* chosen but not shown (V) */
    int sub_stream;                        /* its stream in the file (-1: a file of its own, or none) */
    AVCodecContext *sdec;
    /* a picture subtitle file (Blu-ray .sup, VobSub): its packets kept, and
       decoded a little ahead of the picture shown (only SUB_BITMAPS
       pictures are kept, so decoding the whole file at once kept the last
       few only) */
    AVCodecContext *sfdec;
    AVPacket **sfpkt;
    double *sft, sfoff;                /* each packet's time (as the events'), the offset for sub_decoded */
    int sfn, sfnext;                   /* sfnext: the next to decode; -1: found again from the picture shown */
    SubEvent *sev;                         /* what it says when, in order of start */
    int sev_n, sev_cap;
    unsigned sub_ids;
    Layer sub_layer;                       /* what's on screen now ... */
    uint64_t sub_key;                  /* ... made for this (sub_mix of the size and events; 0: none) */
    int sub_shown, sub_bitmap;         /* ... shown?, a picture? */
    int sub_bx, sub_by, sub_cw, sub_ch;    /* a picture: where, on what canvas */
    void (*attach_release)(void *);
};

static char last_error[256];

static void layer_free(Layer *L);
static void pan_forget(ReelCore *v);
static void sub_close_track(ReelCore *v);
static void sub_packet(ReelCore *v, AVPacket *pkt);
static void sub_clear(ReelCore *v, int bitmaps_only);
static void sub_file_free(ReelCore *v);
static void sub_file_feed(ReelCore *v, double pos);
static void sub_setup(ReelCore *v);
static void fill(ReelCore *v);
static void take_frame(ReelCore *v);
static void hist_clear(ReelCore *v);
static void apk_clear(ReelCore *v);

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

/* The clock pictures are shown by: the system timer, steered by the sound.

   The sound is the reference (the sound hardware plays at its own rate,
   and the pictures must keep to it), but the device only says how much it
   has played now and then: StreamManager a whole block (2048 sample
   frames, 46 ms at 44.1 kHz) at a time. Pictures timed by those steps come
   in bursts, and at 30 fps and more some were skipped as late (Reel
   0.1.18: 21-26 of 30 a second on a Pi 4). So the clock is the system
   timer (sm_base + time since sm_t, at the playback speed). Each time the
   sound's reading moves, where the sound really is is taken as that
   reading plus half the time since we last looked (the step came some time
   in between), and the clock moves a quarter of the way towards it: small
   errors (our lateness in looking, the timer and the sound hardware
   running at slightly different rates) are taken out gradually, and the
   pictures stay evenly spaced. More than 100 ms apart (a seek, a hiccup):
   the clock jumps to the sound. It never runs more than one and a half
   steps past the last reading (if the sound stops, so does the clock), and
   never goes backwards. It starts again after a seek, a resume or a change
   of speed. The stall check (clock_now) uses the raw readings. */
#define SM_GAIN    0.25
#define SM_RESYNC  0.1

static double clock_smooth(ReelCore *v, double c, int64_t now)
{
    double pred, cap;
    if (!v->sm_valid) {
        v->sm_valid = 1;
        v->sm_raw = v->sm_base = v->sm_last = c;
        v->sm_t = v->sm_prev = now;
        return c;
    }
    pred = v->sm_base + (now - v->sm_t) / 1e6 * v->speed;
    if (c > v->sm_raw + 0.0005 || c < v->sm_raw - 0.0005) {
        double m = c + (now - v->sm_prev) / 2e6 * v->speed, e;
        if (c > v->sm_raw) {
            double step = c - v->sm_raw;          /* learn the step's size */
            if (step < 0.25)
                v->sm_step = v->sm_step > 0 ? 0.75 * v->sm_step + 0.25 * step : step;
        }
        e = m - pred;
        if (c < v->sm_raw || e > SM_RESYNC || e < -SM_RESYNC) {
            pred = m;                             /* too far out: start from the sound */
            v->sm_last = m;
        } else
            pred += SM_GAIN * e;
        v->sm_err_sum += fabs(e);
        v->sm_err_n++;
        v->sm_base = pred;
        v->sm_t = now;
        v->sm_raw = c;
    }
    v->sm_prev = now;
    cap = v->sm_raw + 1.5 * (v->sm_step > 0.005 ? v->sm_step : 0.05) * (v->speed > 1 ? v->speed : 1);
    if (pred > cap)
        pred = cap;
    if (pred < v->sm_last)
        pred = v->sm_last;
    v->sm_last = pred;
    return pred;
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
            return clock_smooth(v, c, now);
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

static AVCodecContext *open_with_opts(AVStream *st, const AVCodec *codec, AVDictionary **opts)
{
    AVCodecContext *c;
    if (!codec || !(c = avcodec_alloc_context3(codec)))
        return NULL;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    c->pkt_timebase = st->time_base;
    c->thread_count = 1;              /* one core; no point in threads */
    if (avcodec_open2(c, codec, opts) < 0)
        avcodec_free_context(&c);
    return c;
}

static AVCodecContext *open_with(AVStream *st, const AVCodec *codec)
{
    return open_with_opts(st, codec, NULL);
}

static int pics_for(double rate);

static AVCodecContext *open_decoder(AVStream *st)
{
    return open_with(st, avcodec_find_decoder(st->codecpar->codec_id));
}

/* The video's decoder: for H.264, the Pi's VideoCore first (h264_vchiq,
   riscos-reelhwaccel's vcdec), unless REELCORE_NO_VIDEOCORE. It refuses
   streams it can't take (High 10, 4:2:2, over 1920x1088, 1080p with
   gpu_mem under 128 MB, no VCHIQ), and builds without it don't have it:
   then FFmpeg's own decoder on the ARM. For HEVC, the same with the Pi 4's
   HEVC block (hevc_hwdec, riscos-reelhwaccel's hevcdec), unless
   REELCORE_NO_HEVC_BLOCK: it refuses 4:2:2, 4:4:4, 12-bit, over 4096x4096
   and machines without the block. */
static AVCodecContext *open_video_decoder(ReelCore *v, AVStream *st)
{
    AVCodecContext *c = NULL;
    const AVCodec *vc;
    v->vc = 0;
    v->hb = 0;
    if (st->codecpar->codec_id == AV_CODEC_ID_HEVC && !(v->flags & REELCORE_NO_HEVC_BLOCK) && !v->hb_failed &&
        (vc = avcodec_find_decoder_by_name("hevc_hwdec")) != NULL) {
        /* 10-bit comes out as 8-bit (each sample's top 8 bits) in the
           block's one conversion: everything reelcore draws is 8-bit, and
           10-bit then swscale would be two passes */
        AVDictionary *opts = NULL;
        av_dict_set_int(&opts, "output_8bit", 1, 0);
#ifdef REELCORE_HEVCDEC
        /* (devkit 0.2.8) its frames unconverted: each converted once, when
           shown, straight into the overlay (halved for 4K) */
        av_dict_set_int(&opts, "output_hw", 1, 0);
#endif
        c = open_with_opts(st, vc, &opts);
        av_dict_free(&opts);
        if (c) {
            v->hb = 1;
            v->vc_drop = INT64_MIN;
            av_log(NULL, AV_LOG_INFO, "reelcore: HEVC decoded by the Pi 4's HEVC block (hevc_hwdec)\n");
            return c;
        }
        av_log(NULL, AV_LOG_INFO, "reelcore: the HEVC block can't take this HEVC: decoding on the ARM\n");
        return open_decoder(st);
    }
    if (st->codecpar->codec_id == AV_CODEC_ID_H264 && !(v->flags & REELCORE_NO_VIDEOCORE) && !v->vc_failed &&
        (vc = avcodec_find_decoder_by_name("h264_vchiq")) != NULL) {
        /* Its frames are the VideoCore's own picture buffers (devkit 0.2.1,
           zero-copy): enough of them for the pictures reelcore keeps (those
           decoded ahead, the one shown, one coming in; the decoder keeps 3),
           else the extra ones are copies */
        AVRational fr = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        int keep = pics_for(fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 25) + 2;
        AVDictionary *opts = NULL;
        av_dict_set_int(&opts, "out_buffers", 3 + keep > 16 ? 16 : 3 + keep, 0);
        c = open_with_opts(st, vc, &opts);
        av_dict_free(&opts);
        if (c) {
            v->vc = 1;
            v->vc_drop = INT64_MIN;
            av_log(NULL, AV_LOG_INFO, "reelcore: H.264 decoded by the VideoCore (h264_vchiq)\n");
            return c;
        }
        av_log(NULL, AV_LOG_INFO, "reelcore: the VideoCore can't take this H.264: decoding on the ARM\n");
    }
    return open_decoder(st);
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
    double last[3];                    /* seconds: the newest packet of each kind queued (video, sound, subtitles) */
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
    v->vst = v->fmt->streams[v->vs];
    v->ast = v->as >= 0 ? actx(v)->streams[v->as] : NULL;
    return 0;
}

/* After the inputs are open and the streams chosen: the decoders, the
   picture's size and rate, the sound device. In the caller's thread. */
static int setup_decoders(ReelCore *v)
{
    AVStream *st = v->vst;
    if (!(v->vdec = open_video_decoder(v, st))) {
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
    {   /* a phone video: the display matrix says how to turn it */
        const int32_t *dm = (const int32_t *)av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
        if (dm && !(v->flags & REELCORE_NO_ROTATE)) {
            double r = -av_display_rotation_get(dm);
            int d = (int)lrint(r - 360 * floor(r / 360)) % 360;
            v->rot = d == 90 || d == 180 || d == 270 ? d : 0;
            if (v->rot == 90 || v->rot == 270) {
                int t = v->w;
                v->w = v->h;
                v->h = t;
            }
        }
    }
    {
        AVRational fr = av_guess_frame_rate(v->fmt, st, NULL);
        v->fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
    }
    v->duration = v->fmt->duration > 0 ? v->fmt->duration / (double)AV_TIME_BASE :
                  v->afmt && v->afmt->duration > 0 ? v->afmt->duration / (double)AV_TIME_BASE : 0;
    if (v->as >= 0 && (!(v->adec = open_decoder(v->ast)) || open_audio(v) < 0)) {
        avcodec_free_context(&v->adec);
        v->as = -1;                        /* (its packets are dropped as they come) */
        v->ast = NULL;
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
    v->yuv_k = 1;
    sub_setup(v);
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
    n->last[0] = n->last[1] = n->last[2] = -1e9;
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
    if (fc == v->fmt && p->stream_index == v->sub_stream && v->sub_stream >= 0)
        return 2;
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
    v->vst = v->ast = NULL;
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
        n->last[0] = n->last[1] = n->last[2] = -1e9;
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
    av_freep(&v->rect_buf);
    apk_clear(v);
    av_freep(&v->apk);
    av_frame_free(&v->cur);
    cur_changed(v);
    av_frame_free(&v->narrow_spare);
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
    pan_forget(v);
    hist_clear(v);
    sub_close_track(v);
    av_free(v->sev);
    for (int i = 0; i < v->sub_n; i++)
        av_free(v->sub_tracks[i].path);
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
                     "%u late%s, %u skip spells, %u late skips, %u clock slips",
                     reelcore_position(v), c,
                     v->paused ? " (paused)" : v->audio_clock ? " (sound)" : " (timer)", v->qn,
                     v->vpk_n, (unsigned)(v->vpk_bytes >> 10), v->dropped,
                     v->skipping == 2 ? ", keyframes only" : v->skipping ? ", skipping non-reference frames" : "",
                     v->skip_spells, v->late_skips, v->clock_slips);
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

static int hw_frame(const AVFrame *f);

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
    st->pace_sum = v->pace_sum;
    st->pace_n = v->pace_n;
    st->sync_err_sum = v->sm_err_sum;
    st->sync_err_n = v->sm_err_n;
    st->decoded = v->n_decoded;
    st->shown = v->n_shown;
    st->late = v->dropped;
    st->late_skips = v->late_skips;
    st->clock_slips = v->clock_slips;
    st->hb_not_done = v->hb_not_done;
#ifdef REELCORE_HEVCDEC
    if (v->cur && hw_frame(v->cur)) {          /* the block's own: conversions that waited, cleans */
        hevcdec_stats hs;
        hevcdec_get_stats(hevcdec_frame_decoder((const hevcdec_frame *)v->cur->data[3]), &hs);
        st->hb_stats = 1;
        st->hb_convert_waits = hs.convert_waits;
        st->hb_cs_convert_wait = hs.cs_convert_wait;
        st->hb_cache_cleans = hs.cache_cleans;
        st->hb_cs_cache = hs.cs_cache;
    }
    if (v->hb) {                               /* (hevc_hwdec's count, devkit 0.2.11: non-reference pictures skipped) */
        int64_t n = 0;
        if (av_opt_get_int(v->vdec, "skipped", AV_OPT_SEARCH_CHILDREN, &n) >= 0)
            st->hb_skipped = (unsigned)n;
    }
#endif
    st->narrowed = v->narrowed;
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
    st->auto_fast = v->auto_fast && v->fast != REELCORE_FAST_ON;
    st->decoder = v->vc ? REELCORE_DECODER_VIDEOCORE : v->hb ? REELCORE_DECODER_HEVC_BLOCK :
                  v->vc_failed || v->hb_failed ? REELCORE_DECODER_ARM_AFTER : REELCORE_DECODER_ARM;
    st->auto_fast_spells = v->auto_fast_spells;
    st->decode_avg = v->dec_avg;
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
    if (fc->nb_chapters)
        ADD("Chapters\t%u\n", fc->nb_chapters);

    if (v->vs >= 0) {
        const AVStream *st = v->vst;
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
        if (v->rot)
            ADD("Turned\t%s (as the file says)\n", v->rot == 90 ? "90 degrees clockwise" :
                v->rot == 180 ? "upside down" : "90 degrees anticlockwise");
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
        if (v->vc)
            ADD("Decoder\tVideoCore (h264_vchiq)\n");
        else if (v->hb)
            ADD("Decoder\tHEVC block (hevc_hwdec)\n");
        else
            ADD("Decoder\t%s, 1 thread%s\n", v->vdec && v->vdec->codec ? v->vdec->codec->name : "?",
                v->vc_failed ? " (the VideoCore failed part way)" : v->hb_failed ? " (the HEVC block failed part way)" : "");
        if (v->vdec && v->vdec->has_b_frames)
            ADD("Reordering\t%d frame%s (B-frames)\n", v->vdec->has_b_frames, v->vdec->has_b_frames == 1 ? "" : "s");
    }
    if (v->as >= 0) {
        const AVStream *st = v->ast;
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
    if (v->sub_n) {
        ADD("#Subtitles\n");
        for (int i = 0; i < v->sub_n; i++) {
            char name[96];
            reelcore_subtitle_track_name(v, i, name, sizeof(name));
            ADD("Track %d\t%s%s\n", i + 1, name, i == v->sub_track ? " (shown)" : "");
        }
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
    /* one line at a time, put together from pieces; FFmpeg logs from the
       network reader's thread too, so one at a time */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char line[1024];
    static int len;
    char done[1024];
    int n;
    if (level > av_log_get_level())
        return;
    done[0] = 0;
    pthread_mutex_lock(&lock);
    n = vsnprintf(line + len, sizeof(line) - len, fmt, vl);
    if (n >= 0) {
        len += n;
        if (len >= (int)sizeof(line) - 1) {   /* longer than the line (a long address): given as it is, cut */
            len = sizeof(line) - 1;
            line[len - 1] = '\n';
        }
        if (len && line[len - 1] == '\n') {
            line[len - 1] = 0;
            memcpy(done, line, len);
            len = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    if (done[0] && log_fn)
        log_fn(level, done);                   /* (not holding the lock: it may log itself) */
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
    const AVCodecParameters *vp = v->vst->codecpar;
    int n = snprintf(buf, size, "%s %dx%d", avcodec_get_name(vp->codec_id), vp->width, vp->height);
    if (v->fps > 0 && n < size)
        n += snprintf(buf + n, size - n, ", %.3g fps", v->fps);
    if (v->as >= 0 && n < size) {
        const AVCodecParameters *ap = v->ast->codecpar;
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
/* ---- the HEVC block's frames (hevc_hwdec output_hw, devkit 0.2.8) ----
   They stay unconverted (AV_PIX_FMT_HEVCDEC, data[3] the hevcdec_frame)
   until shown: reelcore_draw_yuv420 converts the one shown straight into
   the overlay, halved for 4K. Anything else that wants the pixels (RGB
   drawing, deinterlacing or turning, stepping back) gets a YUV420P copy. */
static int hw_frame(const AVFrame *f)
{
#ifdef REELCORE_HEVCDEC
    return f && f->format == AV_PIX_FMT_HEVCDEC;
#else
    (void)f;
    return 0;
#endif
}

/* the picture's part of a frame: the block's frames keep their left and top
   crop (FFmpeg takes only the right and bottom off a hardware frame) */
static void frame_window(const AVFrame *f, int *x, int *y, int *w, int *h)
{
    int l = hw_frame(f) ? (int)f->crop_left : 0, t = hw_frame(f) ? (int)f->crop_top : 0;
    *x = l;
    *y = t;
    *w = f->width - l;
    *h = f->height - t;
}

/* a new YUV420P frame of the block's frame f (NULL if no memory) */
static AVFrame *hw_soft(const AVFrame *f)
{
#ifdef REELCORE_HEVCDEC
    const hevcdec_frame *hf = (const hevcdec_frame *)f->data[3];
    AVFrame *s = av_frame_alloc();
    int x, y, w, h;
    frame_window(f, &x, &y, &w, &h);
    if (!s)
        return NULL;
    s->format = AV_PIX_FMT_YUV420P;
    s->width = w;
    s->height = h;
    if (av_frame_get_buffer(s, 32) < 0 || av_frame_copy_props(s, f) < 0) {
        av_frame_free(&s);
        return NULL;
    }
    if (hevcdec_frame_to_i420(hevcdec_frame_decoder(hf), hf, s->data, s->linesize, x, y, w, h) != HEVCDEC_OK)
        av_frame_free(&s);                     /* (the picture can't be finished: the decoder stopped) */
    return s;
#else
    (void)f;
    return NULL;
#endif
}

/* 10-bit video decoded on the ARM (HEVC Main 10 without the block, VP9
   profile 2, AV1 10-bit) narrowed to 8 bits once a picture, in NEON, so
   everything after (the overlay copy, halving, NEON RGB) is the 8-bit
   path: swscale did it in C, 8-13 ms a 1080p picture. The same bytes as
   swscale's own 10 to 8-bit copy (planarCopyWrapper, limited range, its
   ordered dither: (s + d) >> 2, d from a 2x2 pattern by row and column).
   vld1.16 needs 2-byte aligned addresses: the planes and pitches are. */
static const uint16_t narrow_dither[2][8] = { { 1, 2, 1, 2, 1, 2, 1, 2 }, { 3, 0, 3, 0, 3, 0, 3, 0 } };

/* One plane: w x h 10-bit samples at src (pitch in samples) to 8 at dst */
void reelcore_narrow10_plane(uint8_t *dst, int dpitch, const uint16_t *src, int spitch, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint16_t *s = src + (ptrdiff_t)y * spitch, *dt = narrow_dither[y & 1];
        uint8_t *d = dst + (ptrdiff_t)y * dpitch;
        int x = 0;
#ifdef REELCORE_NEON
        uint16x8_t dv = vld1q_u16(dt);
        for (; x + 8 <= w; x += 8)
            vst1_u8(d + x, vqshrn_n_u16(vaddq_u16(vld1q_u16(s + x), dv), 2));   /* (s + d) >> 2, at most 255 */
#endif
        for (; x < w; x++) {
            unsigned n = (s[x] + dt[x & 7]) >> 2;
            d[x] = (uint8_t)(n - (n >> 8));
        }
    }
}

static enum AVPixelFormat narrow_to(int format)
{
    return format == AV_PIX_FMT_YUV420P10LE ? AV_PIX_FMT_YUV420P :
           format == AV_PIX_FMT_YUV422P10LE ? AV_PIX_FMT_YUV422P :
           format == AV_PIX_FMT_YUV444P10LE ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NONE;
}

static AVFrame *narrow_frame(ReelCore *v, const AVFrame *f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    AVFrame *n = v->narrow_spare;
    v->narrow_spare = NULL;
    if (n && (n->format != narrow_to(f->format) || n->width != f->width || n->height != f->height ||
              !av_frame_is_writable(n)))
        av_frame_free(&n);                   /* (a different size: made afresh) */
    if (!n) {
        if (!(n = av_frame_alloc()))
            return NULL;
        n->format = narrow_to(f->format);
        n->width = f->width;
        n->height = f->height;
        if (av_frame_get_buffer(n, 0) < 0) {
            av_frame_free(&n);
            return NULL;
        }
    }
    while (n->nb_side_data)                  /* (a reused one: its last picture's go, or they'd pile up) */
        av_frame_remove_side_data(n, n->side_data[0]->type);
    av_dict_free(&n->metadata);
    if (av_frame_copy_props(n, f) < 0) {
        av_frame_free(&n);
        return NULL;
    }
    v->narrowed++;
    for (int p = 0; p < 3; p++)
        reelcore_narrow10_plane(n->data[p], n->linesize[p], (const uint16_t *)f->data[p], f->linesize[p] / 2,
                                p ? AV_CEIL_RSHIFT(f->width, d->log2_chroma_w) : f->width,
                                p ? AV_CEIL_RSHIFT(f->height, d->log2_chroma_h) : f->height);
    return n;
}

/* the current frame with its pixels (the block's converted once, a 10-bit
   one narrowed once, and kept until the frame changes); NULL if none */
static AVFrame *cur_frame(ReelCore *v)
{
    int narrow = v->cur && !hw_frame(v->cur) && narrow_to(v->cur->format) != AV_PIX_FMT_NONE;
    if (!hw_frame(v->cur) && !narrow)
        return v->cur;
    if (!v->cur_soft) {
        v->cur_soft = narrow ? narrow_frame(v, v->cur) : hw_soft(v->cur);
        v->soft_narrowed = narrow && v->cur_soft;
    }
    return v->cur_soft || hw_frame(v->cur) ? v->cur_soft : v->cur;   /* (no memory: as it is) */
}

/* the current frame changed: its copy goes (a narrowed one's memory kept
   for the next: 3 MB a 1080p picture not allocated each time) */
static void cur_changed(ReelCore *v)
{
    if (v->cur_soft && v->soft_narrowed && !v->narrow_spare) {
        v->narrow_spare = v->cur_soft;
        v->cur_soft = NULL;
    }
    av_frame_free(&v->cur_soft);
    v->soft_narrowed = 0;
}

static void hist_push(ReelCore *v, AVFrame *f, double pts)
{
    /* (the block's frames as copies: hevcdec's frames are kept for the
       pictures to come) */
    AVFrame *c = hw_frame(f) ? hw_soft(f) : av_frame_clone(f);
    if (!c)
        return;
    if (v->hist_n == HIST_N) {            /* the oldest goes */
        av_frame_free(&v->hist[0]);
        memmove(v->hist, v->hist + 1, (HIST_N - 1) * sizeof(v->hist[0]));
        memmove(v->hist_pts, v->hist_pts + 1, (HIST_N - 1) * sizeof(v->hist_pts[0]));
        v->hist_n--;
    }
    v->hist[v->hist_n] = c;
    v->hist_pts[v->hist_n++] = pts;
}

static void hist_clear(ReelCore *v)
{
    for (int i = 0; i < v->hist_n; i++)
        av_frame_free(&v->hist[i]);
    v->hist_n = 0;
}

/* The decoder's own skipping (check_late), as it was before a seek */
static void skip_restore(ReelCore *v)
{
    v->seek_skip = 0;
    v->vdec->skip_frame = v->skipping == 2 ? AVDISCARD_NONKEY : v->skipping ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

static void queue_picture(ReelCore *v, AVFrame *f, double pts)
{
    if (v->seek_target >= 0) {
        if (pts < v->seek_target - 0.001) {
            if (v->bstep)
                hist_push(v, f, pts);     /* stepping back: the ones before, for the next steps */
            return;                       /* before the seek point */
        }
        v->seek_target = -1;
        v->bstep = 0;
        if (v->seek_skip)
            skip_restore(v);
    }
    if (v->qn == QMAX) {                  /* full: the oldest goes (receive_all stops short of this) */
        if (!v->q_overflow++)
            av_log(NULL, AV_LOG_WARNING, "reelcore: a picture pushed out of the full queue\n");
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
static int deint_open(ReelCore *v, const AVFrame *f, int yadif)
{
    char args[200];
    AVFilterContext *y = NULL, *last;
    AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : (AVRational){ 1, 1 };
    deint_close(v);
    if (!(v->dgraph = avfilter_graph_alloc()) || !(v->dframe = av_frame_alloc()))
        goto fail;
    v->dgraph->nb_threads = 1;            /* no threads of its own (a Wimp task) */
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=%d/%d",
             f->width, f->height, f->format, sar.num, sar.den);
    if (avfilter_graph_create_filter(&v->din, avfilter_get_by_name("buffer"), "in", args, NULL, v->dgraph) < 0)
        goto fail;
    last = v->din;
    if (yadif) {
        snprintf(args, sizeof(args), "mode=send_frame:parity=auto:deint=%s", v->deint == REELCORE_DEINT_ON ? "all" : "interlaced");
        if (avfilter_graph_create_filter(&y, avfilter_get_by_name("yadif"), "yadif", args, NULL, v->dgraph) < 0 ||
            avfilter_link(last, 0, y, 0) < 0)
            goto fail;
        last = y;
    }
    if (v->rot) {                         /* phone videos: turned as the file says */
        static const char *const names[2] = { "r1", "r2" };
        const char *f1 = v->rot == 180 ? "hflip" : "transpose", *f2 = v->rot == 180 ? "vflip" : NULL;
        const char *a1 = v->rot == 90 ? "dir=clock" : v->rot == 270 ? "dir=cclock" : NULL;
        for (int i = 0; i < 2; i++) {
            AVFilterContext *r = NULL;
            const char *fn = i ? f2 : f1;
            if (!fn)
                break;
            if (avfilter_graph_create_filter(&r, avfilter_get_by_name(fn), names[i], i ? NULL : a1, NULL, v->dgraph) < 0 ||
                avfilter_link(last, 0, r, 0) < 0)
                goto fail;
            last = r;
        }
    }
    if (avfilter_graph_create_filter(&v->dout, avfilter_get_by_name("buffersink"), "out", NULL, NULL, v->dgraph) < 0 ||
        avfilter_link(last, 0, v->dout, 0) < 0 || avfilter_graph_config(v->dgraph, NULL) < 0)
        goto fail;
    v->dg_w = f->width;
    v->dg_h = f->height;
    v->dg_fmt = f->format;
    v->dg_yadif = yadif;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: %s%s%s %dx%d %s\n",
           !yadif ? "" : v->deint == REELCORE_DEINT_ON ? "deinterlacing every picture (yadif)" : "deinterlacing interlaced pictures (yadif)",
           yadif && v->rot ? ", " : "", !v->rot ? "" : v->rot == 90 ? "turned 90 degrees clockwise" :
           v->rot == 180 ? "turned upside down" : "turned 90 degrees anticlockwise", f->width, f->height,
           av_get_pix_fmt_name(f->format));
    return 0;
fail:
    av_log(NULL, AV_LOG_WARNING, "reelcore: can't %s (%s); pictures are shown as they are\n",
           yadif ? "deinterlace" : "turn the picture", av_get_pix_fmt_name(f->format));
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
        if (v->dg_yadif)
            v->n_deint++;
        av_frame_unref(o);
    }
}

/* Gives yadif a picture (it gives back the one before: it needs the next
   to deinterlace). Returns -1 if the picture must be queued as it is. */
static int deint_feed(ReelCore *v, AVFrame *f, double pts, int yadif)
{
    int64_t t0 = av_gettime_relative(), keep_pts = f->pts;
    int ret;
    if ((!v->dgraph || f->width != v->dg_w || f->height != v->dg_h || f->format != v->dg_fmt ||
         yadif != v->dg_yadif) && deint_open(v, f, yadif) < 0)
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
    double pts = frame_pts(f, v->vst,
                           v->qn ? v->qpts[v->qn - 1] + (v->fps > 0 ? 1 / v->fps : 0.04)
                                 : v->cur ? v->cur_pts : 0);
    v->n_decoded++;
    if (f->interlaced_frame)
        v->n_interlaced++;
    if (f->interlaced_frame)
        v->seen_interlaced = 1;
    /* AUTO: yadif from the first interlaced picture on; and the picture
       turned when the file says (both in one graph) */
    {
        int yadif = v->deint == REELCORE_DEINT_ON || (v->deint == REELCORE_DEINT_AUTO && v->seen_interlaced);
        if (!v->deint_failed && (yadif || v->rot)) {
            /* (the block's frames to the filters as copies) */
            AVFrame *soft = hw_frame(f) ? hw_soft(f) : NULL;
            int r = deint_feed(v, soft ? soft : f, pts, yadif);
            av_frame_free(&soft);
            if (r == 0)
                return;
        }
    }
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

/* The resampler made again when the sound changes part way (a DVB
   recording going from 5.1 to stereo at the adverts, or 48 to 44.1 kHz):
   it was set up once, from the decoder at the start, and then read planes
   the frames didn't have, or played them at the wrong speed. 0, or -1
   (the frame can't be played). */
static int swr_follow(ReelCore *v, const AVFrame *f)
{
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, l = { 0 };
    int64_t rate = 0, fmt = -1;
    int same;
    if (f->sample_rate <= 0 || f->ch_layout.nb_channels <= 0)
        return v->swr ? 0 : -1;            /* (nothing to go on: as it is) */
    if (!v->swr && f->sample_rate == v->swr_bad[0] && f->format == v->swr_bad[1] &&
        f->ch_layout.nb_channels == v->swr_bad[2])
        return -1;                         /* (what failed last time: not tried again each frame) */
    if (v->swr) {
        if (av_opt_get_int(v->swr, "in_sample_rate", 0, &rate) < 0 || av_opt_get_int(v->swr, "in_sample_fmt", 0, &fmt) < 0 ||
            av_opt_get_chlayout(v->swr, "in_chlayout", 0, &l) < 0)
            return 0;
        same = rate == f->sample_rate && fmt == f->format && !av_channel_layout_compare(&l, &f->ch_layout);
        av_channel_layout_uninit(&l);
        if (same)
            return 0;
    }
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: the sound changed to %d Hz, %d channels\n", f->sample_rate,
           f->ch_layout.nb_channels);
    if (swr_alloc_set_opts2(&v->swr, &stereo, AV_SAMPLE_FMT_S16, v->rate, &f->ch_layout, f->format, f->sample_rate, 0,
                            NULL) < 0 || swr_init(v->swr) < 0) {
        swr_free(&v->swr);                 /* (none: this frame not played; a different one tries again) */
        v->swr_bad[0] = f->sample_rate;
        v->swr_bad[1] = f->format;
        v->swr_bad[2] = f->ch_layout.nb_channels;
        return -1;
    }
    return 0;
}

static void got_audio(ReelCore *v, AVFrame *f)
{
    int out_max;
    int n, bytes;
    double pts = frame_pts(f, v->ast, v->audio_end >= 0 ? v->audio_end : 0);

    if (v->aseek_target >= 0) {
        if (pts + f->nb_samples / (double)f->sample_rate < v->aseek_target)
            return;                       /* before the seek point */
        v->aseek_target = -1;
    }
    if (v->stalled || swr_follow(v, f) < 0)
        return;
    out_max = swr_get_out_samples(v->swr, f->nb_samples);
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

/* p back at the head (a decoder that couldn't take it yet) */
static void vpk_unpop(ReelCore *v, AVPacket *p)
{
    v->vpk_head = (v->vpk_head + v->vpk_cap - 1) % v->vpk_cap;
    v->vpk[v->vpk_head] = p;
    v->vpk_n++;
    v->vpk_bytes += p->size;
}

static void vpk_clear(ReelCore *v)
{
    AVPacket *p;
    while ((p = vpk_pop(v)) != NULL)
        av_packet_free(&p);
    v->vpk_head = 0;
}

static int apk_push(ReelCore *v, AVPacket *pkt)
{
    AVPacket *p;
    if (v->apk_n == v->apk_cap) {
        int cap = v->apk_cap ? v->apk_cap * 2 : 256;
        AVPacket **q = av_malloc_array(cap, sizeof(*q));
        if (!q)
            return -1;
        for (int i = 0; i < v->apk_n; i++)
            q[i] = v->apk[(v->apk_head + i) % v->apk_cap];
        av_free(v->apk);
        v->apk = q;
        v->apk_cap = cap;
        v->apk_head = 0;
    }
    if (!(p = av_packet_alloc()))
        return -1;
    av_packet_move_ref(p, pkt);
    v->apk[(v->apk_head + v->apk_n) % v->apk_cap] = p;
    v->apk_n++;
    v->apk_bytes += p->size;
    return 0;
}

static AVPacket *apk_pop(ReelCore *v)
{
    AVPacket *p;
    if (!v->apk_n)
        return NULL;
    p = v->apk[v->apk_head];
    v->apk_head = (v->apk_head + 1) % v->apk_cap;
    v->apk_n--;
    v->apk_bytes -= p->size;
    return p;
}

static void apk_clear(ReelCore *v)
{
    AVPacket *p;
    while ((p = apk_pop(v)) != NULL)
        av_packet_free(&p);
    v->apk_head = 0;
}

static int decode(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video);

/* Sound packets decoded while the sound queued is short of v->ahead (all
   of them with no device, or a stalled one); at the end, the decoder's
   flush once they're done */
static void audio_drain(ReelCore *v)
{
    AVPacket *p;
    if (!v->adec)
        return;
    while (v->apk_n && (!v->dev || v->stalled || queued_audio(v) < v->ahead) && (p = apk_pop(v)) != NULL) {
        decode(v, v->adec, p, 0);
        av_packet_free(&p);
    }
    if (v->eof_demux && !v->apk_n && !v->aflushed) {
        v->aflushed = 1;
        decode(v, v->adec, NULL, 0);
    }
}

/* Sends pkt (NULL = flush) to a decoder and takes all it gives back. */
static int decode_frames(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video);

static int decode(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video)
{
    int64_t t0 = av_gettime_relative();
    unsigned n0 = v->n_decoded;
    int taken = decode_frames(v, dec, pkt, video);
    if (video) {
        int64_t t = av_gettime_relative() - t0;
        v->t_decode += t;
        /* the recent time a picture takes (not on the way to a seek's
           picture, nor while frames are skipped: those are cheap) */
        if (pkt && v->n_decoded == n0 + 1 && !v->need_first && v->seek_target < 0 && !v->skipping)
            v->dec_avg = v->dec_avg > 0 ? v->dec_avg * 0.9 + t / 1e6 * 0.1 : t / 1e6;
    } else
        v->t_audio += av_gettime_relative() - t0;
    return taken;
}

/* Takes every frame the decoder has: 1 at the end of the stream, 0 when it
   wants more input, 2 when the pictures waiting are QROOM (the rest are
   left in the decoder, v->vmore: the VideoCore gives them in bursts), or a
   decoder error */
/* The VideoCore's pictures taken at most: those wanted ahead and one
   more, so that what reelcore holds fits the decoder's buffers (see
   open_video_decoder); the rest wait in the decoder. */
static int pics_wanted(const ReelCore *v);
static int vc_room(const ReelCore *v)
{
    int n = pics_wanted(v) + 1;
    return n < QROOM ? n : QROOM;
}

static int receive_all(ReelCore *v, AVCodecContext *dec, int video)
{
    if (video)
        v->vmore = 0;
    for (;;) {
        int ret;
        if (video && v->qn >= (v->vc || v->hb ? vc_room(v) : QROOM)) {
            v->vmore = 1;
            return 2;
        }
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
            return 1;
        }
        if (ret < 0)
            return ret == AVERROR(EAGAIN) ? 0 : ret;
        if (video) got_video(v, v->frame); else got_audio(v, v->frame);
        av_frame_unref(v->frame);
    }
}

/* 1 if the decoder took pkt, 0 if it's to be sent again later (its input
   full and the pictures waiting are QROOM) */
static int decode_frames(ReelCore *v, AVCodecContext *dec, AVPacket *pkt, int video)
{
    int ret, r = 0;
    if (video && v->vmore && (r = receive_all(v, dec, video)) == 2)
        return 0;                          /* still full: nothing sent */
    ret = r ? 0 : avcodec_send_packet(dec, pkt);
    /* A decoder whose input is full (the VideoCore's) refuses the packet
       for now: take its frames, then the packet again (the ARM's decoders
       always take it) */
    for (int tries = 0; !r && ret == AVERROR(EAGAIN) && tries < 16; tries++) {
        if ((r = receive_all(v, dec, video)) != 0)
            break;
        ret = avcodec_send_packet(dec, pkt);
    }
    if (r == 2 && ret == AVERROR(EAGAIN))
        return 0;                          /* kept for later: the queue is full */
    if (!r && (ret >= 0 || ret == AVERROR_EOF || ret == AVERROR(EAGAIN)))
        r = receive_all(v, dec, video);
    else if (!r && ret < 0)
        r = ret;                           /* (FFmpeg 5.1 runs a hardware decoder's receive_frame inside
                                              send_packet, and its error comes back from there) */
    if (r < 0 && video && v->vc && r == AVERROR_EXTERNAL && !v->vc_failed) {
        v->vc_failed = 1;                  /* switched at the next update: see hw_fallback */
        av_log(NULL, AV_LOG_WARNING, "reelcore: the VideoCore's decoder failed: decoding on the ARM from here\n");
    }
    /* hevc_hwdec: a raw stream it can't take is refused at its first
       picture (ENOSYS), and so is a change of depth part way */
    if (r < 0 && video && v->hb && (r == AVERROR_EXTERNAL || r == AVERROR(ENOSYS)) && !v->hb_failed) {
        v->hb_failed = 1;
        av_log(NULL, AV_LOG_WARNING, "reelcore: the HEVC block %s: decoding on the ARM from here\n",
               r == AVERROR(ENOSYS) ? "can't take this HEVC" : "failed");
    }
    return 1;
}

/* Behind the clock: stop decoding the frames nothing else refers to (most
   B-frames) until caught up; far behind, decode only keyframes. Those
   frames would only be dropped as late anyway, and the sound (the clock)
   doesn't wait. */
static void set_deblock(ReelCore *v);

/* Pictures decoding too slowly for the frame rate: deblocking off by
   itself before frames are skipped (4K, 4:4:4: about a quarter of the
   time); on again once decoding has plenty to spare. */
static void check_slow(ReelCore *v, double lag)
{
    double gap, now, off_for;
    if (v->fast == REELCORE_FAST_ON || (v->flags & REELCORE_NO_AUTOFAST) || v->paused)
        return;
    gap = 1.0 / ((v->fps > 0 ? v->fps : 25) * v->speed);
    now = av_gettime_relative() / 1e6;   /* (real time: the clock goes back at a loop or seek) */
    if (v->auto_fast_probe <= 0)
        v->auto_fast_probe = FAST_PROBE;
    if (!v->auto_fast) {
        if (v->dec_avg > gap * FAST_SLOW || (lag > LATE_FAST && v->dec_avg > gap * FAST_BUSY)) {
            if (v->auto_fast_spells && now - v->auto_fast_on_at < FAST_REGRET)
                v->auto_fast_probe = FFMIN(v->auto_fast_probe * 2, FAST_PROBE_MAX);   /* the try failed */
            v->auto_fast = 1;
            v->auto_fast_spells++;
            v->auto_fast_since = now;
            av_log(NULL, AV_LOG_VERBOSE, "reelcore: decoding too slowly (%.1f ms a picture of %.1f, %.2f s behind): "
                   "deblocking off\n", v->dec_avg * 1000, gap * 1000, lag);
            v->dec_avg *= 0.8;             /* (what it should come down to) */
            set_deblock(v);
        }
        return;
    }
    off_for = now - v->auto_fast_since;
    if (v->skipping || lag > LATE_OK || v->dec_avg <= 0)
        return;
    if ((v->dec_avg < gap * FAST_EASY && off_for > FAST_HOLD) ||
        (v->dec_avg < gap * FAST_SLOW && off_for > v->auto_fast_probe)) {
        int easy = v->dec_avg < gap * FAST_EASY;
        if (easy)
            v->auto_fast_probe = FAST_PROBE;
        v->auto_fast = 0;
        v->auto_fast_on_at = now;
        set_deblock(v);
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: %s (%.1f ms a picture of %.1f): deblocking on\n",
               easy ? "decoding keeps up" : "trying", v->dec_avg * 1000, gap * 1000);
    }
}

/* The VideoCore decodes every frame whatever skip_frame says; the cost to
   the ARM is each picture's copy out of its memory, so the pictures that
   would only be skipped as late (or that come before a seek's) are given
   back to it uncopied: h264_vchiq's drop_before. hevc_hwdec has the same
   (devkit 0.2.7): its block decodes 4K far faster than the ARM converts,
   so only skip_frame nonref is used for it (hb_skip), never nonkey (which
   left it keyframes only). */
static void vc_drop(ReelCore *v)
{
    double before = -1;
    int64_t want = INT64_MIN;
    AVRational tb = v->vst->time_base;
    if (v->seek_target >= 0 && !v->bstep)
        before = v->seek_target - 0.001;     /* (queue_picture throws those away) */
    else if (!v->paused && v->cur && !v->need_first && v->seek_target < 0 && !v->slip)
        before = clock_now(v) - 2.0 / (v->fps > 0 ? v->fps * (v->speed > 1 ? v->speed : 1) : 25);
    if (before > 0 && tb.num > 0)
        want = (int64_t)floor(before / av_q2d(tb));
    if (want != v->vc_drop && av_opt_set_int(v->vdec, "drop_before", want, AV_OPT_SEARCH_CHILDREN) >= 0)
        v->vc_drop = want;
}

/* A hardware decoder (h264_vchiq, hevc_hwdec) decodes every picture, so
   skipping can't make up time: one that only just keeps up (the HEVC block
   with 4K 10-bit at 60 fps) never caught up once behind. Pictures came out
   a few tenths late, nearly all given back unconverted (drop_before) and
   the rest shown: 4K 10-bit with the stats panel, 9 a second for good
   after a moment's hold-up. With no sound to keep to, behind by SLIP_LATE
   with nothing in hand for SLIP_AFTER, the timer is moved back instead:
   nothing more is given back, and the next picture out is shown on time
   (a pause, as the sound-less pictures were held up anyway). With sound,
   the decoder must skip: hevc_hwdec (devkit 0.2.11) leaves the block the
   non-reference pictures while behind (hb_skip), which catches up first
   where a stream has them. */
static void slip_check(ReelCore *v, double now)
{
    int64_t t = av_gettime_relative();
    double last = v->qn ? v->qpts[v->qn - 1] : v->cur_pts;
    if (!(v->vc || v->hb) || v->audio_clock || v->paused || v->need_first || v->seek_target >= 0 || v->bstep ||
        !v->cur || now - last < SLIP_LATE) {
        v->behind_since = 0;
        v->slip = 0;
        return;
    }
    if (!v->behind_since)
        v->behind_since = t;
    if (!v->slip && t - v->behind_since >= (int64_t)(SLIP_AFTER * 1e6)) {
        v->slip = 1;
        vc_drop(v);                        /* (nothing more given back) */
    }
    if (v->slip && v->qn) {
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: %.2f s behind with nothing in hand: the clock moved back\n", now - last);
        timer_set(v, v->qpts[0] - SLIP_AHEAD);
        v->clock_slips++;
        v->slip = 0;
        v->behind_since = 0;
        vc_drop(v);
    }
}

/* The HEVC block decodes every picture it's given, so pictures given back
   unconverted (drop_before) don't make up time, and one that only just
   keeps up (4K 10-bit at 60 fps) stayed behind. hevc_hwdec passes
   skip_frame on (devkit 0.2.11): at AVDISCARD_NONREF the pictures nothing
   refers to never reach the block (a quarter to a third of most streams),
   so it gets ahead. Set while the last picture decoded is due more than
   two pictures ago, cleared once two pictures ahead again. NONKEY isn't
   used: 4K was left keyframes only. */
static void hb_skip(ReelCore *v)
{
    double gap = 2.0 / (v->fps > 0 ? v->fps * (v->speed > 1 ? v->speed : 1) : 25);
    int want = 0;
    if (!v->paused && v->cur && !v->need_first && v->seek_target < 0) {
        double last = v->qn ? v->qpts[v->qn - 1] : v->cur_pts, lag = clock_now(v) - last;
        want = lag > gap ? 1 : lag < -gap ? 0 : v->skipping;
    }
    if (want == v->skipping)
        return;
    if (want)
        v->skip_spells++;
    v->skipping = want;
    v->vdec->skip_frame = want ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: the HEVC block %s\n",
           want ? "behind: not given non-reference pictures" : "caught up: given every picture");
}

static void check_late(ReelCore *v, const AVPacket *p)
{
    static const char *what[3] = { "decoding every frame", "skipping non-reference frames",
                                   "decoding only keyframes" };
    double lag, last;
    int want;
    if (v->vc || v->hb) {
        vc_drop(v);                        /* (the hardware decodes every picture: late ones go unconverted) */
        if (v->hb)
            hb_skip(v);
        return;
    }
    if (v->paused || !v->cur || v->need_first) {
        if (!v->skipping && v->seek_target < 0)
            v->vdec->skip_frame = AVDISCARD_DEFAULT;     /* (not left skipping late ones: steps, decoding ahead) */
        return;
    }
    last = v->qn ? v->qpts[v->qn - 1] : v->cur_pts;
    lag = clock_now(v) - last;
    check_slow(v, lag);
    want = lag > LATE_KEYS ? 2 : lag > LATE_SKIP ? (v->skipping > 1 ? 2 : 1) : lag < LATE_OK ? 0 : v->skipping;
    /* VP9 and AV1 decoded again from a frame after skipped ones are spoilt
       until the next keyframe (each frame's probabilities, motion vectors,
       follow from the last's, not only its pictures): only start again at
       a keyframe. (H.264 and HEVC smudge for a moment; better than waiting.) */
    if (want < 2 && v->skipping == 2 && !(p->flags & AV_PKT_FLAG_KEY) &&
        (v->vdec->codec_id == AV_CODEC_ID_VP9 || v->vdec->codec_id == AV_CODEC_ID_AV1))
        want = 2;
    if (want != v->skipping) {
        if (want > v->skipping)
            v->skip_spells++;
        v->skipping = want;
        v->vdec->skip_frame = want == 2 ? AVDISCARD_NONKEY : want ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: %.2f s behind: %s\n", lag, what[want]);
    }
    /* A picture already late (due more than two pictures ago, as vc_drop)
       would only be thrown away when it came out: while nothing else is
       being skipped, a non-reference one isn't decoded at all (4-8 ms of a
       720p picture, 10-20 of 1080p); the reference ones must be, for the
       pictures after them. (VP9 can't skip just those: it doesn't try.) */
    if (!v->skipping && v->seek_target < 0 && v->vdec->codec_id != AV_CODEC_ID_VP9) {
        enum AVDiscard d = AVDISCARD_DEFAULT;
        if (p->pts != AV_NOPTS_VALUE) {
            double gap = 2.0 / (v->fps > 0 ? v->fps * (v->speed > 1 ? v->speed : 1) : 25);
            if (p->pts * av_q2d(v->vst->time_base) < clock_now(v) - gap)
                d = AVDISCARD_NONREF;
        }
        if (d != AVDISCARD_DEFAULT && v->vdec->skip_frame != d)
            v->late_skips++;
        v->vdec->skip_frame = d;
    }
}

/* Reads until there's a little sound queued and a video packet to decode,
   then decodes pictures until a few are ready. */
/* How many pictures to keep decoded ahead: 0.13 s of them, 3 to 8. With
   only 3, a 60 fps video had 50 ms in hand, and one slow picture (or the
   desktop busy for a moment) made the next one late though decoding
   averaged twice real time. */
static int pics_for(double rate)
{
    int n = (int)ceil(PICS_AHEAD * rate);
    return n < PICS_MIN ? PICS_MIN : n > PICS_MAX ? PICS_MAX : n;
}

static int pics_wanted(const ReelCore *v)
{
    return pics_for(v->fps > 0 ? v->fps * (v->speed > 1 ? v->speed : 1) : 25);
}

static void fill(ReelCore *v)
{
    for (int budget = READ_BUDGET; budget > 0 && !v->eof_demux; budget--) {
        int need_a = v->dev && !v->stalled && queued_audio(v) < v->ahead && v->apk_n == 0;
        int need_v = v->qn < pics_wanted(v) && v->vpk_n == 0;
        int ret, kind;
        if (!need_a && !need_v)
            break;
        if (!need_v && v->vpk_bytes > VPK_MAX_BYTES)
            break;                         /* a strange file: don't eat all the memory */
        ret = next_packet(v, v->pkt, &kind);
        if (ret == AVERROR(EAGAIN))
            break;                         /* (the network: not here yet) */
        if (ret < 0) {                     /* the end: the sound decoder flushed after its */
            v->eof_demux = 1;              /* packets, the video's once its packets are done */
            if (!v->adec) v->eof_audio = 1;
            break;
        }
        if (kind == 0)
            vpk_push(v, v->pkt);
        else if (kind == 1 && v->adec) {
            apk_push(v, v->pkt);
            audio_drain(v);
        } else if (kind == 2)
            sub_packet(v, v->pkt);
        av_packet_unref(v->pkt);
    }
    audio_drain(v);
    /* after a seek, decode on to the seek point in one go (as before) */
    int budget = v->need_first ? READ_BUDGET : DECODE_BUDGET;
    for (int n = 0; n < budget && v->qn < pics_wanted(v); n++) {
        if (!v->vpk_n && !v->eof_demux && v->need_first) {
            /* more packets on the way to the seek point */
            int kind, ret = next_packet(v, v->pkt, &kind);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                v->eof_demux = 1;
                if (!v->adec) v->eof_audio = 1;
                audio_drain(v);
            } else if (ret >= 0) {
                if (kind == 0)
                    vpk_push(v, v->pkt);
                else if (kind == 1 && v->adec) {
                    apk_push(v, v->pkt);
                    audio_drain(v);
                }
                else if (kind == 2)
                    sub_packet(v, v->pkt);
                av_packet_unref(v->pkt);
                continue;
            }
        }
        /* a picture due now is shown first: decoding several in a row (to
           fill up after a slow one) would make it late, and the next ... */
        if (v->qn && !v->need_first && !v->paused && v->qpts[0] <= clock_now(v) + 0.005)
            break;
        if (v->vmore && !v->vpk_n && v->vflushed) {
            /* the end sent: the decoder's last frames, as there's room */
            decode(v, v->vdec, NULL, 1);
            if (v->vmore)
                break;
            continue;
        }
        AVPacket *p = vpk_pop(v);
        if (p) {
            check_late(v, p);
            if (v->seek_target >= 0 && p->pts != AV_NOPTS_VALUE) {
                /* on the way to a seek's picture: pictures nothing else is
                   predicted from, and more than half a second before it,
                   needn't be decoded at all (B-frames: about half) */
                double t = p->pts * av_q2d(v->vst->time_base);
                enum AVDiscard want = t < v->seek_target - 0.5 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
                if (v->skipping == 0 && v->vdec->skip_frame != want) {
                    v->vdec->skip_frame = want;
                    v->seek_skip = 1;
                }
            }
            if (!decode(v, v->vdec, p, 1)) {
                vpk_unpop(v, p);           /* the decoder full and so is the queue: later */
                break;
            }
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

/* How evenly pictures are handed out: for each that follows the one
   before (none skipped between), the difference between the real time
   since that one and the time between them in the file. */
static void pace_note(ReelCore *v)
{
    int64_t now = av_gettime_relative();
    unsigned seq = v->n_shown + v->dropped;
    if (v->pace_t && seq == v->pace_seq + 1 && v->cur_pts > v->pace_pts) {
        double want = (v->cur_pts - v->pace_pts) / v->speed, got = (now - v->pace_t) / 1e6;
        if (want < 0.5) {
            v->pace_sum += fabs(got - want);
            v->pace_n++;
        }
    }
    v->pace_t = now;
    v->pace_pts = v->cur_pts;
    v->pace_seq = seq;
}

static void take_frame(ReelCore *v)
{
    av_frame_free(&v->cur);
    cur_changed(v);
    v->cur = v->q[0];
    v->cur_pts = v->qpts[0];
    if (v->sfdec)
        sub_file_feed(v, v->cur_pts);         /* (a picture subtitle file: the ones coming up) */
    memmove(v->q, v->q + 1, (v->qn - 1) * sizeof(v->q[0]));
    memmove(v->qpts, v->qpts + 1, (v->qn - 1) * sizeof(v->qpts[0]));
    v->qn--;
}

/* The VideoCore or the HEVC block failed part way: FFmpeg's decoder on the
   ARM instead, from where the picture is (a seek there: it flushes and
   refills) */
static void hw_fallback(ReelCore *v)
{
    AVCodecContext *c = open_decoder(v->vst);
    double at = reelcore_position(v);
    const char *who = v->vc ? "the VideoCore" : "the HEVC block";
    if (!c) {
        av_log(NULL, AV_LOG_ERROR, "reelcore: no ARM decoder to take over from %s\n", who);
        v->vc = v->hb = 0;
        return;
    }
    avcodec_free_context(&v->vdec);
    v->vdec = c;
    v->vmore = 0;
    v->vc = v->hb = 0;
    set_deblock(v);                        /* (the fast setting, for the new decoder) */
    av_log(NULL, AV_LOG_INFO, "reelcore: %s on the ARM from %.2f s\n", c->codec ? c->codec->name : "?", at);
    reelcore_seek(v, at);
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
    if ((v->vc && v->vc_failed) || (v->hb && v->hb_failed))
        hw_fallback(v);
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
    slip_check(v, now);
    now = clock_now(v);
    if (v->qn && v->qpts[0] <= now + 0.005) {
        /* skip the frames that are already late: those whose next is due
           more than a picture's time ago. A picture that took longer than
           that to decode (at 60 fps, anything over 16.7 ms) leaves two due
           at once; both are shown, one straight after the other, rather
           than one being thrown away. Falling further behind still skips. */
        double tol = v->fps > 0 ? 1 / (v->fps * (v->speed > 1 ? v->speed : 1)) : 0.04;
        if (tol > 0.04)
            tol = 0.04;
        while (v->qn > 1 && v->qpts[1] <= now - tol) {
            take_frame(v);
            v->dropped++;
        }
#ifdef REELCORE_HEVCDEC
        /* The HEVC block's picture not decoded yet: converting it now would
           wait for it (its latency, both of the block's phases one after
           the other: 17 ms and more at 4K 10-bit), with nothing else given
           to the block meanwhile. Behind, that's every picture, and it
           never caught up (4K 10-bit with the stats panel: 14 a second).
           So leave it, keep feeding the block (fill), and look again: the
           one on screen stays a moment; if it's too late, the next one
           due takes its place (above). At most HB_WAIT_MAX, then wait. */
        if (v->hb && hw_frame(v->q[0]) && now - v->qpts[0] < HB_WAIT_MAX) {
            const hevcdec_frame *hf = (const hevcdec_frame *)v->q[0]->data[3];
            if (!hevcdec_frame_done(hevcdec_frame_decoder(hf), hf)) {
                v->hb_not_done++;
                v->hb_wait = 1;
                goto end_check;
            }
        }
        v->hb_wait = 0;
#endif
        take_frame(v);
        pace_note(v);
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
    /* a picture due, left for the HEVC block to finish: looked at again at
       once. Reel's sleeps are whole centiseconds, and while it's asleep
       nothing is given to the block: 10 ms each time took 4K 10-bit with
       the stats panel from 12-18 pictures a second to 9 (0.1.25-opt6) */
    if (v->hb_wait && v->qn)
        return 0;
    /* pictures still to decode */
    if (v->qn < pics_wanted(v) && (v->vpk_n || !v->eof_demux || !v->vflushed || v->vmore))
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
        v->sm_valid = 0;                    /* and the smoothed clock starts again */
        v->pace_t = 0;
        if (!v->audio_clock || v->audio_end < 0)
            timer_set(v, v->pause_pos);
        if (v->dev)
            aud_pause(v, 0);
        if (v->stepped) {                   /* stepped while paused: the sound from here */
            v->stepped = 0;
            reelcore_seek(v, reelcore_position(v));
        }
        hist_clear(v);
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
    v->vmore = 0;
    if (v->vc || v->hb)
        vc_drop(v);                        /* (the pictures before the seek's) */
    if (v->adec)
        avcodec_flush_buffers(v->adec);
    if (v->sdec) {
        avcodec_flush_buffers(v->sdec);
        sub_clear(v, 1);                  /* pictures come again; text is kept (and not doubled) */
    }
    if (v->sfdec) {
        sub_clear(v, 1);                  /* (a picture subtitle file: decoded again from the new place) */
        v->sfnext = -1;
    }
    if (v->dev && !v->stalled)
        aud_clear(v);
    if (v->swr)
        swr_init(v->swr);                 /* drop what it buffered */
    if (v->tempo)
        tempo_open(v);                    /* and atempo */
    clear_queue(v);
    vpk_clear(v);
    apk_clear(v);
    v->aflushed = 0;
    deint_close(v);                       /* the pictures it held are from before */
    v->vflushed = 0;
    v->eof_demux = v->eof_video = v->eof_audio = 0;
    v->audio_end = -1;
    v->audio_clock = v->dev != 0 && !v->stalled;
    v->stall_since = 0;
    v->sm_valid = 0;
    v->pace_t = 0;
    v->seek_target = v->aseek_target = seconds > 0 ? ts / (double)AV_TIME_BASE : -1;
    v->need_first = 1;
    if (!v->bstep)
        hist_clear(v);
    if (v->seek_skip && v->seek_target < 0)
        skip_restore(v);
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
    AVFrame *f = cur_frame(v);
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

/* ------------------------------------------ layers drawn into the picture */

/* A layer is premultiplied R,G,B,A (and, made when first wanted, the same
   as Y,Cb,Cr,A in the frame's colours for reelcore_draw_yuv420). It is
   drawn into the picture rather than over it, so it shows through a
   hardware overlay too, which covers anything drawn on the screen. Two:
   the stats panel and the subtitle. Each draw places it: its top left
   corner (x0, y0) and scale (sx, sy: destination pixels per layer pixel),
   nearest pixel. */

static void layer_free(Layer *L)
{
    av_freep(&L->rgba);
    av_freep(&L->pm);
    av_freep(&L->ia);
    L->w = L->h = 0;
}

static int layer_alloc(Layer *L, int w, int h)
{
    if (w != L->w || h != L->h || !L->rgba) {
        av_freep(&L->rgba);
        av_freep(&L->pm);
        av_freep(&L->ia);
        if (!(L->rgba = av_malloc((size_t)w * h * 4))) {
            L->w = L->h = 0;
            return AVERROR(ENOMEM);
        }
        L->w = w;
        L->h = h;
    }
    memset(L->rgba, 0, (size_t)w * h * 4);
    L->yuv_c = -1;                             /* made again when next wanted */
    return 0;
}

/* m / 255, exactly, for m 0..65025 (a product of two bytes, or a blend of them) */
#define DIV255(m) (((unsigned)(m) + 1 + ((unsigned)(m) >> 8)) >> 8)

/* src over the layer's pixel, alpha a (0-255), colour r,g,b */
static void layer_put(Layer *L, int x, int y, int r, int g, int b, int a)
{
    uint8_t *d;
    if (x < 0 || y < 0 || x >= L->w || y >= L->h || a <= 0)
        return;
    d = L->rgba + ((size_t)y * L->w + x) * 4;
    if (a >= 255) {
        d[0] = (uint8_t)r; d[1] = (uint8_t)g; d[2] = (uint8_t)b; d[3] = 255;
        return;
    }
    d[0] = (uint8_t)DIV255(r * a + d[0] * (255 - a));
    d[1] = (uint8_t)DIV255(g * a + d[1] * (255 - a));
    d[2] = (uint8_t)DIV255(b * a + d[2] * (255 - a));
    d[3] = (uint8_t)(a + DIV255(d[3] * (255 - a)));
}

/* Rows y0..y1 of the layer cleared to see-through black of alpha a */
static void layer_clear_rows(Layer *L, int y0, int y1, int a)
{
    uint8_t px[4] = { 0, 0, 0, (uint8_t)a };
    uint32_t v;
    uint32_t *d;
    memcpy(&v, px, 4);
    y0 = FFMAX(y0, 0);
    y1 = FFMIN(y1, L->h);
    if (y0 >= y1)
        return;
    d = (uint32_t *)(void *)(L->rgba + (size_t)y0 * L->w * 4);
    for (size_t i = 0, n = (size_t)(y1 - y0) * L->w; i < n; i++)
        d[i] = v;
}

static void layer_box(Layer *L, int x, int y, int w, int h, int r, int g, int b, int a)
{
    for (int j = 0; j < h; j++)
        for (int k = 0; k < w; k++)
            layer_put(L, x + k, y + j, r, g, b, a);
}

/* 16.16 fixed point: layer pixels per destination pixel */
static int layer_step(double s) { return s > 0 ? (int)(65536 / s) : 65536; }

/* Over 32bpp pixels (R,G,B,x, or B,G,R,x with bgr), w x h */
static void layer_blend_rgb(const Place *pl, uint8_t *p, int pitch, int w, int h, int bgr)
{
    const Layer *L = pl->L;
    int x0 = pl->x0, y0 = pl->y0, ix = layer_step(pl->sx), iy = layer_step(pl->sy);
    int xa = FFMAX(0, x0), ya = FFMAX(0, y0);
    int xb = FFMIN(w, x0 + (int)(L->w * pl->sx + 0.5)), yb = FFMIN(h, y0 + (int)(L->h * pl->sy + 0.5));
    for (int y = ya; y < yb; y++) {
        int ly = (int)(((int64_t)(y - y0) * iy) >> 16);
        const uint8_t *row;
        uint8_t *d = p + (size_t)y * pitch + (size_t)xa * 4;
        if (ly >= L->h)
            break;
        row = L->rgba + (size_t)ly * L->w * 4;
        for (int x = xa; x < xb; x++, d += 4) {
            int lx = (int)(((int64_t)(x - x0) * ix) >> 16);
            const uint8_t *s;
            int a;
            if (lx >= L->w)
                break;
            s = row + lx * 4;
            if (!s[3])
                continue;
            a = 255 - s[3];
            d[0] = (uint8_t)(s[bgr ? 2 : 0] + DIV255(d[0] * a));
            d[1] = (uint8_t)(s[1] + DIV255(d[1] * a));
            d[2] = (uint8_t)(s[bgr ? 0 : 2] + DIV255(d[2] * a));
        }
    }
}

/* The layer ready to blend into Y,Cb,Cr in the frame's colours (c:
   REELCORE_YUV_709/_FULL): only the rows changed since it was last made
   (the stats panel: the rows whose values changed, once a second). Fixed
   point (the colours to 1/65536, divides by a from a table): in doubles
   with three divides a pixel it was a hitch on the picture that drew it. */
static int layer_yuv(Layer *L, int c)
{
    static int32_t rc[256];                    /* 65536 / a, rounded */
    double kr = c & REELCORE_YUV_709 ? 0.2126 : 0.299, kb = c & REELCORE_YUV_709 ? 0.0722 : 0.114;
    double ys = c & REELCORE_YUV_FULL ? 255 : 219, cs = c & REELCORE_YUV_FULL ? 255 : 224;
    int64_t KR = (int64_t)(kr * 65536 + 0.5), KB = (int64_t)(kb * 65536 + 0.5), KG = 65536 - KR - KB;
    int64_t YS = (int64_t)(ys * 65536 + 0.5), CBS = (int64_t)(cs / (2 * (1 - kb)) * 65536 + 0.5);
    int64_t CRS = (int64_t)(cs / (2 * (1 - kr)) * 65536 + 0.5), YO = c & REELCORE_YUV_FULL ? 0 : 16;
    size_t n = (size_t)L->w * L->h;
    int y0 = L->dirty0, y1 = L->dirty1;
    if (L->pm && L->yuv_c == c && y0 >= y1)
        return 0;
    if (!L->pm || L->yuv_c != c) {             /* all of it */
        y0 = 0;
        y1 = L->h;
    }
    if (!rc[1])
        for (int a = 1; a < 256; a++)
            rc[a] = (65536 + a / 2) / a;
    if (!L->pm && (!(L->pm = av_malloc(n * 3 * sizeof(*L->pm))) || !(L->ia = av_malloc(n)))) {
        av_freep(&L->pm);
        return AVERROR(ENOMEM);
    }
    for (size_t i = (size_t)y0 * L->w; i < (size_t)y1 * L->w; i++) {
        const uint8_t *s = L->rgba + i * 4;
        int a = s[3], Y, U, V;
        int64_t r = (int64_t)s[0] * rc[a], g = (int64_t)s[1] * rc[a], b = (int64_t)s[2] * rc[a];   /* 0..65536 (a 0: 0) */
        int64_t yy = (KR * r + KG * g + KB * b + 32768) >> 16;
        Y = av_clip((int)(YO + ((yy * YS + ((int64_t)1 << 31)) >> 32)), 0, 255);
        U = av_clip((int)(128 + (((b - yy) * CBS + ((int64_t)1 << 31)) >> 32)), 0, 255);
        V = av_clip((int)(128 + (((r - yy) * CRS + ((int64_t)1 << 31)) >> 32)), 0, 255);
        L->pm[i] = (uint16_t)(Y * a);
        L->pm[n + i] = (uint16_t)(U * a);
        L->pm[2 * n + i] = (uint16_t)(V * a);
        L->ia[i] = (uint8_t)(255 - a);
    }
    L->yuv_c = c;
    L->dirty0 = L->dirty1 = 0;
    return 0;
}

/* d[i] = (pm[k*i] + d[i] * ia[k*i]) / 255 for i < n, k 1 or 2 (every
   other layer pixel: a chroma row). Not static: panel_test checks it. */
void reelcore_blend_run(uint8_t *d, const uint16_t *pm, const uint8_t *ia, int n, int k);
void reelcore_blend_run(uint8_t *d, const uint16_t *pm, const uint8_t *ia, int n, int k)
{
    int i = 0;
#ifdef REELCORE_NEON
    for (; i + 8 + (k - 1) <= n; i += 8) {      /* (k 2: vld2's last load stays in the run) */
        uint8x8_t a = k == 1 ? vld1_u8(ia + i) : vld2_u8(ia + 2 * i).val[0];
        uint16x8_t m = k == 1 ? vld1q_u16(pm + i) : vld2q_u16(pm + 2 * i).val[0];
        m = vmlal_u8(m, vld1_u8(d + i), a);                  /* <= 65025 */
        m = vsraq_n_u16(vaddq_u16(m, vdupq_n_u16(1)), m, 8); /* m + 1 + (m >> 8) */
        vst1_u8(d + i, vshrn_n_u16(m, 8));
    }
#endif
    for (; i < n; i++)
        d[i] = (uint8_t)DIV255(pm[k * i] + d[i] * ia[k * i]);
}

/* Does the layer touch row y of plane p (0 Y, 1 Cb, 2 Cr) of a 4:2:0 picture? */
static int layer_touches(const Place *pl, int p, int y)
{
    int sub = p ? 2 : 1, fy = y * sub;
    return fy + sub > pl->y0 && fy < pl->y0 + pl->L->h * pl->sy;
}

/* Into row y of plane p, w pixels wide (that plane's pixels) */
static void layer_blend_row(const Place *pl, uint8_t *row, int p, int y, int w)
{
    const Layer *L = pl->L;
    int sub = p ? 2 : 1, ix = layer_step(pl->sx), iy = layer_step(pl->sy);
    int fy = y * sub, ly, xa, xb;
    const uint16_t *pm;
    if (fy < pl->y0)
        fy = pl->y0;                           /* (a chroma row half in: its lower half) */
    ly = (int)(((int64_t)(fy - pl->y0) * iy) >> 16);
    if (ly < 0 || ly >= L->h)
        return;
    xa = FFMAX(0, (pl->x0 + sub - 1) / sub);
    xb = FFMIN(w, (pl->x0 + (int)(L->w * pl->sx + 0.5)) / sub);
    pm = L->pm + (size_t)p * L->w * L->h;
    if (ix == 65536) {                         /* 1:1 (the panel): a run, 8 at a time */
        int lx = xa * sub - pl->x0, n = FFMIN(xb - xa, (L->w - lx + sub - 1) / sub);
        if (lx >= 0 && n > 0)
            reelcore_blend_run(row + xa, pm + (size_t)ly * L->w + lx, L->ia + (size_t)ly * L->w + lx, n, sub);
        return;
    }
    for (int x = xa; x < xb; x++) {
        int lx = (int)(((int64_t)(x * sub - pl->x0) * ix) >> 16);
        size_t s;
        if (lx < 0)
            continue;
        if (lx >= L->w)
            break;
        s = (size_t)ly * L->w + lx;
        row[x] = (uint8_t)DIV255(pm[s] + row[x] * L->ia[s]);   /* (a 0: row[x] as it was) */
    }
}

/* ---------------------------------------------------------- the stats panel */

#include "panel_font.h"

/* Sizes at 15 px (panel_fonts[] has others: made at each font's size) */
#define PAN_PAD     8
#define PAN_GAP     12
#define PAN_LEAD    3                       /* between rows */
#define PAN_GRAPH_W 200
#define PAN_GRAPH_H 14
#define PAN_BG      150                     /* the background's alpha: see-through black */
#define PAN_MARGIN  10                      /* from the picture's top left corner */
#define PAN_SIZE    15                      /* the font's size (px, as seen) up to ... */
#define PAN_SIZE_H  900                     /* ... a picture this high on screen, then bigger with it */
#define PAN_SIZE_MAX 30

static int pan_px(const PanelFont *f, int n) { return (n * f->size + PAN_SIZE / 2) / PAN_SIZE; }

static int pan_textw(const PanelFont *f, const char *t) { return t ? (int)strlen(t) * f->w : 0; }

static void pan_text(Layer *L, const PanelFont *f, int x, int y, const char *t, int r, int g, int b)
{
    for (; t && *t; t++, x += f->w) {
        unsigned c = (unsigned char)*t;
        int i = c >= 32 && c < 127 ? (int)c - 32 : c >= 160 ? (int)c - 160 + 95 : '?' - 32;
        const unsigned char *gl = f->data + (size_t)i * f->w * f->h;
        for (int j = 0; j < f->h; j++)
            for (int k = 0; k < f->w; k++)
                layer_put(L, x + k, y + j, r, g, b, gl[j * f->w + k]);
    }
}

/* The panel's font for a picture shown dh display pixels high, k frame
   pixels a display pixel: drawn at the size it's seen, not drawn at one
   size and scaled (a 720p picture full screen on 1920x1200 through the
   overlay: k 0.67, and the 15 px text, pixels left out, was unreadable) */
static int pan_font_for(double dh, double k)
{
    double want = PAN_SIZE * (dh > PAN_SIZE_H ? FFMIN(dh / PAN_SIZE_H, (double)PAN_SIZE_MAX / PAN_SIZE) : 1) * k;
    int n = 0;
    for (int i = 0; i < (int)FF_ARRAY_ELEMS(panel_fonts); i++)
        if (panel_fonts[i].size <= want * 1.1)
            n = i;
    return n;
}

static uint32_t pan_hash(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;            /* FNV-1a */
    return h;
}

/* A row's text and graph, summed (only rows whose sums change are made again) */
static uint32_t pan_row_sum(const ReelCore *v, int i)
{
    uint32_t h = 2166136261u;
    if (v->pan_label[i]) h = pan_hash(h, v->pan_label[i], strlen(v->pan_label[i]) + 1);
    h = pan_hash(h, "|", 1);
    if (v->pan_value[i]) h = pan_hash(h, v->pan_value[i], strlen(v->pan_value[i]) + 1);
    if (v->pan_graph[i]) {
        h = pan_hash(h, v->pan_graph[i], sizeof(float) * v->pan_graph_n);
        h = pan_hash(h, &v->pan_rgb[i], sizeof(v->pan_rgb[i]));
    }
    return h;
}

/* The panel made at font: all of it, or (the same font, rows and label
   column, and no wider) only the rows that changed. It's made again once a
   second, and making every pixel of it was a hitch; it doesn't get
   narrower while it's shown, so a value a letter shorter doesn't make all
   of it again. */
static void pan_build(ReelCore *v, int font)
{
    const PanelFont *f = &panel_fonts[font];
    Layer *L = &v->pan;
    int lw = 0, vw = 0, w, h, rows = v->pan_rows, all;
    int pad = pan_px(f, PAN_PAD), gap = pan_px(f, PAN_GAP), row = f->h + pan_px(f, PAN_LEAD);
    int gw = pan_px(f, PAN_GRAPH_W), gh = FFMAX(pan_px(f, PAN_GRAPH_H), 2);
    uint32_t sum[REELCORE_PANEL_ROWS];
    if (rows <= 0) {
        layer_free(L);
        v->pan_made_rows = 0;
        return;
    }
    for (int i = 0; i < rows; i++) {
        int g = v->pan_graph[i] ? gw + gap : 0;
        lw = FFMAX(lw, pan_textw(f, v->pan_label[i]));
        vw = FFMAX(vw, g + pan_textw(f, v->pan_value[i]));
        sum[i] = pan_row_sum(v, i);
    }
    w = pad + lw + gap + vw + pad;
    h = pad + rows * row + pad - pan_px(f, PAN_LEAD);
    all = !L->rgba || font != v->pan_font || rows != v->pan_made_rows || lw != v->pan_lw || h != L->h || w > L->w;
    v->pan_font = font;
    if (all) {
        if (layer_alloc(L, w, h) < 0) {
            v->pan_made_rows = 0;
            return;
        }
        v->pan_lw = lw;
        v->pan_made_rows = rows;
    }
    for (int i = 0; i < rows; i++) {
        int y = pad + i * row, x = pad + lw + gap;
        int b0 = i ? y : 0, b1 = i + 1 < rows ? y + row : h;   /* the row's band */
        if (!all && sum[i] == v->pan_sum[i])
            continue;
        v->pan_sum[i] = sum[i];
        layer_clear_rows(L, b0, b1, PAN_BG);
        if (!all) {                            /* (just these rows into Y,Cb,Cr again) */
            if (L->dirty0 >= L->dirty1) {
                L->dirty0 = b0;
                L->dirty1 = b1;
            }
            L->dirty0 = FFMIN(L->dirty0, b0);
            L->dirty1 = FFMAX(L->dirty1, b1);
        }
        if (v->pan_label[i])
            pan_text(L, f, pad + lw - pan_textw(f, v->pan_label[i]), y, v->pan_label[i], 255, 255, 255);
        if (v->pan_graph[i]) {
            int gy = y + (f->h - gh) / 2, n = v->pan_graph_n;
            int r = v->pan_rgb[i] >> 16 & 255, g = v->pan_rgb[i] >> 8 & 255, b = v->pan_rgb[i] & 255;
            layer_box(L, x, gy, gw, gh, 40, 40, 40, 255);
            for (int k = 0; k < gw; k++) {
                float s = v->pan_graph[i][k * n / gw];
                int bh = (int)(av_clipf(s, 0, 1) * gh + 0.5f);
                layer_box(L, x + k, gy + gh - bh, 1, bh, r, g, b, 255);
            }
            x += gw + gap;
        }
        if (v->pan_value[i])
            pan_text(L, f, x, y, v->pan_value[i], 230, 230, 230);
    }
}

static void pan_forget_rows(ReelCore *v)
{
    for (int i = 0; i < REELCORE_PANEL_ROWS; i++) {
        av_freep(&v->pan_label[i]);
        av_freep(&v->pan_value[i]);
        av_freep(&v->pan_graph[i]);
    }
    v->pan_rows = 0;
}

static void pan_forget(ReelCore *v)
{
    pan_forget_rows(v);
    layer_free(&v->pan);
    v->pan_made_rows = 0;
}

int reelcore_set_panel(ReelCore *v, const ReelCorePanel *p)
{
    int rows = p ? FFMIN(p->rows, REELCORE_PANEL_ROWS) : 0;
    if (rows <= 0) {
        pan_forget(v);
        return 0;
    }
    pan_forget_rows(v);                        /* (the picture kept: only changed rows made again) */
    if (p->yuv_scale > 0)
        v->yuv_k = p->yuv_scale;
    v->pan_graph_n = p->graph_n;
    for (int i = 0; i < rows; i++) {
        if ((p->label[i] && !(v->pan_label[i] = av_strdup(p->label[i]))) ||
            (p->value[i] && !(v->pan_value[i] = av_strdup(p->value[i]))))
            goto nomem;
        if (p->graph[i] && p->graph_n > 0) {
            if (!(v->pan_graph[i] = av_memdup(p->graph[i], sizeof(float) * p->graph_n)))
                goto nomem;
            v->pan_rgb[i] = p->graph_rgb[i];
        }
    }
    v->pan_rows = rows;
    /* made now at the size last drawn (reelcore_panel_size), again when drawn at another */
    pan_build(v, pan_font_for(v->pan_dh > 0 ? v->pan_dh : 0, v->pan_k > 0 ? v->pan_k : 1));
    return v->pan.rgba ? 0 : AVERROR(ENOMEM);
nomem:
    pan_forget(v);
    return AVERROR(ENOMEM);
}

void reelcore_panel_size(const ReelCore *v, int *w, int *h)
{
    *w = v->pan.rgba ? v->pan.w : 0;
    *h = v->pan.rgba ? v->pan.h : 0;
}

void reelcore_set_yuv_scale(ReelCore *v, double k)
{
    v->yuv_k = k > 0 ? k : 1;
}

/* -------------------------------------------------------------- subtitles */

static void sub_render(ReelCore *v, int dw, int dh);

/* Where the layers go in a w x h destination whose picture is shown
   k display pixels a destination pixel... (k: destination pixels per
   display pixel: 1 when drawn at the display's size, reelcore_set_yuv_scale's
   for an overlay). Returns how many. */
static int layers_place(ReelCore *v, Place *pl, int w, int h, double k)
{
    int n = 0;
    if (v->pan_rows > 0) {                     /* drawn at the size it's seen */
        int font = pan_font_for(h / k, k);
        v->pan_dh = h / k;
        v->pan_k = k;
        if (font != v->pan_font || !v->pan.rgba)
            pan_build(v, font);
        if (v->pan.rgba) {
            int m = pan_px(&panel_fonts[font], PAN_MARGIN);
            pl[n++] = (Place){ &v->pan, m, m, 1, 1 };
        }
    }
    sub_render(v, w, h);                       /* text: at the frame's own size */
    if (v->sub_layer.rgba && v->sub_shown) {
        if (v->sub_bitmap) {                   /* where the subtitle's own canvas puts it */
            double sx = (double)w / v->sub_cw, sy = (double)h / v->sub_ch;
            pl[n] = (Place){ &v->sub_layer, (int)(v->sub_bx * sx), (int)(v->sub_by * sy), sx, sy };
        } else                                 /* text: centred, near the bottom */
            pl[n] = (Place){ &v->sub_layer, (w - v->sub_layer.w) / 2, h - v->sub_layer.h - (int)(h * 0.04), 1, 1 };
        n++;
    }
    return n;
}

#include "sub_font.h"

/* UTF-8 (as FFmpeg's text subtitle decoders give) to Latin-1, with ASS's
   override blocks ({\i1} ...) dropped and \N, \n, \h made plain */
static void sub_plain(char *out, size_t size, const char *in)
{
    size_t n = 0;
    while (*in && n + 4 < size) {
        unsigned c = (unsigned char)*in;
        if (c == '{' && strchr(in, '}')) {
            in = strchr(in, '}') + 1;
            continue;
        }
        if (c == '\\' && (in[1] == 'N' || in[1] == 'n')) {
            out[n++] = '\n';
            in += 2;
            continue;
        }
        if (c == '\\' && in[1] == 'h') {
            out[n++] = ' ';
            in += 2;
            continue;
        }
        if (c == '\r') {
            in++;
            continue;
        }
        if (c < 0x80) {
            out[n++] = (char)c;
            in++;
            continue;
        }
        {
            unsigned u = 0;
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            u = len == 1 ? c : c & (0x3F >> (len - 1));
            for (int i = 1; i < len; i++) {
                if ((in[i] & 0xC0) != 0x80) {
                    len = i;
                    break;
                }
                u = u << 6 | (in[i] & 0x3F);
            }
            in += len;
            if (u >= 0xA0 && u <= 0xFF)
                out[n++] = (char)u;
            else if (u == 0x2018 || u == 0x2019 || u == 0x2032)
                out[n++] = '\'';
            else if (u == 0x201C || u == 0x201D || u == 0x2033)
                out[n++] = '"';
            else if (u == 0x2013 || u == 0x2014 || u == 0x2012)
                out[n++] = '-';
            else if (u == 0x2026) {
                out[n++] = '.'; out[n++] = '.'; out[n++] = '.';
            } else if (u == 0x266A || u == 0x266B)
                out[n++] = '#';
            else if (u >= 0x80)
                out[n++] = '?';
        }
    }
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
        out[--n] = 0;
}

static void sub_event_free(SubEvent *e)
{
    av_freep(&e->text);
    av_freep(&e->rgba);
}

static void sub_clear(ReelCore *v, int bitmaps_only)
{
    int j = 0;
    for (int i = 0; i < v->sev_n; i++) {
        if (!bitmaps_only || v->sev[i].rgba)
            sub_event_free(&v->sev[i]);
        else
            v->sev[j++] = v->sev[i];
    }
    v->sev_n = j;
    v->sub_key = 0;
}

/* Keeps an event (taking its text or bitmap), in order of start */
static void sub_add(ReelCore *v, SubEvent *e)
{
    int i;
    for (i = 0; i < v->sev_n; i++) {
        SubEvent *o = &v->sev[i];
        if (fabs(o->start - e->start) < 0.001 && e->text && o->text && !strcmp(o->text, e->text)) {
            sub_event_free(e);                /* already have it (read again after a seek) */
            return;
        }
    }
    if (e->rgba) {                            /* a picture subtitle ends the one before */
        for (i = 0; i < v->sev_n; i++)
            if (v->sev[i].rgba && v->sev[i].start < e->start && v->sev[i].end > e->start)
                v->sev[i].end = e->start;
    }
    if (v->sev_n == v->sev_cap) {
        int drop = e->rgba ? -1 : 0;
        if (v->sev_cap < SUB_MAX) {
            SubEvent *n = av_realloc_array(v->sev, v->sev_cap ? v->sev_cap * 2 : 64, sizeof(*n));
            if (n) {
                v->sev = n;
                v->sev_cap = v->sev_cap ? v->sev_cap * 2 : 64;
                drop = -2;
            }
        }
        if (drop != -2 && !v->sev_n) {         /* (no memory for the first: it isn't kept) */
            sub_event_free(e);
            return;
        }
        if (drop != -2) {                      /* full: the oldest goes */
            sub_event_free(&v->sev[0]);
            memmove(v->sev, v->sev + 1, (v->sev_n - 1) * sizeof(*v->sev));
            v->sev_n--;
        }
    }
    {
        int bitmaps = 0;                      /* only a few pictures are kept */
        for (i = 0; i < v->sev_n; i++)
            bitmaps += v->sev[i].rgba != NULL;
        if (e->rgba && bitmaps >= SUB_BITMAPS)
            for (i = 0; i < v->sev_n; i++)
                if (v->sev[i].rgba) {
                    sub_event_free(&v->sev[i]);
                    memmove(v->sev + i, v->sev + i + 1, (v->sev_n - i - 1) * sizeof(*v->sev));
                    v->sev_n--;
                    break;
                }
    }
    for (i = v->sev_n; i > 0 && v->sev[i - 1].start > e->start; i--)
        ;
    memmove(v->sev + i + 1, v->sev + i, (v->sev_n - i) * sizeof(*v->sev));
    e->id = ++v->sub_ids;
    v->sev[i] = *e;
    v->sev_n++;
    v->sub_key = 0;
}

/* A decoded AVSubtitle into events; offset: added to its times (an
   external file's times are from 0; the video's may not be) */
static void sub_decoded(ReelCore *v, AVSubtitle *sub, double offset, int cw, int ch)
{
    double base = sub->pts != AV_NOPTS_VALUE ? sub->pts / (double)AV_TIME_BASE + offset : 0;
    double start = base + sub->start_display_time / 1000.0;
    double end = sub->end_display_time && sub->end_display_time != UINT32_MAX ?
                 base + sub->end_display_time / 1000.0 : INFINITY;
    int bx0 = INT_MAX, by0 = INT_MAX, bx1 = 0, by1 = 0;
    char text[1024] = "";
    size_t tn = 0;
    for (unsigned r = 0; r < sub->num_rects; r++) {
        AVSubtitleRect *rc = sub->rects[r];
        if (rc->type == SUBTITLE_BITMAP && rc->w > 0 && rc->h > 0) {
            bx0 = FFMIN(bx0, rc->x); by0 = FFMIN(by0, rc->y);
            bx1 = FFMAX(bx1, rc->x + rc->w); by1 = FFMAX(by1, rc->y + rc->h);
        } else if (rc->type == SUBTITLE_ASS && rc->ass) {
            const char *t = rc->ass;
            for (int commas = 0; *t && commas < 8; t++)   /* ReadOrder,Layer,Style,Name,MarginL,R,V,Effect, */
                if (*t == ',')
                    commas++;
            if (tn && tn + 1 < sizeof(text))
                text[tn++] = '\n';
            sub_plain(text + tn, sizeof(text) - tn, t);
            tn = strlen(text);
        } else if (rc->type == SUBTITLE_TEXT && rc->text) {
            if (tn && tn + 1 < sizeof(text))
                text[tn++] = '\n';
            sub_plain(text + tn, sizeof(text) - tn, rc->text);
            tn = strlen(text);
        }
    }
    if (!(bx1 > bx0 && by1 > by0) && !tn) {   /* nothing (no rectangles, or empty ones: FFmpeg's PGS
                                                 clear): ends the picture before */
        for (int i = 0; i < v->sev_n; i++)
            if (v->sev[i].rgba && v->sev[i].start <= start && v->sev[i].end > start)
                v->sev[i].end = start;
        v->sub_key = 0;
        return;
    }
    if (bx1 > bx0 && by1 > by0) {
        SubEvent e = { 0 };
        int w = bx1 - bx0, h = by1 - by0;
        e.start = start;
        e.end = end;
        e.x = bx0; e.y = by0; e.w = w; e.h = h;
        e.cw = cw > 0 ? cw : FFMAX(bx1, v->cur ? v->cur->width : 720);
        e.ch = ch > 0 ? ch : FFMAX(by1, v->cur ? v->cur->height : 576);
        if (!(e.rgba = av_mallocz((size_t)w * h * 4)))
            return;
        for (unsigned r = 0; r < sub->num_rects; r++) {
            AVSubtitleRect *rc = sub->rects[r];
            const uint32_t *pal = (const uint32_t *)rc->data[1];
            if (rc->type != SUBTITLE_BITMAP || !pal)
                continue;
            for (int y = 0; y < rc->h; y++)
                for (int x = 0; x < rc->w; x++) {
                    uint32_t c = pal[rc->data[0][y * rc->linesize[0] + x]];   /* 0xAARRGGBB */
                    unsigned a = c >> 24;
                    uint8_t *d = e.rgba + ((size_t)(rc->y - by0 + y) * w + (rc->x - bx0 + x)) * 4;
                    d[0] = (uint8_t)((c >> 16 & 255) * a / 255);
                    d[1] = (uint8_t)((c >> 8 & 255) * a / 255);
                    d[2] = (uint8_t)((c & 255) * a / 255);
                    d[3] = (uint8_t)a;
                }
        }
        sub_add(v, &e);
    }
    if (tn) {
        SubEvent e = { 0 };
        e.start = start;
        e.end = end;
        if (!(e.text = av_strdup(text)))
            return;
        sub_add(v, &e);
    }
}

/* A packet of the chosen subtitle stream, as it's read */
static void sub_packet(ReelCore *v, AVPacket *pkt)
{
    AVSubtitle sub;
    int got = 0;
    if (!v->sdec || pkt->stream_index != v->sub_stream)
        return;
    if (avcodec_decode_subtitle2(v->sdec, &sub, &got, pkt) >= 0 && got) {
        sub_decoded(v, &sub, 0, v->sdec->width, v->sdec->height);
        avsubtitle_free(&sub);
    }
}

/* The events on screen at t: a text one may run until the next starts */
static double sub_end(const ReelCore *v, int i)
{
    const SubEvent *e = &v->sev[i];
    if (isfinite(e->end))
        return e->end;
    for (int j = i + 1; j < v->sev_n; j++)
        if (v->sev[j].start > e->start && !v->sev[j].rgba == !e->rgba)
            return FFMIN(v->sev[j].start, e->start + 10);
    return e->start + 10;
}

/* The glyph for Latin-1 character c */
static const SubGlyph *sub_glyph(const SubFont *f, unsigned c)
{
    int i = c >= 32 && c < 127 ? (int)c - 32 : c >= 160 ? (int)c - 160 + 95 : '?' - 32;
    return &f->g[i];
}

static int sub_textw(const SubFont *f, const char *t, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w += sub_glyph(f, (unsigned char)t[i])->adv;
    return w;
}

/* Text into the subtitle layer: white with a black outline, each line
   centred, long lines wrapped at spaces to fit dw */
static void sub_render_text(ReelCore *v, const char *text, int dw, int dh)
{
    const SubFont *f = &sub_fonts[0];
    int maxw, lines = 0, starts[64], lens[64], widest = 0, b, w, h, r;
    uint8_t *cov, *dil;
    double want = dh * 0.052;                 /* about 5% of the picture's height */
    for (unsigned i = 0; i < sizeof(sub_fonts) / sizeof(sub_fonts[0]); i++)
        if (sub_fonts[i].size <= want * 1.15)
            f = &sub_fonts[i];
    r = FFMAX(2, f->size / 12);                /* the outline */
    b = r + 1;
    maxw = FFMAX(dw * 92 / 100 - 2 * b, f->size * 4);
    for (const char *p = text; *p && lines < 64;) {  /* lines, wrapped */
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        while (n > 0 && lines < 64) {
            int take = n;
            if (sub_textw(f, p, n) > maxw) {
                int sp = -1, w0 = 0;
                for (int i = 0; i < n; i++) {
                    w0 += sub_glyph(f, (unsigned char)p[i])->adv;
                    if (w0 > maxw)
                        break;
                    if (p[i] == ' ')
                        sp = i;
                }
                take = sp > 0 ? sp : FFMAX(1, n / 2);
            }
            starts[lines] = (int)(p - text);
            lens[lines] = take;
            widest = FFMAX(widest, sub_textw(f, p, take));
            lines++;
            p += take;
            n -= take;
            while (n > 0 && *p == ' ') {
                p++;
                n--;
            }
        }
        p = nl ? nl + 1 : p + strlen(p);
    }
    if (!lines || !widest) {
        layer_free(&v->sub_layer);
        return;
    }
    w = widest + 2 * b;
    h = lines * f->line + 2 * b;
    cov = av_mallocz((size_t)w * h);
    dil = av_mallocz((size_t)w * h);
    if (!cov || !dil || layer_alloc(&v->sub_layer, w, h) < 0) {
        av_free(cov);
        av_free(dil);
        layer_free(&v->sub_layer);
        return;
    }
    for (int l = 0; l < lines; l++) {
        const char *t = text + starts[l];
        int x = b + (widest - sub_textw(f, t, lens[l])) / 2, y = b + l * f->line;
        for (int i = 0; i < lens[l]; i++) {
            const SubGlyph *g = sub_glyph(f, (unsigned char)t[i]);
            const unsigned char *d = f->data + g->off;
            for (int j = 0; j < g->h; j++)
                for (int k = 0; k < g->w; k++) {
                    int px = x + g->x + k, py = y + g->y + j;
                    if (px >= 0 && py >= 0 && px < w && py < h && d[j * g->w + k] > cov[py * w + px])
                        cov[py * w + px] = d[j * g->w + k];
                }
            x += g->adv;
        }
    }
    for (int y = 0; y < h; y++)                /* the outline: coverage spread by r, round */
        for (int x = 0; x < w; x++) {
            int m = 0;
            for (int dy = -r; dy <= r && m < 255; dy++) {
                int yy = y + dy;
                if (yy < 0 || yy >= h)
                    continue;
                for (int dx = -r; dx <= r; dx++) {
                    int xx = x + dx;
                    if (xx >= 0 && xx < w && dx * dx + dy * dy <= r * r + r && cov[yy * w + xx] > m)
                        m = cov[yy * w + xx];
                }
            }
            dil[y * w + x] = (uint8_t)m;
        }
    for (int i = 0; i < w * h; i++) {          /* white over black: premultiplied */
        uint8_t *d = v->sub_layer.rgba + (size_t)i * 4;
        d[0] = d[1] = d[2] = cov[i];
        d[3] = FFMAX(dil[i], cov[i]);
    }
    av_free(cov);
    av_free(dil);
}

/* The subtitle layer for the current picture's time, made again when what's
   on screen (or the picture's size, for text) changes */
static uint64_t sub_mix(uint64_t h, int x)
{
    for (int b = 0; b < 4; b++, x >>= 8)
        h = (h ^ (uint8_t)x) * 1099511628211ull;
    return h;
}

static void sub_render(ReelCore *v, int dw, int dh)
{
    char text[2048];
    size_t tn = 0;
    uint64_t key = 14695981039346656037ull;   /* FNV-1a of the size and the events' ids */
    int bitmap = -1;
    double t;
    if (v->cur && v->sfdec)
        sub_file_feed(v, v->cur_pts);
    if (v->sub_track < 0 || v->sub_hidden || !v->cur || !v->sev_n) {
        v->sub_shown = 0;
        return;
    }
    t = v->cur_pts;
    key = sub_mix(sub_mix(key, dw), dh);
    text[0] = 0;
    for (int i = 0; i < v->sev_n && v->sev[i].start <= t + 0.001; i++) {
        if (t >= sub_end(v, i))
            continue;
        key = sub_mix(key, v->sev[i].id);
        if (v->sev[i].rgba)
            bitmap = i;                        /* the latest picture */
        else if (v->sev[i].text && tn + strlen(v->sev[i].text) + 2 < sizeof(text)) {
            if (tn)
                text[tn++] = '\n';
            strcpy(text + tn, v->sev[i].text);
            tn += strlen(v->sev[i].text);
        }
    }
    if (bitmap < 0 && !tn) {
        v->sub_shown = 0;
        return;
    }
    v->sub_shown = 1;
    key |= 1;                                  /* (never 0: none) */
    if (key == v->sub_key)
        return;
    v->sub_key = key;
    if (bitmap >= 0) {
        const SubEvent *e = &v->sev[bitmap];
        if (layer_alloc(&v->sub_layer, e->w, e->h) == 0)
            memcpy(v->sub_layer.rgba, e->rgba, (size_t)e->w * e->h * 4);
        v->sub_bitmap = 1;
        v->sub_bx = e->x; v->sub_by = e->y; v->sub_cw = e->cw; v->sub_ch = e->ch;
    } else {
        v->sub_bitmap = 0;
        sub_render_text(v, text, dw, dh);
    }
}

/* ---- subtitle tracks: the file's streams, then files added ---- */

static void sub_close_track(ReelCore *v)
{
    if (v->sub_stream >= 0 && v->fmt && v->sub_stream < (int)v->fmt->nb_streams)
        v->fmt->streams[v->sub_stream]->discard = AVDISCARD_ALL;
    v->sub_stream = -1;
    avcodec_free_context(&v->sdec);
    sub_file_free(v);
    sub_clear(v, 0);
    layer_free(&v->sub_layer);
    v->sub_shown = 0;
}

static void sub_file_free(ReelCore *v)
{
    for (int i = 0; i < v->sfn; i++)
        av_packet_free(&v->sfpkt[i]);
    av_freep(&v->sfpkt);
    av_freep(&v->sft);
    v->sfn = 0;
    v->sfnext = -1;
    avcodec_free_context(&v->sfdec);
}

/* A picture subtitle file's packets up to 2 s ahead of pos decoded into
   events. From the start of the one shown 10 s before pos after a seek, or
   when the picture went back (stepped back) past what was decoded: Blu-ray
   and DVD subtitles carry everything they need from such a point. */
#define SUBF_AHEAD 2.0
#define SUBF_BACK 10.0
static void sub_file_feed(ReelCore *v, double pos)
{
    if (!v->sfdec || !v->sfn)
        return;
    if ((v->sfnext > 0 && v->sft[v->sfnext - 1] > pos + SUBF_AHEAD + SUBF_BACK) ||
        (v->sfnext >= 0 && v->sfnext < v->sfn && v->sft[v->sfnext] < pos - SUBF_BACK))
        v->sfnext = -1;                        /* back before what was decoded, or far past it (a seek) */
    if (v->sfnext < 0) {
        int lo = 0, hi = v->sfn;               /* the first at or after pos - SUBF_BACK */
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (v->sft[mid] < pos - SUBF_BACK) lo = mid + 1; else hi = mid;
        }
        avcodec_flush_buffers(v->sfdec);
        sub_clear(v, 1);
        v->sfnext = lo;
    }
    while (v->sfnext < v->sfn && v->sft[v->sfnext] <= pos + SUBF_AHEAD) {
        AVSubtitle sub;
        int got = 0;
        if (avcodec_decode_subtitle2(v->sfdec, &sub, &got, v->sfpkt[v->sfnext]) >= 0 && got) {
            sub_decoded(v, &sub, v->sfoff, v->sfdec->width, v->sfdec->height);
            avsubtitle_free(&sub);
        }
        v->sfnext++;
    }
}

/* Reads a whole subtitle file into events (a picture one: its packets) */
static int sub_read_file(ReelCore *v, const char *path)
{
    AVFormatContext *fc = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = av_packet_alloc();
    double offset = v->fmt && v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time / (double)AV_TIME_BASE : 0;
    int s, ret, n = 0;
    if (!pkt)
        return AVERROR(ENOMEM);
    if ((ret = avformat_open_input(&fc, path, NULL, NULL)) < 0 ||
        (ret = avformat_find_stream_info(fc, NULL)) < 0 ||
        (ret = s = av_find_best_stream(fc, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0)) < 0 ||
        !(dec = open_decoder(fc->streams[s]))) {
        av_log(NULL, AV_LOG_WARNING, "reelcore: can't read subtitles from %s\n", path);
        av_packet_free(&pkt);
        avformat_close_input(&fc);
        return ret < 0 ? ret : AVERROR_DECODER_NOT_FOUND;
    }
    if (dec->codec_descriptor && (dec->codec_descriptor->props & AV_CODEC_PROP_BITMAP_SUB)) {
        AVRational tb = fc->streams[s]->time_base;
        double last = 0;
        int cap = 0;
        while (av_read_frame(fc, pkt) >= 0) {
            if (pkt->stream_index == s && pkt->size > 0) {
                if (v->sfn == cap) {                   /* (twice as many each time) */
                    int nc = cap ? cap * 2 : 256;
                    AVPacket **np = av_realloc_array(v->sfpkt, nc, sizeof(*np));
                    double *nt = np ? av_realloc_array(v->sft, nc, sizeof(*nt)) : NULL;
                    if (np)
                        v->sfpkt = np;
                    if (nt) {
                        v->sft = nt;
                        cap = nc;
                    }
                }
                if (v->sfn == cap || !(v->sfpkt[v->sfn] = av_packet_clone(pkt))) {
                    av_packet_unref(pkt);
                    break;                     /* (no memory: the subtitles read so far) */
                }
                if (pkt->pts != AV_NOPTS_VALUE)
                    last = pkt->pts * av_q2d(tb) + offset;
                v->sft[v->sfn++] = last;
            }
            av_packet_unref(pkt);
        }
        av_log(NULL, AV_LOG_VERBOSE, "reelcore: %d picture subtitle packets from %s\n", v->sfn, path);
        av_packet_free(&pkt);
        avformat_close_input(&fc);
        v->sfdec = dec;                        /* decoded as the picture comes to them (sub_file_feed) */
        v->sfoff = offset;
        v->sfnext = -1;
        return 0;
    }
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == s) {
            AVSubtitle sub;
            int got = 0;
            if (avcodec_decode_subtitle2(dec, &sub, &got, pkt) >= 0 && got) {
                sub_decoded(v, &sub, offset, dec->width, dec->height);
                avsubtitle_free(&sub);
                n++;
            }
        }
        av_packet_unref(pkt);
    }
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: %d subtitles from %s\n", n, path);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fc);
    return 0;
}

/* The text on screen at the current picture (pictures: "[picture]") */
int reelcore_subtitle_text(const ReelCore *v, char *buf, int size)
{
    int n = 0;
    buf[0] = 0;
    if (v->sub_track < 0 || !v->cur)
        return 0;
    for (int i = 0; i < v->sev_n && v->sev[i].start <= v->cur_pts + 0.001; i++) {
        if (v->cur_pts >= sub_end(v, i))
            continue;
        n += snprintf(buf + n, n < size ? size - n : 0, "%s%s", n ? "\n" : "",
                      v->sev[i].text ? v->sev[i].text : "[picture]");
    }
    return n;
}

int reelcore_subtitle_tracks(const ReelCore *v) { return v->ready ? v->sub_n : 0; }

void reelcore_show_subtitles(ReelCore *v, int on) { v->sub_hidden = !on; }
int reelcore_subtitles_shown(const ReelCore *v) { return v->sub_track >= 0 && !v->sub_hidden; }
int reelcore_subtitle_track(const ReelCore *v) { return v->sub_track; }

int reelcore_subtitle_track_name(const ReelCore *v, int i, char *buf, int size)
{
    const SubTrack *t;
    if (!v->ready || i < 0 || i >= v->sub_n)
        return AVERROR(EINVAL);
    t = &v->sub_tracks[i];
    if (t->path) {                       /* the leaf: RISC OS after the last '.', Unix the last '/' */
        const char *leaf = strrchr(t->path, t->path[0] == '/' ? '/' : '.');
        return snprintf(buf, size, "File: %s", leaf ? leaf + 1 : t->path);
    } else {
        AVStream *st = v->fmt->streams[t->stream];
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        return snprintf(buf, size, "%s%s%s%s(%s)%s", title ? title->value : "", title && lang ? ", " : "",
                        lang ? lang->value : "", title || lang ? " " : "",
                        avcodec_get_name(st->codecpar->codec_id),
                        st->disposition & AV_DISPOSITION_FORCED ? ", forced" : "");
    }
}

int reelcore_set_subtitle_track(ReelCore *v, int i)
{
    if (!v->ready)
        return AVERROR(EAGAIN);
    if (i >= v->sub_n)
        return AVERROR(EINVAL);
    if (i == v->sub_track)
        return 0;
    sub_close_track(v);
    v->sub_track = i < 0 ? -1 : i;
    if (i < 0)
        return 0;
    if (v->sub_tracks[i].path)
        return sub_read_file(v, v->sub_tracks[i].path);
    {
        AVStream *st = v->fmt->streams[v->sub_tracks[i].stream];
        if (!(v->sdec = open_decoder(st))) {
            v->sub_track = -1;
            return AVERROR_DECODER_NOT_FOUND;
        }
        v->sub_stream = v->sub_tracks[i].stream;
        st->discard = AVDISCARD_DEFAULT;
    }
    /* its packets from here: read from the picture shown again */
    if (v->cur && !v->need_first)
        return reelcore_seek(v, reelcore_position(v));
    return 0;
}

int reelcore_add_subtitle_file(ReelCore *v, const char *path)
{
    int i, ret;
    if (!v->ready)
        return AVERROR(EAGAIN);
    for (i = 0; i < v->sub_n; i++)
        if (v->sub_tracks[i].path && !strcmp(v->sub_tracks[i].path, path))
            break;
    if (i == v->sub_n) {
        if (v->sub_n == SUB_TRACKS || !(v->sub_tracks[i].path = av_strdup(path)))
            return AVERROR(ENOMEM);
        v->sub_tracks[i].stream = -1;
        v->sub_n++;
    }
    v->sub_track = -2;                          /* (so it's read again even if chosen) */
    sub_close_track(v);
    v->sub_track = i;
    if ((ret = sub_read_file(v, path)) < 0) {
        av_freep(&v->sub_tracks[i].path);
        memmove(v->sub_tracks + i, v->sub_tracks + i + 1, (v->sub_n - i - 1) * sizeof(*v->sub_tracks));
        v->sub_n--;
        v->sub_track = -1;
        return ret;
    }
    return i;
}

/* At setup: the file's subtitle streams; one marked default or forced is
   chosen (as mpv does), else none */
static void sub_setup(ReelCore *v)
{
    int pick = -1;
    v->sub_track = -1;
    v->sub_stream = -1;
    for (unsigned s = 0; s < v->fmt->nb_streams && v->sub_n < SUB_TRACKS; s++) {
        AVStream *st = v->fmt->streams[s];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE || !avcodec_find_decoder(st->codecpar->codec_id))
            continue;
        if (pick < 0 && st->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED))
            pick = v->sub_n;
        v->sub_tracks[v->sub_n].stream = s;
        v->sub_tracks[v->sub_n].path = NULL;
        v->sub_n++;
    }
    if (pick >= 0)
        reelcore_set_subtitle_track(v, pick);
}

/* ---------------------------------------------------------------- chapters */

int reelcore_chapters(const ReelCore *v) { return v->ready ? (int)v->fmt->nb_chapters : 0; }

double reelcore_chapter_start(const ReelCore *v, int i)
{
    const AVChapter *c;
    double start = v->fmt->start_time != AV_NOPTS_VALUE ? v->fmt->start_time / (double)AV_TIME_BASE : 0;
    if (!v->ready || i < 0 || i >= (int)v->fmt->nb_chapters)
        return -1;
    c = v->fmt->chapters[i];
    return FFMAX(0, c->start * av_q2d(c->time_base) - start);
}

int reelcore_chapter_title(const ReelCore *v, int i, char *buf, int size)
{
    AVDictionaryEntry *t;
    if (!v->ready || i < 0 || i >= (int)v->fmt->nb_chapters)
        return AVERROR(EINVAL);
    t = av_dict_get(v->fmt->chapters[i]->metadata, "title", NULL, 0);
    if (t) {
        char plain[256];
        sub_plain(plain, sizeof(plain), t->value);   /* (UTF-8 to Latin-1) */
        return snprintf(buf, size, "%s", plain);
    }
    return snprintf(buf, size, "Chapter %d", i + 1);
}

int reelcore_chapter_at(const ReelCore *v, double pos)
{
    int n = reelcore_chapters(v), c = -1;
    for (int i = 0; i < n; i++)
        if (reelcore_chapter_start(v, i) <= pos + 0.05)
            c = i;
    return c;
}

/* ----------------------------------------------------------- frame steps */

int reelcore_step(ReelCore *v)
{
    if (!v->ready || !v->paused)
        return AVERROR(EINVAL);
    if (v->need_first)
        return reelcore_update(v);
    for (int tries = 0; !v->qn && tries < 200; tries++) {
        int before = v->n_decoded;
        fill(v);
        if (!v->qn && v->eof_demux && v->vflushed && v->n_decoded == before && !v->vpk_n)
            break;
    }
    if (!v->qn)
        return REELCORE_SAME_FRAME;
    if (v->cur)
        hist_push(v, v->cur, v->cur_pts);  /* (a step back again is at once) */
    take_frame(v);
    v->n_shown++;
    v->pause_pos = v->cur_pts;
    v->stepped = 1;
    return REELCORE_NEW_FRAME;
}

int reelcore_step_back(ReelCore *v)
{
    double d = v->fps > 0 ? 1 / v->fps : 0.04, pos;
    int ret;
    if (!v->ready || !v->paused || !v->cur)
        return AVERROR(EINVAL);
    if (v->need_first)
        return AVERROR(EAGAIN);
    while (v->hist_n && v->hist_pts[v->hist_n - 1] >= v->cur_pts - 0.001)
        av_frame_free(&v->hist[--v->hist_n]);          /* (not before this one) */
    if (v->hist_n && v->qn < QMAX) {
        /* the picture before, kept: at once; the one shown goes back to the
           front of the queue for a step forward */
        memmove(v->q + 1, v->q, v->qn * sizeof(v->q[0]));
        memmove(v->qpts + 1, v->qpts, v->qn * sizeof(v->qpts[0]));
        v->q[0] = v->cur;
        v->qpts[0] = v->cur_pts;
        v->qn++;
        v->cur = v->hist[--v->hist_n];
        cur_changed(v);
        v->cur_pts = v->hist_pts[v->hist_n];
        v->pause_pos = v->cur_pts;
        if (v->sfdec)
            sub_file_feed(v, v->cur_pts);
        v->stepped = 1;
        return REELCORE_NEW_FRAME;
    }
    pos = reelcore_position(v) - 1.5 * d;
    if (pos < 0)
        pos = 0;
    v->stepped = 1;
    hist_clear(v);
    v->bstep = 1;                                      /* keep the ones before it this time */
    ret = reelcore_seek(v, pos);
    if (ret < 0)
        v->bstep = 0;
    return ret;
}

static void fill_black(uint8_t *p, int pitch, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        memset(p + (y + j) * pitch + x * 4, 0, (size_t)w * 4);
}

/* Where the picture goes in w x h (x, y, rw x rh; the rest bars), and the
   part of the frame shown (cx, cy, cw x ch), for flags */
static void place_picture(const ReelCore *v, int fw, int fh, int w, int h, int flags,
                          int *px, int *py, int *prw, int *prh, int *pcx, int *pcy, int *pcw, int *pch)
{
    int x = 0, y = 0, rw = w, rh = h, cx = 0, cy = 0, cw = fw, ch = fh;
    if (flags & (REELCORE_FILL | REELCORE_ORIGINAL)) {
        /* the whole picture at scale s (display pixels, aspect applied):
           fill = cover the rectangle, original = 1:1; what's outside the
           rectangle is cropped, what's left over gets bars */
        double s = flags & REELCORE_FILL ? FFMAX((double)w / v->w, (double)h / v->h) : 1.0;
        double dw = v->w * s, dh = v->h * s;
        rw = FFMAX(dw < w ? (int)(dw + 0.5) : w, 1);
        rh = FFMAX(dh < h ? (int)(dh + 0.5) : h, 1);
        cw = FFMIN(FFMAX((int)(fw * rw / dw + 0.5), 1), fw);
        ch = FFMIN(FFMAX((int)(fh * rh / dh + 0.5), 1), fh);
        cx = (fw - cw) / 2;
        cy = (fh - ch) / 2;
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
    *px = x; *py = y; *prw = rw; *prh = rh;
    *pcx = cx; *pcy = cy; *pcw = cw; *pch = ch;
}

int reelcore_place(const ReelCore *v, int w, int h, int flags, int *x, int *y, int *rw, int *rh)
{
    AVFrame *f = v->cur;
    int cx, cy, cw, ch;
    if (!f)
        return AVERROR(EAGAIN);
    if (w < 1 || h < 1)
        return AVERROR(EINVAL);
    place_picture(v, f->width, f->height, w, h, flags, x, y, rw, rh, &cx, &cy, &cw, &ch);
    return 0;
}

double reelcore_queued_time(ReelCore *v)
{
    double t;
    if (!v->qn || v->paused || !v->ready)
        return 0;
    t = (v->qpts[v->qn - 1] - clock_now(v)) / v->speed;
    return t > 0 ? t : 0;
}

int reelcore_draw_pixels(ReelCore *v, void *pixels, int pitch, int w, int h, int bgr, int flags)
{
    uint8_t *p = pixels;
    AVFrame *f = cur_frame(v);
    int x, y, rw, rh, cx, cy, cw, ch;

    if (!f)
        return AVERROR(EAGAIN);
    if (w < 1 || h < 1)
        return AVERROR(EINVAL);
    place_picture(v, f->width, f->height, w, h, flags, &x, &y, &rw, &rh, &cx, &cy, &cw, &ch);
    if ((x || y || rw < w || rh < h) && !(flags & REELCORE_NO_BORDERS)) {
        fill_black(p, pitch, 0, 0, w, y);
        fill_black(p, pitch, 0, y + rh, w, h - y - rh);
        fill_black(p, pitch, 0, y, x, rh);
        fill_black(p, pitch, x + rw, y, w - x - rw, rh);
    }
    /* RGBA/BGRA: the fourth byte isn't shown, and these get swscale's NEON */
    {
        int ret = convert(v, p + y * pitch + x * 4, pitch, rw, rh,
                          bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, cx, cy, cw, ch);
        if (ret == 0) {
            Place pl[2];
            int n = layers_place(v, pl, rw, rh, 1);
            for (int i = 0; i < n; i++)
                layer_blend_rgb(&pl[i], p + y * pitch + x * 4, pitch, rw, rh, bgr);
        }
        return ret;
    }
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
    v->sm_valid = 0;
    v->sm_step = 0;
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: speed %.2fx\n", speed);
    if (v->dev && !v->stalled && v->adec) {   /* (v->swr can be none for a moment: swr_follow) */
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
    set_deblock(v);
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: fast decoding %s\n",
           mode == REELCORE_FAST_ON ? "on (no deblocking)" :
           mode == REELCORE_FAST_LIGHT ? "light (no deblocking of pictures nothing is predicted from)" : "off");
}

/* The deblocking filter as fast (and auto_fast) say */
static void set_deblock(ReelCore *v)
{
    int mode = v->fast;
    if (!v->vdec)
        return;
    v->vdec->skip_loop_filter = mode == REELCORE_FAST_ON || v->auto_fast ? AVDISCARD_ALL :
                                mode == REELCORE_FAST_LIGHT ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    if (mode == REELCORE_FAST_ON)       /* (the "fast" shortcuts change pictures others are predicted from) */
        v->vdec->flags2 |= AV_CODEC_FLAG2_FAST;
    else
        v->vdec->flags2 &= ~AV_CODEC_FLAG2_FAST;
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
        v->ast->discard = AVDISCARD_ALL;
    actx(v)->streams[s]->discard = AVDISCARD_DEFAULT;
    avcodec_free_context(&v->adec);
    swr_free(&v->swr);
    v->adec = dec;
    v->swr = swr;
    v->as = s;
    v->ast = actx(v)->streams[s];
    av_log(NULL, AV_LOG_VERBOSE, "reelcore: sound track %d (stream %d)\n", i + 1, s);
    return reelcore_seek(v, reelcore_position(v));   /* the new track from here */
}

#ifdef REELCORE_HEVCDEC
/* The layers over the block's picture, already converted into the
   overlay: each layer's rectangle (overlapping ones as one) converted
   again into cached memory, blended there and written over the overlay's.
   The overlay is only written: reading it back a byte at a time (blending
   in place) cost 10-20 ms a picture with the stats panel on, and 4K
   stuttered. Rectangles start on even rows and columns (1:1 wants x and y
   even; halved, a source x a multiple of 4: fx is, and 2 * an even x). */
static int hw_layers(ReelCore *v, hevcdec *d, const hevcdec_frame *hf, uint8_t *const planes[3], const int pitch[3],
                     int w, int h, int fx, int fy, int half, const Place *pl, int nl)
{
    int r[2][4], owner[2], nr = 0;            /* x0, y0, x1, y1 (output pixels) */
    for (int i = 0; i < nl; i++) {
        const Layer *L = pl[i].L;
        int x0 = FFMAX(0, pl[i].x0) & ~1, y0 = FFMAX(0, pl[i].y0) & ~1;
        int x1 = FFMIN(w, (pl[i].x0 + (int)(L->w * pl[i].sx + 0.5) + 1) & ~1);
        int y1 = FFMIN(h, ((int)ceil(pl[i].y0 + L->h * pl[i].sy) + 1) & ~1);
        int j;
        owner[i] = -1;
        if (x0 >= x1 || y0 >= y1)
            continue;                          /* (off the picture) */
        for (j = 0; j < nr; j++)
            if (x0 < r[j][2] && r[j][0] < x1 && y0 < r[j][3] && r[j][1] < y1)
                break;
        if (j < nr) {                          /* overlapping: one rectangle round both */
            r[j][0] = FFMIN(r[j][0], x0); r[j][1] = FFMIN(r[j][1], y0);
            r[j][2] = FFMAX(r[j][2], x1); r[j][3] = FFMAX(r[j][3], y1);
        } else {
            r[nr][0] = x0; r[nr][1] = y0; r[nr][2] = x1; r[nr][3] = y1;
            j = nr++;
        }
        owner[i] = j;
    }
    for (int j = 0; j < nr; j++) {
        int lx = r[j][0], ly = r[j][1], lw = r[j][2] - lx, lh = r[j][3] - ly;
        int cw = (lw + 1) / 2, ch = (lh + 1) / 2, ts[3] = { lw, cw, cw };
        unsigned need = (unsigned)(lw * lh + 2 * cw * ch);
        uint8_t *t[3];
        av_fast_malloc(&v->rect_buf, &v->rect_size, need);
        if (v->rect_buf) {
            t[0] = v->rect_buf; t[1] = t[0] + lw * lh; t[2] = t[1] + cw * ch;
        }
        if (!v->rect_buf || (half && hevcdec_frame_to_i420_half(d, hf, t, ts, fx + 2 * lx, fy + 2 * ly, lw, lh) != HEVCDEC_OK)) {
            /* (no memory, or refused): this rectangle's layers and the
               ones after it blended in the overlay, reading it back; the
               rectangles before are done (blending them again doubled them) */
            for (int i = 0; i < nl; i++)
                if (owner[i] >= j)
                    for (int p = 0; p < 3; p++)
                        for (int y = 0; y < (p ? (h + 1) / 2 : h); y++)
                            if (layer_touches(&pl[i], p, y))
                                layer_blend_row(&pl[i], planes[p] + (size_t)y * pitch[p], p, y, p ? (w + 1) / 2 : w);
            return -1;
        }
        if (!half)
            hevcdec_frame_to_i420(d, hf, t, ts, fx + lx, fy + ly, lw, lh);
        for (int p = 0; p < 3; p++) {
            int sub = p ? 2 : 1, ox = lx / sub, oy = ly / sub, pw = p ? cw : lw, ph = p ? ch : lh;
            for (int y = 0; y < ph; y++) {
                uint8_t *row = t[p] + (size_t)y * ts[p];
                for (int i = 0; i < nl; i++)   /* (the row as if the plane's: x from 0) */
                    if (owner[i] == j && layer_touches(&pl[i], p, oy + y))
                        layer_blend_row(&pl[i], row - ox, p, oy + y, ox + pw);
                memcpy(planes[p] + (size_t)(oy + y) * pitch[p] + ox, row, pw);
            }
        }
    }
    return 0;
}
#endif

#ifdef REELCORE_HEVCDEC
/* One part of the block's frame (x, y, w x h of the output, x and y even)
   into planes at that part's top left */
static int hw_part(hevcdec *d, const hevcdec_frame *hf, uint8_t *const planes[3], const int pitch[3], int fx, int fy,
                   int half, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return HEVCDEC_OK;
    return half ? hevcdec_frame_to_i420_half(d, hf, planes, pitch, fx + 2 * x, fy + 2 * y, w, h)
                : hevcdec_frame_to_i420(d, hf, planes, pitch, fx + x, fy + y, w, h);
}

/* The block's frame with layers over it (the stats panel, subtitles),
   each pixel converted once: the layers' rectangle (widened to the
   block's 128-pixel columns) into cached memory, blended there and
   written to the overlay; everything else straight into the overlay.
   Converting all of it into cached memory and copying it over (opt3)
   took 4K 10-bit from 9.5 to 18 ms a picture (twice the memory traffic,
   which the block's own decoding shares). The overlay is only written.
   (hevcdec 0.1.11 cleans a frame's cache once however many calls.) -1 if
   the block refused or no memory (the caller draws it another way). */
static int hw_bands(ReelCore *v, hevcdec *d, const hevcdec_frame *hf, uint8_t *const planes[3], const int pitch[3],
                    int w, int h, int fx, int fy, int half, const Place *pl, int nl)
{
    int x0 = w, y0 = h, x1 = 0, y1 = 0, col = half ? 64 : 128, rw, rh, cw, ch, ts[3];
    uint8_t *t[3] = { NULL, NULL, NULL };
    for (int i = 0; i < nl; i++) {
        const Layer *L = pl[i].L;
        int a = FFMAX(0, pl[i].x0), b = FFMAX(0, pl[i].y0);
        int c = FFMIN(w, pl[i].x0 + (int)(L->w * pl[i].sx + 0.5) + 1);
        int e = FFMIN(h, (int)ceil(pl[i].y0 + L->h * pl[i].sy) + 1);
        if (a < c && b < e) {
            x0 = FFMIN(x0, a); y0 = FFMIN(y0, b);
            x1 = FFMAX(x1, c); y1 = FFMAX(y1, e);
        }
    }
    if (x0 >= x1 || y0 >= y1)
        x0 = y0 = x1 = y1 = 0;                 /* (all off the picture) */
    x0 = x0 / col * col;                       /* (the block's columns: whole ones are its quick path) */
    x1 = FFMIN(w, (x1 + col - 1) / col * col);
    x1 = x0 + ((x1 - x0) & ~1);
    y0 &= ~1;
    y1 = FFMIN(h, (y1 + 1) & ~1);
    rw = x1 - x0; rh = y1 - y0;
    cw = (rw + 1) / 2; ch = (rh + 1) / 2;
    ts[0] = rw; ts[1] = ts[2] = cw;
    if (rw > 0 && rh > 0) {                    /* the layers' rectangle: into cached memory first */
        av_fast_malloc(&v->rect_buf, &v->rect_size, (size_t)rw * rh + 2 * (size_t)cw * ch);
        if (!v->rect_buf)
            return -1;
        t[0] = v->rect_buf; t[1] = t[0] + (size_t)rw * rh; t[2] = t[1] + (size_t)cw * ch;
        if (hw_part(d, hf, t, ts, fx, fy, half, x0, y0, rw, rh) != HEVCDEC_OK)
            return -1;                         /* (nothing written yet) */
    }
    {   /* the rest straight in: above, below, left and right of it */
        const int part[4][4] = { { 0, 0, w, y0 }, { 0, y1, w, h - y1 }, { 0, y0, x0, rh }, { x1, y0, w - x1, rh } };
        for (int k = 0; k < 4; k++) {
            int px = part[k][0], py = part[k][1];
            uint8_t *o[3];
            if (part[k][2] <= 0 || part[k][3] <= 0)
                continue;
            for (int p = 0; p < 3; p++)
                o[p] = planes[p] + (size_t)(p ? py / 2 : py) * pitch[p] + (p ? px / 2 : px);
            if (hw_part(d, hf, o, pitch, fx, fy, half, px, py, part[k][2], part[k][3]) != HEVCDEC_OK)
                return -1;                     /* (the caller draws all of it again) */
        }
    }
    for (int p = 0; p < 3 && rw > 0 && rh > 0; p++) {   /* the rectangle: blended, then written once */
        int ox = p ? x0 / 2 : x0, oy = p ? y0 / 2 : y0, pw = p ? cw : rw, ph = p ? ch : rh;
        for (int y = 0; y < ph; y++) {
            uint8_t *row = t[p] + (size_t)y * ts[p];
            for (int i = 0; i < nl; i++)       /* (the row as if the plane's: x from 0) */
                if (layer_touches(&pl[i], p, oy + y))
                    layer_blend_row(&pl[i], row - ox, p, oy + y, ox + pw);
            memcpy(planes[p] + (size_t)(oy + y) * pitch[p] + ox, row, pw);
        }
    }
    return 0;
}
#endif

int reelcore_draw_yuv420(ReelCore *v, uint8_t *const planes[3], const int pitch[3], int w, int h, int *colour)
{
    AVFrame *f = v->cur;
    int64_t t0;
    int c = 0, nl, half;
    Place pl[2];
    if (!f)
        return AVERROR(EAGAIN);
    if (f->colorspace == AVCOL_SPC_BT709)            /* as convert() decides */
        c |= REELCORE_YUV_709;
    if (f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P ||
        f->format == AV_PIX_FMT_YUVJ444P || f->format == AV_PIX_FMT_YUVJ440P || f->format == AV_PIX_FMT_YUVJ411P)
        c |= REELCORE_YUV_FULL;                   /* (every yuvj format is full range) */
    if (colour)
        *colour = c;
    if (!planes)
        return 0;                                   /* just the colours */
    {
        int wx, wy, ww, wh;                       /* (the block's frames: their window, after the crop) */
        frame_window(f, &wx, &wy, &ww, &wh);
        if (w < 2 || h < 2 || w > ww || h > wh)
            return AVERROR(EINVAL);
    }
    t0 = av_gettime_relative();
    {
        int k = 0;
        nl = layers_place(v, pl, w, h, v->yuv_k > 0 ? v->yuv_k : 1);
        for (int i = 0; i < nl; i++)
            if (layer_yuv((Layer *)pl[i].L, c) == 0)
                pl[k++] = pl[i];
        nl = k;
    }
#ifdef REELCORE_HEVCDEC
    if (hw_frame(f)) {
        /* the block's frame, converted straight into the overlay: 1:1, or
           (4K into an HD-sized overlay) halved in the same pass */
        const hevcdec_frame *hf = (const hevcdec_frame *)f->data[3];
        hevcdec *d = hevcdec_frame_decoder(hf);
        int fx, fy, fw, fh, done = 0;
        frame_window(f, &fx, &fy, &fw, &fh);
        half = w * 2 <= fw && h * 2 <= fh;
        if (nl && ((!half && w <= fw && h <= fh) || (half && w * 4 > fw && h * 4 > fh && !(fx & 3) && !(fy & 1))) &&
            hw_bands(v, d, hf, planes, pitch, w, h, fx, fy, half, pl, nl) == 0) {
            v->hw_draws++;                        /* (with layers: one conversion call, then copied) */
            v->t_convert += av_gettime_relative() - t0;
            v->conv_w = w;
            v->conv_h = h;
            v->halvings = half;
            return 0;
        }
        if (!half && w <= fw && h <= fh) {
            done = hevcdec_frame_to_i420(d, hf, planes, pitch, fx, fy, w, h) == HEVCDEC_OK;
        } else if (half && w * 4 > fw && h * 4 > fh && !(fx & 3) && !(fy & 1))
            done = hevcdec_frame_to_i420_half(d, hf, planes, pitch, fx, fy, w, h) == HEVCDEC_OK;
        if (done) {
            v->hw_draws++;
            if (nl)
                hw_layers(v, d, hf, planes, pitch, w, h, fx, fy, half, pl, nl);   /* (blends in place itself if it must) */
            v->t_convert += av_gettime_relative() - t0;
            v->conv_w = w;
            v->conv_h = h;
            v->halvings = half;
            return 0;
        }
        if (!(f = cur_frame(v)))                  /* anything else: from a copy */
            return AVERROR(ENOMEM);
    }
#endif
    if (!(f = cur_frame(v)))                      /* (10-bit: narrowed, in NEON) */
        return AVERROR(ENOMEM);
    /* smaller than the frame (4K into an HD-sized overlay): halved */
    half = w * 2 <= f->width && h * 2 <= f->height;
    if ((f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) &&
        (!half || (w * 4 > f->width && h * 4 > f->height))) {
        /* the decoder's own planes: row copies (the rows may be wider), or
           each output row the NEON average of two rows' 2x2 blocks */
        uint8_t *tmp = nl || half ? av_malloc(w) : NULL;
        if ((nl || half) && !tmp)
            return AVERROR(ENOMEM);
        for (int p = 0; p < 3; p++) {
            int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
            for (int y = 0; y < ph; y++) {
                const uint8_t *src;
                int touched = half;
                if (half) {
                    reelcore_halve_plane(tmp, pw, f->data[p] + (size_t)y * 2 * f->linesize[p], f->linesize[p], pw, 1);
                    src = tmp;
                } else
                    src = f->data[p] + (size_t)y * f->linesize[p];
                for (int i = 0; tmp && i < nl; i++)
                    if (layer_touches(&pl[i], p, y)) {
                        if (!touched)                  /* blended in cached memory, then written once */
                            memcpy(tmp, src, pw);
                        touched = 1;
                        layer_blend_row(&pl[i], tmp, p, y, pw);
                    }
                memcpy(planes[p] + (size_t)y * pitch[p], touched ? tmp : src, pw);
            }
        }
        av_free(tmp);
    } else if ((f->format == AV_PIX_FMT_YUV444P || f->format == AV_PIX_FMT_YUVJ444P) && half &&
               w * 4 > f->width && h * 4 > f->height) {
        /* 4:4:4 (a mastering profile, e.g. a 4K trailer) into a half-size
           overlay: luma halved once, colour halved twice, all NEON (swscale
           took 176 ms a picture for 3996x1730 on a Pi 4) */
        uint8_t *tmp = av_malloc((size_t)w * 3);
        if (!tmp)
            return AVERROR(ENOMEM);
        for (int p = 0; p < 3; p++) {
            int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
            for (int y = 0; y < ph; y++) {
                if (!p)
                    reelcore_halve_plane(tmp, pw, f->data[0] + (size_t)y * 2 * f->linesize[0], f->linesize[0], pw, 1);
                else {                                 /* 4 rows -> 2 half rows -> 1 quarter row */
                    reelcore_halve_plane(tmp + w, w, f->data[p] + (size_t)y * 4 * f->linesize[p], f->linesize[p], w, 2);
                    reelcore_halve_plane(tmp, pw, tmp + w, w, pw, 1);
                }
                for (int i = 0; i < nl; i++)
                    if (layer_touches(&pl[i], p, y))
                        layer_blend_row(&pl[i], tmp, p, y, pw);
                memcpy(planes[p] + (size_t)y * pitch[p], tmp, pw);
            }
        }
        av_free(tmp);
    } else {
        /* anything else (10-bit, 4:2:2, 4:4:4, ... or 8K): to 4:2:0 at w x h
           (the same size: each pixel as it is; smaller: averaged) */
        const uint8_t *src[4] = { f->data[0], f->data[1], f->data[2], f->data[3] };
        uint8_t *d[4] = { planes[0], planes[1], planes[2], NULL };
        int dp[4] = { pitch[0], pitch[1], pitch[2], 0 };
        int how = !half ? SWS_POINT : w * 4 > f->width ? SWS_FAST_BILINEAR : SWS_BILINEAR;
        /* a yuvj format as its yuv one: swscale took yuvj as full range
           and made limited from it, though the range said (*colour) is
           the frame's own; the same range in and out, it's left as it is
           (and the cached context is kept: swscale renames yuvj inside) */
        enum AVPixelFormat sf = f->format == AV_PIX_FMT_YUVJ420P ? AV_PIX_FMT_YUV420P :
                                f->format == AV_PIX_FMT_YUVJ422P ? AV_PIX_FMT_YUV422P :
                                f->format == AV_PIX_FMT_YUVJ444P ? AV_PIX_FMT_YUV444P :
                                f->format == AV_PIX_FMT_YUVJ440P ? AV_PIX_FMT_YUV440P :
                                f->format == AV_PIX_FMT_YUVJ411P ? AV_PIX_FMT_YUV411P : f->format;
        v->sws_yuv = sws_getCachedContext(v->sws_yuv, f->width, f->height, sf, w, h, AV_PIX_FMT_YUV420P,
                                          how, NULL, NULL, NULL);
        if (!v->sws_yuv || sws_scale(v->sws_yuv, src, f->linesize, 0, f->height, d, dp) < 0)
            return AVERROR_EXTERNAL;
        for (int i = 0; i < nl; i++)              /* (reads the rows back: slower, but rare) */
            for (int p = 0; p < 3; p++)
                for (int y = 0; y < (p ? h / 2 : h); y++)
                    if (layer_touches(&pl[i], p, y))
                        layer_blend_row(&pl[i], planes[p] + (size_t)y * pitch[p], p, y, p ? w / 2 : w);
    }
    v->t_convert += av_gettime_relative() - t0;
    v->conv_w = w;
    v->conv_h = h;
    v->halvings = half;
    return 0;
}

int reelcore_frame_size(const ReelCore *v, int *w, int *h)
{
    int x, y;
    if (!v->cur)
        return AVERROR(EAGAIN);
    frame_window(v->cur, &x, &y, w, h);
    return 0;
}

void reelcore_attach(ReelCore *v, void *data, void (*release)(void *data))
{
    v->attach = data;
    v->attach_release = release;
}

void *reelcore_attachment(const ReelCore *v) { return v->attach; }
