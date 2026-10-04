/*
 * fffront - !FFmpeg's icon bar front end.
 *
 * Puts the FFmpeg icon on the icon bar. Drop a video (or sound) file on it
 * and ffplay plays it in a desktop window of its own: each file starts a new
 * task, through the Obey file !FFmpeg.Task (which sets the WimpSlot), with
 * ffplay's messages in <Wimp$ScrapDir>.ffplay/log.
 *
 *   Menu on the icon: Info, Convert... (the Convert window, convert.c), Full
 *   screen (ffplay -fs), Log (opens the last ffplay's messages,
 *   <Wimp$ScrapDir>.ffplay/log, verbose), Quit.
 *   Select on the icon: the Convert window.
 *
 * A small UnixLib program with no heap to speak of; the SWIs are called
 * directly. Part of riscos-ffmpeg. GPL v2 or later (as the rest of !FFmpeg).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include "convert.h"
#include "../common/version.h"
#include "../common/proginfo.h"   /* Info: the standard About this program window */

#define OS_WriteC                0x00
#define Wimp_Initialise          0x400C0
#define Wimp_CreateIcon          0x400C2
#define Wimp_Poll                0x400C7
#define Wimp_CreateMenu          0x400D4
#define Wimp_GetPointerInfo      0x400CF
#define Wimp_SendMessage         0x400E7
#define Wimp_ReportError         0x400DF
#define Wimp_CloseDown           0x400DD
#define TaskManager_EnumerateTasks 0x42681

#define MSG_QUIT      0
#define MSG_DATASAVE  1
#define MSG_DATALOAD  3
#define MSG_DATALOADACK 4
#define MSG_PREQUIT   8

#define TASK_WORD 0x4B534154          /* "TASK" */
#define APP_NAME  "FFmpeg"
#define TASK_NAME "FFmpeg player"      /* ffplay's own tasks are called "FFmpeg" (SDL uses the app name) */
static int task, icon, fullscreen, proginfo = -1;
static char ffdir[256];

static _kernel_oserror *swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

static void report(const char *text, int flags)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "%s", text);
    r.r[0] = (intptr_t)&e;
    r.r[1] = flags;                   /* 1 = OK box, 16 = no "Error from" prefix */
    r.r[2] = (intptr_t)APP_NAME;
    swi(Wimp_ReportError, &r);
}

/* Wimp_StartTask with our VFP context switched off meanwhile (UnixLib keeps
   it on our stack; the new task's UnixLib makes its own and VFPSupport
   would otherwise save ours lazily into the wrong place). One asm block so
   the compiler can't put a VFP instruction between the SWIs. */
_kernel_oserror *fffront_start_task(const char *cmd)
{
#if defined(__arm__) && defined(__riscos__)
    _kernel_oserror *err;
    __asm__ volatile(
        "mov    r0, #0\n\t"
        "mov    r1, #0\n\t"
        "swi    0x78EC3\n\t"          /* XVFPSupport_ChangeContext 0: off */
        "movvs  r0, #0\n\t"
        "mov    r4, r0\n\t"
        "mov    r0, %1\n\t"
        "swi    0x600DE\n\t"          /* XWimp_StartTask */
        "movvs  %0, r0\n\t"
        "movvc  %0, #0\n\t"
        "movs   r0, r4\n\t"
        "mov    r1, #0\n\t"
        "swine  0x78EC3\n\t"          /* back on */
        : "=&r"(err) : "r"(cmd) : "r0", "r1", "r2", "r3", "r4", "r14", "cc", "memory");
    return err;
#else
    _kernel_swi_regs r;
    r.r[0] = (intptr_t)cmd;
    return swi(0x400DE, &r);
#endif
}

/* The command that plays FILE: exposed for the host test. */
int fffront_command(char *out, size_t size, const char *dir, const char *file, int fs,
                    const char *scrap)
{
    int n;
    const char *q = strchr(file, ' ') ? "\"" : "";
    if (scrap && *scrap)
        n = snprintf(out, size, "Obey %s.Task ffplay -nostats -hide_banner -loglevel verbose%s %s%s%s > %s.ffplay/log 2>&1",
                     dir, fs ? " -fs" : "", q, file, q, scrap);
    else
        n = snprintf(out, size, "Obey %s.Task ffplay -nostats -hide_banner -loglevel quiet%s %s%s%s",
                     dir, fs ? " -fs" : "", q, file, q);
    return n > 0 && (size_t)n < size ? 0 : -1;
}

void fffront_play(const char *file)
{
    char cmd[1024];
    _kernel_oserror *err;
    if (fffront_command(cmd, sizeof(cmd), ffdir, file, fullscreen, getenv("Wimp$ScrapDir")) < 0) {
        report("The file's name is too long.", 1);
        return;
    }
    if ((err = fffront_start_task(cmd)) != NULL)
        report(err->errmess, 1);
}

/* ---- menu ---------------------------------------------------------- */

typedef struct { int flags, sub, iflags; char text[12]; } item_t;
static struct {
    char title[12];
    unsigned char tfg, tbg, wfg, wbg;
    int width, height, gap;
    item_t item[5];
} menu;
static int menu_x;

static void menu_build(void)
{
    static const char *names[5] = { "Info", "Convert...", "Full screen", "Log", "Quit" };
    memset(&menu, 0, sizeof(menu));
    strcpy(menu.title, APP_NAME);
    menu.tfg = 7; menu.tbg = 2; menu.wfg = 7; menu.wbg = 0;
    menu.width = 12 * 16 + 16;
    menu.height = 44;
    menu.gap = 0;
    for (int i = 0; i < 5; i++) {
        menu.item[i].flags = (i == 2 && fullscreen ? 1 : 0) | (i == 2 ? 2 : 0) | (i == 4 ? 0x80 : 0);
        menu.item[i].sub = i == 0 ? proginfo : -1;   /* Info: About this program (-1: none) */
        menu.item[i].iflags = 0x07000021;     /* text, filled, black on white */
        strncpy(menu.item[i].text, names[i], 12);
    }
}

static void menu_open(int x)
{
    _kernel_swi_regs r;
    menu_x = x;
    conv_menu_forget();                       /* (a Convert popup that was left open) */
    menu_build();
    r.r[1] = (intptr_t)&menu;
    r.r[2] = x - 64;
    r.r[3] = 96 + 5 * 44;
    swi(Wimp_CreateMenu, &r);
}

/* Quit while converting: ask */
static int quit_ok(void)
{
    _kernel_oserror e;
    _kernel_swi_regs r;
    e.errnum = 0;
    snprintf(e.errmess, sizeof(e.errmess), "A file is being converted. Stop converting it and quit?");
    r.r[0] = (intptr_t)&e;
    r.r[1] = 2 | (1 << 8) | (4 << 9);
    r.r[2] = (intptr_t)APP_NAME;
    r.r[3] = (intptr_t)"!ffmpeg";
    r.r[4] = 1;
    r.r[5] = (intptr_t)"Quit";
    return !swi(Wimp_ReportError, &r) && r.r[1] == 3;
}

static void menu_select(const int *sel)
{
    int block[5];
    _kernel_swi_regs r;
    switch (sel[0]) {
    case 0:                                   /* Info: its window is the submenu */
        break;
    case 1:
        conv_open();
        break;
    case 2:
        fullscreen = !fullscreen;
        break;
    case 3: {
        const char *scrap = getenv("Wimp$ScrapDir");
        char cmd[300];
        if (!scrap || !*scrap) {
            report("There's no log: <Wimp$ScrapDir> isn't set.", 1);
            break;
        }
        snprintf(cmd, sizeof(cmd), "Filer_Run %s.ffplay/log", scrap);
        if (fffront_start_task(cmd))
            report("No log yet: play a file first. It's <Wimp$ScrapDir>.ffplay/log.", 1 | 16);
        break;
    }
    case 4:
        if (conv_busy() && !quit_ok())
            break;
        conv_quitting();                      /* (stopped, and its unfinished file deleted) */
        r.r[0] = 0;
        swi(Wimp_CloseDown, &r);
        exit(0);
    }
    /* Adjust keeps the menu open */
    r.r[1] = (intptr_t)block;
    if (!swi(Wimp_GetPointerInfo, &r) && (block[2] & 1))
        menu_open(menu_x);
}

/* ---- set up -------------------------------------------------------- */

/* Our directory, as a full path worked out now. FFmpeg$Dir can hold a
   reference such as <Obey$Dir> that only means the right thing while !Run
   (or the boot sequence) is running, so canonicalise it at start-up
   (OS_FSControl 37); failing that, use the directory of this program, from
   the command line that started it (OS_GetEnv). */
static void find_dir(void)
{
    _kernel_swi_regs r;
    const char *env;
    char *dot;
    size_t n;

    ffdir[0] = 0;
    r.r[0] = 37;
    r.r[1] = (intptr_t)"<FFmpeg$Dir>";
    r.r[2] = (intptr_t)ffdir;
    r.r[3] = 0;
    r.r[4] = 0;
    r.r[5] = sizeof(ffdir);
    if (!swi(0x29 /* OS_FSControl */, &r) && ffdir[0] && r.r[5] > 0 && strchr(ffdir, '.'))
        return;
    ffdir[0] = 0;
    if (swi(0x10 /* OS_GetEnv */, &r))
        return;
    env = (const char *)(intptr_t)r.r[0];
    for (n = 0; env[n] > ' ' && n < sizeof(ffdir) - 1; n++)
        ffdir[n] = env[n];
    ffdir[n] = 0;
    if ((dot = strrchr(ffdir, '.')) != NULL && strchr(ffdir, ':'))
        *dot = 0;                               /* drop the leaf (!RunImage) */
    else
        ffdir[0] = 0;
}



static int already_running(void)
{
    int buf[16 * 4];
    _kernel_swi_regs r;
    r.r[0] = 0;
    do {
        r.r[1] = (intptr_t)buf;
        r.r[2] = sizeof(buf);
        if (swi(TaskManager_EnumerateTasks, &r))
            return 0;
        for (int *p = buf; (char *)p < (char *)(intptr_t)r.r[1]; p += 4)
            if (p[0] != task) {               /* task names end with a control character */
                const char *name = (const char *)(intptr_t)p[1];
                size_t n = strlen(TASK_NAME);
                if (!strncmp(name, TASK_NAME, n) && (unsigned char)name[n] < 32)
                    return 1;
            }
    } while (r.r[0] >= 0);
    return 0;
}

static void iconbar_icon(void)
{
    struct { int w, x0, y0, x1, y1, flags; char name[12]; } b;
    _kernel_swi_regs r;
    memset(&b, 0, sizeof(b));
    b.w = -1;                                 /* right-hand side */
    b.x1 = 68; b.y1 = 68;
    b.flags = 0x301A;                         /* sprite, centred, click */
    strcpy(b.name, "!ffmpeg");
    r.r[0] = 0;
    r.r[1] = (intptr_t)&b;
    swi(Wimp_CreateIcon, &r);
    icon = r.r[0];
}

int fffront_main(void)
{
    static const int messages[] = { MSG_DATALOAD, 2 /* DataSaveAck */, MSG_PREQUIT, 0x502 /* HelpRequest */,
                                    0x808C1, 0x808C2, 0x808C3 /* TaskWindow Output, Ego, Morio */, 0 };
    int block[64];
    _kernel_swi_regs r;

    find_dir();
    r.r[0] = 380;
    r.r[1] = TASK_WORD;
    r.r[2] = (intptr_t)TASK_NAME;
    r.r[3] = (intptr_t)messages;
    if (swi(Wimp_Initialise, &r))
        return 1;
    task = r.r[1];
    if (!*ffdir) {
        report("Can't find the !FFmpeg directory: start FFmpeg by double-clicking !FFmpeg.", 1);
        return 1;
    }
    if (already_running()) {
        r.r[0] = task;
        swi(Wimp_CloseDown, &r);
        return 0;
    }
    iconbar_icon();
    proginfo = proginfo_create("FFmpeg", "Converts and plays video", APP_AUTHOR,
                               FFMPEG_APP_VERSION " (" FFMPEG_APP_DATE ")");
    conv_init(task, ffdir);

    for (;;) {
        r.r[0] = 1 | (1 << 4) | (1 << 5);     /* no null or pointer events (the task windows' output comes as messages) */
        r.r[1] = (intptr_t)block;
        if (swi(Wimp_Poll, &r))
            continue;
        switch (r.r[0]) {
        case 2:                               /* Open_Window_Request */
            r.r[1] = (intptr_t)block;
            swi(0x400C5, &r);
            break;
        case 3:                               /* Close_Window_Request */
            conv_close_request(block);
            break;
        case 6:                               /* Mouse_Click */
            if (conv_click(block))
                break;
            if (block[3] != -2 || block[4] != icon)
                break;
            if (block[2] & 2)
                menu_open(block[0]);
            else
                conv_open();
            break;
        case 7:                               /* User_Drag_Box: the file icon was dropped */
            conv_drag_end();
            break;
        case 8:                               /* Key_Pressed */
            if (!conv_key(block)) {
                r.r[0] = block[6];
                swi(0x400DC, &r);
            }
            break;
        case 9:                               /* Menu_Selection */
            if (conv_menu_open())
                conv_menu_select(block);
            else
                menu_select(block);
            break;
        case 17: case 18: case 19:            /* User_Message(_Recorded, _Acknowledge) */
            if (conv_message(block))
                break;
            if (block[4] == MSG_QUIT) {
                conv_quitting();
                r.r[0] = task;
                swi(Wimp_CloseDown, &r);
                return 0;
            }
            if (block[4] == MSG_DATALOAD && block[5] == -2 && block[6] == icon) {
                int type = block[10];
                char file[256];
                snprintf(file, sizeof(file), "%s", (const char *)&block[11]);
                /* acknowledge first, so the sender doesn't delete a scrap file */
                block[3] = block[2];
                block[4] = MSG_DATALOADACK;
                r.r[0] = 17;
                r.r[1] = (intptr_t)block;
                r.r[2] = block[1];
                swi(Wimp_SendMessage, &r);
                if (type == 0x1000 || type == 0x2000)
                    report("That's a directory: drop a video file.", 1 | 16);
                else
                    fffront_play(file);
            }
            break;
        }
    }
}

#ifndef FFFRONT_NO_MAIN
int main(void) { return fffront_main(); }
#endif
