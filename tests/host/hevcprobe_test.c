/*
 * hevcprobe (tools/hevcprobe) against a fake firmware and fake registers:
 *   - a Pi 4: the clock turned on at its maximum, the HEVC registers read
 *     only while it's on, VERSION reported, the clock put back off;
 *   - -n: no clock change, no HEVC read;
 *   - not a BCM2711 (a Pi 3): stops before mapping anything;
 *   - a clock the firmware won't turn on: no HEVC read, result 1;
 *   - a block that reads all ones: result 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel.h"

int probe_main(int argc, char **argv);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t board = 0xC03111;           /* Pi 4 Model B 4GB: processor 3 = BCM2711 */
static int clk_on, clk_rate = 200000000, clk_max = 550000000, clk_refuses, maps, sets, bad_reads, hevc_reads;
static uint32_t intc_regs[0x400], hevc_regs[0x4000];
static _kernel_oserror err = { 1, "no" };

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    if (n == 0x68 && r->r[0] == 13) {                     /* OS_Memory 13 */
        maps++;
        r->r[3] = (uint32_t)r->r[1] == 0xFEB10000u ? (int)(uintptr_t)intc_regs :
                  (uint32_t)r->r[1] == 0xFEB00000u ? (int)(uintptr_t)hevc_regs : 0;
        return r->r[3] ? NULL : &err;
    }
    if (n == 0x591C5) {                                   /* BCMSupport_SendTempPropertyBuffer */
        uint32_t *b = (uint32_t *)(uintptr_t)r->r[0];
        uint32_t tag = b[2], *v = b + 5;
        b[1] = 0x80000000u;
        b[4] |= 0x80000000u;
        switch (tag) {
        case 0x00010002: v[0] = board; break;
        case 0x00030001: v[1] = clk_on; break;
        case 0x00030002: v[1] = clk_rate; break;
        case 0x00030004: v[1] = v[0] == 11 ? clk_max : 0; break;
        case 0x00030007: v[1] = 100000000; break;
        case 0x00038001: sets++; if (!clk_refuses) clk_on = v[1] & 1; break;
        case 0x00038002: sets++; clk_rate = v[1]; break;
        default: b[1] = 0x80000001u;
        }
        return NULL;
    }
    return &err;
}

void probe_read(uintptr_t log, uint32_t *dst, int n)
{
    if (log >= (uintptr_t)hevc_regs && log < (uintptr_t)(hevc_regs + 0x4000)) {
        hevc_reads++;
        if (!clk_on) bad_reads++;                         /* read with the clock off: could hang the bus */
    }
    memcpy(dst, (void *)log, n * 4);
}

static int run(const char *what, char *arg)
{
    char *argv[3] = { "hevcprobe", arg, NULL };
    int r;
    printf("-- %s\n", what);
    maps = sets = bad_reads = hevc_reads = 0;
    r = probe_main(arg ? 2 : 1, argv);
    return r;
}

int main(void)
{
    int r;
    hevc_regs[15] = 0x00010203;                           /* VERSION */
    r = run("a Pi 4", NULL);
    CHECK(r == 0 && hevc_reads == 2 && bad_reads == 0 && !clk_on && clk_rate == 200000000,
          "Pi 4: result %d, %d HEVC reads (%d with the clock off), clock %s at %d", r, hevc_reads, bad_reads,
          clk_on ? "left on" : "off", clk_rate);
    r = run("-n", "-n");
    CHECK(r == 2 && sets == 0 && hevc_reads == 0 && maps == 2, "-n: result %d, %d clock changes, %d HEVC reads", r, sets, hevc_reads);
    board = 0xA02082;                                     /* a Pi 3: BCM2837 */
    r = run("a Pi 3", NULL);
    CHECK(r == 3 && maps == 0 && sets == 0, "Pi 3: result %d, %d maps, %d clock changes", r, maps, sets);
    board = 0xC03111;
    clk_refuses = 1;
    r = run("the clock won't turn on", NULL);
    CHECK(r == 1 && hevc_reads == 0, "clock refused: result %d, %d HEVC reads", r, hevc_reads);
    clk_refuses = 0;
    memset(hevc_regs, 0xFF, sizeof(hevc_regs));
    r = run("all ones", NULL);
    CHECK(r == 1 && !clk_on, "all ones: result %d, clock %s", r, clk_on ? "left on" : "off");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
