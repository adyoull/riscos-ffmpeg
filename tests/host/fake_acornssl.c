/*
 * A stand-in for RISC OS's AcornSSL module, for testing FFmpeg's AcornSSL
 * backend (patch 0018, libavformat/tls_acornssl.c) on Linux.
 *
 * It is linked into the arm-linux test build of ffmpeg/ffprobe (build/
 * build-ffmpeg.sh, LINUX_ARM_TEST) and passes the bytes through unchanged:
 * no encryption, so "https://" talks plain HTTP to a plain HTTP server.
 * What it does model is what the backend has to get right, taken from the
 * module's documentation and source (doc/AcornSSL, c/api):
 *   - the handshake happens inside Recv/Send, one step a call, each step
 *     failing with ENOTCONN until it's over (FAKE_ACORNSSL_STEPS, default 3);
 *   - FIONBIO through AcornSSL_Ioctl: EWOULDBLOCK instead of waiting;
 *   - MSG_PEEK, SO_ACORNSSL_HOSTNAME (recorded), Version;
 *   - AcornSSL_Close leaves a handed-over socket open;
 *   - errors as RISC OS error blocks: 0x20E00 + a BSD errno (not Linux's
 *     numbers) for socket errors,
 *     AcornSSL's own for a failed handshake (FAKE_ACORNSSL_FAIL=1).
 * FAKE_ACORNSSL_LOG=file appends one line per call, which run.sh checks:
 * the order of the calls, the host name, and that the socket was still
 * open when the session was closed. FAKE_ACORNSSL_ABSENT=1: the module
 * isn't loaded ("SWI not known").
 *
 * Also __get_ro_socket (UnixLib): on Linux the descriptor is the socket.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "kernel.h"

#define MAXH 16
static struct {
    int used, fd, steps, nbio;
    char host[256];
} h[MAXH];
static _kernel_oserror err;

int __get_ro_socket(int fd) { return fd; }

static void logf_(const char *fmt, ...)
{
    const char *f = getenv("FAKE_ACORNSSL_LOG");
    FILE *o;
    va_list ap;
    if (!f || !(o = fopen(f, "a")))
        return;
    va_start(ap, fmt);
    vfprintf(o, fmt, ap);
    va_end(ap);
    fputc('\n', o);
    fclose(o);
}

static _kernel_oserror *dci(int e, const char *msg)
{
    err.errnum = 0x20E00 + e;
    snprintf(err.errmess, sizeof(err.errmess), "%s", msg);
    return &err;
}

static int slot(long handle)
{
    int i = (int)handle - 0x5000;
    return i >= 0 && i < MAXH && h[i].used ? i : -1;
}

/* waits for the socket when the handle is blocking */
static void block_for(int i, short ev)
{
    struct pollfd p = { h[i].fd, ev, 0 };
    if (!h[i].nbio)
        poll(&p, 1, -1);
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    int i;
    swi &= ~0x20000;
    if (getenv("FAKE_ACORNSSL_ABSENT") || swi < 0x50F80 || swi > 0x50F90) {
        err.errnum = 0x1E6;
        snprintf(err.errmess, sizeof(err.errmess), "SWI &%X not known", swi);
        return &err;
    }
    switch (swi) {
    case 0x50F8D:                                           /* Version */
        out->r[0] = 150;
        return NULL;
    case 0x50F88:                                           /* CreateSession */
        for (i = 0; i < MAXH && h[i].used; i++)
            ;
        if (i == MAXH || in->r[1] != 0)
            return dci(22, "Invalid argument");
        memset(&h[i], 0, sizeof(h[i]));
        h[i].used = 1;
        h[i].fd = (int)in->r[0];
        h[i].steps = getenv("FAKE_ACORNSSL_STEPS") ? atoi(getenv("FAKE_ACORNSSL_STEPS")) : 3;
        out->r[0] = 0x5000 + i;
        logf_("createsession socket=%d", h[i].fd);
        return NULL;
    }
    if ((i = slot(in->r[0])) < 0) {
        err.errnum = 0x813F21;
        snprintf(err.errmess, sizeof(err.errmess), "Bad SSL handle");
        return &err;
    }
    switch (swi) {
    case 0x50F8B:                                           /* Setsockopt */
        if (in->r[1] == 0xFFFF && in->r[2] == 0x11E0 && in->r[4] == 4) {
            snprintf(h[i].host, sizeof(h[i].host), "%s", (const char *)(intptr_t)in->r[3]);
            logf_("hostname %s", h[i].host);
            out->r[0] = 0;
            return NULL;
        }
        return dci(42, "Protocol not available");
    case 0x50F81:                                           /* Ioctl */
        if ((unsigned long)in->r[1] == 0x8004667EUL) {
            h[i].nbio = *(int *)(intptr_t)in->r[2] != 0;
            logf_("fionbio %d", h[i].nbio);
            out->r[0] = 0;
            return NULL;
        }
        return dci(22, "Invalid argument");
    case 0x50F84: {                                         /* Close: the socket stays open */
        int open = fcntl(h[i].fd, F_GETFD) != -1;
        logf_("close socket_open=%d", open);
        h[i].used = 0;
        out->r[0] = 0;
        return NULL;
    }
    case 0x50F87: case 0x50F8F: {                           /* Recv, Send */
        ssize_t n;
        if (h[i].steps > 0) {                               /* the handshake, a step a call */
            h[i].steps--;
            if (getenv("FAKE_ACORNSSL_FAIL") && h[i].steps == 0) {
                logf_("handshake failed");
                err.errnum = 0x813F27;
                snprintf(err.errmess, sizeof(err.errmess), "Handshake error (state -9984)");
                return &err;
            }
            return dci(57, "Socket is not connected");   /* BSD ENOTCONN */
        }
        if (swi == 0x50F87) {
            if (in->r[2] > 0)
                block_for(i, POLLIN);
            n = recv(h[i].fd, (void *)(intptr_t)in->r[1], in->r[2],
                     (in->r[3] & 2) ? MSG_PEEK | MSG_DONTWAIT : MSG_DONTWAIT);
        } else {
            block_for(i, POLLOUT);
            n = send(h[i].fd, (const void *)(intptr_t)in->r[1], in->r[2], MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        if (n < 0) {
            if (errno == EAGAIN)
                return dci(35, "Operation would block");  /* the BSD number, as RISC OS gives */
            if (errno == ECONNRESET)
                return dci(54, "Connection reset by peer");
            return dci(errno == EPIPE ? 32 : 5, strerror(errno));   /* (BSD numbers) */
        }
        out->r[0] = (long)n;
        return NULL;
    }
    }
    return dci(22, "Invalid argument");
}
