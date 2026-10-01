/*
 * mmalprobe - can RISC OS talk MMAL to the VideoCore's video decoder?
 *
 * Step 2 of the H.264 hardware track (every Pi from the Pi 1 to the Pi 4).
 * vchiqprobe showed RISC OS's VCHIQ module offers Broadcom's VCHI calls as
 * SWIs, and BCMSound/BCMVideo showed how they're called:
 *
 *   VCHIQ_Initialise   R0 = the R12 its callbacks get -> R0 = instance
 *   VCHIQ_Connect      R0 = 0, R1 = 0, R2 = instance
 *   VCHIQ_ServiceOpen  R0 = instance, R1 -> SERVICE_CREATION_T -> R0 = handle
 *   VCHIQ_MsgQueue     R0 = handle, R1 -> data, R2 = size (R3 flags unused)
 *   VCHIQ_MsgDequeue   R0 = handle, R1 -> buffer, R2 = its size, R3 = flags
 *                      (0: don't wait; an error if nothing's there)
 *                      -> R2 = the message's size
 *   VCHIQ_ServiceUse / ServiceRelease / ServiceClose  R0 = handle
 *   VCHIQ_Disconnect   R0 = instance
 *
 * A service must have a callback, or VCHIQ throws its messages away. It is
 * called in SVC mode (perhaps from an interrupt), with R0 = the param,
 * R1 = the reason, R12 = Initialise's R0, so it can't be in application
 * space: this puts a one-instruction "MOV PC, R14" in the RMA and reads the
 * messages with MsgDequeue.
 *
 * Then, with the "mmal" service (version 15, oldest 10, as Linux's
 * bcm2835 MMAL driver), it asks the firmware to:
 *   1. create "ril.video_decode" and report its ports;
 *   2. describe its first input and output port (format, buffer sizes);
 *   3. list the input port's supported encodings (H.264 is the one we
 *      want; MPEG-2 and VC-1 only appear on a Pi 1-3 with the licence);
 *   4. destroy it again.
 * Nothing is decoded and no memory is shared with the VideoCore.
 *
 *   mmalprobe [-o file] [-c component]
 *
 * Message layouts (no code copied): Raspberry Pi userland's MMAL client,
 * interface/mmal/vc/mmal_vc_msgs.h (Broadcom, BSD-3-Clause), and Linux's
 * vchiq-mmal, mmal-msg*.h (GPL-2.0).
 * Part of riscos-ffmpeg. GPL version 2 or later (see COPYING).
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
#define BCMSupport_SendTempPropertyBuffer 0x591C5
#define VCHIQ_Initialise          0x59200
#define VCHIQ_Connect             0x59201
#define VCHIQ_Disconnect          0x59202
#define VCHIQ_MsgDequeue          0x59204
#define VCHIQ_MsgQueue            0x59205
#define VCHIQ_ServiceClose        0x59209
#define VCHIQ_ServiceOpen         0x5920A
#define VCHIQ_ServiceUse          0x5920D
#define VCHIQ_ServiceRelease      0x5920E

#define FOURCC_BE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FOURCC_LE(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MMAL_SERVICE   FOURCC_BE('m', 'm', 'a', 'l')    /* VCHIQ_MAKE_FOURCC */
#define MMAL_MAGIC     FOURCC_LE('m', 'm', 'a', 'l')
#define VC_MMAL_VER     15
#define VC_MMAL_MIN_VER 10
#define ENC_H264       FOURCC_LE('H', '2', '6', '4')

enum { T_COMPONENT_CREATE = 4, T_COMPONENT_DESTROY = 5, T_PORT_INFO_GET = 8, T_PORT_PARAMETER_GET = 15 };
enum { PORT_CONTROL = 1, PORT_INPUT = 2, PORT_OUTPUT = 3 };
#define PARAM_SUPPORTED_ENCODINGS 1
#define REPLY_CS 300

typedef struct {
    uint32_t magic, type, control_service, context, status, padding;
} mmal_hdr_t;

typedef struct {
    uint32_t priv, name, type;
    uint16_t index, index_all;
    uint32_t is_enabled, format, buffer_num_min, buffer_size_min, buffer_alignment_min,
             buffer_num_recommended, buffer_size_recommended, buffer_num, buffer_size,
             component, userdata, capabilities;
} mmal_port_t;

typedef struct {
    uint32_t type, encoding, encoding_variant, es, bitrate, flags, extradata_size, extradata;
} mmal_es_format_t;

typedef struct {
    uint32_t width, height;
    int32_t crop[4], frame_rate[2], par[2];
    uint32_t color_space;
} mmal_video_format_t;

typedef struct {
    uint32_t status, component_handle, port_type, port_index;
    int32_t found;
    uint32_t port_handle;
    mmal_port_t port;
    mmal_es_format_t format;
    mmal_video_format_t video;
} port_info_reply_t;

typedef char check_hdr[sizeof(mmal_hdr_t) == 24 ? 1 : -1];
typedef char check_port[sizeof(mmal_port_t) == 64 ? 1 : -1];
typedef char check_fmt[sizeof(mmal_es_format_t) == 32 ? 1 : -1];

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
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);   /* tests/host/mmalprobe_test.c */
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
#endif

static _kernel_oserror *swi(int n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t *out0, uint32_t *out2)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    memset(&r, 0, sizeof r);
    r.r[0] = (int)r0; r.r[1] = (int)r1; r.r[2] = (int)r2; r.r[3] = (int)r3;
    e = probe_swi(n, &r);
    if (!e) {
        if (out0) *out0 = (uint32_t)r.r[0];
        if (out2) *out2 = (uint32_t)r.r[2];
    }
    return e;
}

static uint32_t now_cs(void)
{
    uint32_t t = 0;
    swi(OS_ReadMonotonicTime, 0, 0, 0, 0, &t, NULL);
    return t;
}

static const char *fourcc(uint32_t v, char b[5])
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(v >> (8 * i));
        b[i] = c >= 32 && c < 127 ? c : '.';
    }
    b[4] = 0;
    return b;
}

static void board(void)
{
    uint32_t buf[8] = { 8 * 4, 0, 0x00010002, 4, 0, 0, 0, 0 };
    if (swi(BCMSupport_SendTempPropertyBuffer, (uint32_t)(uintptr_t)buf, (uint32_t)(uintptr_t)buf, 0, 0, NULL, NULL) ||
        buf[1] != 0x80000000u)
        say("Board: the firmware didn't answer\n");
    else
        say("Board revision &%08X\n", (unsigned)buf[5]);
}

static const char *const status_names[] = {
    "SUCCESS", "ENOMEM", "ENOSPC", "EINVAL", "ENOSYS", "ENOENT", "ENXIO", "EIO", "ESPIPE",
    "ECORRUPT", "ENOTREADY", "ECONFIG", "EISCONN", "ENOTCONN", "EAGAIN", "EFAULT" };

static const char *st(uint32_t s) { return s < 16 ? status_names[s] : "?"; }

/* ---- the conversation ---- */

static uint32_t handle, context = 1;
static uint32_t msg[128];                  /* 512 bytes: MMAL's largest message */

/* sends type + payload, waits for the reply of the same type (others are
   shown and skipped); returns the reply's payload length, or -1 */
static int transact(uint32_t type, const void *payload, uint32_t len, const char *what)
{
    mmal_hdr_t *h = (mmal_hdr_t *)msg;
    _kernel_oserror *e;
    uint32_t t0, got;
    memset(msg, 0, sizeof msg);
    h->magic = MMAL_MAGIC;
    h->type = type;
    h->context = ++context;
    memcpy(h + 1, payload, len);
    swi(VCHIQ_ServiceUse, handle, 0, 0, 0, NULL, NULL);
    e = swi(VCHIQ_MsgQueue, handle, (uint32_t)(uintptr_t)msg, (uint32_t)sizeof *h + len, 0, NULL, NULL);
    swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);
    if (e) {
        say("%s: VCHIQ_MsgQueue failed: %s\n", what, e->errmess);
        return -1;
    }
    t0 = now_cs();
    while (now_cs() - t0 < REPLY_CS) {
        memset(msg, 0, sizeof msg);
        if (swi(VCHIQ_MsgDequeue, handle, (uint32_t)(uintptr_t)msg, sizeof msg, 0, NULL, &got))
            continue;                       /* nothing yet */
        if (got < sizeof *h || h->magic != MMAL_MAGIC) {
            say("%s: a %u byte message that isn't MMAL's\n", what, (unsigned)got);
            continue;
        }
        if (h->type != type) {
            say("%s: (a type %u message came first, status %u)\n", what, (unsigned)h->type, (unsigned)h->status);
            continue;
        }
        if (h->status)
            say("%s: the reply's header status is %u\n", what, (unsigned)h->status);
        return (int)(got - sizeof *h);
    }
    say("%s: no reply in %d cs\n", what, REPLY_CS);
    return -1;
}

static int port_info(uint32_t comp, uint32_t type, uint32_t *port_handle)
{
    uint32_t req[3] = { comp, type, 0 };
    const char *nm = type == PORT_INPUT ? "Input port 0" : type == PORT_OUTPUT ? "Output port 0" : "Control port";
    port_info_reply_t *p = (port_info_reply_t *)((mmal_hdr_t *)msg + 1);
    char b[5], b2[5];
    int n = transact(T_PORT_INFO_GET, req, sizeof req, nm);
    if (n < (int)offsetof(port_info_reply_t, video))
        return -1;
    say("%s: %s, handle &%X, enabled %u, capabilities &%X\n", nm, st(p->status), (unsigned)p->port_handle,
        (unsigned)p->port.is_enabled, (unsigned)p->port.capabilities);
    if (p->status) return -1;
    say("  format: type %u, encoding %s, variant %s, bitrate %u, flags &%X\n", (unsigned)p->format.type,
        fourcc(p->format.encoding, b), fourcc(p->format.encoding_variant, b2), (unsigned)p->format.bitrate,
        (unsigned)p->format.flags);
    if (n >= (int)sizeof *p)
        say("  video: %ux%u, crop %dx%d, frame rate %d/%d, colour space %s\n", (unsigned)p->video.width,
            (unsigned)p->video.height, (int)p->video.crop[2], (int)p->video.crop[3], (int)p->video.frame_rate[0],
            (int)p->video.frame_rate[1], fourcc(p->video.color_space, b));
    say("  buffers: at least %u of %u bytes (align %u), recommended %u of %u bytes\n",
        (unsigned)p->port.buffer_num_min, (unsigned)p->port.buffer_size_min, (unsigned)p->port.buffer_alignment_min,
        (unsigned)p->port.buffer_num_recommended, (unsigned)p->port.buffer_size_recommended);
    *port_handle = p->port_handle;
    return 0;
}

int probe_main(int argc, char **argv)
{
    const char *component = "ril.video_decode";
    uint32_t instance = 0, stub = 0, setup[11], comp = 0, in_handle = 0, out_handle = 0, ctl_handle = 0;
    int connected = 0, opened = 0, created = 0, verdict = 1, has_h264 = -1, i, n;
    _kernel_oserror *e;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out2 = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            component = argv[++i];
        else {
            printf("Usage: mmalprobe [-o file] [-c component]\n");
            return 1;
        }
    }
    say("mmalprobe: the VideoCore's %s, through VCHIQ and MMAL\n\n", component);
    board();

    /* the callback: MOV PC, R14 in the RMA */
    if ((e = swi(OS_Module, 6, 0, 0, 16, NULL, &stub)) != NULL) {
        say("No RMA for the callback: %s\n", e->errmess);
        goto done;
    }
    {
        _kernel_swi_regs r;
        *(volatile uint32_t *)(uintptr_t)stub = 0xE1A0F00Eu;
        memset(&r, 0, sizeof r);
        r.r[0] = 1; r.r[1] = (int)stub; r.r[2] = (int)(stub + 4);
        probe_swi(OS_SynchroniseCodeAreas, &r);
    }

    if ((e = swi(VCHIQ_Initialise, 0, 0, 0, 0, &instance, NULL)) != NULL) {
        say("VCHIQ_Initialise: %s\n", e->errmess);
        goto done;
    }
    say("VCHIQ instance &%X\n", (unsigned)instance);
    if ((e = swi(VCHIQ_Connect, 0, 0, instance, 0, NULL, NULL)) != NULL) {
        say("VCHIQ_Connect: %s\n", e->errmess);
        goto done;
    }
    connected = 1;
    memset(setup, 0, sizeof setup);
    setup[0] = VC_MMAL_VER; setup[1] = VC_MMAL_MIN_VER; setup[2] = MMAL_SERVICE;
    setup[6] = stub;                        /* callback; param 0 */
    if ((e = swi(VCHIQ_ServiceOpen, instance, (uint32_t)(uintptr_t)setup, 0, 0, &handle, NULL)) != NULL) {
        say("VCHIQ_ServiceOpen \"mmal\": %s\n", e->errmess);
        goto done;
    }
    opened = 1;
    swi(VCHIQ_ServiceRelease, handle, 0, 0, 0, NULL, NULL);   /* (opening leaves it in use, as on Linux) */
    say("MMAL service open, handle &%X\n\n", (unsigned)handle);

    /* 1. create the component */
    {
        uint32_t req[1 + 32 + 1];
        memset(req, 0, sizeof req);
        req[0] = 1;                         /* client_component */
        strncpy((char *)&req[1], component, 127);
        n = transact(T_COMPONENT_CREATE, req, sizeof req, "Create");
        if (n >= 20) {
            uint32_t *rep = (uint32_t *)((mmal_hdr_t *)msg + 1);
            say("Create %s: %s, component &%X, %u input, %u output, %u clock ports\n", component, st(rep[0]),
                (unsigned)rep[1], (unsigned)rep[2], (unsigned)rep[3], (unsigned)rep[4]);
            if (rep[0] == 0) { comp = rep[1]; created = 1; }
        }
    }
    if (!created) goto done;
    verdict = 0;

    /* 2. its ports */
    port_info(comp, PORT_CONTROL, &ctl_handle);
    if (port_info(comp, PORT_INPUT, &in_handle)) verdict = 2;
    if (port_info(comp, PORT_OUTPUT, &out_handle)) verdict = 2;

    /* 3. what the input takes */
    if (in_handle) {
        uint32_t req[4] = { comp, in_handle, PARAM_SUPPORTED_ENCODINGS, 8 + 64 * 4 };
        n = transact(T_PORT_PARAMETER_GET, req, sizeof req, "Supported encodings");
        if (n >= 12) {
            uint32_t *rep = (uint32_t *)((mmal_hdr_t *)msg + 1), size = rep[2], k;
            char b[5];
            if (rep[0]) {
                say("Supported encodings: %s\n", st(rep[0]));
            } else {
                say("Supported encodings on the input:");
                has_h264 = 0;
                for (k = 0; 8 + 4 * (k + 1) <= size && 12 + 4 * (k + 1) <= (uint32_t)n && k < 64; k++) {
                    say(" %s", fourcc(rep[3 + k], b));
                    if (rep[3 + k] == ENC_H264) has_h264 = 1;
                }
                say("\n");
            }
        }
    }

done:
    /* 4. and away again */
    if (created) {
        n = transact(T_COMPONENT_DESTROY, &comp, 4, "Destroy");
        if (n >= 4) say("Destroy: %s\n", st(((uint32_t *)((mmal_hdr_t *)msg + 1))[0]));
    }
    if (opened) {
        if ((e = swi(VCHIQ_ServiceClose, handle, 0, 0, 0, NULL, NULL)) != NULL) {
            say("VCHIQ_ServiceClose: %s (the callback is left in the RMA)\n", e->errmess);
            stub = 0;
        }
    }
    if (connected)
        swi(VCHIQ_Disconnect, instance, 0, 0, 0, NULL, NULL);
    if (stub)
        swi(OS_Module, 7, 0, stub, 0, NULL, NULL);
    say("\nResult: %s\n",
        verdict == 1 ? "the video decoder couldn't be reached (see above)" :
        has_h264 == 1 ? "OK - the VideoCore's video decoder answers and takes H.264" :
        has_h264 == 0 ? "the decoder answers, but doesn't list H.264" :
        verdict == 2 ? "the decoder was created, but a port didn't answer" :
                       "the decoder answers (its encodings weren't listed)");
    if (out2) fclose(out2);
    return verdict == 0 && has_h264 == 0 ? 3 : verdict;
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
