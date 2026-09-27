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


#ifdef __cplusplus
extern "C" {
#endif

typedef struct ReelCore ReelCore;

/* reelcore_open flags */
#define REELCORE_NO_AUDIO   1   /* don't play the sound (pictures follow a timer) */
#define REELCORE_LOOP       2   /* start again at the end */
#define REELCORE_PAUSED     4   /* open paused (reelcore_pause(v, 0) starts it) */

/* reelcore_update results */
#define REELCORE_SAME_FRAME 0   /* nothing new to show */
#define REELCORE_NEW_FRAME  1   /* a new frame is current: draw it */
#define REELCORE_END        2   /* played to the end (not with REELCORE_LOOP) */

/* reelcore_draw_pixels flags (also for ffegl_draw_surface) */
#define REELCORE_STRETCH    1   /* fill the rectangle (default: keep the shape, black bars) */
#define REELCORE_NO_BORDERS 2   /* keep the shape but leave the bars alone */

/* Opens a file or URL. NULL on failure (the reason is logged through
   av_log; reelcore_last_error() gives it too). */
ReelCore *reelcore_open(const char *url, int flags);
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

/* Draws the current frame into 32bpp memory: w x h pixels, pitch bytes a
   row, top row first. bgr = 0: bytes R,G,B,x (sprite type 6 / TBGR, 0x00BBGGRR);
   bgr = 1: bytes B,G,R,x (0x00RRGGBB). */
int reelcore_draw_pixels(ReelCore *v, void *pixels, int pitch, int w, int h,
                      int bgr, int flags);

/* The current frame's own size in pixels (as decoded, before the display
   aspect): for converting it 1:1 with reelcore_draw_pixels(..., REELCORE_STRETCH).
   0, or AVERROR(EAGAIN) before the first frame. */
int reelcore_frame_size(const ReelCore *v, int *w, int *h);

/* For a layer on top (ffegl keeps its texture state here): one pointer per
   video, and a function that frees it, called by reelcore_close(). */
void reelcore_attach(ReelCore *v, void *data, void (*release)(void *data));
void *reelcore_attachment(const ReelCore *v);

#ifdef __cplusplus
}
#endif

#endif
