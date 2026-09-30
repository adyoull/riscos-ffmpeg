/*
 * HEVCHW (reelhwaccel/hevchw/module) against a fake RISC OS, a fake firmware, a fake
 * HAL and memory standing in for the HEVC block, its interrupt control and
 * the GIC. The module's own header.s is linked in (its hw_swi weakened), so
 * the veneers and the assembler IRQ handler are the real ones.
 *   - init maps the three blocks; a failed map frees the workspace and the
 *     veneer returns the error with V set;
 *   - *HEVCInfo shows VERSION with the clock on, nothing read with it off;
 *   - *HEVCRegTest turns the clock on (at its maximum), writes, reads back
 *     and restores; a clock that won't turn on is an error;
 *   - *HEVCIRQTest: ID 130 goes pending -> device 34 claimed, enabled, the
 *     handler counts, clears the test interrupt, HAL_IRQClear, released;
 *     another ID pending -> nothing claimed; ID 130 already enabled ->
 *     the test interrupt isn't raised; the handler's storm guard;
 *   - *HEVCMemTest: below 1 GB, above 1 GB, no PCI module;
 *   - finalisation releases the device and frees the workspace.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../reelhwaccel/hevchw/module/hw.h"

typedef struct { int errnum; char errmess[64]; } err_t;
const err_t *hw_init(ws_t **pw);
const err_t *hw_final(ws_t **pw);
const err_t *hw_command(const char *tail, int params, ws_t **pw, int n);
void hw_irq_handler(void);
void init_veneer(void), final_veneer(void), cmd_veneer0(void), cmd_veneer1(void);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static char out[8192];
static uint32_t intc[0x400], hevc[0x4000], gicd[0x400];
/* (volatile: the veneers and the handler are asm calls, which GCC doesn't
   see modifying these through hw_swi and the HAL fakes) */
static volatile int clk_on, clk_rate, clk_max = 500000000, clk_refuses, map_fail, claims, releases, enabled_dev = -1,
           disabled_dev = -1, cleared, cleared_dev, time_cs, claim_refuses, no_pci, pci_phys, freed_pci,
           rma_claims, rma_frees, pend_id = 130;
static volatile uintptr_t claim_code, claim_ws;
static uint32_t pci_mem[1 << 18];
static err_t e = { 1, "fake" };

static void hal_clear(int dev) { cleared++; cleared_dev = dev; }
static void hal_disable(int dev) { disabled_dev = dev; }

static uint32_t mask_bit(int id) { return 1u << (id & 31); }

/* the fake GIC: the block's line is level-triggered on pend_id */
void hw_test_poke(ws_t *w)
{
    (void)w;
    if (intc[0] & ICTRL_TEST_INT) gicd[GICD_ISPENDR / 4 + pend_id / 32] |= mask_bit(pend_id);
    else gicd[GICD_ISPENDR / 4 + pend_id / 32] &= ~mask_bit(pend_id);
}

/* call the real handler as the kernel would: r12 = the workspace */
static void irq(uintptr_t ws)
{
    register uintptr_t r12 __asm__("r12") = ws;
    __asm__ volatile("blx %1" : "+r"(r12) : "r"((uintptr_t)hw_irq_handler)
                     : "r0", "r1", "r2", "r3", "lr", "memory", "cc");
}

static void clear_fake_ictrl(void)
{
    /* what the hardware does with a write: latched bits written 1 clear */
    intc[0] &= ~ICTRL_INT_BITS;
}

int hw_swi(int n, uint32_t *r)
{
    switch (n) {
    case 0x02: strncat(out, (const char *)(uintptr_t)r[0], sizeof out - strlen(out) - 1); return 0;
    case 0x03: strncat(out, "\n", sizeof out - strlen(out) - 1); return 0;
    case 0x1E:
        if (r[0] == 6) { rma_claims++; r[2] = (uint32_t)(uintptr_t)malloc(r[3]); return 0; }
        if (r[0] == 7) { rma_frees++; free((void *)(uintptr_t)r[2]); return 0; }
        break;
    case 0x39: {
        const char *s = (const char *)(uintptr_t)r[1];
        if (no_pci) return (int)(uintptr_t)&e;
        r[0] = !strcmp(s, "PCI_RAMAlloc") ? 0x50100 : !strcmp(s, "PCI_RAMFree") ? 0x50101 : 0;
        return r[0] ? 0 : (int)(uintptr_t)&e;
    }
    case 0x42:
        time_cs++;
        /* the device, if claimed and enabled, interrupts while the line is up */
        if (claim_code && enabled_dev == 34 && (intc[0] & ICTRL_TEST_INT) && pend_id == 130) {
            irq(claim_ws);
            clear_fake_ictrl();
        }
        r[0] = (uint32_t)time_cs;
        return 0;
    case 0x4B:
        if (claim_refuses) return (int)(uintptr_t)&e;
        claims++; CHECK(r[0] == 34, "claimed device %u", r[0]);
        claim_code = r[1]; claim_ws = r[2];
        return 0;
    case 0x4C:
        releases++; CHECK(r[1] == claim_code && r[2] == claim_ws, "released a different claim");
        claim_code = 0;
        return 0;
    case 0x68:
        if (r[0] == 13) {
            if (map_fail && r[1] == HEVC_PHYS) return (int)(uintptr_t)&e;
            r[3] = r[1] == INTC_PHYS ? (uint32_t)(uintptr_t)intc : r[1] == HEVC_PHYS ? (uint32_t)(uintptr_t)hevc :
                   r[1] == GICD_PHYS ? (uint32_t)(uintptr_t)gicd : 0;
            return r[3] ? 0 : (int)(uintptr_t)&e;
        }
        break;
    case 0x7A:
        if (r[8] == 1) {
            r[0] = r[9] == 3 ? (uint32_t)(uintptr_t)hal_clear : r[9] == 2 ? (uint32_t)(uintptr_t)hal_disable : 0;
            r[1] = 0x1234;
            return r[0] ? 0 : (int)(uintptr_t)&e;
        }
        if (r[8] == 0) {
            if (r[9] == 1) enabled_dev = (int)r[0];
            if (r[9] == 2) { disabled_dev = (int)r[0]; if (enabled_dev == (int)r[0]) enabled_dev = -1; }
            return 0;
        }
        break;
    case 0x591C5: {
        uint32_t *b = (uint32_t *)(uintptr_t)r[0], *v = b + 5;
        b[1] = 0x80000000u;
        CHECK(v[0] == HEVC_CLOCK, "clock %u", v[0]);
        switch (b[2]) {
        case 0x00030001: v[1] = (uint32_t)clk_on; break;
        case 0x00030002: v[1] = clk_on ? (uint32_t)clk_rate : 0; break;
        case 0x00030004: v[1] = (uint32_t)clk_max; break;
        case 0x00038002: clk_rate = (int)v[1]; break;
        case 0x00038001: if (!clk_refuses) clk_on = (int)(v[1] & 1); v[1] = (uint32_t)clk_on; break;
        default: b[1] = 0x80000001u;
        }
        return 0;
    }
    case 0x50100:
        CHECK(r[0] == 1u << 20, "PCI_RAMAlloc size %u", r[0]);
        r[0] = (uint32_t)(uintptr_t)pci_mem; r[1] = (uint32_t)pci_phys;
        return 0;
    case 0x50101: freed_pci++; CHECK(r[0] == (uint32_t)(uintptr_t)pci_mem, "freed a different block"); return 0;
    }
    CHECK(0, "unexpected SWI &%X", n);
    return (int)(uintptr_t)&e;
}

/* call a veneer as the kernel would: r12 = the private word; returns r0, V in *v */
static uintptr_t veneer(void (*fn)(void), ws_t **pw, int *v)
{
    register uintptr_t r12 __asm__("r12") = (uintptr_t)pw;
    register uintptr_t r0 __asm__("r0") = 0;
    register uintptr_t r1 __asm__("r1") = 0;
    uint32_t psr;
    __asm__ volatile("blx %4\n\tmrs %2, apsr" : "+r"(r0), "+r"(r12), "=r"(psr), "+r"(r1) : "r"((uintptr_t)fn)
                     : "r2", "r3", "lr", "memory", "cc");
    *v = (psr >> 28) & 1;
    return r0;
}

static void reset(void)
{
    out[0] = 0;
    memset(intc, 0, sizeof intc); memset(hevc, 0, sizeof hevc); memset(gicd, 0, sizeof gicd);
    intc[0] = 0x88800044u;
    hevc[R_VERSION / 4] = 0x202; hevc[R_STATUS / 4] = 1;
    clk_on = 1; clk_rate = 250000000; clk_refuses = map_fail = claim_refuses = no_pci = 0;
    claims = releases = cleared = 0; enabled_dev = disabled_dev = -1; claim_code = 0;
    pend_id = 130; pci_phys = 0x3E000000; freed_pci = 0; time_cs = 0;
}

int main(void)
{
    ws_t *w = NULL;
    const err_t *er;
    int v;
    uintptr_t r0;

    /* a failed map: the workspace freed, the error returned with V set */
    reset(); map_fail = 1; rma_claims = rma_frees = 0;
    r0 = veneer(init_veneer, &w, &v);
    CHECK(v && r0 && ((const err_t *)r0)->errnum == 0x81A501, "failed init: V %d r0 %lx", v, (unsigned long)r0);
    CHECK(!w && rma_claims == 1 && rma_frees == 1, "failed init leaves the workspace (%d/%d %p)", rma_claims, rma_frees, (void *)w);

    /* init through the veneer: V clear */
    reset();
    r0 = veneer(init_veneer, &w, &v);
    CHECK(!v && !r0 && w, "init: V %d r0 %lx", v, (unsigned long)r0);
    CHECK(w->intc == intc && w->hevc == hevc && w->gicd == gicd, "the three blocks mapped");
    CHECK(w->hal_clear == (void *)hal_clear && w->hal_disable == (void *)hal_disable && w->hal_sb == (void *)0x1234,
          "HAL routines found");
    CHECK(offsetof(ws_t, intc) == 0 && offsetof(ws_t, irqs) == 4 && offsetof(ws_t, device) == 8 &&
          offsetof(ws_t, hal_clear) == 12 && offsetof(ws_t, hal_sb) == 16 && offsetof(ws_t, hal_disable) == 20,
          "the handler's workspace offsets");

    /* *HEVCInfo, through the command veneer */
    r0 = veneer(cmd_veneer0, &w, &v);
    CHECK(!v && !r0, "info veneer");
    CHECK(strstr(out, "HEVC clock: on, 250 MHz") && strstr(out, "VERSION &00000202") && strstr(out, "STATUS &00000001")
          && strstr(out, "ICTRL &88800044") && strstr(out, "HAL IRQClear found, IRQDisable found"), "info:\n%s", out);
    printf("%s", out);
    out[0] = 0; clk_on = 0; hevc[R_VERSION / 4] = 0xDEAD;
    hw_command("", 0, &w, 0);
    CHECK(strstr(out, "clock off") && !strstr(out, "VERSION"), "info, clock off:\n%s", out);

    /* *HEVCRegTest: the clock on at its maximum, restored registers */
    reset(); clk_on = 0; clk_rate = 0;
    hevc[R_COLBASE / 4] = 0x1000;
    er = hw_command("", 0, &w, 1);
    CHECK(!er && clk_on && clk_rate == clk_max, "regtest: clock on at max (%d %d)", clk_on, clk_rate);
    CHECK(strstr(out, "Result: OK"), "regtest:\n%s", out);
    CHECK(hevc[R_COLBASE / 4] == 0x1000 && hevc[R_PUWBASE / 4] == 0, "regtest restores the registers");
    printf("%s", out);
    reset(); clk_on = 0; clk_refuses = 1;
    er = hw_command("", 0, &w, 1);
    CHECK(er && er->errnum == 0x81A502, "regtest with no clock: an error");
    out[0] = 0;
    r0 = veneer(cmd_veneer1, &w, &v);          /* the veneer's error path: V set */
    CHECK(v && r0 && ((const err_t *)r0)->errnum == 0x81A502, "regtest veneer: V %d", v);

    /* *HEVCIRQTest: ID 130 goes pending, the device claimed, one interrupt */
    reset();
    w->irqs = 0;
    er = hw_command("", 0, &w, 2);
    CHECK(!er && strstr(out, "newly pending at the GIC: 130") && strstr(out, "Result: OK"), "irqtest:\n%s", out);
    CHECK(claims == 1 && releases == 1 && !w->claimed && w->irqs >= 1, "claimed %d released %d irqs %u", claims, releases, w->irqs);
    CHECK(cleared >= 1 && cleared_dev == 34, "HAL_IRQClear(34) from the handler (%d, %d)", cleared, cleared_dev);
    CHECK(disabled_dev == 34 && enabled_dev == -1, "the device disabled again");
    CHECK(!(intc[0] & ICTRL_TEST_INT) && !(intc[0] & ICTRL_ZERO_MASK), "ICTRL left clean: %08X", intc[0]);
    printf("%s", out);

    /* another line: nothing claimed */
    reset(); pend_id = 131;
    er = hw_command("", 0, &w, 2);
    CHECK(!er && strstr(out, "pending at the GIC: 131") && strstr(out, "not claiming") && !claims, "irqtest, ID 131:\n%s", out);
    CHECK(!(intc[0] & ICTRL_TEST_INT), "test interrupt off again");

    /* nothing at all */
    /* a line that was pending already: nothing new */
    reset(); pend_id = 131; gicd[GICD_ISPENDR / 4 + 131 / 32] |= mask_bit(131);
    er = hw_command("", 0, &w, 2);
    CHECK(!er && strstr(out, "GIC: none") && !claims, "irqtest, none:\n%s", out);

    /* ID 130 already enabled: the test interrupt isn't raised */
    reset(); gicd[GICD_ISENABLER / 4 + 130 / 32] |= mask_bit(130);
    er = hw_command("", 0, &w, 2);
    CHECK(!er && strstr(out, "already enabled") && !claims && !(intc[0] & ICTRL_TEST_INT), "irqtest, enabled:\n%s", out);

    /* the vector refused */
    reset(); claim_refuses = 1;
    er = hw_command("", 0, &w, 2);
    CHECK(!er && strstr(out, "refused device 34") && !w->claimed, "irqtest, refused:\n%s", out);

    /* the handler's storm guard: the 64th interrupt disables the device */
    reset(); w->irqs = 0; w->device = 34; disabled_dev = -1;
    for (int i = 0; i < 63; i++) irq((uintptr_t)w);
    CHECK(disabled_dev == -1 && w->irqs == 63, "no disable before 64");
    intc[0] = 0x88800044u | ICTRL_TEST_INT | (1u << 11) | (0x5u << 12);
    irq((uintptr_t)w);
    CHECK(disabled_dev == 34 && w->irqs == 64, "disabled at 64 (%d)", disabled_dev);
    CHECK(intc[0] == 0x88800044u,
          "handler's ICTRL write: %08X", intc[0]);

    /* *HEVCMemTest */
    reset();
    er = hw_command("", 0, &w, 3);
    CHECK(!er && strstr(out, "physical &3E000000: below 1 GB, bus address &FE000000") && strstr(out, "Written and read back")
          && strstr(out, "Result: OK") && freed_pci == 1, "memtest:\n%s", out);
    printf("%s", out);
    reset(); pci_phys = 0x40100000;
    hw_command("", 0, &w, 3);
    CHECK(strstr(out, "above 1 GB") && strstr(out, "out of the block's reach") && freed_pci == 1, "memtest high:\n%s", out);
    reset(); no_pci = 1;
    hw_command("", 0, &w, 3);
    CHECK(strstr(out, "no PCI_RAMAlloc"), "memtest, no PCI:\n%s", out);

    /* finalisation: a claimed device released, the workspace freed */
    reset(); rma_frees = 0;
    claim_code = (uintptr_t)hw_irq_handler; claim_ws = (uintptr_t)w; w->claimed = 1; w->device = 34;
    intc[0] |= ICTRL_TEST_INT;
    r0 = veneer(final_veneer, &w, &v);
    CHECK(!v && !r0 && !w && releases == 1 && rma_frees == 1 && !(intc[0] & ICTRL_TEST_INT), "final: v %d r0 %lx w %p releases %d frees %d ictrl %08X", v, (unsigned long)r0, (void *)w, releases, rma_frees, intc[0]);

    printf(fails ? "hevchw_test: %d failures\n" : "hevchw_test: all passed\n", fails);
    return fails != 0;
}
