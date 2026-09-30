/*
 * vchiqprobe (tools/vchiqprobe) against fake modules and a fake firmware:
 *   - a VCHIQ module with a SWI table and *commands (one with message
 *     tokens), two clients (one using the X forms, one a SWI past the
 *     names) and a module with no calls;
 *   - -d: the directory made, VCHIQ and both clients saved as Data;
 *   - no VCHIQ module: result 2; a module with no SWIs: result 1;
 *   - an insane length word: the 64K guess, still parsed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

int probe_main(int argc, char **argv);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define CHUNK 0x56C00u
typedef struct { uint32_t len; uint8_t b[0x10000]; } fmod_t;   /* the length word, then the module */
static fmod_t vchiq, bcmsound, other, portable;
static fmod_t *mods[4];
static int nmods, have_vchiq = 1, mkdirs, saves;
static char saved[8][64];
static uint32_t saved_type[8], saved_len[8];
static _kernel_oserror err = { 1, "no" };

void probe_copy(uintptr_t src, void *dst, size_t n) { memcpy(dst, (const void *)src, n); }

static void put32(uint8_t *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static uint32_t puts_at(uint8_t *b, uint32_t off, const char *s) { strcpy((char *)b + off, s); return off + (uint32_t)strlen(s) + 1; }

static void make(fmod_t *m, const char *title, uint32_t chunk, int with_table, uint32_t len)
{
    uint32_t p = 0x100;
    memset(m, 0, sizeof *m);
    m->len = len;
    put32(m->b, 0x10, p); p = puts_at(m->b, p, title);
    put32(m->b, 0x14, p); p = puts_at(m->b, p, "Test\t1.00 (01 Jan 2026)");
    put32(m->b, 0x1C, chunk);
    if (with_table) {
        put32(m->b, 0x20, 0x800);
        put32(m->b, 0x24, p);
        p = puts_at(m->b, p, "VCHIQ");
        p = puts_at(m->b, p, "Open");
        p = puts_at(m->b, p, "Close");
        p = puts_at(m->b, p, "QueueMessage");
        m->b[p++] = 0;
        p = (p + 3) & ~3u;
        put32(m->b, 0x18, p);
        p = puts_at(m->b, p, "VCHIQInfo"); p = (p + 3) & ~3u;
        put32(m->b, p, 0x900); put32(m->b, p + 4, 0); put32(m->b, p + 8, 0x200); put32(m->b, p + 12, 0x220); p += 16;
        p = puts_at(m->b, p, "VCHIQTok"); p = (p + 3) & ~3u;
        put32(m->b, p, 0x904); put32(m->b, p + 4, 1u << 28); put32(m->b, p + 8, 0x240); put32(m->b, p + 12, 0x248); p += 16;
        m->b[p] = 0;
        puts_at(m->b, 0x200, "Syntax: *VCHIQInfo");
        puts_at(m->b, 0x220, "Shows things.");
        puts_at(m->b, 0x240, "SVTOK");
        puts_at(m->b, 0x248, "HVTOK");
        put32(m->b, 0x2C, 0x260);
        puts_at(m->b, 0x260, "Resources:$.Resources.VCHIQ.Messages");
    }
}

_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r)
{
    if (n == 0x591C5) {
        uint32_t *b = (uint32_t *)(uintptr_t)r->r[0];
        b[1] = 0x80000000u;
        b[5] = 0xA02082;                       /* Pi 3 B */
        return NULL;
    }
    if (n == 0x1E && r->r[0] == 18) {
        const char *want = (const char *)(uintptr_t)r->r[1];
        for (int i = 0; i < nmods; i++)
            if (!strcmp((char *)mods[i]->b + 0x100, want) && (mods[i] != &vchiq || have_vchiq)) {
                r->r[3] = (int)(uintptr_t)mods[i]->b;
                return NULL;
            }
        return &err;
    }
    if (n == 0x1E && r->r[0] == 12) {
        int i = r->r[1];
        if (i >= nmods) return &err;
        r->r[3] = (int)(uintptr_t)mods[i]->b;
        r->r[1] = i + 1; r->r[2] = 0;
        return NULL;
    }
    if (n == 0x08 && r->r[0] == 8) { mkdirs++; return NULL; }
    if (n == 0x08 && r->r[0] == 10) {
        if (saves < 8) {
            snprintf(saved[saves], sizeof saved[0], "%s", (const char *)(uintptr_t)r->r[1]);
            saved_type[saves] = (uint32_t)r->r[2];
            saved_len[saves] = (uint32_t)(r->r[5] - r->r[4]);
        }
        saves++;
        return NULL;
    }
    CHECK(0, "unexpected SWI &%X", n);
    return &err;
}

static char *run(int argc, char **argv, int *ret)
{
    static char buf[16384];
    FILE *f;
    size_t got;
    *ret = probe_main(argc, argv);
    f = fopen("/tmp/vchiqprobe_test.out", "r");
    got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[got] = 0;
    if (f) fclose(f);
    return buf;
}

int main(void)
{
    char *o;
    int ret;
    char *args[] = { "vchiqprobe", "-o", "/tmp/vchiqprobe_test.out", "-d", "<Obey$Dir>.Modules", NULL };

    make(&vchiq, "VCHIQ", CHUNK, 1, 0x1000 + 4);
    make(&bcmsound, "BCMSound", 0, 0, 0x1000 + 4);
    put32(bcmsound.b, 0x800, 0xEF000000u | (CHUNK + 0x20000u));        /* SWI XVCHIQ_Open */
    put32(bcmsound.b, 0x804, 0x0F000000u | (CHUNK + 2));               /* SWIEQ VCHIQ_QueueMessage */
    put32(bcmsound.b, 0x808, 0xEF000000u | (CHUNK + 0x20002u));
    make(&portable, "Portable", 0, 0, 0x7FFFFFFF);                      /* an insane length: 64K guess */
    put32(portable.b, 0x900, 0xEF000000u | (CHUNK + 5));                /* past the names */
    make(&other, "Other", 0, 0, 0x1000 + 4);
    put32(other.b, 0x800, 0xEF000000u | 0x20);                          /* OS_ swis: not counted */
    put32(other.b, 0x804, 0xFF000000u | CHUNK);                         /* cond 0xF: not a SWI */
    mods[0] = &other; mods[1] = &vchiq; mods[2] = &bcmsound; mods[3] = &portable; nmods = 4;

    o = run(5, args, &ret);
    printf("%s", o);
    CHECK(ret == 0, "result %d", ret);
    CHECK(strstr(o, "Pi 3 B, BCM2837"), "board");
    CHECK(strstr(o, "SWI chunk &56C00, prefix \"VCHIQ\", 3 names") && strstr(o, "&56C02  VCHIQ_QueueMessage"), "SWIs");
    CHECK(strstr(o, "VCHIQInfo") && strstr(o, "syntax: Syntax: *VCHIQInfo") && strstr(o, "help: Shows things."), "command");
    CHECK(strstr(o, "tokens: SVTOK / HVTOK"), "tokenised command");
    CHECK(strstr(o, "Messages file: Resources:$.Resources.VCHIQ.Messages"), "messages");
    CHECK(strstr(o, "BCMSound: 3 calls: Open(1) QueueMessage(2)"), "BCMSound's calls");
    CHECK(strstr(o, "Portable: 1 call: chunk+5(1)"), "Portable's call");
    CHECK(!strstr(o, "Other:"), "Other isn't a client");
    CHECK(mkdirs == 1 && saves == 3, "mkdir %d saves %d", mkdirs, saves);
    CHECK(!strcmp(saved[0], "<Obey$Dir>.Modules.VCHIQ") && saved_len[0] == 0x1000, "VCHIQ saved: %s %u", saved[0], saved_len[0]);
    CHECK(!strcmp(saved[1], "<Obey$Dir>.Modules.BCMSound") && saved_type[1] == 0xFFD, "BCMSound saved as Data");
    CHECK(!strcmp(saved[2], "<Obey$Dir>.Modules.Portable") && saved_len[2] == 0x10000 - 4, "Portable: the 64K guess (%u)", saved_len[2]);
    CHECK(strstr(o, "Result: OK - the interface and its users"), "result line");

    /* no VCHIQ */
    have_vchiq = 0; saves = 0;
    o = run(3, args, &ret);
    CHECK(ret == 2 && strstr(o, "No module called VCHIQ") && !saves, "no VCHIQ: %d", ret);
    have_vchiq = 1;

    /* a module with no SWIs */
    {
        char *a2[] = { "vchiqprobe", "-o", "/tmp/vchiqprobe_test.out", "-m", "Other", NULL };
        o = run(5, a2, &ret);
        CHECK(ret == 1 && strstr(o, "No SWIs.") && strstr(o, "reached another way"), "no SWIs: %d", ret);
    }

    printf(fails ? "vchiqprobe_test: %d failures\n" : "vchiqprobe_test: all passed\n", fails);
    return fails != 0;
}
