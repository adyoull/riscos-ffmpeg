/* Lets riscos-mesa's RISC OS libraries (libEGL.a, libOSMesa.a from the
 * devkit: GCCSDK, arm hard float, UnixLib) run in an arm-linux program
 * under qemu, for mesa_eglimage_test. The libraries are copied with their
 * UnixLib-only symbols renamed (ro_*, see run.sh) and those are provided
 * here: stdio streams, the ctype table, errno, and pthreads (the test is
 * single threaded; UnixLib's pthread types are smaller than glibc's).
 * _kernel_swi fakes the few SWIs libEGL asks for a pbuffer and pixmaps. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

FILE *ro___stdin, *ro___stdout, *ro___stderr;
int ro_errno;
static unsigned char ctype_tab[257];
const unsigned char *const ro___ctype = ctype_tab + 1;

__attribute__((constructor(101))) static void shim_init(void)
{
    ro___stdin = stdin; ro___stdout = stdout; ro___stderr = stderr;
    for (int c = 0; c < 256; c++)                  /* UnixLib's bits, C locale */
        ctype_tab[c + 1] = (iscntrl(c) ? 1 : 0) | (isupper(c) ? 2 : 0) | (islower(c) ? 4 : 0) |
                           (isalpha(c) ? 8 : 0) | (ispunct(c) ? 16 : 0) | (isspace(c) ? 32 : 0) |
                           (isdigit(c) ? 64 : 0) | (isxdigit(c) ? 128 : 0);
}

/* pthreads: one thread */
int ro_pthread_mutex_init(void *m, const void *a) { return 0; }
int ro_pthread_mutex_destroy(void *m) { return 0; }
int ro_pthread_mutex_lock(void *m) { return 0; }
int ro_pthread_mutex_unlock(void *m) { return 0; }
int ro_pthread_mutexattr_init(void *a) { return 0; }
int ro_pthread_mutexattr_destroy(void *a) { return 0; }
int ro_pthread_mutexattr_settype(void *a, int t) { return 0; }
int ro_pthread_cond_init(void *c, const void *a) { return 0; }
int ro_pthread_cond_destroy(void *c) { return 0; }
int ro_pthread_cond_signal(void *c) { return 0; }
int ro_pthread_cond_broadcast(void *c) { return 0; }
int ro_pthread_cond_wait(void *c, void *m) { abort(); }
int ro_pthread_cond_timedwait(void *c, void *m, const void *t) { abort(); }
int ro_pthread_create(void *t, const void *a, void *(*f)(void *), void *arg) { return 11; /* EAGAIN */ }
int ro_pthread_join(unsigned long t, void **r) { return 3; }
int ro_pthread_sigmask(int h, const void *s, void *o) { return 0; }
unsigned long ro_pthread_self(void) { return 1; }
int ro_pthread_equal(unsigned long a, unsigned long b) { return a == b; }
int ro_pthread_once(int *once, void (*f)(void))
{
    if (!*once) { *once = 1; f(); }
    return 0;
}
static void *keys[64]; static int nkeys;
int ro_pthread_key_create(unsigned *k, void (*d)(void *)) { *k = nkeys++; return 0; }
int ro_pthread_key_delete(unsigned k) { return 0; }
void *ro_pthread_getspecific(unsigned k) { return keys[k]; }
int ro_pthread_setspecific(unsigned k, const void *v) { keys[k] = (void *)v; return 0; }

/* SWIs. The screen: 640x480, 32bpp TBGR, eig 1. */
#define OS_ReadModeVariable 0x35
#define OS_ReadVduVariables 0x31
/* libavformat's AcornSSL backend (patch 0018) refers to UnixLib's
   __get_ro_socket */
__attribute__((weak)) int __get_ro_socket(int fd) { return fd; }

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror err = { 1, "not faked" };
    switch (swi & ~0x20000) {
    case OS_ReadModeVariable: {
        unsigned m = (unsigned)in->r[0];
        int log2bpp = -1, flags = 0;
        if (m == (unsigned)-1 || m == (1 | (90 << 1) | (90 << 14) | (6u << 27))) log2bpp = 5;
        else if (m > 255 && !(m & 1)) {                       /* a mode selector */
            const int *sel = (const int *)(unsigned long)m;
            log2bpp = sel[3];
            for (const int *p = sel + 5; *p != -1; p += 2) if (p[0] == 0) flags = p[1];
        } else if ((m >> 27) != 0) log2bpp = (m >> 27) == 6 ? 5 : -1;
        if (log2bpp < 0) return &err;
        out->r[2] = in->r[1] == 9 ? log2bpp : in->r[1] == 0 ? flags :
                    in->r[1] == 4 || in->r[1] == 5 ? 1 : in->r[1] == 11 ? 639 : in->r[1] == 12 ? 479 : 0;
        return NULL;
    }
    case OS_ReadVduVariables: {
        int *vars = (int *)(long)in->r[0], *vals = (int *)(long)in->r[1];
        for (; *vars != -1; vars++, vals++)
            *vals = *vars == 4 || *vars == 5 ? 1 : *vars == 9 ? 5 : *vars == 11 ? 639 : *vars == 12 ? 479 : 0;
        return NULL;
    }
    default:
        if (getenv("SHIM_TRACE")) fprintf(stderr, "shim: SWI &%x\n", swi);
        return &err;
    }
}
/* (riscos-mesa 20.3.5-12's libEGL reads pixmap sprites' modes with the C flag) */
_kernel_oserror *_kernel_swi_c(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out, int *carry)
{
    if (carry) *carry = 0;
    return _kernel_swi(swi, in, out);
}
int _kernel_osbyte(int a, int x, int y) { return 0; }
int _kernel_oswrch(int c) { return 0; }
