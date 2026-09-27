/* Host fake of UnixLib's <kernel.h>, for tests/host. */
#ifndef FAKE_KERNEL_H
#define FAKE_KERNEL_H
typedef struct { long r[10]; } _kernel_swi_regs;   /* int on RISC OS; long so 64-bit host tests can pass pointers */
typedef struct { int errnum; char errmess[252]; } _kernel_oserror;
_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out);
#endif
