/*
 * eabiprobe - which ARMEABISupport (and friends) a machine has loaded, and
 * which copies are waiting to replace them.
 *
 * Every GCCSDK program (Reel, FFmpeg, Iris, ...) is a client of the
 * ARMEABISupport module while it runs, and ARMEABISupport refuses to quit
 * while it has any client ("There are still clients using ARMEABISupport").
 * So if a program's !Run asks for a newer ARMEABISupport than the one
 * loaded (RMEnsure ARMEABISupport <newer> RMLoad its own copy) while
 * another GCCSDK program is running, it fails to start. This reports what
 * decides that; it only reads:
 *
 *   1. the loaded ARMEABISupport, SharedUnixLibrary, PThreadTicker,
 *      VFPSupport and DDEUtils: version, where (ROM or RMA), how many
 *      instances, and ARMEABISupport's SWIs;
 *   2. the copies in System:Modules (canonical name and version);
 *   3. ARMEABISupport's own list of the programs it knows
 *      (*ARMEABISupport_Info, saved to a file and copied in);
 *   4. the running tasks;
 *   5. if the Filer has seen !Iris (Iris$Dir): the module lines of its
 *      !Run, and every module inside it, with its version;
 *   6. a verdict: a copy newer than the loaded ARMEABISupport would fail
 *      to load while any GCCSDK program runs.
 *
 *   eabiprobe [-o file] [-i infofile] [-l label] [-a appdir]...
 *      -o file      also add the report to file
 *      -i infofile  where *ARMEABISupport_Info's output is saved
 *      -l label     a heading for this run ("before Reel", ...)
 *      -a appdir    look inside another application, as for !Iris
 *
 * This program is itself a GCCSDK program, so it appears in
 * ARMEABISupport's list while it runs.
 *
 * Part of riscos-ffmpeg. GPL version 2 or later (see COPYING).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"

#define OS_CLI                     0x05
#define OS_GBPB                    0x0C
#define OS_Module                  0x1E
#define OS_FSControl               0x29
#define TaskManager_EnumerateTasks 0x42681
#define EABI_CHUNK                 0x59D00
#define MAX_MODULE                 (1u << 20)
#define MAX_APPS                   8

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
/* host test: tests/host/eabiprobe_test.c supplies these */
_kernel_oserror *probe_swi(int n, _kernel_swi_regs *r);
void probe_copy(uintptr_t src, void *dst, size_t n);
FILE *probe_fopen(const char *name, const char *mode);
#else
static _kernel_oserror *probe_swi(int n, _kernel_swi_regs *r) { return _kernel_swi(n, r, r); }
static FILE *probe_fopen(const char *name, const char *mode) { return fopen(name, mode); }

/* Copies n bytes (a multiple of 4) of module memory in SVC mode: the RMA
   and ROM needn't be readable from USR mode on every RISC OS 5. The
   destination is touched here first. */
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

/* ---- module images (loaded, or read from a file) ---- */

typedef struct {
    uint8_t *img;
    uint32_t len;
} mod_t;

static uint32_t w32(const mod_t *m, uint32_t off)
{
    if (off + 4 > m->len) return 0;
    return (uint32_t)m->img[off] | (uint32_t)m->img[off + 1] << 8 |
           (uint32_t)m->img[off + 2] << 16 | (uint32_t)m->img[off + 3] << 24;
}

static const char *str_at(const mod_t *m, uint32_t off)
{
    if (!off || off >= m->len) return "";
    for (uint32_t i = off; i < m->len; i++)
        if (!m->img[i]) return (const char *)m->img + off;
    return "";
}

/* A loaded module: its length is the word before it (trusted within limits) */
static int mod_copy(uintptr_t base, mod_t *m)
{
    uint32_t len = 0;
    probe_copy(base - 4, &len, 4);
    if (len < 0x34 + 4 || len > MAX_MODULE)
        len = 0x10000;
    len = (len - 4) & ~3u;
    m->img = malloc(len);
    if (!m->img) return -1;
    probe_copy(base, m->img, len);
    m->len = len;
    return 0;
}

static int mod_file(const char *name, mod_t *m)
{
    FILE *f = probe_fopen(name, "rb");
    long n;
    m->img = NULL; m->len = 0;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x34 || n > (long)MAX_MODULE || !(m->img = malloc((size_t)n))) { fclose(f); return -1; }
    m->len = (uint32_t)fread(m->img, 1, (size_t)n, f);
    fclose(f);
    return m->len >= 0x34 ? 0 : -1;
}

/* "ARMEABISupport\t1.05 (07 Jan 2023)": the version, scaled by 100 */
static int help_version(const char *help)
{
    const char *p = strchr(help, '\t');
    int major = 0, minor = 0;
    for (p = p ? p : help; *p; p++)        /* the first "digits.digits" */
        if (*p >= '0' && *p <= '9' && sscanf(p, "%d.%d", &major, &minor) == 2)
            return major * 100 + minor;
    return -1;
}

/* the help string, tabs as spaces, for printing */
static void help_text(const mod_t *m, char *buf, size_t n)
{
    snprintf(buf, n, "%s", str_at(m, w32(m, 0x14)));
    for (char *p = buf; *p; p++)
        if (*p == '\t') *p = ' ';
}

/* ---- 1. what's loaded ---- */

static const char *const watched[] = { "ARMEABISupport", "SharedUnixLibrary", "PThreadTicker", "VFPSupport", "DDEUtils" };
#define N_WATCHED ((int)(sizeof watched / sizeof watched[0]))
static int loaded_version[N_WATCHED];

static void loaded(void)
{
    say("Loaded modules:\n");
    for (int w = 0; w < N_WATCHED; w++) {
        _kernel_swi_regs r;
        int instances = 0, mno = 0, inst = 0;
        uintptr_t base;
        uint32_t pw;
        mod_t m;
        char help[128];
        loaded_version[w] = -1;
        r.r[0] = 18; r.r[1] = (int)(uintptr_t)watched[w];
        if (probe_swi(OS_Module, &r)) {
            say("  %-18s not loaded\n", watched[w]);
            continue;
        }
        base = (uintptr_t)(uint32_t)r.r[3];
        pw = (uint32_t)r.r[4];
        for (;;) {                          /* its instances */
            mod_t x;
            r.r[0] = 12; r.r[1] = mno; r.r[2] = inst;
            if (probe_swi(OS_Module, &r)) break;
            mno = r.r[1]; inst = r.r[2];
            if (mod_copy((uintptr_t)(uint32_t)r.r[3], &x)) continue;
            if (!strcmp(str_at(&x, w32(&x, 0x10)), watched[w])) instances++;
            free(x.img);
        }
        if (mod_copy(base, &m)) { say("  %-18s (out of memory)\n", watched[w]); continue; }
        help_text(&m, help, sizeof help);
        loaded_version[w] = help_version(help);
        say("  %-18s %s\n  %-18s at &%08X (%s), workspace &%08X%s\n", watched[w], help, "",
            (unsigned)base, base >= 0xFC000000u ? "ROM" : "RMA", (unsigned)pw,
            instances > 1 ? ", more than one instance" : "");
        if (w == 0) {                       /* ARMEABISupport's SWIs */
            uint32_t chunk = w32(&m, 0x1C), t = w32(&m, 0x24);
            say("  %-18s SWI chunk &%X:", "", (unsigned)chunk);
            if (t && t < m.len) {
                const char *s = str_at(&m, t);
                uint32_t o = t + (uint32_t)strlen(s) + 1;
                int k = 0;
                say(" (%s_)", s);
                while (o < m.len && m.img[o] && k < 16) {
                    const char *n = (const char *)m.img + o;
                    say(" %s", n);
                    o += (uint32_t)strlen(n) + 1;
                    k++;
                }
            }
            say("\n");
            if (chunk != EABI_CHUNK) say("  (not the expected chunk &%X)\n", EABI_CHUNK);
        }
        free(m.img);
    }
}

/* ---- 2. the copies on disc ---- */

typedef struct { char where[256]; char help[128]; int version; } copy_t;
static copy_t copies[64];
static int ncopies;

static void canonical(const char *in, char *out, size_t n)
{
    _kernel_swi_regs r;
    memset(&r, 0, sizeof r);
    r.r[0] = 37; r.r[1] = (int)(uintptr_t)in; r.r[2] = (int)(uintptr_t)out; r.r[5] = (int)n;
    if (probe_swi(OS_FSControl, &r)) snprintf(out, n, "%s", in);
}

static void file_copy(const char *name, const char *want_title)
{
    mod_t m;
    char where[256], help[128];
    if (mod_file(name, &m)) return;
    if (want_title && strcmp(str_at(&m, w32(&m, 0x10)), want_title)) { free(m.img); return; }
    canonical(name, where, sizeof where);
    help_text(&m, help, sizeof help);
    if (ncopies < 64) {
        copy_t *c = &copies[ncopies++];
        snprintf(c->where, sizeof c->where, "%s", where);
        snprintf(c->help, sizeof c->help, "%s", help);
        c->version = !strcmp(str_at(&m, w32(&m, 0x10)), "ARMEABISupport") ? help_version(help) : -1;
    }
    say("  %s\n      %s\n", where, help);
    free(m.img);
}

static void system_copies(void)
{
    static const char *const files[] = { "ARMEABISupport", "SharedULib", "PThrTicker" };
    say("\nIn System:Modules (what !Run files load):\n");
    for (int i = 0; i < 3; i++) {
        char name[64];
        int before = ncopies;
        snprintf(name, sizeof name, "System:Modules.%s", files[i]);
        file_copy(name, NULL);
        if (ncopies == before) say("  System:Modules.%s: none\n", files[i]);
    }
}

/* ---- 3. ARMEABISupport's own list ---- */

static void eabi_info(const char *infofile)
{
    char cmd[300], line[256];
    _kernel_swi_regs r;
    FILE *f;
    int n = 0;
    say("\nARMEABISupport's programs (*ARMEABISupport_Info; \"App\" = one running program,\n"
        "this probe among them):\n");
    if (!infofile) { say("  (not asked for)\n"); return; }
    snprintf(cmd, sizeof cmd, "ARMEABISupport_Info { > %s }", infofile);
    r.r[0] = (int)(uintptr_t)cmd;
    if (probe_swi(OS_CLI, &r)) { say("  (the command failed: an old ARMEABISupport, or none)\n"); return; }
    if (!(f = probe_fopen(infofile, "r"))) { say("  (no output)\n"); return; }
    while (fgets(line, sizeof line, f)) {
        say("  %s", line);
        if (!strncmp(line, "App ", 4)) n++;
        if (!strchr(line, '\n')) say("\n");
    }
    fclose(f);
    say("  (%d program%s)\n", n, n == 1 ? "" : "s");
}

/* ---- 4. the tasks ---- */

static void tasks(void)
{
    uint32_t buf[16 * 4];
    _kernel_swi_regs r;
    int handle = 0, n = 0;
    say("\nRunning tasks:\n");
    do {
        r.r[0] = handle; r.r[1] = (int)(uintptr_t)buf; r.r[2] = (int)sizeof buf;
        if (probe_swi(TaskManager_EnumerateTasks, &r)) { say("  (TaskManager didn't answer)\n"); return; }
        handle = r.r[0];
        for (uint32_t *e = buf; (uintptr_t)e < (uintptr_t)(uint32_t)r.r[1]; e += 4) {
            char name[68];
            uint32_t at = e[1], words[17];
            probe_copy(at & ~3u, words, sizeof words);
            memcpy(name, (const char *)words + (at & 3), 64);
            name[64] = 0;
            for (char *p = name; *p; p++)
                if ((unsigned char)*p < 32) { *p = 0; break; }
            say("  %-32s %6uK%s\n", name, (unsigned)(e[2] >> 10), e[3] & 1 ? "  (module)" : "");
            n++;
        }
    } while (handle >= 0 && n < 256);
}

/* ---- 5. an application's modules ---- */

static void walk(const char *dir, int depth)
{
    uint32_t buf[256];
    _kernel_swi_regs r;
    int offset = 0;
    if (depth > 4) return;
    do {
        memset(&r, 0, sizeof r);
        r.r[0] = 10; r.r[1] = (int)(uintptr_t)dir; r.r[2] = (int)(uintptr_t)buf;
        r.r[3] = 32; r.r[4] = offset; r.r[5] = (int)sizeof buf; r.r[6] = 0;
        if (probe_swi(OS_GBPB, &r)) return;
        offset = r.r[4];
        uint8_t *e = (uint8_t *)buf;
        for (int i = 0; i < r.r[3]; i++) {
            uint32_t load, type;
            char path[512];
            const char *name = (const char *)e + 20;
            memcpy(&load, e, 4);
            memcpy(&type, e + 16, 4);
            snprintf(path, sizeof path, "%s.%s", dir, name);
            if (type == 2 || type == 3)
                walk(path, depth + 1);
            else if ((load & 0xFFFFFF00u) == 0xFFFFFA00u)   /* filetype &FFA: a module */
                file_copy(path, NULL);
            e += (20 + strlen(name) + 1 + 3) & ~3u;
        }
    } while (offset != -1);
}

static void app(const char *dir)
{
    char run[300], line[256];
    FILE *f;
    say("\n%s:\n", dir);
    snprintf(run, sizeof run, "%s.!Run", dir);
    if ((f = probe_fopen(run, "r")) != NULL) {
        say("  !Run's module lines:\n");
        while (fgets(line, sizeof line, f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (strstr(line, "RMEnsure") || strstr(line, "RMLoad") || strstr(line, "RMKill"))
                say("    %s\n", line);
        }
        fclose(f);
    } else {
        say("  (no !Run)\n");
    }
    say("  Modules inside it:\n");
    walk(dir, 0);
}

/* ---- main ---- */

int probe_main(int argc, char **argv)
{
    const char *infofile = NULL, *label = NULL, *apps[MAX_APPS];
    int napps = 0, i, newer = -1;
    time_t now = time(NULL);
    char when[32];

    out2 = NULL; ncopies = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out2 = fopen(argv[++i], "a");
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) infofile = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc && napps < MAX_APPS) apps[napps++] = argv[++i];
        else {
            printf("Usage: eabiprobe [-o file] [-i infofile] [-l label] [-a appdir]...\n");
            return 1;
        }
    }
    strftime(when, sizeof when, "%d %b %Y %H:%M:%S", localtime(&now));
    say("eabiprobe 0.1: ARMEABISupport and the GCCSDK modules, %s%s%s\n\n", when,
        label ? " - " : "", label ? label : "");

    loaded();
    system_copies();
    eabi_info(infofile);
    tasks();
    {
        const char *iris = getenv("Iris$Dir");
        if (iris && *iris && napps < MAX_APPS) apps[napps++] = iris;
        else say("\n(Iris$Dir isn't set: open the directory holding !Iris, then run this again,\n"
                 "to see what Iris's !Run loads)\n");
    }
    for (i = 0; i < napps; i++) app(apps[i]);

    /* 6. the verdict */
    for (i = 0; i < ncopies; i++)
        if (copies[i].version > loaded_version[0] && (newer < 0 || copies[i].version > copies[newer].version))
            newer = i;
    say("\n");
    if (loaded_version[0] < 0)
        say("Result: ARMEABISupport isn't loaded (this probe should have loaded it: is it installed?)\n");
    else if (newer >= 0)
        say("Result: a newer ARMEABISupport (%d.%02d) is in\n  %s\n"
            "than the one loaded (%d.%02d). Loading it fails with \"There are still clients using\n"
            "ARMEABISupport\" while any GCCSDK program runs (Reel, FFmpeg, this probe...). Merge the\n"
            "newest ARMEABISupport into !System (PackMan, or !Boot's System merge) and restart.\n",
            copies[newer].version / 100, copies[newer].version % 100, copies[newer].where,
            loaded_version[0] / 100, loaded_version[0] % 100);
    else
        say("Result: OK - no copy found is newer than the loaded ARMEABISupport (%d.%02d)\n",
            loaded_version[0] / 100, loaded_version[0] % 100);
    say("\n");
    if (out2) fclose(out2);
    return newer >= 0 ? 2 : 0;
}

#ifndef PROBE_TEST
int main(int argc, char **argv) { return probe_main(argc, argv); }
#endif
