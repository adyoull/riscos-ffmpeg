/*
 * mmaldecode (tools/mmaldecode) against a fake VCHIQ module and a fake
 * MMAL video decoder. The fake "stream" is frame records (a start code and
 * a value); the fake decoder turns each into a picture (Y, U, V filled
 * from the value) laid out as MMAL's I420 (pitch 32-aligned, rows 16-
 * aligned), and the expected list is Adler-32 from 0 of the packed
 * visible picture, as FFmpeg's framecrc. The real callback stub runs (in
 * executable memory) when the fake completes a bulk receive.
 *   - the normal path: several 64 KB pieces by bulk, a short last piece in
 *     the message, EOS, an EFCH that needs no change, every picture right;
 *   - an EFCH to a bigger size: disable, reformat, re-enable, buffers again;
 *   - a wrong picture: stops there (and with -n goes on and counts);
 *   - a decoder that goes quiet: gives up; an error event: stops;
 *   - every run: component disabled and destroyed, ports disabled,
 *     use/release balanced, service closed, disconnected, stub freed.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel.h"

int probe_main(int argc, char **argv);
static void pci_open(int rw);
/* SVC mode: PCI_RAMAlloc memory is reachable only here (and by the "VideoCore") */
void probe_svc_copy(void *dst, const void *src, size_t n) { pci_open(1); memcpy(dst, src, n); pci_open(0); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define MAGIC 0x6C616D6Du
#define W 70
#define H 38
#define NFRAMES 9
static _kernel_oserror err = { 1, "fake error" }, empty = { 2, "none" };

/* scenario */
static int first_event_sent;
static int big_efch, corrupt_at = -1, go_quiet, error_event, long_event, awaiting_reformat;
/* counters */
static int uses, releases, opens, closes, connects, disconnects, freed, created, destroyed, comp_enabled,
           port_on[4], shorts, bulks_tx, bulks_rx, efch_sent, disables_out;
static uint32_t stub, time_cs;

/* the fake decoder's state */
static uint8_t got_stream[1 << 20];
static uint32_t got_len, pending_tx, pending_tx_ctx;
static uint32_t out_w, out_h, out_size_set;
static int out_bufs[8], nout, frames_made, eos_in, eos_out_sent, need_rx;
static uint32_t rx_len_expected;
static uint8_t cur_frame[1 << 16];
static uint32_t outq[64][128], outq_len[64];
static int noutq;

static void post(uint32_t type, const void *payload, uint32_t len, uint32_t status)
{
    if (getenv("FAKEDBG")) fprintf(stderr, "post type %u len %u\n", type, len);
    uint32_t *m = outq[noutq];
    memset(m, 0, 512);
    m[0] = MAGIC; m[1] = type; m[4] = status;
    memcpy(m + 6, payload, len);
    outq_len[noutq++] = 24 + len;
}

static void reply(const uint32_t *req, const void *payload, uint32_t len) { post(req[1], payload, len, 0); }

typedef struct { uint32_t magic, comp, port, ctx; } drv_t;

static void buffer_back(uint32_t port, uint32_t ctx, uint32_t length, uint32_t flags)
{
    uint32_t b[68];                          /* 272 bytes */
    memset(b, 0, sizeof b);
    b[0] = MAGIC; b[1] = 0xC0DE; b[2] = port; b[3] = ctx;
    b[4 + 4 + 4] = 0;                        /* (buffer header) */
    b[8 + 5] = length;                       /* length at buffer header +20 */
    b[8 + 7] = flags;
    post(12, b, sizeof b, 0);
}

/* frames: the value after each 00 00 00 01 */
static int frame_values(uint8_t *v)
{
    int n = 0;
    for (uint32_t i = 0; i + 4 < got_len; i++)
        if (!got_stream[i] && !got_stream[i + 1] && !got_stream[i + 2] && got_stream[i + 3] == 1) v[n++] = got_stream[i + 4];
    return n;
}

static void render(uint8_t val, uint8_t *dst)
{
    uint32_t pw = (out_w + 31) & ~31u, ph = (out_h + 15) & ~15u;
    memset(dst, 0x55, pw * ph * 3 / 2);      /* padding: must not be checksummed */
    for (uint32_t y = 0; y < H; y++) memset(dst + y * pw, val, W);
    for (uint32_t y = 0; y < (H + 1) / 2; y++) {
        memset(dst + pw * ph + y * (pw / 2), val + 1, (W + 1) / 2);
        memset(dst + pw * ph + (pw / 2) * (ph / 2) + y * (pw / 2), val + 2, (W + 1) / 2);
    }
}

/* hands out the next picture if there's an output buffer */
static void produce(void)
{
    uint8_t vals[64];
    int n;
    if (!eos_in || need_rx || go_quiet || awaiting_reformat) return;
    n = frame_values(vals);
    if (!nout) return;
    if (long_event == 1) {                   /* an event whose data comes by bulk transfer */
        uint32_t ev[5 + 64 + 1];
        memset(ev, 0, sizeof ev);
        ev[1] = 2; ev[3] = 0x48435045u; ev[4] = 300;
        post(16, ev, sizeof ev, 0);
        need_rx = 3; rx_len_expected = 300;  /* its data follows by bulk */
        long_event = 2;
        return;
    }
    if (!efch_sent) {                        /* the first thing out: a format change */
        uint32_t ev[5 + 256 / 4 + 1];
        uint32_t *fc;
        memset(ev, 0, sizeof ev);
        ev[1] = 3; ev[2] = 0; ev[3] = 0x48434645u; ev[4] = 232;
        fc = &ev[5];
        if (big_efch) { out_w = 200; out_h = 100; }
        fc[0] = ((out_w + 31) & ~31u) * ((out_h + 15) & ~15u) * 3 / 2;   /* size min */
        fc[1] = 1; fc[2] = fc[0]; fc[3] = 3;
        fc[5 + 0] = 3; fc[5 + 1] = 0x30323449u;                          /* format: video, I420 */
        fc[13 + 0] = (out_w + 31) & ~31u; fc[13 + 1] = (out_h + 15) & ~15u; /* video width/height */
        fc[13 + 4] = W; fc[13 + 5] = H;                                   /* crop w/h */
        post(16, ev, sizeof ev, 0);
        efch_sent = 1;
        if (big_efch) { awaiting_reformat = 1; return; }   /* nothing more until the port is re-enabled */
    }
    if (error_event && frames_made == 2) {
        uint32_t ev[5 + 64 + 1];
        memset(ev, 0, sizeof ev);
        ev[1] = 3; ev[3] = 0x4F525245u;
        post(16, ev, sizeof ev, 0);
        error_event = 0;
        return;
    }
    if (frames_made < n) {
        uint32_t pw = (out_w + 31) & ~31u, ph = (out_h + 15) & ~15u, size = pw * ph * 3 / 2;
        if (!frames_made && !first_event_sent) {   /* as the Pi: a 44 byte event (cmd 0, length = a picture's size), data by bulk */
            uint32_t ev[5] = { 1, 3, 0, 0, size };
            post(16, ev, sizeof ev, 0);
            need_rx = 3; rx_len_expected = size;
            first_event_sent = 1;
            return;
        }
        render(vals[frames_made], cur_frame);
        if (frames_made == corrupt_at) cur_frame[5] ^= 1;
        buffer_back(2, (uint32_t)out_bufs[--nout], size, 0);
        need_rx = 1; rx_len_expected = size;
        frames_made++;
    } else if (!eos_out_sent) {
        buffer_back(2, (uint32_t)out_bufs[--nout], 0, 1);
        need_rx = 2; rx_len_expected = 8;
        eos_out_sent = 1;
    }
}

static void firmware(const uint32_t *m, uint32_t len)
{
    const uint32_t *p = m + 6;
    CHECK(m[0] == MAGIC && len <= 512, "bad message");
    switch (m[1]) {
    case 4: { uint32_t r[5] = { 0, 0xC0DE, 1, 1, 1 }; created++; reply(m, r, sizeof r); break; }
    case 8: {                                /* PORT_INFO_GET */
        uint32_t r[6 + 16 + 8 + 13 + 32];
        memset(r, 0, sizeof r);
        r[1] = 0xC0DE; r[2] = p[1]; r[5] = p[1] - 1;          /* input handle 1, output 2 */
        r[6 + 6] = 1; r[6 + 7] = 2048; r[6 + 9] = 20; r[6 + 10] = 81920;
        reply(m, r, sizeof r);
        break;
    }
    case 9: {                                /* PORT_INFO_SET */
        uint32_t r[6 + 16 + 8 + 13 + 32];
        const uint32_t *port = p + 3, *fmt = p + 3 + 16, *vid = p + 3 + 16 + 8;
        memset(r, 0, sizeof r);
        r[1] = 0xC0DE; r[2] = p[0]; r[5] = p[1] - 1;
        memcpy(&r[6], port, 64); memcpy(&r[6 + 16], fmt, 32); memcpy(&r[6 + 16 + 8], vid, 44);
        if (p[1] == 2) {
            CHECK(fmt[1] == 0x34363248u && vid[0] == W && vid[1] == H && port[11] == 3 && port[12] == 65536,
                  "input format %08X %ux%u, %u x %u", fmt[1], vid[0], vid[1], port[11], port[12]);
        } else if (p[1] == 3) {
            CHECK(fmt[1] == 0x30323449u && vid[0] % 32 == 0 && vid[1] % 16 == 0, "output format %08X %ux%u", fmt[1], vid[0], vid[1]);
            r[6 + 10] = vid[0] * vid[1] * 3 / 2;               /* size recommended */
            out_size_set = port[12];
        }
        reply(m, r, sizeof r);
        break;
    }
    case 6: comp_enabled = 1; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 7: comp_enabled = 0; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 5: destroyed++; { uint32_t r = 0; reply(m, &r, 4); } break;
    case 10: {                               /* PORT_ACTION */
        uint32_t r = 0, port = p[1];
        CHECK(port == 1 || port == 2, "port action on %u", port);
        if (p[2] == 1) { port_on[port] = 1; if (port == 2) awaiting_reformat = 0; }
        else {
            port_on[port] = 0;
            if (port == 2) {
                disables_out++;
                while (nout) buffer_back(2, (uint32_t)out_bufs[--nout], 0, 0);   /* before the reply, as VC does */
            }
        }
        reply(m, &r, 4);
        produce();
        break;
    }
    case 11: {                               /* BUFFER_FROM_HOST */
        const drv_t *d = (const drv_t *)p;
        uint32_t length = p[8 + 5], flags = p[8 + 7], pim = p[8 + 14 + 10 + 2];
        CHECK(d->magic == MAGIC && d->comp == 0xC0DE, "drvbuf");
        if (d->port == 1) {
            CHECK(port_on[1], "input buffer with the port off");
            if (pim) {
                CHECK(pim == length && length <= 128, "short data %u/%u", pim, length);
                memcpy(got_stream + got_len, (const uint8_t *)&p[8 + 14 + 10 + 3], pim);
                got_len += pim;
                shorts++;
                buffer_back(1, d->ctx, 0, 0);
            } else if (length) {
                pending_tx = length; pending_tx_ctx = d->ctx;
            } else {
                buffer_back(1, d->ctx, 0, 0);
            }
            if (flags & 1) eos_in = 1;
        } else {
            CHECK(d->port == 2 && port_on[2], "output buffer for port %u (on %d)", d->port, port_on[2]);
            CHECK(p[8 + 4] >= out_size_set, "output alloc_size %u < %u", p[8 + 4], out_size_set);
            out_bufs[nout++] = (int)d->ctx;
        }
        produce();
        break;
    }
    default:
        CHECK(0, "message type %u", m[1]);
    }
}

typedef void stub_fn(uint32_t param, uint32_t reason, uint32_t h);

/* PCI_RAMAlloc: the only memory VCHIQ's bulk transfers handle (it assumes
   physically contiguous); every bulk range must lie inside one block */
static uint32_t pci_lo[32], pci_hi[32];
static int npci_blocks, pci_live, no_pci_mem;
static void pci_open(int rw)
{
    for (int i = 0; i < npci_blocks; i++)
        if (pci_lo[i]) mprotect((void *)(uintptr_t)pci_lo[i], (pci_hi[i] - pci_lo[i] + 4095) & ~4095u, rw ? PROT_READ | PROT_WRITE : PROT_NONE);
}
static int in_pci(uint32_t a, uint32_t n)
{
    for (int i = 0; i < npci_blocks; i++)
        if (pci_lo[i] && a >= pci_lo[i] && a + n <= pci_hi[i]) return 1;
    return 0;
}

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    uint32_t *R = (uint32_t *)r->r;
    switch (n) {
    case 0x42: R[0] = time_cs++; return NULL;
    case 0x1E:
        if (R[0] == 6) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            stub = (uint32_t)(uintptr_t)p;
            R[2] = stub;
            return NULL;
        }
        if (R[0] == 7) { freed++; CHECK(R[2] == stub, "freed another block"); return NULL; }
        break;
    case 0x6E: CHECK(R[0] == 1 && R[1] == stub, "sync"); __builtin___clear_cache((char *)(uintptr_t)stub, (char *)(uintptr_t)stub + 64); return NULL;
    case 0x59200: R[0] = 0x1A57; return NULL;
    case 0x59201: connects++; return NULL;
    case 0x59202: disconnects++; return NULL;
    case 0x5920A: {
        uint32_t *s = (uint32_t *)(uintptr_t)R[1];
        opens++;
        CHECK(s[2] == 0x6D6D616Cu && s[6] == stub && s[7] == stub + 64, "open params");
        uses++;
        R[0] = 0x5E7;
        return NULL;
    }
    case 0x59209: closes++; return NULL;
    case 0x5920D: uses++; return NULL;
    case 0x5920E: releases++; return NULL;
    case 0x59205: CHECK(uses > releases, "queue while not in use"); firmware((const uint32_t *)(uintptr_t)R[1], R[2]); return NULL;
    case 0x39: {
        const char *nm = (const char *)(uintptr_t)R[1];
        R[0] = !strcmp(nm, "PCI_RAMAlloc") ? 0x50100 : !strcmp(nm, "PCI_RAMFree") ? 0x50101 : 0;
        return R[0] ? NULL : &err;
    }
    case 0x50100: {
        void *p;
        CHECK(R[1] == 4096, "PCI alignment %u", R[1]);
        if (no_pci_mem) return &err;
        /* privileged on RISC OS: no access from "USR mode" here (a stray
           access is a SIGSEGV); probe_svc_copy and the fake VideoCore open it */
        p = mmap(NULL, (R[0] + 4095) & ~4095u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pci_lo[npci_blocks] = (uint32_t)(uintptr_t)p; pci_hi[npci_blocks++] = (uint32_t)(uintptr_t)p + R[0];
        pci_live++;
        R[0] = (uint32_t)(uintptr_t)p; R[1] = 0x01000000;
        return NULL;
    }
    case 0x50101:
        for (int i = 0; i < npci_blocks; i++)
            if (pci_lo[i] == R[0]) {
                munmap((void *)(uintptr_t)R[0], (pci_hi[i] - pci_lo[i] + 4095) & ~4095u);
                pci_lo[i] = 0; pci_live--; return NULL;
            }
        CHECK(0, "PCI_RAMFree of an unknown block");
        return &err;
    case 0x59203:                            /* BulkQueueTransmit */
        CHECK(in_pci(R[1], R[2]), "transmit from memory that isn't PCI_RAMAlloc'd");
        CHECK(uses > releases, "transmit while not in use");
        CHECK(pending_tx && R[2] == pending_tx && R[3] == 4 && (R[1] & 3) == 0, "transmit %u (expected %u), flags %u",
              R[2], pending_tx, R[3]);
        pci_open(1);
        memcpy(got_stream + got_len, (const void *)(uintptr_t)R[1], R[2]);
        pci_open(0);
        got_len += R[2];
        bulks_tx++;
        buffer_back(1, pending_tx_ctx, 0, 0);
        pending_tx = 0;
        produce();
        return NULL;
    case 0x5920F: {                          /* BulkQueueReceive */
        CHECK(need_rx && R[2] == ((rx_len_expected + 3) & ~3u) && R[3] == 6, "receive %u (expected %u), flags %u",
              R[2], rx_len_expected, R[3]);
        CHECK((R[1] & 63) == 0, "receive buffer not aligned");
        CHECK(in_pci(R[1], R[2]), "receive into memory that isn't PCI_RAMAlloc'd");
        pci_open(1);
        if (need_rx == 1) memcpy((void *)(uintptr_t)R[1], cur_frame, R[2]);
        pci_open(0);
        need_rx = 0;
        bulks_rx++;
        ((stub_fn *)(uintptr_t)stub)(stub + 64, 4, R[4]);      /* the real callback */
        produce();
        return NULL;
    }
    case 0x59204:
        CHECK(R[2] == 512 && R[3] == 0, "dequeue regs");
        if (!noutq) return &empty;
        memcpy((void *)(uintptr_t)R[1], outq[0], outq_len[0]);
        R[2] = outq_len[0];
        memmove(outq[0], outq[1], sizeof outq[0] * (size_t)(noutq - 1));
        memmove(outq_len, outq_len + 1, sizeof outq_len[0] * (size_t)(noutq - 1));
        noutq--;
        return NULL;
    }
    CHECK(0, "unexpected SWI &%X", n);
    return &err;
}

/* ---- the test stream and its expected list ---- */

static uint32_t adler0(uint32_t a, const uint8_t *p, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    while (n--) { s1 = (s1 + *p++) % 65521; s2 = (s2 + s1) % 65521; }
    return s2 << 16 | s1;
}

static void make_files(void)
{
    FILE *f = fopen("/tmp/mmaldecode_test.h264", "wb"), *c = fopen("/tmp/mmaldecode_test.crc", "w");
    uint8_t filler[40000];
#define FILLER 36415
    memset(filler, 0xAA, sizeof filler);
    fprintf(c, "#tb 0: 1/25\n#dimensions 0: %dx%d\n", W, H);
    for (int k = 0; k < NFRAMES; k++) {
        uint8_t v = (uint8_t)(10 + 7 * k), sc[5] = { 0, 0, 0, 1, v };
        uint8_t y[W], u[(W + 1) / 2], vv[(W + 1) / 2];
        uint32_t a = 0;
        fwrite(sc, 1, 5, f);
        fwrite(filler, 1, FILLER, f);        /* 5 whole 64 KB pieces, then 100 bytes (in the message) */
        memset(y, v, W); memset(u, v + 1, sizeof u); memset(vv, v + 2, sizeof vv);
        for (int r = 0; r < H; r++) a = adler0(a, y, W);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, u, sizeof u);
        for (int r = 0; r < (H + 1) / 2; r++) a = adler0(a, vv, sizeof vv);
        fprintf(c, "0, %10d, %10d, 1, %8d, 0x%08x\n", k, k, W * H * 3 / 2, a);
    }
    fclose(f);
    fclose(c);
}

static char *run(int *ret, int keep_going)
{
    static char buf[16384];
    char *argv[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out", "-n",
                     "/tmp/mmaldecode_test.h264", "/tmp/mmaldecode_test.crc", NULL };
    char *argv2[] = { "mmaldecode", "-o", "/tmp/mmaldecode_test.out",
                      "/tmp/mmaldecode_test.h264", "/tmp/mmaldecode_test.crc", NULL };
    FILE *f;
    size_t got;
    uses = releases = opens = closes = connects = disconnects = freed = created = destroyed = comp_enabled = 0;
    memset(port_on, 0, sizeof port_on);
    shorts = bulks_tx = bulks_rx = efch_sent = disables_out = 0;
    got_len = pending_tx = 0; awaiting_reformat = 0; first_event_sent = 0; out_w = W; out_h = H; nout = frames_made = eos_in = eos_out_sent = need_rx = 0;
    noutq = 0;
    remove("/tmp/mmaldecode_test.out");
    *ret = keep_going ? probe_main(6, argv) : probe_main(5, argv2);
    f = fopen("/tmp/mmaldecode_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static void cleaned(const char *what)
{
    CHECK(uses == releases, "%s: use/release %d/%d", what, uses, releases);
    CHECK(opens == closes && connects == disconnects && freed == 1, "%s: open/close %d/%d, connect %d/%d, freed %d",
          what, opens, closes, connects, disconnects, freed);
    CHECK(pci_live == 0, "%s: %d PCI blocks not freed", what, pci_live);
    npci_blocks = 0;
    CHECK(!comp_enabled && !port_on[1] && !port_on[2] && created == destroyed, "%s: left enabled (%d %d %d), %d/%d",
          what, comp_enabled, port_on[1], port_on[2], created, destroyed);
}

int main(void)
{
    int ret;
    char *o;
    make_files();

    o = run(&ret, 0);
    printf("%s", o);
    CHECK(ret == 0 && strstr(o, "9 pictures decoded") && strstr(o, "9 of 9 checked against FFmpeg: 0 wrong") &&
          strstr(o, "Result: OK"), "normal path (%d)", ret);
    CHECK(bulks_tx == 6 && shorts == 0, "pieces: %d by bulk, %d in the message", bulks_tx, shorts);
    CHECK(bulks_rx == 11 && disables_out == 1, "receives %d (event + 9 pictures + EOS), disables %d (at the end)", bulks_rx, disables_out);
    CHECK(got_len == 9 * (FILLER + 5), "stream arrived whole: %u", got_len);
    cleaned("normal");

    big_efch = 1;
    o = run(&ret, 0);
    CHECK(ret == 0 && disables_out == 2 && strstr(o, "Format changed: I420 224x112") && strstr(o, "Result: OK"),
          "bigger format (%d, disables %d):\n%s", ret, disables_out, o);
    cleaned("bigger");
    big_efch = 0;

    corrupt_at = 3;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "Picture 3: checksum") && strstr(o, "pictures differ"), "wrong picture (%d)", ret);
    cleaned("wrong");
    o = run(&ret, 1);
    CHECK(ret == 1 && strstr(o, "9 of 9 checked against FFmpeg: 1 wrong"), "-n (%d):\n%s", ret, o);
    cleaned("-n");
    corrupt_at = -1;

    go_quiet = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "Nothing from the decoder") && strstr(o, "didn't finish"), "quiet (%d)", ret);
    cleaned("quiet");
    go_quiet = 0;

    long_event = 1;
    o = run(&ret, 0);
    CHECK(ret == 0 && long_event == 2 && bulks_rx == 12 && strstr(o, "Event EPCH (&48435045, 300 bytes) on port type 2"),
          "long event (%d, %d receives):\n%s", ret, bulks_rx, o);
    cleaned("long event");
    long_event = 0;

    error_event = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "reports an error"), "error event (%d)", ret);
    cleaned("error");

    no_pci_mem = 1;
    o = run(&ret, 0);
    CHECK(ret == 1 && strstr(o, "No physically contiguous memory") && !opens && !pci_live, "no PCI memory (%d)", ret);
    npci_blocks = 0;
    no_pci_mem = 0;

    printf(fails ? "mmaldecode_test: %d failures\n" : "mmaldecode_test: all passed\n", fails);
    return fails != 0;
}
