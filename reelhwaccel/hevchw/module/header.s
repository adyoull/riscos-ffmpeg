@ header.s - HEVCHW's module header, its entry veneers and its IRQ handler.
@ The C (hevchw.c) is freestanding: no C library, all SWIs through
@ hw_swi. Everything here is position independent (offsets from the
@ module's base); the link checks there are no absolute relocations.
@ Part of riscos-ffmpeg (reelhwaccel/hevchw/). GPL version 2 or later
@ (see COPYING).

        .syntax unified
        .arm
        .section .text.header, "ax"
        .global module_base
module_base:
        .word   0                               @ start
        .word   init_veneer - module_base       @ initialisation
        .word   final_veneer - module_base      @ finalisation
        .word   0                               @ service calls
        .word   title - module_base
        .word   help - module_base
        .word   cmd_table - module_base
        .word   0, 0, 0, 0                      @ no SWIs
        .word   0                               @ messages
        .word   flags - module_base

title:  .asciz  "HEVCHW"
help:   .asciz  "HEVCHW\t0.01 (30 Sep 2026) Pi 4 HEVC block tests (riscos-ffmpeg)"
        .balign 4
flags:  .word   1                               @ 32-bit compatible

@ *commands: name, code, info (min | max << 16), syntax, help
cmd_table:
        .asciz  "HEVCInfo"
        .balign 4
        .word   cmd_veneer0 - module_base, 0x00000000, syn_info - module_base, hlp_info - module_base
        .asciz  "HEVCRegTest"
        .balign 4
        .word   cmd_veneer1 - module_base, 0x00000000, syn_reg - module_base, hlp_reg - module_base
        .asciz  "HEVCIRQTest"
        .balign 4
        .word   cmd_veneer2 - module_base, 0x00000000, syn_irq - module_base, hlp_irq - module_base
        .asciz  "HEVCMemTest"
        .balign 4
        .word   cmd_veneer3 - module_base, 0x00000000, syn_mem - module_base, hlp_mem - module_base
        .word   0

syn_info: .asciz "Syntax: *HEVCInfo"
hlp_info: .asciz "*HEVCInfo shows the HEVC block's clock, version, status and interrupt control.\r"
syn_reg:  .asciz "Syntax: *HEVCRegTest"
hlp_reg:  .asciz "*HEVCRegTest writes patterns to three of the block's address registers, reads them back and puts them back to 0.\r"
syn_irq:  .asciz "Syntax: *HEVCIRQTest"
hlp_irq:  .asciz "*HEVCIRQTest makes the block raise its test interrupt: finds its line at the GIC, then claims it and counts the interrupts that arrive.\r"
syn_mem:  .asciz "Syntax: *HEVCMemTest"
hlp_mem:  .asciz "*HEVCMemTest allocates physically contiguous memory the block could use (PCI_RAMAlloc) and shows where it is.\r"
        .balign 4

@ Errors: C returns 0 or a pointer to a RISC OS error block; V set for one.
        .macro  ret_err
        cmp     r0, #0
        ldmfdeq sp!, {r4-r11, pc}               @ (cmp #0 leaves V clear)
        cmp     r0, #0x80000000
        cmnvc   r0, #0x80000000                 @ V set, whatever r0 is
        ldmfd   sp!, {r4-r11, pc}
        .endm

        .global init_veneer, final_veneer     @ (global for the host test)
init_veneer:                                    @ r10 env, r11 instance, r12 -> private word
        stmfd   sp!, {r4-r11, lr}
        mov     r0, r12
        bl      hw_init
        ret_err

final_veneer:                                   @ r12 -> private word
        stmfd   sp!, {r4-r11, lr}
        mov     r0, r12
        bl      hw_final
        ret_err

        .macro  cmd n
        .global cmd_veneer\n
cmd_veneer\n:                                   @ r0 tail, r1 parameters, r12 -> private word
        stmfd   sp!, {r4-r11, lr}
        mov     r2, r12
        mov     r3, #\n
        bl      hw_command
        ret_err
        .endm
        cmd 0
        cmd 1
        cmd 2
        cmd 3

@ The device vector handler: IRQ mode, IRQs off, r12 = the workspace
@ (hw.h's ws_t). May corrupt r0-r3 and r12. Clears the block's latched
@ interrupts and the test interrupt, counts, disables the device if it
@ keeps coming (a storm guard), and tells the GIC it's done.
        .global hw_irq_handler
hw_irq_handler:
        stmfd   sp!, {r4, r9, lr}
        mov     r4, r12
        ldr     r0, [r4, #0]                    @ the interrupt controller
        ldr     r1, [r0]                        @ ICTRL
        bic     r1, r1, #0x100                  @ TEST_INT off
        bic     r1, r1, #0xFF000                @ reserved bits 12-19 and 11: write 0
        bic     r1, r1, #0x800
        str     r1, [r0]                        @ (latched bits written back as 1: cleared)
        ldr     r1, [r4, #4]
        add     r1, r1, #1
        str     r1, [r4, #4]
        cmp     r1, #64
        blo     1f
        ldr     r0, [r4, #8]                    @ a storm: disable the device
        ldr     r9, [r4, #16]
        mov     lr, pc
        ldr     pc, [r4, #20]                   @ HAL_IRQDisable(device)
1:      ldr     r0, [r4, #8]
        ldr     r9, [r4, #16]
        mov     lr, pc
        ldr     pc, [r4, #12]                   @ HAL_IRQClear(device)
        ldmfd   sp!, {r4, r9, pc}

@ int hw_swi(int swi, uint32_t *r): r[0..9] in and out; returns 0, or the
@ error block's address (the X form, through OS_CallASWIR12).
        .global hw_swi
hw_swi:
        stmfd   sp!, {r4-r11, lr}
        mov     r12, r0
        orr     r12, r12, #0x20000              @ X bit
        mov     r11, r1
        ldmia   r11, {r0-r9}
        swi     0x20071                         @ XOS_CallASWIR12
        stmia   r11, {r0-r9}
        movvc   r0, #0
        ldmfd   sp!, {r4-r11, pc}

        .section .note.GNU-stack, "", %progbits
