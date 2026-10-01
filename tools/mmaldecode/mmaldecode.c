/*
 * mmaldecode - decode an H.264 stream on the VideoCore, through RISC OS's
 * VCHIQ module and MMAL, and check every picture.
 *
 * Step 3 of the H.264 hardware track (every Pi from the Pi 1 to the Pi 4).
 * mmalprobe showed the firmware's ril.video_decode answers; this makes it
 * decode:
 *
 *   1. the input port is set to H.264 at the stream's size, the output to
 *      I420 at that size (32 x 16 aligned), 3 buffers each;
 *   2. the component and both ports are enabled;
 *   3. the stream goes in, in 64 KB pieces: each a BUFFER_FROM_HOST
 *      message, the data after it by VCHIQ bulk transfer (or in the
 *      message itself if 128 bytes or less); the last piece carries EOS;
 *   4. empty output buffers are handed over; each picture comes back as
 *      BUFFER_TO_HOST, then its bytes by a bulk receive into our memory,
 *      whose completion the RMA callback counts; an empty EOS buffer
 *      still gets an 8 byte receive (as Linux's driver), to keep order;
 *   5. a FORMAT_CHANGED event on the output is honoured: port disabled,
 *      the new format set, re-enabled, the buffers handed over again;
 *   6. each picture's visible part is checksummed (Adler-32 of the packed
 *      I420, as FFmpeg's -f framecrc) and compared with the list made by
 *      FFmpeg's own decoder; the time from first input to EOS is timed.
 *
 *   mmaldecode [-o file] [-n] stream.h264 expected.crc
 *      -o file   also add the report to file
 *      -n        don't stop at the first wrong picture
 *
 * The callback is 9 instructions in the RMA: it counts BULK_RECEIVED (4)
 * and BULK_RECEIVE_ABORTED (18) callbacks in two words after it (its
 * param). Nothing else runs outside this program.
 *
 * Message layouts from Linux's vchiq-mmal (mmal-msg.h); the VCHIQ SWI
 * conventions from RISC OS's VCHIQ, BCMSound and BCMVideo (see mmalprobe).
 * Part of riscos-ffmpeg. MIT licence.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

#define OS_Module                 0x1E
#define OS_ReadMonotonicTime      0x42
#define OS_SynchroniseCodeAreas   0x6E
#define VCHIQ_Initialise          0x59200
#define VCHIQ_Connect             0x59201
#define VCHIQ_Disconnect          0x59202
#define VCHIQ_BulkQueueTransmit   0x59203
#define VCHIQ_MsgDequeue          0x59204
#define VCHIQ_MsgQueue            0x59205
#define VCHIQ_ServiceClose        0x59209
#define VCHIQ_ServiceOpen         0x5920A
#define VCHIQ_ServiceUse          0x5920D
#define VCHIQ_ServiceRelease      0x5920E
#define VCHIQ_BulkQueueReceive    0x5920F
/* VCHIQ_BulkQueueReceive takes only 0/4 (no callback), 1 (wait) or 6 (a
   callback when done): 2 on its own is refused (from the module's code) */
#define VCHI_FLAGS_CALLBACK_WHEN_OP_COMPLETE 2
#define VCHI_FLAGS_CALLBACK_WHEN_DONE        6
#define VCHI_FLAGS_BLOCK_UNTIL_QUEUED        4

#define FOURCC_BE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FOURCC_LE(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MMAL_SERVICE   FOURCC_BE('m', 'm', 'a', 'l')
#define MMAL_MAGIC     FOURCC_LE('m', 'm', 'a', 'l')
#define ENC_H264       FOURCC_LE('H', '2', '6', '4')
#define ENC_I420       FOURCC_LE('I', '4', '2', '0')
#define EV_FORMAT_CHANGED FOURCC_LE('E', 'F', 'C', 'H')
#define EV_ERROR       FOURCC_LE('E', 'R', 'R', 'O')
#define EV_EOS         FOURCC_LE('E', 'E', 'O', 'S')

enum { T_COMPONENT_CREATE = 4, T_COMPONENT_DESTROY, T_COMPONENT_ENABLE, T_COMPONENT_DISABLE, T_PORT_INFO_GET,
       T_PORT_INFO_SET, T_PORT_ACTION, T_BUFFER_FROM_HOST, T_BUFFER_TO_HOST, T_EVENT_TO_HOST = 16 };
enum { PORT_CONTROL = 1, PORT_INPUT = 2, PORT_OUTPUT = 3 };
enum { ACTION_ENABLE = 1, ACTION_DISABLE = 2 };
enum { ES_VIDEO = 3 };
#define FLAG_EOS       1u
#define TIME_UNKNOWN   0x8000000000000000ull
#define SHORT_DATA     128
#define IN_BUFS        3
#define IN_SIZE        (64 * 1024)
#define OUT_BUFS       3
#define REPLY_CS       300
#define IDLE_CS        500                 /* no progress for this long: give up */

typedef struct { uint32_t magic, type, control_service, context, status, padding; } hdr_t;
typedef struct {
    uint32_t priv, name, type;
    uint16_t index, index_all;
    uint32_t is_enabled, format, buffer_num_min, buffer_size_min, buffer_alignment_min,
             buffer_num_recommended, buffer_size_recommended, buffer_num, buffer_size,
             component, userdata, capabilities;
} port_t;
typedef struct { uint32_t type, encoding, encoding_variant, es, bitrate, flags, extradata_size, extradata; } fmt_t;
typedef struct { uint32_t width, height; int32_t crop[4], frame_rate[2], par[2]; uint32_t color_space; } video_t;
typedef struct {                           /* PORT_INFO_GET/SET replies */
    uint32_t status, component_handle, port_type, port_index;
    int32_t found;
    uint32_t port_handle;
    port_t port;
    fmt_t format;
    video_t video;
    uint8_t extradata[128];
} port_info_t;
typedef struct {
    uint32_t component_handle, port_type, port_index;
    port_t port;
    fmt_t format;
    video_t video;
    uint8_t extradata[128];
} port_set_t;
typedef struct { uint32_t magic, component_handle, port_handle, client_context; } drvbuf_t;
typedef struct {
    drvbuf_t drvbuf, drvbuf_ref;
    uint32_t next, priv, cmd, data, alloc_size, length, offset, flags;
    uint64_t pts, dts;
    uint32_t type, user_data;
    uint32_t planes, offsets[4], pitch[4], vflags;
    int32_t is_zero_copy, has_reference;
    uint32_t payload_in_message;
    uint8_t short_data[SHORT_DATA];
} buffer_msg_t;
typedef struct {
    uint32_t client_component, port_type, port_num, cmd, length;
    uint8_t data[256];
    uint32_t delayed_buffer;
} event_msg_t;
typedef struct {                           /* an EFCH event's data */
    uint32_t buffer_size_min, buffer_num_min, buffer_size_recommended, buffer_num_recommended, es_ptr;
    fmt_t format;
    video_t video;
} format_changed_t;

typedef char chk_hdr[sizeof(hdr_t) == 24 ? 1 : -1];
typedef char chk_port[sizeof(port_t) == 64 ? 1 : -1];
typedef char chk_video[sizeof(video_t) == 44 ? 1 : -1];
typedef char chk_buf[offsetof(buffer_msg_t, pts) == 64 && sizeof(buffer_msg_t) == 272 ? 1 : -1];   /* (as Linux's: 8-aligned) */
typedef char chk_set[sizeof(port_set_t) == 280 ? 1 : -1];

static FILE *out2;
static int verbose;

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
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);   /* tests/host/mmaldecode_test.c */
void probe_svc_copy(void *dst, const void *src, size_t n);
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* word copy in SVC mode: the RMA needn't be user accessible on every
   RISC OS 5 (both ends touched in USR mode first when they're ours) */
static void probe_svc_copy(void *dst, const void *src, size_t n)
{
    size_t words = n / 4;
    if (!words) return;
    __asm__ volatile(
        "mov   r4, %0\n\t"
        "mov   r5, %1\n\t"
        "mov   r6, %2\n\t"
        "swi   0x16\n\t"                    /* OS_EnterOS */
        "1:\n\t"
        "ldr   r7, [r5], #4\n\t"
        "str   r7, [r4], #4\n\t"
        "subs  r6, r6, #1\n\t"
        "bne   1b\n\t"
        "swi   0x7C\n\t"                    /* OS_LeaveOS */
        :
        : "r"(dst), "r"(src), "r"(words)
        : "r0", "r4", "r5", "r6", "r7", "r14", "memory", "cc");
}
#endif

static _kernel_oserror *swi5(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4,
                             uint32_t *o0, uint32_t *o2)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)r0; r.r[1] = (int)r1; r.r[2] = (int)r2; r.r[3] = (int)r3; r.r[4] = (int)r4;
    e = probe_swi(n, &r);
    if (!e) {
        if (o0) *o0 = (uint32_t)r.r[0];
        if (o2) *o2 = (uint32_t)r.r[2];
    }
    return e;
}
#define swi(n, a, b, c, d, o0, o2) swi5(n, a, b, c, d, 0, o0, o2)

static uint32_t now_cs(void)
{
    uint32_t t = 0;
    swi(OS_ReadMonotonicTime, 0, 0, 0, 0, &t, NULL);
    return t;
}

static const char *const status_names[] = {
    "SUCCESS", "ENOMEM", "ENOSPC", "EINVAL", "ENOSYS", "ENOENT", "ENXIO", "EIO", "ESPIPE",
    "ECORRUPT", "ENOTREADY", "ECONFIG", "EISCONN", "ENOTCONN", "EAGAIN", "EFAULT" };
static const char *st(uint32_t s) { return s < 16 ? status_names[s] : "?"; }

static const char *fourcc(uint32_t v, char b[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(v >> (8 * i));
        b[i] = c >= 32 && c < 127 ? c : '.';
    }
    b[4] = 0;
    return b;
}

static uint32_t adler(uint32_t a, const uint8_t *p, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) { s1 += *p++; s2 += s1; }
        s1 %= 65521; s2 %= 65521;
    }
    return s2 << 16 | s1;
}

/* ---- the session ---- */

static uint32_t handle, context = 1, stub, comp;
static uint32_t in_port, out_port;         /* port handles */
static port_info_t in_info, out_info;
static uint32_t msg[128];
#define MAXPEND 32
static uint32_t pend[MAXPEND][128], pend_len[MAXPEND];
static int npend;

/* the callback's counters (in the RMA, after the code) */
static uint32_t bulk_done(int which)
{
    uint32_t v[2];
    probe_svc_copy(v, (const void *)(uintptr_t)(stub + 64), 8);
    return v[which];
}

static int send_msg(uint32_t type, const void *payload, uint32_t len, uint32_t *ctx)
{
    hdr_t *h = (hdr_t *)msg;
    _kernel_oserror *e;
    memset(msg, 0, sizeof msg);
    h->magic = MMAL_MAGIC;
    h->type = type;
    h->context = ++context;
    if (ctx) *ctx = context;
    memcpy(h + 1, payload, len);
    swi(VCHIQ_ServiceUse, handle, 0, 0, 0, NULL, NULL);
    e = swi(VCHIQ_MsgQueue, handle, (uint32_t)(uintptr_t)msg, (uint32_t)sizeof *h + len, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);
    if (e) say("VCHIQ_MsgQueue: %s\n", e->errmess);
    return e ? -1 : 0;
}

/* one message into msg[]: its length, or 0 if none waiting */
static uint32_t poll_msg(void)
{
    uint32_t got = 0;
    memset(msg, 0, sizeof msg);
    if (swi(VCHIQ_MsgDequeue, handle, (uint32_t)(uintptr_t)msg, sizeof msg, 0, NULL, &got))
        return 0;
    if (got < sizeof(hdr_t) || ((hdr_t *)msg)->magic != MMAL_MAGIC) {
        say("(a %u byte message that isn't MMAL's)\n", (unsigned)got);
        return 0;
    }
    return got;
}

/* sends and waits for the reply of the same type; anything else that comes
   meanwhile (pictures, events) is kept for the main loop */
static int transact(uint32_t type, const void *payload, uint32_t len, const char *what)
{
    uint32_t t0, got;
    if (send_msg(type, payload, len, NULL)) return -1;
    t0 = now_cs();
    while (now_cs() - t0 < REPLY_CS) {
        if (!(got = poll_msg())) continue;
        if (((hdr_t *)msg)->type == type) return (int)(got - sizeof(hdr_t));
        if (npend < MAXPEND) {
            memcpy(pend[npend], msg, got);
            pend_len[npend++] = got;
        } else {
            say("%s: too many messages waiting; one dropped\n", what);
        }
    }
    say("%s: no reply in %d cs\n", what, REPLY_CS);
    return -1;
}

static uint32_t *reply_payload(void) { return (uint32_t *)((hdr_t *)msg + 1); }

static int port_get(uint32_t type, port_info_t *pi, const char *what)
{
    uint32_t req[3] = { comp, type, 0 };
    int n = transact(T_PORT_INFO_GET, req, sizeof req, what);
    if (n < (int)offsetof(port_info_t, extradata)) return -1;
    memcpy(pi, reply_payload(), sizeof *pi);
    if (pi->status) { say("%s: %s\n", what, st(pi->status)); return -1; }
    return 0;
}

static int port_set(uint32_t type, port_info_t *pi, const char *what)
{
    port_set_t s;
    int n;
    memset(&s, 0, sizeof s);
    s.component_handle = comp;
    s.port_type = type;
    s.port_index = 0;
    s.port = pi->port;
    s.format = pi->format;
    s.format.extradata_size = 0;
    s.video = pi->video;
    n = transact(T_PORT_INFO_SET, &s, sizeof s, what);
    if (n < 4) return -1;
    if (reply_payload()[0]) { say("%s: %s\n", what, st(reply_payload()[0])); return -1; }
    if (n >= (int)offsetof(port_info_t, extradata)) {
        port_info_t *r = (port_info_t *)reply_payload();
        pi->port = r->port;                 /* the firmware's view (sizes may grow) */
        pi->format = r->format;
        pi->video = r->video;
    }
    return 0;
}

static int port_action(port_info_t *pi, uint32_t action, const char *what)
{
    uint32_t req[3 + 16];
    int n;
    memset(req, 0, sizeof req);
    req[0] = comp; req[1] = pi->port_handle; req[2] = action;
    memcpy(&req[3], &pi->port, sizeof pi->port);
    n = transact(T_PORT_ACTION, req, sizeof req, what);
    if (n < 4) return -1;
    if (reply_payload()[0]) { say("%s: %s\n", what, st(reply_payload()[0])); return -1; }
    return 0;
}

static int simple(uint32_t type, const char *what)
{
    int n = transact(type, &comp, 4, what);
    if (n < 4) return -1;
    if (reply_payload()[0]) { say("%s: %s\n", what, st(reply_payload()[0])); return -1; }
    return 0;
}

/* ---- buffers ---- */

static uint8_t *in_buf[IN_BUFS], *out_buf[OUT_BUFS];
static uint8_t *frame;                     /* a picture copied out of PCI memory, to check */
static int in_busy[IN_BUFS], out_busy[OUT_BUFS];
static uint32_t out_size;

/* Memory the VideoCore reads or writes by bulk transfer must be
   physically contiguous: RISC OS's VCHIQ module (0.14, +&CDC) asks
   OS_Memory 0 for the physical address of the first page only and builds
   the page list from there. So those buffers come from PCI_RAMAlloc
   (physically contiguous, below 1 GB), and are given back at the end.
   That memory is privileged (a USR mode access aborts: the Pi's 0.4 run),
   so this program only reaches it with probe_svc_copy. */
#define MAXPCI 16
static uint32_t pci_alloc_swi, pci_free_swi, pci_blocks[MAXPCI];
static int npci;
static uint8_t *evbuf;                     /* for event data that comes by bulk */
static uint32_t evsize;
static int big_events;
static const char *dump_dir;               /* -d: the first pictures and event data saved here */

static void dump(const char *kind, int n, const uint8_t *p, uint32_t len)
{
    char name[300];
    FILE *f;
    if (!dump_dir || n > 2) return;
    snprintf(name, sizeof name, "%s.%s%d", dump_dir, kind, n);
    if ((f = fopen(name, "wb")) != NULL) {
        fwrite(p, 1, len, f);
        fclose(f);
    }
}

static void *pci_alloc(size_t n)
{
    uint32_t log = 0;
    _kernel_swi_regs r;
    if (!pci_alloc_swi) {
        memset(&r, 0, sizeof r);
        r.r[1] = (int)(uintptr_t)"PCI_RAMAlloc";
        if (probe_swi(0x39, &r)) return NULL;              /* OS_SWINumberFromString */
        pci_alloc_swi = (uint32_t)r.r[0];
        r.r[1] = (int)(uintptr_t)"PCI_RAMFree";
        if (probe_swi(0x39, &r)) return NULL;
        pci_free_swi = (uint32_t)r.r[0];
    }
    if (npci == MAXPCI) return NULL;
    if (swi((int)pci_alloc_swi, (uint32_t)n, 4096, 0, 0, &log, NULL) || !log) return NULL;
    pci_blocks[npci++] = log;
    return (void *)(uintptr_t)log;
}

static void pci_free_all(void)
{
    while (npci) swi((int)pci_free_swi, pci_blocks[--npci], 0, 0, 0, NULL, NULL);
}

static void *aligned(size_t n)
{
    uint8_t *p = malloc(n + 4096);
    if (!p) return NULL;
    p = (uint8_t *)(((uintptr_t)p + 4095) & ~(uintptr_t)4095);
    memset(p, 0, n);                        /* (all of it mapped before the VideoCore writes) */
    return p;
}

static int buffer_to_vc(port_info_t *pi, int idx, uint8_t *data, uint32_t alloc, uint32_t len, uint32_t flags)
{
    buffer_msg_t b;
    memset(&b, 0, sizeof b);
    b.drvbuf.magic = MMAL_MAGIC;
    b.drvbuf.component_handle = comp;
    b.drvbuf.port_handle = pi->port_handle;
    b.drvbuf.client_context = (uint32_t)idx;
    b.data = (uint32_t)(uintptr_t)data;
    b.alloc_size = alloc;
    b.length = len;
    b.flags = flags;
    b.pts = b.dts = TIME_UNKNOWN;
    /* the data always follows by bulk transfer: MMAL only takes it in the
       message for opaque or clock ports (userland mmal_vc_port_send) */
    if (send_msg(T_BUFFER_FROM_HOST, &b, sizeof b, NULL)) return -1;
    if (verbose) say("  tx: %s buffer %d, %u bytes%s\n", pi == &in_info ? "input" : "output", idx, (unsigned)len,
                     flags & FLAG_EOS ? ", EOS" : "");
    if (len) {
        _kernel_oserror *e;
        swi(VCHIQ_ServiceUse, handle, 0, 0, 0, NULL, NULL);
        e = swi5(VCHIQ_BulkQueueTransmit, handle, (uint32_t)(uintptr_t)data, (len + 3) & ~3u, VCHI_FLAGS_BLOCK_UNTIL_QUEUED, 0,
                 NULL, NULL);
        swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);
        if (e) { say("VCHIQ_BulkQueueTransmit: %s\n", e->errmess); return -1; }
    }
    return 0;
}

static int give_outputs(void)
{
    for (int i = 0; i < OUT_BUFS; i++)
        if (!out_busy[i]) {
            if (buffer_to_vc(&out_info, i, out_buf[i], out_size, 0, 0)) return -1;
            out_busy[i] = 1;
        }
    return 0;
}

/* receives n bytes by bulk transfer into dst and waits for the callback */
static int bulk_in(uint8_t *dst, uint32_t n, uint32_t tag)
{
    uint32_t before = bulk_done(0), aborted = bulk_done(1), t0;
    _kernel_oserror *e;
    swi(VCHIQ_ServiceUse, handle, 0, 0, 0, NULL, NULL);
    e = swi5(VCHIQ_BulkQueueReceive, handle, (uint32_t)(uintptr_t)dst, (n + 3) & ~3u,
             VCHI_FLAGS_CALLBACK_WHEN_DONE, tag, NULL, NULL);
    swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);
    if (e) { say("VCHIQ_BulkQueueReceive: %s\n", e->errmess); return -1; }
    t0 = now_cs();
    while (bulk_done(0) == before) {
        if (bulk_done(1) != aborted) { say("A bulk receive was aborted\n"); return -1; }
        if (now_cs() - t0 > REPLY_CS) { say("A bulk receive didn't finish in %d cs\n", REPLY_CS); return -1; }
    }
    return 0;
}

/* ---- the expected checksums ---- */

static uint32_t *want;
static int nwant, want_w, want_h;

static int read_crcs(const char *name)
{
    FILE *f = fopen(name, "r");
    char line[256];
    int cap = 0;
    if (!f) { say("Can't open %s\n", name); return -1; }
    while (fgets(line, sizeof line, f)) {
        char *x;
        if (line[0] == '#') {
            sscanf(line, "#dimensions 0: %dx%d", &want_w, &want_h);
            continue;
        }
        if (!(x = strstr(line, "0x"))) continue;
        if (nwant == cap) {
            cap = cap ? cap * 2 : 256;
            want = realloc(want, (size_t)cap * sizeof *want);
            if (!want) { fclose(f); return -1; }
        }
        want[nwant++] = (uint32_t)strtoul(x, NULL, 16);
    }
    fclose(f);
    if (!want_w || !want_h || !nwant) { say("%s has no #dimensions or no frames\n", name); return -1; }
    return 0;
}

/* Signatures: FFmpeg's pictures as block means (an 8x8 grid of Y, 4x4 of U
   and of V: 96 numbers a picture), from build.sh. The VideoCore's H.264
   output isn't always bit-exact with FFmpeg's (the Pi 4: a few chroma
   values 1 lower, in moving areas), so each picture is also compared this
   way: "close" if no block mean differs by more than 1.0; otherwise the
   FFmpeg picture it is closest to is named, which shows pictures out of
   order or missing. */
#define SIGN 96
static float (*sig)[SIGN];
static int nsig;

static int read_sigs(const char *name)
{
    FILE *f = fopen(name, "r");
    int cap = 0;
    float v[SIGN];
    if (!f) { say("Can't open %s\n", name); return -1; }
    for (;;) {
        int k;
        for (k = 0; k < SIGN; k++) if (fscanf(f, "%f", &v[k]) != 1) break;
        if (k < SIGN) break;
        if (nsig == cap) {
            cap = cap ? cap * 2 : 64;
            if (!(sig = realloc(sig, (size_t)cap * sizeof *sig))) { fclose(f); return -1; }
        }
        memcpy(sig[nsig++], v, sizeof v);
    }
    fclose(f);
    return 0;
}

static void grid(const uint8_t *p, uint32_t pitch, uint32_t w, uint32_t h, int n, float *out)
{
    for (int gy = 0; gy < n; gy++)
        for (int gx = 0; gx < n; gx++) {
            uint32_t y0 = h * (uint32_t)gy / (uint32_t)n, y1 = h * (uint32_t)(gy + 1) / (uint32_t)n;
            uint32_t x0 = w * (uint32_t)gx / (uint32_t)n, x1 = w * (uint32_t)(gx + 1) / (uint32_t)n;
            double sum = 0;
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = x0; x < x1; x++) sum += p[y * pitch + x];
            *out++ = (float)(sum / (double)((y1 - y0) * (x1 - x0)));
        }
}

static void frame_sig(const uint8_t *p, const buffer_msg_t *b, float *s);

static float sig_diff(const float *a, const float *b)
{
    float m = 0;
    for (int k = 0; k < SIGN; k++) { float d = a[k] > b[k] ? a[k] - b[k] : b[k] - a[k]; if (d > m) m = d; }
    return m;
}

/* Adler-32 of the visible picture, packed (Y, U, V) */
static uint32_t frame_crc(const uint8_t *p, const buffer_msg_t *b)
{
    uint32_t w = (uint32_t)want_w, h = (uint32_t)want_h, a = 0, y;   /* (framecrc starts Adler-32 at 0) */
    uint32_t pitch = out_info.video.width, rows = out_info.video.height;
    uint32_t off[3], pit[3];
    if (b->planes == 3 && b->pitch[0]) {
        for (int i = 0; i < 3; i++) { off[i] = b->offsets[i]; pit[i] = b->pitch[i]; }
    } else {
        off[0] = 0; pit[0] = pitch;
        off[1] = pitch * rows; pit[1] = pitch / 2;
        off[2] = off[1] + (pitch / 2) * (rows / 2); pit[2] = pitch / 2;
    }
    for (y = 0; y < h; y++) a = adler(a, p + off[0] + y * pit[0], w);
    for (int c = 1; c < 3; c++)
        for (y = 0; y < (h + 1) / 2; y++) a = adler(a, p + off[c] + y * pit[c], (w + 1) / 2);
    return a;
}

static void frame_sig(const uint8_t *p, const buffer_msg_t *b, float *s)
{
    uint32_t w = (uint32_t)want_w, h = (uint32_t)want_h;
    uint32_t pitch = out_info.video.width, rows = out_info.video.height, off[3], pit[3];
    if (b->planes == 3 && b->pitch[0]) {
        for (int i = 0; i < 3; i++) { off[i] = b->offsets[i]; pit[i] = b->pitch[i]; }
    } else {
        off[0] = 0; pit[0] = pitch;
        off[1] = pitch * rows; pit[1] = pitch / 2;
        off[2] = off[1] + (pitch / 2) * (rows / 2); pit[2] = pitch / 2;
    }
    grid(p + off[0], pit[0], w, h, 8, s);
    grid(p + off[1], pit[1], (w + 1) / 2, (h + 1) / 2, 4, s + 64);
    grid(p + off[2], pit[2], (w + 1) / 2, (h + 1) / 2, 4, s + 80);
}

/* ---- output format ---- */

static void set_out_format(uint32_t w, uint32_t h)
{
    out_info.format.type = ES_VIDEO;
    out_info.format.encoding = ENC_I420;
    out_info.video.width = (w + 31) & ~31u;
    out_info.video.height = (h + 15) & ~15u;
    out_info.video.crop[0] = out_info.video.crop[1] = 0;
    out_info.video.crop[2] = (int32_t)w;
    out_info.video.crop[3] = (int32_t)h;
    out_info.port.buffer_num = OUT_BUFS;
    out_info.port.buffer_size = out_info.video.width * out_info.video.height * 3 / 2;
}

static int configure_output(void)
{
    uint32_t need;
    if (port_set(PORT_OUTPUT, &out_info, "Output format")) return -1;
    need = out_info.port.buffer_size;
    if (out_info.port.buffer_size_recommended > need) need = out_info.port.buffer_size_recommended;
    if (out_info.port.buffer_size_min > need) need = out_info.port.buffer_size_min;
    if (need > out_size) {
        for (int i = 0; i < OUT_BUFS; i++) {
            out_buf[i] = pci_alloc(need);
            if (!out_buf[i]) { say("No physically contiguous memory (PCI_RAMAlloc) for %u byte pictures\n", (unsigned)need); return -1; }
        }
        out_size = need;
        if (!(frame = aligned(need + 4))) { say("Out of memory for a %u byte picture\n", (unsigned)need); return -1; }
    }
    if (out_info.port.buffer_size != out_size) {
        out_info.port.buffer_size = out_size;
        out_info.port.buffer_num = OUT_BUFS;
        if (port_set(PORT_OUTPUT, &out_info, "Output buffers")) return -1;
    }
    return 0;
}

/* ---- main ---- */

int probe_main(int argc, char **argv)
{
    const char *stream_name = NULL, *crc_name = NULL, *sig_name = NULL;
    int close_n = 0, far_n = 0;
    uint8_t *stream = NULL;
    uint32_t stream_len = 0, sent = 0, instance = 0, setup[11], t_start = 0, t_end = 0, last_progress;
    int connected = 0, opened = 0, created = 0, enabled = 0, in_on = 0, out_on = 0, keep_going = 0;
    int frames = 0, wrong = 0, eos_sent = 0, eos_seen = 0, fatal = 0, i, format_changes = 0, events = 0;
    _kernel_oserror *e;
    static const uint32_t code[9] = { 0xe3510004u, 0x05903000u, 0x02833001u, 0x05803000u, 0xe3510012u,
                                      0x05903004u, 0x02833001u, 0x05803004u, 0xe1a0f00eu };

    /* (a fresh start each time: the host tests call this more than once) */
    free(want); want = NULL; nwant = want_w = want_h = 0; npci = 0; pci_alloc_swi = 0; evbuf = NULL; evsize = 0; dump_dir = NULL; big_events = 0; nsig = 0; npend = 0; out_size = 0; stub = 0; comp = 0;
    memset(in_busy, 0, sizeof in_busy); memset(out_busy, 0, sizeof out_busy); out2 = NULL; verbose = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");   /* (added to: Test runs three) */
        else if (!strcmp(argv[i], "-n")) keep_going = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump_dir = argv[++i];
        else if (!stream_name) stream_name = argv[i];
        else if (!crc_name) crc_name = argv[i];
        else if (!sig_name) sig_name = argv[i];
        else stream_name = NULL;
    }
    if (!stream_name || !crc_name) {
        printf("Usage: mmaldecode [-o file] [-n] [-v] [-d dir] stream.h264 expected.crc\n");
        return 1;
    }
    say("mmaldecode: %s on the VideoCore, through VCHIQ and MMAL\n", stream_name);
    if (read_crcs(crc_name)) goto done;
    if (sig_name && read_sigs(sig_name)) goto done;
    {
        FILE *f = fopen(stream_name, "rb");
        long n;
        if (!f) { say("Can't open %s\n", stream_name); goto done; }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        stream = aligned((size_t)n + 4);       /* padded to 4 bytes with zeros (trailing_zero_8bits) */
        if (!stream || fread(stream, 1, (size_t)n, f) != (size_t)n) { fclose(f); say("Can't read %s\n", stream_name); goto done; }
        fclose(f);
        stream_len = ((uint32_t)n + 3) & ~3u;
    }
    say("%u bytes, %dx%d, %d pictures expected\n\n", (unsigned)stream_len, want_w, want_h, nwant);
    for (i = 0; i < IN_BUFS; i++)
        if (!(in_buf[i] = pci_alloc(IN_SIZE))) { say("No physically contiguous memory (PCI_RAMAlloc; is the PCI module loaded?)\n"); goto done; }

    /* the callback */
    if ((e = swi(OS_Module, 6, 0, 0, 72, NULL, &stub)) != NULL) { say("No RMA: %s\n", e->errmess); goto done; }
    {
        uint32_t img[18];
        _kernel_swi_regs r;
        memset(img, 0, sizeof img);
        memcpy(img, code, sizeof code);
        probe_svc_copy((void *)(uintptr_t)stub, img, sizeof img);
        memset(&r, 0, sizeof r);
        r.r[0] = 1; r.r[1] = (int)stub; r.r[2] = (int)(stub + sizeof code);
        probe_swi(OS_SynchroniseCodeAreas, &r);
    }

    if ((e = swi(VCHIQ_Initialise, 0, 0, 0, 0, &instance, NULL)) != NULL) { say("VCHIQ_Initialise: %s\n", e->errmess); goto done; }
    if ((e = swi(VCHIQ_Connect, 0, 0, instance, 0, NULL, NULL)) != NULL) { say("VCHIQ_Connect: %s\n", e->errmess); goto done; }
    connected = 1;
    memset(setup, 0, sizeof setup);
    setup[0] = 15; setup[1] = 10; setup[2] = MMAL_SERVICE;
    setup[6] = stub; setup[7] = stub + 64;
    if ((e = swi(VCHIQ_ServiceOpen, instance, (uint32_t)(uintptr_t)setup, 0, 0, &handle, NULL)) != NULL) {
        say("VCHIQ_ServiceOpen \"mmal\": %s\n", e->errmess);
        goto done;
    }
    opened = 1;
    swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);

    /* 1. the component and its ports */
    {
        uint32_t req[1 + 32 + 1];
        int n;
        memset(req, 0, sizeof req);
        req[0] = 1;
        strcpy((char *)&req[1], "ril.video_decode");
        n = transact(T_COMPONENT_CREATE, req, sizeof req, "Create");
        if (n < 20 || reply_payload()[0]) {
            if (n >= 4) say("Create: %s\n", st(reply_payload()[0]));
            else say("The VideoCore's MMAL service isn't answering. If an earlier run stopped part way,\n"
                     "it can stay stuck until the machine is restarted.\n");
            goto done;
        }
        comp = reply_payload()[1];
        created = 1;
    }
    if (port_get(PORT_INPUT, &in_info, "Input port") || port_get(PORT_OUTPUT, &out_info, "Output port")) goto done;
    in_port = in_info.port_handle;
    out_port = out_info.port_handle;
    in_info.format.type = ES_VIDEO;
    in_info.format.encoding = ENC_H264;
    in_info.video.width = (uint32_t)want_w;
    in_info.video.height = (uint32_t)want_h;
    in_info.port.buffer_num = IN_BUFS;
    in_info.port.buffer_size = IN_SIZE;
    if (port_set(PORT_INPUT, &in_info, "Input format")) goto done;
    set_out_format((uint32_t)want_w, (uint32_t)want_h);
    if (configure_output()) goto done;
    {
        char b[5];
        say("Output: %s %ux%u, %d buffers of %u bytes\n", fourcc(out_info.format.encoding, b),
            (unsigned)out_info.video.width, (unsigned)out_info.video.height, OUT_BUFS, (unsigned)out_size);
    }

    if (verbose) {
        char b1[5], b2[5];
        say("  input port %u: %s %ux%u, %u buffers of %u (min %u x %u, recommended %u x %u)\n",
            (unsigned)in_info.port_handle, fourcc(in_info.format.encoding, b1), (unsigned)in_info.video.width,
            (unsigned)in_info.video.height, (unsigned)in_info.port.buffer_num, (unsigned)in_info.port.buffer_size,
            (unsigned)in_info.port.buffer_num_min, (unsigned)in_info.port.buffer_size_min,
            (unsigned)in_info.port.buffer_num_recommended, (unsigned)in_info.port.buffer_size_recommended);
        say("  output port %u: %s %ux%u, %u buffers of %u (min %u x %u, recommended %u x %u)\n",
            (unsigned)out_info.port_handle, fourcc(out_info.format.encoding, b2), (unsigned)out_info.video.width,
            (unsigned)out_info.video.height, (unsigned)out_info.port.buffer_num, (unsigned)out_info.port.buffer_size,
            (unsigned)out_info.port.buffer_num_min, (unsigned)out_info.port.buffer_size_min,
            (unsigned)out_info.port.buffer_num_recommended, (unsigned)out_info.port.buffer_size_recommended);
    }
    /* 2. enable */
    if (simple(T_COMPONENT_ENABLE, "Enable")) goto done;
    enabled = 1;
    if (port_action(&in_info, ACTION_ENABLE, "Input enable")) goto done;
    in_on = 1;
    if (port_action(&out_info, ACTION_ENABLE, "Output enable")) goto done;
    out_on = 1;
    if (give_outputs()) goto done;

    /* 3-6. feed, collect, check */
    t_start = last_progress = now_cs();
    while (!eos_seen && !fatal) {
        uint32_t got;
        /* input: fill every free buffer */
        for (i = 0; i < IN_BUFS && !eos_sent; i++) {
            uint32_t n, flags = 0;
            if (in_busy[i]) continue;
            n = stream_len - sent < IN_SIZE ? stream_len - sent : IN_SIZE;
            probe_svc_copy(in_buf[i], stream + sent, (n + 3) & ~3u);   /* (the stream is padded) */
            sent += n;
            if (sent == stream_len) { flags = FLAG_EOS; eos_sent = 1; }
            if (buffer_to_vc(&in_info, i, in_buf[i], IN_SIZE, n, flags)) { fatal = 1; break; }
            in_busy[i] = 1;
        }
        /* a message: kept ones first */
        if (npend) {
            got = pend_len[0];
            memcpy(msg, pend[0], got);
            memmove(pend[0], pend[1], sizeof pend[0] * (size_t)(npend - 1));
            memmove(pend_len, pend_len + 1, sizeof pend_len[0] * (size_t)(npend - 1));
            npend--;
        } else if (!(got = poll_msg())) {
            if (now_cs() - last_progress > IDLE_CS) {
                say("Nothing from the decoder for %d cs (%d pictures so far)\n", IDLE_CS, frames);
                say("  %u of %u bytes sent; input buffers with the decoder: %d %d %d; output: %d %d %d; "
                    "bulk receives done %u, aborted %u\n", (unsigned)sent, (unsigned)stream_len,
                    in_busy[0], in_busy[1], in_busy[2], out_busy[0], out_busy[1], out_busy[2],
                    (unsigned)bulk_done(0), (unsigned)bulk_done(1));
                fatal = 1;
            }
            continue;
        }
        last_progress = now_cs();
        if (verbose) {
            const uint32_t *w = reply_payload();
            if (((hdr_t *)msg)->type == T_BUFFER_TO_HOST)
                say("  rx: buffer back, status %u, port %u, buffer %u, length %u, flags &%X, in message %u\n",
                    (unsigned)((hdr_t *)msg)->status, (unsigned)w[2], (unsigned)w[3], (unsigned)w[8 + 5],
                    (unsigned)w[8 + 7], (unsigned)((buffer_msg_t *)w)->payload_in_message);
            else
                say("  rx: type %u, status %u, %u bytes: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    (unsigned)((hdr_t *)msg)->type, (unsigned)((hdr_t *)msg)->status, (unsigned)got,
                    (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5],
                    (unsigned)w[6], (unsigned)w[7]);
        }
        if (((hdr_t *)msg)->type == T_BUFFER_TO_HOST) {
            buffer_msg_t b;
            memcpy(&b, reply_payload(), sizeof b);
            if (b.drvbuf.magic != MMAL_MAGIC || b.drvbuf.client_context >= IN_BUFS + OUT_BUFS) {
                say("A returned buffer we don't know\n");
                continue;
            }
            if (b.drvbuf.port_handle == in_port) {
                in_busy[b.drvbuf.client_context % IN_BUFS] = 0;
                if (((hdr_t *)msg)->status) say("Input buffer back with status %s\n", st(((hdr_t *)msg)->status));
                continue;
            }
            if (b.drvbuf.port_handle != out_port || b.drvbuf.client_context >= OUT_BUFS) {
                say("A buffer back from port %u\n", (unsigned)b.drvbuf.port_handle);
                continue;
            }
            {
                int k = (int)b.drvbuf.client_context;
                out_busy[k] = 0;
                if (((hdr_t *)msg)->status) {
                    say("Output buffer back with status %s\n", st(((hdr_t *)msg)->status));
                    continue;
                }
                if (b.length && !b.payload_in_message) {
                    if (b.length > out_size) { say("A %u byte picture for a %u byte buffer\n", (unsigned)b.length, (unsigned)out_size); fatal = 1; break; }
                    if (bulk_in(out_buf[k], b.length, (uint32_t)k)) { fatal = 1; break; }
                    probe_svc_copy(frame, out_buf[k], (b.length + 3) & ~3u);
                } else if (b.length) {
                    memcpy(frame, b.short_data, b.payload_in_message);
                } else if (b.flags & FLAG_EOS) {
                    if (bulk_in(out_buf[k], 8, (uint32_t)k)) { fatal = 1; break; }   /* keeps the order */
                }
                if (b.length) {
                    uint32_t crc = frame_crc(frame, &b);
                    dump("p", frames, frame, b.length);
                    if (verbose)
                        say("  picture %d: planes %u, offsets %u %u %u, pitch %u %u %u; bytes %02X %02X %02X %02X; checksum &%08X\n",
                            frames, (unsigned)b.planes, (unsigned)b.offsets[0], (unsigned)b.offsets[1], (unsigned)b.offsets[2],
                            (unsigned)b.pitch[0], (unsigned)b.pitch[1], (unsigned)b.pitch[2],
                            frame[0], frame[1], frame[2], frame[3], (unsigned)crc);
                    if (frames < nwant && crc != want[frames]) {
                        int is_close = 0;
                        if (frames < nsig) {
                            float s[SIGN], d = 1e9f, best = 1e9f;
                            int bj = -1;
                            frame_sig(frame, &b, s);
                            d = sig_diff(s, sig[frames]);
                            for (int j = 0; j < nsig; j++) {
                                float dj = sig_diff(s, sig[j]);
                                if (dj < best) { best = dj; bj = j; }
                            }
                            if (d <= 1.0f) { is_close = 1; close_n++; }
                            else {
                                far_n++;
                                if (far_n <= 5) say("Picture %d: not close to FFmpeg's picture %d (%.1f); closest is FFmpeg's %d (%.1f)\n",
                                                    frames, frames, (double)d, bj, (double)best);
                            }
                        }
                        if (!is_close) {
                            if (wrong < 5 && frames >= nsig) say("Picture %d: checksum &%08X, FFmpeg's &%08X\n", frames, (unsigned)crc, (unsigned)want[frames]);
                            wrong++;
                            if (!keep_going) fatal = 1;
                        }
                    }
                    frames++;
                }
                if (b.flags & FLAG_EOS) { eos_seen = 1; t_end = now_cs(); }
                else if (give_outputs()) fatal = 1;
            }
        } else if (((hdr_t *)msg)->type == T_EVENT_TO_HOST) {
            event_msg_t ev;
            char c[5];
            memcpy(&ev, reply_payload(), sizeof ev);
            events++;
            /* An event's length can exceed the 256 bytes the message holds;
               then its data follows by bulk transfer (userland's client
               queues a receive for it). The Pi sends one such event (cmd 0,
               length = a picture's size) on the output before the first
               picture: 0.6, which didn't receive it, got every picture one
               transfer late and the decoder stuck at the end waiting to
               send its last one. So it's received (into PCI memory), and
               with -v checksummed as if it were a picture, and with -d
               saved, to see what it is. */
            if (ev.length > sizeof ev.data) {
                if (ev.length > evsize) {
                    evbuf = pci_alloc(ev.length + 4);
                    evsize = evbuf ? ev.length : 0;
                }
                if (!evbuf || bulk_in(evbuf, ev.length, 99)) { fatal = 1; break; }
                if (ev.length <= out_size) {
                    buffer_msg_t none;
                    memset(&none, 0, sizeof none);
                    probe_svc_copy(frame, evbuf, (ev.length + 3) & ~3u);
                    if (verbose)
                        say("  event data: bytes %02X %02X %02X %02X; as a picture, checksum &%08X\n",
                            frame[0], frame[1], frame[2], frame[3], (unsigned)frame_crc(frame, &none));
                    dump("e", big_events++, frame, ev.length);
                }
                probe_svc_copy(ev.data, evbuf, sizeof ev.data);
            }
            if (ev.cmd == EV_FORMAT_CHANGED && ev.port_type == PORT_OUTPUT) {
                format_changed_t fc;
                memcpy(&fc, ev.data, sizeof fc);
                format_changes++;
                say("Format changed: %s %ux%u (crop %dx%d), buffers %u x %u bytes\n", fourcc(fc.format.encoding, c),
                    (unsigned)fc.video.width, (unsigned)fc.video.height, (int)fc.video.crop[2], (int)fc.video.crop[3],
                    (unsigned)fc.buffer_num_recommended, (unsigned)fc.buffer_size_recommended);
                /* MMAL's clients always answer a format change by disabling
                   the port, setting the format it gives and enabling it again
                   (userland's examples, Linux's codec driver); 0.7 skipped it
                   when the size hadn't changed, and the decoder then sent its
                   first picture as an event and lost one: so always. */
                {
                    /* the output's buffers come back when it's disabled */
                    if (port_action(&out_info, ACTION_DISABLE, "Output disable")) { fatal = 1; break; }
                    out_on = 0;
                    for (i = 0; i < OUT_BUFS; i++) out_busy[i] = 0;
                    out_info.format = fc.format;
                    out_info.video = fc.video;
                    out_info.port.buffer_num = OUT_BUFS;
                    out_info.port.buffer_size = fc.buffer_size_recommended > fc.buffer_size_min ?
                                                fc.buffer_size_recommended : fc.buffer_size_min;
                    if (configure_output() || port_action(&out_info, ACTION_ENABLE, "Output enable")) { fatal = 1; break; }
                    out_on = 1;
                    /* anything the disable sent back is stale */
                    for (i = 0; i < npend; )
                        if (((hdr_t *)pend[i])->type == T_BUFFER_TO_HOST &&
                            ((buffer_msg_t *)((hdr_t *)pend[i] + 1))->drvbuf.port_handle == out_port) {
                            memmove(pend[i], pend[i + 1], sizeof pend[0] * (size_t)(npend - i - 1));
                            memmove(pend_len + i, pend_len + i + 1, sizeof pend_len[0] * (size_t)(npend - i - 1));
                            npend--;
                        } else {
                            i++;
                        }
                    if (give_outputs()) { fatal = 1; break; }
                }
            } else if (ev.cmd == EV_ERROR) {
                say("The decoder reports an error (port type %u)\n", (unsigned)ev.port_type);
                fatal = 1;
            } else {
                say("Event %s (&%08X, %u bytes) on port type %u, number %u\n", fourcc(ev.cmd, c), (unsigned)ev.cmd,
                    (unsigned)ev.length, (unsigned)ev.port_type, (unsigned)ev.port_num);
            }
        } else {
            say("(a type %u message)\n", (unsigned)((hdr_t *)msg)->type);
        }
    }
    if (eos_seen) {
        uint32_t cs = t_end - t_start ? t_end - t_start : 1;
        say("\n%d pictures decoded in %u.%02u s: %u.%u pictures a second\n", frames, (unsigned)(cs / 100),
            (unsigned)(cs % 100), (unsigned)(frames * 100 / cs), (unsigned)(frames * 1000 / cs % 10));
    }
    say("%d of %d checked against FFmpeg: %d wrong\n", frames < nwant ? frames : nwant, nwant, wrong);
    if (format_changes) say("(%d format change%s)\n", format_changes, format_changes == 1 ? "" : "s");

done:
    if (out_on) port_action(&out_info, ACTION_DISABLE, "Output disable");
    if (in_on) port_action(&in_info, ACTION_DISABLE, "Input disable");
    if (enabled) simple(T_COMPONENT_DISABLE, "Disable");
    if (created) simple(T_COMPONENT_DESTROY, "Destroy");
    if (opened && (e = swi(VCHIQ_ServiceClose, handle, 0, 0, 0, NULL, NULL)) != NULL) {
        say("VCHIQ_ServiceClose: %s (the callback is left in the RMA)\n", e->errmess);
        stub = 0;
    }
    if (connected) swi(VCHIQ_Disconnect, instance, 0, 0, 0, NULL, NULL);
    if (stub) swi(OS_Module, 7, 0, stub, 0, NULL, NULL);
    pci_free_all();
    {
        int ok = eos_seen && !fatal && !wrong && frames == nwant;
        if (nsig) say("Not bit-exact but close to FFmpeg's (block means within 1): %d; not close: %d\n", close_n, far_n);
        say("\nResult: %s\n", ok && !close_n ? "OK - the VideoCore decoded every picture exactly as FFmpeg does" :
                              ok ? "OK - every picture matches FFmpeg's, some within rounding (see above)" :
                              eos_seen && frames != nwant ? "the decoder finished, but with a different number of pictures" :
                              wrong ? "pictures differ from FFmpeg's" : "the decode didn't finish (see above)");
        if (out2) fclose(out2);
        (void)events;
        return ok ? 0 : 1;
    }
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
