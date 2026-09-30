/*
 * hw.h - HEVCHW: the Raspberry Pi 4's HEVC decoder block, from RISC OS.
 * Shared between the module's C (hevchw.c), its assembler (header.s) and
 * the host tests.
 *
 * Part of riscos-ffmpeg (hwhevc/). MIT licence.
 */
#ifndef HWHEVC_HW_H
#define HWHEVC_HW_H

#include <stdint.h>

/* where things are (Raspberry Pi Linux device tree, bcm2711*.dtsi; bus
   0x7Exxxxxx = ARM 0xFExxxxxx, and the ARM local block at 0xFF800000) */
#define PERI_BASE    0xFE000000u
#define HEVC_PHYS    (PERI_BASE + 0x00B00000u)      /* "hevc", 64 KB */
#define HEVC_SIZE    0x10000u
#define INTC_PHYS    (PERI_BASE + 0x00B10000u)      /* "intc", 4 KB */
#define INTC_SIZE    0x1000u
#define GICD_PHYS    0xFF841000u                     /* GIC-400 distributor (0x40041000 local) */
#define GICD_SIZE    0x1000u
#define HEVC_SPI     98                              /* GIC_SPI 98: interrupt ID 130 */
#define HEVC_CLOCK   11                              /* firmware clock: HEVC */
/* RISC OS's device numbers on the Pi 4: the V3D module uses device 10 for
   V3D (GIC_SPI 74, ID 106), so device = ID - 96; HEVC's would be 34. The
   IRQ test checks the line at the GIC before trusting this. */
#define DEVICE_OF_ID(id) ((id) - 96)

/* HEVC block registers (rpivid_hw.h) */
#define R_STATUS     0x38
#define R_VERSION    0x3C
#define R_PUWBASE    0x50
#define R_COEFFWBASE 0x58
#define R_COLBASE    0x8038
/* interrupt controller */
#define IC_ICTRL           0x0
#define ICTRL_INT_BITS     ((1u << 0) | (1u << 4) | (1u << 25) | (1u << 29))  /* latched: write 1 to clear */
#define ICTRL_TEST_INT     (1u << 8)                /* forces the interrupt line high */
#define ICTRL_ZERO_MASK    ((0xFFu << 12) | (1u << 11))
/* GIC distributor */
#define GICD_ISENABLER     0x100
#define GICD_ISPENDR       0x200

/* The module's workspace (in the RMA). The IRQ handler (header.s) uses
   the first five words: keep them in this order. */
typedef struct {
    volatile uint32_t *intc;          /* +0  */
    volatile uint32_t irqs;           /* +4  interrupts seen */
    uint32_t device;                  /* +8  the device claimed */
    void *hal_clear;                  /* +12 HAL_IRQClear */
    void *hal_sb;                     /* +16 the HAL's static base */
    void *hal_disable;                /* +20 HAL_IRQDisable */
    volatile uint32_t *hevc;
    volatile uint32_t *gicd;
    int claimed;                      /* the device vector is ours */
} ws_t;

#endif
