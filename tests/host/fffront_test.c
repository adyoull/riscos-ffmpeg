/* Host test of the icon bar front end (frontend/fffront.c) with a scripted
 * fake Wimp: a file dropped on the icon starts ffplay through !FFmpeg.Task
 * (acknowledged first), the menu toggles full screen, directories are
 * refused, Select on the icon opens the Convert window (convert_test.c
 * tests that), and Message_Quit ends it. Build: see tests/host/run.sh. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

int fffront_main(void);
int fffront_command(char *, size_t, const char *, const char *, int, const char *);

static const char *canon = "SDFS::Pi.$.Apps.!FFmpeg";   /* what OS_FSControl 37 gives; NULL = fail */
static int fails, step, icon_made, acks, reports, closed, windows, opened;
static char started[4][1024]; static int nstarted;
static char icon_name[13]; static int icon_flags;
static int proginfo_made, proginfo_icons, info_sub = -99; static char proginfo_labels[64];
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void dataload(int *b, const char *name, int type)
{
    memset(b, 0, 256);
    b[0] = 256; b[1] = 0x777; b[2] = 55; b[4] = 3;      /* Message_DataLoad from the Filer */
    b[5] = -2; b[6] = 1; b[10] = type;
    strcpy((char *)&b[11], name);
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "fake" };
    switch (swi) {
    case 0x400C0: out->r[1] = 0x1234; return NULL;                 /* Wimp_Initialise */
    case 0x29:                                                     /* OS_FSControl 37 */
        if (in->r[0] != 37 || !canon || strcmp((const char *)(intptr_t)in->r[1], "<FFmpeg$Dir>")) return &err;
        strcpy((char *)(intptr_t)in->r[2], canon);
        out->r[5] = in->r[5] - (long)strlen(canon) - 1;
        return NULL;
    case 0x10: out->r[0] = (long)"SDFS::Pi.$.Other.!FFmpeg.!RunImage "; return NULL;   /* OS_GetEnv */
    case 0x42681: out->r[0] = -1; return NULL;                     /* EnumerateTasks: none */
    case 0x400C2: {                                                /* Wimp_CreateIcon */
        int *b = (int *)(intptr_t)in->r[1];
        icon_made++; icon_flags = b[5]; memcpy(icon_name, &b[6], 12);
        out->r[0] = 1; return NULL;
    }
    case 0x400DE:                                                  /* Wimp_StartTask */
        snprintf(started[nstarted++ & 3], 1024, "%s", (const char *)(intptr_t)in->r[0]);
        return NULL;
    case 0x400E7: {                                                /* Wimp_SendMessage */
        int *b = (int *)(intptr_t)in->r[1];
        if (in->r[0] == 17 && b[4] == 4 && b[3] == 55 && in->r[2] == 0x777) acks++;
        return NULL;
    }
    case 0x400DF: reports++; return NULL;                          /* Wimp_ReportError */
    case 0x400CF: { int *b = (int *)(intptr_t)in->r[1]; b[2] = 4; return NULL; }  /* pointer: Select */
    case 0x400D4: {                                                /* Wimp_CreateMenu */
        int *b = (int *)(intptr_t)in->r[1];
        if (b && !strncmp((const char *)&b[7 + 3], "Info", 12)) info_sub = b[7 + 1];
        return NULL;
    }
    case 0x400C1: {                                                /* Wimp_CreateWindow */
        int *b = (int *)(intptr_t)in->r[1];
        if (b[7] == (int)0x84000012) {                             /* About this program */
            proginfo_made++; proginfo_icons = b[21];
            snprintf(proginfo_labels, sizeof(proginfo_labels), "%.12s|%.12s|%.12s|%.12s",
                     (char *)(b + 22 + 5), (char *)(b + 22 + 16 + 5), (char *)(b + 22 + 32 + 5), (char *)(b + 22 + 48 + 5));
            out->r[0] = 0x7000; return NULL;
        }
        windows++; out->r[0] = 0x6000; return NULL;                /* the Convert window */
    }
    case 0x400C5: opened++; return NULL;                           /* Wimp_OpenWindow */
    case 0x400CB: case 0x400CD: case 0x35: return NULL;
    case 0x400DD: closed++; return NULL;                           /* Wimp_CloseDown */
    case 0x400C7: {                                                /* Wimp_Poll: the script */
        int *b = (int *)(intptr_t)in->r[1];
        switch (step++) {
        case 0: dataload(b, "SDFS::Pi.$.Films.clip/mp4", 0xFFD); out->r[0] = 18; break;
        case 1: memset(b, 0, 20); b[3] = -2; b[4] = 1; b[2] = 2; out->r[0] = 6; break;  /* Menu click */
        case 2: b[0] = 2; b[1] = -1; out->r[0] = 9; break;         /* Full screen */
        case 3: dataload(b, "ADFS::HD4.$.My Films.b/mkv", 0xFFD); out->r[0] = 18; break;
        case 4: dataload(b, "ADFS::HD4.$.Films", 0x1000); out->r[0] = 18; break;
        case 5: memset(b, 0, 20); b[3] = -2; b[4] = 1; b[2] = 4; out->r[0] = 6; break;  /* Select click */
        default: memset(b, 0, 24); b[4] = 0; out->r[0] = 17; break;   /* Message_Quit */
        }
        return NULL;
    }
    }
    return &err;
}

int main(void)
{
    char cmd[1024];
    /* FFmpeg$Dir as the boot sequence can leave it: canonicalised at start-up */
    setenv("FFmpeg$Dir", "<Obey$Dir>", 1);
    setenv("Wimp$ScrapDir", "SDFS::Pi.$.Scrap", 1);
    CHECK(fffront_main() == 0, "front end didn't end cleanly");
    CHECK(icon_made == 1 && !strcmp(icon_name, "!ffmpeg") && (icon_flags & 0xF00A) == 0x300A,
          "icon bar icon: %d made, '%s', flags %x", icon_made, icon_name, icon_flags);
    CHECK(nstarted == 2, "%d tasks started (want 2)", nstarted);
    CHECK(!strcmp(started[0], "Obey SDFS::Pi.$.Apps.!FFmpeg.Task ffplay -nostats -hide_banner -loglevel verbose "
                  "SDFS::Pi.$.Films.clip/mp4 > SDFS::Pi.$.Scrap.ffplay/log 2>&1"), "command 1: %s", started[0]);
    CHECK(!strcmp(started[1], "Obey SDFS::Pi.$.Apps.!FFmpeg.Task ffplay -nostats -hide_banner -loglevel verbose -fs "
                  "\"ADFS::HD4.$.My Films.b/mkv\" > SDFS::Pi.$.Scrap.ffplay/log 2>&1"), "command 2: %s", started[1]);
    CHECK(acks == 3, "%d DataLoadAcks (want 3)", acks);
    CHECK(reports == 1, "%d reports (want 1: the directory)", reports);
    CHECK(windows == 1 && opened == 1, "Select on the icon: %d windows made, %d opened", windows, opened);
    CHECK(closed == 1, "Wimp_CloseDown %d times", closed);
    CHECK(proginfo_made == 1 && proginfo_icons == 8 && !strcmp(proginfo_labels, "Name:|Purpose:|Author:|Version:"),
          "Info window: made %d, %d icons, labels %s", proginfo_made, proginfo_icons, proginfo_labels);
    CHECK(info_sub == 0x7000, "icon bar menu: Info's submenu is %x (want the Info window)", info_sub);
    CHECK(fffront_command(cmd, sizeof(cmd), "D", "f", 0, NULL) == 0 &&
          !strcmp(cmd, "Obey D.Task ffplay -nostats -hide_banner -loglevel quiet f"), "no scrap: %s", cmd);
    /* OS_FSControl fails: the program's own directory from OS_GetEnv */
    proginfo_made = 0;
    canon = NULL; step = 0; nstarted = 0; closed = 0; acks = 0; reports = 0; icon_made = 0; windows = opened = 0;
    CHECK(fffront_main() == 0 && nstarted == 2 &&
          !strncmp(started[0], "Obey SDFS::Pi.$.Other.!FFmpeg.Task ffplay ", 42), "OS_GetEnv fallback: %s", started[0]);
    printf("  started: %s\n", started[0]);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
