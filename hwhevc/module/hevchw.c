/*
 * hevchw.c - HEVCHW, a RISC OS module that owns the Raspberry Pi 4's HEVC
 * decoder block, step 2 towards hardware HEVC decoding in Reel. It proves
 * the three things a driver needs, one *command each:
 *
 *   *HEVCInfo     the clock, VERSION, STATUS, the interrupt control word
 *   *HEVCRegTest  registers can be written: patterns into three address
 *                 registers, read back, then 0 again
 *   *HEVCIRQTest  the block's interrupt reaches RISC OS: the block's own
 *                 test interrupt (ICTRL bit 8) is raised with the device
 *                 masked, to find its line at the GIC; only if that's the
 *                 expected one (ID 130) is the device claimed, enabled, and
 *                 the interrupts counted
 *   *HEVCMemTest  memory the block could read and write: physically
 *                 contiguous, from PCI_RAMAlloc, below 1 GB (the bus
 *                 address the block would be given)
 *
 * Freestanding: no C library. SWIs through hw_swi (header.s). Built as a
 * position-independent image (build.sh checks there are no absolute
 * relocations). Host tests build this file with HW_TEST and fakes.
 *
 * Part of riscos-ffmpeg (hwhevc/). MIT licence.
 */
#include <stddef.h>
#include <stdint.h>
#include "hw.h"

#define OS_Write0              0x02
#define OS_NewLine             0x03
#define OS_Module              0x1E
#define OS_SWINumberFromString 0x39
#define OS_ReadMonotonicTime   0x42
#define OS_ClaimDeviceVector   0x4B
#define OS_ReleaseDeviceVector 0x4C
#define OS_Memory              0x68
#define OS_Hardware            0x7A
#define BCMSupport_SendTempPropertyBuffer 0x591C5
#define HAL_IRQEnable  1
#define HAL_IRQDisable 2
#define HAL_IRQClear   3

typedef struct { int errnum; char errmess[64]; } err_t;

/* (from header.s, or the host test's fakes) */
#define HIDDEN __attribute__((visibility("hidden")))   /* (PC-relative, no GOT) */
HIDDEN int hw_swi(int swi, uint32_t *r);
HIDDEN void hw_irq_handler(void);

#ifdef HW_TEST                       /* the host test: the fake block reacts here */
void hw_test_poke(ws_t *w);
#define POKE(w) hw_test_poke(w)
#else
#define POKE(w) ((void)0)
#endif

static uint32_t R[10];                /* SWI registers */

static int swi0(int n) { return hw_swi(n, R); }

/* ---- output: OS_Write0 / OS_NewLine, and a small formatter ---- */

static void put(const char *s)
{
    R[0] = (uint32_t)(uintptr_t)s;
    swi0(OS_Write0);
}

static void nl(void) { swi0(OS_NewLine); }

static void hex8(uint32_t v)
{
    char b[10];
    b[0] = '&';
    for (int i = 0; i < 8; i++) {
        uint32_t d = (v >> (28 - 4 * i)) & 15;
        b[1 + i] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    b[9] = 0;
    put(b);
}

static void dec(uint32_t v)                  /* (no division: subtraction by powers of ten) */
{
    static const uint32_t p[] = { 1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u };
    char b[12];
    int n = 0, started = 0;
    for (int i = 0; i < 10; i++) {
        int d = 0;
        while (v >= p[i]) { v -= p[i]; d++; }
        if (d || started || i == 9) { b[n++] = (char)('0' + d); started = 1; }
    }
    b[n] = 0;
    put(b);
}

static uint32_t now_cs(void)
{
    swi0(OS_ReadMonotonicTime);
    return R[0];
}

/* ---- the firmware (BCMSupport's property mailbox) ---- */

static int fw_tag(uint32_t tag, uint32_t *val, int in_words, int out_words)
{
    uint32_t buf[16];
    int words = in_words > out_words ? in_words : out_words;
    buf[0] = (uint32_t)((6 + words) * 4);
    buf[1] = 0;
    buf[2] = tag;
    buf[3] = (uint32_t)(words * 4);
    buf[4] = (uint32_t)(in_words * 4);
    for (int i = 0; i < words; i++)
        buf[5 + i] = i < in_words ? val[i] : 0;
    buf[5 + words] = 0;
    R[0] = (uint32_t)(uintptr_t)buf;
    R[1] = (uint32_t)(uintptr_t)buf;
    R[2] = 0;
    if (swi0(BCMSupport_SendTempPropertyBuffer) || buf[1] != 0x80000000u)
        return -1;
    for (int i = 0; i < out_words; i++)
        val[i] = buf[5 + i];
    return 0;
}

/* The HEVC clock: 1 on (turned on at its maximum if it was off) */
static int clock_on(uint32_t *rate)
{
    uint32_t v[3];
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(0x00030001, v, 1, 2) == 0 && !(v[1] & 1)) {
        uint32_t max[2] = { HEVC_CLOCK, 0 };
        if (fw_tag(0x00030004, max, 1, 2) == 0 && max[1]) {
            v[0] = HEVC_CLOCK; v[1] = max[1]; v[2] = 0;
            fw_tag(0x00038002, v, 3, 2);
        }
        v[0] = HEVC_CLOCK; v[1] = 1;
        fw_tag(0x00038001, v, 2, 2);
    }
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(0x00030001, v, 1, 2) != 0 || !(v[1] & 1))
        return 0;
    v[0] = HEVC_CLOCK; v[1] = 0;
    *rate = fw_tag(0x00030002, v, 1, 2) == 0 ? v[1] : 0;
    return 1;
}

/* ---- set-up ---- */

static const err_t e_mem = { 0x81A500, "HEVCHW: no memory for the workspace" };
static const err_t e_map = { 0x81A501, "HEVCHW: couldn't map the HEVC block's registers" };
static const err_t e_clock = { 0x81A502, "HEVCHW: the HEVC clock won't turn on (BCMSupport?)" };

static volatile uint32_t *map_io(uint32_t phys, uint32_t size)
{
    R[0] = 13; R[1] = phys; R[2] = size;
    if (swi0(OS_Memory))
        return NULL;
    return (volatile uint32_t *)(uintptr_t)R[3];
}

static void *hal_lookup(int entry, void **sb)
{
    R[8] = 1; R[9] = (uint32_t)entry;        /* OS_Hardware 1: look up a HAL routine */
    if (swi0(OS_Hardware))
        return NULL;
    if (sb)
        *sb = (void *)(uintptr_t)R[1];
    return (void *)(uintptr_t)R[0];
}

static int hal_call(int entry, uint32_t arg)
{
    R[0] = arg; R[8] = 0; R[9] = (uint32_t)entry;   /* OS_Hardware 0: call a HAL routine */
    return swi0(OS_Hardware);
}

const err_t *hw_init(ws_t **pw)
{
    ws_t *w;
    R[0] = 6; R[3] = sizeof(ws_t);           /* OS_Module 6: claim */
    if (swi0(OS_Module))
        return &e_mem;
    w = (ws_t *)(uintptr_t)R[2];
    for (size_t i = 0; i < sizeof(ws_t); i++)
        ((uint8_t *)w)[i] = 0;
    *pw = w;
    w->intc = map_io(INTC_PHYS, INTC_SIZE);
    w->hevc = map_io(HEVC_PHYS, HEVC_SIZE);
    w->gicd = map_io(GICD_PHYS, GICD_SIZE);
    if (!w->intc || !w->hevc || !w->gicd) {   /* (no finalisation follows a failed init) */
        R[0] = 7; R[2] = (uint32_t)(uintptr_t)w;
        swi0(OS_Module);
        *pw = NULL;
        return &e_map;
    }
    w->hal_clear = hal_lookup(HAL_IRQClear, &w->hal_sb);
    w->hal_disable = hal_lookup(HAL_IRQDisable, NULL);
    return NULL;
}

static void irq_release(ws_t *w)
{
    if (!w->claimed)
        return;
    hal_call(HAL_IRQDisable, w->device);
    R[0] = w->device; R[1] = (uint32_t)(uintptr_t)hw_irq_handler; R[2] = (uint32_t)(uintptr_t)w;
    swi0(OS_ReleaseDeviceVector);
    w->claimed = 0;
}

const err_t *hw_final(ws_t **pw)
{
    ws_t *w = *pw;
    if (w) {
        if (w->intc)                          /* no test interrupt left on */
            w->intc[IC_ICTRL] = (w->intc[IC_ICTRL] & ~(ICTRL_TEST_INT | ICTRL_ZERO_MASK)) | ICTRL_INT_BITS;
        irq_release(w);
        R[0] = 7; R[2] = (uint32_t)(uintptr_t)w;
        swi0(OS_Module);
        *pw = NULL;
    }
    return NULL;
}

/* ---- the commands ---- */

static void info(ws_t *w)
{
    uint32_t v[2] = { HEVC_CLOCK, 0 }, on = 0, rate = 0;
    if (fw_tag(0x00030001, v, 1, 2) == 0) on = v[1] & 1;
    v[0] = HEVC_CLOCK; v[1] = 0;
    if (fw_tag(0x00030002, v, 1, 2) == 0) rate = v[1];
    put("HEVC clock: "); put(on ? "on, " : "off, "); dec(rate / 1000000u); put(" MHz"); nl();
    put("ICTRL "); hex8(w->intc[IC_ICTRL]); nl();
    if (on) {
        put("VERSION "); hex8(w->hevc[R_VERSION / 4]);
        put("  STATUS "); hex8(w->hevc[R_STATUS / 4]); nl();
    } else {
        put("(clock off: the HEVC registers aren't read)"); nl();
    }
    put("Interrupts seen: "); dec(w->irqs);
    if (w->claimed) { put(", device "); dec(w->device); put(" claimed"); }
    nl();
    put("HAL IRQClear "); put(w->hal_clear ? "found" : "not found");
    put(", IRQDisable "); put(w->hal_disable ? "found" : "not found"); nl();
}

static const err_t *reg_test(ws_t *w)
{
    static const uint32_t regs[3] = { R_PUWBASE, R_COEFFWBASE, R_COLBASE };
    static const uint32_t pats[2] = { 0xA5A5A5C0u, 0x5A5A5A40u };
    uint32_t rate;
    int ok = 1;
    if (!clock_on(&rate))
        return &e_clock;
    for (int i = 0; i < 3; i++) {
        volatile uint32_t *r = &w->hevc[regs[i] / 4];
        uint32_t was = *r, got[2];
        for (int k = 0; k < 2; k++) {
            *r = pats[k];
            got[k] = *r;
        }
        *r = was;
        put("  "); hex8(regs[i]); put(": wrote "); hex8(pats[0]); put(" read "); hex8(got[0]);
        put(", wrote "); hex8(pats[1]); put(" read "); hex8(got[1]); put(", back to "); hex8(*r); nl();
        if (got[0] == got[1] || got[0] == was)
            ok = 0;                           /* nothing stuck */
    }
    put(ok ? "Result: OK - the registers take what's written" : "Result: the registers didn't change");
    nl();
    return NULL;
}

/* IDs (32..255) pending at the GIC */
static void pending(ws_t *w, uint32_t *p)
{
    for (int i = 0; i < 8; i++)
        p[i] = w->gicd[(GICD_ISPENDR / 4) + i];
}

static const err_t *irq_test(ws_t *w)
{
    uint32_t before[8], during[8], ictrl, rate, t0, found = 0, n_new = 0;
    int id;
    if (!clock_on(&rate))
        return &e_clock;
    if (w->claimed)
        irq_release(w);
    ictrl = w->intc[IC_ICTRL] & ~(ICTRL_ZERO_MASK | ICTRL_INT_BITS | ICTRL_TEST_INT);
    if (w->gicd[GICD_ISENABLER / 4 + (HEVC_SPI + 32) / 32] & (1u << ((HEVC_SPI + 32) & 31))) {
        put("ID 130 is already enabled at the GIC (something else owns it?): not raising the test interrupt");
        nl();
        return NULL;
    }
    /* 1. the test interrupt with the device masked: which line goes pending? */
    pending(w, before);
    w->intc[IC_ICTRL] = ictrl | ICTRL_TEST_INT;
    (void)w->intc[IC_ICTRL];
    POKE(w);
    pending(w, during);
    w->intc[IC_ICTRL] = ictrl | ICTRL_INT_BITS;
    POKE(w);
    put("Test interrupt raised (device masked); newly pending at the GIC:");
    for (id = 32; id < 256; id++) {
        uint32_t bit = 1u << (id & 31);
        if ((during[id >> 5] & bit) && !(before[id >> 5] & bit)) {
            put(" "); dec((uint32_t)id);
            if (id == HEVC_SPI + 32) found = 1;
            n_new++;
        }
    }
    if (!n_new) put(" none");
    nl();
    if (!found) {
        put("Result: ID 130 (GIC_SPI 98) didn't go pending: not claiming anything");
        nl();
        return NULL;
    }
    /* 2. claim it and let one through */
    w->device = DEVICE_OF_ID(HEVC_SPI + 32);
    w->irqs = 0;
    R[0] = w->device; R[1] = (uint32_t)(uintptr_t)hw_irq_handler; R[2] = (uint32_t)(uintptr_t)w;
    if (swi0(OS_ClaimDeviceVector)) {
        put("Result: OS_ClaimDeviceVector refused device "); dec(w->device); nl();
        return NULL;
    }
    w->claimed = 1;
    hal_call(HAL_IRQEnable, w->device);
    w->intc[IC_ICTRL] = ictrl | ICTRL_TEST_INT;
    t0 = now_cs();
    while (!w->irqs && now_cs() - t0 < 20)
        ;
    put("Device "); dec(w->device); put(" claimed; interrupts in "); dec(now_cs() - t0);
    put(" cs: "); dec(w->irqs); nl();
    w->intc[IC_ICTRL] = ictrl | ICTRL_INT_BITS;        /* (the handler turned it off already) */
    irq_release(w);
    put(w->irqs ? "Result: OK - the block's interrupt reaches RISC OS" :
                  "Result: pending at the GIC, but no interrupt came to the handler");
    nl();
    return NULL;
}

static const err_t *mem_test(void)
{
    uint32_t pci_alloc, pci_free, log, phys, size = 1u << 20;
    R[1] = (uint32_t)(uintptr_t)"PCI_RAMAlloc";
    if (swi0(OS_SWINumberFromString)) {
        put("Result: no PCI_RAMAlloc (the PCI module isn't loaded?)"); nl();
        return NULL;
    }
    pci_alloc = R[0];
    R[1] = (uint32_t)(uintptr_t)"PCI_RAMFree";
    if (swi0(OS_SWINumberFromString)) {
        put("Result: no PCI_RAMFree"); nl();
        return NULL;
    }
    pci_free = R[0];
    R[0] = size; R[1] = 4096; R[2] = 0;
    if (swi0((int)pci_alloc)) {
        put("Result: PCI_RAMAlloc refused 1 MB"); nl();
        return NULL;
    }
    log = R[0]; phys = R[1];
    {
        volatile uint32_t *m = (volatile uint32_t *)(uintptr_t)log;
        m[0] = 0x12345678u; m[(size / 4) - 1] = 0x9ABCDEF0u;
        put("1 MB at logical "); hex8(log); put(", physical "); hex8(phys);
        put(phys < 0x40000000u ? ": below 1 GB, bus address " : ": above 1 GB (the block can't reach it)");
        if (phys < 0x40000000u) hex8(phys | 0xC0000000u);
        nl();
        put(m[0] == 0x12345678u && m[(size / 4) - 1] == 0x9ABCDEF0u ? "Written and read back" : "Read back wrong");
        nl();
    }
    R[0] = log;
    swi0((int)pci_free);
    put(phys < 0x40000000u ? "Result: OK - memory the block can use" : "Result: the memory is out of the block's reach");
    nl();
    return NULL;
}

const err_t *hw_command(const char *tail, int params, ws_t **pw, int n)
{
    ws_t *w = *pw;
    (void)tail; (void)params;
    switch (n) {
    case 0: info(w); return NULL;
    case 1: return reg_test(w);
    case 2: return irq_test(w);
    case 3: return mem_test();
    }
    return NULL;
}
