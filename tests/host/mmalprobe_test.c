/*
 * mmalprobe (tools/mmalprobe) against a fake VCHIQ module and a fake MMAL
 * firmware, which checks each request the way the VideoCore would:
 *   - a Pi 4: create ril.video_decode, the three ports, the encodings
 *     (H.264 among them), destroy; everything closed, the callback stub
 *     (MOV PC,R14, code areas synchronised) freed; Use/Release balanced;
 *   - the service open refused: nothing sent, still disconnected;
 *   - a component the firmware doesn't know: ENOENT, no ports asked;
 *   - a firmware that never answers: timeouts, still cleaned up;
 *   - an unrelated message arriving first: skipped;
 *   - no H.264 in the list: result 3.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

int probe_main(int argc, char **argv);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define MAGIC 0x6C616D6Du
static _kernel_oserror err = { 1, "fake error" }, empty = { 2, "no message" };
static int open_refuses, silent, unknown_comp, no_h264, stray_first;
static int inits, connects, disconnects, opens, closes, uses, releases, queued, synced, created, destroyed, claimed, freed;
static uint32_t stub_addr, stub_word, time_cs;
static uint32_t replies[8][128], reply_len[8];
static int nreplies;

static void reply(const uint32_t *req, const void *payload, uint32_t len)
{
    uint32_t *m = replies[nreplies];
    memset(m, 0, 512);
    m[0] = MAGIC; m[1] = req[1]; m[3] = req[3];
    memcpy(m + 6, payload, len);
    reply_len[nreplies++] = 24 + len;
}

static void firmware(const uint32_t *m, uint32_t len)
{
    const uint32_t *p = m + 6;
    CHECK(m[0] == MAGIC && len >= 24 && len <= 512, "a bad message (magic %08X, %u bytes)", m[0], len);
    if (silent) return;
    if (stray_first) {                        /* something else first */
        uint32_t s[6] = { 0 };
        uint32_t fake[6] = { MAGIC, 16, 0, 0, 0, 0 };
        reply(fake, s, 8);
        stray_first = 0;
    }
    switch (m[1]) {
    case 4: {                                  /* COMPONENT_CREATE */
        uint32_t r[5] = { 0, 0xC0DE, 1, 1, 0 };
        CHECK(len == 24 + 4 + 128 + 4, "create size %u", len);
        if (unknown_comp || strcmp((const char *)&p[1], "ril.video_decode")) r[0] = 5;
        else created++;
        reply(m, r, sizeof r);
        break;
    }
    case 8: {                                  /* PORT_INFO_GET */
        uint32_t r[6 + 16 + 8 + 13 + 32];
        memset(r, 0, sizeof r);
        CHECK(p[0] == 0xC0DE && p[2] == 0, "port info for %X/%u", p[0], p[2]);
        r[1] = p[0]; r[2] = p[1]; r[5] = 0x100 + p[1];
        r[6 + 6] = 20; r[6 + 7] = 65536;      /* buffer_num_min, buffer_size_min */
        r[6 + 16 + 1] = p[1] == 2 ? 0x34363248u : 0x30323449u;   /* H264 / I420 */
        r[6 + 16 + 8] = 1920; r[6 + 16 + 8 + 1] = 1088;
        reply(m, r, sizeof r);
        break;
    }
    case 15: {                                 /* PORT_PARAMETER_GET */
        uint32_t r[3 + 4] = { 0, 1, 8 + 16, 0x3247504Du, 0x34363248u, 0x47504A4Du, 0x31435657u };
        CHECK(p[0] == 0xC0DE && p[1] == 0x102 && p[2] == 1 && p[3] == 8 + 256, "parameter get %X %X %u %u", p[0], p[1], p[2], p[3]);
        if (no_h264) r[4] = 0x38505056u;       /* VP8 */
        reply(m, r, sizeof r);
        break;
    }
    case 5: {
        uint32_t r = 0;
        CHECK(p[0] == 0xC0DE, "destroy %X", p[0]);
        destroyed++;
        reply(m, &r, 4);
        break;
    }
    default:
        CHECK(0, "message type %u", m[1]);
    }
}

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    uint32_t *R = (uint32_t *)r->r;
    switch (n) {
    case 0x591C5: { uint32_t *b = (uint32_t *)(uintptr_t)R[0]; b[1] = 0x80000000u; b[5] = 0xB03115; return NULL; }
    case 0x42: R[0] = time_cs++; return NULL;
    case 0x1E:
        if (R[0] == 6) { claimed++; R[2] = (uint32_t)(uintptr_t)malloc(R[3]); stub_addr = R[2]; return NULL; }
        if (R[0] == 7) { freed++; CHECK(R[2] == stub_addr, "freed something else"); free((void *)(uintptr_t)R[2]); return NULL; }
        break;
    case 0x6E:
        synced++; stub_word = *(uint32_t *)(uintptr_t)stub_addr;
        CHECK(R[0] == 1 && R[1] == stub_addr && R[2] == stub_addr + 4, "sync range");
        return NULL;
    case 0x59200: inits++; CHECK(R[0] == 0, "Initialise R0"); R[0] = 0x1A57; return NULL;
    case 0x59201: connects++; CHECK(R[0] == 0 && R[1] == 0 && R[2] == 0x1A57, "Connect regs"); return NULL;
    case 0x59202: disconnects++; CHECK(R[0] == 0x1A57, "Disconnect R0"); return NULL;
    case 0x5920A: {
        uint32_t *s = (uint32_t *)(uintptr_t)R[1];
        opens++;
        CHECK(R[0] == 0x1A57 && s[0] == 15 && s[1] == 10 && s[2] == 0x6D6D616Cu && s[6] == stub_addr && s[7] == 0,
              "ServiceOpen params %u %u %08X cb %08X", s[0], s[1], s[2], s[6]);
        if (open_refuses) return &err;
        uses++;                                /* opening leaves it in use */
        R[0] = 0x5E7;
        return NULL;
    }
    case 0x59209: closes++; CHECK(R[0] == 0x5E7, "Close R0"); return NULL;
    case 0x5920D: uses++; CHECK(R[0] == 0x5E7, "Use R0"); return NULL;
    case 0x5920E: releases++; CHECK(R[0] == 0x5E7, "Release R0"); return NULL;
    case 0x59205:
        queued++;
        CHECK(R[0] == 0x5E7 && uses > releases, "MsgQueue with the service in use");
        firmware((const uint32_t *)(uintptr_t)R[1], R[2]);
        return NULL;
    case 0x59204:
        CHECK(R[0] == 0x5E7 && R[2] == 512 && R[3] == 0, "MsgDequeue regs");
        if (!nreplies) return &empty;
        memcpy((void *)(uintptr_t)R[1], replies[0], reply_len[0]);
        R[2] = reply_len[0];
        memmove(replies[0], replies[1], sizeof replies[0] * (size_t)(nreplies - 1));
        memmove(reply_len, reply_len + 1, sizeof reply_len[0] * (size_t)(nreplies - 1));
        nreplies--;
        return NULL;
    }
    CHECK(0, "unexpected SWI &%X", n);
    return &err;
}

static char *run(int *ret, int extra, char *e1, char *e2)
{
    static char buf[16384];
    char *argv[] = { "mmalprobe", "-o", "/tmp/mmalprobe_test.out", e1, e2, NULL };
    FILE *f;
    size_t got;
    inits = connects = disconnects = opens = closes = uses = releases = queued = synced = created = destroyed = 0;
    claimed = freed = 0; nreplies = 0; stub_word = 0;
    *ret = probe_main(3 + extra, argv);
    f = fopen("/tmp/mmalprobe_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

static void common_cleanup(const char *what)
{
    CHECK(claimed == 1 && freed == 1 && synced == 1 && stub_word == 0xE1A0F00Eu, "%s: the stub (claimed %d freed %d synced %d word %08X)",
          what, claimed, freed, synced, stub_word);
    CHECK(connects == disconnects && inits == 1, "%s: connect/disconnect %d/%d", what, connects, disconnects);
    CHECK(opens == closes || (open_refuses && closes == 0), "%s: open/close %d/%d", what, opens, closes);
    CHECK(uses == releases, "%s: use/release %d/%d", what, uses, releases);
}

int main(void)
{
    int ret;
    char *o;

    o = run(&ret, 0, NULL, NULL);
    printf("%s", o);
    CHECK(ret == 0, "result %d", ret);
    CHECK(strstr(o, "Create ril.video_decode: SUCCESS, component &C0DE, 1 input, 1 output, 0 clock"), "create");
    CHECK(strstr(o, "Input port 0: SUCCESS, handle &102") && strstr(o, "encoding H264"), "input port");
    CHECK(strstr(o, "Output port 0: SUCCESS, handle &103") && strstr(o, "encoding I420") && strstr(o, "1920x1088"), "output port");
    CHECK(strstr(o, "at least 20 of 65536 bytes"), "buffers");
    CHECK(strstr(o, "Supported encodings on the input: MPG2 H264 MJPG WVC1"), "encodings");
    CHECK(strstr(o, "Destroy: SUCCESS") && destroyed == 1, "destroy");
    CHECK(strstr(o, "Result: OK - the VideoCore's video decoder answers and takes H.264"), "result");
    common_cleanup("Pi 4");

    open_refuses = 1;
    o = run(&ret, 0, NULL, NULL);
    CHECK(ret == 1 && strstr(o, "VCHIQ_ServiceOpen \"mmal\": fake error") && !queued, "open refused");
    common_cleanup("open refused");
    open_refuses = 0;

    unknown_comp = 1;
    o = run(&ret, 0, NULL, NULL);
    CHECK(ret == 1 && strstr(o, ": ENOENT") && queued == 1 && !destroyed, "unknown component (%d, %d)", ret, queued);
    common_cleanup("unknown");
    unknown_comp = 0;

    {
        o = run(&ret, 2, "-c", "ril.nothing");
        CHECK(ret == 1 && strstr(o, "Create ril.nothing: ENOENT"), "-c");
        common_cleanup("-c");
    }

    silent = 1;
    o = run(&ret, 0, NULL, NULL);
    CHECK(ret == 1 && strstr(o, "Create: no reply in 300 cs"), "silent firmware");
    common_cleanup("silent");
    silent = 0;

    stray_first = 1;
    o = run(&ret, 0, NULL, NULL);
    CHECK(ret == 0 && strstr(o, "(a type 16 message came first"), "stray message");
    common_cleanup("stray");

    no_h264 = 1;
    o = run(&ret, 0, NULL, NULL);
    CHECK(ret == 3 && strstr(o, "doesn't list H.264"), "no H.264: %d", ret);
    common_cleanup("no H.264");

    printf(fails ? "mmalprobe_test: %d failures\n" : "mmalprobe_test: all passed\n", fails);
    return fails != 0;
}
