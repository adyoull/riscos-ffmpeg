/*
 * vchiqprobe - what does RISC OS's VCHIQ module offer, and who uses it?
 *
 * The first step towards hardware H.264 decoding in Reel on every Pi from
 * the Pi 1 to the Pi 4: the VideoCore's decoder is reached with MMAL,
 * sent over VCHIQ, and RISC OS has a VCHIQ module in its ROM. Its client
 * interface isn't documented where we can read it, so this finds it out
 * from the module itself. It only reads:
 *
 *   1. the board (through BCMSupport's property mailbox), for the record;
 *   2. the VCHIQ module's header: title, help string, SWI chunk, the SWI
 *      names, the *commands with their syntax and help, the messages file;
 *   3. every other module, for SWI instructions in VCHIQ's chunk: which
 *      modules are its clients and which of its SWIs they call;
 *   4. with -d dir: copies of VCHIQ and its clients (type Data, so a
 *      double-click can't load them), to be disassembled for the calls'
 *      registers. Nothing is loaded, called or changed.
 *
 *   vchiqprobe [-o file] [-d dir] [-m module]
 *      -o file    also write the report to file
 *      -d dir     save the module copies in dir (created if need be)
 *      -m module  look at another module instead of VCHIQ
 *
 * Part of riscos-ffmpeg. MIT licence.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"

#define OS_File                            0x08
#define OS_Module                          0x1E
#define BCMSupport_SendTempPropertyBuffer  0x591C5
#define TAG_BOARD_REV                      0x00010002
#define MAX_MODULE                         (1u << 20)
#define MAX_CLIENTS                        16
#define MAX_SWIS                           64

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
/* host test: tests/host/vchiqprobe_test.c supplies these */
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);
void probe_copy(uintptr_t src, void *dst, size_t n);
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }

/* Copies n bytes (a multiple of 4) of module memory in SVC mode: the RMA
   and ROM needn't be readable from USR mode on every RISC OS 5. The
   destination is touched here first (a page not yet mapped would abort
   if first written in SVC mode). */
static void probe_copy(uintptr_t src, void *dst, size_t n)
{
    size_t words = n / 4;
    if (!words)
        return;
    memset(dst, 0, n);
    __asm__ volatile(
        "mov   r4, %0\n\t"
        "mov   r5, %1\n\t"
        "mov   r6, %2\n\t"
        "swi   0x16\n\t"                    /* OS_EnterOS */
        "1:\n\t"
        "ldr   r7, [r4], #4\n\t"
        "str   r7, [r5], #4\n\t"
        "subs  r6, r6, #1\n\t"
        "bne   1b\n\t"
        "swi   0x7C\n\t"                    /* OS_LeaveOS */
        :
        : "r"(src), "r"(dst), "r"(words)
        : "r0", "r4", "r5", "r6", "r7", "r14", "memory", "cc");
}
#endif

typedef struct {
    uintptr_t base;
    uint32_t len;
    uint8_t *img;                          /* a copy */
    char title[48];
} mod_t;

static uint32_t w32(const mod_t *m, uint32_t off)
{
    if (off + 4 > m->len) return 0;
    return (uint32_t)m->img[off] | (uint32_t)m->img[off + 1] << 8 |
           (uint32_t)m->img[off + 2] << 16 | (uint32_t)m->img[off + 3] << 24;
}

/* a string in the module, or "" if the offset is outside it */
static const char *str_at(const mod_t *m, uint32_t off)
{
    uint32_t i;
    if (!off || off >= m->len) return "";
    for (i = off; i < m->len; i++)
        if (!m->img[i]) return (const char *)m->img + off;
    return "";
}

/* Copies a module: its length is the word before it (a ROM module's
   length + 4, or an RMA block's size); trusted only within limits. */
static int mod_read(uintptr_t base, mod_t *m)
{
    uint32_t len = 0;
    memset(m, 0, sizeof *m);
    m->base = base;
    probe_copy(base - 4, &len, 4);
    if (len < 0x34 + 4 || len > MAX_MODULE)
        len = 0x10000;                      /* (a guess: enough for the header and some code) */
    len = (len - 4) & ~3u;
    m->img = malloc(len);
    if (!m->img) return -1;
    probe_copy(base, m->img, len);
    m->len = len;
    snprintf(m->title, sizeof m->title, "%s", str_at(m, w32(m, 0x10)));
    return 0;
}

static void mod_free(mod_t *m) { free(m->img); m->img = NULL; }

static int fw_board(uint32_t *rev)
{
    uint32_t buf[8] = { 8 * 4, 0, TAG_BOARD_REV, 4, 0, 0, 0, 0 };
    _kernel_swi_regs r;
    r.r[0] = (int)(uintptr_t)buf;
    r.r[1] = (int)(uintptr_t)buf;
    r.r[2] = 0;
    if (probe_swi(BCMSupport_SendTempPropertyBuffer, &r) || buf[1] != 0x80000000u)
        return -1;
    *rev = buf[5];
    return 0;
}

static const char *board_name(uint32_t rev)
{
    static const char *const types[] = {
        "Pi 1 A", "Pi 1 B", "Pi 1 A+", "Pi 1 B+", "Pi 2 B", "Alpha", "CM1", "?", "Pi 3 B", "Pi Zero",
        "CM3", "?", "Pi Zero W", "Pi 3 B+", "Pi 3 A+", "?", "CM3+", "Pi 4 B", "Pi Zero 2 W", "Pi 400",
        "CM4", "CM4S" };
    uint32_t t;
    if (!(rev & 0x800000u)) return "Pi 1 (old-style revision)";
    t = (rev >> 4) & 0xFF;
    return t < sizeof types / sizeof *types ? types[t] : "?";
}

static const char *const socs[] = { "BCM2835", "BCM2836", "BCM2837", "BCM2711", "BCM2712" };

/* the SWI names from the module's decoding table: names[i] = SWI chunk + i */
static int swi_names(const mod_t *m, const char **prefix, const char *names[MAX_SWIS])
{
    uint32_t t = w32(m, 0x24), p;
    int n = 0;
    *prefix = "";
    if (!t || t >= m->len) return 0;
    *prefix = str_at(m, t);
    p = t + (uint32_t)strlen(*prefix) + 1;
    while (p < m->len && m->img[p] && n < MAX_SWIS) {
        names[n] = str_at(m, p);
        if (!*names[n]) break;
        p += (uint32_t)strlen(names[n]) + 1;
        n++;
    }
    return n;
}

/* SWI instructions (any condition) in chunk..chunk+63, with or without X */
static int count_swis(const mod_t *m, uint32_t chunk, uint32_t used[MAX_SWIS])
{
    int total = 0;
    for (uint32_t off = 0; off + 4 <= m->len; off += 4) {
        uint32_t ins = w32(m, off), num;
        if ((ins & 0x0F000000u) != 0x0F000000u || (ins >> 28) == 0xF)
            continue;
        num = (ins & 0x00FFFFFFu) & ~0x20000u;
        if (num >= chunk && num < chunk + MAX_SWIS) {
            used[num - chunk]++;
            total++;
        }
    }
    return total;
}

static void save(const char *dir, const mod_t *m)
{
    char name[256], leaf[32];
    _kernel_swi_regs r;
    _kernel_oserror *e;
    int i, n = 0;
    for (i = 0; m->title[i] && n < 24; i++) {
        char c = m->title[i];
        leaf[n++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
    }
    leaf[n] = 0;
    snprintf(name, sizeof name, "%s.%s", dir, n ? leaf : "Unnamed");
    r.r[0] = 10; r.r[1] = (int)(uintptr_t)name; r.r[2] = 0xFFD;
    r.r[4] = (int)(uintptr_t)m->img; r.r[5] = (int)(uintptr_t)(m->img + m->len);
    e = probe_swi(OS_File, &r);
    if (e) say("  couldn't save %s: %s\n", name, e->errmess);
    else say("  saved %s (%u bytes, from &%08X)\n", name, (unsigned)m->len, (unsigned)m->base);
}

int probe_main(int argc, char **argv)
{
    const char *want = "VCHIQ", *dir = NULL, *prefix, *names[MAX_SWIS];
    mod_t vc, clients[MAX_CLIENTS];
    int nclients = 0, nswis, verdict = 0, i;
    uint32_t chunk, rev;
    _kernel_swi_regs r;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out2 = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            want = argv[++i];
        else {
            printf("Usage: vchiqprobe [-o file] [-d dir] [-m module]\n");
            return 1;
        }
    }
    say("vchiqprobe: RISC OS's %s module, its interface and its clients\n\n", want);

    /* 1. the board */
    if (fw_board(&rev) == 0) {
        uint32_t soc = rev & 0x800000u ? (rev >> 12) & 15 : 0;
        say("Board revision &%08X: %s, %s\n", (unsigned)rev, board_name(rev), soc < 5 ? socs[soc] : "?");
    } else {
        say("Board: the firmware didn't answer (BCMSupport?)\n");
    }

    /* 2. the module */
    r.r[0] = 18; r.r[1] = (int)(uintptr_t)want;
    if (probe_swi(OS_Module, &r)) {
        say("No module called %s is loaded.\n\nResult: no %s module\n", want, want);
        if (out2) fclose(out2);
        return 2;
    }
    if (mod_read((uintptr_t)(uint32_t)r.r[3], &vc)) {
        say("Out of memory\n");
        return 3;
    }
    say("\nModule %s at &%08X, %u bytes\n", vc.title, (unsigned)vc.base, (unsigned)vc.len);
    say("Help: %s\n", str_at(&vc, w32(&vc, 0x14)));
    chunk = w32(&vc, 0x1C);
    say("Start &%X  init &%X  final &%X  service &%X  SWI handler &%X  flags word %s\n",
        (unsigned)w32(&vc, 0), (unsigned)w32(&vc, 4), (unsigned)w32(&vc, 8), (unsigned)w32(&vc, 0xC),
        (unsigned)w32(&vc, 0x20), w32(&vc, 0x30) ? "present" : "absent");
    if (w32(&vc, 0x2C)) say("Messages file: %s\n", str_at(&vc, w32(&vc, 0x2C)));
    nswis = swi_names(&vc, &prefix, names);
    if (chunk) {
        say("SWI chunk &%X, prefix \"%s\", %d names:\n", (unsigned)chunk, prefix, nswis);
        for (i = 0; i < nswis; i++)
            say("  &%05X  %s_%s\n", (unsigned)(chunk + (uint32_t)i), prefix, names[i]);
    } else {
        say("No SWIs.\n");
    }
    {
        uint32_t c = w32(&vc, 0x18);
        if (c && c < vc.len) {
            say("*Commands:\n");
            while (c < vc.len && vc.img[c]) {
                const char *nm = str_at(&vc, c);
                uint32_t e = (c + (uint32_t)strlen(nm) + 1 + 3) & ~3u, info = w32(&vc, e + 4);
                say("  %s (code &%X, info &%08X)\n", nm, (unsigned)w32(&vc, e), (unsigned)info);
                if (!(info & (1u << 28))) {        /* not message tokens */
                    const char *syn = str_at(&vc, w32(&vc, e + 8)), *hlp = str_at(&vc, w32(&vc, e + 12));
                    if (*syn) say("    syntax: %s\n", syn);
                    if (*hlp) say("    help: %s\n", hlp);
                } else {
                    say("    syntax/help tokens: %s / %s\n", str_at(&vc, w32(&vc, e + 8)), str_at(&vc, w32(&vc, e + 12)));
                }
                c = e + 16;
            }
        }
    }

    /* 3. its clients */
    if (chunk) {
        int mno = 0, inst = 0;
        say("\nModules calling %s SWIs:\n", prefix);
        for (;;) {
            mod_t m;
            uint32_t used[MAX_SWIS] = { 0 };
            int n;
            r.r[0] = 12; r.r[1] = mno; r.r[2] = inst;
            if (probe_swi(OS_Module, &r)) break;
            mno = r.r[1]; inst = r.r[2];
            if ((uintptr_t)(uint32_t)r.r[3] == vc.base || mod_read((uintptr_t)(uint32_t)r.r[3], &m))
                continue;
            n = count_swis(&m, chunk, used);
            if (n) {
                say("  %s: %d call%s:", m.title, n, n == 1 ? "" : "s");
                for (i = 0; i < MAX_SWIS; i++)
                    if (used[i]) {
                        if (i < nswis) say(" %s(%u)", names[i], (unsigned)used[i]);
                        else say(" chunk+%d(%u)", i, (unsigned)used[i]);
                    }
                say("\n");
                if (nclients < MAX_CLIENTS) { clients[nclients++] = m; continue; }
            }
            mod_free(&m);
        }
        if (!nclients) say("  none\n");
    }

    /* 4. copies */
    if (dir) {
        say("\nCopies (type Data) in %s:\n", dir);
        r.r[0] = 8; r.r[1] = (int)(uintptr_t)dir; r.r[4] = 0;
        probe_swi(OS_File, &r);                /* (an existing directory is fine) */
        save(dir, &vc);
        for (i = 0; i < nclients; i++)
            save(dir, &clients[i]);
    }
    verdict = !chunk ? 1 : 0;
    say("\nResult: %s\n", verdict ? "the module has no SWIs: it must be reached another way" :
                          nclients ? "OK - the interface and its users are listed above" :
                                     "OK - the interface is listed above; no other module calls it");
    for (i = 0; i < nclients; i++) mod_free(&clients[i]);
    mod_free(&vc);
    if (out2) fclose(out2);
    return verdict;
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
