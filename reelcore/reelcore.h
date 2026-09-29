/*
 * reelcore.h - the player core of riscos-ffmpeg (used by !Reel and, through
 * ffegl, by !ReelEGL and the EGL examples).
 *
 * A small layer over libavformat/libavcodec/libswscale/libswresample that
 * plays a video file (or a network stream) with its sound and gives each
 * picture at its time. Open a file, call reelcore_update() from your event
 * loop, and when it says there's a new frame put it where you want it with
 * reelcore_draw_pixels(): any 32bpp memory, e.g. a sprite. For EGL surfaces
 * and OpenGL textures, ffegl.h adds ffegl_draw_surface() and
 * ffegl_texture() on top. reelcore itself has no EGL in it.
 *
 * Sound plays through SharedSoundBuffer/StreamManager on RISC OS, given
 * the sound from reelcore_update() itself (SDL2's audio elsewhere, or with
 * REELCORE_AUDIO=sdl), and is the clock the pictures follow. There are no
 * threads: decoding happens inside reelcore_update(), so call it often
 * (every Wimp null event, or every frame); reelcore_idle_time() says how
 * long you may sleep in between.
 *
 *   ReelCore *v = reelcore_open("SDFS::Pi.$.clip/mp4", 0);
 *   ...
 *   int r = reelcore_update(v);          // in the event loop
 *   if (r == REELCORE_NEW_FRAME)
 *       reelcore_draw_pixels(v, pixels, pitch, w, h, 0, 0);
 *   else if (r == REELCORE_END) ...
 *   ...
 *   reelcore_close(v);
 *
 * Part of riscos-ffmpeg. LGPL 2.1 or later (as FFmpeg's libraries).
 */
#ifndef REELCORE_H
#define REELCORE_H
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReelCore ReelCore;

/* reelcore_open flags */
#define REELCORE_NO_AUDIO   1   /* don't play the sound (pictures follow a timer) */
#define REELCORE_LOOP       2   /* start again at the end */
#define REELCORE_PAUSED     4   /* open paused (reelcore_pause(v, 0) starts it) */
#define REELCORE_ASYNC      8   /* reelcore_open_source: return at once; see below */

/* reelcore_update results */
#define REELCORE_SAME_FRAME 0   /* nothing new to show */
#define REELCORE_NEW_FRAME  1   /* a new frame is current: draw it */
#define REELCORE_END        2   /* played to the end (not with REELCORE_LOOP) */
#define REELCORE_OPENING    3   /* (REELCORE_ASYNC) still opening: call again later */
#define REELCORE_READY      4   /* (REELCORE_ASYNC) open now: the size etc. are known */
#define REELCORE_FAILED     5   /* (REELCORE_ASYNC) couldn't open: reelcore_last_error() */

/* reelcore_draw_pixels flags (also for ffegl_draw_surface) */
#define REELCORE_STRETCH    1   /* fill the rectangle (default: keep the shape, black bars) */
#define REELCORE_NO_BORDERS 2   /* keep the shape but leave the bars alone */
#define REELCORE_FILL       4   /* keep the shape and cover the rectangle (the rest is cropped) */
#define REELCORE_ORIGINAL   8   /* one display pixel a pixel, centred (cropped or with bars) */

/* Opens a file or URL. NULL on failure (the reason is logged through
   av_log; reelcore_last_error() gives it too). */
ReelCore *reelcore_open(const char *url, int flags);

/* What to play, more fully: a video file or address, and optionally the
   sound from another (as yt-dlp gives a site's best video and best sound,
   "bestvideo+bestaudio"), HTTP headers ("Name: value\r\n" lines) and a
   user agent the site wants (yt-dlp's http_headers), and a title. */
typedef struct ReelCoreSource {
    const char *url;
    const char *audio_url;             /* or NULL: the sound is in url */
    const char *headers;               /* or NULL */
    const char *user_agent;            /* or NULL: FFmpeg's */
    const char *title;                 /* or NULL: from the file */
} ReelCoreSource;

/* Opens a source. Network addresses (reelcore_is_network) are opened and
   read by a thread of reelcore's own, reading up to 10 s ahead, so a slow
   connection doesn't hold up the caller; everything else is still called
   from the caller's thread. With REELCORE_ASYNC the call returns at once
   (NULL only if it couldn't even start) and reelcore_update() returns
   REELCORE_OPENING until the address is open, then REELCORE_READY once
   (reelcore_width etc. now answer), or REELCORE_FAILED. Without it, the
   call waits until it's open. Files are always opened at once. */
ReelCore *reelcore_open_source(const ReelCoreSource *src, int flags);
/* 1: an address (a "scheme://" other than file:), read over the network */
int reelcore_is_network(const char *url);
/* 1 open, 0 still opening (REELCORE_ASYNC), -1 failed */
int reelcore_ready(const ReelCore *v);

/* Reading from the network, for a "buffering" display and Media info */
typedef struct ReelCoreNet {
    int opening;                       /* not open yet */
    int buffering;                     /* nothing read ahead: the picture and sound wait */
    int ended;                         /* read to the end */
    double ahead;                      /* seconds read ahead of the picture shown */
    unsigned bytes_ahead;              /* ... in bytes */
    long long bytes_read;              /* in all */
    char error[200];                   /* why it failed, or reading stopped; "" */
} ReelCoreNet;
/* 0 (st cleared) for a file; 1 for an address */
int reelcore_net(const ReelCore *v, ReelCoreNet *st);
const char *reelcore_last_error(void);
void reelcore_close(ReelCore *v);

/* The video's size in pixels (display aspect applied to the width), its
   frame rate and length in seconds (0 when unknown, e.g. a live stream),
   and whether it has sound that is being played. */
int reelcore_width(const ReelCore *v);
int reelcore_height(const ReelCore *v);
double reelcore_frame_rate(const ReelCore *v);
double reelcore_duration(const ReelCore *v);
int reelcore_has_audio(const ReelCore *v);

/* A one-line description of the streams, e.g. "h264 1280x720, 30 fps;
   aac 48000 Hz, 2 channels; mov,mp4,m4a,3gp,3g2,mj2". Returns its length
   (as snprintf). */
int reelcore_info(const ReelCore *v, char *buf, int size);

/* Frames skipped because they were already late (the CPU fell behind). */
unsigned reelcore_dropped_frames(const ReelCore *v);

/* For logs: one line on the state of playback now: position and clock,
   pictures waiting, late frames, and the sound (which output, how much is
   queued; with SharedSoundBuffer, StreamManager's added and played counts).
   Returns its length (as snprintf). */
int reelcore_debug(const ReelCore *v, char *buf, int size);

/* What's in the file, for an information window: lines of "Label\tValue\n",
   with "#Section\n" lines between (File, Video, Audio, Sound output).
   Returns the length (as snprintf). */
int reelcore_media_info(const ReelCore *v, char *buf, int size);

/* Running totals, for "stats for nerds": take two, some time apart, and
   divide the differences by the time. Times are the processor time spent,
   in seconds (measured with the centisecond clock on RISC OS, so only
   right on average over many frames). */
typedef struct ReelCoreStats {
    double position, clock;
    int clock_source;                 /* 0 timer, 1 sound, 2 paused */
    double fps;                       /* the video's own frame rate */
    unsigned decoded;                 /* pictures decoded */
    unsigned shown;                   /* pictures handed out (REELCORE_NEW_FRAME) */
    unsigned late;                    /* pictures skipped as late */
    double decode_time;               /* decoding pictures */
    double audio_time;                /* decoding and resampling sound */
    double convert_time;              /* converting and scaling pictures (swscale) */
    int convert_w, convert_h;         /* the last conversion's size */
    int pictures_waiting, packets_waiting;
    unsigned packet_bytes;
    int skip_level;                   /* 0 every frame, 1 not non-reference ones, 2 keyframes only */
    unsigned skip_spells;
    int sound;                        /* 0 none, 1 SharedSoundBuffer, 2 SDL */
    int sound_stalled;
    double sound_queued;              /* seconds */
    unsigned sound_added, sound_played;   /* StreamManager's counts (SharedSoundBuffer) */
    long long bytes_read;             /* from the file */
    double speed;                     /* reelcore_set_speed */
    int fast;                         /* reelcore_set_fast */
    int audio_track, audio_tracks;    /* the sound track played (0 = the first; -1 none), and how many */
    int deinterlace;                  /* REELCORE_DEINT_* */
    unsigned interlaced;              /* pictures decoded that were interlaced */
    unsigned deinterlaced;            /* pictures that came out of the deinterlacer */
    double deinterlace_time;          /* seconds spent deinterlacing */
    int halvings;                     /* the last conversion halved the picture this many times first */
    /* how evenly pictures are handed out: running totals of |real time
       between two pictures - their time apart in the file| (seconds), for
       each picture that followed the one before; the mean is pace_sum /
       pace_n (take the difference of two readings for a recent mean) */
    double pace_sum;
    unsigned pace_n;
    /* how far the clock pictures follow was from the sound each time the
       sound's reading moved (running totals, seconds): the lip sync */
    double sync_err_sum;
    unsigned sync_err_n;
} ReelCoreStats;
void reelcore_stats(const ReelCore *v, ReelCoreStats *st);

/* Sends FFmpeg's (and reelcore's) messages, one line at a time without the
   newline, to FN instead of stderr; VERBOSE adds reelcore's sound details
   (stream opened, started, refused blocks) and FFmpeg's verbose messages.
   FN NULL puts things back. */
void reelcore_set_log(void (*fn)(int level, const char *line), int verbose);

/* Decodes what is needed and chooses the frame for "now".
   Returns REELCORE_NEW_FRAME, REELCORE_SAME_FRAME, REELCORE_END, or a negative
   AVERROR code. The first call after opening (or seeking) always gives
   REELCORE_NEW_FRAME once a picture is decoded. */
int reelcore_update(ReelCore *v);

/* After reelcore_update(): how long the caller can sleep (e.g. Wimp_PollIdle)
   before calling it again, in seconds. 0 while there's work to do now
   (pictures to decode, sound to top up, a picture due); otherwise the time
   until the next picture is due, at most 0.1 s (the sound is kept 0.5 s
   ahead, so that's plenty). Pictures are decoded ahead before sleeping, so
   waking when one is due is enough to show it on time. */
double reelcore_idle_time(ReelCore *v);

/* Position of the current frame in seconds. */
double reelcore_position(const ReelCore *v);

void reelcore_pause(ReelCore *v, int paused);
int reelcore_paused(const ReelCore *v);
/* Seeks to a time in seconds (to the key frame at or before it). */
int reelcore_seek(ReelCore *v, double seconds);
/* Sound volume, 0.0 to 1.0. */
void reelcore_set_volume(ReelCore *v, double volume);
double reelcore_volume(const ReelCore *v);

/* Playback speed, 0.5 to 2 (1 = normal). The sound keeps its pitch
   (FFmpeg's atempo filter). Changing it restarts the sound from the picture
   on screen (a short gap). */
int reelcore_set_speed(ReelCore *v, double speed);
double reelcore_speed(const ReelCore *v);

/* Deinterlacing (FFmpeg's yadif, one picture per frame; NEON on ARM with
   riscos-ffmpeg's patch 0015). AUTO (the default) deinterlaces the pictures
   the decoder marks as interlaced, and costs nothing for progressive video;
   ON deinterlaces every picture (for files that don't say); OFF never. */
#define REELCORE_DEINT_OFF  0
#define REELCORE_DEINT_AUTO 1
#define REELCORE_DEINT_ON   2
void reelcore_set_deinterlace(ReelCore *v, int mode);
int reelcore_deinterlace(const ReelCore *v);

/* Fast decoding: skips the deblocking filter (H.264, HEVC and others).
   REELCORE_FAST_ON (1, as "on" always was): on every picture, about 20% less
   decoding time, a slightly softer and blockier picture; the errors carry
   on into the pictures predicted from them until the next keyframe.
   REELCORE_FAST_LIGHT: only on pictures no other picture is predicted from
   (most B-frames), so nothing carries over: about 15% less for typical
   H.264 with B-frames, and invisible once the picture is shown smaller
   (Reel's mini player). Can be changed at any time. */
#define REELCORE_FAST_OFF   0
#define REELCORE_FAST_ON    1
#define REELCORE_FAST_LIGHT 2
void reelcore_set_fast(ReelCore *v, int mode);
int reelcore_fast(const ReelCore *v);

/* Big reductions are halved first (2x2 averages, NEON on ARM) while the
   picture is at least twice the size wanted both ways, up to this many
   times; see reelcore.c. reelcore_halve_plane is exported for the tests. */
#define REELCORE_HALVINGS 3
void reelcore_halve_plane(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int w, int h);

/* The file's sound tracks: how many, which one plays (0 = the first; -1 =
   none), a short description ("aac, 2 ch, eng, Commentary"), and changing
   track (the new one starts from the picture on screen). */
int reelcore_audio_tracks(const ReelCore *v);
int reelcore_audio_track(const ReelCore *v);
int reelcore_audio_track_name(const ReelCore *v, int track, char *buf, int size);
int reelcore_set_audio_track(ReelCore *v, int track);

/* Draws the current frame into 32bpp memory: w x h pixels, pitch bytes a
   row, top row first. bgr = 0: bytes R,G,B,x (sprite type 6 / TBGR, 0x00BBGGRR);
   bgr = 1: bytes B,G,R,x (0x00RRGGBB). */
int reelcore_draw_pixels(ReelCore *v, void *pixels, int pitch, int w, int h,
                      int bgr, int flags);

/* The current frame's own size in pixels (as decoded, before the display
   aspect): for converting it 1:1 with reelcore_draw_pixels(..., REELCORE_STRETCH).
   0, or AVERROR(EAGAIN) before the first frame. */
int reelcore_frame_size(const ReelCore *v, int *w, int *h);

/* The current frame as planar 4:2:0, Y then Cb then Cr (RISC OS's "YV12",
   FFmpeg's yuv420p), for a hardware overlay: w x h pixels (even, at most
   the frame's size: normally reelcore_frame_size rounded down to even), no
   scaling. yuv420p frames are copied row by row (write-only: fine for
   uncached overlay memory); other formats go through swscale. *colour (may
   be NULL): REELCORE_YUV_709 (else BT.601) | REELCORE_YUV_FULL (else video
   range), for the overlay's ModeFlags; planes NULL: only *colour. 0, or a
   negative AVERROR. */
#define REELCORE_YUV_709   1
#define REELCORE_YUV_FULL  2
int reelcore_draw_yuv420(ReelCore *v, uint8_t *const planes[3], const int pitch[3], int w, int h,
                         int *colour);

/* For a layer on top (ffegl keeps its texture state here): one pointer per
   video, and a function that frees it, called by reelcore_close(). */
void reelcore_attach(ReelCore *v, void *data, void (*release)(void *data));
void *reelcore_attachment(const ReelCore *v);

#ifdef __cplusplus
}
#endif

#endif
