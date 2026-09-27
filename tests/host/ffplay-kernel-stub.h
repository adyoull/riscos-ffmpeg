/* ffplay-direct.sh: ffplay's RISC OS code on the Linux host. The SWIs it
 * calls to leave a TaskWindow, faked: FAKE_TASKWINDOW=1 says we are in one,
 * OS_GetEnv gives a RISC OS command line, and Wimp_StartTask writes its
 * command to the file named by FAKE_STARTTASK. */
#include <stdio.h>
#include <stdlib.h>
#include "kernel.h"
static _kernel_oserror *ffplay_host_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "not faked" };
    switch (swi) {
    case 0x43380: out->r[0] = getenv("FAKE_TASKWINDOW") != NULL; return NULL;
    case 0x10:    out->r[0] = (long)"SDFS::Pi.$.Apps.!FFmpeg.ffplay clip/mp4"; return NULL;
    case 0x400DE: {
        FILE *f = fopen(getenv("FAKE_STARTTASK"), "w");
        if (!f) return &err;
        fputs((const char *)(long)in->r[0], f);
        fclose(f);
        return NULL;
    }
    default: return &err;
    }
}
#define _kernel_swi ffplay_host_swi
