/*
 * convert.c - !FFmpeg's Convert window.
 *
 * Opened by clicking the FFmpeg icon (or Convert... on its menu). One
 * window, top to bottom in the order you use it:
 *
 *   1. Source: drop a video or sound file on the window. ffprobe reads it
 *      and a line says what's in it (codecs, size, rate, sound, length).
 *   2. Convert to: a preset (the default suits playing on this machine),
 *      with one line saying what it does. The options under it show what
 *      the preset set; changing any of them makes it "Custom". Options
 *      that don't apply (the picture size for a sound-only format, sound
 *      when the file has none) are shaded, not hidden.
 *   3. Save: the RISC OS way. Drag the file icon to a directory display,
 *      or type a full path and press Return or click Convert. The name is
 *      filled in from the source's (e.g. film_720/mp4).
 *   4. Progress: a bar, where it's got to, how fast (x real time) and
 *      about how long is left; Stop. Then Play, Show (the directory) and,
 *      if it failed, Log with ffmpeg's own messages.
 *
 * Nothing is lost by mistake: an existing file is only replaced after
 * asking, a stopped conversion's unfinished file is deleted, closing the
 * window or quitting while converting asks first. Interactive help
 * (!Help) describes every part of the window.
 *
 * ffprobe and ffmpeg run in task windows of their own (*TaskWindow ...
 * -task -txt: no window, their output comes to us as TaskWindow_Output
 * messages), so the desktop carries on while they work. ffmpeg's
 * "-progress pipe:1" lines (out_time_us=, speed=, progress=) drive the
 * bar; its errors (-v error) are the log. Their command lines are written
 * to Obey files in <Wimp$ScrapDir>.ffmpeg, which keeps *TaskWindow's own
 * command line short.
 *
 * Part of riscos-ffmpeg. GPL v2 or later (as the rest of !FFmpeg).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "convert.h"

#ifdef CONV_HOST_PATHS                  /* the host test: Unix paths for our own files */
#define DIRSEP "/"
#else
#define DIRSEP "."
#endif

#define Wimp_CreateWindow      0x400C1
#define Wimp_CreateIcon        0x400C2
#define Wimp_OpenWindow        0x400C5
#define Wimp_CloseWindow       0x400C6
#define Wimp_GetWindowState    0x400CB
#define Wimp_SetIconState      0x400CD
#define Wimp_GetIconState      0x400CE
#define Wimp_GetPointerInfo    0x400CF
#define Wimp_DragBox           0x400D0
#define Wimp_SetCaretPosition  0x400D2
#define Wimp_CreateMenu        0x400D4
#define Wimp_ProcessKey        0x400DC
#define Wimp_ReportError       0x400DF
#define Wimp_SendMessage       0x400E7
#define DragASprite_Start      0x42400
#define DragASprite_Stop       0x42401
#define MimeMap_Translate      0x50B00
#define OS_File                0x08
#define OS_ReadMonotonicTime   0x42

#define MSG_DATASAVE      1
#define MSG_DATASAVEACK   2
#define MSG_DATALOAD      3
#define MSG_DATALOADACK   4
#define MSG_PREQUIT       8
#define MSG_HELPREQUEST   0x502
#define MSG_HELPREPLY     0x503
#define MSG_TW_OUTPUT     0x808C1
#define MSG_TW_EGO        0x808C2
#define MSG_TW_MORIO      0x808C3
#define MSG_TW_MORITE     0x808C4

#define TXT_PROBE   0x50524F42          /* our -txt handles: "PROB", "CONV" */
#define TXT_CONVERT 0x434F4E56

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* ---- the settings and the command line (no Wimp: the host test calls these) ---- */

const ConvPreset conv_presets[CONV_PRESETS] = {
    /* name, what it does, format, size, quality, speed, sound, easy */
    { "For playing here", "MP4 up to 720 lines, easy to decode: best for Reel",
      CONV_MP4, 2, 2, 1, 1, 1 },
    { "MP4, same size", "MP4 at the picture's own size, good quality",
      CONV_MP4, 0, 1, 1, 1, 0 },
    { "Smaller file", "MP4 up to 480 lines, lower quality: small, for sending",
      CONV_MP4, 4, 3, 1, 1, 0 },
    { "Sound only: MP3", "Only the sound, as a high quality MP3 file",
      CONV_MP3, 0, 1, 1, 1, 0 },
    { "Sound only: AAC", "Only the sound, as an AAC (M4A) file",
      CONV_M4A, 0, 1, 1, 1, 0 },
    { "Custom", "Your own choice of the options below", -1, 0, 0, 0, 0, 0 },
};
const char *const conv_format_names[CONV_FORMATS] = { "MP4 video", "MKV video", "MP3 sound", "M4A sound" };
const char *const conv_format_ext[CONV_FORMATS] = { "mp4", "mkv", "mp3", "m4a" };
const char *const conv_size_names[CONV_SIZES] = { "Same", "1080 lines", "720 lines", "576 lines", "480 lines", "360 lines" };
static const int size_lines[CONV_SIZES] = { 0, 1080, 720, 576, 480, 360 };
const char *const conv_quality_names[CONV_QUALITIES] = { "Best", "Good", "Normal", "Small" };
static const int crf[CONV_QUALITIES] = { 18, 21, 23, 28 };
static const int aac_kbps[CONV_QUALITIES] = { 256, 160, 128, 96 };
static const int mp3_q[CONV_QUALITIES] = { 0, 2, 4, 6 };
const char *const conv_speed_names[CONV_SPEEDS] = { "Fastest", "Fast", "Balanced" };
static const char *const x264_preset[CONV_SPEEDS] = { "ultrafast", "veryfast", "medium" };

int conv_sound_only(int format) { return format == CONV_MP3 || format == CONV_M4A; }

void conv_apply_preset(ConvSettings *s, int p)
{
    const ConvPreset *c = &conv_presets[p];
    s->preset = p;
    if (c->format < 0)
        return;                         /* Custom: keep what's there */
    s->format = c->format;
    s->size = c->size;
    s->quality = c->quality;
    s->speed = c->speed;
    s->sound = c->sound;
    s->easy = c->easy;
}

/* "1:02:03.5", "2:13", "95" -> seconds; -1 if it isn't a time, 0 for "" */
double conv_parse_time(const char *t)
{
    double v = 0, part = 0, frac = 0, scale = 0.1;
    int colons = 0, digits = 0, dot = 0;
    while (*t == ' ') t++;
    if (!*t)
        return 0;
    for (; *t && *t != ' '; t++) {
        if (*t >= '0' && *t <= '9') {
            if (dot) { frac += (*t - '0') * scale; scale /= 10; }
            else part = part * 10 + (*t - '0');
            digits++;
        } else if (*t == ':' && !dot && digits && colons < 2) {
            v = (v + part) * 60;
            part = 0;
            digits = 0;
            colons++;
        } else if (*t == '.' && !dot && digits) {
            dot = 1;
        } else
            return -1;
    }
    if (!digits && !dot)
        return -1;
    if (colons && part >= 60)
        return -1;
    return v + part + frac;
}

void conv_format_time(char *buf, size_t n, double s)
{
    int t = s > 0 ? (int)(s + 0.5) : 0;
    if (t >= 3600)
        snprintf(buf, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

static void add(char *buf, size_t size, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void add(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (*n >= size)
        return;
    va_start(ap, fmt);
    k = vsnprintf(buf + *n, size - *n, fmt, ap);
    va_end(ap);
    *n += k > 0 ? (size_t)k : 0;
}

static const char *q(const char *name) { return strchr(name, ' ') ? "\"" : ""; }

/* ffmpeg's arguments for converting src to out. 0, or -1 with why in err. */
int conv_build_args(char *buf, size_t size, const ConvSettings *s, const ConvSource *in,
                    const char *src, const char *out, char *err, size_t errsize)
{
    size_t n = 0;
    double from = conv_parse_time(s->from), to = conv_parse_time(s->to);
    int sound_only = conv_sound_only(s->format);
    int video = !sound_only && in->has_video;
    int sound = in->has_audio && (s->sound || sound_only);

    buf[0] = 0;
    if (from < 0 || to < 0) {
        snprintf(err, errsize, "Trim: give times as h:mm:ss, m:ss or seconds (e.g. 1:30).");
        return -1;
    }
    if (in->duration > 0 && from >= in->duration) {
        snprintf(err, errsize, "Trim: the start is after the end of the file.");
        return -1;
    }
    if (to > 0 && to <= from) {
        snprintf(err, errsize, "Trim: the end must be after the start.");
        return -1;
    }
    if (sound_only && !in->has_audio) {
        snprintf(err, errsize, "This file has no sound to save.");
        return -1;
    }
    if (!sound_only && !in->has_video) {
        snprintf(err, errsize, "This file has no picture: choose a sound format (MP3 or M4A).");
        return -1;
    }
    add(buf, size, &n, "-nostdin -hide_banner -v error -y");
    if (from > 0)
        add(buf, size, &n, " -ss %.3f", from);
    if (to > 0)
        add(buf, size, &n, " -to %.3f", to);
    add(buf, size, &n, " -i %s%s%s", q(src), src, q(src));
    if (video) {
        char vf[160] = "";
        size_t v = 0;
        int lines = size_lines[s->size];
        add(buf, size, &n, " -map 0:v:0");
        if (sound)
            add(buf, size, &n, " -map 0:a:0");
        if (s->deinterlace)
            add(vf, sizeof(vf), &v, "yadif=deint=interlaced");
        if (lines && in->height > lines)
            add(vf, sizeof(vf), &v, "%sscale=-2:%d", v ? "," : "", lines);
        else if ((in->width | in->height) & 1)          /* H.264 needs even sizes */
            add(vf, sizeof(vf), &v, "%sscale=trunc(iw/2)*2:trunc(ih/2)*2", v ? "," : "");
        if (v)
            add(buf, size, &n, " -vf %s", vf);
        add(buf, size, &n, " -c:v libx264 -preset %s -crf %d%s -pix_fmt yuv420p",
            x264_preset[s->speed], crf[s->quality], s->easy ? " -tune fastdecode" : "");
    } else
        add(buf, size, &n, " -vn -map 0:a:0");
    if (sound) {
        if (s->format == CONV_MP3)
            add(buf, size, &n, " -c:a libmp3lame -q:a %d", mp3_q[s->quality]);
        else
            add(buf, size, &n, " -c:a aac -b:a %dk", aac_kbps[s->quality]);
        if (in->channels > 2 || s->format != CONV_MKV)
            add(buf, size, &n, " -ac 2");
    } else
        add(buf, size, &n, " -an");
    if (s->format == CONV_MP4 || s->format == CONV_M4A)
        add(buf, size, &n, " -movflags +faststart");
    add(buf, size, &n, " -progress pipe:1 -nostats %s%s%s", q(out), out, q(out));
    if (n >= size) {
        snprintf(err, errsize, "The file names are too long.");
        return -1;
    }
    return 0;
}

/* The output's name from the source's: its leaf without the extension,
   with a suffix saying what was done, and the format's extension
   ("film/mkv" -> "film_720/mp4"). */
void conv_output_leaf(char *buf, size_t size, const char *src, const ConvSettings *s, const ConvSource *in)
{
    char base[128], *ext;
    const char *leaf;
    int lines = size_lines[s->size];
    if (src[0] == '/') {                    /* a Unix name: dir/name.ext */
        leaf = strrchr(src, '/') + 1;
        snprintf(base, sizeof(base), "%s", leaf);
        if ((ext = strrchr(base, '.')) != NULL && ext != base) *ext = 0;
    } else {                                /* RISC OS: Disc::Name.$.dir.name/ext */
        leaf = strrchr(src, '.');
        if (!leaf) leaf = strrchr(src, ':');
        leaf = leaf ? leaf + 1 : src;
        snprintf(base, sizeof(base), "%s", leaf);
        if ((ext = strrchr(base, '/')) != NULL && ext != base) *ext = 0;
    }
    if (strlen(base) > 60)
        base[60] = 0;
    if (conv_sound_only(s->format))
        snprintf(buf, size, "%s/%s", base, conv_format_ext[s->format]);
    else if (lines && in->height > lines)
        snprintf(buf, size, "%s_%d/%s", base, lines, conv_format_ext[s->format]);
    else
        snprintf(buf, size, "%s_new/%s", base, conv_format_ext[s->format]);
}

/* ffprobe -of compact=p=0 -show_entries ... : one line per stream, and the
   format's duration: "codec_type=video|codec_name=h264|width=1280|..." */
void conv_parse_probe(ConvSource *in, const char *text)
{
    const char *line = text;
    memset(in, 0, sizeof(*in));
    while (*line) {
        char l[512], *field, *save = NULL;
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        ConvStream st;
        memset(&st, 0, sizeof(st));
        if (len >= sizeof(l)) len = sizeof(l) - 1;
        memcpy(l, line, len);
        l[len] = 0;
        line += len + (end ? 1 : 0);
        for (field = strtok_r(l, "|\r", &save); field; field = strtok_r(NULL, "|\r", &save)) {
            char *eq = strchr(field, '=');
            if (!eq) continue;
            *eq++ = 0;
            if (!strcmp(field, "codec_type")) snprintf(st.type, sizeof(st.type), "%s", eq);
            else if (!strcmp(field, "codec_name")) snprintf(st.codec, sizeof(st.codec), "%s", eq);
            else if (!strcmp(field, "width")) st.width = atoi(eq);
            else if (!strcmp(field, "height")) st.height = atoi(eq);
            else if (!strcmp(field, "channels")) st.channels = atoi(eq);
            else if (!strcmp(field, "sample_rate")) st.rate = atoi(eq);
            else if (!strcmp(field, "field_order")) st.interlaced = strcmp(eq, "progressive") && strcmp(eq, "unknown") && *eq;
            else if (!strcmp(field, "r_frame_rate")) {
                int a = 0, b = 1;
                if (sscanf(eq, "%d/%d", &a, &b) == 2 && b) st.fps = (double)a / b;
            } else if (!strcmp(field, "duration") && strcmp(eq, "N/A"))
                in->duration = atof(eq);
        }
        if (!strcmp(st.type, "video") && !in->has_video && strcmp(st.codec, "mjpeg") && strcmp(st.codec, "png")) {
            in->has_video = 1;              /* (a cover picture isn't a video) */
            in->width = st.width;
            in->height = st.height;
            in->fps = st.fps;
            in->interlaced = st.interlaced;
            snprintf(in->vcodec, sizeof(in->vcodec), "%s", st.codec);
        } else if (!strcmp(st.type, "audio") && !in->has_audio) {
            in->has_audio = 1;
            in->channels = st.channels;
            in->rate = st.rate;
            snprintf(in->acodec, sizeof(in->acodec), "%s", st.codec);
        }
    }
}

static const char *codec_label(const char *c)
{
    static const struct { const char *name, *label; } t[] = {
        { "h264", "H.264" }, { "hevc", "HEVC" }, { "mpeg2video", "MPEG-2" }, { "mpeg1video", "MPEG-1" },
        { "mpeg4", "MPEG-4" }, { "vp8", "VP8" }, { "vp9", "VP9" }, { "av1", "AV1" }, { "theora", "Theora" },
        { "aac", "AAC" }, { "mp3", "MP3" }, { "ac3", "AC-3" }, { "eac3", "E-AC-3" }, { "opus", "Opus" },
        { "vorbis", "Vorbis" }, { "flac", "FLAC" }, { "mp2", "MP2" }, { "dts", "DTS" }, { "alac", "ALAC" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(c, t[i].name))
            return t[i].label;
    return c;
}

/* "H.264, 1280x720, 25 fps, interlaced; AAC stereo; 2:13" */
void conv_describe(char *buf, size_t size, const ConvSource *in)
{
    size_t n = 0;
    char d[16];
    buf[0] = 0;
    if (in->has_video) {
        add(buf, size, &n, "%s, %dx%d", codec_label(in->vcodec), in->width, in->height);
        if (in->fps > 0)
            add(buf, size, &n, in->fps == (int)in->fps ? ", %.0f fps" : ", %.2f fps", in->fps);
        if (in->interlaced)
            add(buf, size, &n, ", interlaced");
    }
    if (in->has_audio)
        add(buf, size, &n, "%s%s %s", n ? "; " : "", codec_label(in->acodec),
            in->channels == 1 ? "mono" : in->channels == 2 ? "stereo" : in->channels == 6 ? "5.1" : "surround");
    if (!in->has_video && !in->has_audio)
        add(buf, size, &n, "No picture or sound FFmpeg can read");
    if (in->duration > 0) {
        conv_format_time(d, sizeof(d), in->duration);
        add(buf, size, &n, "; %s long", d);
    }
}

/* One of ffmpeg's -progress lines */
void conv_parse_progress(ConvProgress *p, const char *line)
{
    if (!strncmp(line, "out_time_us=", 12) || !strncmp(line, "out_time_ms=", 12)) {
        long long us = atoll(line + 12);
        if (us >= 0) p->out_time = us / 1e6;
    } else if (!strncmp(line, "speed=", 6)) {
        p->speed = atof(line + 6);
    } else if (!strncmp(line, "total_size=", 11)) {
        p->bytes = atoll(line + 11);
    } else if (!strncmp(line, "progress=", 9)) {
        p->ended = !strncmp(line + 9, "end", 3);
        p->updates++;
    } else if (*line && !strchr(line, '=')) {
        snprintf(p->error, sizeof(p->error), "%s", line);   /* ffmpeg's -v error messages */
    }
}

/* ---- the window ---------------------------------------------------------- */

/* icons */
enum {
    I_SRC_L, I_SRC, I_INFO,
    I_PRESET_L, I_PRESET, I_PRESET_B, I_PRESET_D,
    I_FORMAT_L, I_FORMAT, I_FORMAT_B, I_SIZE_L, I_SIZE, I_SIZE_B,
    I_QUAL_L, I_QUAL, I_QUAL_B, I_SPEED_L, I_SPEED, I_SPEED_B,
    I_SOUND, I_DEINT, I_EASY,
    I_TRIM_L, I_FROM, I_TO_L, I_TO, I_TRIM_H,
    I_FILE, I_NAME, I_CONVERT,
    I_BAR, I_FILL, I_STOP, I_STATUS,
    I_PLAY, I_SHOW, I_LOG,
    N_ICONS
};

#define IF_TEXT   0x1
#define IF_SPRITE 0x2
#define IF_BORDER 0x4
#define IF_HCENT  0x8
#define IF_VCENT  0x10
#define IF_FILLED 0x20
#define IF_INDIR  0x100
#define IF_RJUST  0x200
#define IF_BUTTON(t) ((t) << 12)
#define IF_SELECTED (1 << 21)
#define IF_SHADED   (1 << 22)
#define IF_COL(fg, bg) (((fg) << 24) | ((bg) << 28))

typedef struct { int x0, y0, x1, y1, flags; char *text; const char *valid; int len; } icon_t;

#define WIN_W 1100
#define WIN_H 832
#define BAR_X0 24
#define BAR_X1 880

static struct {
    int task, win, open;
    char ffdir[256];
    ConvSettings set;
    ConvSource in;
    ConvProgress prog;
    int state;                          /* CONV_* */
    int child, child_txt;               /* the task window running ffprobe/ffmpeg */
    char src[256], out[256];
    char probe_text[4096];
    size_t probe_n;
    char line[512];                     /* a partial output line */
    size_t line_n;
    FILE *log;
    int t0_cs;
    int fill_x1;
    int menu_icon;                      /* which popup's menu is open (-1: none) */
    int drag_ref;
    /* icon texts */
    char t_src[256], t_info[160], t_preset[40], t_preset_d[120], t_format[20], t_size[20],
         t_qual[20], t_speed[20], t_name[256], t_status[200], t_file[16];
    char t_from[16], t_to[16];
} C;

static char s_sound[] = "Sound", s_deint[] = "Deinterlace if needed", s_easy[] = "Quick to decode (for RISC OS)";
static char s_src_l[] = "Source", s_preset_l[] = "Convert to", s_format_l[] = "Format", s_size_l[] = "Size",
            s_qual_l[] = "Quality", s_speed_l[] = "Speed", s_trim_l[] = "From", s_to_l[] = "to",
            s_trim_h[] = "e.g. 1:30; empty: the start / the end", s_convert[] = "Convert", s_stop[] = "Stop",
            s_play[] = "Play", s_show[] = "Show", s_log[] = "Log", s_empty[] = "";
static char v_popup[] = "R5;Sgright,pgright", v_display[] = "R2", v_opt[] = "Soptoff,opton", v_default[] = "R6,3",
            v_action[] = "R5,3", v_write[] = "Pptr_write;Ktar;A~ ", v_time[] = "Pptr_write;Ktar;A0-9:.",
            v_label[] = "", v_fill[] = "", v_file[16];

static const char *const help[N_ICONS] = {
    [I_SRC_L] = "The file to convert.",
    [I_SRC] = "The file to convert: drag a video or sound file from a Filer window and drop it on this window.",
    [I_INFO] = "What's in the file: the picture's codec, size and rate, the sound, and how long it is.",
    [I_PRESET] = "What to make. Click the arrow for the choices. The options below show what it sets; change any "
                 "of them to make your own (Custom).",
    [I_PRESET_B] = "Click to choose what to make.",
    [I_PRESET_D] = "What the chosen preset makes.",
    [I_FORMAT] = "The kind of file to make: MP4 or MKV video, or only the sound as MP3 or M4A.",
    [I_FORMAT_B] = "Click to choose the kind of file.",
    [I_SIZE] = "The picture's height at most: a bigger picture is made smaller, keeping its shape (never larger).",
    [I_SIZE_B] = "Click to choose the picture's size.",
    [I_QUAL] = "Better quality makes a bigger file. Normal is hard to tell from the original.",
    [I_QUAL_B] = "Click to choose the quality.",
    [I_SPEED] = "How hard the encoder works: Balanced makes a smaller file for the same quality, but takes longer.",
    [I_SPEED_B] = "Click to choose how fast to convert.",
    [I_SOUND] = "Keep the sound (as AAC in MP4 or MKV). Untick for a silent video.",
    [I_DEINT] = "Remove the stripes of interlaced video (TV, DVD, camcorders). Only acts on interlaced pictures.",
    [I_EASY] = "Makes the video simpler to decode (no deblocking or CABAC), so it plays smoothly on RISC OS; "
               "the file is a little bigger.",
    [I_FROM] = "Convert only part of the file: start from this time (h:mm:ss, m:ss or seconds). Empty: from the start.",
    [I_TO] = "Stop at this time. Empty: to the end.",
    [I_FILE] = "Drag this to a directory display to put the new file there instead.",
    [I_NAME] = "Where the new file goes: next to the original to start with. Edit it, or drag the icon to "
               "another directory; Return converts.",
    [I_CONVERT] = "Click to convert into the file named on the left.",
    [I_BAR] = "How far the conversion has got.",
    [I_STOP] = "Click to stop converting. The unfinished file is deleted.",
    [I_STATUS] = "Where the conversion has got, how fast it goes, and about how long is left.",
    [I_PLAY] = "Click to play the new file with ffplay.",
    [I_SHOW] = "Click to open the directory with the new file.",
    [I_LOG] = "Click to read ffmpeg's messages, which say why it couldn't convert.",
};

static void icon_mk(icon_t *ic, int x0, int y0, int x1, int y1, int flags, char *text, const char *valid, int len)
{
    ic->x0 = x0; ic->y0 = y0; ic->x1 = x1; ic->y1 = y1;
    ic->flags = flags | IF_INDIR;
    ic->text = text; ic->valid = valid; ic->len = len;
}

#define LABEL(i, x0, y0, x1, y1, s) \
    icon_mk(&w.icon[i], x0, y0, x1, y1, IF_TEXT | IF_VCENT | IF_RJUST | IF_COL(7, 1), s, v_label, sizeof(s))
#define DISPLAY(i, x0, y0, x1, y1, buf) \
    icon_mk(&w.icon[i], x0, y0, x1, y1, IF_TEXT | IF_BORDER | IF_VCENT | IF_FILLED | IF_COL(7, 0), buf, v_display, sizeof(buf))
#define POPUP(i, x0, y0) \
    icon_mk(&w.icon[i], x0, y0, (x0) + 44, (y0) + 48, \
            IF_TEXT | IF_SPRITE | IF_BORDER | IF_FILLED | IF_HCENT | IF_VCENT | IF_BUTTON(3) | IF_COL(7, 1), s_empty, v_popup, 1)
#define OPTION(i, x0, y0, x1, s) \
    icon_mk(&w.icon[i], x0, y0, x1, (y0) + 44, IF_TEXT | IF_SPRITE | IF_VCENT | IF_BUTTON(11) | IF_COL(7, 1), s, v_opt, sizeof(s))
#define ACTION(i, x0, y0, x1, y1, s, v) \
    icon_mk(&w.icon[i], x0, y0, x1, y1, IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_BUTTON(3) | IF_COL(7, 1), s, v, sizeof(s))

static int create_window(void)
{
    struct {
        int vis[4];
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        int ext[4];
        int tflags, wbutton, sprites;
        short minw, minh;
        char *title; const char *tvalid; int tlen;
        int nicons;
        icon_t icon[N_ICONS];
    } w;
    static char title[] = "Convert";
    _kernel_swi_regs r;

    memset(&w, 0, sizeof(w));
    w.vis[2] = WIN_W; w.vis[3] = WIN_H;
    w.behind = -1;
    w.flags = (int)0x87000012u;         /* new format, back, close, title, moveable, auto-redraw */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext[1] = -WIN_H; w.ext[2] = WIN_W;
    w.tflags = IF_TEXT | IF_BORDER | IF_HCENT | IF_VCENT | IF_FILLED | IF_INDIR | IF_COL(7, 2);
    w.wbutton = 10 << 12;               /* (drops come as messages; clicks on the background: nothing) */
    w.sprites = 1;
    w.title = title; w.tvalid = (const char *)-1; w.tlen = sizeof(title);
    w.nicons = N_ICONS;

    /* 1. the source */
    LABEL(I_SRC_L, 16, -76, 184, -28, s_src_l);
    DISPLAY(I_SRC, 200, -76, WIN_W - 24, -28, C.t_src);
    icon_mk(&w.icon[I_INFO], 200, -120, WIN_W - 24, -80, IF_TEXT | IF_VCENT | IF_COL(4, 1), C.t_info, v_label, sizeof(C.t_info));
    /* 2. what to make */
    LABEL(I_PRESET_L, 16, -196, 184, -148, s_preset_l);
    DISPLAY(I_PRESET, 200, -196, 640, -148, C.t_preset);
    POPUP(I_PRESET_B, 644, -196);
    icon_mk(&w.icon[I_PRESET_D], 200, -240, WIN_W - 24, -200, IF_TEXT | IF_VCENT | IF_COL(4, 1), C.t_preset_d, v_label, sizeof(C.t_preset_d));
    LABEL(I_FORMAT_L, 16, -304, 184, -256, s_format_l);
    DISPLAY(I_FORMAT, 200, -304, 420, -256, C.t_format);
    POPUP(I_FORMAT_B, 424, -304);
    LABEL(I_SIZE_L, 480, -304, 620, -256, s_size_l);
    DISPLAY(I_SIZE, 636, -304, 856, -256, C.t_size);
    POPUP(I_SIZE_B, 860, -304);
    LABEL(I_QUAL_L, 16, -360, 184, -312, s_qual_l);
    DISPLAY(I_QUAL, 200, -360, 420, -312, C.t_qual);
    POPUP(I_QUAL_B, 424, -360);
    LABEL(I_SPEED_L, 480, -360, 620, -312, s_speed_l);
    DISPLAY(I_SPEED, 636, -360, 856, -312, C.t_speed);
    POPUP(I_SPEED_B, 860, -360);
    OPTION(I_SOUND, 200, -416, 360, s_sound);
    OPTION(I_DEINT, 380, -416, 780, s_deint);
    OPTION(I_EASY, 200, -468, 700, s_easy);
    LABEL(I_TRIM_L, 16, -532, 184, -484, s_trim_l);
    icon_mk(&w.icon[I_FROM], 200, -532, 360, -484, IF_TEXT | IF_BORDER | IF_VCENT | IF_FILLED | IF_BUTTON(15) | IF_COL(7, 0),
            C.t_from, v_time, sizeof(C.t_from));
    LABEL(I_TO_L, 364, -532, 424, -484, s_to_l);
    icon_mk(&w.icon[I_TO], 440, -532, 600, -484, IF_TEXT | IF_BORDER | IF_VCENT | IF_FILLED | IF_BUTTON(15) | IF_COL(7, 0),
            C.t_to, v_time, sizeof(C.t_to));
    icon_mk(&w.icon[I_TRIM_H], 616, -532, WIN_W - 24, -484, IF_TEXT | IF_VCENT | IF_COL(4, 1), s_trim_h, v_label, sizeof(s_trim_h));
    /* 3. saving */
    icon_mk(&w.icon[I_FILE], 24, -664, 132, -560, IF_TEXT | IF_SPRITE | IF_HCENT | IF_BUTTON(6) | IF_COL(7, 1),
            s_empty, v_file, 1);
    icon_mk(&w.icon[I_NAME], 148, -636, 860, -588, IF_TEXT | IF_BORDER | IF_VCENT | IF_FILLED | IF_BUTTON(15) | IF_COL(7, 0),
            C.t_name, v_write, sizeof(C.t_name));
    ACTION(I_CONVERT, 880, -644, WIN_W - 24, -580, s_convert, v_default);
    /* 4. progress */
    icon_mk(&w.icon[I_BAR], BAR_X0, -716, BAR_X1, -684, IF_TEXT | IF_BORDER | IF_FILLED | IF_COL(7, 0), s_empty, "R2", 1);
    icon_mk(&w.icon[I_FILL], BAR_X0 + 4, -712, BAR_X0 + 4, -688, IF_TEXT | IF_FILLED | IF_COL(7, 10), s_empty, v_fill, 1);
    ACTION(I_STOP, 896, -724, WIN_W - 24, -676, s_stop, v_action);
    icon_mk(&w.icon[I_STATUS], 24, -772, WIN_W - 24, -732, IF_TEXT | IF_VCENT | IF_COL(7, 1), C.t_status, v_label, sizeof(C.t_status));
    ACTION(I_PLAY, 24, -824, 184, -776, s_play, v_action);
    ACTION(I_SHOW, 200, -824, 360, -776, s_show, v_action);
    ACTION(I_LOG, 376, -824, 536, -776, s_log, v_action);

    r.r[1] = (intptr_t)&w;
    if (swi(Wimp_CreateWindow, &r))
        return -1;
    C.win = r.r[0];
    return 0;
}

static void icon_flags(int i, int eor, int clear)
{
    int b[4] = { C.win, i, eor, clear };
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)b;
    swi(Wimp_SetIconState, &r);
}
static void refresh(int i) { icon_flags(i, 0, 0); }
static void shade(int i, int on) { icon_flags(i, on ? IF_SHADED : 0, IF_SHADED); }
static void select_icon(int i, int on) { icon_flags(i, on ? IF_SELECTED : 0, IF_SELECTED); }

static int selected(int i)
{
    int b[10];
    _kernel_swi_regs r;
    b[0] = C.win;
    b[1] = i;
    r.r[1] = (intptr_t)b;
    if (swi(Wimp_GetIconState, &r))
        return 0;
    return (b[6] & IF_SELECTED) != 0;
}

static void resize_icon(int i, int x0, int y0, int x1, int y1)
{
    _kernel_swi_regs r;
    r.r[0] = C.win; r.r[1] = i; r.r[2] = x0; r.r[3] = y0; r.r[4] = x1; r.r[5] = y1;
    swi(0x400FC /* Wimp_ResizeIcon */, &r);
    r.r[0] = C.win; r.r[1] = BAR_X0; r.r[2] = y0; r.r[3] = BAR_X1; r.r[4] = y1;
    swi(0x400D1 /* Wimp_ForceRedraw */, &r);
}

static void set_text(int i, char *buf, size_t size, const char *text)
{
    if (strcmp(buf, text)) {
        snprintf(buf, size, "%s", text);
        refresh(i);
    }
}
#define SET(i, buf, text) set_text(i, buf, sizeof(buf), text)

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status(const char *fmt, ...)
{
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    SET(I_STATUS, C.t_status, t);
}

static void report(const char *text, int flags)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = flags;
    r.r[2] = (intptr_t)"Convert";
    swi(Wimp_ReportError, &r);
}

/* A question with our own buttons ("Replace", "Stop"...) and Cancel:
   1 if the first was chosen */
static int ask(const char *text, const char *buttons)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = 2 | (1 << 8) | (4 << 9);   /* Cancel, new style, a question */
    r.r[2] = (intptr_t)"Convert";
    r.r[3] = (intptr_t)"!ffmpeg";
    r.r[4] = 1;
    r.r[5] = (intptr_t)buttons;
    if (swi(Wimp_ReportError, &r))
        return 0;
    return r.r[1] == 3;
}

static void progress_bar(double frac)
{
    int x1 = BAR_X0 + 4 + (int)((BAR_X1 - BAR_X0 - 8) * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
    if (x1 != C.fill_x1) {
        C.fill_x1 = x1;
        resize_icon(I_FILL, BAR_X0 + 4, -712, x1, -688);
    }
}

/* The file icon's sprite for the format's file type */
static int format_filetype(int format)
{
    _kernel_swi_regs r;
    r.r[0] = 3;                         /* from an extension */
    r.r[1] = (intptr_t)conv_format_ext[format];
    r.r[2] = 0;                         /* to a file type */
    if (swi(MimeMap_Translate, &r))
        return 0xFFD;
    return r.r[3];
}

/* Is there a sprite of this name in the Wimp's pool? (Wimp_SpriteOp 40) */
static int wimp_sprite(const char *name)
{
    _kernel_swi_regs r;
    r.r[0] = 40;
    r.r[2] = (intptr_t)name;
    return !swi(0x400E9 /* Wimp_SpriteOp */, &r);
}

/* Everything that shows the settings, and what can be used now */
static void show_settings(void)
{
    int busy = conv_busy();
    int have = C.src[0] && C.state != CONV_PROBING && C.state != CONV_NONE;
    int sound_only = conv_sound_only(C.set.format);
    int video_opts = have && !busy && !sound_only && C.in.has_video;
    char leaf[256];
    snprintf(leaf, sizeof(leaf), "file_%03x", format_filetype(C.set.format));
    if (!wimp_sprite(leaf))             /* a type with no icon: the Filer's "?" */
        snprintf(leaf, sizeof(leaf), "file_xxx");
    if (strcmp(v_file + 1, leaf)) {
        snprintf(v_file, sizeof(v_file), "S%s", leaf);
        refresh(I_FILE);
    }
    SET(I_PRESET, C.t_preset, conv_presets[C.set.preset].name);
    SET(I_PRESET_D, C.t_preset_d, conv_presets[C.set.preset].desc);
    SET(I_FORMAT, C.t_format, conv_format_names[C.set.format]);
    SET(I_SIZE, C.t_size, conv_size_names[C.set.size]);
    SET(I_QUAL, C.t_qual, conv_quality_names[C.set.quality]);
    SET(I_SPEED, C.t_speed, conv_speed_names[C.set.speed]);
    select_icon(I_SOUND, (C.set.sound || sound_only) && (!have || C.in.has_audio));
    select_icon(I_DEINT, C.set.deinterlace);
    select_icon(I_EASY, C.set.easy);
    for (int i = I_PRESET; i <= I_FORMAT_B; i++)
        shade(i, !have || busy);
    shade(I_PRESET_L, !have || busy);
    shade(I_FORMAT_L, !have || busy);
    shade(I_QUAL, !have || busy); shade(I_QUAL_B, !have || busy); shade(I_QUAL_L, !have || busy);
    for (int i = I_SIZE_L; i <= I_SIZE_B; i++) shade(i, !video_opts);
    for (int i = I_SPEED_L; i <= I_SPEED_B; i++) shade(i, !video_opts);
    shade(I_DEINT, !video_opts);
    shade(I_EASY, !video_opts);
    shade(I_SOUND, !have || busy || sound_only || !C.in.has_audio || !C.in.has_video);
    for (int i = I_TRIM_L; i <= I_TRIM_H; i++) shade(i, !have || busy);
    shade(I_FILE, !have || busy);
    shade(I_NAME, !have || busy);
    shade(I_CONVERT, !have || busy);
    shade(I_STOP, C.state != CONV_RUNNING);
    shade(I_PLAY, C.state != CONV_DONE);
    shade(I_SHOW, C.state != CONV_DONE);
    shade(I_LOG, C.state != CONV_FAILED && C.state != CONV_DONE && C.state != CONV_STOPPED);
    shade(I_BAR, C.state != CONV_RUNNING && C.state != CONV_DONE);
}

/* The output's full path, from the source and the settings: at first in
   the source's own directory (so Convert works at once), then wherever it
   was saved or typed. Only the leaf follows the settings. */
static void suggest_name(void)
{
    char leaf[200], name[256];
    const char *base = C.t_name[0] ? C.t_name : C.src;
    const char *dot = strrchr(base, base[0] == '/' ? '/' : '.');
    if (!C.src[0])
        return;
    conv_output_leaf(leaf, sizeof(leaf), C.src, &C.set, &C.in);
    if (dot)
        snprintf(name, sizeof(name), "%.*s%c%s", (int)(dot - base), base, *dot, leaf);
    else
        snprintf(name, sizeof(name), "%s", leaf);
    SET(I_NAME, C.t_name, name);
}

void conv_init(int task, const char *ffdir)
{
    memset(&C, 0, sizeof(C));
    C.task = task;
    C.menu_icon = -1;
    C.fill_x1 = BAR_X0 + 4;
    snprintf(C.ffdir, sizeof(C.ffdir), "%s", ffdir);
    conv_apply_preset(&C.set, 0);
    C.set.deinterlace = 1;
    snprintf(C.t_src, sizeof(C.t_src), "Drop a video or sound file here");
    snprintf(C.t_status, sizeof(C.t_status), "Drop a video or sound file on this window to convert it.");
}

int conv_window(void) { return C.win; }
int conv_busy(void) { return C.state == CONV_RUNNING || C.state == CONV_PROBING || C.state == CONV_STOPPING; }

void conv_open(void)
{
    int b[9];
    _kernel_swi_regs r;
    if (!C.win) {
        if (create_window() < 0) {
            report("Can't create the Convert window.", 1);
            return;
        }
        show_settings();
    }
    b[0] = C.win;
    r.r[1] = (intptr_t)b;
    swi(Wimp_GetWindowState, &r);
    if (!C.open) {                      /* centred the first time */
        b[1] = 400; b[2] = 200; b[3] = 400 + WIN_W; b[4] = 200 + WIN_H;
        {
            _kernel_swi_regs m;
            int sw = 0, sh = 0;
            m.r[0] = -1; m.r[1] = 11; if (!swi(0x35, &m)) sw = m.r[2] + 1;
            m.r[0] = -1; m.r[1] = 4;  if (!swi(0x35, &m)) sw <<= m.r[2];
            m.r[0] = -1; m.r[1] = 12; if (!swi(0x35, &m)) sh = m.r[2] + 1;
            m.r[0] = -1; m.r[1] = 5;  if (!swi(0x35, &m)) sh <<= m.r[2];
            if (sw > WIN_W && sh > WIN_H) {
                b[1] = (sw - WIN_W) / 2; b[3] = b[1] + WIN_W;
                b[2] = (sh - WIN_H) / 2; b[4] = b[2] + WIN_H;
            }
        }
        b[5] = 0; b[6] = 0;
    }
    b[7] = -1;
    swi(Wimp_OpenWindow, &r);
    C.open = 1;
}

static void close_window(void)
{
    _kernel_swi_regs r;
    r.r[1] = (intptr_t)&C.win;
    swi(Wimp_CloseWindow, &r);
    C.open = 0;
}

/* ---- running ffprobe and ffmpeg in task windows ---------------------------- */

static void scrap_path(char *buf, size_t n, const char *leaf)
{
    const char *scrap = getenv("Wimp$ScrapDir");
    snprintf(buf, n, "%s" DIRSEP "ffmpeg" DIRSEP "%s", scrap && *scrap ? scrap : "", leaf);
}

/* Writes "PROGRAM ARGS" to an Obey file and runs it in a task window whose
   output comes to us. 0, or -1 (reported). */
static int run(const char *program, const char *args, int txt, const char *obey_leaf)
{
    char path[300], cmd[600], dir[300];
    FILE *f;
    _kernel_swi_regs r;
    _kernel_oserror *e;
    const char *scrap = getenv("Wimp$ScrapDir");
    if (!scrap || !*scrap) {
        report("<Wimp$ScrapDir> isn't set, so FFmpeg can't run in the background.", 1);
        return -1;
    }
    snprintf(dir, sizeof(dir), "%s" DIRSEP "ffmpeg", scrap);
    r.r[0] = 8; r.r[1] = (intptr_t)dir; r.r[4] = 0;     /* OS_File 8: the directory */
    swi(OS_File, &r);
    scrap_path(path, sizeof(path), obey_leaf);
    if (!(f = fopen(path, "w"))) {
        report("Can't write to <Wimp$ScrapDir>.", 1);
        return -1;
    }
    fprintf(f, "| Run by !FFmpeg's Convert window\n%s.%s %s\n", C.ffdir, program, args);
    fclose(f);
    r.r[0] = 18; r.r[1] = (intptr_t)path; r.r[2] = 0xFEB;   /* an Obey file */
    swi(OS_File, &r);
    snprintf(cmd, sizeof(cmd), "TaskWindow \"Obey %s\" -wimpslot 45056K -name \"FFmpeg convert\" -task &%x -txt &%x -quit",
             path, C.task, txt);
    C.child = 0;
    C.child_txt = txt;
    C.line_n = 0;
    if ((e = fffront_start_task(cmd)) != NULL) {
        report(e->errmess, 1);
        return -1;
    }
    return 0;
}

static void stop_child(void)
{
    int b[5];
    _kernel_swi_regs r;
    if (!C.child)
        return;
    b[0] = 20; b[3] = 0; b[4] = MSG_TW_MORITE;
    r.r[0] = 17; r.r[1] = (intptr_t)b; r.r[2] = C.child;
    swi(Wimp_SendMessage, &r);
}

static void set_source(const char *file)
{
    char args[400];
    if (conv_busy()) {
        report("Wait until this conversion has finished (or stop it) before choosing another file.", 1 | 16);
        return;
    }
    snprintf(C.src, sizeof(C.src), "%s", file);
    C.t_name[0] = 0;                    /* the new file goes next to it, until saved elsewhere */
    {                                   /* its name; the line under it says what's in it */
        const char *leaf = strrchr(file, file[0] == '/' ? '/' : '.');
        SET(I_SRC, C.t_src, leaf ? leaf + 1 : file);
    }
    SET(I_INFO, C.t_info, "Reading the file...");
    C.probe_n = 0;
    C.probe_text[0] = 0;
    C.state = CONV_PROBING;
    status(" ");
    progress_bar(0);
    show_settings();
    snprintf(args, sizeof(args), "-v error -of compact=p=0 -show_entries format=duration:"
             "stream=codec_type,codec_name,width,height,r_frame_rate,field_order,channels,sample_rate %s%s%s",
             q(file), file, q(file));
    if (run("ffprobe", args, TXT_PROBE, "Probe") < 0) {
        C.state = CONV_NONE;
        SET(I_INFO, C.t_info, "Couldn't start ffprobe.");
        show_settings();
    }
}

static void probed(void)
{
    char d[160];
    conv_parse_probe(&C.in, C.probe_text);
    conv_describe(d, sizeof(d), &C.in);
    SET(I_INFO, C.t_info, d);
    if (!C.in.has_video && !C.in.has_audio) {
        C.state = CONV_NONE;
        status("FFmpeg can't read this file: choose another.");
    } else {
        C.state = CONV_READY;
        if (!C.in.has_video && !conv_sound_only(C.set.format))
            conv_apply_preset(&C.set, 3);            /* sound only: MP3 */
        suggest_name();
        status("Choose what to make, then click Convert: the new file goes next to the original.");
    }
    show_settings();
}

/* Keeps the typed trim times in the settings */
void conv_sync(void)
{
    snprintf(C.set.from, sizeof(C.set.from), "%s", C.t_from);
    snprintf(C.set.to, sizeof(C.set.to), "%s", C.t_to);
}

/* The conversion to out (a full path): after asking before replacing */
static void start(const char *out)
{
    char args[1200], err[200];
    _kernel_swi_regs r;
    conv_sync();
    if (!strcmp(out, C.src)) {
        report("That's the file being converted: save the new one under another name.", 1 | 16);
        return;
    }
    if (conv_build_args(args, sizeof(args), &C.set, &C.in, C.src, out, err, sizeof(err)) < 0) {
        report(err, 1 | 16);
        return;
    }
    r.r[0] = 17; r.r[1] = (intptr_t)out;           /* OS_File 17: is it there? */
    if (!swi(OS_File, &r) && r.r[0] == 1) {
        char msg[300], leaf[128];
        const char *l = strrchr(out, '.');
        snprintf(leaf, sizeof(leaf), "%s", l ? l + 1 : out);
        snprintf(msg, sizeof(msg), "%s is already there. Replace it?", leaf);
        if (!ask(msg, "Replace"))
            return;
    }
    snprintf(C.out, sizeof(C.out), "%s", out);
    SET(I_NAME, C.t_name, out);             /* Convert again: the same place */
    memset(&C.prog, 0, sizeof(C.prog));
    if (C.log)
        fclose(C.log);
    {
        char lp[300];
        scrap_path(lp, sizeof(lp), "ConvertLog");
        C.log = fopen(lp, "w");
        if (C.log)
            fprintf(C.log, "%s.ffmpeg %s\n\n", C.ffdir, args);
    }
    C.state = CONV_RUNNING;
    r.r[0] = 0;
    swi(OS_ReadMonotonicTime, &r);
    C.t0_cs = r.r[0];
    progress_bar(0);
    status("Starting ffmpeg...");
    show_settings();
    if (run("ffmpeg", args, TXT_CONVERT, "Convert") < 0) {
        C.state = CONV_FAILED;
        status("Couldn't start ffmpeg.");
        show_settings();
    }
}

static void show_progress(void)
{
    char at[16], total[16], left[16];
    double dur = C.in.duration, from = conv_parse_time(C.set.from), to = conv_parse_time(C.set.to);
    if (to > 0 && (dur <= 0 || to < dur)) dur = to;
    dur -= from > 0 ? from : 0;
    conv_format_time(at, sizeof(at), C.prog.out_time);
    if (dur > 0) {
        conv_format_time(total, sizeof(total), dur);
        progress_bar(C.prog.out_time / dur);
        if (C.prog.speed > 0) {
            conv_format_time(left, sizeof(left), (dur - C.prog.out_time) / C.prog.speed);
            status("Converting: %s of %s, %.2gx real time, about %s left", at, total, C.prog.speed, left);
        } else
            status("Converting: %s of %s", at, total);
    } else
        status("Converting: %s so far", at);
}

static void finished(void)
{
    _kernel_swi_regs r;
    int took;
    char t[16];
    r.r[0] = 0;
    swi(OS_ReadMonotonicTime, &r);
    took = r.r[0] - C.t0_cs;
    conv_format_time(t, sizeof(t), took / 100.0);
    if (C.log) {
        fclose(C.log);
        C.log = NULL;
    }
    if (C.state == CONV_STOPPING) {
        r.r[0] = 6; r.r[1] = (intptr_t)C.out;      /* delete the unfinished file */
        swi(OS_File, &r);
        C.state = CONV_STOPPED;
        progress_bar(0);
        status("Stopped. The unfinished file was deleted.");
    } else if (C.prog.ended && !C.prog.error[0]) {
        const char *l = strrchr(C.out, '.');
        r.r[0] = 18; r.r[1] = (intptr_t)C.out; r.r[2] = format_filetype(C.set.format);
        swi(OS_File, &r);
        C.state = CONV_DONE;
        progress_bar(1);
        status("Done: %s, %.1f MB, in %s.", l ? l + 1 : C.out, C.prog.bytes / 1048576.0, t);
    } else {
        C.state = CONV_FAILED;
        progress_bar(0);
        status("Couldn't convert: %s", C.prog.error[0] ? C.prog.error : "ffmpeg stopped early (see Log).");
    }
    show_settings();
}

/* Output from a task window: whole lines */
static void output(const char *data, int n)
{
    for (int i = 0; i < n; i++) {
        char c = data[i];
        if (C.child_txt == TXT_PROBE) {
            if (C.probe_n < sizeof(C.probe_text) - 1) {
                C.probe_text[C.probe_n++] = c;
                C.probe_text[C.probe_n] = 0;
            }
            continue;
        }
        if (C.log)
            fputc(c, C.log);
        if (c == '\n' || c == '\r') {
            C.line[C.line_n] = 0;
            if (C.line_n) {
                conv_parse_progress(&C.prog, C.line);
                if (!strncmp(C.line, "progress=", 9) && C.state == CONV_RUNNING)
                    show_progress();
            }
            C.line_n = 0;
        } else if (C.line_n < sizeof(C.line) - 1)
            C.line[C.line_n++] = c;
    }
}

/* ---- the Wimp's events ------------------------------------------------------ */

/* popup menus */
typedef struct { int flags, sub, iflags; char *text; const char *valid; int len; } item_t;
static struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    item_t item[8];
} menu;

static void popup(int ic)
{
    const char *const *names;
    int n, cur;
    int b[10];
    _kernel_swi_regs r;
    switch (ic) {
    case I_PRESET_B: n = CONV_PRESETS; cur = C.set.preset; names = NULL; break;
    case I_FORMAT_B: n = CONV_FORMATS; cur = C.set.format; names = conv_format_names; break;
    case I_SIZE_B:   n = CONV_SIZES; cur = C.set.size; names = conv_size_names; break;
    case I_QUAL_B:   n = CONV_QUALITIES; cur = C.set.quality; names = conv_quality_names; break;
    case I_SPEED_B:  n = CONV_SPEEDS; cur = C.set.speed; names = conv_speed_names; break;
    default: return;
    }
    memset(&menu, 0, sizeof(menu));
    snprintf(menu.title, sizeof(menu.title), "%s", ic == I_PRESET_B ? "Convert to" : ic == I_FORMAT_B ? "Format" :
             ic == I_SIZE_B ? "Size" : ic == I_QUAL_B ? "Quality" : "Speed");
    menu.tfg = 7; menu.tbg = 2; menu.wfg = 7; menu.wbg = 0;
    menu.width = 12 * 16 + 32;
    menu.height = 44;
    for (int i = 0; i < n; i++) {
        const char *t = names ? names[i] : conv_presets[i].name;
        int w = (int)strlen(t) * 16 + 48;
        menu.item[i].flags = (i == cur ? 1 : 0) | (i == n - 1 ? 0x80 : 0) | (ic == I_PRESET_B && i == CONV_PRESETS - 2 ? 2 : 0);
        menu.item[i].sub = -1;
        menu.item[i].iflags = 0x07000021 | IF_INDIR;
        menu.item[i].text = (char *)t;
        menu.item[i].valid = (const char *)-1;
        menu.item[i].len = (int)strlen(t) + 1;
        if (ic == I_FORMAT_B && conv_sound_only(i) != 0 && !C.in.has_audio)
            menu.item[i].iflags |= IF_SHADED;
        if (ic == I_FORMAT_B && !conv_sound_only(i) && !C.in.has_video)
            menu.item[i].iflags |= IF_SHADED;
        if (ic == I_PRESET_B && i < CONV_PRESETS - 1 &&
            ((conv_sound_only(conv_presets[i].format) && !C.in.has_audio) ||
             (!conv_sound_only(conv_presets[i].format) && !C.in.has_video)))
            menu.item[i].iflags |= IF_SHADED;
        if (w > menu.width) menu.width = w;
    }
    /* to the right of the button, as RISC OS popups are */
    b[0] = C.win;
    r.r[1] = (intptr_t)b;
    swi(Wimp_GetWindowState, &r);
    C.menu_icon = ic;
    r.r[1] = (intptr_t)&menu;
    {
        int bx = b[1] - b[5], by = b[4] - b[6];
        int x1 = ic == I_PRESET_B ? 688 : ic == I_FORMAT_B || ic == I_QUAL_B ? 468 : 904;
        int y1 = ic == I_PRESET_B ? -148 : ic == I_FORMAT_B || ic == I_SIZE_B ? -256 : -312;
        r.r[2] = bx + x1;
        r.r[3] = by + y1;
    }
    swi(Wimp_CreateMenu, &r);
}

int conv_menu_open(void) { return C.menu_icon >= 0; }
void conv_menu_forget(void) { C.menu_icon = -1; }

void conv_menu_select(const int *sel)
{
    int ic = C.menu_icon, i = sel[0];
    C.menu_icon = -1;
    if (i < 0)
        return;
    switch (ic) {
    case I_PRESET_B: conv_apply_preset(&C.set, i); break;
    case I_FORMAT_B: C.set.format = i; C.set.preset = CONV_PRESETS - 1; break;
    case I_SIZE_B:   C.set.size = i; C.set.preset = CONV_PRESETS - 1; break;
    case I_QUAL_B:   C.set.quality = i; C.set.preset = CONV_PRESETS - 1; break;
    case I_SPEED_B:  C.set.speed = i; C.set.preset = CONV_PRESETS - 1; break;
    }
    suggest_name();
    show_settings();
}

static void drag_file(int *mouse)
{
    int b[10], box[4];
    _kernel_swi_regs r;
    (void)mouse;
    b[0] = C.win;
    b[1] = I_FILE;
    r.r[1] = (intptr_t)b;
    swi(Wimp_GetIconState, &r);
    {
        int wb[9];
        wb[0] = C.win;
        r.r[1] = (intptr_t)wb;
        swi(Wimp_GetWindowState, &r);
        box[0] = wb[1] - wb[5] + b[2]; box[1] = wb[4] - wb[6] + b[3];
        box[2] = wb[1] - wb[5] + b[4]; box[3] = wb[4] - wb[6] + b[5];
    }
    r.r[0] = 0x85;                      /* centred, bounded by the screen, drop shadow */
    r.r[1] = 1;                         /* the Wimp's sprites */
    r.r[2] = (intptr_t)(v_file + 1);
    r.r[3] = (intptr_t)box;
    if (swi(DragASprite_Start, &r)) {   /* no DragASprite: a dashed box */
        int d[10] = { C.win, 5, box[0], box[1], box[2], box[3], -0x7FFFFFFF, -0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF };
        r.r[1] = (intptr_t)d;
        swi(Wimp_DragBox, &r);
    }
    C.drag_ref = 1;
}

/* The drag ended: offer the file to whatever it was dropped on (Message_DataSave) */
void conv_drag_end(void)
{
    int p[5], m[64];
    _kernel_swi_regs r;
    const char *leaf;
    if (!C.drag_ref)
        return;
    C.drag_ref = 0;
    swi(DragASprite_Stop, &r);
    r.r[1] = (intptr_t)p;
    swi(Wimp_GetPointerInfo, &r);
    if (p[3] == C.win || (p[3] < 0 && p[3] != -2))
        return;                         /* on our own window, or nothing */
    leaf = strrchr(C.t_name, '.');
    leaf = leaf ? leaf + 1 : C.t_name;
    memset(m, 0, sizeof(m));
    m[3] = 0;
    m[4] = MSG_DATASAVE;
    m[5] = p[3]; m[6] = p[4]; m[7] = p[0]; m[8] = p[1];
    m[9] = (int)(C.in.duration > 0 ? C.in.duration * 150000 : 1 << 20);   /* a guess at the size */
    m[10] = format_filetype(C.set.format);
    snprintf((char *)&m[11], 212, "%s", leaf);
    m[0] = (44 + (int)strlen((char *)&m[11]) + 1 + 3) & ~3;
    r.r[0] = 17; r.r[1] = (intptr_t)m; r.r[2] = p[3]; r.r[3] = p[4];
    swi(Wimp_SendMessage, &r);
}

static void convert_named(void)
{
    if (!strchr(C.t_name, '.') && !strchr(C.t_name, ':')) {
        report("The name needs a directory: drag the file icon to a directory display, "
               "or type a full path (e.g. SDFS::Pi.$.Films.new/mp4).", 1 | 16);
        return;
    }
    start(C.t_name);
}

static void filer_run(const char *what)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "Filer_Run %s", what);
    fffront_start_task(cmd);
}

int conv_click(const int *b)
{
    if (b[3] != C.win)
        return 0;
    if (b[2] & 2)                       /* Menu: nothing of its own */
        return 1;
    switch (b[4]) {
    case I_PRESET_B: case I_FORMAT_B: case I_SIZE_B: case I_QUAL_B: case I_SPEED_B:
    case I_PRESET: case I_FORMAT: case I_SIZE: case I_QUAL: case I_SPEED: {
        int ic = b[4];
        if (ic == I_PRESET || ic == I_FORMAT || ic == I_SIZE || ic == I_QUAL || ic == I_SPEED)
            ic++;                       /* the display field opens its menu too */
        if (!conv_busy() && C.state != CONV_NONE)
            popup(ic);
        break;
    }
    case I_SOUND: C.set.sound = selected(I_SOUND); C.set.preset = CONV_PRESETS - 1; show_settings(); break;
    case I_DEINT: C.set.deinterlace = selected(I_DEINT); show_settings(); break;
    case I_EASY:  C.set.easy = selected(I_EASY); C.set.preset = CONV_PRESETS - 1; show_settings(); break;
    case I_FILE:
        if ((b[2] & (64 | 16)) && C.state != CONV_NONE && !conv_busy())
            drag_file((int *)b);
        break;
    case I_CONVERT: if (C.state != CONV_NONE && !conv_busy()) convert_named(); break;
    case I_STOP:
        if (C.state == CONV_RUNNING) {
            C.state = CONV_STOPPING;
            status("Stopping...");
            stop_child();
        }
        break;
    case I_PLAY: if (C.state == CONV_DONE) fffront_play(C.out); break;
    case I_SHOW:
        if (C.state == CONV_DONE) {
            char dir[256], *dot;
            snprintf(dir, sizeof(dir), "%s", C.out);
            if ((dot = strrchr(dir, '.')) != NULL) *dot = 0;
            filer_run(dir);
        }
        break;
    case I_LOG: {
        char lp[300];
        scrap_path(lp, sizeof(lp), "ConvertLog");
        filer_run(lp);
        break;
    }
    }
    return 1;
}

int conv_key(const int *b)
{
    _kernel_swi_regs r;
    if (b[0] != C.win)
        return 0;
    if (b[6] == 13 && C.state != CONV_NONE && !conv_busy()) {   /* Return in the last writable */
        convert_named();
        return 1;
    }
    if (b[6] == 27 && !conv_busy()) {
        close_window();
        return 1;
    }
    r.r[0] = b[6];
    swi(Wimp_ProcessKey, &r);
    return 1;
}

int conv_close_request(const int *b)
{
    if (b[0] != C.win)
        return 0;
    if (C.state == CONV_RUNNING) {
        if (!ask("A file is being converted. Stop converting it?", "Stop"))
            return 1;
        C.state = CONV_STOPPING;
        stop_child();
    }
    close_window();
    return 1;
}

static void reply(int *b, int action, const char *text)
{
    _kernel_swi_regs r;
    int to = b[1];
    b[3] = b[2];
    b[4] = action;
    snprintf((char *)&b[5], 236, "%s", text);
    b[0] = (20 + (int)strlen(text) + 1 + 3) & ~3;
    r.r[0] = 17; r.r[1] = (intptr_t)b; r.r[2] = to;
    swi(Wimp_SendMessage, &r);
}

/* Messages: 1 if it was ours */
int conv_message(int *b)
{
    switch (b[4]) {
    case MSG_DATALOAD:
        if (!C.win || b[5] != C.win)
            return 0;
        {
            char file[256];
            int type = b[10];
            snprintf(file, sizeof(file), "%s", (const char *)&b[11]);
            b[3] = b[2];
            b[4] = MSG_DATALOADACK;
            {
                _kernel_swi_regs r;
                r.r[0] = 17; r.r[1] = (intptr_t)b; r.r[2] = b[1];
                swi(Wimp_SendMessage, &r);
            }
            if (type == 0x1000 || type == 0x2000)
                report("That's a directory: drop a video or sound file.", 1 | 16);
            else
                set_source(file);
        }
        return 1;
    case MSG_DATASAVEACK:
        if (!C.win)
            return 0;
        if (b[9] == -1)                     /* an application, not a directory */
            report("Drag the file icon to a directory display: the new file is written there.", 1 | 16);
        else
            start((const char *)&b[11]);
        return 1;
    case MSG_HELPREQUEST:
        if (!C.win || b[8] != C.win)
            return 0;
        {
            int ic = b[9];
            const char *t = ic >= 0 && ic < N_ICONS && help[ic] ? help[ic] :
                "The Convert window: drop a file on it, choose what to make, then drag the file icon to a directory.";
            if (ic == I_SRC_L || ic == I_PRESET_L || ic == I_FORMAT_L || ic == I_SIZE_L || ic == I_QUAL_L ||
                ic == I_SPEED_L || ic == I_TRIM_L || ic == I_TO_L)
                t = help[ic + 1] ? help[ic + 1] : t;
            reply(b, MSG_HELPREPLY, t);
        }
        return 1;
    case MSG_TW_EGO:
        if (b[5] != C.child_txt)
            return 0;
        C.child = b[1];
        return 1;
    case MSG_TW_OUTPUT:
        if (!C.child || b[1] != C.child)
            return 0;
        output((const char *)&b[6], b[5]);
        return 1;
    case MSG_TW_MORIO:
        if (!C.child || b[1] != C.child)
            return 0;
        C.child = 0;
        if (C.line_n) {                 /* the last line, if unfinished */
            output("\n", 1);
        }
        if (C.child_txt == TXT_PROBE)
            probed();
        else
            finished();
        return 1;
    case MSG_PREQUIT:
        if (C.state != CONV_RUNNING)
            return 0;
        if (!ask("A file is being converted. Stop converting it and quit?", "Stop")) {
            _kernel_swi_regs r;         /* object: acknowledge the message */
            int to = b[1];
            b[3] = b[2];
            r.r[0] = 19; r.r[1] = (intptr_t)b; r.r[2] = to;
            swi(Wimp_SendMessage, &r);
        } else
            stop_child();
        return 1;
    }
    return 0;
}

#ifdef CONV_TEST
int conv_test_state(void) { return C.state; }
const char *conv_test_text(int which)
{
    switch (which) {
    case 0: return C.t_info;
    case 1: return C.t_status;
    case 2: return C.t_name;
    case 3: return C.t_preset;
    case 4: return C.t_size;
    case 5: return C.t_format;
    case 6: return v_file;
    }
    return "";
}
char *conv_test_buffer(int which) { return which == 0 ? C.t_name : which == 1 ? C.t_from : C.t_to; }
int conv_test_fill(void) { return C.fill_x1; }
#endif
