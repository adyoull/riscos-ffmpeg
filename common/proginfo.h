/*
 * proginfo.h - the standard RISC OS "About this program" window.
 *
 * Like every RISC OS application's: Info on the icon bar menu has a
 * submenu arrow, and moving to it opens a small window with the program's
 * Name, Purpose, Author and Version. The window is a menu leaf (the item's
 * submenu pointer is the window handle), so the Wimp opens and closes it
 * with the menu; the program does nothing else with it.
 *
 * Header only (static functions), for !FFmpeg's front end and for Reel /
 * ReelEGL, which have no Templates file: the window is built in code.
 *
 *   int w = proginfo_create("Reel", "Plays video", "Andrew Youll",
 *                           "0.1.14 (28-Sep-2026)");
 *   ... then give the menu's Info item  sub = w.
 *
 * Returns the window handle, or -1 if the Wimp refused (the caller then
 * leaves Info without a submenu).
 */
#ifndef RISCOS_FFMPEG_PROGINFO_H
#define RISCOS_FFMPEG_PROGINFO_H

#include <stdint.h>
#include <string.h>
#include <kernel.h>

#define PROGINFO_ROWS 4

/* The texts live as long as the program: the icons are indirected to them */
static char proginfo_text[PROGINFO_ROWS][64];

static int proginfo_create(const char *name, const char *purpose, const char *author, const char *version)
{
    static const char *labels[PROGINFO_ROWS] = { "Name:", "Purpose:", "Author:", "Version:" };
    static char title[] = "About this program";
    /* the window block with its icons (Wimp_CreateWindow) */
    struct {
        int vis[4];
        int sx, sy, behind, flags;
        unsigned char tfg, tbg, wfg, wbg, sofg, sibg, tfocus, xflags;
        int ext[4];
        int tflags, wbutton, sprites;
        short minw, minh;
        int title[3];
        int nicons;
        struct { int box[4]; unsigned flags; int data[3]; } icon[PROGINFO_ROWS * 2];
    } w;
    const char *values[PROGINFO_ROWS];
    _kernel_swi_regs r;
    int value_w = 450, width, height;

    values[0] = name; values[1] = purpose; values[2] = author; values[3] = version;
    for (int i = 0; i < PROGINFO_ROWS; i++) {
        int tw;
        strncpy(proginfo_text[i], values[i], sizeof(proginfo_text[i]) - 1);
        proginfo_text[i][sizeof(proginfo_text[i]) - 1] = 0;
        tw = (int)strlen(proginfo_text[i]) * 16 + 32;       /* the desktop font is about 16 units a letter */
        if (tw > value_w)
            value_w = tw;
    }
    width = 152 + value_w + 12;
    height = 8 + PROGINFO_ROWS * 52;

    memset(&w, 0, sizeof(w));
    w.vis[0] = 400; w.vis[1] = 400; w.vis[2] = 400 + width; w.vis[3] = 400 + height;
    w.behind = -1;
    w.flags = (int)0x84000012u;         /* new format, title bar, moveable, auto-redraw */
    w.tfg = 7; w.tbg = 2; w.wfg = 7; w.wbg = 1; w.sofg = 3; w.sibg = 1; w.tfocus = 12;
    w.ext[0] = 0; w.ext[1] = -height; w.ext[2] = width; w.ext[3] = 0;
    w.tflags = 0x0000013D;              /* text, border, centred, filled, indirected */
    w.wbutton = 0;                      /* clicks ignored */
    w.sprites = 1;
    w.title[0] = (int)(intptr_t)title;
    w.title[1] = -1;
    w.title[2] = (int)sizeof(title);
    w.nicons = PROGINFO_ROWS * 2;
    for (int i = 0; i < PROGINFO_ROWS; i++) {
        int top = -8 - i * 52;
        /* the label: right-aligned grey-background text, not indirected */
        w.icon[2 * i].box[0] = 8;   w.icon[2 * i].box[1] = top - 48;
        w.icon[2 * i].box[2] = 148; w.icon[2 * i].box[3] = top;
        w.icon[2 * i].flags = 0x17000211u;      /* text, v centred, right-justified; black on grey */
        strncpy((char *)w.icon[2 * i].data, labels[i], 12);
        /* the value: a white bordered box, indirected to the text */
        w.icon[2 * i + 1].box[0] = 152;          w.icon[2 * i + 1].box[1] = top - 48;
        w.icon[2 * i + 1].box[2] = 152 + value_w; w.icon[2 * i + 1].box[3] = top;
        w.icon[2 * i + 1].flags = 0x0700013Du;  /* text, border, centred, filled, indirected; black on white */
        w.icon[2 * i + 1].data[0] = (int)(intptr_t)proginfo_text[i];
        w.icon[2 * i + 1].data[1] = -1;
        w.icon[2 * i + 1].data[2] = (int)sizeof(proginfo_text[i]);
    }
    r.r[1] = (intptr_t)&w;
    if (_kernel_swi(0x400C1, &r, &r))   /* Wimp_CreateWindow */
        return -1;
    return r.r[0];
}

#endif
