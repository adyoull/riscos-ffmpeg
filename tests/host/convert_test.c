/*
 * Host test of !FFmpeg's Convert window (frontend/convert.c, through
 * fffront.c's poll loop) with a scripted fake Wimp and fake task windows:
 * the window opens from the icon; a dropped file is read by ffprobe (its
 * output split across messages); the description, the suggested name and
 * the shading follow; a popup menu changes the size (Custom); a name that
 * isn't a full path is refused; the file icon is dragged to a Filer window
 * (DataSave, DataSaveAck) and ffmpeg runs with the right arguments; the
 * progress lines move the bar and say what's left; Stop kills the task and
 * deletes the unfinished file; Convert again asks before replacing; a
 * finished file gets its type and can be played; a failure shows ffmpeg's
 * error; interactive help answers; quitting or closing while converting
 * asks. Then the logic alone: times, names, arguments, errors.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel.h"
#include "../../frontend/convert.h"

int fffront_main(void);
int conv_test_state(void);
const char *conv_test_text(int which);
char *conv_test_buffer(int which);
int conv_test_fill(void);

enum { I_SRC_L, I_SRC, I_INFO, I_PRESET_L, I_PRESET, I_PRESET_B, I_PRESET_D, I_FORMAT_L, I_FORMAT, I_FORMAT_B,
       I_SIZE_L, I_SIZE, I_SIZE_B, I_QUAL_L, I_QUAL, I_QUAL_B, I_SPEED_L, I_SPEED, I_SPEED_B, I_SOUND, I_DEINT,
       I_EASY, I_TRIM_L, I_FROM, I_TO_L, I_TO, I_TRIM_H, I_OPTS_L, I_OPTS, I_OPTS_H, I_FILE, I_NAME, I_CONVERT, I_BAR, I_FILL, I_STOP,
       I_STATUS, I_PLAY, I_SHOW, I_LOG, N_ICONS };

#define WINH 0x6000
#define FILER 0x7777
static int fails, step;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL (step %d): ", step); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char scrap[128];
static int win_made, win_open, nicons, iflags[64], ibox[64][4];
static int icons[64 * 8];                      /* the window's icon blocks (32 bytes each), copied as the Wimp does */
static char started[16][700]; static int nstarted;
static char reported[8][260]; static int nreports, ask_answer = 3;
static int menu_items; static char menu_title[16];
static char drag_sprite[16]; static int drags;
static int datasaves, datasave_type; static char datasave_leaf[64];
static int morites, morite_to, acks, ack19, helps; static char help_text[240];
static int settype = -1, deleted; static char deleted_name[256];
static int exists;                              /* OS_File 17's answer for the output */
static int fake_cs;

static int shaded(int i) { return (iflags[i] >> 22) & 1; }
static int is_selected(int i) { return (iflags[i] >> 21) & 1; }

static char obey[1024];
static void read_obey(const char *cmd)
{
    char path[300];
    const char *p = strstr(cmd, "\"Obey ");
    FILE *f;
    size_t n;
    obey[0] = 0;
    if (!p) return;
    p += 6;
    n = strcspn(p, "\"");
    memcpy(path, p, n);
    path[n] = 0;
    if ((f = fopen(path, "r")) != NULL) {
        n = fread(obey, 1, sizeof(obey) - 1, f);
        obey[n] = 0;
        fclose(f);
    }
}

static void msg(int *b, int action, int from, int size)
{
    memset(b, 0, 256);
    b[0] = size; b[1] = from; b[2] = 100 + step; b[4] = action;
}

static void tw_output(int *b, int from, const char *text)
{
    msg(b, 0x808C1, from, 256);
    b[5] = (int)strlen(text);
    memcpy(&b[6], text, strlen(text));
}

static void click(int *b, int icon_, int buttons)
{
    memset(b, 0, 24);
    b[0] = 500; b[1] = 500; b[2] = buttons; b[3] = WINH; b[4] = icon_;
}

/* the script: returns the event */
static int script(int *b)
{
    switch (step++) {
    case 0: memset(b, 0, 24); b[2] = 4; b[3] = -2; b[4] = 1; return 6;          /* Select on the icon bar icon */
    case 1:
        CHECK(win_made == 1 && win_open && nicons == N_ICONS, "window: made %d, open %d, %d icons", win_made, win_open, nicons);
        CHECK(shaded(I_CONVERT) && shaded(I_FILE) && shaded(I_PRESET) && !shaded(I_SRC), "shading before a file");
        msg(b, 3, 0x888, 256);                                                   /* DataLoad on the window */
        b[5] = WINH; b[6] = -1; b[10] = 0xFFF;
        strcpy((char *)&b[11], "SDFS::Pi.$.Films.trailer/mkv");
        return 18;
    case 2:
        CHECK(acks == 1, "DataLoad not acknowledged");
        CHECK(nstarted == 1 && strstr(started[0], "TaskWindow \"Obey ") && strstr(started[0], "-task &1234 -txt &50524f42 -quit") &&
              strstr(started[0], "-wimpslot 45056K"), "probe task: %s", started[0]);
        read_obey(started[0]);
        CHECK(strstr(obey, "SDFS::Pi.$.Apps.!FFmpeg.ffprobe -v error -of compact=p=0 -show_entries format=duration:stream=") &&
              strstr(obey, " SDFS::Pi.$.Films.trailer/mkv\n"), "probe Obey file: %s", obey);
        CHECK(!strcmp(conv_test_text(0), "Reading the file...") && shaded(I_CONVERT), "while reading: '%s'", conv_test_text(0));
        msg(b, 0x808C2, 0x999, 24); b[5] = 0x50524F42; return 17;                /* TaskWindow_Ego */
    case 3: tw_output(b, 0x999, "codec_name=h264|codec_type=video|width=1920|height=1080|r_frame_rate=25/1|field_order=tt\ncodec_name=ac3|codec_type=aud"); return 17;
    case 4: tw_output(b, 0x999, "io|sample_rate=48000|channels=6\nduration=120.000000\n"); return 17;
    case 5: msg(b, 0x808C3, 0x999, 20); return 17;                               /* TaskWindow_Morio */
    case 6:
        CHECK(!strcmp(conv_test_text(0), "H.264, 1920x1080, 25 fps, interlaced; AC-3 5.1; 2:00 long"), "info: '%s'", conv_test_text(0));
        CHECK(!strcmp(conv_test_text(2), "SDFS::Pi.$.Films.trailer_720/mp4") && !strcmp(conv_test_text(3), "For playing here"),
              "name '%s', preset '%s'", conv_test_text(2), conv_test_text(3));
        CHECK(!shaded(I_CONVERT) && !shaded(I_SIZE) && is_selected(I_DEINT) && is_selected(I_EASY) && is_selected(I_SOUND) &&
              shaded(I_STOP) && shaded(I_PLAY), "shading with a video");
        CHECK(!strcmp(conv_test_text(6), "Sfile_bf8"), "file icon '%s'", conv_test_text(6));
        if (getenv("CONV_DUMP")) {                                               /* for a picture of the window */
            FILE *f = fopen(getenv("CONV_DUMP"), "w");
            for (int i = 0; f && i < nicons; i++) {
                const int *ic = icons + i * 8;
                fprintf(f, "%d\t%d\t%d\t%d\t%d\t%s\t%s\n", ic[0], ic[1], ic[2], ic[3], iflags[i],
                        (const char *)(intptr_t)ic[5], ic[6] == -1 ? "" : (const char *)(intptr_t)ic[6]);
            }
            if (f) fclose(f);
        }
        click(b, I_SIZE_B, 4); return 6;                                         /* the Size popup */
    case 7:
        CHECK(menu_items == 6 && !strcmp(menu_title, "Size"), "size menu: %d items, '%s'", menu_items, menu_title);
        b[0] = 4; b[1] = -1; return 9;                                           /* 480 lines */
    case 8:
        CHECK(!strcmp(conv_test_text(4), "480 lines") && !strcmp(conv_test_text(3), "Custom") &&
              !strcmp(conv_test_text(2), "SDFS::Pi.$.Films.trailer_480/mp4"), "after 480: size '%s', preset '%s', name '%s'",
              conv_test_text(4), conv_test_text(3), conv_test_text(2));
        strcpy(conv_test_buffer(0), "trailer_480/mp4");                          /* the user types a bare name */
        strcpy(conv_test_buffer(1), "0:10");                                     /* trim, as typed */
        strcpy(conv_test_buffer(2), "1:40");
        strcpy(conv_test_buffer(3), "-metadata title=Trailer");                 /* more options, as typed */
        memset(b, 0, 28); b[0] = WINH; b[1] = I_NAME; b[6] = 13; return 8;      /* Return: not a full path */
    case 9:
        CHECK(nreports == 1 && strstr(reported[0], "needs a directory"), "refused name: %s", reported[0]);
        CHECK(nstarted == 1, "started anyway");
        click(b, I_FILE, 64); return 6;                                          /* drag the file icon */
    case 10:
        CHECK(drags == 1 && !strcmp(drag_sprite, "file_bf8"), "drag: %d, sprite '%s'", drags, drag_sprite);
        memset(b, 0, 16); return 7;                                              /* dropped on a Filer window */
    case 11:
        CHECK(datasaves == 1 && !strcmp(datasave_leaf, "trailer_480/mp4") && datasave_type == 0xBF8,
              "DataSave: %d, '%s', type %x", datasaves, datasave_leaf, datasave_type);
        msg(b, 2, 0x333, 256); b[3] = 999; b[9] = 1234;                          /* DataSaveAck from the Filer */
        strcpy((char *)&b[11], "SDFS::Pi.$.Out.trailer_480/mp4");
        return 17;
    case 12:
        CHECK(nstarted == 2 && strstr(started[1], "-txt &434f4e56"), "convert task: %s", started[1]);
        read_obey(started[1]);
        CHECK(!strcmp(obey, "| Run by !FFmpeg's Convert window\nSDFS::Pi.$.Apps.!FFmpeg.ffmpeg -nostdin -hide_banner -v error -y "
                      "-ss 10.000 -to 100.000 -i SDFS::Pi.$.Films.trailer/mkv -map 0:v:0 -map 0:a:0 "
                      "-vf yadif=deint=interlaced,scale=-2:480 -c:v libx264 -preset veryfast -crf 23 -tune fastdecode "
                      "-pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 -movflags +faststart -metadata title=Trailer -progress pipe:1 -nostats "
                      "SDFS::Pi.$.Out.trailer_480/mp4\nEcho ffmpeg-exit=<Sys$ReturnCode>\n"), "ffmpeg Obey file:\n%s", obey);
        CHECK(shaded(I_CONVERT) && shaded(I_SIZE) && !shaded(I_STOP) && !strcmp(conv_test_text(2), "SDFS::Pi.$.Out.trailer_480/mp4"),
              "while converting: shading, name '%s'", conv_test_text(2));
        msg(b, 0x808C2, 0x998, 24); b[5] = 0x434F4E56; return 17;
    case 13: tw_output(b, 0x998, "frame=1\nout_time_us=45000000\nspeed=0.5x\nprogress=continue\n"); return 17;
    case 14:
        CHECK(!strcmp(conv_test_text(1), "Converting: 0:45 of 1:30, 0.5x real time, about 1:30 left"), "status: '%s'", conv_test_text(1));
        CHECK(conv_test_fill() == 28 + 424, "bar to %d (want half: 452)", conv_test_fill());
        click(b, I_STOP, 4); return 6;
    case 15:
        CHECK(morites == 1 && morite_to == 0x998 && !strcmp(conv_test_text(1), "Stopping..."), "Stop: %d Morite to %x, '%s'",
              morites, morite_to, conv_test_text(1));
        msg(b, 0x808C3, 0x998, 20); return 17;
    case 16:
        CHECK(deleted == 1 && !strcmp(deleted_name, "SDFS::Pi.$.Out.trailer_480/mp4") &&
              !strcmp(conv_test_text(1), "Stopped. The unfinished file was deleted.") && !shaded(I_CONVERT) && !shaded(I_LOG),
              "stopped: deleted %d '%s', '%s'", deleted, deleted_name, conv_test_text(1));
        exists = 1;
        click(b, I_CONVERT, 4); return 6;                                        /* again: it's there now */
    case 17:
        CHECK(nreports == 2 && strstr(reported[1], "trailer_480/mp4 is already there. Replace it?"), "replace question: %s", reported[1]);
        CHECK(nstarted == 3, "not started after Replace");
        msg(b, 0x808C2, 0x997, 24); b[5] = 0x434F4E56; return 17;
    case 18: fake_cs += 20000; tw_output(b, 0x997, "out_time_us=90000000\nspeed=0.6x\ntotal_size=31457280\nprogress=end\n"); return 17;
    case 19: msg(b, 0x808C3, 0x997, 20); return 17;
    case 20:
        CHECK(settype == 0xBF8 && !strcmp(conv_test_text(1), "Done: trailer_480/mp4, 30.0 MB, in 3:20.") && !shaded(I_PLAY) &&
              !shaded(I_SHOW) && conv_test_fill() == 876, "done: type %x, '%s', fill %d", settype, conv_test_text(1), conv_test_fill());
        click(b, I_PLAY, 4); return 6;
    case 21:
        CHECK(nstarted == 4 && strstr(started[3], "ffplay") && strstr(started[3], " SDFS::Pi.$.Out.trailer_480/mp4"), "Play: %s", started[3]);
        msg(b, 0x502, 0x444, 40); b[5] = 500; b[6] = 500; b[8] = WINH; b[9] = I_CONVERT; return 17;   /* HelpRequest */
    case 22:
        CHECK(helps == 1 && strstr(help_text, "Click to convert"), "help: %s", help_text);
        click(b, I_CONVERT, 4); return 6;                                        /* a failure */
    case 23: msg(b, 0x808C2, 0x996, 24); b[5] = 0x434F4E56; return 17;
    case 24: tw_output(b, 0x996, "SDFS::Pi.$.Out.trailer_480/mp4: Permission denied\n"); return 17;
    case 25: msg(b, 0x808C3, 0x996, 20); return 17;
    case 26:
        CHECK(!strcmp(conv_test_text(1), "Couldn't convert: SDFS::Pi.$.Out.trailer_480/mp4: Permission denied "
                      "The unfinished file was deleted.") && !shaded(I_LOG) && shaded(I_PLAY) && deleted == 2 &&
              !strcmp(deleted_name, "SDFS::Pi.$.Out.trailer_480/mp4"),
              "failed: '%s', deleted %d '%s' (a failed file isn't left behind)", conv_test_text(1), deleted, deleted_name);
        click(b, I_FORMAT_B, 4); return 6;                                       /* sound only */
    case 27: b[0] = 2; b[1] = -1; return 9;                                      /* MP3 */
    case 28:
        CHECK(!strcmp(conv_test_text(5), "MP3 sound") && shaded(I_SIZE) && shaded(I_DEINT) && shaded(I_SOUND) && is_selected(I_SOUND) &&
              !strcmp(conv_test_text(6), "Sfile_xxx"), "MP3: format '%s', shading, icon '%s' (no file_1ad sprite: the unknown one)",
              conv_test_text(5), conv_test_text(6));
        CHECK(!strcmp(conv_test_text(2), "SDFS::Pi.$.Out.trailer/mp3"), "MP3 name keeps the directory: '%s'", conv_test_text(2));
        click(b, I_CONVERT, 4); return 6;                                        /* converting, then quit */
    case 29: msg(b, 0x808C2, 0x995, 24); b[5] = 0x434F4E56; return 17;
    case 30: ask_answer = 2; msg(b, 8, 0x111, 20); return 18;                   /* PreQuit: Cancel */
    case 31:
        CHECK(ack19 == 1, "PreQuit not objected to (%d)", ack19);
        ask_answer = 3; memset(b, 0, 20); b[0] = WINH; return 3;                /* close the window: Stop */
    case 32:
        CHECK(morites == 2 && morite_to == 0x995 && !win_open, "close while converting: %d Morites, open %d", morites, win_open);
        msg(b, 0x808C3, 0x995, 20); return 17;
    case 33:
        CHECK(conv_test_state() == CONV_STOPPED && deleted == 3, "after closing: state %d, deleted %d", conv_test_state(), deleted);
        click(b, I_CONVERT, 4); return 6;                                        /* again (Replace) */
    case 34: msg(b, 0x808C2, 0x994, 24); b[5] = 0x434F4E56; return 17;
    case 35:   /* a recording that starts part way: decoder messages, then to the end, exit status 0 */
        tw_output(b, 0x994, "[h264 @ 0x1234] non-existing PPS 0 referenced\n[h264 @ 0x1234] no frame!\n"
                  "out_time_us=90000000\ntotal_size=1048576\nprogress=end\nffmpeg-exit=0\n");
        return 17;
    case 36: msg(b, 0x808C3, 0x994, 20); return 17;
    case 37:
        CHECK(conv_test_state() == CONV_DONE && deleted == 3 && !strncmp(conv_test_text(1), "Done: ", 6),
              "decoder messages, then the end and exit 0: state %d, deleted %d, '%s' (it worked: the file stays)",
              conv_test_state(), deleted, conv_test_text(1));
        click(b, I_CONVERT, 4); return 6;                                        /* again, then quit while converting */
    case 38: msg(b, 0x808C2, 0x993, 24); b[5] = 0x434F4E56; return 17;
    case 39: ask_answer = 3; msg(b, 8, 0x111, 20); return 18;                   /* PreQuit: Stop */
    case 40:
        CHECK(morites == 3 && morite_to == 0x993 && conv_test_state() == CONV_STOPPING,
              "PreQuit, Stop: %d Morites to %x, state %d", morites, morite_to, conv_test_state());
        msg(b, 0, 0x111, 20); return 17;                                         /* Message_Quit */
    case 41:   /* (conv_quitting waiting for the task window to end) */
        CHECK(deleted == 3 && conv_test_state() == CONV_STOPPING, "quitting: deleted %d before ffmpeg ended, state %d",
              deleted, conv_test_state());
        msg(b, 0x808C3, 0x993, 20); return 17;
    default: msg(b, 0, 0x111, 20); return 17;                                   /* Message_Quit */
    }
}


/* libavformat's AcornSSL backend (patch 0018) refers to UnixLib's
   __get_ro_socket, if libavformat is linked in */
__attribute__((weak)) int __get_ro_socket(int fd) { return fd; }

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake" };
    int *b = (int *)(intptr_t)in->r[1];
    switch (swi) {
    case 0x400C0: out->r[1] = 0x1234; return NULL;                               /* Wimp_Initialise */
    case 0x29:                                                                   /* OS_FSControl 37 */
        strcpy((char *)(intptr_t)in->r[2], "SDFS::Pi.$.Apps.!FFmpeg");
        out->r[5] = 100;
        return NULL;
    case 0x42681: out->r[0] = -1; return NULL;
    case 0x400C2: out->r[0] = 1; return NULL;                                    /* the icon bar icon */
    case 0x400C1:                                                                /* Wimp_CreateWindow */
        if (b[7] == (int)0x84000012) { out->r[0] = WINH + 0x100; return NULL; } /* Info: About this program (fffront_test checks it) */
        win_made++;
        nicons = b[21];
        memcpy(icons, b + 22, (size_t)(nicons < 64 ? nicons : 64) * 32);
        for (int i = 0; i < nicons && i < 64; i++) {
            iflags[i] = icons[i * 8 + 4];
            memcpy(ibox[i], &icons[i * 8], 16);
        }
        out->r[0] = WINH;
        return NULL;
    case 0x400C5: win_open = 1; return NULL;                                     /* OpenWindow */
    case 0x400C6: win_open = 0; return NULL;                                     /* CloseWindow */
    case 0x400CB: b[1] = 400; b[2] = 200; b[3] = 1500; b[4] = 1032; b[5] = 0; b[6] = 0; b[7] = -1; return NULL;
    case 0x400CD: iflags[b[1]] = (iflags[b[1]] & ~b[3]) ^ b[2]; return NULL;   /* SetIconState */
    case 0x400CE: memcpy(&b[2], ibox[b[1]], 16); b[6] = iflags[b[1]]; return NULL;
    case 0x400FC: case 0x400D1: case 0x400D2: case 0x400DC: return NULL;
    case 0x400D4: {                                                              /* CreateMenu */
        const char *m = (const char *)b;
        const int *items = (const int *)(m + 28);
        memcpy(menu_title, m, 12); menu_title[12] = 0;
        menu_items = 0;
        while (menu_items < 10 && !(items[menu_items * 6] & 0x80)) menu_items++;
        menu_items++;
        return NULL;
    }
    case 0x42400: strcpy(drag_sprite, (const char *)(intptr_t)in->r[2]); drags++; return NULL;   /* DragASprite_Start */
    case 0x42401: return NULL;
    case 0x400CF: b[0] = 900; b[1] = 600; b[2] = 4; b[3] = FILER; b[4] = -1; return NULL;      /* GetPointerInfo */
    case 0x400DF:                                                                /* ReportError */
        snprintf(reported[nreports++ & 7], 260, "%s", ((_kernel_oserror *)(intptr_t)in->r[0])->errmess);
        out->r[1] = (in->r[1] >> 9 & 7) == 4 ? ask_answer : 1;
        return NULL;
    case 0x400E7:                                                                /* SendMessage */
        if (in->r[0] == 19) { ack19++; return NULL; }
        if (b[4] == 4) acks++;
        if (b[4] == 1 && in->r[2] == FILER) {
            datasaves++; datasave_type = b[10]; snprintf(datasave_leaf, sizeof(datasave_leaf), "%s", (char *)&b[11]);
        }
        if (b[4] == 0x808C4) { morites++; morite_to = in->r[2]; }
        if (b[4] == 0x503) { helps++; snprintf(help_text, sizeof(help_text), "%s", (char *)&b[5]); }
        return NULL;
    case 0x400DE:                                                                /* StartTask */
        snprintf(started[nstarted++ & 15], 700, "%s", (const char *)(intptr_t)in->r[0]);
        return NULL;
    case 0x08:                                                                   /* OS_File */
        switch (in->r[0]) {
        case 8: mkdir((const char *)(intptr_t)in->r[1], 0777); return NULL;
        case 18: if (strstr((const char *)(intptr_t)in->r[1], "Out.")) settype = in->r[2]; return NULL;
        case 17: out->r[0] = exists; return NULL;
        case 6: deleted++; snprintf(deleted_name, sizeof(deleted_name), "%s", (const char *)(intptr_t)in->r[1]);
            out->r[0] = 1; return NULL;                                          /* (a file was there) */
        }
        return &err;
    case 0x50B00: {                                                              /* MimeMap_Translate: extension -> type */
        const char *e = (const char *)(intptr_t)in->r[1];
        out->r[3] = !strcmp(e, "mp4") ? 0xBF8 : !strcmp(e, "mp3") ? 0x1AD : !strcmp(e, "mkv") ? 0xB91 : 0xFFD;
        return NULL;
    }
    case 0x42: out->r[0] = fake_cs; return NULL;                                 /* OS_ReadMonotonicTime */
    case 0x400E9:                                                                /* Wimp_SpriteOp 40: only these exist */
        return in->r[0] == 40 && (!strcmp((const char *)(intptr_t)in->r[2], "file_bf8") ||
                                  !strcmp((const char *)(intptr_t)in->r[2], "file_xxx")) ? NULL : &err;
    case 0x35: out->r[2] = in->r[1] == 11 ? 1919 : in->r[1] == 12 ? 1079 : 1; return NULL;
    case 0x400DD: return NULL;                                                   /* CloseDown */
    case 0x400C7: case 0x400E1: out->r[0] = script(b); return NULL;              /* Wimp_Poll, Wimp_PollIdle */
    }
    return &err;
}

int main(void)
{
    ConvSettings s;
    ConvSource in = { 1, 1, 1280, 720, 0, 25, 60, 2, 48000, "h264", "aac" };
    char a[1200], e[200];

    snprintf(scrap, sizeof(scrap), "/tmp/convtest%d", (int)getpid());
    mkdir(scrap, 0777);
    setenv("Wimp$ScrapDir", scrap, 1);
    setenv("FFmpeg$Dir", "<Obey$Dir>", 1);
    CHECK(fffront_main() == 0, "front end didn't end cleanly");
    CHECK(step == 42 && deleted == 4 && !strcmp(deleted_name, "SDFS::Pi.$.Out.trailer/mp3") && conv_test_state() == CONV_STOPPED,
          "quit while converting: step %d, deleted %d '%s', state %d (stopped, and the unfinished file deleted)", step,
          deleted, deleted_name, conv_test_state());
    {   /* the Options field takes spaces ("-vf hflip"); the name doesn't */
        const char *vo = (const char *)(intptr_t)icons[I_OPTS * 8 + 6], *vn = (const char *)(intptr_t)icons[I_NAME * 8 + 6];
        CHECK(vo && !strstr(vo, "A~ ") && vn && strstr(vn, "A~ "), "validation: Options '%s', name '%s'", vo ? vo : "", vn ? vn : "");
    }
    {   /* did it work? */
        ConvProgress p;
        memset(&p, 0, sizeof(p));
        conv_parse_progress(&p, "[h264 @ 0x1] no frame!");
        conv_parse_progress(&p, "progress=end");
        conv_parse_progress(&p, "ffmpeg-exit=0");
        CHECK(conv_succeeded(&p) && p.error[0], "messages, the end, exit 0: worked");
        conv_parse_progress(&p, "ffmpeg-exit=1");
        CHECK(!conv_succeeded(&p), "the end, exit 1: failed");
        memset(&p, 0, sizeof(p));
        conv_parse_progress(&p, "progress=end");
        CHECK(conv_succeeded(&p), "the end, no messages, no exit status: worked");
        conv_parse_progress(&p, "Conversion failed!");
        CHECK(!conv_succeeded(&p), "the end, a message, no exit status: failed (as before)");
        memset(&p, 0, sizeof(p));
        conv_parse_progress(&p, "progress=continue");
        conv_parse_progress(&p, "ffmpeg-exit=0");
        CHECK(!conv_succeeded(&p), "exit 0 without the end: failed");
    }
    printf("  scripted desktop: %d steps\n", step);

    /* the logic alone */
    step = 100;
    CHECK(conv_parse_time("") == 0 && conv_parse_time("95") == 95 && conv_parse_time("1:30") == 90 &&
          conv_parse_time("1:02:03.5") == 3723.5 && conv_parse_time("1:75") < 0 && conv_parse_time("x") < 0 &&
          conv_parse_time("1::2") < 0, "times");
    memset(&s, 0, sizeof(s));
    conv_apply_preset(&s, 0);
    conv_output_leaf(a, sizeof(a), "/home/a/My film.mp4", &s, &in);
    CHECK(!strcmp(a, "My film_new/mp4"), "Unix name: %s", a);
    conv_output_leaf(a, sizeof(a), "ADFS::4.$.Video.clip", &s, &in);
    CHECK(!strcmp(a, "clip_new/mp4"), "no extension: %s", a);
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) == 0 &&
          !strcmp(a, "-nostdin -hide_banner -v error -y -i in -map 0:v:0 -map 0:a:0 -c:v libx264 -preset veryfast -crf 23 "
                     "-tune fastdecode -pix_fmt yuv420p -c:a aac -b:a 128k -ac 2 -movflags +faststart -progress pipe:1 -nostats out"),
          "720p source, preset 0: %s", a);
    in.width = 321;                                                            /* odd: made even */
    conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e));
    CHECK(strstr(a, " -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 ") != NULL, "odd width: %s", a);
    in.width = 1280;
    strcpy(s.from, "3:00");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "after the end"), "from past the end: %s", e);
    strcpy(s.from, "0:20"); strcpy(s.to, "0:10");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "must be after"), "to before from: %s", e);
    strcpy(s.to, "1:x");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "h:mm:ss"), "bad time: %s", e);
    s.from[0] = s.to[0] = 0;
    conv_apply_preset(&s, 3);
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) == 0 &&
          !strcmp(a, "-nostdin -hide_banner -v error -y -i in -vn -map 0:a:0 -c:a libmp3lame -q:a 2 -ac 2 -progress pipe:1 -nostats out"),
          "MP3: %s", a);
    in.has_audio = 0;
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "no sound"), "MP3 of silence: %s", e);
    conv_apply_preset(&s, 1);
    s.sound = 1;
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "o ut", e, sizeof(e)) == 0 && strstr(a, " -an ") &&
          strstr(a, " \"o ut\""), "silent video, spaced name: %s", a);
    in.has_video = 0; in.has_audio = 1;
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "no picture"), "MP4 from sound: %s", e);
    in.has_video = 1;
    conv_apply_preset(&s, 0);                                                  /* the extra options */
    strcpy(s.extra, "  -vf hflip -r 25  ");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) == 0 &&
          strstr(a, " -movflags +faststart -vf hflip -r 25 -progress pipe:1 -nostats out"), "extra options: %s", a);
    strcpy(s.extra, "hflip");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "start with an option"),
          "extra not an option: %s", e);
    strcpy(s.extra, "-y -i other.mp4");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "-i can't"), "extra -i: %s", e);
    strcpy(s.extra, "-r 25\n-an");
    CHECK(conv_build_args(a, sizeof(a), &s, &in, "in", "out", e, sizeof(e)) < 0 && strstr(e, "one line"), "extra newline: %s", e);
    s.extra[0] = 0;
    {
        ConvSource p;
        conv_parse_probe(&p, "codec_name=mjpeg|codec_type=video|width=500|height=500\ncodec_name=mp3|codec_type=audio|channels=2\nduration=N/A\n");
        CHECK(!p.has_video && p.has_audio && p.duration == 0, "cover picture isn't a video");
    }
    {
        ConvProgress p;
        memset(&p, 0, sizeof(p));
        conv_parse_progress(&p, "out_time_us=-9223372036854775807");
        conv_parse_progress(&p, "speed=N/A");
        CHECK(p.out_time == 0 && p.speed == 0, "N/A progress");
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
