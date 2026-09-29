/* convert.h - !FFmpeg's Convert window (convert.c). */
#ifndef CONVERT_H
#define CONVERT_H
#include <stddef.h>
#include <kernel.h>

enum { CONV_MP4, CONV_MKV, CONV_MP3, CONV_M4A, CONV_FORMATS };
#define CONV_PRESETS   6
#define CONV_SIZES     6
#define CONV_QUALITIES 4
#define CONV_SPEEDS    3

/* the window's state */
enum { CONV_NONE, CONV_PROBING, CONV_READY, CONV_RUNNING, CONV_STOPPING, CONV_DONE, CONV_FAILED, CONV_STOPPED };

typedef struct {
    const char *name, *desc;
    int format, size, quality, speed, sound, easy;   /* format -1: Custom */
} ConvPreset;

typedef struct {
    int preset, format, size, quality, speed;
    int sound, deinterlace, easy;
    char from[16], to[16];              /* trim, as typed ("" = start / end) */
    char extra[256];                    /* more ffmpeg options for the output, as typed ("" = none) */
} ConvSettings;

typedef struct {                        /* what ffprobe found */
    int has_video, has_audio;
    int width, height, interlaced;
    double fps, duration;
    int channels, rate;
    char vcodec[16], acodec[16];
} ConvSource;

typedef struct { char type[12], codec[16]; int width, height, channels, rate, interlaced; double fps; } ConvStream;

typedef struct {                        /* ffmpeg's -progress lines, and its errors */
    double out_time, speed;
    long long bytes;
    int ended, updates;
    char error[160];
} ConvProgress;

extern const ConvPreset conv_presets[CONV_PRESETS];
extern const char *const conv_format_names[CONV_FORMATS], *const conv_format_ext[CONV_FORMATS];
extern const char *const conv_size_names[CONV_SIZES], *const conv_quality_names[CONV_QUALITIES],
                  *const conv_speed_names[CONV_SPEEDS];

/* the logic (no Wimp) */
int conv_sound_only(int format);
void conv_apply_preset(ConvSettings *s, int preset);
double conv_parse_time(const char *t);
void conv_format_time(char *buf, size_t n, double seconds);
int conv_build_args(char *buf, size_t size, const ConvSettings *s, const ConvSource *in,
                    const char *src, const char *out, char *err, size_t errsize);
void conv_output_leaf(char *buf, size_t size, const char *src, const ConvSettings *s, const ConvSource *in);
void conv_parse_probe(ConvSource *in, const char *text);
void conv_describe(char *buf, size_t size, const ConvSource *in);
void conv_parse_progress(ConvProgress *p, const char *line);

/* the window, from fffront.c's poll loop */
void conv_init(int task, const char *ffdir);
void conv_open(void);
int conv_window(void);
int conv_busy(void);
int conv_click(const int *block);
int conv_key(const int *block);
int conv_close_request(const int *block);
int conv_menu_open(void);
void conv_menu_forget(void);
void conv_menu_select(const int *sel);
void conv_drag_end(void);
int conv_message(int *block);
void conv_sync(void);

/* from fffront.c */
_kernel_oserror *fffront_start_task(const char *cmd);
void fffront_play(const char *file);

#endif
