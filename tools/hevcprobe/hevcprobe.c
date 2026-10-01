/*
 * hevcprobe - can RISC OS reach the Raspberry Pi 4's HEVC decoder block?
 *
 * The first step towards hardware HEVC decoding in Reel. It only looks,
 * and puts back what it changed:
 *
 *   1. Asks the firmware (through BCMSupport's property mailbox) which
 *      board this is: a Pi 4 / 400 / CM4 (BCM2711) is required.
 *   2. Asks for the HEVC clock (firmware clock 11): is it on, its rate and
 *      its limits (a clock that doesn't exist reports a maximum of 0).
 *   3. Maps the block's two register areas (OS_Memory 13) and reads the
 *      interrupt controller's control word, which Linux's rpivid driver
 *      also touches with the clock off.
 *   4. Unless -n: turns the clock on at its maximum rate, reads the HEVC
 *      block's registers (VERSION at 0x3C, STATUS, and the rest of the
 *      first 0x80 bytes and of 0x8000..0x8044), then puts the clock back
 *      as it was.
 *
 * Only reads the hardware; the one thing it changes is the clock, through
 * the firmware, and it restores it. The HEVC registers are read only with
 * the clock confirmed on (reading an unclocked block could hang the bus).
 *
 * Addresses and the clock number come from the Raspberry Pi Linux device
 * tree (bcm2711-rpi-ds.dtsi: codec@7eb10000, "intc" 0x7eb10000 + 0x1000,
 * "hevc" 0x7eb00000 + 0x10000, firmware clock 11), and the register names
 * from its rpivid driver; bus 0x7Exxxxxx is ARM 0xFExxxxxx on the BCM2711
 * (bcm2711.dtsi ranges), which is how RISC OS runs the Pi 4.
 *
 *   hevcprobe [-n] [-o file]
 *      -n       don't touch the clock (and so don't read the HEVC registers)
 *      -o file  also write the report to file
 *
 * (Facts only: no code copied from the device tree or rpivid.)
 * Part of riscos-ffmpeg. GPL version 2 or later (see COPYING).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

#define OS_Memory                          0x68
#define BCMSupport_SendTempPropertyBuffer  0x591C5

#define PERI_BASE      0xFE000000u          /* ARM view of bus 0x7E000000 (BCM2711, low peripherals) */
#define HEVC_PHYS      (PERI_BASE + 0x00B00000u)
#define HEVC_SIZE      0x10000u
#define INTC_PHYS      (PERI_BASE + 0x00B10000u)
#define INTC_SIZE      0x1000u
#define HEVC_CLOCK     11                   /* RPI_FIRMWARE_HEVC_CLK_ID */

/* firmware property tags */
#define TAG_BOARD_REV      0x00010002
#define TAG_GET_CLK_STATE  0x00030001
#define TAG_GET_CLK_RATE   0x00030002
#define TAG_GET_MAX_RATE   0x00030004
#define TAG_GET_MIN_RATE   0x00030007
#define TAG_SET_CLK_STATE  0x00038001
#define TAG_SET_CLK_RATE   0x00038002

static FILE *out2;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (out2) {
        va_start(ap, fmt);
        vfprintf(out2, fmt, ap);
        va_end(ap);
    }
}

#ifdef PROBE_TEST
/* host test: tests/host/hevcprobe_test.c supplies these */
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);
void probe_read(uintptr_t log, uint32_t *dst, int n);
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* Reads n words from a device mapping, in SVC mode: an OS_Memory 13
   mapping needn't be readable from USR mode. OS_EnterOS / OS_LeaveOS
   around a plain loop, all in one asm block (the C code's stack isn't
   touched in SVC mode). */
static void probe_read(uintptr_t log, uint32_t *dst, int n)
{
    if (n <= 0)
        return;
    memset(dst, 0, (size_t)n * 4);          /* touch dst here: a stack page not yet mapped
                                               would abort if first written in SVC mode */
    __asm__ volatile(
        "mov   r4, %0\n\t"
        "mov   r5, %1\n\t"
        "mov   r6, %2\n\t"
        "swi   0x16\n\t"                    /* OS_EnterOS */
        "1:\n\t"
        "ldr   r7, [r4], #4\n\t"
        "str   r7, [r5], #4\n\t"
        "subs  r6, r6, #1\n\t"
        "bne   1b\n\t"
        "dsb\n\t"
        "swi   0x7C\n\t"                    /* OS_LeaveOS */
        :
        : "r"(log), "r"(dst), "r"(n)
        : "r0", "r4", "r5", "r6", "r7", "r14", "memory", "cc");
}
#endif

/* One firmware property request with one tag; val: in and out words.
   0 = the firmware answered it. */
static int fw_tag(uint32_t tag, uint32_t *val, int in_words, int out_words)
{
    uint32_t buf[16];
    int words = in_words > out_words ? in_words : out_words, i;
    _kernel_swi_regs r;
    buf[0] = (uint32_t)((6 + words) * 4);
    buf[1] = 0;                             /* a request */
    buf[2] = tag;
    buf[3] = (uint32_t)(words * 4);         /* value buffer size */
    buf[4] = (uint32_t)(in_words * 4);      /* request size */
    for (i = 0; i < words; i++)
        buf[5 + i] = i < in_words ? val[i] : 0;
    buf[5 + words] = 0;                     /* end tag */
    r.r[0] = (int)(uintptr_t)buf;
    r.r[1] = (int)(uintptr_t)buf;
    r.r[2] = 0;
    if (probe_swi(BCMSupport_SendTempPropertyBuffer, &r))
        return -1;
    if (buf[1] != 0x80000000u)
        return -2;                          /* not answered */
    for (i = 0; i < out_words; i++)
        val[i] = buf[5 + i];
    return 0;
}

static uintptr_t map_io(uint32_t phys, uint32_t size)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    r.r[0] = 13;                            /* map in IO space */
    r.r[1] = (int)phys;
    r.r[2] = (int)size;
    if ((e = probe_swi(OS_Memory, &r)) != NULL) {
        say("  OS_Memory 13 &%08X: %s\n", (unsigned)phys, e->errmess);
        return 0;
    }
    return (uintptr_t)(uint32_t)r.r[3];
}

static const char *const hevc_names[] = {
    "SPS0", "SPS1", "PPS", "SLICE", "TILESTART", "TILEEND", "SLICESTART", "MODE",
    "LEFT0", "LEFT1", "LEFT2", "LEFT3", "QP", "CONTROL", "STATUS", "VERSION",
    "BFBASE", "BFNUM", "BFCONTROL", "BFSTATUS", "PUWBASE", "PUWSTRIDE", "COEFFWBASE", "COEFFWSTRIDE",
    "SLICECMDS", "BEGINTILEEND", "TRANSFER", "CFBASE", "CFNUM", "CFSTATUS"
};
static const char *const hevc2_names[] = {
    "PURBASE", "PURSTRIDE", "COEFFRBASE", "COEFFRSTRIDE", "NUMROWS", "CONFIG2", "OUTYBASE", "OUTYSTRIDE",
    "OUTCBASE", "OUTCSTRIDE", "STATUS2", "FRAMESIZE", "MVBASE", "MVSTRIDE", "COLBASE", "COLSTRIDE", "CURRPOC"
};

int probe_main(int argc, char **argv)
{
    int no_clock = 0, i, verdict = 0;
    uint32_t v[3], rev, was_on = 0, was_rate = 0, max_rate = 0, min_rate = 0, ictrl;
    uintptr_t hevc, intc;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n"))
            no_clock = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out2 = fopen(argv[++i], "w");
        else {
            printf("Usage: hevcprobe [-n] [-o file]\n");
            return 1;
        }
    }
    say("hevcprobe: the Raspberry Pi 4's HEVC decoder block, seen from RISC OS\n\n");

    /* 1. the board */
    v[0] = 0;
    if ((i = fw_tag(TAG_BOARD_REV, v, 0, 1)) != 0) {
        say("Board: the firmware didn't answer (%s). Is BCMSupport loaded? Is this a Raspberry Pi?\n",
            i == -1 ? "BCMSupport_SendTempPropertyBuffer failed" : "no reply");
        verdict = 3;
        goto done;
    }
    rev = v[0];
    say("Board revision &%08X: ", (unsigned)rev);
    if (!(rev & 0x800000u) || ((rev >> 12) & 15) != 3) {
        say("not a BCM2711 (Pi 4, 400, CM4); processor %u. Stopping.\n",
            (unsigned)(rev & 0x800000u ? (rev >> 12) & 15 : 99));
        verdict = 3;
        goto done;
    }
    say("BCM2711 (Pi 4 family), board type &%02X, %u MB\n", (unsigned)((rev >> 4) & 0xFF),
        256u << ((rev >> 20) & 7));

    /* 2. the clock */
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(TAG_GET_MAX_RATE, v, 1, 2) == 0) max_rate = v[1];
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(TAG_GET_MIN_RATE, v, 1, 2) == 0) min_rate = v[1];
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(TAG_GET_CLK_RATE, v, 1, 2) == 0) was_rate = v[1];
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(TAG_GET_CLK_STATE, v, 1, 2) == 0) was_on = v[1] & 1;
    say("HEVC clock (firmware clock %d): %s, %u MHz now, %u to %u MHz\n", HEVC_CLOCK,
        was_on ? "on" : "off", (unsigned)(was_rate / 1000000), (unsigned)(min_rate / 1000000),
        (unsigned)(max_rate / 1000000));
    if (!max_rate) {
        say("  The firmware doesn't know this clock (maximum 0). Stopping.\n");
        verdict = 3;
        goto done;
    }

    /* 3. the registers */
    intc = map_io(INTC_PHYS, INTC_SIZE);
    hevc = map_io(HEVC_PHYS, HEVC_SIZE);
    if (!intc || !hevc) {
        say("Couldn't map the block's registers. Stopping.\n");
        verdict = 3;
        goto done;
    }
    say("Mapped: interrupt controller &%08X at &%08X, HEVC &%08X at &%08X\n", (unsigned)INTC_PHYS,
        (unsigned)intc, (unsigned)HEVC_PHYS, (unsigned)hevc);
    probe_read(intc, &ictrl, 1);
    say("Interrupt controller ICTRL (clock %s): &%08X\n", was_on ? "on" : "off", (unsigned)ictrl);

    if (no_clock) {
        say("\n-n: the clock left alone, so the HEVC registers weren't read.\n");
        verdict = 2;
        goto done;
    }

    /* 4. clock on, read, clock back */
    if (!was_on || was_rate < max_rate) {
        v[0] = HEVC_CLOCK; v[1] = max_rate; v[2] = 0;
        fw_tag(TAG_SET_CLK_RATE, v, 3, 2);
        v[0] = HEVC_CLOCK; v[1] = 1;
        fw_tag(TAG_SET_CLK_STATE, v, 2, 2);
    }
    {
        uint32_t on = 0, rate = 0;
        v[0] = HEVC_CLOCK; v[1] = 0;
        if (fw_tag(TAG_GET_CLK_STATE, v, 1, 2) == 0) on = v[1] & 1;
        v[0] = HEVC_CLOCK; v[1] = 0;
        if (fw_tag(TAG_GET_CLK_RATE, v, 1, 2) == 0) rate = v[1];
        say("Clock now: %s, %u MHz\n", on ? "on" : "off", (unsigned)(rate / 1000000));
        if (!on) {
            say("  The firmware wouldn't turn it on, so the HEVC registers weren't read.\n");
            verdict = 1;
        } else {
            uint32_t a[30], b[17];
            int alive = 0, all_ones = 1;
            probe_read(hevc, a, 30);
            probe_read(hevc + 0x8000, b, 17);
            probe_read(intc, &ictrl, 1);
            say("\nHEVC registers (clock on):\n");
            for (i = 0; i < 30; i++) {
                say("  &%04X %-13s &%08X\n", i * 4, hevc_names[i], (unsigned)a[i]);
                if (a[i] && a[i] != 0xFFFFFFFFu) alive = 1;
                if (a[i] != 0xFFFFFFFFu) all_ones = 0;
            }
            for (i = 0; i < 17; i++)
                say("  &%04X %-13s &%08X\n", 0x8000 + i * 4, hevc2_names[i], (unsigned)b[i]);
            say("Interrupt controller ICTRL (clock on): &%08X\n", (unsigned)ictrl);
            say("\nVERSION &%08X: %s\n", (unsigned)a[15],
                a[15] && a[15] != 0xFFFFFFFFu ? "the block answers" :
                all_ones ? "all ones: nothing answering at this address" :
                !alive ? "all zeros: it may be held in reset, or powered off" : "zero, but other registers aren't");
            verdict = a[15] && a[15] != 0xFFFFFFFFu ? 0 : 1;
        }
    }
    /* back as it was */
    if (!was_on) {
        v[0] = HEVC_CLOCK; v[1] = 0;
        fw_tag(TAG_SET_CLK_STATE, v, 2, 2);
    }
    if (was_rate) {
        v[0] = HEVC_CLOCK; v[1] = was_rate; v[2] = 0;
        fw_tag(TAG_SET_CLK_RATE, v, 3, 2);
    }
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(TAG_GET_CLK_STATE, v, 1, 2) == 0)
        say("Clock put back: %s\n", v[1] & 1 ? "on" : "off");

done:
    say("\nResult: %s\n", verdict == 0 ? "OK - the HEVC block is there and answers" :
                          verdict == 1 ? "the block didn't answer" :
                          verdict == 2 ? "partial (-n)" : "stopped early");
    if (out2)
        fclose(out2);
    return verdict;
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
